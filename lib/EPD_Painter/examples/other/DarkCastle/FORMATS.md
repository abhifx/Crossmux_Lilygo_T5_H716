# Dark Castle file formats

Everything below was reverse engineered from the game's own 68000 code. None of
it is documented anywhere, and none of it is guesswork: each field is cited
back to the instruction that reads it, so it can be checked.

Offsets are into the `CODE` resources of the `Data A` file. Segment 4 is named
`ASMSEG1` in its resource name, and holds the hand written assembly: the
blitter and both codecs.

---

## 1. Getting at the files

The disk is an 800K **HFS standard** volume (`Macintosh HFS data (bootable),
512 byte blocks, 1594 blocks, volume "Dark Castle"`). Modern macOS dropped HFS
standard support, so `hdiutil attach` fails with *image not recognised* and
there is nothing on a stock box that will mount it. `dc_extract.py` parses the
volume directly: MDB at sector 2, catalog B-tree, extents overflow B-tree, then
the resource fork of each file.

| File | Resource fork | What is in it |
|---|---|---|
| `Dark Castle` | 1.7 KB | a stub: `CODE` 0 and 1 only |
| `Data A` | 348 KB | code, hero and guard art, title screens, sounds |
| `Data B` | 394 KB | room screens, room art, sounds |
| `Castle Preferences` | 2 KB | high scores, key bindings, a recorded demo |

The application itself holds almost nothing. All seven real code segments live
in `Data A`, named `Main`, `HEROSEG`, `InitSeg`, `ASMSEG1`, `GAMESEG`,
`GreatHall` and `HiScore`.

### Resource types

| Type | Count | Meaning |
|---|---|---|
| `PSCR` | 20 | full screen picture, dictionary coded (§2) |
| `PPCT` | 172 | sprite animation, bit RLE (§3) |
| `IMAG` | 6 | image descriptor pointing at a `PSCR` or `PICT` (§4) |
| `GPTH` | 4 | guard path: waypoints plus the `PPCT` ids to animate |
| `SFTR` | 17 | per room object list |
| `Stbl` | 4 | score table, one per difficulty |
| `SOUN` | 73 | digitised sound |
| `DCAS`, `OUVN` | | file tagging, used to tell Data A from Data B |

---

## 2. `PSCR` — full screen pictures

A two table dictionary coder. Every one of the 20 resources decodes to exactly
21888 bytes: 512 x 342 at 64 bytes per row, 1 = black.

```
+0        uint16  length of the control stream, in bytes
+2..+9            run dictionary, 8 entries
+10..+136         single dictionary, indices 1..127
+137              unused
+138              control stream
```

The header is always 138 bytes, which is why every `PSCR` resource is exactly
138 bytes longer than the number in its first word — the giveaway that led to
the format.

Control byte `c`:

| `c` | meaning |
|---|---|
| `0` | escape: emit the next byte raw (consumes two stream bytes) |
| `1..0x7F` | emit `single_dict[c]`, i.e. the byte at `+9 + c` |
| `0x80..0xFF` | emit `run_dict[(c >> 4) & 7]` — the byte at `+2 + idx` — `(c & 0x0F) + 1` times |

Note the loop counter is over *control bytes*, not output bytes: the decoder
runs until it has consumed the count in the header, and the escape case
decrements it twice.

Derivation, `ASMSEG1 +0x587e`:

```
005886  movea.l $8(a6), a4        ; a4 = src
00588a  movea.l $c(a6), a3        ; a3 = dst
00588e  move.w  (a4), d7          ; control byte count
005892  movea.l a4, a1
005894  adda.w  #$8a, a1          ; +138: start of the control stream
005898  move.b  (a1)+, d0
00589a  beq.b   $58a6             ; 0 -> raw byte
00589c  blt.b   $58ac             ; high bit set -> run
0058a0  move.b  $9(a4, d0.w), (a3)+     ; single dictionary
0058a6  move.b  (a1)+, (a3)+ ; subq.w #1, d7  ; raw, costs two
0058ac  move.b  d0, d1 ; andi.b #$f, d1       ; run length - 1
0058b4  andi.b  #$70, d0 ; lsr.b #$4, d0      ; run dictionary index
0058bc  move.b  $2(a4, d0.w), (a3)+ ; dbra d1
```

---

## 3. `PPCT` — sprites

A 14 byte header, then a bit oriented RLE.

```
+0   uint16  mode     0 and 3 carry a mask plane, 1 2 and 4 do not
+2   uint16  frames
+4   uint16  width, in 16 bit words
+6   uint16  height, in pixels
+8   uint16  bytes per plane per frame  (= width*2 * height)
+10  uint16  registration x
+12  uint16  registration y
+14          packed stream
```

Planes are stored image, mask, image, mask… for each frame in turn. The
destination starts **zeroed** and the decoder only ever ORs, which is what makes
the skip opcode free:

| byte `b` | meaning |
|---|---|
| `< 0x80` | literal: the low 7 bits, written MSB first |
| `== 0x80` | end of stream |
| `0x81..0xBF` | leave `(b & 0x3F) + 7` bits clear |
| `0xC0..0xFF` | set `(b & 0x3F) + 7` bits |

The `+7` bias falls out of the original's arithmetic rather than being an
arbitrary constant. It tracks a bit cursor `d1` counting *down* from 7 within
the current byte, and computes `d2 = c + 7 - d1 - 1`, then fills `8 - u` bits to
finish the current byte, `d2 >> 3` whole bytes, and `d2 & 7` more. With
`u = 7 - d1` bits already used, that totals `c + 7` bits regardless of alignment.

This is also why every screen's tail is padded with `0x8F` bytes and terminated
by `0x80`: `0x8F` skips 22 already-clear bits at a time, running the cursor out
to the end of the buffer.

Derivation: `ASMSEG1 +0x57c0`, arguments `(src, dst, srcLen, dstLen)`, called
via the A5 jump table at `$3a2(a5)`.

---

## 3a. `PICT` — and why you cannot skip them

**The Great Hall is a `PICT`, not a `PSCR`.** This is the single easiest thing
to get wrong in this game's data. The loader at `GreatHall +0x182` asks for
`PICT` with the id from the `IMAG` record and only *falls back* to `PSCR`:

```
000182  clr.l   -(a7)
000184  move.l  $25e(pc), -(a7)   ; 'PICT'
000188  move.w  d5, -(a7)         ; id from IMAG +14
00018a  _GetResource
00018c  move.l  (a7)+, d7
00018e  bne.b   $1e0              ; got a PICT -> DrawPicture path
000190  clr.l   -(a7)
000192  move.l  $25a(pc), -(a7)   ; 'PSCR'
...
```

Walk only the `PSCR` list and you get 20 screens, none of which is the hall —
`PSCR 4020` is a different stone hall entirely, which is an easy false match.
The hall is `PICT 10000`, 512x342, drawn in one-point perspective.

These are standard QuickDraw **PICT version 1**, with one trap: a large picture
is cut into **horizontal bands**, each its own `BitsRect` opcode carrying a
`dstRect` that says where it belongs. Decoding the first band alone yields a
48-row sliver of a 342-row screen. Every band must be composited by its
`dstRect`.

Useful PICTs:

| id | size | what |
|---|---|---|
| 10000 | 512x342 | **the Great Hall** |
| 16000 | 512x33 | the status bar along the bottom |
| 16100..16103 | ~13x21 | digits |
| 16200, 16201..16204 | small | room name plates |
| 20000..20005 | ~500x310 | end / cut scenes |

`dc_extract.py` decodes these at extraction time and re-encodes them with the
**sprite codec** (§3), so the port carries two small decoders rather than a
QuickDraw interpreter.

Related: `PPCT 10050` (64x159, 3 frames) is the Great Hall's centre door
opening, and the centre door in the artwork is exactly 64 wide and 159 tall.
`10051` and `10052` are side doors; `10008` is the hero drawn in hall
perspective.

---

## 4. `IMAG` — image descriptors

Sixteen bytes, and only the ones the title sequence uses.

```
+0  uint16  mode: 0 and 3 mean two planes, 1 2 and 4 mean one
+2  uint16  frames
+4  uint16  width in words
+6  uint16  height
+8  uint16  bytes per plane
+10 uint16  width in pixels
+12 uint16  height
+14 uint16  resource id of the PICT or PSCR holding the pixels
```

`IMAG 12000` reads `0001 0001 0020 0156 5580 0200 0156 2ee0`: one frame,
0x20 = 32 words = 512 px wide, 0x156 = 342 tall, 0x5580 = 21888 bytes, and
0x2ee0 = 12000, which is `PSCR 12000` — the title screen. The loader in
`GreatHall +0x166` tries `PICT` with that id first and falls back to `PSCR`.

---

## 5. Screen geometry

The sprite blitter at `ASMSEG1 +0x04` writes straight to a 64 byte per row
bitmap and clips vertically against `0x136` = **310**, not 342. The bottom 32
rows were the Mac's status strip. So the playfield is **512 x 310**, which is
what this port treats as the world.

---

## 6. What there is no data for

**Collision.** There is no geometry resource, and the floors exist only in the
artwork. `black = solid` does not work globally: the cave rooms use large solid
black masses as *scenery*, while the walls elsewhere are 50% dither that would
read as floor. This port derives a solid map from each room bitmap with a
density test (13 of every 16 pixels) and corrects it with a short per-room fixup
list. See `dc_rooms.cpp`, and `tools/dc_probe.py --map` to see what a room
derives to.

**Object placement.** `SFTR` gives a per room list of small integers — object
type ids — but their coordinates are baked into `GAMESEG`, which has not been
reverse engineered here. Spawn points in `dc_rooms.cpp` are authored.

**Score table.** `Stbl` 0..3 are the four difficulty levels, 36 words each:
sixteen entries that scale 0x2000 / 0x3000 / 0x4000 / 0x5000 across the
difficulties and a completion bonus of 0x5000 / 0x7500 / 0x10000 / 0x15000.
Wired up only loosely here.

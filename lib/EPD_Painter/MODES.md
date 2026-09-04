# Modes — a mode is a record, and `setQuality()` installs it

Status: **design, not implemented.** Written 28 July 2026, after a tuning
session on the H716 that produced the first HIGH tables any board has had.

## The problem this fixes

A display configuration is currently three orthogonal knobs:

```cpp
epd.setQuality(QUALITY_NORMAL);      // pass counts, delays, waveform tables, periods
epd.driver().setGreyLevels(16);      // level count, buffer allocation
epd.driver().setDirectTransitions(true);   // grey-to-grey trains, spill planes
```

Their product contains combinations that do not exist. `FAST` + 16 greys is
refused at runtime. `HIGH` + 4 levels compiles, allocates, paints, and has
never been measured on any panel. Meanwhile the things that *must* agree —
a table, the pass count it was calibrated at, and the period it was
calibrated at — are stored in three different places and agree only by
convention.

That convention broke, and it cost a day. `_grey16_build_trains()` had no
notion of quality, so HIGH ran NORMAL's 13-pass tables at HIGH's 19 ms
period: a 27% dose error on every pass, with no mechanism anywhere that
could have noticed. `setQuality()` did not even rebuild the train library,
so switching quality could not have corrected it in principle.

The legal configurations are six, and each is a different machine:

| mode | levels | passes | pacing | tables |
|---|---|---|---|---|
| `FAST_4` | 4 | 7 | no inter-pass delay | `fast_darker`/`fast_lighter` + `dir_fast` |
| `NORMAL_4` | 4 | 13 | 4 ms after each row loop | `normal_darker`/`normal_lighter` |
| `NORMAL_4_DIRECT` | 4 | 13 | 4 ms after each row loop | as above + `dir_normal` |
| `NORMAL_16` | 16 | 13 | constant 20 ms period | `g16_apply`/`g16_remove` |
| `HIGH_16` | 16 | 20 | constant 20 ms period | `g16_apply_high`/`g16_remove_high` |
| `HIGH_4` | 4 | 13 | 8 ms after each row loop | none — untuned, formula library |

Name those six and the illegal ones cannot be written down. `HIGH_4` is named
but ships no record: the combination exists, it has simply never been
measured on any panel (see open question 1).

## The record

A mode is fully described by one self-contained record: what it draws, how
long it takes, and the measurements that make it true. `setQuality()` reads
it and configures the engine. Built-in modes are records compiled into the
preset; tuned modes are records in flash; they are the same kind of thing
and go through the same install path.

```
header (fixed)
  magic          'EPTM'
  version        format version
  board_key      hash of pins + geometry; refuses another board's record
  mode_id        FAST_4 | NORMAL_4 | NORMAL_4_DIRECT | NORMAL_16 | HIGH_16 | HIGH_4
  levels         4 or 16
  flags          bit 0 = direct transitions
  train_len      passes per train: 7, 13 or 20
  pace           CONSTANT_PERIOD | FIXED_DELAY
  pace_us        the period (CONSTANT_PERIOD) or the delay (FIXED_DELAY)
  settle_us      minimum idle appended when the row loop overran the period
  row_extra_us   extra charge time held after each row's latch
  le_hold_ns     held latch width
  payload_len    bytes of payload following
  crc            over header + payload

payload (only what this mode uses)
  levels == 16   apply[16][train_len], remove[16][train_len]
  levels == 4    darker[3][train_len], lighter[3][train_len]
  flags & DIRECT direct[4][4][26], direct_loaded (uint16 bitmask)
  always         level_lum[levels]      measured landing, 255 = paper white
```

**Pacing is a model, not a number, which is why it needs its own field.**
16-grey pads every pass to a constant period so retention dose depends only
on the trains. 4-level waits a fixed time after the row loop and has no
period at all — FAST waits zero, which is most of why it is fast. One
`pass_us` field cannot mean both without lying about one of them.

**The payload is variable-length, and that is what makes this cheaper than
what we have.** Today's fixed `Blob` is 1522 bytes whatever it holds, because
it reserves room for every quality's tables in every record — 416 bytes of
empty `direct[4][4][26]` in a 16-grey blob, 1024 bytes of empty
`apply`/`remove` in a FAST one. Sized to contents instead:

| mode | payload | vs today's 1522 |
|---|---|---|
| `FAST_4` | 42 + 416 + 4 = 462 | −70% |
| `NORMAL_4` | 78 + 4 = 82 | −95% |
| `NORMAL_4_DIRECT` | 78 + 416 + 4 = 498 | −67% |
| `NORMAL_16` | 416 + 16 = 432 | −72% |
| `HIGH_16` | 640 + 16 = 656 | −57% |

All five records compiled in (HIGH_4 has none), with headers, is about 2.2 KB — less than the 4.5 KB
three fixed blobs cost today, and comparable to the 1.9 KB of hand-written
C tables while carrying the timing the tables are only valid at, which the C
tables cannot.

## What `setQuality()` does

```cpp
bool setQuality(Mode m);                        // built-in, or flash if tuned
bool setQuality(const uint8_t *rec, size_t n);  // an arbitrary record
```

1. Validate — magic, version, CRC, `board_key`. Refuse otherwise.
2. Reconcile buffers against `levels` and `flags`: 16-grey needs the 4bpp
   planes, direct needs the spill planes. Allocate what is now required and
   KEEP what is not — see open question 2; freeing on a switch would turn
   `FAST_4 <-> NORMAL_16` toggling into a malloc storm mid-animation.
3. Install timing: `pace`, `pace_us`, `settle_us`, `row_extra_us`,
   `le_hold_ns`.
4. Install tables into the live library, and `direct_loaded` so unconverged
   pairs stay absent rather than driving a train of floats.
5. Install `level_lum` for the dither quantiser.
6. Record the active mode.

**Validation happens before anything is installed.** A record that fails at
step 4 must leave the previous mode intact — a half-installed mode is a
panel driving one quality's tables at another's timing, which is the exact
fault this design exists to make impossible.

Lookup precedence for `setQuality(Mode)`: a tuned record in flash for that
mode wins over the compiled-in one. That is how a tuned board keeps its own
measurements without the sketch knowing anything about storage.

## What this retires

- `TrainTables` — the tables move into records.
- `Config::g16_pass_us_normal` / `g16_pass_us_high` / `g16_settle_us` /
  `row_extra_us_*` / `le_hold_ns` — timing moves into records, beside the
  tables it belongs to.
- `setGreyLevels()` and `setDirectTransitions()` as public API — implied by
  the mode.
- `EPD_PainterTuned::install()` — becomes `setQuality(record)`.

## Migration

The old API stays and maps onto the new one, because every example, the LVGL
wrapper, the photo viewer and the README use it:

```cpp
setQuality(QUALITY_NORMAL) + setGreyLevels(16)  ->  NORMAL_16
setQuality(QUALITY_FAST)   + setGreyLevels(4)   ->  FAST_4
setQuality(QUALITY_NORMAL) + setGreyLevels(4)   ->  NORMAL_4 (+ DIRECT if enabled)
setQuality(QUALITY_HIGH)   + setGreyLevels(16)  ->  HIGH_16
setQuality(QUALITY_FAST)   + setGreyLevels(16)  ->  refused, as now
```

Existing v4/v3/v2 blobs are read and converted into records on load, so a
board tuned before this change keeps its tables.

## Open questions

1. **`HIGH_4`** — RESOLVED 28 July: name it, leave it untuned. So the enum has
   six entries and one of them ships without a record. `setQuality(HIGH_4)`
   therefore has to do something defensible with no tables: it falls back to
   the formula library at 13 passes, exactly as an untuned board does today,
   and says so once on the serial log. Refusing it outright was the
   alternative and would have made the enum a lie — the combination does
   exist, it has simply never been measured.
2. **Mode switching cost.** Buffer reconciliation on every switch means
   alloc/free churn if an app toggles modes per frame, and these are not small
   buffers — the 4bpp planes are ~259 KB each and the spill planes more.
   RESOLVED 28 July: allocate on demand and do NOT free on a switch, so
   `FAST_4 <-> NORMAL_16` toggling costs nothing after the first pass through
   each. Add an explicit `releaseUnusedBuffers()` for apps that would rather
   have the PSRAM back. Automatic freeing turns a mode switch into a malloc
   storm at exactly the moment the app is trying to animate.
3. **Does `NORMAL_4_DIRECT` stay a mode, or is direct purely a flag?**
   RESOLVED 28 July: keep it named. The flag is in the record either way, but naming
   the combination lets a tuned `NORMAL_4_DIRECT` record exist independently
   of `NORMAL_4` — which matters, because the direct trains are the half that
   goes stale on its own and is worth re-measuring alone.
4. **What is `level_lum` normalised against?** Today each tune normalises to
   its OWN white and black, so every mode's curve ends at 0 — which asserts
   that FAST's darkest level is as dark as HIGH's. That is very unlikely to be
   true: FAST spends 7 undelayed passes where HIGH spends 20 paced ones, and
   the dose ladder puts the saturation knee at pass 7 with 103 of 109 units of
   depth reached, so FAST plausibly lands near but not at the floor.
   It matters because `drawGray8()` quantises against this curve, so a wrong
   endpoint biases every image in that mode.
   RESOLVED 28 July: normalise every mode against a COMMON pair of endpoints —
   paper white and the deepest black the panel reaches in any mode — so
   `FAST_4`'s darkest entry reads its true value rather than a nominal 0.
   Cheap to settle: the FAST tune measures its own black, and a 16-grey black
   patch in the same scan session gives the reference directly. Worth doing
   during the FAST run rather than reasoning about.

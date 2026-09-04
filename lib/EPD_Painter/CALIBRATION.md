# Calibration, the tuning rig, and what we learned

Working notes for the self-tuning system: how it works, how to drive it, what
has been measured, and what is still wrong. Written after a long session on
two LilyGo boards so the next person does not rediscover it all.

Read alongside `DECISION_ENGINE.md` (what the trains are) and
`examples/other/tuneup/README.md` (how to run the rig).

---

## 1. The short version

The panel's 16 grey levels are **not evenly spaced**, and their spacing
differs per board. Everything here exists to measure where they actually are
and to move them where they should be.

| Thing | Where it lives |
|---|---|
| Tuner, 16-grey (NORMAL, HIGH) | `examples/other/tuneup/tuner.h` (~2200 lines) |
| Tuner, FAST (4-level + directs) | `examples/other/tuneup/tuner_fast.h` |
| Tuned tables | LittleFS, one blob per quality, `src/EPD_Painter_tuned.h` |
| Shipped tables | `src/EPD_Painter_trains.h` |
| Scanner bridge | NAS, `escl_bridge.py` — see §7 |

The tuner paints a card, scans it on a flatbed, compares each grey against an
in-frame dithered reference, and edits the drive trains until they match.

### The three qualities are three different machines

Not one tune with a speed knob. They differ in what they can express, and
mixing their tables is meaningless — a train is only valid at the pass
period AND the pass count it was calibrated at.

| | levels | passes | period | tables |
|---|---|---|---|---|
| FAST | 4 | 7 | no inter-pass delay | `fast_darker`/`fast_lighter`, `dir_fast` |
| NORMAL | 16 | 13 | 15 ms | `g16_apply`/`g16_remove` |
| HIGH | 16 | **20** | 15 ms | `g16_apply_high`/`g16_remove_high` |

Each is stored in its own blob, so tuning one cannot cost you another.

**HIGH is NORMAL's timing with more slots.** Same pass period; 20 passes
instead of 13. It is not "the same trains, slower", and the period is the
same on purpose.

*Measured, LilyGo, 26 July 2026* — dose ladder swept at 19 / 15 / 13 ms,
20 passes, `D` at the menu:

| period | first pass | steepest pass | slots still resolving | total depth | frame |
|---|---|---|---|---|---|
| 19 ms | 1.62 steps | 4.11 steps | 7 | 47 | 380 ms |
| 15 ms | 1.44 steps | 3.72 steps | 10 | 47 | 300 ms |
| 13 ms | 1.15 steps | 3.47 steps | 10 | 47 | 260 ms |

**Depth is identical at all three.** A longer pass buys nothing at the
black end while making every slot coarser AND leaving most of them dead —
at 19 ms the response stops moving after 7 of the 20 passes. So the old
19 ms HIGH was strictly worse than 15 ms on every axis including speed.

13 ms is not safe yet: the period must exceed the worst-case row loop
(~10.5 ms at ten sweeps) plus the settle floor (4 ms), so 13 ms would let
heavy content silently pad out to 14.5 ms and dose becomes
content-dependent again. Measured row loops on the match card were
6.8–8.8 ms, so 15 ms ran with 6+ ms of margin all run. Taking 13 ms needs
a worst-case-content measurement first.

**Black is anchored at the measured knee**, not at the train length. The
panel saturates around pass 10, so `tuneProbe()` sets L15 to knee + 1
(typically 11 darkens) and holds the dose curve flat past it. Driving
black for 20 passes wastes ten AND forces a 20-whiten charge-matched
erase, roughly doubling the charge cycled through every black paint for no
extra depth.

**Suspect the deep end of the ladder.** A card measures at most 14 doses
(16 columns, two are gradient anchors), so the 20-pass ladder splices a
second card starting at pass 7. In all three sweeps a 3–4 unit step lands
exactly ON the join at pass 15, which is the signature of residual
horizontal bias rather than depth — the two-point gradient fit does not
fully remove it. The curve is therefore held flat past the knee. If the
deep end ever matters, re-run the ladder with a different splice base: if
the step follows the join, it is an artefact.

**Before this, HIGH was broken rather than merely untuned.** It shared
NORMAL's 13-pass trains and drove them at a 27% longer pass period, and
`setQuality()` did not rebuild the train library at all — so switching to
HIGH silently ran one quality's trains at the other's dose. Both are fixed:
the library is per-quality and rebuilt on every quality change.

---

## 2. Operational gotchas — read this before touching hardware

These cost hours. All are real, all bit us.

**Flash with `CDCOnBoot=cdc` or you get no serial.**
```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,USBMode=hwcdc,CDCOnBoot=cdc
```
The FQBN default is `CDCOnBoot=default` (disabled), which routes the sketch's
`Serial` to **UART0 physical pins**, not USB. ROM boot messages still appear
over USB-Serial-JTAG, so you see a boot log and then silence at exactly the
same line every time. Two runs' telemetry were lost to this, and two "fixes"
were written chasing a phantom before the FQBN was checked.

**Opening the serial port RESETS the board.** Attach *before* starting a run,
never during. A speculative reconnect is a reset: an earlier sniffer treated
60 s of silence as a stale endpoint and reopened, which killed a run outright.
Silence means nothing on this hardware; only reconnect if the device node is
genuinely replaced.

**The port name changes.** `usbmodem1301` and `usbmodem11301` have both
appeared for the same board across re-enumerations. Rescan, don't hardcode.

**`!` does not execute in the Claude mobile app.** Commands pasted with a `!`
prefix arrive as plain text. If a shell command matters, either get a
permission rule in place or wait for the desktop. Two printer-config attempts
were believed to have run and had not, which produced a wrong conclusion about
the printer rejecting them.

---

## 3. What the tuner does

```
registration  -> find the panel on the glass (adaptive dark threshold)
geometry      -> anchor ring, exact panel-px to scan-px mapping
dose ladder   -> measure the pure-darken response curve
gap ladder    -> measure the float-gap gain
seed model    -> derive starting shapes from the measured physics
cycles        -> measure card, attribute drift, edit 1-2 levels, repeat
  stall       -> rescue the worst level (whole card, 14 candidates)
removes       -> derive charge-matched removes, verify optically
results       -> SAVE or SKIP
```

**Trains** are per-level drive sequences of 13 passes: `0` float, `1` darken,
`2` whiten. The planner works in a `(d,w,g,r)` grammar — d darkens, w whitens,
g float gap, r re-darkens — which spans most useful shapes but **cannot**
express interior floats in arbitrary positions or alternating `d,w,d,w` tails.
The rescue (§5) works on raw sequences and can.

---

## 4. Measured facts

**Float gaps are worth nothing.** Twelve measurements across two panels:
`-0.53, -0.50, -0.20, -0.40, -0.30, -0.1, -0.2, -0.4, -0.6, ...` — all within
noise of zero, bases never agreeing on sign. The knob is switched off when
measured as nothing, *not floored*: an early version clamped a measured zero up
to 1.0, which made the gap the finest lever in the set and the seeder
sprinkled gaps buying units the panel never delivered.

Physically: the inter-pass settle floor (4 ms of a 15–20 ms period) has already
saturated the relaxation, so an extra period of rest adds nothing. **This is
settled — do not re-investigate.** Untested: FAST 4-level, which has *no*
inter-pass delay and is where the idea might still hold.

**Whiten gain differs per board and must be measured.**

| Board | measured `gw` |
|---|---|
| LilyGo A (orig) | 18.8 – 26.1, mean ~23 (nine `n=2` readings) |
| LilyGo B | 17.5 |
| built-in fallback | **31.0** — wrong on both |

`tuneCalibrateGains()` can only solve `gw` from a level carrying whitens and no
re-darkens. Nothing guaranteed one existed; when none did it silently used
31.0, measured on a different board. The seeder now reserves probes where the
constraint costs least, refusing any costing more than ~a level step.

**Tolerances must scale with the panel.** Full scale ≈ 97 scanner units over 15
intervals, so a level step ≈ 6.5 — on *that* panel, at *that* scan resolution.
All thresholds are now fractions of `tuneStep()`, measured from the panel's own
reference ladder. The registration dark threshold was likewise a hardcoded grey
of 120; a second board whose black rendered lighter put no pixels below it and
reported "no dark panel found" with the board plainly on the glass.

**A gap tolerance must be LOOSER than the placement tolerance.** A gap is the
*difference* of two neighbouring errors, so it runs larger than either and can
reach twice the worst placement error. Gating gaps tighter than placement makes
convergence unreachable by construction — a whole morning of runs ended
`converged=0`, stalling on gap while `maxerr` was already inside tolerance, with
the planner burning cycles chasing a target it could not hit. Now 1.10 steps
against 0.70 for placement.

---

## 5. The rescue

When the planner stalls, hand the worst level the **whole card**: the 16 columns
take their train from their own level index, so they can be borrowed as
experiment slots. Fourteen candidate trains, one scan, keep the best.

Two things make it trustworthy:

**Slot 1 holds the incumbent.** Candidates are judged by how far they sit from
the incumbent *in the same frame*, and that delta applied to the incumbent's
known error. Judging against a reference measured in a *different column*
carried ~13 units of horizontal bias and made a rescue "improve" a level by
adopting its own shape unchanged.

**Candidates are raw drive sequences** — substitute, insert (shifting right),
delete (shifting left) — so the search reaches shapes the grammar cannot
express. A level the rescue owns stores its train literally (`hasRaw`/`raw[13]`)
and the grammar planner leaves it alone.

Consequence handled: `tuneRemoveOf()` used to derive its charge match from
`(d,w,g,r)`. For a raw train that computes a remove for a shape the level no
longer has, and **a DC imbalance is ghosting**. It counts the actual sequence
now (`tuneNetOf`).

Observed working: L9 pinned at −5 for eight cycles, rescue found `(8,2,0,0)`,
converged two cycles later.

---

## 6. What is still wrong

**The converger oscillates.** Levels still move 15–40 units in one cycle. The
risk cap (bounding an edit's *uncertainty*, not just its predicted move) damped
it from 52 but did not cure it. A restructure trading whitens for re-darkens
predicts ~0 because the terms cancel while their errors do not.

**`converged=1` is not a quality gate.** It tests the *applies* only. One run
converged with the tightest applies of the day and `erase 19.1%` — one level's
remove residual at −19 where everything else was under −6. The removes are
derived analytically, verified in three widening rounds, then accepted whatever
they are, and **the attempt score does not see them at all**.

> **This is the highest-value next change**: score attempts on the removes as
> well as the applies. Everything done in this session improved the half that
> was already working.

**Run-to-run variance exceeds most effects being measured.** Two runs of the
*identical* binary produced `+3.3%` and `+22.2%` worst error. Every A/B
comparison of one run against one run is therefore uninformative. Measure the
noise floor — three runs of one binary — before believing any change helped.

**`src/EPD_Painter_trains.h` predates the converger.** Those tables came from
the older Python ladder (`extras/calibration/tune.py`), whose operators only
*flip* symbols in place and can never produce an interior float. Whatever the
tuner learns goes to the blob, not there.

**No board ships a HIGH table yet.** `g16_apply_high`/`g16_remove_high` are
null in every preset, so until a board is tuned for HIGH it runs the formula
library at 20 passes. The formula assumes the darken response is linear in
pass count and it is not — the deep end saturates — so an untuned HIGH ramp
will bunch at the dark end. That is expected, not a regression: it is what
"untuned" looks like, and it is more visible at 20 passes than at 13 because
all seven extra passes land in the saturated region.

**The FAST tune's direct phase is unproven on glass.** The four-level half
follows the same method as the 16-grey tune, but the grey-to-grey phase is a
new 1-D search over the balancing-run depth, with the DC constraint imposed
rather than searched. It has been written against the measured shape lessons
(darkening pairs put their whitens first, lightening pairs their darkens
first) but not yet run end to end. Treat its first results as a measurement
of the method, not of the panel.

---

## 6a. The pass budget — read this before trusting any tuned table

**Tables tuned on the match card can be wrong on real content, and the
reason is timing, not the tables.**

A 16-grey pass is padded to a constant period so retention dose depends only
on the trains. The budget available for the row loop is therefore
`period - settle floor`. Exceed it and the pass silently runs LONG, and
every pixel on that frame is over-dosed.

Measured, LilyGo, 27 July 2026 (`EPD_GREY16_PASS_TIMING`):

| content | worst row loop |
|---|---|
| match card (16 uniform columns) | 7–8 ms |
| full-height staircase | ~10 ms |
| a photograph + UI card | **13.3 ms** |

With the old 15 ms period and 4 ms settle floor the budget was 11 ms, so the
photograph overran: pad computed to 1.7 ms, was forced up to the 4 ms floor,
and the pass ran **17.3 ms instead of 15 — a 15% over-dose**. The same
tables that measured `worst 3.0, 0 inversions` on the match card produced
two inversions and a black landing at 65 instead of 47 on that screen.

This is the explanation for the match-card-versus-staircase disagreement
that dogged the whole July deep dive (it grew 9 → 16 → 19 → 31 → 38 → 51 lum
across one session). It is not panel drift and not the search.

**The fix is `g16_settle_us`, now 1500 rather than 4000.** The floor was
pure margin, and the float-gap probe measures a *whole extra pass period* of
idle settle as worth zero — thirteen times across three panels. Dropping it
takes the budget from 11 ms to 13.5 ms, above the worst row loop observed,
at no cost in frame time. The alternative is an 18 ms period, which costs
20% of the frame rate for the same effect.

**Rule of thumb:** if `row-loop max > period - g16_settle_us` for any
content you care about, your tables are being driven at a period they were
not calibrated at. Check it with `EPD_GREY16_PASS_TIMING` before blaming
anything else.

## 6b. Row charging time

`Config::row_extra_us` (4 µs) is held after each row's latch so the pixel
finishes charging. It is not a settle knob — it is inside the window during
which the pixel capacitor is being driven.

Measured on a LilyGo: L12's left-to-right gradient fell **6.0 → 2.4** scan
units and its landing moved **13 units darker**. The control experiment
settles the mechanism: adding the *same* 2.16 ms as pass padding instead
moved the mean only 2.5, so this is the charging window and not dose. The
whole line is latched at once (`LE` latches the shift chain, then `CKV`
advances the gate), so there is no sequential drive along a row — the
gradient is charging under simultaneous load, and columns further from the
feed are simply slower to arrive.

Free at 16-grey: the pass is already padded, so it comes out of idle time.
**Never applied at 4-level** — no padding there, so it would cost ~27% of
the frame rate at NORMAL and far more at FAST, to fix a uniformity problem
four coarse levels do not have.

It is recorded in the tuned blob alongside the pass period, because it is
equally load-bearing: 4 µs against 0 moves a deep level about two grey
steps.

## 6c. Position is the accuracy floor

`U` at the tuneup menu drives every patch to one level and compares against
a dither of the same density — which cancels optics, lamp falloff and
geometry and leaves the positional variation of the *drive*.

- Board A: an arch, sd ~1.3 units, **reproduced column-for-column hours
  apart after a completely different workload** — so it is panel geometry,
  not charge history.
- Board B: a U shape, sd ~3.5.
- Tuned results tracked those floors almost exactly: A reached 1.0, B 7.4.

Two consequences. The "per-patch noise" the confirmation round prints is
largely this, not random error. And **position sensitivity is a property of
the WAVEFORM, not only the panel** — tuning once made a level 5x *more*
position-sensitive while placing it perfectly, because it moved the level
off saturation onto the steep part of the response. The evolutionary tuner
now carries a robustness term (`EVO_ROBUST_W`) for exactly this.

## 7. The scanner rig

Canon PIXMA MG5200 — a 2010 device with no eSCL — bridged by a bespoke
`escl_bridge.py` on an Asustor NAS (`ssh admin@AS3302T-1307`,
`/volume1/.@plugins/AppCentral/entware/opt/etc/escl-bridge/`). Published as
`asustor-nas-scanner-bridge` on GitHub.

**Go through the bridge, never around it.** The bridge serialises every
acquisition on a lock. Running `scanimage` directly bypasses that and two
clients can hit the printer's one-client BJNP stack at once. Use an eSCL job
(`scanpanel.py` does this).

**It wedges on scan RATE, not length.** The printer accepts one session and
releases it lazily. Sustained scanning outruns it and the scan/print daemons
crash — HTTP and LPD stay up, BJNP 8610/8612 and 9100 stop listening. **Only a
power cycle fixes it**; three separate remote restarts (including one that
demonstrably restarted the network services) did not touch the scan engine.
Best-of-3 tuning attempts triple the load and caused exactly this, hence the
60 s cooldown between attempts.

**Port checks are not a health test.** Those ports show `connection refused`
even when healthy — BJNP only binds them once a UDP session is negotiated. The
only valid check is attempting a scan.

A persistent SANE handle removes the *second* BJNP conversation per job
(`sane_init` + backend load + discovery, which `scanimage` redid every time):
7.7 s → 3.9 s per scan. It cannot hold the TCP session open — the pixma backend
connects and disconnects *inside* each acquisition, independent of
`sane_open`/`close`.

---

## 8. Image drawing

`drawGray8()` quantises against `Config::level_lum` — what each level
*measurably* renders as, filled in by `EPD_PainterTuned::install()` from the
tuned blob. On a measured LilyGo the gaps between neighbouring levels ran 6 to
32 units against an ideal 17. Diffusion copes fine with uneven levels (it
grains more where gaps are wide); it only *bands* when it is wrong about where
they sit.

Fixing this — using the final card's `ref[]` rather than a `probeRef[]` read
minutes earlier — is what visibly cleaned up photographs. The spacing work in
the converger did *not*; do not credit it.

Everything streams (`GrayDither::begin/row/end`): ~4 KB of error rows whatever
the image size. No decoder dependency in the core; `EPD_Painter_Image.h` is
opt-in behind an explicit `#define` — **not** `__has_include`, because the
Arduino builder only puts a library on the include path when it sees an
unconditional include.

---

## 9. Driving a run remotely

`serialctl.py` (scratchpad) attaches once and takes commands through a file,
because a second port open would reset the board.

| key | at the menu | at the quality choice | at results |
|---|---|---|---|
| `T` | start self-tune | — | — |
| `f` / `n` / `h` | — | FAST / NORMAL / HIGH | — |
| `s` | demo scan | — | **save** |
| `k` | — | — | **skip** |
| `q` | — | back out | abort between cycles |

`s` is *scan* at the menu and *save* at the results screen — an easy and
embarrassing mistake to make.

Results screen: `worst` = placement error as % of full scale, `erase` = worst
remove residual, plus an inversion count. **Compare on all three**; a good
`worst` with a bad `erase` is an unusable table.

Best set so far on LilyGo A: **worst +3.3%, erase 3.2%, 0 inversions.**
Nothing in a long session of changes beat it — see the variance note in §6
before concluding anything from that.

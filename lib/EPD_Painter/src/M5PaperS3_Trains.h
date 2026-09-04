// ============================================================================
// M5PaperS3_Trains.h — every waveform the M5PaperS3 drives, in one place.
//
// One file per board: the 4-level waveform pairs (FAST/NORMAL/HIGH), the
// 16-grey decision-engine tables, and the grey-to-grey direct trains. The
// preset in EPD_Painter_presets.h carries pins and timing only and points
// here; auto-detection therefore selects these tables with the board. A
// tuned blob in NVS (see EPD_Painter_tuned.h and examples/other/tuneup)
// overrides them at begin().
//
// Every table is only valid at the timing recorded beside it — the pass
// period and pass count it was calibrated at. See EPD_Painter_trains.h
// for the drive-code conventions.
// ============================================================================

#ifndef EPD_PAINTER_M5PAPERS3_TRAINS_H
#define EPD_PAINTER_M5PAPERS3_TRAINS_H

#include <stdint.h>

// ---- 4-level waveform pairs ------------------------------------------------
inline const EPD_Painter::Waveforms EPD_WF_M5PAPERS3 = {
    .fast_lighter   = { { 1, 2, 2, 2, 2, 2, 3 },
                        { 3, 2, 2, 2, 2, 2, 3 },
                        { 2, 2, 2, 2, 2, 2, 2 } },
    .fast_darker    = { { 1, 1, 3, 3, 1, 3, 1 },
                        { 3, 1, 1, 1, 1, 1, 3 },
                        { 1, 1, 1, 1, 1, 1, 1 } },
    // NORMAL tables calibrated optically against in-frame dithered
    // references (extras/calibration match chart, 2026-07-18): each
    // driven grey matches a 66%/33% black-dot dither of the panel's
    // own black+white to within ~3 scanner grey levels, i.e. the
    // levels are linear-reflectance spaced. Each
    // darker+lighter row pair is DC balanced (#1s == #2s).
    // 4-grey NORMAL re-tuned 30 July 2026 by pair-evolution (cube ghost
    // weighting, tuner_evo4.h): levels worst 2.0 scan units, erase residual
    // 2.4, five of six direct pairs converged. Replaces the 21 July optical
    // set, whose L1 measured 19 units LIGHTER than its one-third-density
    // dither reference — it looked good, but the dither had no curve to know
    // that by. Each pair is exactly DC balanced (nets +5/-5, +6/-6, +13/-13).
    //
    // The inherited code-3 slots are left verbatim as measured. Code 3 now
    // emits FLOAT (see EPD_Painter.cpp), so they contribute no charge and sit
    // outside the ledger — the tune was measured with that already true.
    .normal_lighter = { { 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2 },
                        { 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .normal_darker  = { { 1, 1, 1, 3, 1, 1, 1, 0, 0, 1, 2, 3, 2 },
                        { 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
    .high_lighter   = { { 1, 3, 1, 1, 1, 2, 1, 2, 2, 2, 2, 2, 2 },
                        { 1, 3, 3, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .high_darker    = { { 1, 3, 1, 1, 2, 2, 2, 1, 2, 1, 1, 2, 1 },
                        { 3, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 2 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
};

// Measured landing of the four 4-grey NORMAL levels, 255 = paper white,
// 30 July 2026 — the same run that produced the waveform pair above, so the
// curve describes THESE levels rather than a different set's.
//
// This is what drawGray8() dithers photographs against. Before it existed the
// board fell back to an assumed even ramp (255/170/85/0); level 1 actually
// lands at 189 and level 2 at 99, so the assumption was out by 19 and 14
// respectively. Config carries ONE level_lum4 pointer shared by FAST and
// 4-grey NORMAL; NORMAL's is wired because that is the tier apps render at.
inline const uint8_t LEVEL_LUM4_M5PAPERS3[4] = { 255, 189, 99, 0 };

// ---- 16-grey decision-engine tables ----------------------------------------
// Tuned 21 July 2026, seeded from the LilyGo T5 S3 GPS set — the two
// panels share the same glass physics family, and 10 of 15 levels passed
// unchanged on the first scan. Divergences the glass insisted on: L1 is
// a single darken (this panel's first-pass response is stronger), L7
// deepened to (3,3,2), L12 collapsed to five pure darkens, L14 gained a
// pass.
inline const uint8_t TUNED16_M5PAPERS3_NORMAL[16][13] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  1  (1,0)  */ {1,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  2  (2,1)  */ {1,1,2,0,0,0,0,0,0,0,0,0,0},
  /*  3  (3,2)  */ {1,1,1,2,2,0,0,0,0,0,0,0,0},
  /*  4 (3,3,1) */ {1,1,1,2,2,2,1,0,0,0,0,0,0},
  /*  5  (4,2)  */ {1,1,1,1,2,2,0,0,0,0,0,0,0},
  /*  6 (2,2,2) */ {1,1,2,2,1,1,0,0,0,0,0,0,0},
  /*  7 (3,3,2) */ {1,1,1,2,2,2,1,1,0,0,0,0,0},
  /*  8 (3,2,2) */ {1,1,1,2,2,1,1,0,0,0,0,0,0},
  /*  9  (5,1)  */ {1,1,1,1,1,2,0,0,0,0,0,0,0},
  /* 10 (7,2,1) */ {1,1,1,1,1,1,1,2,2,1,0,0,0},
  /* 11  (7,1)  */ {1,1,1,1,1,1,1,2,0,0,0,0,0},
  /* 12  (5,0)  */ {1,1,1,1,1,0,0,0,0,0,0,0,0},
  /* 13 (11,1)  */ {1,1,1,1,1,1,1,1,1,1,1,2,0},
  /* 14  (9,0)  */ {1,1,1,1,1,1,1,1,1,0,0,0,0},
  /* 15 (13,0)  */ {1,1,1,1,1,1,1,1,1,1,1,1,1},
};

// Charge-matched removes (see the LilyGo tables for the method and the
// darkens-first shape rationale). Net whitens per level equal the
// apply's net darkens: L1:1 L2:1 L3:1 L4:1 L5:2 L6:2 L7:2 L8:3 L9:4
// L10:6 L11:6 L12:5 L13:10 L14:9 L15:13.
inline const uint8_t TUNED16_M5PAPERS3_NORMAL_REMOVE[16][13] = {
  /*  0 */ {0,0,0,0,0,0,0,0,0,0,0,0,0},   // never removed
  /*  1 */ {2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  2 */ {1,1,2,2,2,0,0,0,0,0,0,0,0},
  /*  3 */ {1,1,1,1,2,2,2,2,2,0,0,0,0},
  /*  4 */ {1,1,1,1,1,2,2,2,2,2,2,0,0},
  /*  5 */ {1,1,1,2,2,2,2,2,0,0,0,0,0},
  /*  6 */ {1,1,1,1,2,2,2,2,2,2,0,0,0},
  /*  7 */ {1,1,1,1,2,2,2,2,2,2,0,0,0},
  /*  8 */ {1,1,1,2,2,2,2,2,2,0,0,0,0},
  /*  9 */ {1,1,2,2,2,2,2,2,0,0,0,0,0},
  /* 10 */ {2,2,2,2,2,2,0,0,0,0,0,0,0},
  /* 11 */ {2,2,2,2,2,2,0,0,0,0,0,0,0},
  /* 12 */ {1,1,2,2,2,2,2,2,2,0,0,0,0},
  /* 13 */ {2,2,2,2,2,2,2,2,2,2,0,0,0},
  /* 14 */ {2,2,2,2,2,2,2,2,2,0,0,0,0},
  /* 15 */ {2,2,2,2,2,2,2,2,2,2,2,2,2},
};

// ---- direct grey-to-grey trains --------------------------------------------
// NORMAL set re-tuned 30 July 2026 against the waveform pair above, FIVE of
// six pairs converged (errors 0.5 to 2.7 scan units against a 4.0 tolerance).
// Both directions use the overshoot-and-return shape: leading whitens, then
// darkens that walk the landing back to target.
//
// 2->1 did NOT converge (best 10.0) and is deliberately absent, so that pair
// falls back to the DC-correct two-step. Depth search drove it to 0 and could
// get no closer — a ~90 lum lightening between two levels that the new
// calibration placed far apart. Everything else lands tightly.
inline const uint8_t DIRECT_M5PAPERS3_NORMAL[4][4][26] = {
  { {0}, {0}, {0}, {0} },
  { /* from 1 */
    {0},
    {0},
    /* 1->2 net +1  (7)  err 2.7 */ {2,2,2,1,1,1,1},
    /* 1->3 net +8  (14) err 0.5 */ {2,2,2,1,1,1,1,1,1,1,1,1,1,1},
  },
  { /* from 2 */
    {0},
    {0},                                  /* 2->1 not converged: two-step */
    {0},
    /* 2->3 net +7  (13) err 1.5 */ {2,2,2,1,1,1,1,1,1,1,1,1,1},
  },
  { /* from 3 */
    {0},
    /* 3->1 net -8  (14) err 1.2 */ {2,2,2,2,2,2,2,2,2,2,2,1,1,1},
    /* 3->2 net -7  (17) err 0.9 */ {2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1},
    {0},
  },
};

// FAST set (tuned 21 July 2026, same card method, all six pairs within
// ±3.0). FAST potentials Q=(4,5,7), so the nets are small — but FAST
// darken passes are weak (no inter-pass delay), so the darkening directs
// need leading whitens plus a LONG charge-neutral darken run to actually
// move the glass. Five of six trains run past FAST's 7 passes (extension
// passes are nearly free undelayed). The lightening directs needed
// almost nothing: whitens bite hard even short.
inline const uint8_t DIRECT_M5PAPERS3_FAST[4][4][26] = {
  { {0}, {0}, {0}, {0} },
  { /* from 1 */
    {0},
    {0},
    /* 1->2 net +1 (9)  */ {2,2,2,2,1,1,1,1,1},
    /* 1->3 net +3 (9)  */ {2,2,2,1,1,1,1,1,1},
  },
  { /* from 2 */
    {0},
    /* 2->1 net -1 (1)  */ {2},
    {0},
    /* 2->3 net +2 (10) */ {2,2,2,2,1,1,1,1,1,1},
  },
  { /* from 3 */
    {0},
    /* 3->1 net -3 (5)  */ {2,2,2,2,1},
    /* 3->2 net -2 (8)  */ {2,2,2,2,2,1,1,1},
    {0},
  },
};

#endif // EPD_PAINTER_M5PAPERS3_TRAINS_H

// ============================================================================
// LilyGo_H716_Trains.h — every waveform the LilyGo T5 4.7" EPD47 H716 drives.
//
// One file per board: the 4-level waveform pairs (FAST/NORMAL/HIGH), the
// 16-grey decision-engine tables, and the grey-to-grey direct trains. The
// preset in EPD_Painter_presets.h carries pins and timing only and points
// here; auto-detection therefore selects these tables with the board. A
// tuned blob in NVS (see EPD_Painter_tuned.h and examples/other/tuneup)
// overrides them at begin().
//
// Everything below was measured on one H716 panel with the self-tuning rig
// (examples/other/tuneup, flatbed scanner in the loop): the 16-grey NORMAL
// and 4-level NORMAL tables on 28 July 2026; the FAST 4-level pair, its
// direct trains, and the 16-grey HIGH tables on 30 July. All at the
// preset's timing: 20 ms pass period, 4 us row charge, 1000 ns LE (FAST has
// no configurable period — 7 undelayed passes always). The tables are
// valid ONLY at that timing.
//
// Every set here is complete and wired in EPD_Painter_presets.h. The one
// gap is not a missing table but a Config limitation: there is a single
// level_lum4 pointer shared by FAST and 4-grey NORMAL, so only FAST's
// measured curve can be installed (see LEVEL_LUM4_H716_FAST below).
// ============================================================================

#ifndef EPD_PAINTER_LILYGO_H716_TRAINS_H
#define EPD_PAINTER_LILYGO_H716_TRAINS_H

#include <stdint.h>

// ---- 4-level waveform pairs ------------------------------------------------
// FAST pair evolved from scratch (blobs erased) 30 July 2026 by pair-
// evolution (tuner_evo4.h), levels worst 8.7 units, all 6/6 directs
// converged. The FIRST attempt that session collapsed L1/L2 toward white
// (measured curve 255 255 253 0): evoPairFitness's cubed ghost term is
// right at 16 greys, where losing one of fifteen levels to caution is a
// minor cost, but at FAST's only two free levels it made "barely move
// ink" the fitness-optimal answer. evo4PairFitness (SQUARE, not cube, on
// the residual) fixed it — see tuner_evo4.h. L2's apply now carries a
// deliberate embedded whiten (mixed with darkens, not a pure run); nets
// +4/+5/+7 for L1/L2/L3, still DC-balanced against each remove.
//
// NORMAL pair scanner-tuned 28 July 2026 against the corrected dose
// ladder (worst 6.3 units against an 8.0 tolerance, erase residual 6.4)
// — a plain darken run, this panel's undelayed first passes being
// strong enough that no whiten trim was needed.
//
// The HIGH 4-level pair is the 22 July dithertune set, untouched: the
// HIGH_4 combination has never been through the scanner rig.
inline const EPD_Painter::Waveforms EPD_WF_H716 = {
    .fast_lighter   = { { 0, 2, 0, 2, 2, 2, 0 },
                        { 1, 2, 2, 2, 2, 2, 2 },
                        { 2, 2, 2, 2, 2, 2, 2 } },
    .fast_darker    = { { 1, 1, 0, 0, 0, 1, 1 },
                        { 1, 1, 2, 1, 1, 1, 1 },
                        { 1, 1, 1, 1, 1, 1, 1 } },
    .normal_lighter = { { 1, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                        { 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .normal_darker  = { { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                        { 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
    .high_lighter   = { { 1, 3, 1, 1, 1, 2, 1, 2, 2, 2, 2, 2, 2 },
                        { 1, 1, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .high_darker    = { { 1, 3, 1, 1, 1, 2, 2, 2, 1, 2, 2, 1, 1 },
                        { 1, 1, 1, 1, 2, 2, 1, 1, 2, 1, 2, 1, 1 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
};

// Measured landing of the four FAST levels, 255 = paper white — 30 July
// 2026, same session as the waveform pair above. (4-grey NORMAL measured
// {255,192,101,0} on 28 July — Config carries one level_lum4 pointer for
// both 4-level modes, a limitation the mode records in MODES.md exist to
// remove. The FAST curve is wired because that is what a FAST tuned blob
// installs.)
inline const uint8_t LEVEL_LUM4_H716_FAST[4] = { 255, 201, 141, 0 };

// ---- 16-grey decision-engine tables ----------------------------------------
// Evolutionary tune, 28 July 2026 (raw drive sequences — no grammar; see
// examples/other/tuneup/tuner_evo.h). NORMAL: match card worst 2.3 scan
// units after confirmation, full-height staircase worst 4.2 with ZERO
// inversions. Seeded from this board's previous tuned set.
inline const uint8_t TUNED16_H716_NORMAL[16][13] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 255
  /*  1 */ {1,1,2,2,0,0,0,0,0,0,0,0,0},  // measured 242
  /*  2 */ {1,1,1,1,1,1,2,2,2,0,0,0,0},  // measured 225
  /*  3 */ {1,1,2,1,0,2,0,0,0,0,0,0,0},  // measured 200
  /*  4 */ {0,1,1,2,0,0,0,0,0,0,0,0,0},  // measured 187
  /*  5 */ {1,1,1,2,0,1,2,0,0,0,0,0,0},  // measured 158
  /*  6 */ {1,1,2,1,1,0,1,1,2,1,2,0,0},  // measured 129
  /*  7 */ {0,1,1,0,1,1,0,1,1,1,2,1,2},  // measured  98
  /*  8 */ {1,1,1,0,1,1,1,2,0,0,2,0,1},  // measured  77
  /*  9 */ {1,1,1,1,2,0,0,0,1,0,0,0,0},  // measured  53
  /* 10 */ {1,0,0,1,1,2,1,0,1,0,0,0,0},  // measured  40
  /* 11 */ {1,0,1,1,1,1,2,0,1,0,0,0,0},  // measured  35
  /* 12 */ {1,1,1,0,1,0,0,0,0,0,0,0,0},  // measured  27
  /* 13 */ {0,1,1,0,1,1,0,1,0,0,0,0,0},  // measured  19
  /* 14 */ {1,0,1,1,1,0,0,1,1,0,1,0,0},  // measured   6
  /* 15 */ {1,1,1,1,1,1,1,1,1,1,1,0,0},  // measured   0
};

// Charge-matched removes, optically verified same session (white-landing
// residuals within 3.7 scan units).
inline const uint8_t TUNED16_H716_NORMAL_REMOVE[16][13] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  1 */ {1,2,1,2,0,0,0,0,0,0,0,0,0},
  /*  2 */ {1,1,1,2,2,2,2,2,2,0,0,0,0},
  /*  3 */ {1,1,1,1,2,2,2,2,2,0,0,0,0},
  /*  4 */ {1,1,1,1,2,2,2,2,2,0,0,0,0},
  /*  5 */ {1,1,1,1,2,2,2,2,2,2,0,0,0},
  /*  6 */ {1,1,1,2,2,2,2,2,2,2,0,0,0},
  /*  7 */ {1,1,2,2,2,2,2,2,2,2,0,0,0},
  /*  8 */ {1,1,2,2,2,2,2,2,2,0,0,0,0},
  /*  9 */ {1,1,2,2,2,2,2,2,0,0,0,0,0},
  /* 10 */ {1,2,2,2,2,2,0,0,0,0,0,0,0},
  /* 11 */ {1,2,2,2,2,2,2,0,0,0,0,0,0},
  /* 12 */ {1,2,2,2,2,2,0,0,0,0,0,0,0},
  /* 13 */ {2,2,2,2,2,0,0,0,0,0,0,0,0},
  /* 14 */ {2,2,2,2,2,2,2,0,0,0,0,0,0},
  /* 15 */ {2,2,2,2,2,2,2,2,2,2,2,0,0},
};

// What each level ACTUALLY renders as, measured on the FULL-HEIGHT
// STAIRCASE rather than the tuning card: a photograph is staircase-like
// content, and the two curves differ (the card's rows are cheaper to
// convert, so heavy content runs a slightly different dose). Image
// dithering quantises against this; the staircase measurement repeats
// within 1-2 lum.
inline const uint8_t LEVEL_LUM_H716_NORMAL[16] =
    { 255, 242, 225, 200, 187, 158, 129, 98, 77, 53, 40, 35, 27, 19, 6, 0 };

// HIGH: re-tuned 30 July 2026, 60 generations, ghost-CUBED fitness (the
// 28 July set it replaces was made with the square — see
// tuner_evo.h/evoGhostPow). 20 passes at the same 20 ms period (more
// slots, not slower ones). Confirmation worst 5.0 against 4.0-4.4 units of
// per-patch scan noise, so placement is at the measurement floor; every
// remove residual inside -4.1; staircase ZERO inversions.
//
// Kept over the 28 July set for MINIMUM GAP, which is the number that
// decides banding: 4 lum here against 1 before. A photograph sharing the
// frame shifts levels by ~10-17 lum (the verdict's "vs match card" figure),
// and that shift can only merge neighbours already closer than it — this
// table's 13 lum drift did NOT invert its staircase where the square's
// 17 lum did. Wide spacing is what survives context, not accurate placement.
//
// The light end is deliberately thin (L1 is a lone darken landing 37 lum
// under white) because the cube prices caution highly; the stored curve
// below records where the levels actually land, and the dither mixes
// against it.
inline const uint8_t TUNED16_H716_HIGH[16][32] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 255
  /*  1 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 218
  /*  2 */ {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 207
  /*  3 */ {1,1,0,0,0,0,0,2,1,0,0,0,0,0,0,2,0,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 194
  /*  4 */ {1,1,1,0,1,2,0,0,2,1,0,1,2,1,2,1,0,2,1,2,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 185
  /*  5 */ {1,1,1,2,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 160
  /*  6 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 150
  /*  7 */ {0,1,1,1,0,2,1,0,1,1,1,0,2,0,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 141
  /*  8 */ {0,0,0,0,0,1,2,2,0,1,2,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 116
  /*  9 */ {2,1,1,1,2,1,2,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  82
  /* 10 */ {0,0,0,0,0,0,0,1,2,1,1,0,0,1,2,1,2,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  75
  /* 11 */ {1,0,0,0,0,0,0,0,0,0,1,0,0,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  56
  /* 12 */ {0,1,1,2,1,0,0,0,0,0,0,0,0,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  52
  /* 13 */ {0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  32
  /* 14 */ {2,1,0,1,1,2,1,2,1,0,1,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  11
  /* 15 */ {1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured   0
};

inline const uint8_t TUNED16_H716_HIGH_REMOVE[16][32] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  1 */ {0,0,1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  2 */ {1,1,1,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  3 */ {0,0,0,0,1,2,0,1,0,0,1,1,2,2,1,0,2,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  4 */ {0,1,0,1,2,0,2,2,1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  5 */ {0,0,1,1,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  6 */ {1,1,1,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  7 */ {0,1,0,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  8 */ {0,1,1,1,0,2,1,1,2,2,2,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  9 */ {1,1,1,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 10 */ {1,2,1,0,1,2,2,2,2,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 11 */ {0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 12 */ {1,0,0,2,0,2,2,0,0,0,0,0,0,0,2,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 13 */ {0,1,2,2,2,0,0,0,0,0,0,1,2,2,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 14 */ {0,1,2,0,0,0,0,0,0,0,0,0,2,2,0,2,2,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 15 */ {2,2,2,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

inline const uint8_t LEVEL_LUM_H716_HIGH[16] =
    { 255, 218, 207, 194, 185, 160, 150, 141, 116, 82, 75, 56, 52, 32, 11, 0 };

// ---- direct grey-to-grey trains (NORMAL) -----------------------------------
// Scanner-tuned 28 July 2026, all SIX pairs converged (worst 6.4 scan
// units, tolerance 8.0) — the first complete set on this board. Both
// directions use the overshoot-and-return shape: darkening pairs put
// their charge-balancing whitens first; lightening pairs whiten PAST the
// target and let re-darkens (full fresh-response strength on this glass)
// walk the landing back down. DC-matched to the 4-grey NORMAL pairs
// above: potentials Q=(2,4,13).
inline const uint8_t DIRECT_H716_NORMAL[4][4][26] = {
  { {0}, {0}, {0}, {0} },
  { /* from 1 */
    {0},
    {0},
    /* 1->2 net +2  (6) */ {2,2,1,1,1,1},
    /* 1->3 net +11 (17) */ {2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  },
  { /* from 2 */
    {0},
    /* 2->1 net -2  (4) */ {2,2,2,1},
    {0},
    /* 2->3 net +9 (15) */ {2,2,2,1,1,1,1,1,1,1,1,1,1,1,1},
  },
  { /* from 3 */
    {0},
    /* 3->1 net -11 (15) */ {2,2,2,2,2,2,2,2,2,2,2,2,2,1,1},
    /* 3->2 net -9  (19) */ {2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1},
    {0},
  },
};

// ---- direct grey-to-grey trains (FAST) -------------------------------------
// Scanner-tuned 30 July 2026 against the new FAST levels above, all SIX
// pairs converged (worst 8.7 scan units against a 2.5 tolerance was the
// LEVELS number carried into the direct phase as its reference card, not
// the directs' own error — each pair's own err is noted below and all
// are well inside tolerance). Replaces the July set, which was tuned
// against the previous FAST waveform pair and its potentials and would
// have carried the wrong net charge against this one.
inline const uint8_t DIRECT_H716_FAST[4][4][26] = {
  { {0}, {0}, {0}, {0} },
  { /* from 1 */
    {0},
    {0},
    /* 1->2 net +1 (7)  */ {2,2,2,1,1,1,1},
    /* 1->3 net +3 (11) */ {2,2,2,2,1,1,1,1,1,1,1},
  },
  { /* from 2 */
    {0},
    /* 2->1 net -1 (3)  */ {2,2,1},
    {0},
    /* 2->3 net +2 (12) */ {2,2,2,2,2,1,1,1,1,1,1,1},
  },
  { /* from 3 */
    {0},
    /* 3->1 net -3 (5)  */ {2,2,2,2,1},
    /* 3->2 net -2 (8)  */ {2,2,2,2,2,1,1,1},
    {0},
  },
};

#endif // EPD_PAINTER_LILYGO_H716_TRAINS_H

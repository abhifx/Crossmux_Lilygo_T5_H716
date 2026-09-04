// ============================================================================
// LilyGo_T5S3_GPS_Trains.h — every waveform the LilyGo T5 S3 GPS drives.
//
// One file per board: the 4-level waveform pairs (FAST/NORMAL/HIGH), the
// 16-grey decision-engine tables, and the grey-to-grey direct trains. The
// preset in EPD_Painter_presets.h carries pins and timing only and points
// here; auto-detection therefore selects these tables with the board. A
// tuned blob in NVS (see EPD_Painter_tuned.h and examples/other/tuneup)
// overrides them at begin().
// ============================================================================

#ifndef EPD_PAINTER_LILYGO_T5S3_GPS_TRAINS_H
#define EPD_PAINTER_LILYGO_T5S3_GPS_TRAINS_H

#include <stdint.h>

// ---- 4-level waveform pairs ------------------------------------------------
inline const EPD_Painter::Waveforms EPD_WF_LILYGO_T5S3_GPS = {
    // FAST and NORMAL 4-level pairs re-tuned 30 July 2026 by pair-evolution
    // (tuner_evo4.h). FAST: levels worst 7.5 scan units, erase 9.0 — a wash
    // against the 21 July set on levels, kept for its direct trains, all SIX
    // of which converged and three of them near-exactly. FAST's erase is
    // limited by construction: 7 undelayed passes with the DC ledger
    // enforced leave the search only the ORDER of a fixed charge, and 11
    // generations could not better 9.9 by reordering alone.
    //
    // NORMAL: levels worst 2.3, erase 3.3, 6 of 6 directs — better than the
    // seed on BOTH axes (8.7 / 3.9), no trade needed. Every pair exactly DC
    // balanced. Inherited code-3 slots are left verbatim; code 3 now emits
    // FLOAT (EPD_Painter.cpp) so they carry no charge, and the tune was
    // measured with that already true.
    //
    // The HIGH 4-level pair is untouched and has never been through the rig.
    .fast_lighter   = { { 0, 3, 2, 3, 2, 2, 2 },
                        { 3, 2, 2, 2, 2, 2, 3 },
                        { 2, 2, 2, 2, 2, 2, 2 } },
    .fast_darker    = { { 1, 1, 1, 3, 1, 3, 0 },
                        { 1, 3, 3, 1, 1, 1, 1 },
                        { 1, 1, 1, 1, 1, 1, 1 } },
    .normal_lighter = { { 1, 1, 1, 2, 2, 1, 2, 2, 2, 2, 2, 3, 2 },
                        { 2, 3, 1, 2, 2, 2, 2, 2, 3, 2, 3, 3, 2 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .normal_darker  = { { 1, 1, 1, 1, 1, 2, 1, 2, 0, 3, 2, 0, 1 },
                        { 1, 0, 1, 0, 3, 3, 1, 1, 3, 3, 1, 1, 1 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
    .high_lighter   = { { 1, 3, 1, 1, 1, 2, 1, 2, 2, 2, 2, 2, 2 },
                        { 1, 1, 3, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2 },
                        { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },
    .high_darker    = { { 1, 3, 1, 1, 1, 2, 2, 2, 1, 2, 2, 1, 1 },
                        { 1, 1, 1, 1, 2, 2, 1, 1, 2, 1, 2, 1, 1 },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } },
};

// Measured landing of the four FAST levels, 255 = paper white (30 Jul).
// Config carries ONE level_lum4 pointer shared by FAST and 4-grey NORMAL;
// 4-grey NORMAL measured {255,193,106,0} in the same session.
inline const uint8_t LEVEL_LUM4_LILYGO_T5S3[4] = { 255, 209, 149, 0 };

// ---- 16-grey decision-engine tables ----------------------------------------
// Tuned 20 July 2026 UNDER THE CONSTANT PASS PERIOD (15 ms NORMAL) and
// cross-verified: strictly monotonic on both the match card (max err 4.2
// scan units) and the full-height staircase. What the glass taught us
// (vs the formula library's assumptions):
//  - A whiten pass is STRONG: after a short darken run one whiten takes
//    back 20-30 units, near saturation ~10 — never a "half step".
//  - The dose response saturates hard: darken passes 8..13 buy ~3 units
//    total. Deep greys cannot be spaced by run length alone.
//  - Fine steps come from RE-DARKENING after a whiten: patterns like
//    1,1,2,1,1 climb from the lifted grey in small fresh-response steps.
inline const uint8_t TUNED16_LILYGO_T5S3_NORMAL[16][13] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 255
  /*  1 */ {0,1,0,0,1,1,1,1,1,2,2,2,2},  // measured 240
  /*  2 */ {0,0,1,0,1,0,1,0,1,0,0,2,2},  // measured 212
  /*  3 */ {0,1,2,1,1,1,2,1,1,2,2,1,2},  // measured 194
  /*  4 */ {1,0,0,0,0,1,1,1,0,1,2,0,2},  // measured 175
  /*  5 */ {0,1,1,1,1,1,1,0,2,2,0,0,0},  // measured 171
  /*  6 */ {1,1,1,1,1,1,1,1,1,0,0,2,2},  // measured 142
  /*  7 */ {1,2,1,1,1,1,1,1,2,2,1,1,2},  // measured 114
  /*  8 */ {0,1,0,1,1,0,0,2,1,0,0,0,0},  // measured 101
  /*  9 */ {1,1,1,2,1,0,1,1,1,1,2,2,1},  // measured  88
  /* 10 */ {1,0,2,2,1,0,1,0,1,1,2,1,0},  // measured  74
  /* 11 */ {2,1,1,1,1,1,1,1,2,1,1,1,2},  // measured  44
  /* 12 */ {1,1,2,1,2,1,1,1,1,2,1,0,0},  // measured  43
  /* 13 */ {2,1,1,1,1,2,2,1,1,2,1,1,1},  // measured  23
  /* 14 */ {1,0,1,1,1,1,2,0,1,0,0,1,1},  // measured  15
  /* 15 */ {1,1,1,1,1,1,1,1,1,1,1,0,0},  // measured   0
};

// Charge-matched removes (phase D). The DC constraint: under the
// constant pass period every pass carries equal dose, so a train's net
// charge is simply darkens - whitens; a paint+unpaint cycle returns the
// pixel's ledger to zero only if the remove's net whitens equal the
// apply's net darkens. Scanner-tuned 21 July 2026 with the remove-ladder
// method. What the glass taught us: appended charge-neutral (1,2) scrub
// pairs erase NOTHING — the shape that works is k darkens FIRST
// (deepening toward saturation, optically cheap), then all net+k whitens
// firing from a dark state where whitens are strong. All 15 levels land
// within 1.5 scan units of clear-white.
// Apply nets: L1:0 L2:+1 L3:+1 L4:+1 L5:+2 L6:+2 L7:+3 L8:+3 L9:+4
//             L10:+6 L11:+6 L12:+5 L13:+10 L14:+8 L15:+13
inline const uint8_t TUNED16_LILYGO_T5S3_NORMAL_REMOVE[16][13] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  1 */ {1,0,2,2,1,0,1,0,1,2,2,2,2},
  /*  2 */ {1,2,1,1,1,0,2,2,0,0,2,2,2},
  /*  3 */ {1,1,1,0,0,1,2,2,2,2,2,2,0},
  /*  4 */ {1,1,1,2,2,2,0,2,2,0,0,0,2},
  /*  5 */ {0,1,1,2,1,2,2,0,2,2,2,0,2},
  /*  6 */ {1,2,2,2,2,0,0,2,2,2,1,2,2},
  /*  7 */ {2,1,1,2,2,2,2,1,2,2,0,0,2},
  /*  8 */ {2,1,1,1,1,2,2,2,2,2,2,0,0},
  /*  9 */ {2,2,2,2,0,0,0,0,0,2,1,2,2},
  /* 10 */ {1,1,2,1,2,0,2,2,2,0,0,0,2},
  /* 11 */ {0,0,0,2,0,2,2,0,2,2,2,2,0},
  /* 12 */ {1,0,2,2,2,2,2,0,0,0,0,2,0},
  /* 13 */ {1,2,1,1,2,2,2,2,2,2,0,2,0},
  /* 14 */ {0,0,2,2,2,2,0,2,2,0,0,0,2},
  /* 15 */ {2,2,2,2,2,2,2,2,2,2,2,0,0},
};


// What each level ACTUALLY renders as, measured on the FULL-HEIGHT
// STAIRCASE (30 Jul): confirmation worst 2.9, staircase worst 5.4, ZERO
// inversions. Image dithering quantises against this.
//
// L11 and L12 land only 1 unit apart. With ~17 lum of card-to-staircase
// drift on this board, that pair merges under photographic content — a
// known narrow spot rather than a measurement error.
inline const uint8_t LEVEL_LUM_LILYGO_T5S3_NORMAL[16] =
    { 255, 240, 212, 194, 175, 171, 142, 114, 101, 88, 74, 44, 43, 23, 15, 0 };

// ---- 16-grey HIGH ----------------------------------------------------------
// Tuned 30 July 2026, 60 generations, cube ghost weighting, 20 passes at the
// same 15 ms period. Confirmation worst 2.5 scan units against ~4 units of
// per-patch scan noise — the best confirmation figure measured on any board —
// staircase worst 5.0 with ZERO inversions.
//
// A first attempt the same afternoon confirmed at 2.7 and was REJECTED: its
// staircase reversed L8/L9 by 18 units, a pair the match card had called a
// 1-unit tie. That is the card-to-staircase drift (21 lum at L8 on that run)
// doing what it does to any pair spaced closer than the drift. This set's
// narrowest gap is 4 units at the dark end, so it is not immune either.
inline const uint8_t TUNED16_LILYGO_T5S3_HIGH[16][32] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 255
  /*  1 */ {1,0,0,0,1,1,1,1,1,0,0,1,2,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 238
  /*  2 */ {0,2,1,0,1,0,1,1,1,1,0,2,0,2,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 219
  /*  3 */ {1,1,1,1,1,1,2,0,0,2,2,1,2,1,0,0,2,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 202
  /*  4 */ {0,0,1,0,0,1,0,1,1,2,2,0,2,0,0,1,1,2,1,2,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 177
  /*  5 */ {0,0,1,0,1,1,1,1,1,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 164
  /*  6 */ {0,0,0,1,2,0,0,1,1,1,1,1,2,0,1,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 137
  /*  7 */ {0,1,2,2,1,2,2,2,1,1,1,2,0,1,1,0,1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 124
  /*  8 */ {1,2,2,2,1,1,1,1,1,1,1,1,0,2,2,1,1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},  // measured 108
  /*  9 */ {0,0,0,0,0,1,2,1,1,1,1,2,1,1,1,1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  85
  /* 10 */ {2,0,1,1,1,1,1,2,0,2,2,1,1,1,2,1,2,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  74
  /* 11 */ {0,0,2,0,0,0,1,0,0,1,1,0,1,0,1,1,1,0,1,2,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  44
  /* 12 */ {1,1,0,2,1,2,1,1,0,2,0,1,0,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  33
  /* 13 */ {1,0,1,1,2,2,2,1,2,1,2,1,1,1,2,1,1,2,1,1,0,0,0,0,0,0,0,0,0,0,0,0},  // measured  27
  /* 14 */ {0,1,2,1,1,1,1,1,0,0,1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured   4
  /* 15 */ {1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // measured   0
};
inline const uint8_t TUNED16_LILYGO_T5S3_HIGH_REMOVE[16][32] = {
  /*  0 (white) */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  1 */ {2,0,1,0,2,1,1,2,2,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  2 */ {1,1,1,2,1,2,1,2,2,1,0,2,1,2,0,2,0,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  3 */ {1,0,1,0,0,0,2,2,2,2,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  4 */ {0,2,1,1,1,2,1,1,1,2,0,2,1,2,2,2,0,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  5 */ {0,0,0,0,2,1,0,1,1,0,0,0,1,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  6 */ {1,1,2,2,2,1,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  7 */ {1,0,1,1,1,2,1,0,2,2,0,1,2,2,2,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  8 */ {1,2,1,0,2,0,1,2,2,2,2,1,1,2,1,2,2,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /*  9 */ {0,0,2,0,0,2,2,2,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 10 */ {1,0,2,1,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 11 */ {1,2,2,0,0,0,2,2,2,1,2,0,0,2,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 12 */ {0,2,0,1,1,1,2,0,0,2,0,2,0,0,2,0,2,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 13 */ {2,2,1,1,0,1,2,2,2,2,2,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 14 */ {0,2,1,0,2,2,0,0,2,2,2,2,1,0,2,0,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* 15 */ {2,2,2,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

inline const uint8_t LEVEL_LUM_LILYGO_T5S3_HIGH[16] =
    { 255, 238, 219, 202, 177, 164, 137, 124, 108, 85, 74, 44, 33, 27, 4, 0 };

// ---- direct grey-to-grey trains --------------------------------------------
// Tuned 21 July 2026, seeded from the M5PaperS3 finals (the panels share
// a physics family; four of six FAST trains transferred UNCHANGED).
// NORMAL all within ±4.0 of reference, FAST within ±3.3. This panel's
// re-darkens after whitening bite weaker than the M5Paper's, so most
// trains carry more charge-neutral drive mass.
// NORMAL potentials Q=(4,7,13); FAST potentials identical to the
// M5PaperS3's (4,5,7).
inline const uint8_t DIRECT_LILYGO_T5S3_NORMAL[4][4][26] = {
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,2,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
  },
};

inline const uint8_t DIRECT_LILYGO_T5S3_FAST[4][4][26] = {
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,2,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,2,2,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  },
  {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
    {2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},   // not converged - absent
  },
};

#endif // EPD_PAINTER_LILYGO_T5S3_GPS_TRAINS_H

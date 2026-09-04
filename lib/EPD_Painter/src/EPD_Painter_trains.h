// ============================================================================
// EPD_Painter_trains.h — the per-board waveform train files, gathered.
//
// Every board's optical data lives in ONE file per board — the 4-level
// waveform pairs, the 16-grey decision-engine tables, and the direct
// grey-to-grey trains, with the calibration history beside the numbers:
//
//   M5PaperS3_Trains.h         M5PaperS3
//   LilyGo_T5S3_GPS_Trains.h   LilyGo T5 S3 GPS
//   LilyGo_H716_Trains.h       LilyGo T5 4.7" EPD47 H716
//   LilyGo_H752_Trains.h       LilyGo T5 S3 H752 (hand-built 4-level only)
//
// The preset in EPD_Painter_presets.h carries pins and timing and points
// its Config at that board's tables, so selecting a board — explicitly or
// by auto-detection — selects its trains with it. Precedence at runtime:
//
//   1. a tuned blob in NVS (written by examples/other/tuneup), installed
//      automatically by begin() when one validates for this board;
//   2. the board's train file, via the preset;
//   3. the formula library (16-grey) / two-step path (direct), for
//      anything the board has no table for.
//
// Drive-code conventions:
//
// 16-grey tables: drive codes per level (0 float, 1 darken, 2 whiten),
// apply + charge-matched remove, indexed by level 0..15 (level 0 =
// white, never driven). Tuned with the match-card method: each level's
// flat native patch optically matched (flatbed scan) against a
// Bayer-dithered black/white reference of density g/15, under the
// preset's constant 16-grey pass period.
//
// ONE SET PER QUALITY, and they are not interchangeable. A train is
// valid only at the pass period AND the pass count it was calibrated
// at, and the two 16-grey qualities share neither:
//
//   g16_apply / g16_remove            [16][13]  NORMAL, at g16_pass_us_normal
//   g16_apply_high / g16_remove_high  [16][32]  HIGH,   at g16_pass_us_high
//
// No board yet ships a HIGH set — tune one with examples/other/tuneup
// (pick HIGH) and it is stored per-board in NVS. A quality with no table
// falls back to the formula library, which is monotonic but not accurate.
//
// Direct tables: [from][to][26] trains for the 4-level direct
// grey-to-grey engine, NORMAL and FAST sets. DC constraint by
// construction: net darkens = Q(to) - Q(from) with Q(g) the net darkens
// of the quality's apply train, making the charge ledger
// path-independent on any route.
// ============================================================================

#ifndef EPD_PAINTER_TRAINS_H
#define EPD_PAINTER_TRAINS_H

#include "M5PaperS3_Trains.h"
#include "LilyGo_T5S3_GPS_Trains.h"
#include "LilyGo_H716_Trains.h"
#include "LilyGo_H752_Trains.h"

#endif // EPD_PAINTER_TRAINS_H

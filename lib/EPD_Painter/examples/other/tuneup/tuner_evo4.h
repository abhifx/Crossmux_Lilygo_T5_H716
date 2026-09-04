// ============================================================================
// tuner_evo4.h — pair-evolution for the 4-level tiers (FAST and NORMAL_4).
//
// The same design as the 16-grey pair-evolution in tuner_evo.h, scaled to
// three levels: each individual is an (apply, remove) pair — the darker
// row that lands the grey and the lighter row that takes it back to white
// — mutated ONLY by the DC-neutral pair operators (evoMutatePair), so no
// candidate can ever carry a charge imbalance. Fitness is the round trip,
// err^2 + resid^2, and the erase that the ledger demands between
// generations is the measurement of the remove.
//
// This replaces the model-based converger (dose ladder + gap probe +
// damped fixed-point solve) for the levels phase: the model could only
// express (d,w,gap,r) shapes and derived its removes by formula — the
// formula whose net-zero scrub is measurably a ghost on this glass.
//
// Level 3 is the anchor: the dithered references are built from its
// pixels, so its APPLY is pinned — but its REMOVE evolves like any other
// (mutated with apply-edits disabled), because L3's erase residual is
// ghosting like everyone else's.
//
// CARD: same geometry as the 16-grey evo card — a 140 px dithered
// reference band (density g/3 per group of four columns), then four
// 100 px candidate bands, each painted and erased under its own pair set.
// Two scans per generation: the painted card, and the erased card.
// ============================================================================

#pragma once

#if TUNEUP_HAVE_JPEGDEC

static const int EVO4_GENS = 16;

// 4-level fitness. The ghost term is CUBED like the 16-grey tier's, with
// ONE exception: FAST keeps the square (Tony's split, 30 Jul).
//
// The cube is the product decision everywhere it can be afforded — a
// ghost outlives the frame it was made in, a slightly-off grey does not.
// What it costs is that the cheapest way for a level to be erasable is to
// barely move ink, so it biases the search toward thin applies.
//
// FAST cannot afford that bias. Seven undelayed passes leave no room to
// be both well-placed and cleanly erasable, and the cube resolves the
// squeeze by declining to move ink at all: the H716's first from-scratch
// FAST run collapsed L1 and L2 onto white, measured curve 255 255 253 0,
// with only 4/6 direct pairs converging off that broken base. The square
// re-ran to 255 201 141 0 and 6/6 directs.
//
// NORMAL_4 has 13 delayed passes — room enough for the cube's caution,
// and it is the tier real 4-grey content is actually rendered at, so it
// is where a ghost matters most. HIGH_4 likewise.
//
// Note the direct grey-to-grey search has no ghost term to weight at
// all: it is judged on where the transition lands optically, and its DC
// balance is structural (net charge matched by construction) rather than
// a fitness pressure.
static float evo4PairFitness(float err, float resid) {
  const float ge = 1.0f + fabsf(err);
  const float gh = 1.0f + fabsf(resid);
  const float p = (fastQuality == EPD_Painter::Quality::QUALITY_FAST)
                      ? 2.0f : evoGhostPow;
  return ge * ge + powf(gh, p);
}

struct Evo4Lv {
  uint8_t ap[TUNE_LEN_MAX], rm[TUNE_LEN_MAX];                      // elite pair
  uint8_t candAp[EVO_BANDS][TUNE_LEN_MAX], candRm[EVO_BANDS][TUNE_LEN_MAX];
  float   err, resid;               // signed, last generation
  float   sigma;
  int     wins, trials, stale;
  bool    apPinned;                 // L3: the references are made of it
};
// PSRAM, first use — same internal-RAM discipline as ev[] in tuner_evo.h.
static Evo4Lv *e4 = nullptr;
static uint8_t e4EliteBand[4];

// Canvas with candidate bands 0..upto populated; later bands stay white so
// they are not driven yet, earlier bands keep their values so the delta
// engine leaves their drive standing (the trick the 16-grey card uses).
static void evo4FillCanvas(int upto) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int y = 0; y < H; y++) {
    uint8_t *row = fb + (size_t)y * W;
    if (y < EVO_REF_H) {
      for (int x = 0; x < W; x++) {
        const int g = ((x * 16) / W) / 4;
        const int thr = (g * 64 + 1) / 3;
        row[x] = (tuneBayer8[y & 7][x & 7] < thr) ? 3 : 0;
      }
      continue;
    }
    const int band = (y - EVO_REF_H) / EVO_BAND_H;
    if (band > upto) { memset(row, 0, W); continue; }
    for (int x = 0; x < W; x++) row[x] = (uint8_t)(((x * 16) / W) / 4);
  }
}

static void evo4UploadBand(int b, bool removes) {
  for (int lv = 1; lv <= 3; lv++) {
    if (removes) fastUploadRemove(lv, e4[lv].candRm[b]);
    else         fastUploadApply(lv,  e4[lv].candAp[b]);
  }
}

static void evo4Paint() {
  tuneClear();
  for (int b = 0; b < EVO_BANDS; b++) {
    evo4UploadBand(b, false);
    evo4FillCanvas(b);
    tunePaint();
  }
}

// Reverse order, each band erased by ITS OWN candidates' removes — the
// measurement half of every pair. The reference band goes last, under
// band 0's tables (it was painted with them).
static void evo4Erase() {
  for (int b = EVO_BANDS - 1; b >= 0; b--) {
    evo4UploadBand(b, true);
    evo4FillCanvas(b - 1);
    tunePaint();
  }
  memset(epd.getBuffer(), 0, (size_t)epd.width() * epd.height());
  tunePaint();
}

static void evo4Repaint() { evo4Paint(); }

// Group means: the interior two columns of each group of four, per band.
static bool evo4Measure(float ref[4], float nat[EVO_BANDS][4]) {
  tuneRepaint = evo4Repaint;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  for (int g = 0; g < 4; g++) {
    int pa0, pa1, pb0, pb1, sx0, sx1, sy0, sy1;
    tuneColSpan(g * 4 + 1, &pa0, &pa1);
    tuneColSpan(g * 4 + 2, &pb0, &pb1);
    tunePanelRect(pa0, pb1, 12, EVO_REF_H - 12, &sx0, &sx1, &sy0, &sy1);
    ref[g] = tuneRectMean(sx0, sx1, sy0, sy1);
    for (int b = 0; b < EVO_BANDS; b++) {
      const int y0 = EVO_REF_H + b * EVO_BAND_H + 12;
      const int y1 = EVO_REF_H + (b + 1) * EVO_BAND_H - 12;
      tunePanelRect(pa0, pb1, y0, y1, &sx0, &sx1, &sy0, &sy1);
      nat[b][g] = tuneRectMean(sx0, sx1, sy0, sy1);
    }
  }
  return true;
}

// The whole 4-level levels phase. Seeds from the INSTALLED tables (NVS
// blob or preset — the board's best current answer), evolves pairs, and
// leaves the elites live in the waveform tables. Fills fastRef[] and
// fl[1..2].err for the results screen and the level curve, and returns
// the worst level placement and worst erase residual.
static bool fastEvolve(float *worstOut, float *ghostOut) {
  Serial.printf("[evo4] pair-evolution, 4-grey %s, %d passes, %d generations\n",
                fastQualityName(), fastLen, EVO4_GENS);
  if (!e4) e4 = (Evo4Lv *)heap_caps_calloc(4, sizeof(Evo4Lv), MALLOC_CAP_SPIRAM);
  if (!e4) { Serial.println("[evo4] no PSRAM for the evolution state"); return false; }
  randomSeed(micros());
  for (int lv = 1; lv <= 3; lv++) {
    Evo4Lv &L = e4[lv];
    memset(&L, 0, sizeof(L));
    memcpy(L.ap, fastDarkerRow(lv - 1),  fastLen);
    memcpy(L.rm, fastLighterRow(lv - 1), fastLen);
    evoBalanceRemove(L.ap, L.rm, fastLen);   // once; neutral ops keep it forever
    L.sigma = 1.0f;
    L.apPinned = (lv == 3);
    L.err = 1e9f;
  }

  static float ref[4], nat[EVO_BANDS][4], natW[EVO_BANDS][4];
  float worst = 999, worstR = 999;
  for (int gen = 0; gen < EVO4_GENS; gen++) {
    if (tuneAbortRequested()) { Serial.println("[evo4] aborted"); break; }

    for (int lv = 1; lv <= 3; lv++) {
      Evo4Lv &L = e4[lv];
      memcpy(L.candAp[0], L.ap, TUNE_LEN_MAX);
      memcpy(L.candRm[0], L.rm, TUNE_LEN_MAX);
      for (int b = 1; b < EVO_BANDS; b++) {
        for (int tries = 0; tries < 40; tries++) {
          memcpy(L.candAp[b], L.ap, TUNE_LEN_MAX);
          memcpy(L.candRm[b], L.rm, TUNE_LEN_MAX);
          // Pinned apply: aliasing the remove as both halves confines
          // every operator to the remove, still net-preserving.
          if (L.apPinned) evoMutatePair(L.candRm[b], L.candRm[b], L.sigma, fastLen);
          else            evoMutatePair(L.candAp[b], L.candRm[b], L.sigma, fastLen);
          if (evoNetOfLen(L.candAp[b], fastLen) > 0) break;
        }
      }
      int home = 0;
      for (int i = EVO_BANDS - 1; i > 0; i--) {
        const int j = (int)random(0, i + 1);
        for (int p = 0; p < TUNE_LEN_MAX; p++) {
          uint8_t t = L.candAp[i][p]; L.candAp[i][p] = L.candAp[j][p]; L.candAp[j][p] = t;
          t = L.candRm[i][p];         L.candRm[i][p] = L.candRm[j][p]; L.candRm[j][p] = t;
        }
        if (home == i)      home = j;
        else if (home == j) home = i;
      }
      e4EliteBand[lv] = (uint8_t)home;
    }

    evo4Paint();
    if (!evo4Measure(ref, nat)) return false;
    for (int g = 0; g < 4; g++) fastRef[g] = ref[g];
    evo4Erase();
    if (!evo4Measure(ref, natW)) return false;    // the erased card: residuals

    worst = 0; worstR = 0;
    int improved = 0;
    for (int lv = 1; lv <= 3; lv++) {
      Evo4Lv &L = e4[lv];
      const int home = e4EliteBand[lv];
      float bestSc = 1e18f, bestE = 0, bestR = 0;
      int bestB = home;
      for (int b = 0; b < EVO_BANDS; b++) {
        const float g0 = nat[b][0] - ref[0], g3 = nat[b][3] - ref[3];
        const float e = (nat[b][lv] - ref[lv]) - (g0 + (g3 - g0) * (lv / 3.0f));
        const float r = natW[b][lv] - natW[b][0];
        const float sc = evo4PairFitness(e, r);  // ghost-weighted, tier-scaled
        if (b != home) L.trials++;
        if (sc < bestSc) { bestSc = sc; bestE = e; bestR = r; bestB = b; }
      }
      if (bestB != home) {
        memcpy(L.ap, L.candAp[bestB], TUNE_LEN_MAX);
        memcpy(L.rm, L.candRm[bestB], TUNE_LEN_MAX);
        L.wins++; L.stale = 0; improved++;
      } else L.stale++;
      L.err = bestE;
      L.resid = bestR;
      if (lv != 3 && fabsf(bestE) > worst) worst = fabsf(bestE);  // L3 is the anchor
      if (fabsf(bestR) > worstR) worstR = fabsf(bestR);
      if (L.trials >= 9) {
        const float rate = (float)L.wins / (float)L.trials;
        L.sigma *= (rate > 0.2f) ? 1.3f : 0.85f;
        L.wins = L.trials = 0;
      }
      if (L.stale >= 4) { L.sigma *= 1.5f; L.stale = 0; }
      L.sigma = constrain(L.sigma, 1.0f, 5.0f);
    }
    Serial.printf("[evo4] gen %2d  worst %5.1f  ghost %4.1f  improved %d/3  | "
                  "L1 %+5.1f/%+4.1f  L2 %+5.1f/%+4.1f  L3 rm %+4.1f\n",
                  gen, worst, worstR, improved,
                  e4[1].err, e4[1].resid, e4[2].err, e4[2].resid, e4[3].resid);
    if (worst <= fastTol() && worstR <= fastTol()) {
      Serial.println("[evo4] round trip inside tolerance - stopping early");
      break;
    }
  }

  // Leave the elite pairs live in the waveform tables, and hand the
  // results screen what it draws from.
  for (int lv = 1; lv <= 3; lv++) {
    fastUploadApply(lv,  e4[lv].ap);
    fastUploadRemove(lv, e4[lv].rm);
    if (lv <= 2) fl[lv].err = e4[lv].err;
  }
  *worstOut = worst;
  *ghostOut = worstR;
  return true;
}

#endif  // TUNEUP_HAVE_JPEGDEC

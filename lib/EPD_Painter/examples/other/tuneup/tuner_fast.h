// ============================================================================
// tuner_fast.h — the FAST-quality tune.
//
// FAST is a different machine from NORMAL and HIGH, not a faster setting of
// one. It has no 16-grey mode at all: seven undelayed passes cannot hold
// sixteen distinguishable doses, and setGreyLevels(16) refuses while the
// quality is FAST. So this tune is about the two things FAST actually has:
//
//   1. The four levels. Waveforms::fast_darker[3][7] draws levels 1..3 from
//      white, fast_lighter[3][7] takes them back. Level 3 is pinned to
//      seven darkens — that is as dark as FAST goes, by construction — so
//      only levels 1 and 2 are free, which is why this converges in a
//      fraction of the time the 16-grey tune takes.
//
//   2. The six grey-to-grey direct trains (dir_fast). These are what make
//      FAST animation clean: without them every grey-to-grey pixel
//      two-steps through white and punches a visible hole (the megademo's
//      bouncing-title artifact). NORMAL and the M5PaperS3 have had a tuned
//      set since July; FAST never did.
//
// THE LEVELS PHASE IS PAIR-EVOLUTION — see tuner_evo4.h. Each level's
// apply (darker row) and remove (lighter row) evolve TOGETHER under
// DC-neutral mutation, judged on the whole round trip err^2 + resid^2.
// The model-based converger that used to live here (dose ladder, gap
// probe, damped (d,w,gap,r) solve, formula removes) was superseded by it:
// the formula's net-zero scrub erase is measurably a ghost on this glass,
// and only a searched remove can find the shapes that are not.
//
// This file keeps the shared 4-level plumbing (card, measurement, table
// upload, the pure-dose probe the directs-only path still uses) and the
// direct grey-to-grey search: nine columns carry three references and all
// six transitions in a single paint, so every pair is measured every scan.
// ============================================================================

#pragma once

#if TUNEUP_HAVE_JPEGDEC

// The pass count is fixed by the ENGINE, and differs per quality: FAST runs
// exactly 7 undelayed passes (Waveforms::fast_* are [3][7]), NORMAL and HIGH
// run 13 (Waveforms::normal_* / high_*). Everything below is written against
// fastLen so the same search tunes any of them; declarations use F_LEN_MAX.
//
// This is why 4-level NORMAL is a genuinely different tune and not a setting:
// 13 slots hold shapes 7 cannot, and a train is only valid at the length it
// was measured at — the same rule that keeps the 16-grey NORMAL and HIGH
// tables apart.
static const int F_LEN_MAX = 13;
static int fastLen = 7;
static EPD_Painter::Quality fastQuality = EPD_Painter::Quality::QUALITY_FAST;

// Which waveform pair this quality drives. The three are different widths and
// different arrays, so every read and write of a candidate goes through here
// rather than naming fast_darker directly.
static uint8_t *fastDarkerRow(int lv) {
  auto &wf = epd.driver()._config.waveforms;
  switch (fastQuality) {
    case EPD_Painter::Quality::QUALITY_FAST: return wf.fast_darker[lv];
    case EPD_Painter::Quality::QUALITY_HIGH: return wf.high_darker[lv];
    default:                                 return wf.normal_darker[lv];
  }
}
static uint8_t *fastLighterRow(int lv) {
  auto &wf = epd.driver()._config.waveforms;
  switch (fastQuality) {
    case EPD_Painter::Quality::QUALITY_FAST: return wf.fast_lighter[lv];
    case EPD_Painter::Quality::QUALITY_HIGH: return wf.high_lighter[lv];
    default:                                 return wf.normal_lighter[lv];
  }
}
static const char *fastQualityName() {
  switch (fastQuality) {
    case EPD_Painter::Quality::QUALITY_FAST: return "FAST";
    case EPD_Painter::Quality::QUALITY_HIGH: return "HIGH";
    default:                                 return "NORMAL";
  }
}

// Placement tolerance, as a fraction of a level step. Four levels over the
// same full scale means a step is five times a 16-grey step, so the same
// fraction is a much looser absolute number.
//
// 0.25 (~8 scan units) was the model-based converger's ask, and the pair
// evolution sailed inside it in two generations on its first outing —
// worst 3.7 with ghosts under 4.1. Halved to 0.12 (~4 units): still 2-3x
// the per-patch scan noise, so the search is chasing panel rather than
// noise, and the extra generations are cheap at ~10 s each.
static const float FAST_TOL_STEPS = 0.12f;
static const int   FAST_DIR_CYCLES = 10;

struct FastLv {
  float  err;          // gradient-corrected (native - reference), scan units
};
static FastLv fl[4];
static float fastDose[F_LEN_MAX + 1];   // landing of {1 x n} (probe diagnostic)
static float fastRef[4];            // the four dither references

// One level step in scan units, from the panel's own reference ladder.
static float fastStep() {
  const float span = fastRef[0] - fastRef[3];
  return (span > 20.0f ? span : 20.0f) / 3.0f;
}
static float fastTol() { return FAST_TOL_STEPS * fastStep(); }

// ---------------------------------------------------------------------------
// Cards
// ---------------------------------------------------------------------------

// The 4-level match card. Same idea as the 16-grey one and deliberately
// the same geometry, so tuneMeasureCardOnce() reads it unchanged: sixteen
// columns, but in four groups of four. Each group pairs a Bayer-dithered
// reference of density g/3 — built only from full black and full white
// pixels, so it is the optical truth for that level — against the native
// 4-level grey below it. Four columns per level is not waste: it averages
// out the horizontal bias that a single column carries.
static void tunePaintFastCard() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height(), mid = H / 2;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const int g = ((x * 16) / W) / 4;       // 0..3
      uint8_t v;
      if (y < mid - 3) {
        const int thr = (g * 64 + 1) / 3;
        v = (tuneBayer8[y & 7][x & 7] < thr) ? 3 : 0;
      } else if (y < mid + 3) {
        v = 3;
      } else {
        v = (uint8_t)g;
      }
      fb[(size_t)y * W + x] = v;
    }
}

static void tuneRepaintFastCard() {
  tunePaintFastCard();
  tuneClear();
  tunePaint();
}

// Measure the 4-level card: mean the four columns of each group, then
// remove the panel's vertical gradient by interpolating the deltas the
// two extreme groups show (they are the same drive top and bottom).
static bool tuneMeasureFast(float ref[4], float nat[4]) {
  float r16[16], n16[16];
  tuneRepaint = tuneRepaintFastCard;
  if (!tuneMeasureCardOnce(r16, n16)) return false;
  for (int g = 0; g < 4; g++) {
    float rs = 0, ns = 0;
    for (int c = 0; c < 4; c++) { rs += r16[g * 4 + c]; ns += n16[g * 4 + c]; }
    ref[g] = rs / 4;
    nat[g] = ns / 4;
  }
  Serial.print("[fast] ref:");
  for (int g = 0; g < 4; g++) Serial.printf(" %3.0f", ref[g]);
  Serial.print("  nat:");
  for (int g = 0; g < 4; g++) Serial.printf(" %3.0f", nat[g]);
  Serial.println();
  return true;
}

static void fastErrors(const float ref[4], const float nat[4], float err[4]) {
  const float g0 = nat[0] - ref[0], g3 = nat[3] - ref[3];
  for (int g = 0; g < 4; g++)
    err[g] = (nat[g] - ref[g]) - (g0 + (g3 - g0) * (g / 3.0f));
}

// ---------------------------------------------------------------------------
// Trains
// ---------------------------------------------------------------------------
// Write straight into the live waveform table: paint() re-reads
// Config::waveforms every frame, so an upload takes effect on the next one.
static void fastUploadApply(int lv, const uint8_t t[F_LEN_MAX]) {
  memcpy(fastDarkerRow(lv - 1), t, fastLen);
}
static void fastUploadRemove(int lv, const uint8_t t[F_LEN_MAX]) {
  memcpy(fastLighterRow(lv - 1), t, fastLen);
}

// Net charge of the train a level is ACTUALLY running: darkens minus
// whitens, counted from the live waveform table rather than from the
// converger's (d,w,g,r) state. The distinction matters in a directs-only
// run, where the converger never fills fl[] — counting from fl[] there
// read every level's charge as zero, so every direct train was built with
// net = 0 and none of them was DC-matched to the levels on the glass.
static int fastNetOf(int lv) {
  const uint8_t *t = fastDarkerRow(lv - 1);
  int net = 0;
  for (int i = 0; i < fastLen; i++) { if (t[i] == 1) net++; else if (t[i] == 2) net--; }
  return net;
}
// ---------------------------------------------------------------------------
// Probe: the pure-darken response, 1..7 passes.
//
// Only two slots per card (level 3 is held at seven darkens as the deep
// anchor the gradient correction needs), so the ladder takes three cards
// rather than the single card the 16-grey ladder manages. Cheap: FAST
// paints in well under a second.
// ---------------------------------------------------------------------------
static bool fastProbe() {
  // Two doses per card — levels 1 and 2 — with level 3 held at the full-length
  // darken run as the deep anchor. So the number of cards follows the pass
  // count: doses 1..fastLen-1 have to be measured, and the last one comes free
  // from the anchor.
  //
  // This was a hardcoded 3, which is exactly right at FAST's 7 passes and
  // silently wrong at 13: doses 7..12 were never measured, stayed zero, and
  // zero reads DARKER than any real reading, so the monotone clamp preserved
  // it and the solver believed a 7-darken run landed at maximum black. It only
  // escaped notice because the two free levels happen to want light doses.
  const int ncards = (fastLen - 1 + 1) / 2;   // ceil((fastLen-1)/2)
  Serial.printf("[fast] pure-darken ladder (1..%d passes, %d cards)...\n",
                fastLen, ncards);
  uint8_t anchor[F_LEN_MAX];
  for (int i = 0; i < fastLen; i++) anchor[i] = 1;
  fastUploadApply(3, anchor);

  float ref[4], nat[4];
  for (int card = 0; card < ncards; card++) {
    const int n1 = card * 2 + 1, n2 = card * 2 + 2;
    if (n1 >= fastLen) break;                 // the anchor covers fastLen
    uint8_t t[F_LEN_MAX];
    memset(t, 0, fastLen);
    for (int i = 0; i < n1; i++) t[i] = 1;
    fastUploadApply(1, t);
    memset(t, 0, fastLen);
    for (int i = 0; i < n2; i++) t[i] = 1;
    fastUploadApply(2, t);

    tunePaintFastCard();
    tuneClear();
    tunePaint();
    if (!tuneMeasureFast(ref, nat)) return false;

    // Gradient correction against this frame's own extremes, so the three
    // cards land on one scale.
    const float g0 = nat[0] - ref[0], g3 = nat[3] - ref[3];
    fastDose[n1] = nat[1] - (g0 + (g3 - g0) * (1 / 3.0f));
    if (n2 < fastLen)                          // n2 can overrun on an odd length
      fastDose[n2] = nat[2] - (g0 + (g3 - g0) * (2 / 3.0f));
    if (card == 0) for (int g = 0; g < 4; g++) fastRef[g] = ref[g];
    fastDose[fastLen] = nat[3] - g3;
  }

  for (int n = 2; n <= fastLen; n++)                 // saturation is monotone
    if (fastDose[n] > fastDose[n - 1]) fastDose[n] = fastDose[n - 1];

  Serial.print("[fast] dose curve: ");
  for (int n = 1; n <= fastLen; n++) Serial.printf("%.0f ", fastDose[n]);
  Serial.printf(" (step %.1f units)\n", fastStep());
  return true;
}


// ---------------------------------------------------------------------------
// Phase 2: the six grey-to-grey direct trains.
//
// Card layout, nine columns in three groups (the grey16_testcard 'D' card):
//
//   | ref  2>1  3>1 | ref  1>2  3>2 | ref  1>3  2>3 |
//
// The group's reference column is painted from white, so each transition
// is judged against the same level arrived at the ordinary way, in the same
// frame. All six pairs are therefore measured by every scan — which is why
// this phase is cheap despite being six unknowns.
//
// DC balance is not searched for, it is imposed: a direct train's net
// darkens must equal Q(to) - Q(from), with Q(g) the net darkens of the
// level's own apply train. That makes the charge ledger path-independent —
// white>1>2>3>white nets zero along any route — so the only free parameter
// per pair is how deep the balancing run goes before the payload. Which
// direction that run takes is the shape lesson from the July work:
// darkening transitions put their whitens FIRST (so the darkens land on
// freshly whitened glass, where they are strong), lightening transitions
// put their darkens first for the same reason mirrored.
// ---------------------------------------------------------------------------
static const uint8_t DIR_FROM[9] = { 0, 2, 3,  0, 1, 3,  0, 1, 2 };
static const uint8_t DIR_TO[9]   = { 1, 1, 1,  2, 2, 2,  3, 3, 3 };

static void tunePaintDirCard(const uint8_t *lv) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int i = 0; i < 9; i++) {
    const int x0 = (i * W) / 9, x1 = ((i + 1) * W) / 9;
    for (int y = 0; y < H; y++)
      memset(fb + (size_t)y * W + x0, lv[i], x1 - x0);
  }
}

// Drive the card: establish every column's FROM level, settle, then repaint
// the TO levels in ONE paint. That single paint is the direct engine's
// gate — a pair without a loaded train would two-step through white here
// and read as a bad landing, which is exactly what we want to see.
static void tuneDriveDirCard() {
  tuneClear();
  tunePaintDirCard(DIR_FROM);
  epd.paint();
  tuneWaitIdle();
  delay(300);
  tunePaintDirCard(DIR_TO);
  epd.paint();
  tuneWaitIdle();
}

static void tuneRepaintDirCard() { tuneDriveDirCard(); }

// Read the nine columns, then take out the horizontal trend the three
// reference columns reveal. That trend is real and it is large — the
// 16-grey rescue measured ~13 units across the card — so comparing a
// transition column against its group's reference without correcting it
// would bury a 3-unit landing error under a bias three times the size.
static bool tuneMeasureDirCard(float land[9]) {
  tuneRepaint = tuneRepaintDirCard;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  const int W = epd.width(), H = epd.height();
  for (int i = 0; i < 9; i++) {
    const int px0 = (i * W) / 9 + W / 36, px1 = ((i + 1) * W) / 9 - W / 36;
    int sx0, sx1, sy0, sy1;
    tunePanelRect(px0, px1, (int)(H * 0.08f), (int)(H * 0.92f), &sx0, &sx1, &sy0, &sy1);
    land[i] = tuneRectMean(sx0, sx1, sy0, sy1);
  }
  Serial.print("[fast] dir card:");
  for (int i = 0; i < 9; i++) Serial.printf(" %3.0f", land[i]);
  Serial.println();
  return true;
}

// Net darkens of a level's tuned apply train — the charge a pixel at that
// level is holding.
static int fastQ(int lv) { return lv == 0 ? 0 : fastNetOf(lv); }

// Build a direct train for `net` charge with a balancing run of `depth`.
// Returns its length.
//
// Both directions use the overshoot-and-return shape the July hand-tuned
// tables all share: drive PAST the target toward the far rail, then pull
// back onto it, with `depth` sizing the return leg.
//
//   darkening  depth whitens, then depth + net darkens
//   lightening depth + |net| whitens, then depth re-darkens
//
// The lightening branch was originally the darkening branch mirrored —
// darkens first, then whitens — and that family cannot land at all on
// this glass: its shallowest member is |net| pure whitens, which for
// 3->2 (net -9) already overshoots the target by ~60 scan units, and
// every deeper member lands lighter still. Whitens-first puts the
// landing leg on the re-darkens instead, which fire from a light state
// at full fresh-response strength and walk the pixel DOWN to the target.
static int fastDirTrain(int net, int depth, uint8_t *out, int cap) {
  memset(out, 0, cap);
  int p = 0;
  if (net >= 0) {
    for (int i = 0; i < depth && p < cap; i++) out[p++] = 2;
    for (int i = 0; i < depth + net && p < cap; i++) out[p++] = 1;
  } else {
    for (int i = 0; i < depth - net && p < cap; i++) out[p++] = 2;   // depth + |net|
    for (int i = 0; i < depth && p < cap; i++) out[p++] = 1;
  }
  return p;
}

static bool fastTuneDirect(uint8_t dirtbl[4][4][EPD_Painter::DEC_WF_LEN_DIR],
                           uint16_t *loadedOut, float *worstOut) {
  // The direct engine wants 25.9 KB of INTERNAL RAM for its sweep list, and
  // 16-grey is still holding 64.8 KB of the same pool for a mode we are not
  // in — enough, with the fastbuffer and the WiFi stack, to make this fail on
  // a board with megabytes of PSRAM spare. Hand it back first.
  while (!epd.driver().paintIdle()) delay(10);
  epd.driver().releaseUnusedBuffers();
  if (!epd.driver().setDirectTransitions(true)) {
    Serial.printf("[fast] direct engine would not enable (internal heap free: "
                  "%u bytes, largest block %u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
  }
  // Start every pair from the two-step so an un-converged pair is left
  // unloaded rather than loaded with a bad train.
  for (int f = 1; f <= 3; f++)
    for (int t = 1; t <= 3; t++)
      if (f != t) epd.driver().setDirectTrain((uint8_t)f, (uint8_t)t, nullptr);

  int depth[9];
  float bestErr[9];
  int   bestDepth[9];
  for (int i = 0; i < 9; i++) { depth[i] = 3; bestErr[i] = 1e9f; bestDepth[i] = 3; }

  const int cap = EPD_Painter::DEC_WF_LEN_DIR;
  uint8_t tr[EPD_Painter::DEC_WF_LEN_DIR];
  float land[9];
  float worst = 999;

  for (int cyc = 0; cyc < FAST_DIR_CYCLES; cyc++) {
    if (tuneAbortRequested()) { Serial.println("[fast] aborted"); break; }
    for (int i = 0; i < 9; i++) {
      const int f = DIR_FROM[i], t = DIR_TO[i];
      if (!f) continue;                                  // reference column
      const int net = fastQ(t) - fastQ(f);
      const int len = fastDirTrain(net, depth[i], tr, cap);
      epd.driver().setDirectTrain((uint8_t)f, (uint8_t)t, tr, len);
    }
    tuneDriveDirCard();
    if (!tuneMeasureDirCard(land)) return false;

    // NO horizontal-trend correction. There used to be one: a line fitted
    // through the three reference columns (0, 3, 6) and subtracted as if the
    // difference between them were a gradient across the glass.
    //
    // They are not the same thing measured three times — they are levels 1, 2
    // and 3, which is the whole point of the card (DIR_TO is 1,1,1 2,2,2
    // 3,3,3). Measured, they read 132 / 85 / 53, so the "gradient" fitted the
    // LEVEL RAMP and came out at -13.2 units per column. Subtracting that
    // inside each group moved every target by up to 26 units and invented most
    // of the error the search was chasing: column 5 read 155 against a real
    // target of 85, a +70 miss, but was scored -26 further out at +96.
    //
    // A real gradient exists here (~2.4 units left-to-right after row_extra was
    // tuned) but it is a third of the tolerance and cannot be recovered from
    // three points that differ by design. Judge each column against its own
    // group's reference.
    worst = 0;
    for (int i = 0; i < 9; i++) {
      const int f = DIR_FROM[i];
      if (!f) continue;
      // This group's own reference column: the level every transition in the
      // group is trying to land on.
      const float target = land[(i / 3) * 3];
      const float err = land[i] - target;
      if (fabsf(err) < bestErr[i]) { bestErr[i] = fabsf(err); bestDepth[i] = depth[i]; }
      if (fabsf(err) > worst) worst = fabsf(err);
      // One step of a damped 1-D search. With the overshoot-and-return
      // shape, depth sizes the returning drive for BOTH directions: a
      // darkening pair's extra darkens and a lightening pair's re-darkens
      // both push the landing darker. A HIGHER scan value is LIGHTER, so
      // err > 0 means this column landed too light and wants more depth —
      // one sign, no branches.
      const int net = fastQ(DIR_TO[i]) - fastQ(f);
      const float step = err / max(2.0f, 0.35f * fastStep());
      const int d = depth[i] + (int)lroundf(step);
      depth[i] = constrain(d, 0, (cap - abs(net)) / 2);
    }
    Serial.printf("[fast] dir cycle %2d  worst %.1f / tol %.1f  depths", cyc, worst, fastTol());
    for (int i = 0; i < 9; i++) if (DIR_FROM[i]) Serial.printf(" %d>%d:%d", DIR_FROM[i], DIR_TO[i], depth[i]);
    Serial.println();
    if (worst <= fastTol()) break;
  }

  // Keep each pair's best-measured depth, and publish only the pairs that
  // actually landed. A pair left on the two-step is slower but correct;
  // a pair loaded with a bad train is neither.
  uint16_t loaded = 0;
  for (int i = 0; i < 9; i++) {
    const int f = DIR_FROM[i], t = DIR_TO[i];
    if (!f) continue;
    const int net = fastQ(t) - fastQ(f);
    const int len = fastDirTrain(net, bestDepth[i], tr, cap);
    if (bestErr[i] <= 1.5f * fastTol()) {
      epd.driver().setDirectTrain((uint8_t)f, (uint8_t)t, tr, len);
      memcpy(dirtbl[f][t], tr, EPD_Painter::DEC_WF_LEN_DIR);
      loaded |= (uint16_t)(1u << ((f << 2) | t));
      Serial.printf("[fast] %d>%d converged: depth %d, net %+d, %d passes, err %.1f\n",
                    f, t, bestDepth[i], net, len, bestErr[i]);
    } else {
      epd.driver().setDirectTrain((uint8_t)f, (uint8_t)t, nullptr);
      Serial.printf("[fast] %d>%d NOT converged (best err %.1f) - left on the two-step\n",
                    f, t, bestErr[i]);
    }
  }
  *loadedOut = loaded;
  *worstOut = worst;
  return true;
}

// ---------------------------------------------------------------------------
// Results.
//
// Its own screen rather than the 16-grey one: that page draws a sixteen
// level ramp out of probeRef[] and tl[].err, neither of which a FAST run
// ever fills. It would have rendered a plausible-looking page of stale
// numbers, which is worse than no page at all.
// ---------------------------------------------------------------------------
static bool fastResultsScreen(float worst, float erase, int npairs, float worstDir) {
  const int W = epd.width(), H = epd.height();
  const float span = max(20.0f, fastRef[0] - fastRef[3]);
  auto pct = [&](float u) { return 100.0f * u / span; };

  uint8_t *fb = epd.getBuffer();
  memset(fb, 0, (size_t)W * H);

  epd.fillRect(0, 0, W, 46, 3);
  epd.setTextColor(0);
  epd.setTextSize(4);
  epd.setCursor(14, 8);
  epd.print("FAST DONE");
  epd.setTextSize(2);
  epd.setCursor(300, 16);
  char hd[72];
  snprintf(hd, sizeof(hd), "levels %+.1f%%   erase %.1f%%   %d/6 pairs",
           pct(worst), pct(erase), npairs);
  epd.print(hd);

  // Four patches, dithered reference above and tuned grey below. A level
  // that is right shows no seam — far easier to judge than a grey alone.
  epd.setTextColor(3);
  const int cw = W / 4, rampY = 60, rampH = 150, half = rampH / 2;
  for (int g = 0; g < 4; g++) {
    const int x = g * cw;
    const int thr = (g * 64 + 1) / 3;
    for (int y = rampY; y < rampY + half; y++)
      for (int px = x + 2; px < x + cw - 2; px++)
        fb[(size_t)y * W + px] = (tuneBayer8[y & 7][px & 7] < thr) ? 3 : 0;
    epd.fillRect(x + 2, rampY + half, cw - 4, rampH - half, (uint8_t)g);
    epd.drawRect(x + 2, rampY, cw - 4, rampH, 3);
    char lb[24];
    snprintf(lb, sizeof(lb), "L%d  %+.1f%%", g,
             pct(g >= 1 && g <= 2 ? fl[g].err : 0.0f));
    epd.setTextSize(2);
    epd.setCursor(x + 12, rampY + rampH + 8);
    epd.print(lb);
  }
  epd.setTextSize(1);
  epd.setCursor(6, rampY - 10);
  epd.print("top = target dither, bottom = tuned level (a good level shows no seam)");

  epd.setTextSize(2);
  epd.setCursor(20, rampY + rampH + 44);
  char l2[96];
  snprintf(l2, sizeof(l2), "Grey-to-grey: %d of 6 pairs tuned, worst landing %+.1f%%",
           npairs, pct(worstDir));
  epd.print(l2);
  epd.setCursor(20, rampY + rampH + 68);
  epd.print(npairs == 6
            ? "All transitions go direct - no two-step through white."
            : "Untuned pairs keep the two-step: correct, but they flash white.");

  Btn save = { 60, H - 130, 360, 110 };
  Btn skip = { 540, H - 130, 360, 110 };
  drawBtn(save, "SAVE", "use on every boot");
  drawBtn(skip, "SKIP", "keep built-in");
  tuneClear();
  tunePaint();
  touchRewake();

  Serial.printf("[fast] results: levels %+.1f%%, erase %.1f%%, %d/6 pairs\n",
                pct(worst), pct(erase), npairs);
  Serial.println("[fast] tap SAVE or SKIP (serial: s = save, k = skip)");
  for (;;) {
    if (Serial.available()) {
      const int c = Serial.read();
      if (c == 's') return true;
      if (c == 'k' || c == 'q') return false;
    }
    int px, py;
    if (touchTapped(px, py)) {
      if (hitBtn(save, px, py)) return true;
      if (hitBtn(skip, px, py)) return false;
    }
    delay(8);
  }
}

// ---------------------------------------------------------------------------
// The whole FAST run.
// ---------------------------------------------------------------------------
// The levels phase, defined in tuner_evo4.h (included after this file).
static bool fastEvolve(float *worstOut, float *ghostOut);
// The 4-level tune, at any quality that has a 4-level mode.
//
//   q         FAST (7 passes) or NORMAL (13). HIGH works and is untested.
//   doLevels  tune the four levels themselves.
//   doDirect  tune the six grey-to-grey trains for THIS quality (dir_fast at
//             FAST, dir_normal at NORMAL) — separable because the directs are
//             the half that goes stale on their own, and re-measuring six
//             transitions is a fraction of the cost of a full level tune.
//
// Levels-only leaves the existing directs alone; directs-only measures against
// whatever levels are currently loaded, which is what you want when the levels
// are already good.
static bool runSelfTuneFast(EPD_Painter::Quality q = EPD_Painter::Quality::QUALITY_FAST,
                            bool doLevels = true, bool doDirect = true) {
  if (!scPort) {
    Serial.println("[fast] no scanner selected");
    return false;
  }
  if (!doLevels && !doDirect) return false;

  fastQuality = q;
  fastLen = (q == EPD_Painter::Quality::QUALITY_FAST) ? 7 : 13;

  Serial.printf("[fast] ===== SELF-TUNE START (4-grey %s, %d passes: %s%s%s) =====\n",
                fastQualityName(), fastLen,
                doLevels ? "levels" : "", (doLevels && doDirect) ? " + " : "",
                doDirect ? "directs" : "");
  Serial.println("[fast] send q to abort between cycles");

  // 16-grey must go before the quality can: the driver refuses FAST while
  // sixteen levels are on, and it is right to.
  if (!epd.driver().setGreyLevels(4)) {
    Serial.println("[fast] setGreyLevels(4) refused");
    return false;
  }
  epd.setQuality(q);

  // The registration patterns (tuneFill(15), the quadrant, the ring) are
  // written for 16-grey and paint level 15. That is not a bug here: the
  // 4-level compaction masks each canvas byte with 0x03, so 15 arrives as
  // 3 = black. Said out loud because it is an accident of the encoding
  // rather than a decision, and a future change to the packing would break
  // registration in this mode only.

  // Keep the tables we are about to overwrite, so a SKIP really does
  // restore what the board had.
  EPD_Painter::Waveforms saved = epd.driver()._config.waveforms;

  // Any failure from here out must put the board back the way it runs:
  // the probes overwrite live trains, and everything after this function
  // (the menu, the demo scan, the 16-grey tunes) expects 16 greys at
  // NORMAL. Early returns used to leave the board in 4-level FAST with
  // half-clobbered waveforms.
  auto bail = [&]() -> bool {
    epd.driver()._config.waveforms = saved;
    epd.driver().reloadDirectTrains();
    epd.driver().setDirectTransitions(false);
    epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
    epd.driver().setGreyLevels(16);
    tuneResultScreen(false, 0, 0);
    return false;
  };

  if (!tuneRegister()) return bail();

  float worst4 = 999, erase = 0;
  if (doLevels) {
    // Pair-evolution (tuner_evo4.h): each level's apply and remove evolve
    // together under DC-neutral mutation, fitness err^2 + resid^2. The
    // erase residual is measured every generation, so the ghost the old
    // formula-derived removes shipped with is now something selection
    // kills rather than something a one-shot verify reports.
    if (!fastEvolve(&worst4, &erase)) return bail();
  } else {
    // Directs-only: the trains already on the board are the reference, and
    // fastNetOf() reads them from the live table. The probe is still needed
    // for fastRef (the dither references every tolerance scales from) —
    // but it overwrites the loaded level trains with its ladder, so put
    // them back the moment it is done. Without this the direct search
    // measured against ladder leftovers, and the export below printed
    // those leftovers as if they were the tuned levels.
    Serial.printf("[fast] levels not tuned this run - measuring directs "
                  "against the %s trains already loaded\n", fastQualityName());
    if (!fastProbe()) return bail();
    epd.driver()._config.waveforms = saved;
    worst4 = 0;      // levels untouched this run; nothing to report on them
  }

  // The measured landing of the four levels, normalised so 0 is paper and
  // 3 is the deepest FAST black. drawGray8() quantises against this — and
  // 4-level mode had no curve of its own until now, so it was quantising
  // against the first four entries of the 16-grey curve, which describe
  // four near-white levels rather than a full ramp.
  uint8_t lum4[4];
  if (doLevels) {
    float meas[4];
    for (int g = 0; g < 4; g++) meas[g] = fastRef[g] + (g >= 1 && g <= 2 ? fl[g].err : 0.0f);
    tuneCurveFrom(meas, 4, lum4, "fast");
  } else {
    // Levels were not measured this run, so keep the curve the board
    // already has — rebuilding one from the converger's stale state would
    // overwrite a measured curve with garbage on save.
    const uint8_t *cur = epd.driver()._config.levelLum(4);
    if (cur) memcpy(lum4, cur, 4);
    else for (int g = 0; g < 4; g++) lum4[g] = (uint8_t)(255 - g * 85);
  }

  static uint8_t dirtbl[4][4][EPD_Painter::DEC_WF_LEN_DIR];
  memset(dirtbl, 0, sizeof(dirtbl));
  uint16_t loaded = 0;
  float worstDir = 999;
  if (doDirect) {
    Serial.printf("[fast] tuning grey-to-grey direct trains (%s)...\n",
                  fastQualityName());
    if (!fastTuneDirect(dirtbl, &loaded, &worstDir))
      Serial.println("[fast] direct phase failed - keeping the two-step for every pair");
  }

  int npairs = 0;
  for (int b = 0; b < 16; b++) if (loaded & (1u << b)) npairs++;

  uint8_t dark[3][F_LEN_MAX], light[3][F_LEN_MAX];
  memset(dark, 0, sizeof(dark));
  memset(light, 0, sizeof(light));
  for (int lv = 0; lv < 3; lv++) {
    memcpy(dark[lv],  fastDarkerRow(lv),  fastLen);
    memcpy(light[lv], fastLighterRow(lv), fastLen);
  }

  Serial.printf("[fast] levels worst %.1f, erase %.1f, %d of 6 direct pairs tuned\n",
                worst4, erase < 0 ? 0.0f : erase, npairs);

  const bool keep = fastResultsScreen(worst4, erase < 0 ? 0.0f : erase,
                                     npairs, worstDir);
  bool saved_ok = false, loaded_ok = false;
  if (keep && fastQuality == EPD_Painter::Quality::QUALITY_FAST) {
    // The blob's waveform slots are FAST-width. Narrowing here rather than
    // widening the record is deliberate: at FAST there is nothing past pass 7
    // to lose, and bumping the format would oblige every reader to migrate.
    uint8_t dark7[3][7], light7[3][7];
    for (int lv = 0; lv < 3; lv++) {
      memcpy(dark7[lv],  dark[lv],  7);
      memcpy(light7[lv], light[lv], 7);
    }
    saved_ok = tunedSaveFast(epd.driver(), dark7, light7, dirtbl, loaded, lum4);
    loaded_ok = saved_ok && tunedLoad(epd.driver(), SET_FAST);
    Serial.printf("[fast] flash save %s, reload %s\n",
                  saved_ok ? "OK" : "FAILED", loaded_ok ? "OK" : "FAILED");
    char sub[96];
    snprintf(sub, sizeof(sub), "FAST: 4 levels + %d of 6 direct pairs, loaded every boot",
             npairs);
    tuneConfirmScreen(loaded_ok ? "SAVED" : "SAVE FAILED",
                      loaded_ok ? "Your panel now uses these FAST trains"
                                : "Nothing was stored",
                      loaded_ok ? sub : "Could not write to storage.");
  } else if (keep) {
    // 4-grey NORMAL/HIGH have nowhere to go in flash yet: the blob's waveform
    // slots are 7 wide and these are 13. Rather than bump the format on the
    // way past — and risk the read-modify-write clobbering the 16-grey tables
    // that share this quality's blob — the tables are printed as source. They
    // stay live in RAM for this session so the result can be looked at, and
    // become permanent by being pasted into the preset.
    Serial.printf("\n// ---- 4-grey %s, measured on this panel ----\n",
                  fastQualityName());
    Serial.printf("// levels worst %.1f units, erase %.1f, %d of 6 direct pairs.\n",
                  worst4, erase < 0 ? 0.0f : erase, npairs);
    Serial.printf("// %d passes. Valid only at this quality's pass timing.\n", fastLen);
    const char *dn = fastQuality == EPD_Painter::Quality::QUALITY_HIGH ? "high" : "normal";
    Serial.printf(".%s_darker = {\n", dn);
    for (int lv = 0; lv < 3; lv++) {
      Serial.print("    {");
      for (int p = 0; p < fastLen; p++)
        Serial.printf("%d%s", dark[lv][p], p < fastLen - 1 ? "," : "");
      Serial.println("},");
    }
    Serial.println("},");
    Serial.printf(".%s_lighter = {\n", dn);
    for (int lv = 0; lv < 3; lv++) {
      Serial.print("    {");
      for (int p = 0; p < fastLen; p++)
        Serial.printf("%d%s", light[lv][p], p < fastLen - 1 ? "," : "");
      Serial.println("},");
    }
    Serial.println("},");
    if (doDirect) {
      Serial.printf("inline const uint8_t DIRECT_%s_%s[4][4][%d] = {\n",
                    TUNED_EXPORT_PREFIX, fastQuality == EPD_Painter::Quality::QUALITY_HIGH
                                             ? "HIGH" : "NORMAL",
                    EPD_Painter::DEC_WF_LEN_DIR);
      for (int f = 0; f < 4; f++) {
        Serial.println("  {");
        for (int t = 0; t < 4; t++) {
          const bool got = (loaded >> ((f << 2) | t)) & 1;
          Serial.print("    {");
          for (int q2 = 0; q2 < EPD_Painter::DEC_WF_LEN_DIR; q2++)
            Serial.printf("%d%s", got ? dirtbl[f][t][q2] : 0,
                          q2 < EPD_Painter::DEC_WF_LEN_DIR - 1 ? "," : "");
          Serial.printf("},%s\n", got ? "" : "   // not converged - absent");
        }
        Serial.println("  },");
      }
      Serial.println("};");
    }
    Serial.println("// ---- end ----\n");
    saved_ok = loaded_ok = true;   // nothing to write; the tables are live
    tuneConfirmScreen("MEASURED",
                      "Tables printed to the serial log",
                      "4-grey NORMAL has no flash slot yet - paste into the preset.");
  } else {
    epd.driver()._config.waveforms = saved;
    epd.driver().reloadDirectTrains();
    Serial.println("[fast] discarded - built-in tables restored");
    tuneConfirmScreen("SKIPPED", "Nothing was changed",
                      "The built-in tables are still in charge.");
  }

  // Put the sketch back the way it runs: the menu, the demo scan and the
  // 16-grey tunes all expect 16 greys at NORMAL, and leaving the board in
  // 4-level FAST with the direct engine's spill planes allocated would be
  // a surprise the next thing to run has to discover for itself.
  epd.driver().setDirectTransitions(false);
  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
  epd.driver().setGreyLevels(16);

  Serial.printf("[fast] ===== SELF-TUNE DONE (%s) =====\n",
                keep ? "saved" : "discarded");
  return keep && saved_ok && loaded_ok;
}

#endif  // TUNEUP_HAVE_JPEGDEC

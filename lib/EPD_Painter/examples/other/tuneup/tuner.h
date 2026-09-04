// ============================================================================
// tuner.h — stage 2: the on-device self-tuning engine.
//
// Runs with the board FACE-DOWN on the scanner glass, so everything is
// driven over serial ('T') or by the menu's TUNE button (which gives you a
// countdown to close the lid). Progress goes to serial; the panel itself
// is busy being the test subject.
//
// The method is the dither-match card (see extras/calibration/dithertune.py
// and the tuning comments in src/EPD_Painter_trains.h): each of the 16
// columns pairs a Bayer-dithered black/white reference (the optical truth
// for level g = density g/15) with the same level painted as a native
// waveform grey. Scanner gamma, lamp falloff and session drift cancel
// because every comparison lives inside one frame of one scan.
//
// GEOMETRY (learned the hard way): thresholded bounding boxes are fooled
// by whatever else is dark near the panel — the USB cable, and the lid
// shadow wedge it props open. So after locating the panel roughly (black
// paint, wide scan) and learning the face-down mirroring (corner mark),
// the tuner paints a black RING 40 px inside the panel edges and walks
// outward from the scan centre to the ring's INNER edges. Those anchors
// give an exact panel-pixel-to-scan-pixel mapping that nothing outside
// the panel can disturb, and every later measurement uses the anchors —
// including scans of an all-white panel, which no threshold could find.
//
// WHAT LIVES HERE is the shared rig, not a search: registration and
// geometry, the match card, scanning and measurement, charge-matched
// removes and their optical verification, the results/confirm screens,
// and the dose-ladder sweep. The 16-grey search itself is the
// evolutionary tuner in tuner_evo.h; the 4-level and direct-train search
// is tuner_fast.h. (A damped (d,w,g,r) grammar converger used to live
// here too — gain learning, rescues, the lot. The evolutionary tuner beat
// it by an order of magnitude on every panel tried and it was removed;
// it is in git history if ever wanted again.)
//
// Charge-matched removes are derived analytically from the tuned applies
// (k darkens then k + net whitens; wr − k = net keeps the DC ledger
// exact), then verified optically: paint the card, repaint white, scan
// the residual against the L0 column.
//
// The result is packaged by EPD_Painter_tuned.h and stored by whatever
// tuned_storage.h implements (LittleFS here); it is reloaded on
// every boot.
// ============================================================================

#pragma once

#if TUNEUP_HAVE_JPEGDEC

// ============================================================================
// Train length — the one thing that differs in KIND between the qualities.
//
// NORMAL spends 13 passes per train, HIGH spends 20. That is not a knob:
// it changes what the tuner can express. A pure-darken run saturates
// somewhere around pass 8-10, so at 13 passes the deep end is nearly
// flat and every level below the knee has to be placed with whitens and
// re-darkens — which need spare passes to fit. HIGH's extra seven are
// exactly that room, and they are also the only route to a black darker
// than a saturating 13-pass run can reach.
//
// Everything in this file that emits, parses, uploads, mutates or stores
// a train reads tuneLen. Each entry point sets it from the quality being
// tuned, and it must agree with the driver's g16TrainLen() —
// a train tuned at one length is meaningless at the other.
// ============================================================================
static const int TUNE_LEN_MAX = EPD_Painter::DEC_WF_LEN16_MAX;   // 20
static int tuneLen = EPD_Painter::DEC_WF_LEN16;                  // 13 until set

// ---- knobs -----------------------------------------------------------------
// Must be a resolution the scanner natively offers — eSCL servers snap an
// unsupported value to the nearest supported one, silently returning a
// much smaller image than asked for (100 dpi became 75 on the Canon,
// which shrank the panel below the geometry checks and failed the run).
static const int   TUNE_DPI        = 150;
// Breather between scans. Not for the panel's benefit - for the PRINTER's.
// The pixma backend opens a fresh TCP connection to the scanner inside every
// acquisition (the persistent SANE handle cannot prevent that), and this
// printer starts refusing those connections under sustained rate. A variable
// rather than a constant because the evolutionary tuner measures 60
// candidates per scan instead of 15, so it can afford to be far more patient
// per scan and still finish sooner.
static int tuneScanGapMs = 4000;
static const float TUNE_REG_W_MM   = 210.0f;
static const float TUNE_REG_H_MM   = 148.0f;

static uint32_t tunePeriodUs = 0;  // 0 = tune at the preset period; 'P' overrides

// ---- geometry --------------------------------------------------------------
struct TuneGeo {
  float x0mm, y0mm, wmm, hmm;  // tight scan window (mm from the glass origin)
  bool  flipx, flipy;          // panel-to-scan axis flips (face-down mirror)
  int   ax0, ax1, ay0, ay1;    // scan px of the ring's inner edges
                               // (= panel px RING_T and W-RING_T / H-RING_T)
  bool  valid;
};
static TuneGeo tgeo = {};
static const int RING_T = 40;  // ring thickness in panel px

// ---- per-level tuner state -------------------------------------------------
struct TuneLv {
  int8_t d, w, g, r;           // train = d darkens, w whitens, g gaps, r re-darkens
  float  err;
  float  gd, gw, gr, gg;       // expected gain per step (scanner units)
  float  bestAbs;              // smallest |err| ever measured for this level …
  int8_t bd, bw, bg, br;       // … and the shape that achieved it
  // A train stored literally rather than as (d,w,g,r) — the evolutionary
  // tuner's shapes are raw drive sequences the grammar cannot express
  // (leading floats, alternating d,w,d,w tails). When hasRaw is set the
  // grammar fields are ignored by tuneTrainOf().
  bool    hasRaw;
  uint8_t raw[TUNE_LEN_MAX];
  // An EVOLVED remove, when the pair-evolution produced one. Selected on
  // its measured erase residual, so it outranks the formula derivation in
  // tuneRemoveOf() — and removeExtra widening does not apply to it
  // (selection already owned that trade-off).
  bool    hasRawRm;
  uint8_t rawRm[TUNE_LEN_MAX];
};
static TuneLv tl[16];
// Landing of {1 x n} (monotone-enforced), n = 1..tuneLen. Indexed by pass
// count, so it needs one more entry than the longest train.
static float tuneDose[TUNE_LEN_MAX + 1];
static float probeRef[16];     // dither reference means from the probe frame

// ---- the unit every tolerance is expressed in ------------------------------
// One level step, in scanner units, measured from this panel's own dither
// reference ladder: white to black over 15 intervals. This is what makes the
// tuner portable - a different panel, scan resolution or lamp changes the
// absolute numbers, and every threshold scales with it automatically.
//
// probeRef[] is filled by whatever measured the card last (the dose sweep,
// or the evolutionary tuner every generation), so this is only valid once a
// card has been read. The floor stops a failed or nonsense read turning
// into absurdly tight tolerances.
static float tuneStep() {
  const float span = probeRef[0] - probeRef[15];
  return (span > 20.0f ? span : 20.0f) / 15.0f;
}
static int   removeExtra[16];

// ============================================================================
// Panel painting (canvas is 8bpp level codes; we're in 16-grey mode)
// ============================================================================
static void tuneWaitIdle() {
  while (!epd.driver().paintIdle()) delay(10);
  delay(150);
}
static void tunePaint() { epd.paint(); tuneWaitIdle(); }

// Activation flash before every measurement.
//
// A pixel's response depends on where its particles have been sitting, so
// measuring straight after arbitrary content bakes that history into the
// result. Driving the panel rail-to-rail several times first agitates the
// ink and leaves every pixel in the same physical state, which is what
// makes one cycle comparable with the next.
//
// This is built from repeated HARD clears rather than a hand-rolled
// black/white paint loop, because a hard clear is DC balanced by
// construction: its four phases run {6,2,4,8} passes with alternating
// polarity, so ten darkening passes are matched by ten whitening ones.
// Flashing by painting level 15 then level 0 would instead rely on the
// removes being charge-matched to the applies — and during convergence
// they are not, since matched removes are only derived at the end. That
// route would inject charge on every cycle.
static const int TUNE_ACTIVATE_CLEARS = 5;      // rail-to-rail flashing before each measurement

// No settling anywhere in here: the flashes run back-to-back and the
// caller drives the panel the instant this returns. Any pause lets the
// agitated particles relax again, which is the whole thing this is trying
// to avoid — so the usual post-paint settle is deliberately not used.
static void tuneClear() {
  for (int i = 0; i < TUNE_ACTIVATE_CLEARS; i++) {
    epd.clear();
    while (!epd.driver().paintIdle()) delay(2);
  }
}

static void tuneFill(uint8_t g) {
  memset(epd.getBuffer(), g, (size_t)epd.width() * epd.height());
}

// Registration pattern: black square over the panel's top-left quadrant.
static void tunePaintQuadrant() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  memset(fb, 0, (size_t)W * H);
  for (int y = 0; y < H / 2; y++) memset(fb + (size_t)y * W, 15, W / 2);
}

// Geometry anchor pattern: black ring hugging the panel edges, white core.
static void tunePaintRing() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  memset(fb, 0, (size_t)W * H);
  for (int y = 0; y < H; y++) {
    if (y < RING_T || y >= H - RING_T) { memset(fb + (size_t)y * W, 15, W); continue; }
    memset(fb + (size_t)y * W, 15, RING_T);
    memset(fb + (size_t)y * W + W - RING_T, 15, RING_T);
  }
}

static const uint8_t tuneBayer8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 },
  { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 },
  { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 },
  { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 },
  { 63, 31, 55, 23, 61, 29, 53, 21 }
};

// The match card (same layout as grey16_testcard 'm'): top half = Bayer
// dithered references, thin black divider, bottom half = native levels.
static void tunePaintMatchCard() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height(), mid = H / 2;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const int g = (x * 16) / W;
      uint8_t v;
      if (y < mid - 3) {
        const int thr = (g * 64 + 7) / 15;
        v = (tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0;
      } else if (y < mid + 3) {
        v = 15;
      } else {
        v = (uint8_t)g;
      }
      fb[(size_t)y * W + x] = v;
    }
}

// Each of these puts the panel back into the state a scan expects, so a
// measurement can be retried after the user has been away fixing things.
static void tuneRepaintCard()  { tunePaintMatchCard(); tuneClear(); tunePaint(); }
static void tuneRepaintRing()  { tunePaintRing();      tuneClear(); tunePaint(); }
static void tuneRepaintBlack() { tuneFill(15);         tuneClear(); tunePaint(); }
static void tuneRepaintQuad()  { tunePaintQuadrant();  tuneClear(); tunePaint(); }
static void tuneRepaintCardThenWhite() {
  tuneRepaintCard();
  tuneFill(0);
  tunePaint();
}

// ============================================================================
// Scan + geometry
// ============================================================================
static bool tuneRegister();               // defined below
static void (*tuneRepaint)() = nullptr;   // repaints the pattern being measured
static bool tuneRecovering = false;       // suppresses nested recovery

// Full-screen "come and fix me" card. The board is face-down on the glass
// while tuning, so this is what the user reads when they pick it up —
// which is the only moment it can possibly be read.
static void tuneAlertScreen(const char *l1, const char *l2, const char *l3) {
  const int W = epd.width(), H = epd.height();
  epd.fillScreen(15);                      // black field: unmissable
  epd.setTextColor(0);
  epd.setTextSize(4);
  epd.setCursor(40, 60);
  epd.print(l1);
  epd.setTextSize(3);
  epd.setCursor(40, 140);
  epd.print(l2);
  epd.setTextSize(2);
  epd.setCursor(40, 200);
  epd.print(l3);
  epd.setCursor(40, H - 60);
  epd.print("Then TAP THE SCREEN to carry on  (serial: any key, q = abort)");
  tunePaint();
}

// An outcome the user must actually see. E-paper takes a moment to settle,
// so this WAITS for a tap rather than flashing by on a timer — a message
// that vanishes before the screen has resolved reads as "nothing happened".
static void tuneConfirmScreen(const char *title, const char *l2, const char *l3) {
  const int W = epd.width(), H = epd.height();

  // On a panel-sized screen the artwork carries the message; the empty
  // rounded panel it leaves is exactly where the button and status go.
  Btn ok;
  if (W == TUNEUP_SPLASH_W && H == TUNEUP_SPLASH_H) {
    tuneupSplashDraw(epd.getBuffer(), W, H);
    ok = { TUNEUP_SPLASH_BTN_X, TUNEUP_SPLASH_BTN_Y,
           TUNEUP_SPLASH_BTN_W, TUNEUP_SPLASH_BTN_H };
    // Status card down the artwork's clear left field: the outcome, a
    // 16-level check, and the button. It sits on white so none of it has
    // to be read against the photograph.
    const int cardX = 14, cardY = 246, cardW = 282, cardH = 250;
    epd.fillRect(cardX, cardY, cardW, cardH, 0);
    epd.drawRect(cardX, cardY, cardW, cardH, 15);

    epd.setTextColor(15);
    epd.setTextSize(3);
    epd.setCursor(cardX + 12, cardY + 14);
    epd.print(title);

    // 16 patches, two rows of eight, each boxed and gapped so adjacent
    // levels can be compared directly against a white surround. Judging
    // level separation through a photograph confuses the panel's level
    // spacing with the dither; this shows the levels themselves.
    // However many levels are actually live. Drawing sixteen patches while
    // the panel is in 4-level mode does not show sixteen greys — the packer
    // masks each value with 0x03, so it shows the same four greys four times
    // over, which reads as a fault in the tune rather than in the card.
    const int nlv = epd.driver().greyLevels();
    const int perRow = nlv > 8 ? 8 : nlv;
    const int pw = 30, ph = 30, gap = 3, gx = cardX + 12, gy = cardY + 66;
    epd.setTextSize(1);
    epd.setCursor(gx, gy - 12);
    epd.printf("%d LEVELS", nlv);
    for (int i = 0; i < nlv; i++) {
      const int cx = gx + (i % perRow) * (pw + gap);
      const int cy = gy + (i / perRow) * (ph + gap);
      epd.fillRect(cx, cy, pw, ph, (uint8_t)i);
      epd.drawRect(cx, cy, pw, ph, (uint8_t)(nlv - 1));
    }

    epd.fillRect(ok.x, ok.y, ok.w, ok.h, 0);
    epd.drawRect(ok.x, ok.y, ok.w, ok.h, 15);
    epd.drawRect(ok.x + 1, ok.y + 1, ok.w - 2, ok.h - 2, 15);
    epd.setTextSize(3);
    epd.setCursor(ok.x + (ok.w - 36) / 2, ok.y + (ok.h - 24) / 2);
    epd.print("OK");
  } else {
    epd.fillScreen(0);
    epd.setTextColor(15);
    epd.setTextSize(5);
    epd.setCursor(40, 90);
    epd.print(title);
    epd.setTextSize(3);
    epd.setCursor(40, 190);
    epd.print(l2);
    epd.setTextSize(2);
    epd.setCursor(40, 250);
    epd.print(l3);
    ok = { W / 2 - 160, 360, 320, 110 };
    drawBtn(ok, "OK");
  }

  // Activate LAST, immediately before driving. The flash only helps if the
  // drive follows at once — leave a gap (a JPEG decode and a dither pass,
  // several seconds) and the particles settle back before the image is
  // painted, so the agitation is wasted. Tony spotted this from the long
  // pause between the flashing stopping and the picture appearing.
  tuneClear();
  epd.paint();
  while (!epd.driver().paintIdle()) delay(10);

  // This page has NO timeout — it waits for the user. Two things are needed
  // to make that hold in practice:
  //  * the tap must land ON the button. Accepting a tap anywhere let a
  //    single phantom dismiss the page, which looked exactly like a timeout.
  //  * touch is ignored until the panel has settled. A full-screen 16-grey
  //    paint throws transients into the GT911, and polling fast enough to
  //    catch real presses also catches those.
  delay(700);
  while (Serial.available()) Serial.read();
  touchRewake();
  int px, py;
  touchTapped(px, py);                    // consume anything already latched
  Serial.printf("[tune] %s - %s (tap OK, or any key)\n", title, l2);
  for (;;) {
    if (Serial.available()) { while (Serial.available()) Serial.read(); return; }
    if (touchTapped(px, py)) {
      Serial.printf("[touch] tap at %d,%d (OK %d..%d/%d..%d)\n", px, py,
                    ok.x, ok.x + ok.w, ok.y, ok.y + ok.h);
      if (hitBtn(ok, px, py)) return;
    }
    delay(8);        // must out-pace GT_FRAME_GAP_MAX_MS (40ms) or every
                     // press is discarded as an EPD phantom
  }
}

// Park until the user acknowledges. Returns false if they abort.
static bool tuneAlertAndWait() {
  Serial.println("\n[tune] *** SCANNER NOT RESPONDING ***");
  Serial.println("[tune] *** power-cycle the scanner, then tap the panel ***");
  tuneAlertScreen("SCANNER ERROR", "Please reset the scanner",
                  "The scanner stopped answering. Switch it off and on again.");
  touchRewake();
  for (;;) {
    if (Serial.available()) return Serial.read() != 'q';
    int px, py;
    if (touchTapped(px, py)) return true;
    delay(8);        // fast enough for the touch confirmation window
  }
}

static bool tuneScan(float x0, float y0, float w, float h) {
  // Flatbeds wedge under sustained scanning (the Canon's BJNP stack stops
  // accepting connections for a minute or so), so be patient before
  // troubling anyone: several quiet retries, and only then ask for hands.
  for (int attempt = 0;; attempt++) {
    delay(tuneScanGapMs);
    const size_t len = esclScan(x0, y0, w, h, TUNE_DPI);
    if (len && decodeGrayFromScan(len)) return true;
    if (attempt < 3) {
      Serial.printf("[tune] scan failed (attempt %d/4), waiting 20 s...\n", attempt + 1);
      delay(20000);
      continue;
    }
    if (tuneRecovering) return false;      // don't nest recovery inside recovery
    if (!tuneAlertAndWait()) return false;

    // They may have picked the board up to read that, so the panel's
    // position on the glass is no longer trustworthy: re-register, then
    // repaint whatever we were measuring and start the scan over.
    //
    // tuneRegister() reassigns tuneRepaint to its own patterns as it works
    // (black, quadrant, ring), so the caller's repaint must be saved around
    // it — without this, recovery repainted the RING and the retried scan
    // measured ring data as card data.
    void (*measuring)() = tuneRepaint;
    tuneRecovering = true;
    const bool ok = tuneRegister();
    tuneRecovering = false;
    tuneRepaint = measuring;
    if (!ok) { Serial.println("[tune] could not re-register after recovery"); return false; }
    if (tuneRepaint) tuneRepaint();
    attempt = -1;                          // fresh retry budget
  }
}

// Adaptive bounding box for the wide registration scan: cutoffs derive
// from the strongest row/column counts, not the window size.
// Pick the dark/light split from the scan itself rather than assuming one.
//
// The threshold here used to be a hardcoded 120: a pixel counted as "dark"
// only if it scanned below that. That silently encoded ONE panel's contrast.
// A second LilyGo, whose black renders lighter under the preset tables, put
// no pixels at all below 120 - so registration reported "no dark panel found"
// with the board sitting plainly on the glass.
//
// The scan is reliably bimodal at this point (bright scanner bed, dark panel
// painted full black), so the split can be measured. Percentiles rather than
// min/max, because a single hot pixel or a shadow at the platen edge would
// drag either extreme. Returns 0 if the image has no usable contrast, which
// is the honest answer when nothing is on the glass.
static uint8_t tuneDarkThreshold() {
  const int W = grayW, H = grayH;
  uint32_t hist[256] = {0};
  const size_t n = (size_t)W * H;
  for (size_t i = 0; i < n; i++) hist[grayImg[i]]++;

  const uint32_t lowCut = (uint32_t)(n * 0.02f), highCut = (uint32_t)(n * 0.98f);
  int lo = 0, hi = 255;
  uint32_t acc = 0;
  for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc >= lowCut)  { lo = v; break; } }
  acc = 0;
  for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc >= highCut) { hi = v; break; } }

  // Too little spread means there is no panel/bed distinction to find.
  if (hi - lo < 25) return 0;
  // 45% of the way up from dark to light: comfortably above the panel's own
  // black, comfortably below the bed, and it tracks whatever contrast this
  // particular panel and scanner happen to produce.
  return (uint8_t)(lo + 0.45f * (hi - lo));
}

static bool tuneBBoxAdaptive(uint8_t thresh, int *bx0, int *by0,
                             int *bx1, int *by1) {
  const int W = grayW, H = grayH;
  if (W > 2048 || H > 2048) return false;
  // PSRAM, not .bss: these were 16 KB of INTERNAL RAM — the pool lwIP
  // buffers received packets in. Starving it made every HTTP response
  // vanish while requests kept sending (int heap measured at 1.8 KB free
  // mid-session), which cost a night of scanner-chasing.
  static int *rowsC = nullptr, *colsC = nullptr;
  if (!rowsC) {
    rowsC = (int *)heap_caps_malloc(2048 * sizeof(int), MALLOC_CAP_SPIRAM);
    colsC = (int *)heap_caps_malloc(2048 * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!rowsC || !colsC) return false;
  }
  memset(rowsC, 0, sizeof(int) * H);
  memset(colsC, 0, sizeof(int) * W);
  for (int y = 0; y < H; y++) {
    const uint8_t *row = grayImg + (size_t)y * W;
    for (int x = 0; x < W; x++)
      if (row[x] < thresh) { rowsC[y]++; colsC[x]++; }
  }
  int rmax = 0, cmax = 0;
  for (int y = 0; y < H; y++) rmax = max(rmax, rowsC[y]);
  for (int x = 0; x < W; x++) cmax = max(cmax, colsC[x]);
  const int rcut = max(40, rmax / 2), ccut = max(40, cmax / 2);
  int x0 = W, x1 = -1, y0 = H, y1 = -1;
  for (int y = 0; y < H; y++)
    if (rowsC[y] > rcut) { if (y < y0) y0 = y; if (y > y1) y1 = y; }
  for (int x = 0; x < W; x++)
    if (colsC[x] > ccut) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
  if (x1 < 0 || y1 < 0 || x1 - x0 < 60 || y1 - y0 < 40) return false;
  *bx0 = x0; *by0 = y0; *bx1 = x1; *by1 = y1;
  return true;
}

// Ring anchors: from the scan centre (always inside the white core), walk
// outward until the dark ring bars are hit. Cable shadows and lid wedges
// sit OUTSIDE the ring and can never move its inner edges.
static bool tuneFindRing() {
  const int W = grayW, H = grayH;
  const int cx = W / 2, cy = H / 2;
  // per-column / per-row dark counts
  auto colDark = [&](int x) {
    int n = 0;
    for (int y = 0; y < H; y++) n += (grayImg[(size_t)y * W + x] < 120);
    return n;
  };
  auto rowDark = [&](int y) {
    int n = 0;
    const uint8_t *row = grayImg + (size_t)y * W;
    for (int x = 0; x < W; x++) n += (row[x] < 120);
    return n;
  };
  const int ccut = (int)(H * 0.25f), rcut = (int)(W * 0.25f);
  int ax0 = -1, ax1 = -1, ay0 = -1, ay1 = -1;
  for (int x = cx; x >= 0; x--)   if (colDark(x) > ccut) { ax0 = x; break; }
  for (int x = cx; x < W; x++)    if (colDark(x) > ccut) { ax1 = x; break; }
  for (int y = cy; y >= 0; y--)   if (rowDark(y) > rcut) { ay0 = y; break; }
  for (int y = cy; y < H; y++)    if (rowDark(y) > rcut) { ay1 = y; break; }
  if (ax0 < 0 || ax1 < 0 || ay0 < 0 || ay1 < 0) return false;
  // Sanity, as a fraction of the frame so the check survives any dpi.
  if (ax1 - ax0 < grayW / 4 || ay1 - ay0 < grayH / 4) return false;
  tgeo.ax0 = ax0; tgeo.ax1 = ax1; tgeo.ay0 = ay0; tgeo.ay1 = ay1;
  Serial.printf("[tune] ring anchors x %d..%d  y %d..%d\n", ax0, ax1, ay0, ay1);
  return true;
}

// Map a panel-pixel rect (interior coordinates) to scan pixels via the
// ring anchors, honouring the face-down flips.
static void tunePanelRect(int px0, int px1, int py0, int py1,
                          int *sx0, int *sx1, int *sy0, int *sy1) {
  const float sx = (float)(tgeo.ax1 - tgeo.ax0) / (epd.width() - 2 * RING_T);
  const float sy = (float)(tgeo.ay1 - tgeo.ay0) / (epd.height() - 2 * RING_T);
  auto mapx = [&](int x) {
    const float o = (x - RING_T) * sx;
    return (int)(tgeo.flipx ? tgeo.ax1 - o : tgeo.ax0 + o);
  };
  auto mapy = [&](int y) {
    const float o = (y - RING_T) * sy;
    return (int)(tgeo.flipy ? tgeo.ay1 - o : tgeo.ay0 + o);
  };
  int a = mapx(px0), b = mapx(px1);
  *sx0 = min(a, b); *sx1 = max(a, b);
  a = mapy(py0); b = mapy(py1);
  *sy0 = min(a, b); *sy1 = max(a, b);
}

static float tuneRectMean(int cx0, int cx1, int ry0, int ry1) {
  cx0 = max(cx0, 0); ry0 = max(ry0, 0);
  cx1 = min(cx1, grayW); ry1 = min(ry1, grayH);
  uint64_t sum = 0;
  uint32_t n = 0;
  for (int y = ry0; y < ry1; y++) {
    const uint8_t *row = grayImg + (size_t)y * grayW;
    for (int x = cx0; x < cx1; x++) { sum += row[x]; n++; }
  }
  return n ? (float)sum / n : 0.0f;
}

// Column i's interior x-range in panel px (25% margins inside the 60 px bar).
static void tuneColSpan(int i, int *px0, int *px1) {
  const int bw = epd.width() / 16;
  *px0 = i * bw + bw / 4;
  *px1 = (i + 1) * bw - bw / 4;
}

// Measure the match card via the anchors: per-column means of the ref and
// native bands. No thresholding — geometry is fixed.
//
// Does NOT set tuneRepaint: the caller knows which card is on the glass
// (this same geometry reads the 16-grey card AND the 4-level one), so the
// caller sets the recovery repaint. Setting it here once repainted a
// 16-grey card into a 4-level measurement after a scanner wedge.
static bool tuneMeasureCardOnce(float ref[16], float nat[16]) {
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  const int H = epd.height();
  for (int i = 0; i < 16; i++) {
    int px0, px1, sx0, sx1, sy0, sy1;
    tuneColSpan(i, &px0, &px1);
    tunePanelRect(px0, px1, (int)(H * 0.06f), (int)(H * 0.42f), &sx0, &sx1, &sy0, &sy1);
    ref[i] = tuneRectMean(sx0, sx1, sy0, sy1);
    tunePanelRect(px0, px1, (int)(H * 0.58f), (int)(H * 0.94f), &sx0, &sx1, &sy0, &sy1);
    nat[i] = tuneRectMean(sx0, sx1, sy0, sy1);
  }
  return true;
}

// Gradient-corrected errors: the L0 and L15 pairs are identical drives top
// and bottom; interpolate their deltas by level.
// Average several scans of the SAME painted card.
//
// Once the errors are down near the tolerance, a scan's own noise — a
// unit or two — is a large fraction of what is being corrected, and the
// converger starts chasing that noise instead of the panel. Averaging two
// reads cuts it by root two. The card is still on the glass after a scan,
// so this costs only the extra scan: no repaint, no extra drive, and the
// panel is not disturbed between the two reads.
static int tuneScansPerMeasure = 1;

static bool tuneMeasureCard(float ref[16], float nat[16]) {
  if (!tuneMeasureCardOnce(ref, nat)) return false;
  for (int k = 1; k < tuneScansPerMeasure; k++) {
    float r2[16], n2[16];
    if (!tuneMeasureCardOnce(r2, n2)) break;      // keep the reads we have
    for (int i = 0; i < 16; i++) {
      ref[i] = (ref[i] * k + r2[i]) / (k + 1);
      nat[i] = (nat[i] * k + n2[i]) / (k + 1);
    }
  }
  return true;
}

static void tunePrintBands(const char *tag, const float ref[16], const float nat[16]) {
  Serial.printf("[tune] %s ref:", tag);
  for (int i = 0; i < 16; i++) Serial.printf(" %3.0f", ref[i]);
  Serial.println();
  Serial.printf("[tune] %s nat:", tag);
  for (int i = 0; i < 16; i++) Serial.printf(" %3.0f", nat[i]);
  Serial.println();
}

// Normalise measured landings into the stored level curve: 255 = the
// measured white (meas[0]), 0 = the deepest measured black (meas[n-1]).
// This is the curve image dithering quantises against, so it is derived
// the same way everywhere it is built.
static void tuneCurveFrom(const float *meas, int n, uint8_t *lum, const char *tag) {
  const float white = meas[0], black = meas[n - 1];
  const float span = (white - black) > 1.0f ? (white - black) : 1.0f;
  Serial.printf("[%s] measured level curve (255=white): ", tag);
  for (int i = 0; i < n; i++) {
    lum[i] = (uint8_t)constrain((int)lroundf(255.0f * (meas[i] - black) / span), 0, 255);
    Serial.printf("%d ", lum[i]);
  }
  Serial.println();
}

// ============================================================================
// Registration: rough locate -> orientation -> ring anchors.
// ============================================================================
static bool tuneRegister() {
  Serial.println("[tune] registration: full black, wide scan...");
  tuneFill(15);
  tuneClear();
  tunePaint();
  tuneRepaint = tuneRepaintBlack;
  if (!tuneScan(0, 0, TUNE_REG_W_MM, TUNE_REG_H_MM)) return false;
  int x0, y0, x1, y1;
  const uint8_t dth = tuneDarkThreshold();
  Serial.printf("[tune] dark threshold %d (measured from the scan)\n", dth);
  if (!dth || !tuneBBoxAdaptive(dth, &x0, &y0, &x1, &y1)) {
    Serial.println("[tune] no dark panel found - is the board face-down on the glass?");
    return false;
  }
  // Generous margins: the window only has to CONTAIN the panel, and the
  // ring anchors (found by walking out from the centre) are indifferent
  // to anything beyond it. Clipping the panel, by contrast, loses a ring
  // bar and kills the run.
  const float mmpp = 25.4f / TUNE_DPI;
  tgeo.x0mm = max(0.0f, x0 * mmpp - 5.0f);
  tgeo.y0mm = max(0.0f, y0 * mmpp - 5.0f);
  tgeo.wmm  = (x1 - x0) * mmpp + 12.0f;
  tgeo.hmm  = (y1 - y0) * mmpp + 12.0f;
  Serial.printf("[tune] panel near %.0f,%.0f  %.0fx%.0f mm\n",
                tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm);

  Serial.println("[tune] orientation: quadrant mark...");
  tunePaintQuadrant();
  tuneClear();
  tunePaint();
  tuneRepaint = tuneRepaintQuad;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;

  // WHICH QUADRANT IS DARKEST, not where the dark pixels are.
  //
  // The obvious implementation - threshold and take a bounding box - is
  // wrong, and it took a second board to show it. The M5PaperS3 has a
  // pronounced dark border around its active area; that border is darker
  // than the threshold and spans the full width, so the "mark" bbox comes
  // back as the whole panel (rmax 613 of 715 px) and the derived centre is
  // the panel centre, which encodes no orientation at all. On a low
  // contrast scan the same border also drags the histogram spread under
  // the 25-level gate and the step fails outright.
  //
  // Comparing quadrant MEANS is immune to both: a border contributes
  // equally to all four, so it cancels, and there is no threshold to fail.
  // The mark is a full quarter of the panel, so the signal is enormous.
  {
    const int qw = grayW / 2, qh = grayH / 2;
    double q[2][2] = { { 0, 0 }, { 0, 0 } };
    long   n[2][2] = { { 0, 0 }, { 0, 0 } };
    // Inset by an eighth all round so the bezel and any bed at the edges
    // stay out of it entirely.
    const int ix = grayW / 8, iy = grayH / 8;
    for (int y = iy; y < grayH - iy; y++) {
      const uint8_t *row = grayImg + (size_t)y * grayW;
      const int qy = (y < qh) ? 0 : 1;
      for (int x = ix; x < grayW - ix; x++) {
        const int qx = (x < qw) ? 0 : 1;
        q[qy][qx] += row[x];
        n[qy][qx]++;
      }
    }
    for (int a = 0; a < 2; a++)
      for (int b = 0; b < 2; b++) q[a][b] = n[a][b] ? q[a][b] / n[a][b] : 255.0;

    int dy = 0, dx = 0;
    double lowest = 1e9;
    for (int a = 0; a < 2; a++)
      for (int b = 0; b < 2; b++)
        if (q[a][b] < lowest) { lowest = q[a][b]; dy = a; dx = b; }

    // The mark is a quarter of the panel painted black: it must be clearly
    // darker than the brightest quadrant or something is wrong.
    double brightest = 0;
    for (int a = 0; a < 2; a++)
      for (int b = 0; b < 2; b++) brightest = max(brightest, q[a][b]);
    Serial.printf("[tune] quadrant means TL %.0f TR %.0f BL %.0f BR %.0f\n",
                  q[0][0], q[0][1], q[1][0], q[1][1]);
    if (brightest - lowest < 15.0) {
      Serial.println("[tune] quadrant mark not found (no quadrant stands out)");
      return false;
    }
    // The mark is painted TOP-LEFT in panel space. Wherever it landed in
    // scan space tells us the mirroring.
    tgeo.flipx = (dx == 1);
    tgeo.flipy = (dy == 1);
  }
  Serial.printf("[tune] orientation: flipx=%d flipy=%d\n", tgeo.flipx, tgeo.flipy);

  Serial.println("[tune] geometry: anchor ring...");
  tunePaintRing();
  tuneClear();
  tunePaint();
  tuneRepaint = tuneRepaintRing;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  if (!tuneFindRing()) {
    Serial.println("[tune] anchor ring not found");
    return false;
  }
  tgeo.valid = true;
  return true;
}

// ============================================================================
// Trains: (d, w, g, r) grammar
// ============================================================================
static void tuneTrainOfShape(int d, int w, int g, int r, uint8_t *out) {
  memset(out, 0, TUNE_LEN_MAX);
  int p = 0;
  for (int i = 0; i < d && p < tuneLen; i++) out[p++] = 1;
  for (int i = 0; i < w && p < tuneLen; i++) out[p++] = 2;
  p += g;                              // float gap
  if (p > tuneLen) p = tuneLen;
  for (int i = 0; i < r && p < tuneLen; i++) out[p++] = 1;
}

static void tuneTrainOf(int lv, uint8_t *out) {
  if (tl[lv].hasRaw) { memcpy(out, tl[lv].raw, TUNE_LEN_MAX); return; }
  tuneTrainOfShape(tl[lv].d, tl[lv].w, tl[lv].g, tl[lv].r, out);
}

// Net charge of whatever train a level ACTUALLY has: darkens minus whitens,
// counted from the drive sequence rather than inferred from (d,w,g,r).
// Inferring it breaks the moment a level goes raw - the remove would be
// charge-matched to a shape the level no longer has, and a DC imbalance is
// ghosting.
static int tuneNetOf(int lv) {
  uint8_t t[TUNE_LEN_MAX];
  tuneTrainOf(lv, t);
  int net = 0;
  for (int i = 0; i < tuneLen; i++) { if (t[i] == 1) net++; else if (t[i] == 2) net--; }
  return net;
}

static void tuneUploadApplies() {
  uint8_t t[TUNE_LEN_MAX];
  for (int lv = 1; lv <= 15; lv++) {
    tuneTrainOf(lv, t);
    epd.driver().setDecisionTrain((uint8_t)(lv << 1), t);
  }
}

// Parse a raw train into the grammar (d 1s, w 2s, g 0s, r 1s).
static void tuneParse(const uint8_t *t, TuneLv *L) {
  int p = 0;
  L->d = L->w = L->g = L->r = 0;
  while (p < tuneLen && t[p] == 1) { L->d++; p++; }
  while (p < tuneLen && t[p] == 2) { L->w++; p++; }
  while (p < tuneLen && t[p] == 0) { L->g++; p++; }
  while (p < tuneLen && t[p] == 1) { L->r++; p++; }
  if (!L->r) L->g = 0;                 // trailing floats aren't a gap
}

static void tuneSeedShapes() {
  // Seed from the preset tables for the quality being tuned, when the
  // board has them. Borrowing the other quality's would be worse than
  // useless: at a different length and pass period those shapes are tens
  // of units off, and the damped converger cannot walk that far.
  const bool high = (tuneLen == EPD_Painter::DEC_WF_LEN16_HIGH);
  const auto &trains = epd.driver()._config.trains;
  const uint8_t *ap = high ? (trains.g16_apply_high ? &trains.g16_apply_high[0][0] : nullptr)
                           : (trains.g16_apply      ? &trains.g16_apply[0][0]      : nullptr);
  for (int lv = 1; lv <= 15; lv++) {
    uint8_t t[TUNE_LEN_MAX];
    memset(t, 0, sizeof(t));
    if (ap) {
      memcpy(t, ap + (size_t)lv * tuneLen, tuneLen);
    } else {
      const int dose = (lv * 2 * tuneLen + 7) / 15;
      const int full = dose / 2;
      for (int p = 0; p < full && p < tuneLen; p++) t[p] = 1;
      if ((dose & 1) && full + 1 < tuneLen) { t[full] = 1; t[full + 1] = 2; }
    }
    tl[lv] = {};
    tuneParse(t, &tl[lv]);
    tl[lv].gd = 15.0f; tl[lv].gw = 22.0f; tl[lv].gr = 12.0f; tl[lv].gg = 5.0f;
    tl[lv].bestAbs = 1e9f;
    tl[lv].bd = tl[lv].d; tl[lv].bw = tl[lv].w;
    tl[lv].bg = tl[lv].g; tl[lv].br = tl[lv].r;
  }
  // Black anchor: a full-length darken run — the deep anchor the dose
  // sweep's gradient correction needs.
  tl[15].d = tuneLen; tl[15].w = 0; tl[15].g = 0; tl[15].r = 0;
  memset(removeExtra, 0, sizeof(removeExtra));
}


// ============================================================================
// Pure-dose ladder. Column c runs {1 x n}; measured against the same
// frame's dither refs. Yields the dose curve (monotone-enforced).
//
// The card has 16 columns and two of them (0 and 15) are the gradient
// anchors, so ONE card can measure at most 14 doses. That was never a
// constraint at 13 passes. At HIGH's 20 it is, so the ladder runs a second
// card and splices — see runDoseSweep().
// ============================================================================

// One card of the ladder: column c (1..14) drives {1 x (base + c - 1)},
// column 15 keeps its full-length deep anchor so the gradient correction
// has something to work with. Fills land[1..14] with gradient-corrected
// landings, and refOut (if given) with the frame's dither references.
static bool tuneDoseCard(int base, float *land, float *refOut) {
  uint8_t t[TUNE_LEN_MAX];
  for (int c = 1; c <= 14; c++) {
    const int n = min(base + c - 1, tuneLen);
    memset(t, 0, sizeof(t));
    for (int i = 0; i < n; i++) t[i] = 1;
    epd.driver().setDecisionTrain((uint8_t)(c << 1), t);
  }
  memset(t, 0, sizeof(t));
  for (int i = 0; i < tuneLen; i++) t[i] = 1;
  epd.driver().setDecisionTrain((uint8_t)(15 << 1), t);

  tunePaintMatchCard();
  tuneClear();
  tunePaint();
  tuneRepaint = tuneRepaintCard;
  float ref[16], nat[16];
  if (!tuneMeasureCard(ref, nat)) return false;
  tunePrintBands(base == 1 ? "probe" : "probe2", ref, nat);
  const float g0 = nat[0] - ref[0], g15 = nat[15] - ref[15];
  for (int c = 1; c <= 14; c++)
    land[c] = nat[c] - (g0 + (g15 - g0) * (c / 15.0f));
  if (refOut) for (int i = 0; i < 16; i++) refOut[i] = ref[i];
  return true;
}


// ============================================================================
// Charge-matched removes: k darkens then k + net whitens (wr − k = net).
// ============================================================================
static void tuneRemoveOf(int lv, uint8_t *out) {
  memset(out, 0, TUNE_LEN_MAX);
  if (lv == 0) return;
  if (tl[lv].hasRawRm) { memcpy(out, tl[lv].rawRm, TUNE_LEN_MAX); return; }
  const int net = tuneNetOf(lv);          // from the real train, raw or not
  if (net <= 0) {
    out[0] = 1; out[1] = 2; out[2] = 1; out[3] = 2;   // charge-neutral scrub
    return;
  }
  // Depth proxy: the darken count of the actual train, so a raw shape sizes
  // its remove sensibly too.
  int darks = 0;
  { uint8_t t[TUNE_LEN_MAX]; tuneTrainOf(lv, t);
    for (int i = 0; i < tuneLen; i++) if (t[i] == 1) darks++; }
  int wr = max(net, max(3, darks / 2 + 3) + removeExtra[lv]);
  if (wr > tuneLen) wr = tuneLen;
  int k = wr - net;
  if (k < 0) k = 0;
  while (k + wr > tuneLen && k > 0) { k--; wr--; }
  if (k + wr > tuneLen) { wr = min(tuneLen, net); k = 0; }
  int p = 0;
  for (int i = 0; i < k; i++) out[p++] = 1;
  for (int i = 0; i < wr && p < tuneLen; i++) out[p++] = 2;
}

static void tuneUploadRemoves() {
  uint8_t t[TUNE_LEN_MAX];
  for (int lv = 1; lv <= 15; lv++) {
    tuneRemoveOf(lv, t);
    epd.driver().setDecisionTrain((uint8_t)((lv << 1) | 1), t);
  }
}

// Optical remove check via the anchors (works on an all-white panel).
static float tuneVerifyRemoves() {
  tunePaintMatchCard();
  tuneClear();
  tunePaint();
  tuneFill(0);
  tunePaint();
  delay(300);
  tuneRepaint = tuneRepaintCardThenWhite;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return -1;
  const int H = epd.height();
  float col[16];
  for (int i = 0; i < 16; i++) {
    int px0, px1, sx0, sx1, sy0, sy1;
    tuneColSpan(i, &px0, &px1);
    tunePanelRect(px0, px1, (int)(H * 0.08f), (int)(H * 0.92f), &sx0, &sx1, &sy0, &sy1);
    col[i] = tuneRectMean(sx0, sx1, sy0, sy1);
  }
  float worst = 0;
  int worstLv = 0;
  Serial.print("[tune] remove residuals: ");
  for (int i = 1; i <= 15; i++) {
    const float res = col[i] - col[0];
    Serial.printf("%+.1f ", res);
    if (fabsf(res) > fabsf(worst)) { worst = res; worstLv = i; }
    // Widen the erase for any level leaving a visible residual. -6.0 was too
    // slack: the evolutionary HIGH run left black at -4.6 - the worst level
    // on the card - and sailed under the trigger. removeExtra adds k darkens
    // AND k whitens, so it buys erase strength at IDENTICAL net charge; it
    // is close to free and there is no reason to be stingy with it.
    if (res < -2.5f) removeExtra[i]++;   // ghost: widen that level's erase
  }
  Serial.printf("  worst L%d %+.1f\n", worstLv, worst);
  return fabsf(worst);
}

// ============================================================================
// Reports
// ============================================================================
static void tuneDumpTables() {
  uint8_t t[TUNE_LEN_MAX];
  Serial.printf("[tune] final apply trains, %d passes (d,w,g,r):\n", tuneLen);
  for (int lv = 1; lv <= 15; lv++) {
    tuneTrainOf(lv, t);
    Serial.printf("  L%-2d (%d,%d,%d,%d)  {", lv, tl[lv].d, tl[lv].w, tl[lv].g, tl[lv].r);
    for (int p = 0; p < tuneLen; p++) Serial.printf("%d%s", t[p], p < tuneLen - 1 ? "," : "");
    Serial.println("}");
  }
  Serial.println("[tune] removes:");
  for (int lv = 1; lv <= 15; lv++) {
    tuneRemoveOf(lv, t);
    Serial.printf("  L%-2d {", lv);
    for (int p = 0; p < tuneLen; p++) Serial.printf("%d%s", t[p], p < tuneLen - 1 ? "," : "");
    Serial.println("}");
  }
}

// ============================================================================
// Results screen: what the tune achieved, and the choice of whether to keep
// it. Painted at the end, so it is what the user reads when they pick the
// board up off the glass.
//
// Errors are shown as a percentage of FULL SCALE (the panel's own white-to-
// black span, measured this run) rather than raw scanner units, because
// that is the number that means something: 5% out is 5% of everything the
// panel can do.
// ============================================================================
static bool tuneResultsScreen(float maxerr, float removeWorst) {
  const int W = epd.width(), H = epd.height();
  const float span = max(20.0f, probeRef[0] - probeRef[15]);   // full scale
  auto pct = [&](float units) { return 100.0f * units / span; };

  uint8_t *fb = epd.getBuffer();
  memset(fb, 0, (size_t)W * H);

  // ---- header ----
  epd.fillRect(0, 0, W, 46, 15);
  epd.setTextColor(0);
  epd.setTextSize(4);
  epd.setCursor(14, 8);
  epd.print("COMPLETE");
  epd.setTextSize(2);
  epd.setCursor(270, 16);
  char hd[64];
  snprintf(hd, sizeof(hd), "worst %+.1f%%   erase %.1f%%",
           pct(maxerr), pct(removeWorst));
  epd.print(hd);

  // ---- the ramp, each level shown against its own reference ----
  // Top half of a patch is the Bayer-dithered reference for that level
  // (built only from full black and full white pixels, so it is the
  // optical truth); the bottom half is the tuned grey. A level that is
  // right has no visible seam — which is far easier to judge than looking
  // at a grey on its own. Patches are outlined so the near-white end
  // doesn't vanish into the page.
  epd.setTextColor(15);
  const int cw = W / 16, rampY = 56, rampH = 84, half = rampH / 2;
  for (int i = 0; i < 16; i++) {
    const int x = i * cw;
    const int thr = (i * 64 + 7) / 15;
    for (int y = rampY; y < rampY + half; y++)
      for (int px = x + 1; px < x + cw - 1; px++)
        fb[(size_t)y * W + px] = (tuneBayer8[y & 7][px & 7] < thr) ? 15 : 0;
    epd.fillRect(x + 1, rampY + half, cw - 2, rampH - half, (uint8_t)i);
    epd.drawRect(x + 1, rampY, cw - 2, rampH, 15);
    epd.setTextSize(1);
    char lb[6];
    snprintf(lb, sizeof(lb), "%d", i);
    epd.setCursor(x + cw / 2 - (i > 9 ? 6 : 3), rampY + rampH + 4);
    epd.print(lb);
  }
  epd.setTextSize(1);
  epd.setCursor(4, rampY - 10);
  epd.print("top = target dither, bottom = tuned grey (a good level shows no seam)");

  // ---- error bars about a zero line ----
  const int zero = 240, ppp = 4;            // pixels per percent
  epd.drawFastHLine(0, zero, W, 15);
  epd.setTextSize(1);
  epd.setCursor(4, zero - 60);
  epd.print("too light");
  epd.setCursor(4, zero + 54);
  epd.print("too dark");
  for (int i = 1; i <= 15; i++) {
    const float p = pct(tl[i].err);
    int h = (int)(fabsf(p) * ppp);
    if (h > 52) h = 52;
    const int x = i * cw + cw / 2 - 9;
    char lb[8];
    snprintf(lb, sizeof(lb), "%+.0f", p);
    const int tx = i * cw + cw / 2 - (fabsf(p) >= 9.5f ? 9 : 6);
    if (p > 0) {
      if (h > 1) epd.fillRect(x, zero - h, 18, h, 15);
      epd.setCursor(tx, zero - h - 11);     // label above the bar
    } else {
      if (h > 1) epd.fillRect(x, zero + 1, 18, h, 15);
      epd.setCursor(tx, zero + h + 4);      // label below the bar
    }
    epd.print(lb);
  }

  // ---- monotonicity: adjacent levels that came out the wrong way round
  // are visible as banding, so say so plainly rather than hiding it in a
  // max-error figure ----
  int inversions = 0;
  for (int i = 1; i < 15; i++) {
    const float a = probeRef[i] + tl[i].err, b = probeRef[i + 1] + tl[i + 1].err;
    if (b > a + 0.5f) inversions++;
  }
  epd.setTextSize(2);
  epd.setCursor(20, 322);
  if (inversions) {
    char w2[80];
    snprintf(w2, sizeof(w2), "WARNING: %d level pair%s inverted (banding)",
             inversions, inversions == 1 ? "" : "s");
    epd.print(w2);
  } else {
    epd.print("Ramp is smooth - every level darker than the last.");
  }
  epd.setCursor(20, 346);
  epd.print(pct(maxerr) <= 5.0f && !inversions
            ? "Good result - safe to keep."
            : "Coarser than the built-in tables.");

  // ---- the choice ----
  Btn save = { 60, 386, 360, 116 };
  Btn skip = { 540, 386, 360, 116 };
  drawBtn(save, "SAVE", "use on every boot");
  drawBtn(skip, "SKIP", "keep built-in");
  tuneClear();          // activate immediately before the drive
  tunePaint();
  touchRewake();          // 15 minutes of painting leaves the GT911 mute

  Serial.printf("[tune] results: worst %+.1f%% of full scale, erase %.1f%%, "
                "%d inversion(s)\n", pct(maxerr), pct(removeWorst), inversions);
  Serial.println("[tune] tap SAVE or SKIP on the panel (serial: s = save, k = skip)");

  // Serial is checked FIRST and unconditionally: if the touch controller
  // has died (it is reset by the panel's own power cycling, and comes back
  // on a different I2C address), this keyboard route is the only way out.
  for (;;) {
    if (Serial.available()) {
      const int c = Serial.read();
      if (c == 's') return true;
      if (c == 'k' || c == 'q') return false;
    }
    int px, py;
    if (touchTapped(px, py)) {
      Serial.printf("[touch] tap at %d,%d  (SAVE %d..%d/%d..%d  SKIP %d..%d)\n",
                    px, py, save.x, save.x + save.w, save.y, save.y + save.h,
                    skip.x, skip.x + skip.w);
      if (hitBtn(save, px, py)) return true;
      if (hitBtn(skip, px, py)) return false;
    }
    delay(8);
  }
}

static void tuneResultScreen(bool ok, float maxerr, int cycles) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      fb[(size_t)y * W + x] = (uint8_t)((x * 16) / W);
  epd.fillRect(0, H / 2 - 40, W, 80, 0);
  epd.setTextSize(3);
  epd.setTextColor(15);
  epd.setCursor(40, H / 2 - 28);
  epd.print(ok ? "SELF-TUNE COMPLETE" : "SELF-TUNE FAILED");
  epd.setTextSize(2);
  epd.setCursor(40, H / 2 + 8);
  if (ok) {
    char b[80];
    snprintf(b, sizeof(b), "max err %.1f units in %d cycles - saved to flash",
             maxerr, cycles);
    epd.print(b);
  } else {
    epd.print("see serial log");
  }
  tunePaint();
}

// Paint the results screen with the figures from a real run, so the layout
// can be checked without sitting through a 15-minute tune.
static void tunePreviewResults() {
  static const float demoErr[16] =
      { 0, 0, -3, 2, -2, 3, -6, -6, 9, -6, -5, -4, -3, 0, 3, 0 };
  for (int i = 0; i < 16; i++) {
    probeRef[i] = 155.0f - i * 6.7f;      // a typical measured reference set
    tl[i].err = demoErr[i];
  }
  // Walk the whole end-of-run flow — results page, then the outcome page —
  // so the artwork and the button placement can be checked without a tune.
  // Nothing is written to flash.
  const bool keep = tuneResultsScreen(9.0f, 3.1f);
  if (keep)
    tuneConfirmScreen("SAVED", "Your panel now uses these greys",
                      "Verified in flash - loaded on every boot (preview only)");
  else
    tuneConfirmScreen("SKIPPED", "Nothing was changed",
                      "The built-in tables are still in charge.");
}

// ============================================================================
// Dose-ladder sweep: what does the pass period actually buy?
//
// The pass period IS the per-pass dose — a row holds its latched drive
// until its next latch, so shortening the period shortens every pixel's
// exposure by the same factor. That makes the period the quantiser of the
// whole system: the finest placement any train can achieve is one pass,
// and one pass is worth however many scan units the period delivers.
//
// The light end is where that bites. From white, the first darken pass is
// the strongest response the panel has, so if it is worth more than a
// level step then levels 1..3 cannot be reached by darkening at all and
// have to be built as darken-then-whiten shapes.
//
// This runs ONLY the ladder, at each period in turn, and prints the curve
// and the step sizes. Six scans a period against forty for a full tune, so
// the period can be chosen by measurement instead of by argument.
// Registration is reused across periods — the board has not moved.
// ============================================================================
static bool tuneAbortRequested();

static bool runDoseSweep(const int *periods, int nper) {
  if (!scPort) { Serial.println("[sweep] no scanner selected"); return false; }
  epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) {
    Serial.println("[sweep] setGreyLevels(16) refused");
    return false;
  }
  tuneSeedShapes();
  if (!tgeo.valid && !tuneRegister()) return false;

  const int savedPeriod = epd.driver()._config.g16_pass_us_high;
  Serial.printf("[sweep] %d passes per train, settle floor %d us\n",
                tuneLen, epd.driver()._config.g16_settle_us);
  for (int i = 0; i < nper; i++) {
    if (tuneAbortRequested()) { Serial.println("[sweep] aborted"); break; }
    const int per = periods[i];
    epd.driver()._config.g16_pass_us_high = per;
    Serial.printf("\n[sweep] ===== %d us per pass (%.2f s per full paint) =====\n",
                  per, per * tuneLen / 1e6f);

    float landA[16], landB[16], ref[16];
    if (!tuneDoseCard(1, landA, ref)) return false;
    for (int i2 = 0; i2 < 16; i2++) probeRef[i2] = ref[i2];
    for (int n = 1; n <= min(14, tuneLen); n++) tuneDose[n] = landA[n];
    if (tuneLen > 14) {
      const int base = tuneLen - 13;
      if (!tuneDoseCard(base, landB, nullptr)) return false;
      float sum = 0;
      int nov = 0;
      for (int c = 1; c <= 14; c++) {
        const int pass = base + c - 1;
        if (pass <= 14) { sum += tuneDose[pass] - landB[c]; nov++; }
      }
      const float shift = nov ? sum / nov : 0.0f;
      for (int c = 1; c <= 14; c++) {
        const int pass = base + c - 1;
        if (pass > 14 && pass <= tuneLen) tuneDose[pass] = landB[c] + shift;
      }
    }
    for (int n = 2; n <= tuneLen; n++)
      if (tuneDose[n] > tuneDose[n - 1]) tuneDose[n] = tuneDose[n - 1];

    const float step = tuneStep();
    Serial.printf("[sweep] %d us  step %.2f units  curve:", per, step);
    for (int n = 1; n <= tuneLen; n++) Serial.printf(" %.0f", tuneDose[n]);
    Serial.println();
    Serial.printf("[sweep] %d us  per-pass deltas:", per);
    Serial.printf(" %.1f", probeRef[0] - tuneDose[1]);
    for (int n = 2; n <= tuneLen; n++) Serial.printf(" %.1f", tuneDose[n - 1] - tuneDose[n]);
    Serial.println();
    // The two numbers that decide the period. The first is how much of a
    // level step the coarsest single pass costs - above 1.0 the light end
    // cannot be placed by darkening. The second is the knee: how many
    // passes before the curve stops moving, which is where black should
    // be anchored.
    const float first = (probeRef[0] - tuneDose[1]) / step;
    int knee = tuneLen;
    for (int n = 2; n <= tuneLen; n++)
      if (tuneDose[n - 1] - tuneDose[n] < 0.15f * step) { knee = n - 1; break; }
    Serial.printf("[sweep] %d us  FIRST PASS = %.2f level steps   knee at pass %d "
                  "(depth %.0f of %.0f units)\n",
                  per, first, knee, probeRef[0] - tuneDose[knee],
                  probeRef[0] - tuneDose[tuneLen]);
  }
  epd.driver()._config.g16_pass_us_high = savedPeriod;
  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
  Serial.println("\n[sweep] done - period restored to the preset");
  return true;
}

// ============================================================================
// The whole show
// ============================================================================
static bool tuneAbortRequested() {
  while (Serial.available()) {
    if (Serial.read() == 'q') return true;
  }
  return false;
}


#else  // !TUNEUP_HAVE_JPEGDEC

// Without an image decoder there is nothing to measure a scan with, so the
// whole tuner compiles out. runSelfTune() in the sketch reports it; only
// the period override needs to still exist for the serial handler.
static uint32_t tunePeriodUs = 0;

#endif

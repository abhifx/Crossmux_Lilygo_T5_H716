// ============================================================================
// web_browser — what a web browser FEELS like on e-paper.
//
// Not a browser: there is no network, no HTML and no layout engine. It is a
// hand-built page that behaves the way a browser's viewport behaves, so the
// question it answers is the one that matters before anybody writes the real
// thing — does scrolling a page of text and photographs on this panel feel
// usable, or does it feel like e-paper?
//
// Drag with your thumb and the page follows it. Let go with some speed and it
// keeps going and settles. Tap the top bar to jump back to the top.
//
// ---------------------------------------------------------------------------
// WHY 4-GREY NORMAL
//
// Scrolling is the worst case a delta-update engine can be given. Every other
// demo in this library wins by repainting the few pixels that changed; when a
// page slides under the viewport, essentially every pixel changes, so the
// delta engine's advantage is gone and what is left is the raw cost of a
// full-frame update. Two things make that affordable:
//
//   4 levels, not 16. NORMAL 16-grey spends 13 passes at 20 ms; the 4-level
//   pair spends 13 short passes with a fixed inter-pass delay, which is a
//   fraction of the frame time. Four greys are enough for text, chrome and
//   dithered photographs.
//
//   DIRECT grey-to-grey trains, which this sketch lets you toggle with 'd'.
//   Off, a pixel changing between two greys is erased to white and redrawn,
//   so a moving page carries a white shimmer through its mid-tones. On, it is
//   driven straight to its new level in one paint — but the frame extends to
//   the longest direct train on screen (19 slots against NORMAL's 13 on this
//   board), so cleaner mid-tones cost frame time. Feel both.
//
// ONE QUALITY: 4-level NORMAL, moving and still. Photographs are hidden while
// the page moves — they are the only mid-grey content, and mid-greys are what
// a moving frame pays for — and drawn again when it stops.
//
// PORTRAIT, because a page wants to be taller than it is wide. 4-level mode
// supports ROTATION_CW (16-grey does not), so the canvas is 540x960.
//
// paintLater() rather than paint(): it submits a frame and returns instead of
// waiting for the panel. If the thumb moves again before the panel finishes,
// the newer frame replaces the queued one and the stale render is dropped —
// so the page tracks the finger as closely as the hardware allows instead of
// falling behind by a queue of frames nobody wants to see any more.
//
// IMAGES are dithered ONCE at startup into level codes and then blitted.
// Re-running the error diffusion every frame would be slower AND would make
// the grain crawl as the offset changed, because clipping the image changes
// which rows the error propagates through.
//
// Board: auto-probed. Requires the Adafruit GFX library and gt911-arduino.
// ============================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "EPD_Painter_Adafruit.h"
#include <gt911_lite.h>
#include "hero_photo.h"                  // 8bpp greyscale, generated
#include "browser_img.h"                 // struct Img (see the note there)

// Portrait: 540 wide x 960 tall.
EPD_PainterAdafruit epd(EPD_PAINTER_PRESET.withRotation(EPD_Painter::Rotation::ROTATION_CW));

// ---- 4-level palette ------------------------------------------------------
// The canvas holds LEVEL CODES, not luminance: 0 = paper white, 3 = black.
// The 4-level packer masks each byte with 0x03, so a code above 3 wraps to
// white rather than clamping — never write 4..15 here.
static const uint8_t WHITE = 0, INK = 3;

// ONE BIT. Ink or paper, nothing between — this sketch never uses levels 1
// and 2 at all.
//
// The engine is still 4-level; there is no 2bpp mode to switch to. What this
// buys is that no pixel ever makes a grey-to-grey transition, and grey-to-grey
// is what costs: without direct trains it is erased to white and redrawn (the
// white flash you can see through a moving photograph), and with them it
// extends the frame to the longest direct train in play. Black and white are
// an apply on the dark plane or a remove on the light plane — one cycle each.
//
// It also lets the inter-pass gap come down. The 4-level tables are
// calibrated at 4 ms, and levels 1 and 2 sit on the responsive part of the
// dose curve, so shortening it moves them. Level 3 is a full darken run and
// level 0 a full whiten run: both saturated, both indifferent to a shorter
// settle. Measured on breakout, 4 ms -> 1 ms took a cycle from 125 ms to
// 86 ms with no visible change.
static const uint8_t LIGHT = WHITE, MID = INK;

// Inter-pass gap, microseconds. Only safe this short because of the above.
static const int PASS_GAP_US = 2000;

static GT911_Lite touch;
static bool     touch_ok = false;
static uint16_t tp_xmax = 0, tp_ymax = 0;
static bool     tp_swap = false;

// Instrumentation: how many frames we asked for versus how many the panel
// actually showed, and how long a render costs. paintsCompleted() counts real
// drive cycles, so submitted-minus-completed is the frames paintLater() threw
// away as stale — which is the number that tells you whether the render loop
// or the glass is the limit.
static uint32_t framesSubmitted = 0, paintsAtStart = 0, renderMs = 0;

// Direct grey-to-grey transitions, toggled at runtime with 'd'.
//
// OFF (the default here): a pixel changing from one grey to another is erased
// to white and redrawn, so a moving page carries a white shimmer through every
// mid-tone. Frames are the quality's plain pass count.
//
// ON: that pixel is driven straight to its new level in one paint. Cleaner
// mid-tones, but the frame EXTENDS to the longest direct train on screen —
// this board's 3->2 NORMAL direct is 19 slots against NORMAL's 13 — so a page
// holding text and photographs pays the longest train every frame.
//
// So it is a genuine trade, not a free win, and it is worth feeling both ways
// round on real content rather than reasoning about it.
static bool wantDirect = false;

// Photographs stay on screen while the page moves.
//
// They were hidden during motion at first, and the reason was real: a 4-level
// dithered photograph is the only mid-grey content on the page, and every
// mid-grey pixel is a grey-to-grey transition that either two-steps through
// white or pays the longest direct train. Text is ink on paper — the cheapest
// transition there is — so a moving page of text alone was much quicker.
//
// In TWO-COLOUR mode that argument disappears: the halftone is pure ink and
// paper, so a photograph costs no more per pixel than the text does. In
// 4-grey mode showing them while scrolling does still cost, and you can feel
// the difference by toggling 'c' mid-drag.
static bool showPhotos = true;
static bool needSettle = false;

// Below this many pixels of movement per frame, the glide STOPS rather than
// creeping. On e-paper a step is a whole frame — a couple of hundred
// milliseconds of drive for a movement you can barely see — so a long slow
// tail is all cost and no benefit. Ending decisively also gets the settle
// frame (with the photographs) up sooner.
static const float MIN_GLIDE_PX = 5.0f;

static int shownScroll = -1;             // scroll position last submitted
static int VW, VH;                       // viewport (canvas) size
static const int CHROME_H = 56;          // fixed browser bar height
static int PAGE_H = 0;                   // virtual page height, computed
static int scrollY = 0;

// ---- dithered images ------------------------------------------------------
// Each is a block of level codes, ready to blit. Dithered once at startup
// against the panel's MEASURED level curve (Config::level_lum) via
// drawGray8(), so the greys land where the tuner says they land.
static Img heroImg  = {0, 0, nullptr};
static Img inlineImg = {0, 0, nullptr};

// Dither a real 8bpp greyscale photograph into level codes, ONCE.
//
// The source stays in flash (memory-mapped on the ESP32, so it reads like
// RAM); only the level block is allocated. drawGray8() error-diffuses against
// Config::level_lum — the levels as measured on THIS panel by the tuner — so
// the four greys land where the glass actually puts them. Quantising against
// an assumed even ramp is what makes dithered photographs band.
static bool makePhoto(Img &img, const uint8_t *grey, int w, int h) {
  uint8_t *bw = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
  int16_t *cur = (int16_t *)calloc(w + 2, sizeof(int16_t));
  int16_t *nxt = (int16_t *)calloc(w + 2, sizeof(int16_t));
  if (!bw || !cur || !nxt) {
    Serial.println("[browser] no PSRAM for the halftone plane");
    heap_caps_free(bw); free(cur); free(nxt);
    return false;
  }

  // Floyd-Steinberg to pure ink or paper. drawGray8() is not used here
  // because it targets whatever level count is LIVE, which is four.
  // Serpentine, with the error shared exactly — truncating toward zero and
  // giving the remainder to the pixel ahead — the same discipline the
  // library's GrayDither uses. A fixed scan direction organises the
  // diffusion bias into column-aligned worms; alternating it breaks them up.
  for (int y = 0; y < h; y++) {
    memset(nxt, 0, (size_t)(w + 2) * sizeof(int16_t));
    const bool rtl = (y & 1);
    for (int i = 0; i < w; i++) {
      const int x = rtl ? (w - 1 - i) : i;
      const int fwd = rtl ? -1 : 1;
      int v = (int)grey[(size_t)y * w + x] + cur[x + 1];
      if (v < 0) v = 0; else if (v > 255) v = 255;
      const bool ink = (v < 128);
      bw[(size_t)y * w + x] = ink ? INK : WHITE;
      const int e = v - (ink ? 0 : 255);
      const int e3 = (e * 3) / 16, e5 = (e * 5) / 16, e1 = (e * 1) / 16;
      const int e7 = e - e3 - e5 - e1;      // ahead: share + remainder
      cur[x + 1 + fwd] += e7;
      nxt[x + 1 - fwd] += e3;
      nxt[x + 1]       += e5;
      nxt[x + 1 + fwd] += e1;
    }
    memcpy(cur, nxt, (size_t)(w + 2) * sizeof(int16_t));
  }
  free(cur); free(nxt);
  img.w = w; img.h = h; img.bw = bw;
  return true;
}

// Synthesise an 8bpp greyscale "photograph" and dither it to level codes.
// kind 0 = a soft vignetted portrait-ish blob, 1 = a landscape with sky
// gradient and hills. Procedural so the example carries no image assets.
static bool makeImage(Img &img, int w, int h, int kind) {
  uint8_t *grey = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
  if (!grey) return false;

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float v;
      if (kind == 0) {
        // Centre-weighted subject with a falloff, plus a gentle diagonal
        // light and a little texture so the dither has something to chew on.
        const float cx = (x - w * 0.5f) / (w * 0.42f);
        const float cy = (y - h * 0.46f) / (h * 0.52f);
        const float r  = sqrtf(cx * cx + cy * cy);
        v = 232.0f - 150.0f * r * r;                    // bright subject, dark surround
        v += 26.0f * ((float)x / w - (float)y / h);     // diagonal light
        v += 7.0f * sinf(x * 0.11f) * cosf(y * 0.09f);  // texture
      } else {
        // Sky gradient, a hill silhouette, and a foreground band.
        const float t = (float)y / h;
        const int   horizon = (int)(h * 0.62f);
        const int   hill = horizon - (int)(28.0f * sinf(x * 0.014f)
                                        + 16.0f * sinf(x * 0.031f + 1.2f));
        if (y < hill)            v = 250.0f - 90.0f * t;          // sky
        else if (y < horizon)    v = 96.0f - 30.0f * sinf(x * 0.02f); // hills
        else                     v = 170.0f - 70.0f * (t - 0.62f) * 2.4f;
        v += 5.0f * sinf(x * 0.23f + y * 0.17f);
      }
      if (v < 0) v = 0; else if (v > 255) v = 255;
      grey[(size_t)y * w + x] = (uint8_t)v;
    }
  }

  // drawGray8 quantises against the measured curve for the LIVE level count
  // and error-diffuses. Destination is the level block itself.
  const bool ok = makePhoto(img, grey, w, h);
  heap_caps_free(grey);
  return ok;
}

// Blit a level block at (dx, dy) in canvas space, clipped to the content area.
static void blitImg(const Img &img, int dx, int dy) {
  if (!img.bw) return;
  if (dy >= VH || dy + img.h <= CHROME_H) return;   // fully off-screen: free
  uint8_t *fb = epd.getBuffer();
  const uint8_t *plane = img.bw;
  for (int sy = 0; sy < img.h; sy++) {
    const int y = dy + sy;
    if (y < CHROME_H || y >= VH) continue;              // never over the chrome
    const uint8_t *src = plane + (size_t)sy * img.w;
    uint8_t *dst = fb + (size_t)y * VW + dx;
    int n = img.w;
    int x0 = dx;
    if (x0 < 0) { src -= x0; dst -= x0; n += x0; x0 = 0; }
    if (x0 + n > VW) n = VW - x0;
    if (n > 0) memcpy(dst, src, n);
  }
}

// ---- text helpers ---------------------------------------------------------
// Adafruit's built-in font is 6x8 per unit size, so size s gives 6s x 8s.
static int charW(int s) { return 6 * s; }
static int lineH(int s) { return 8 * s + 3 * s; }

// Word-wrapped paragraph. Returns the y just past the last line. Draws
// nothing for lines outside the viewport, so an off-screen page costs almost
// nothing — the same trick a real browser plays.
static int drawWrapped(const char *text, int x, int y, int w, int size,
                       uint8_t colour) {
  epd.setTextSize(size);
  epd.setTextColor(colour);
  const int cw = charW(size), lh = lineH(size);
  const int maxChars = (w / cw) > 0 ? (w / cw) : 1;
  const char *p = text;
  while (*p) {
    // Find the longest run that fits, breaking on the last space.
    int len = 0, lastSpace = -1;
    while (p[len] && len < maxChars) {
      if (p[len] == ' ') lastSpace = len;
      len++;
    }
    int take = len;
    if (p[len] && lastSpace > 0) take = lastSpace;      // break at a word
    if (y + lh > CHROME_H && y < VH) {                  // visible?
      epd.setCursor(x, y);
      for (int i = 0; i < take; i++) epd.write(p[i]);
    }
    y += lh;
    p += take;
    while (*p == ' ') p++;
  }
  return y;
}

static int drawHeading(const char *text, int x, int y, int w, int size) {
  return drawWrapped(text, x, y, w, size, INK);
}

// ---- the page -------------------------------------------------------------
static const char *BODY1 =
    "The President used a forty minute address from the Palacio on Thursday "
    "to set out the reform programme his government will put before the "
    "assembly this autumn, framing it as a choice about who the next decade "
    "is built for.";

static const char *BODY2 =
    "Three measures carry most of the weight. Regional transport funding "
    "would roughly double over four years. The apprenticeship guarantee, "
    "trailed since the spring, is extended to school leavers in every "
    "province. A housing bill would let municipalities borrow against future "
    "receipts to build directly.";

static const char *BODY3 =
    "Opposition leaders called the arithmetic optimistic and said the housing "
    "provisions in particular depend on borrowing terms the treasury has not "
    "published. The finance ministry is expected to release costings before "
    "the assembly returns.";

static const char *QUOTE =
    "\"A future for every family is not a slogan. It is a budget line, and "
    "it has to survive contact with one.\"";

static const char *BODY4 =
    "Reaction outside the Palacio was warmer than the chamber is likely to "
    "be. Whether the programme survives the autumn depends less on Thursday's "
    "reception than on whether those costings hold up when the treasury "
    "finally shows its working.";

static const char *BULLETS[] = {
    "Transport funding doubled",
    "Apprenticeship guarantee widened",
    "Municipal housing borrowing",
    "Costings due before the autumn",
};

// Draw the whole page for the current scrollY. Everything is positioned in
// PAGE space and offset by scrollY; drawWrapped and blitImg clip.
// Returns the total page height (so the first call can measure it).
static int renderPage(int sy) {
  const int M = 18;                       // page margin
  const int W = VW - 2 * M;               // text column width
  int y = CHROME_H + 14 - sy;

  epd.fillScreen(WHITE);

  // Masthead
  y = drawHeading("THE NORTE TIMES", M, y, W, 3);
  y += 4;
  if (y - 6 > CHROME_H && y - 6 < VH) epd.fillRect(M, y - 6, W, 2, INK);
  y += 10;
  drawWrapped("POLITICS  |  THURSDAY", M, y, W, 2, MID);
  y += lineH(2) + 12;

  // Headline + byline
  y = drawHeading("President pledges reform in address to the nation", M, y, W, 4);
  y += 6;
  y = drawWrapped("By A. Weston, Political Editor   -   4 min read", M, y, W, 2, MID);
  y += 12;

  // Hero image
  if (heroImg.bw) {
    if (showPhotos) blitImg(heroImg, M, y);
    y += heroImg.h + 6;
    y = drawWrapped("The President addresses the nation, Thursday.", M, y, W, 2, MID);
    y += 14;
  }

  // Body
  y = drawWrapped(BODY1, M, y, W, 3, INK);  y += 12;
  y = drawWrapped(BODY2, M, y, W, 3, INK);  y += 16;

  // Pull quote in a tinted box
  {
    const int boxTop = y;
    const int inner = W - 24;
    // Measure by drawing off-screen-safe: compute height from wrap maths.
    epd.setTextSize(3);
    const int cw = charW(3), lh = lineH(3);
    const int maxChars = inner / cw;
    int lines = 0; const char *p = QUOTE;
    while (*p) {
      int len = 0, lastSpace = -1;
      while (p[len] && len < maxChars) { if (p[len]==' ') lastSpace = len; len++; }
      int take = len; if (p[len] && lastSpace > 0) take = lastSpace;
      lines++; p += take; while (*p==' ') p++;
    }
    const int boxH = lines * lh + 20;
    if (boxTop + boxH > CHROME_H && boxTop < VH) {
      epd.fillRect(M, boxTop, W, boxH, LIGHT);
      epd.fillRect(M, boxTop, 4, boxH, INK);            // quote rule
    }
    drawWrapped(QUOTE, M + 16, boxTop + 10, inner, 3, INK);
    y = boxTop + boxH + 16;
  }

  y = drawWrapped(BODY3, M, y, W, 3, INK);  y += 16;

  // Inline image, centred
  if (inlineImg.bw) {
    if (showPhotos) blitImg(inlineImg, M + (W - inlineImg.w) / 2, y);
    y += inlineImg.h + 6;
    y = drawWrapped("The northern provinces, where funding would land first.", M, y, W, 2, MID);
    y += 16;
  }

  // Bullets
  y = drawHeading("What was announced", M, y, W, 3);
  y += 8;
  for (unsigned i = 0; i < sizeof(BULLETS) / sizeof(BULLETS[0]); i++) {
    if (y + lineH(3) > CHROME_H && y < VH)
      epd.fillCircle(M + 6, y + 11, 4, INK);
    y = drawWrapped(BULLETS[i], M + 22, y, W - 22, 3, INK);
    y += 4;
  }
  y += 12;

  y = drawWrapped(BODY4, M, y, W, 3, INK);  y += 18;

  // Footer
  if (y > CHROME_H && y < VH) epd.fillRect(M, y, W, 1, MID);
  y += 8;
  y = drawWrapped("(c) 2026 - rendered with EPD_Painter", M, y, W, 2, MID);
  y += 20;

  return y + sy - CHROME_H;               // page height in page space
}

// Fixed browser chrome, drawn last so the page can never bleed into it.
static void renderChrome() {
  epd.fillRect(0, 0, VW, CHROME_H, LIGHT);
  epd.fillRect(0, CHROME_H - 1, VW, 1, MID);

  // Back chevron
  epd.fillTriangle(20, CHROME_H / 2, 32, CHROME_H / 2 - 9,
                   32, CHROME_H / 2 + 9, INK);

  // URL pill
  const int px = 44, pw = VW - 44 - 16, ph = 30, py = (CHROME_H - ph) / 2;
  epd.fillRoundRect(px, py, pw, ph, ph / 2, WHITE);
  epd.drawRoundRect(px, py, pw, ph, ph / 2, MID);
  epd.setTextSize(2);
  epd.setTextColor(INK);
  epd.setCursor(px + 12, py + 12);
  epd.print("nortetimes.example");

  // Scroll indicator down the right edge of the content area
  if (PAGE_H > VH - CHROME_H) {
    const int trackTop = CHROME_H + 4, trackH = VH - CHROME_H - 8;
    const int barH = trackH * (VH - CHROME_H) / PAGE_H;
    const int maxScroll = PAGE_H - (VH - CHROME_H);
    const int barY = trackTop + (trackH - barH) * scrollY / (maxScroll > 0 ? maxScroll : 1);
    epd.fillRect(VW - 5, barY, 3, barH < 12 ? 12 : barH, MID);
  }
}

static void renderAll() {
  renderPage(scrollY);
  renderChrome();
}

// ---- settle ---------------------------------------------------------------
// Quality never changes: this demo stays in 4-level NORMAL throughout, moving
// and still. (A FAST-while-scrolling tier was tried and removed — it is the
// classic e-reader trick, but it makes the moving page visibly rougher and the
// point here is what NORMAL alone feels like.)
//
// What DOES change is the photographs: hidden while moving, drawn when the
// page stops. The settle frame is the one you actually read.
static void settlePage() {
  showPhotos = true;                      // the photographs arrive here
  needSettle = false;
  renderAll();
  // paintLater(), NOT paint(): submit and return. A blocking settle held the
  // loop for the whole drive — around 200 ms — during which touch was not
  // read at all, so a drag started right after the page stopped was simply
  // dropped. Submitting asynchronously means the finger is picked up
  // immediately; if it moves, the next frame replaces this one and the
  // photographs come off again, which is exactly the right behaviour.
  epd.paintLater();
  shownScroll = scrollY;
}

// ---- touch ---------------------------------------------------------------
static void touchInit() {
  TwoWire *bus = epd.getConfig().i2c.wire;
  if (!bus) { Serial.println("[browser] no I2C bus configured"); return; }
  bus->setTimeOut(50);                    // a wedged bus must not hang the UI
  touch.begin(bus);
  for (uint8_t addr : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    bus->beginTransmission(addr);
    bus->write(0x81); bus->write(0x46);
    if (bus->endTransmission(false) != 0) continue;
    if (bus->requestFrom(addr, (uint8_t)4) != 4) continue;
    uint16_t xm = bus->read(); xm |= bus->read() << 8;
    uint16_t ym = bus->read(); ym |= bus->read() << 8;
    if (xm == 0 || ym == 0 || xm == 0xFFFF) break;
    tp_xmax = xm; tp_ymax = ym;
    tp_swap = (xm < ym);
    touch_ok = true;
    Serial.printf("[browser] touch 0x%02x, range %ux%u%s\n", addr, xm, ym,
                  tp_swap ? " (swapped)" : "");
    return;
  }
  Serial.println("[browser] no touch controller - serial keys only");
}

// Current touch point in CANVAS (portrait) coordinates, or false if up.
// The controller reports in panel orientation, so the same swap the tuner
// uses maps it into the rotated canvas.
static bool touchPoint(int &px, int &py) {
  if (!touch_ok) return false;
  touch.read();
  if (!touch.isTouched) return false;
  const uint16_t rx = touch.x, ry = touch.y;
  const int PW = epd.getConfig().width, PH = epd.getConfig().height;
  int lx, ly;                             // landscape panel coords
  if (tp_swap) {
    lx = (int)ry * PW / tp_ymax;
    ly = (int)(tp_xmax - rx) * PH / tp_xmax;
  } else {
    lx = (int)rx * PW / tp_xmax;
    ly = (int)ry * PH / tp_ymax;
  }
  // Panel is landscape (PW x PH); canvas is rotated CW to (PH x PW).
  px = PH - 1 - ly;
  py = lx;
  if (px < 0) px = 0; if (px >= VW) px = VW - 1;
  if (py < 0) py = 0; if (py >= VH) py = VH - 1;
  return true;
}

static int maxScroll() {
  const int m = PAGE_H - (VH - CHROME_H);
  return m > 0 ? m : 0;
}
static void clampScroll() {
  if (scrollY < 0) scrollY = 0;
  const int m = maxScroll();
  if (scrollY > m) scrollY = m;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // Survive a flash-triggered reset. On the M5PaperS3 the system rail is
  // latched by pin_syspwr, and begin()'s default is to release it on a reset
  // that was not a real power-on — so the board pulses the latch and switches
  // itself OFF moments after an upload ("[BOOT] pin_syspwr pulsed"), which
  // looks exactly like a crash. A demo you reflash repeatedly wants the rail
  // held; note the flip side is that it will not power down on its own, so it
  // sits drawing from the battery until you press the button.
  epd.setAutoShutdown(false);

  if (!epd.begin()) {
    Serial.println("[browser] begin() failed - check board/preset");
    for (;;) delay(1000);
  }

  // 4-level NORMAL is the whole point: enough greys for text and dithered
  // photos, cheap enough to repaint a whole page while a thumb is moving.
  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);

  // Direct grey-to-grey trains, OFF by default here so the default build
  // shows the plain two-step behaviour. Press 'd' to toggle and compare.
  if (wantDirect && !epd.driver().setDirectTransitions(true)) {
    Serial.println("[browser] direct transitions refused (RAM)");
    wantDirect = false;
  }

  epd.driver()._config.pass_gap_us_normal = PASS_GAP_US;
  epd.driver().setPaintProfile(true);   // one line per drive cycle
  epd.driver().setIdleTimeout(120);       // keep the rails up between scrolls

  VW = epd.width();
  VH = epd.height();
  Serial.printf("[browser] viewport %dx%d, %d greys\n",
                VW, VH, epd.driver().greyLevels());

  if (!makePhoto(heroImg, HERO_GREY, HERO_W, HERO_H))
    Serial.println("[browser] no PSRAM for the hero photo");
  if (!makeImage(inlineImg, 240, 170, 1))
    Serial.println("[browser] no PSRAM for the inline image");

  Serial.printf("[browser] one bit; halftones: hero %s, inline %s; "
                "inter-pass gap %d us\n",
                heroImg.bw ? "ok" : "MISSING", inlineImg.bw ? "ok" : "MISSING",
                PASS_GAP_US);

  touchInit();

  epd.clear();
  while (!epd.driver().paintIdle()) delay(10);

  // First render measures the page, second uses the real height so the
  // scrollbar is right on the very first frame.
  PAGE_H = renderPage(0);
  renderAll();
  epd.paint();
  while (!epd.driver().paintIdle()) delay(10);

  paintsAtStart = epd.driver().paintsCompleted();
  Serial.printf("[browser] page %d px, max scroll %d\n", PAGE_H, maxScroll());
  Serial.println("[browser] 4-grey NORMAL throughout; photographs hidden "
                 "while moving, drawn on settle");
  Serial.printf("[browser] direct grey-to-grey: %s ('d' toggles)\n",
                wantDirect ? "ON" : "off");
  Serial.println("[browser] drag to scroll; serial: j/k = page down/up, "
                 "t = top, r = redraw, d = direct");
}

void loop() {
  static bool dragging = false;
  static int  grabY = 0, grabScroll = 0, lastY = 0;
  static uint32_t lastMoveMs = 0;
  static float velocity = 0;              // px per frame, for the flick
  static bool  movedFar = false;

  int tx, ty;
  const bool down = touchPoint(tx, ty);

  if (down && !dragging) {
    dragging = true; movedFar = false;
    grabY = lastY = ty;
    grabScroll = scrollY;
    velocity = 0;
    lastMoveMs = millis();
  } else if (down && dragging) {
    // Content follows the thumb: dragging DOWN reveals earlier content.
    const int want = grabScroll - (ty - grabY);
    if (abs(ty - grabY) > 6) movedFar = true;
    const uint32_t now = millis();
    const int dy = ty - lastY;
    if (dy != 0 && now != lastMoveMs) {
      // Low-passed so one jittery sample cannot launch a flick.
      velocity = 0.7f * velocity + 0.3f * (float)(-dy);
      lastMoveMs = now;
    }
    lastY = ty;
    scrollY = want;
    clampScroll();
    // Tracking the thumb costs nothing; RENDERING is what costs 40 ms, so it
    // happens only when the panel can actually take a frame. Rendering
    // flat-out and letting paintLater() drop the surplus measured 42 frames
    // submitted for 14 shown — a second of wasted CPU per drag, and every
    // displayed frame older than it needed to be, because it was rendered
    // before the panel was free rather than at the moment it came free.
  } else if (!down && dragging) {
    dragging = false;
    if (!movedFar) {
      // A tap. On the chrome, jump to the top.
      if (ty < CHROME_H && scrollY != 0) scrollY = 0;   // rendered below
      velocity = 0;
    }
  }

  // Flick momentum, advanced one step per DISPLAYED frame rather than per
  // loop pass, so the glide is paced by the glass instead of by the CPU.
  if (!dragging && fabsf(velocity) >= MIN_GLIDE_PX && epd.driver().paintIdle()) {
    scrollY += (int)velocity;
    clampScroll();
    velocity *= 0.82f;
    // Stop outright rather than trail off: hitting an end, or decaying below
    // one frame's worth of visible movement, both end the glide here.
    if (scrollY == 0 || scrollY == maxScroll()) velocity = 0;
    if (fabsf(velocity) < MIN_GLIDE_PX) velocity = 0;
  }

  // ONE render per panel frame, at whatever the scroll position is by the
  // time the panel is free. This is the whole latency story: the newest
  // possible content, rendered once, submitted immediately.
  if (scrollY != shownScroll && epd.driver().paintIdle()) {
    const uint32_t r0 = millis();
    renderAll();
    renderMs = millis() - r0;
    epd.paintLater();
    framesSubmitted++;
    shownScroll = scrollY;
    needSettle = true;
  }

  // Stopped moving: put the readable frame up, in NORMAL.
  if (!dragging && fabsf(velocity) < MIN_GLIDE_PX && needSettle) {
    const uint32_t s0 = millis();
    settlePage();
    Serial.printf("[browser] scroll %d/%d | moving frames %lu, panel %lu, "
                  "render %lu ms | settle submit %lu ms\n",
                  scrollY, maxScroll(), (unsigned long)framesSubmitted,
                  (unsigned long)(epd.driver().paintsCompleted() - paintsAtStart),
                  (unsigned long)renderMs, (unsigned long)(millis() - s0));
    framesSubmitted = 0;
    paintsAtStart = epd.driver().paintsCompleted();
  }

  // Serial conveniences, useful when the panel is face down on a scanner.
  if (Serial.available()) {
    const int c = Serial.read();
    const int page = (VH - CHROME_H) * 3 / 4;
    bool act = true;
    switch (c) {
      case 'j': scrollY += page; break;
      case 'k': scrollY -= page; break;
      case 't': scrollY = 0;     break;
      case 'r':                  break;
      case 'd':
        wantDirect = !wantDirect;
        if (wantDirect && !epd.driver().setDirectTransitions(true)) {
          wantDirect = false;
          Serial.println("[browser] direct refused (RAM)");
        } else if (!wantDirect) {
          epd.driver().setDirectTransitions(false);
        }
        Serial.printf("[browser] direct grey-to-grey: %s\n",
                      wantDirect ? "ON (one paint, longer frame)"
                                 : "off (two-step through white)");
        break;
      default:  act = false;     break;
    }
    if (act) {
      clampScroll();
      showPhotos = true;                  // a keyboard jump is not motion
      needSettle = false;
      renderAll();
      epd.paintLater();                   // non-blocking, same as the settle
      shownScroll = scrollY;              // else the loop repaints it again
      Serial.printf("[browser] scroll %d / %d\n", scrollY, maxScroll());
    }
  }

  delay(4);
}

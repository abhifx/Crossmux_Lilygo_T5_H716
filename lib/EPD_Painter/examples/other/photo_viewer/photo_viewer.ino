// ============================================================================
// EPD_Painter PHOTO VIEWER — JPEGs from an SD card, in 16 native greys.
//
// Put photographs in a /photos folder on an SD card and this shows them,
// one per tap. They are rendered in the panel's 16 grey levels using the
// board's own scanner-tuned trains and, crucially, its MEASURED level
// curve — see "why the curve matters" below.
//
//   Left half of the screen  ... previous photo
//   Right half               ... next photo
//   Serial: n / p / space    ... next / previous / next
//   Serial: i                ... what the current photo is doing
//
// WHY THE CURVE MATTERS
// A panel's 16 greys are NOT evenly spaced. On the LilyGo T5 S3 the
// measured curve runs
//   255 253 231 219 205 190 172 135 126 115 61 55 47 35 21 0
// so levels 0 and 1 differ by 2 units while levels 9 and 10 differ by 54.
// A dither that assumes even steps therefore pushes error into levels that
// cannot express it, and the result bands badly. Quantising against the
// measured curve instead is what makes photographs look right.
//
// The curve is produced by the tuneup example and stored alongside the
// tuned trains; this sketch just loads it. Without it (an untuned board)
// the viewer falls back to assuming even levels, which still works but
// will band on smooth gradients.
//
// Requires: Adafruit GFX, JPEGDEC, gt911-arduino (touch).
// ============================================================================

// Choose your board (or leave all commented for auto-probe).
//#define EPD_PAINTER_PRESET_M5PAPER_S3
//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#define EPD_PAINTER_HAVE_JPEGDEC 1
#include "EPD_Painter_Image.h"
#include <esp_heap_caps.h>
#include "EPD_Painter_Adafruit.h"
#include "EPD_Painter_tuned.h"
#include <gt911_lite.h>
#include <Preferences.h>

// Declared up here rather than beside the drawing code: the Arduino builder
// auto-generates function prototypes and inserts them ABOVE the sketch body,
// so any type used in a signature must already exist by then.
struct Btn { int x, y, w, h; };

static EPD_PainterAdafruit epd(EPD_PAINTER_PRESET);

static const char *PHOTO_DIR = "/photos";
// The tuneup example keeps one blob per quality, plus the single-blob
// path older versions of it wrote. Install whatever this board has: the
// sets are independent, and install() refuses anything that is not a
// valid blob for this board, so trying all four is safe.
static const char *TUNED_PATHS[] = {
  "/epd_g16_tuned.bin",      // legacy single blob = the NORMAL set
  "/epd_tuned_fast.bin",
  "/epd_tuned_normal.bin",
  "/epd_tuned_high.bin",
};

// Decode no larger than this before scaling down — keeps the intermediate
// image inside PSRAM whatever the camera produced.
static const int MAX_DECODE_W = 1920;

static int  panelW = 0, panelH = 0;
static bool useSdmmc = false;

// ============================================================================
// SD pins per board (the map proven in the bad_apple example)
// ============================================================================
struct SdPins {
  int  cs, sclk, mosi, miso;
  bool sdmmc;          // true: native 1-bit SD protocol, false: SPI mode
  int  other_cs;       // another device on a shared bus to deselect (-1: none)
};

static bool sdPinsForBoard(SdPins &p) {
  const auto &cfg = epd.getConfig();
  if (cfg.i2c.sda == 39 && cfg.i2c.scl == 40) {        // LilyGo T5 S3 GPS / Pro
    p = { 12, 14, 13, 21, false, 46 };                 // SPI; deselect LoRa (46)
    return true;
  }
  if (cfg.i2c.sda == 41 && cfg.i2c.scl == 42) {        // M5PaperS3
    p = { 47, 39, 38, 40, true, -1 };
    return true;
  }
  if (cfg.i2c.sda == 18 && cfg.i2c.scl == 17) {        // LilyGo EPD47 H716
    p = { 42, 11, 15, 16, false, -1 };
    return true;
  }
  if (cfg.i2c.sda == 6 && cfg.i2c.scl == 5) {          // LilyGo T5 S3 H752
    p = { 42, 11, 15, 16, false, -1 };
    return true;
  }
  return false;
}

static File openPath(const char *path) {
  return useSdmmc ? SD_MMC.open(path, FILE_READ) : SD.open(path, FILE_READ);
}

// ============================================================================
// Touch — same GT911 pattern as the other examples.
//
// NOTE the poll interval below: the driver only trusts a press it sees on
// two frames within 40 ms, deliberately, because EPD refresh transients
// produce isolated frames. Poll any slower and every real tap is thrown
// away as a phantom.
// ============================================================================
static GT911_Lite touch;
static uint16_t tpXmax = 960, tpYmax = 540;
static bool tpSwap = false, touchOk = false, touchWasDown = false;

static void touchInit() {
  TwoWire *bus = epd.getConfig().i2c.wire;
  if (!bus) return;
  bus->setTimeOut(50);
  touch.begin(bus);
  for (uint8_t addr : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    bus->beginTransmission(addr);
    bus->write(0x81); bus->write(0x46);
    if (bus->endTransmission(false) != 0) continue;
    if (bus->requestFrom(addr, (uint8_t)4) != 4) continue;
    uint16_t xm = bus->read(); xm |= bus->read() << 8;
    uint16_t ym = bus->read(); ym |= bus->read() << 8;
    if (xm == 0 || ym == 0 || xm == 0xFFFF) break;
    tpXmax = xm; tpYmax = ym;
    tpSwap = (xm < ym);
    touchOk = true;
    Serial.printf("[viewer] touch 0x%02x, range %ux%u%s\n", addr, xm, ym,
                  tpSwap ? " (swapped)" : "");
    return;
  }
  Serial.println("[viewer] no touch controller - serial control only");
}

static bool touchTapped(int &px, int &py) {
  if (!touchOk) return false;
  touch.read();
  const bool down = touch.isTouched;
  const bool tap = down && !touchWasDown;
  touchWasDown = down;
  if (!tap) return false;
  const uint16_t rx = touch.x, ry = touch.y;
  if (tpSwap) {
    px = (int)ry * panelW / tpYmax;
    py = (int)(tpXmax - rx) * panelH / tpXmax;
  } else {
    px = (int)rx * panelW / tpXmax;
    py = (int)ry * panelH / tpYmax;
  }
  return true;
}

// ============================================================================
// Photo rendering
//
// The library does all of it now, and it STREAMS: the JPEG is dithered row by
// row as it decodes, so the decoded photo never exists in memory. This sketch
// used to allocate the full greyscale image - ~1.9 MB for a phone photo - and
// carry its own Floyd-Steinberg loop. Both are gone.
//
// Quantising happens against the panel's MEASURED level curve, which reaches
// the driver through EPD_PainterTuned::install() pointing Config::level_lum at
// the tuned blob. On an untuned board that is null and the library falls back
// to an even ramp: still works, but smooth gradients will band.
//// ============================================================================
static uint8_t  *fileBuf = nullptr;    // whole JPEG, read from the card

// The curve reaches the driver through EPD_PainterTuned::install(), which
// points Config::level_lum at the tuned blob's measurements. On an untuned
// board level_lum is null and the library falls back to an even ramp: still
// works, but smooth gradients will band.
// ============================================================================

// ============================================================================
// Photo list
// ============================================================================
// How long each photo stays up before the next one loads. This is DWELL time
// - the clock starts when a photo finishes painting - so the interval means
// the same thing whatever the image costs to render.
// ---- settings, persisted in NVS -------------------------------------------
// Dwell choices rather than a free number: on a touch panel with no keyboard,
// cycling a short list is the whole interaction, and these are the values
// anyone actually wants.
static const uint32_t DWELL_CHOICES[] = { 5000, 8000, 15000, 30000, 60000, 300000 };
static const int      N_DWELL = sizeof(DWELL_CHOICES) / sizeof(DWELL_CHOICES[0]);

// ---- frontlight (LilyGo) ---------------------------------------------------
// The T5 S3 GPS carries a frontlight on GPIO 11, driven by LEDC PWM. Other
// boards in this family do not, so the setting only appears where the hardware
// is: the panel presets differ by I2C pins, which is enough to tell them
// apart at runtime under auto-probe, where a compile-time #ifdef cannot.
#define VIEWER_BL_PIN   11
#define VIEWER_BL_FREQ  5000
#define VIEWER_BL_BITS  8

static bool hasBacklight = false;      // set in setup() from the detected board
static const uint8_t BL_CHOICES[] = { 0, 64, 128, 192, 255 };
static const int     N_BL = sizeof(BL_CHOICES) / sizeof(BL_CHOICES[0]);
static int           blIdx = 0;        // -> off

static void applyBacklight() {
  if (!hasBacklight) return;
  ledcWrite(VIEWER_BL_PIN, BL_CHOICES[blIdx]);
}

static Preferences prefs;
static int      dwellIdx    = 1;       // -> 8 s
static int      flashClears = 4;       // rail-to-rail clears between photos
static bool     slideshow   = true;
static uint32_t lastShownMs = 0;
static bool     inSettings  = false;

static uint32_t dwellMs() { return DWELL_CHOICES[dwellIdx]; }

static void loadSettings() {
  prefs.begin("viewer", true);
  slideshow   = prefs.getBool("slide", true);
  dwellIdx    = prefs.getInt("dwell", 1);
  flashClears = prefs.getInt("flash", 4);
  blIdx       = prefs.getInt("bl", 0);
  prefs.end();
  if (blIdx < 0 || blIdx >= N_BL) blIdx = 0;
  if (dwellIdx < 0 || dwellIdx >= N_DWELL) dwellIdx = 1;
  if (flashClears < 0 || flashClears > 8)  flashClears = 4;
}

static void saveSettings() {
  prefs.begin("viewer", false);
  prefs.putBool("slide", slideshow);
  prefs.putInt("dwell", dwellIdx);
  prefs.putInt("flash", flashClears);
  prefs.putInt("bl", blIdx);
  prefs.end();
}



static const int MAX_PHOTOS = 200;
static String photos[MAX_PHOTOS];
static int    nPhotos = 0, current = 0;

static bool isJpeg(const char *name) {
  // Skip dotfiles, and AppleDouble stubs in particular. macOS writes a "._"
  // sidecar beside every real file on a FAT/exFAT card: same .jpg extension,
  // no image inside. Matching on the extension alone means half the entries
  // on a Mac-copied card are metadata that cannot decode.
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (base[0] == '.') return false;

  const char *dot = strrchr(base, '.');
  if (!dot) return false;
  return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg");
}

static void scanPhotos() {
  nPhotos = 0;
  File dir = openPath(PHOTO_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[viewer] no %s folder on the card\n", PHOTO_DIR);
    // Say what IS there. "No photos" with nothing else to go on sends people
    // hunting for a card fault when it is usually a name or a case mismatch.
    File root = openPath("/");
    if (root && root.isDirectory()) {
      Serial.println("[viewer] card root contains:");
      for (File e = root.openNextFile(); e; e = root.openNextFile())
        Serial.printf("[viewer]   %s%s\n", e.name(), e.isDirectory() ? "/" : "");
    } else {
      Serial.println("[viewer] cannot open the card root either");
    }
    return;
  }
  for (File f = dir.openNextFile(); f && nPhotos < MAX_PHOTOS; f = dir.openNextFile()) {
    if (!f.isDirectory() && isJpeg(f.name())) {
      String p = String(PHOTO_DIR) + "/" + f.name();
      photos[nPhotos++] = p;
    }
    f.close();
  }
  dir.close();
  // simple alphabetical order so the sequence is predictable
  for (int i = 0; i < nPhotos; i++)
    for (int j = i + 1; j < nPhotos; j++)
      if (photos[j] < photos[i]) { String t = photos[i]; photos[i] = photos[j]; photos[j] = t; }
  Serial.printf("[viewer] %d photo(s) in %s\n", nPhotos, PHOTO_DIR);
}

// ============================================================================
static void showMessage(const char *l1, const char *l2) {
  epd.fillScreen(0);
  epd.setTextColor(15);
  epd.setTextSize(3);
  epd.setCursor(40, panelH / 2 - 40);
  epd.print(l1);
  if (l2) {
    epd.setTextSize(2);
    epd.setCursor(40, panelH / 2 + 10);
    epd.print(l2);
  }
  epd.paint();
  while (!epd.driver().paintIdle()) delay(10);
}

// Rail-to-rail flash to clear ghosting between photos. Same approach as the
// tuner's activation flash; the count is a trade between how thoroughly the
// previous image is erased and how long the panel spends flashing.

// ============================================================================
// On-screen controls
//
// E-paper has no hover and a slow redraw, so the controls are drawn INTO the
// photo rather than composited over it - they cost nothing extra to display
// and vanish the moment the slideshow takes over.
//
// In slideshow mode the picture is left completely clean and a tap anywhere
// opens settings. In manual mode there is nowhere to discover the controls
// from, so prev/next/settings are drawn along the bottom.
// ============================================================================
static const int BAR_H = 64;

static Btn btnPrev, btnNext, btnSettings;

static bool hit(const Btn &b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void drawButton(const Btn &b, const char *label, int textSize = 3) {
  epd.fillRect(b.x, b.y, b.w, b.h, 0);            // white plate
  epd.drawRect(b.x, b.y, b.w, b.h, 15);
  epd.setTextColor(15);
  epd.setTextSize(textSize);
  const int cw = 6 * textSize, ch = 8 * textSize;
  epd.setCursor(b.x + (b.w - (int)strlen(label) * cw) / 2,
                b.y + (b.h - ch) / 2);
  epd.print(label);
}

// Lay the bar out for the current panel size, then draw it into the canvas.
static void drawNavBar() {
  const int y = panelH - BAR_H, third = panelW / 3;
  btnPrev     = { 0,           y, third,            BAR_H };
  btnNext     = { third * 2,   y, panelW - third*2, BAR_H };
  btnSettings = { third,       y, third,            BAR_H };
  drawButton(btnPrev,     "<");
  drawButton(btnSettings, "SETTINGS", 2);
  drawButton(btnNext,     ">");
}

// ---- settings page ---------------------------------------------------------
static Btn rowSlide, rowDwell, rowFlash, rowLight, rowDone;

static void drawSettings() {
  epd.fillScreen(0);
  epd.setTextColor(15);
  epd.setTextSize(4);
  epd.setCursor(30, 24);
  epd.print("SETTINGS");

  const int x = 30, w = panelW - 60, h = 78;
  int y = 96;
  char buf[64];

  auto row = [&](Btn &b, const char *label, const char *value) {
    b = { x, y, w, h };
    epd.drawRect(x, y, w, h, 15);
    epd.setTextSize(3);
    epd.setCursor(x + 16, y + (h - 24) / 2);
    epd.print(label);
    epd.setCursor(x + w - 16 - (int)strlen(value) * 18, y + (h - 24) / 2);
    epd.print(value);
    y += h + 14;
  };

  row(rowSlide, "Slideshow", slideshow ? "ON" : "OFF");

  if (dwellMs() >= 60000) snprintf(buf, sizeof(buf), "%lu min", (unsigned long)(dwellMs() / 60000));
  else                    snprintf(buf, sizeof(buf), "%lu sec", (unsigned long)(dwellMs() / 1000));
  row(rowDwell, "Time per photo", buf);

  snprintf(buf, sizeof(buf), "%d", flashClears);
  row(rowFlash, "Flash clears", buf);

  if (hasBacklight) {
    if (!blIdx) snprintf(buf, sizeof(buf), "OFF");
    else        snprintf(buf, sizeof(buf), "%d%%", BL_CHOICES[blIdx] * 100 / 255);
    row(rowLight, "Frontlight", buf);
  }

  epd.setTextSize(2);
  epd.setCursor(x + 4, y + 6);
  epd.print("Tap a row to change it. More flashes erase more");
  epd.setCursor(x + 4, y + 26);
  epd.print("ghosting but take longer between pictures.");

  rowDone = { panelW / 2 - 110, panelH - BAR_H - 16, 220, BAR_H };
  drawButton(rowDone, "DONE");

  epd.paint();
  while (!epd.driver().paintIdle()) delay(10);
}

static void deepClear() {
  for (int i = 0; i < flashClears; i++) {
    epd.clear();
    while (!epd.driver().paintIdle()) delay(2);
  }
}

static bool showPhoto(int idx) {
  if (idx < 0 || idx >= nPhotos) return false;
  const uint32_t t0 = millis();
  Serial.printf("[viewer] %d/%d  %s\n", idx + 1, nPhotos, photos[idx].c_str());

  File f = openPath(photos[idx].c_str());
  if (!f) { Serial.println("[viewer]   open failed"); return false; }
  const size_t len = f.size();
  if (fileBuf) heap_caps_free(fileBuf);
  fileBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!fileBuf) { f.close(); Serial.println("[viewer]   no PSRAM for file"); return false; }
  f.read(fileBuf, len);
  f.close();

  // One call: decodes, shrinks, fits, centres and dithers against the panel's
  // measured level curve - STREAMING, so the decoded photo never exists in
  // memory. Only a ~26 KB strip and the error rows, against the ~1.9 MB this
  // sketch used to allocate for a phone photo.
  //
  // The decode scale is chosen for us: the largest built-in shrink that still
  // leaves the image at least panel-sized, so a 12 MP photo is not decoded in
  // full only to be thrown away.
  // 1) STREAM the photo into the framebuffer (PSRAM). Nothing reaches the
  //    panel yet, so the screen keeps the previous picture while this runs -
  //    several seconds for a large JPEG, and a white screen for that long
  //    would look like a fault.
  if (!EPD_PainterImage::drawJpegFit(epd, fileBuf, len)) {
    // Say so on the PANEL as well as the wire. A silent failure leaves
    // whatever was on screen before, which reads as "the viewer is broken"
    // rather than "that one file would not decode".
    Serial.println("[viewer]   decode failed");
    showMessage("CANNOT SHOW THIS ONE", photos[idx].c_str());
    return false;
  }
  // In manual mode the controls go INTO the picture - drawn after the photo,
  // before the paint, so they cost nothing extra and disappear cleanly when
  // the slideshow is running.
  if (!slideshow) drawNavBar();

  // 2) DEEP CLEAR, the way the tuner does before a measurement. Photographs
  //    put every level on the glass at once, and going straight from one to
  //    the next leaves the previous image faintly readable in the flats.
  //    Rail-to-rail flashing agitates the ink and lands every pixel in the
  //    same physical state, so the new paint starts from a known page.
  //
  //    Hard clears specifically, rather than painting black then white: a
  //    hard clear is DC balanced by construction, so flashing between every
  //    photo adds no net charge no matter how long the viewer runs. Back to
  //    back with no settle - a pause just lets the particles relax again.
  deepClear();

  // 3) PAINT. The driver sees a known-white screen and the framebuffer we
  //    prepared in (1), so this is a full, clean redraw.
  epd.paint();
  while (!epd.driver().paintIdle()) delay(10);
  // Start the dwell clock only now the picture is actually on the glass.
  lastShownMs = millis();
  Serial.printf("[viewer]   drawn in %lu ms\n", millis() - t0);
  return true;
}

static void step(int dir) {
  if (!nPhotos) return;
  int tries = nPhotos;
  while (tries--) {
    current = (current + dir + nPhotos) % nPhotos;
    if (showPhoto(current)) return;      // skip anything that will not decode
  }
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Rig-friendly: a reset should not power the board off mid-slideshow.
  epd.setAutoShutdown(false);

  if (!epd.begin()) { Serial.println("[viewer] display begin() failed"); for (;;) delay(1000); }
  panelW = epd.width();
  panelH = epd.height();

  // The panel still holds the previous image after a reset while the
  // driver assumes it is blank, so start from a real clear.
  epd.clear();
  while (!epd.driver().paintIdle()) delay(10);

  // Tuned trains + the measured level curve, if this board has been
  // through the tuneup example. Safe to call on an untuned board.
  bool tuned = false;
  if (LittleFS.begin(true)) {
    for (const char *path : TUNED_PATHS) {
      File f = LittleFS.open(path, "r");
      if (!f) continue;
      uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
      const size_t n = f.read(buf, sizeof(buf));
      f.close();
      if (EPD_PainterTuned::install(epd.driver(), buf, n)) tuned = true;
    }
  }
  const uint8_t *curve = EPD_PainterTuned::levelLuminance();
  Serial.printf("[viewer] trains: %s, level curve: %s\n",
                tuned ? "flash-tuned" : "preset",
                curve ? "measured" : "assumed even");
  if (curve) {
    Serial.print("[viewer] curve:");
    for (int i = 0; i < 16; i++) Serial.printf(" %d", curve[i]);
    Serial.println();
  }

  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
  if (!epd.driver().setGreyLevels(16)) {
    Serial.println("[viewer] setGreyLevels(16) refused");
    for (;;) delay(1000);
  }
  touchInit();

  SdPins sd;
  if (!sdPinsForBoard(sd)) {
    showMessage("NO SD PIN MAP", "This board is not in sdPinsForBoard()");
    for (;;) delay(1000);
  }
  pinMode(sd.cs, OUTPUT);
  digitalWrite(sd.cs, HIGH);
  if (sd.other_cs >= 0) { pinMode(sd.other_cs, OUTPUT); digitalWrite(sd.other_cs, HIGH); }

  useSdmmc = sd.sdmmc;
  bool sdOk;
  if (sd.sdmmc) {
    SD_MMC.setPins(sd.sclk, sd.mosi, sd.miso);
    sdOk = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  } else {
    SPI.begin(sd.sclk, sd.miso, sd.mosi, sd.cs);
    sdOk = SD.begin(sd.cs, SPI, 25000000);
  }
  if (!sdOk) {
    showMessage("NO SD CARD", "Insert a card with a /photos folder, then reset");
    for (;;) delay(1000);
  }

  loadSettings();

  // Only the LilyGo T5 S3 GPS in this family has the frontlight. Identify it
  // by the I2C pins the preset actually resolved to, which works under
  // auto-probe where a compile-time check cannot.
  {
    const auto &i2c = epd.driver()._config.i2c;
    hasBacklight = (i2c.sda == 39 && i2c.scl == 40);
    if (hasBacklight) {
      ledcAttach(VIEWER_BL_PIN, VIEWER_BL_FREQ, VIEWER_BL_BITS);
      applyBacklight();
      Serial.printf("[viewer] frontlight on GPIO %d, %s\n", VIEWER_BL_PIN,
                    blIdx ? "on" : "off");
    } else {
      Serial.println("[viewer] no frontlight on this board");
    }
  }

  scanPhotos();
  if (!nPhotos) {
    showMessage("NO PHOTOS", "Put .jpg files in /photos on the SD card");
    for (;;) delay(1000);
  }
  Serial.println("[viewer] tap right = next, left = previous (serial: n p space i)");
  showPhoto(current);
}

void loop() {
  int px, py;

  // ---- settings page owns the screen while it is open ----
  if (inSettings) {
    if (touchTapped(px, py)) {
      bool dirty = true;
      if      (hit(rowSlide, px, py)) slideshow = !slideshow;
      else if (hit(rowDwell, px, py)) dwellIdx  = (dwellIdx + 1) % N_DWELL;
      else if (hit(rowFlash, px, py)) flashClears = (flashClears + 1) % 9;
      else if (hasBacklight && hit(rowLight, px, py)) {
        blIdx = (blIdx + 1) % N_BL;
        applyBacklight();            // immediate, so the choice is visible
      }
      else if (hit(rowDone,  px, py)) {
        saveSettings();
        inSettings = false;
        lastShownMs = millis();
        showPhoto(current);          // back to the picture, controls as set
        return;
      } else dirty = false;
      if (dirty) drawSettings();     // redraw with the new value
    }
    delay(8);
    return;
  }

  // ---- normal viewing ----
  if (touchTapped(px, py)) {
    if (slideshow) {
      // Nothing is drawn over a running slideshow, so there is nowhere to aim
      // - any tap opens settings, which is where the stop button lives.
      inSettings = true;
      drawSettings();
    } else if (hit(btnSettings, px, py)) {
      inSettings = true;
      drawSettings();
    } else if (hit(btnPrev, px, py)) step(-1);
    else if (hit(btnNext, px, py))   step(+1);
    else {
      // Above the bar: keep the old left/right halves, so tapping the picture
      // still works for anyone used to it.
      step(px >= panelW / 2 ? +1 : -1);
    }
    return;
  }

  if (Serial.available()) {
    const int c = Serial.read();
    if (c == 'n' || c == ' ') step(+1);
    else if (c == 'p')        step(-1);
    else if (c == 'c')      { inSettings = true; drawSettings(); }
    else if (c == 's') {
      slideshow = !slideshow;
      saveSettings();
      lastShownMs = millis();
      Serial.printf("[viewer] slideshow %s\n", slideshow ? "on" : "off");
      showPhoto(current);            // redraw so the bar appears/disappears
    } else if (c == 'i')
      Serial.printf("[viewer] %d/%d %s  (slideshow %s, %lus, %d clears)\n",
                    current + 1, nPhotos, nPhotos ? photos[current].c_str() : "-",
                    slideshow ? "on" : "off",
                    (unsigned long)(dwellMs() / 1000), flashClears);
    return;
  }

  // Auto-advance. Timed from when the last photo FINISHED painting, not from
  // when it started: a large JPEG takes several seconds to decode, clear and
  // paint, and measuring from the start would make the dwell shrink with image
  // size - big photos would flick past fastest, which is backwards.
  if (slideshow && nPhotos > 1 && millis() - lastShownMs >= dwellMs())
    step(+1);

  delay(8);      // fast enough for the touch driver's confirmation window
}

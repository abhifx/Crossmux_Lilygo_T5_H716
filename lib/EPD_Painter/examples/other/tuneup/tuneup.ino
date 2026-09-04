// ============================================================================
// EPD_Painter TUNEUP — the self-tuning rig.
//
// Any user with a network scanner can lay their board face-down on the
// glass and let it tune its own waveform trains — the same closed optical
// loop used to tune the shipped presets, with the BOARD driving the
// scanner instead of a PC.
//
//   1. WiFi   — credentials live in this file (see WIFI_SSID below). If
//               they're still blank the panel tells you to set them and
//               reflash.
//   2. Scanner — discovers eSCL/AirScan scanners via mDNS (_uscan._tcp)
//               and lists them on the panel; tap one to pick it (the
//               choice is remembered).
//   3. Demo scan — requests a grayscale eSCL scan and shows it on the
//               panel in 16 native greys.
//   4. SELF-TUNE — put the board face-down on the glass and it tunes its
//               own 16-grey trains against dithered optical references,
//               saves them to flash (LittleFS), and loads them on every
//               boot from then on. See tuner.h for the method.
//
// eSCL ("AirScan") is the driverless scanning protocol spoken by nearly
// every network scanner made since ~2015, and by sane-airscan bridges for
// older ones. No drivers, just HTTP + XML — well within an ESP32.
//
// Touch: GT911 via https://github.com/tonywestonuk/gt911-arduino
// (same pattern as the maze_3d example). While the board is face-down the
// touchscreen is against the glass, so tuning is driven over serial or by
// the TUNE button's countdown.
//
// Serial commands (115200):
//   Tuning     T self-tune (pick quality) · 4 4-grey tune (then f/n/N) ·
//              d directs-only at NORMAL · E [gens] evo tune HIGH · q abort
//   Results    e export all tables as source · L blob status · X erase blobs ·
//              V results-screen preview · C paint match card · f/g/G splash
//              at FAST/NORMAL/HIGH
//   Probes     D dose ladder · S staircase check · c store staircase curve ·
//              p store photo-context curve · B stability sweep ·
//              U uniformity probe · K/k/l compensation sweep (period/row/LE) ·
//              J timing benches · Q paint quadrant mark
//   Knobs      P <us> tune-period override · W <us> row charge ·
//              A/R/F evo toggles (settle mask / shuffle / ref ladder)
//   Rig        s demo scan · n re-pick scanner · m menu · r reboot
//
// Libraries: Adafruit GFX (required), gt911-arduino (required),
// JPEGDEC (required for tuning and the scan preview).
// ============================================================================

// ---------------------------------------------------------------------------
// >>> YOUR NETWORK GOES HERE <<<
//
// Either fill these in, or — better, and what stops a password reaching a
// public repository — drop a wifi_secrets.h next to this sketch:
//
//     #define WIFI_SSID "your network"
//     #define WIFI_PASS "your password"
//
// It is gitignored, so it cannot be committed by accident. This file has
// carried a real password to GitHub once already; the header exists so it
// cannot happen twice.
// ---------------------------------------------------------------------------
#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif

// Name this board gets in exported tables (`e` at the menu): "H716" prints
// TUNED16_H716_HIGH and so on, ready to paste into src/EPD_Painter_trains.h.
#ifndef TUNED_EXPORT_PREFIX
  #define TUNED_EXPORT_PREFIX "LILYGO_T5S3"
#endif

// Choose your board (or leave all commented for auto-probe).
//#define EPD_PAINTER_PRESET_M5PAPER_S3
//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS

// Set to 0 if you don't want to install the JPEGDEC library — the demo scan
// still runs (reporting the size only) but SELF-TUNE needs it.
// (An explicit switch, not __has_include: the Arduino build only pulls a
// library into the include path when it sees an unconditional include.)
#define TUNEUP_HAVE_JPEGDEC 1

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "EPD_Painter_Adafruit.h"
#include "tuned_storage.h"
#include "tuneup_splash.h"
#include <gt911_lite.h>

#if TUNEUP_HAVE_JPEGDEC
#include <JPEGDEC.h>
#endif

EPD_PainterAdafruit epd(EPD_PAINTER_PRESET);
Preferences prefs;

// A boxed, centred, tappable label (defined up here so the Arduino
// preprocessor's hoisted prototypes can see the type).
struct Btn { int x, y, w, h; };

// ---- demo scan parameters --------------------------------------------------
// Region measured from the glass origin (the corner arrow on the scanner
// bed). 125 x 72 mm comfortably covers a 960x540 panel laid in the corner.
static const int   SCAN_DPI   = 150;
static const float SCAN_W_MM  = 125.0f;
static const float SCAN_H_MM  = 72.0f;
static const size_t SCAN_BUF_MAX = 2u * 1024u * 1024u;  // PSRAM, plenty for a JPEG

// ---- state -----------------------------------------------------------------
static String   scHost;            // scanner host (IP as text) …
static uint16_t scPort = 0;        // … and port
static String   scName = "?";      // MakeAndModel from its capabilities
static uint8_t *scanBuf = nullptr; // shared download buffer (PSRAM)
static bool     tunedLoaded = false;

// ============================================================================
// EPD console — a dumb teletype on the panel. Every line also goes to serial.
// ============================================================================
static int uiRow = 48;

static void uiClear(const char *title) {
  epd.fillScreen(0);
  epd.fillRect(0, 0, epd.width(), 36, 15);
  epd.setTextWrap(false);
  epd.setTextSize(3);
  epd.setTextColor(0);
  epd.setCursor(12, 7);
  epd.print(title);
  epd.setTextSize(2);
  epd.setTextColor(15);
  uiRow = 48;
  epd.paint();
  Serial.printf("\n===== %s =====\n", title);
}

static void uiLine(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.println(b);
  b[78] = 0;  // one panel row at text size 2
  if (uiRow > epd.height() - 18) {           // body full — wipe and restart
    epd.fillRect(0, 40, epd.width(), epd.height() - 40, 0);
    uiRow = 48;
  }
  epd.setCursor(12, uiRow);
  epd.print(b);
  uiRow += 18;
  epd.paint();
}

// ============================================================================
// Touch — GT911, same probing pattern as maze_3d: the display driver owns
// the I2C bus; the coordinate range comes from the controller's own config
// registers (0x8146/8148), swapped axes detected from portrait ranges.
// ============================================================================
static GT911_Lite touch;
static uint16_t tp_xmax = 960, tp_ymax = 540;
static bool     tp_swap = false, touch_ok = false;

static void touchInit() {
  TwoWire *bus = epd.getConfig().i2c.wire;
  if (!bus) return;
  // Never let a wedged bus hang the UI: a stalled I2C read would block the
  // wait loops forever, taking the serial console down with it and leaving
  // the board looking dead.
  bus->setTimeOut(50);
  touch_ok = false;
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
    Serial.printf("[tuneup] touch 0x%02x, range %ux%u%s\n", addr, xm, ym,
                  tp_swap ? " (swapped)" : "");
    return;
  }
  Serial.println("[tuneup] no touch controller found!");
}

// Re-open the touch controller and forget any stuck press state.
//
// After a long tuning run — a quarter of an hour of continuous panel
// driving, with the power controller sharing this same I2C bus — the
// GT911 stops reporting, so every interactive screen that follows looks
// dead to the user. Re-running begin() revives it; clearing the edge flag
// covers the other half of the problem, a press left latched "down" by
// the board resting face-first on the scanner glass, which would stop any
// later tap ever registering as a fresh press.
static bool touch_was_down = false;

// Called before a screen that waits for a tap. It only forgets a stale
// press edge — the board resting face-down on the glass can leave one
// latched, and then no later tap counts as a fresh press.
//
// It deliberately does NOT re-initialise the controller. The dead buttons
// this was written to cure turned out to be a polling-rate problem (see
// the delay() note in tuner.h's wait loops), and re-applying the GT911's
// config mid-session is a risk for no proven gain.
static void touchRewake() {
  touch_was_down = false;
}

// One rising-edge tap, mapped to panel coordinates. Non-blocking.
static bool touchTapped(int &px, int &py) {
  if (!touch_ok) return false;
  bool &was_down = touch_was_down;
  touch.read();
  bool down = touch.isTouched;
  bool tap = down && !was_down;
  was_down = down;
  if (!tap) return false;
  uint16_t rx = touch.x, ry = touch.y;
  if (tp_swap) {
    px = (int)ry * epd.width() / tp_ymax;
    py = (int)(tp_xmax - rx) * epd.height() / tp_xmax;
  } else {
    px = (int)rx * epd.width() / tp_xmax;
    py = (int)ry * epd.height() / tp_ymax;
  }
  return true;
}

static void drawBtn(const Btn &b, const char *l1, const char *l2 = nullptr) {
  epd.drawRect(b.x, b.y, b.w, b.h, 15);
  epd.drawRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 15);
  epd.setTextColor(15);
  epd.setTextSize(3);
  int tw = strlen(l1) * 18;
  epd.setCursor(b.x + (b.w - tw) / 2, b.y + (l2 ? 10 : (b.h - 21) / 2));
  epd.print(l1);
  if (l2) {
    epd.setTextSize(2);
    tw = strlen(l2) * 12;
    epd.setCursor(b.x + (b.w - tw) / 2, b.y + b.h - 26);
    epd.print(l2);
  }
  epd.setTextSize(2);
}
static bool hitBtn(const Btn &b, int px, int py) {
  return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}

// ============================================================================
// WiFi — credentials are compile-time. No keyboard needed anywhere.
// ============================================================================
static void wifiConnect() {
  if (!strlen(WIFI_SSID)) {
    uiClear("WIFI NOT SET");
    uiLine("This board has no keyboard, so the WiFi details live in the");
    uiLine("sketch itself.");
    uiLine("");
    uiLine("Open   examples/other/tuneup/tuneup.ino");
    uiLine("Set    #define WIFI_SSID \"your network\"");
    uiLine("       #define WIFI_PASS \"your password\"");
    uiLine("near the top of the file, then flash it again.");
    for (;;) delay(1000);
  }
  uiClear("WIFI");
  WiFi.mode(WIFI_STA);
  // Modem power-save OFF. In power-save the ESP32 sleeps between beacons
  // and depends on the router buffering downstream frames for it — and
  // when that goes wrong, requests still SEND fine while responses are
  // delayed or lost, which reads as "the scanner stopped answering" with
  // a perfectly healthy scanner (one whole night went to exactly that:
  // job POSTs arriving at the bridge, the board never seeing the replies).
  // Costs ~70 mA; this is a mains-fed rig, not a battery app.
  WiFi.setSleep(false);
  for (int attempt = 1;; attempt++) {
    uiLine("Connecting to \"%s\" (attempt %d) ...", WIFI_SSID, attempt);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (millis() - t0 < 20000) {
      if (WiFi.status() == WL_CONNECTED) {
        uiLine("Connected, IP %s", WiFi.localIP().toString().c_str());
        delay(800);
        return;
      }
      delay(200);
    }
    WiFi.disconnect(true);
    uiLine("No luck - check WIFI_SSID / WIFI_PASS in the sketch. Retrying.");
    delay(2000);
  }
}

// ============================================================================
// Minimal HTTP/1.0 client. HTTP/1.0 on purpose: the server can't chunk the
// reply and closes the connection at the end of the body — so "read until
// close" is the whole framing story, and a blocking scanner (one that scans
// DURING the GET) just looks like a slow body.
// ============================================================================
static bool httpTxn(const char *method, const String &path, const String &body,
                    uint8_t *buf, size_t bufMax, size_t *outLen,
                    int *outStatus, String *outLocation,
                    uint32_t timeoutMs = 120000) {
  *outLen = 0;
  *outStatus = 0;
  WiFiClient c;
  if (!c.connect(scHost.c_str(), scPort, 8000)) {
    Serial.printf("[http] %s: connect to %s:%u failed\n", method,
                  scHost.c_str(), scPort);
    return false;
  }
  c.setTimeout(timeoutMs);

  c.printf("%s %s HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\nUser-Agent: EPD_Painter-tuneup\r\n",
           method, path.c_str(), scHost.c_str(), scPort);
  if (body.length())
    c.printf("Content-Type: text/xml\r\nContent-Length: %u\r\n", (unsigned)body.length());
  c.print("\r\n");
  if (body.length()) c.print(body);

  String line = c.readStringUntil('\n');           // "HTTP/1.x 200 OK"
  int sp = line.indexOf(' ');
  if (sp < 0) {
    // The request went out and no status line ever came back — the
    // response direction is failing. Say so, WITH the internal heap
    // state: lwIP buffers received segments in internal RAM, and a
    // starved pool drops them silently — sends keep working while every
    // response "vanishes", which spent a whole night masquerading as a
    // scanner wedge.
    Serial.printf("[http] %s: no response from %s:%u (request sent OK; "
                  "int heap %u free, largest %u)\n",
                  method, scHost.c_str(), scPort,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    c.stop();
    return false;
  }
  *outStatus = atoi(line.c_str() + sp + 1);

  for (;;) {                                        // headers
    line = c.readStringUntil('\n');
    line.trim();
    if (!line.length()) break;
    if (outLocation && line.startsWith("Location:")) {
      *outLocation = line.substring(9);
      outLocation->trim();
    }
  }

  size_t n = 0;                                     // body until close
  uint32_t deadline = millis() + timeoutMs;
  while ((c.connected() || c.available()) && millis() < deadline && n < bufMax) {
    int av = c.available();
    if (!av) { delay(5); continue; }
    int r = c.read(buf + n, min((size_t)av, bufMax - n));
    if (r > 0) n += r;
  }
  c.stop();
  *outLen = n;
  return true;
}

// Pull "<anyprefix:Tag>value<" out of an XML blob — good enough for eSCL.
static String xmlTag(const char *xml, const char *tag) {
  String needle = String(":") + tag + ">";
  const char *p = strstr(xml, needle.c_str());
  if (!p) return "";
  p += needle.length();
  const char *e = strchr(p, '<');
  if (!e) return "";
  return String(p).substring(0, e - p);
}

// ============================================================================
// eSCL scan — POST a job for a region (mm from the glass origin), download
// the JPEG into scanBuf, DELETE the job. Returns the byte count (0 = fail).
// ============================================================================
static size_t esclScan(float x0mm, float y0mm, float wmm, float hmm, int dpi) {
  const int xo = (int)(x0mm / 25.4f * 300.0f);       // eSCL units: 1/300 inch
  const int yo = (int)(y0mm / 25.4f * 300.0f);
  const int w300 = (int)(wmm / 25.4f * 300.0f);
  const int h300 = (int)(hmm / 25.4f * 300.0f);
  String xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" "
      "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">"
      "<pwg:Version>2.0</pwg:Version>"
      "<scan:Intent>Photo</scan:Intent>"
      "<pwg:ScanRegions>"
      "<pwg:ScanRegion>"
      "<pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>"
      "<pwg:XOffset>" + String(xo) + "</pwg:XOffset>"
      "<pwg:YOffset>" + String(yo) + "</pwg:YOffset>"
      "<pwg:Width>" + String(w300) + "</pwg:Width>"
      "<pwg:Height>" + String(h300) + "</pwg:Height>"
      "</pwg:ScanRegion>"
      "</pwg:ScanRegions>"
      "<scan:InputSource>Platen</scan:InputSource>"
      "<scan:ColorMode>Grayscale8</scan:ColorMode>"
      "<scan:XResolution>" + String(dpi) + "</scan:XResolution>"
      "<scan:YResolution>" + String(dpi) + "</scan:YResolution>"
      "<pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>"
      "</scan:ScanSettings>";

  size_t len; int status; String location;
  if (!httpTxn("POST", "/eSCL/ScanJobs", xml, scanBuf, 4096, &len, &status, &location, 15000))
    return 0;
  if ((status != 201 && status != 200) || !location.length()) {
    Serial.printf("[escl] job refused (HTTP %d)\n", status);
    return 0;
  }
  if (location.startsWith("http")) {              // absolute URL -> path only
    int slash = location.indexOf('/', 8);
    if (slash > 0) location = location.substring(slash);
  }

  // Some scanners block the GET until the page is ready; others answer 503
  // until then. Handle both.
  //
  // Do NOT retry a 404 here. On the MG5200's NAS bridge the GET performs the
  // whole scan inline, and when scanimage finally gives up the bridge marks
  // the job done and answers 503 — so the 503 retry below immediately draws
  // a 404 from the now-dead job. That 404 means "this job is finished",
  // never "not yet", and retrying it can only burn the timeout. Failing fast
  // is correct: the rig's outer retry POSTs a FRESH job, which is the only
  // thing that can succeed once the Canon's BJNP stack has wedged.
  uint32_t t0 = millis();
  for (;;) {
    if (!httpTxn("GET", location + "/NextDocument", "", scanBuf, SCAN_BUF_MAX, &len, &status, nullptr, 120000))
      return 0;
    if (status == 200 && len) break;
    if (status == 503 && millis() - t0 < 120000) { delay(1500); continue; }
    Serial.printf("[escl] no document (HTTP %d)\n", status);
    return 0;
  }
  { // tell the scanner we're done with the job (best effort). Its own
    // buffer: parking the reply in the tail of scanBuf would corrupt the
    // end of a JPEG that filled the buffer, before it was ever decoded.
    static uint8_t dbuf[512];
    size_t dl; int ds;
    httpTxn("DELETE", location, "", dbuf, sizeof(dbuf), &dl, &ds, nullptr, 5000);
  }
  const bool isJpeg = len > 2 && scanBuf[0] == 0xFF && scanBuf[1] == 0xD8;
  Serial.printf("[escl] %u bytes in %lu s (%s)\n", (unsigned)len,
                (millis() - t0) / 1000, isJpeg ? "JPEG" : "not JPEG?");
  return isJpeg ? len : 0;
}

// ============================================================================
// JPEG -> 8-bit grayscale (grayImg/grayW/grayH)
// ============================================================================
#if TUNEUP_HAVE_JPEGDEC
// In PSRAM, NOT a static. A JPEGDEC instance is ~50 KB, and as a static it
// lived in internal DRAM — the same pool lwIP allocates WiFi RECEIVE
// buffers from. This sketch's statics plus the driver's fastbuffer plus
// the WiFi stack left ~2 KB of internal free in 16-grey mode, at which
// point every HTTP response was silently dropped on arrival while
// requests kept sending: two days of "the scanner stopped answering"
// with a perfectly healthy scanner. Measured at the failure: 1836 bytes
// free, largest block 1396.
static JPEGDEC *jpegDecP;
#define jpegDec (*jpegDecP)
static uint8_t *grayImg = nullptr;
static int      grayW = 0, grayH = 0;

static int jpegDrawCb(JPEGDRAW *d) {
  const uint8_t *src = (const uint8_t *)d->pPixels;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= grayH) break;
    int w = min(d->iWidth, grayW - d->x);
    if (w > 0) memcpy(grayImg + (size_t)dy * grayW + d->x, src + (size_t)y * d->iWidth, w);
  }
  return 1;
}

static bool decodeGrayFromScan(size_t len) {
  if (!jpegDec.openRAM(scanBuf, (int)len, jpegDrawCb)) {
    Serial.printf("[jpeg] open failed (err %d)\n", jpegDec.getLastError());
    return false;
  }
  jpegDec.setPixelType(EIGHT_BIT_GRAYSCALE);
  grayW = jpegDec.getWidth();
  grayH = jpegDec.getHeight();

  // If the scanner ignored our region/resolution and sent something huge,
  // decode at 1/2, 1/4 or 1/8 scale rather than running out of PSRAM.
  if (grayImg) { heap_caps_free(grayImg); grayImg = nullptr; }
  int opt = 0;
  for (int shift = 0; shift <= 3; shift++) {
    const size_t need = ((size_t)grayW >> shift) * ((size_t)grayH >> shift);
    if (need + 65536 < heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)) {
      static const int opts[] = { 0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH };
      opt = opts[shift];
      grayW >>= shift;
      grayH >>= shift;
      break;
    }
    if (shift == 3) {
      Serial.println("[jpeg] out of PSRAM even at 1/8 scale");
      jpegDec.close();
      return false;
    }
  }
  grayImg = (uint8_t *)heap_caps_malloc((size_t)grayW * grayH, MALLOC_CAP_SPIRAM);
  if (!grayImg) { jpegDec.close(); return false; }
  int ok = jpegDec.decode(0, 0, opt);
  jpegDec.close();
  if (!ok) Serial.printf("[jpeg] decode failed (err %d)\n", jpegDec.getLastError());
  return ok;
}

// ---- the "Tune up Complete" artwork ---------------------------------------
// Decoded straight out of flash into the canvas. An ordered dither carries
// the photograph's gradients: at 16 levels a face quantised flat shows
// obvious contour bands across the skin, while the dither's texture is
// finer than the eye resolves at reading distance.
static uint8_t *splashFb = nullptr;
static int      splashFbW = 0, splashFbH = 0;

static const uint8_t splashBayer8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 }
};

// The decoder hands back blocks, so gather the whole greyscale image first
// (into the scan buffer, which is idle here) and dither it in one pass.
static uint8_t *splashGray = nullptr;

static int splashDrawCb(JPEGDRAW *d) {
  const uint8_t *src = (const uint8_t *)d->pPixels;
  for (int y = 0; y < d->iHeight; y++) {
    const int dy = d->y + y;
    if (dy < 0 || dy >= splashFbH) continue;
    uint8_t *drow = splashGray + (size_t)dy * splashFbW;
    for (int x = 0; x < d->iWidth; x++) {
      const int dx = d->x + x;
      if (dx >= 0 && dx < splashFbW) drow[dx] = src[(size_t)y * d->iWidth + x];
    }
  }
  return 1;
}

// Floyd-Steinberg to however many levels the panel is actually in.
//
// A photograph needs error diffusion, not an ordered matrix: at 16 levels
// a Bayer dither leaves its own cross-hatch in smooth areas and still
// contours across skin, whereas diffusion pushes each pixel's rounding
// error into its neighbours and the banding disappears into fine grain.
//
// THE LEVEL COUNT IS NOT A CONSTANT. This diffused to 16 levels whatever the
// panel was in, and in 4-level mode the packer masks each byte with 0x03 —
// so levels 4, 8 and 12 all came out white and a face rendered as a relief
// carving with the eyes and lips as isolated blobs. Four greys are faint but
// perfectly good for a photograph; four greys with the mid-tones wrapped
// around to white are not a photograph at all.
// Dither from every splashStride'th level. 1 = all of them. 2 = eight of
// sixteen: the steps double, so any context drift of a level is half the
// fraction of a step — the banding-vs-grain trade, pushed toward grain.
static int splashStride = 1;

// Quantise-only mode: nearest level, NO error diffusion. The diagnostic
// that separates dither artifacts from level artifacts — worming needs
// diffused error to exist, so an image that cleans up here convicts the
// dither, and streaks that survive convict the levels.
static bool splashNoDither = false;

static void splashDiffuse(uint8_t *fb, int W, int H) {
  // What each level really renders as, measured on the glass by the tuner
  // and kept with the tables. The levels are NOT evenly spaced — on this
  // panel the light end bunches together — so quantising against an
  // assumed even ramp diffuses error into levels that cannot express it,
  // which is precisely what banding looks like. Falls back to even steps
  // on a board that has never been measured.
  //
  // The curve has to match the level COUNT as well: Config::levelLum(4)
  // returns the 4-level curve the FAST tune measured, where the first four
  // entries of the 16-grey curve are four near-white levels standing in for
  // a whole ramp.
  const int nlv = epd.driver().greyLevels();
  const uint8_t *lum = epd.driver()._config.levelLum(nlv);
  uint8_t even[16];
  if (!lum) {
    for (int i = 0; i < nlv; i++)
      even[i] = (uint8_t)(255 - i * 255 / (nlv - 1));
    lum = even;
  }

  // PSRAM, not .bss (3.8 KB of internal otherwise — see the lwIP note in
  // httpTxn's failure path).
  static int16_t *errCur = nullptr, *errNext = nullptr;
  if (!errCur) {
    errCur  = (int16_t *)heap_caps_malloc(TUNEUP_SPLASH_W * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    errNext = (int16_t *)heap_caps_malloc(TUNEUP_SPLASH_W * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!errCur || !errNext) return;
  }
  memset(errCur, 0, TUNEUP_SPLASH_W * sizeof(int16_t));
  for (int y = 0; y < H; y++) {
    // FULL-WIDTH clear and copy. When these rows moved from static arrays
    // to PSRAM pointers, sizeof() silently became sizeof(pointer) = 8
    // bytes: the error rows were never cleared OR carried between rows,
    // error accumulated unbounded down the image, and every dithered
    // photo grew catastrophic full-height streaks within hours.
    memset(errNext, 0, TUNEUP_SPLASH_W * sizeof(int16_t));
    const uint8_t *srow = splashGray + (size_t)y * W;
    uint8_t *drow = fb + (size_t)y * W;
    // Serpentine + exact error conservation, matching the library's
    // GrayDither: a fixed scan direction organises diffusion bias into
    // column-aligned worms, and truncated shares leak error.
    const bool rtl = (y & 1);
    for (int i = 0; i < W; i++) {
      const int x = rtl ? (W - 1 - i) : i;
      const int fwd = rtl ? -1 : 1;
      int v = (int)srow[x] + errCur[x];
      if (v < 0) v = 0; else if (v > 255) v = 255;
      // nearest level by MEASURED luminance, over the live levels only
      int lv = 0, bestd = 1000;
      for (int li = 0; li < nlv; li += splashStride) {
        const int d = abs((int)lum[li] - v);
        if (d < bestd) { bestd = d; lv = li; }
      }
      drow[x] = (uint8_t)lv;                       // lum[] is already level-indexed
      if (splashNoDither) continue;                // quantise-only diagnostic
      const int err = v - (int)lum[lv];            // what we could not show
      const int e3 = (err * 3) / 16;
      const int e5 = (err * 5) / 16;
      const int e1 = (err * 1) / 16;
      const int e7 = err - e3 - e5 - e1;           // ahead: share + remainder
      const int xa = x + fwd, xb = x - fwd;
      if (xa >= 0 && xa < W) { errCur[xa] += (int16_t)e7; errNext[xa] += (int16_t)e1; }
      if (xb >= 0 && xb < W) errNext[xb] += (int16_t)e3;
      errNext[x] += (int16_t)e5;
    }
    memcpy(errCur, errNext, TUNEUP_SPLASH_W * sizeof(int16_t));
  }
}

static void tuneupSplashDraw(uint8_t *fb, int fbW, int fbH) {
  splashFb = fb; splashFbW = fbW; splashFbH = fbH;
  memset(fb, 0, (size_t)fbW * fbH);

  // Borrow the scan buffer for the greyscale image — it is only in use
  // while a scan is in flight, and never during this screen.
  splashGray = scanBuf;
  if (!splashGray) { Serial.println("[splash] no buffer"); return; }
  memset(splashGray, 255, (size_t)fbW * fbH);

  // Reuse the file-scope decoder: a JPEGDEC holds several kilobytes of
  // internal buffers, so declaring one on the stack overflows the loop
  // task (stack canary panic).
  if (!jpegDec.openRAM((uint8_t *)TUNEUP_SPLASH_JPEG, (int)TUNEUP_SPLASH_JPEG_LEN,
                       splashDrawCb)) {
    Serial.printf("[splash] open failed (%d)\n", jpegDec.getLastError());
    return;
  }
  jpegDec.setPixelType(EIGHT_BIT_GRAYSCALE);
  const bool ok = jpegDec.decode(0, 0, 0);
  jpegDec.close();
  if (!ok) {
    Serial.printf("[splash] decode failed (%d)\n", jpegDec.getLastError());
    return;
  }
  splashDiffuse(fb, fbW, fbH);
}

// Show the decoded scan letterboxed on the panel in 16 native greys, with
// a histogram/stats line on serial.
static void showScan() {
  uint32_t hist[16] = {0};
  uint64_t sum = 0;
  const size_t np = (size_t)grayW * grayH;
  for (size_t i = 0; i < np; i++) {
    hist[grayImg[i] >> 4]++;
    sum += grayImg[i];
  }
  Serial.printf("[analysis] %dx%d mean %.1f\n", grayW, grayH, (double)sum / np);
  Serial.print("[analysis] histogram (dark..light): ");
  for (int i = 15; i >= 0; i--) Serial.printf("%lu ", (unsigned long)hist[i]);
  Serial.println();

  const int W = epd.width(), H = epd.height();
  float s = min((float)W / grayW, (float)H / grayH);
  int dw = (int)(grayW * s), dh = (int)(grayH * s);
  int x0 = (W - dw) / 2, y0 = (H - dh) / 2;
  uint8_t *fb = epd.getBuffer();
  memset(fb, 0, (size_t)W * H);
  for (int y = 0; y < dh; y++) {
    const uint8_t *srow = grayImg + (size_t)(int)(y / s) * grayW;
    uint8_t *drow = fb + (size_t)(y0 + y) * W + x0;
    for (int x = 0; x < dw; x++)
      drow[x] = 15 - (srow[(int)(x / s)] >> 4);   // 255 (paper white) -> level 0
  }
  epd.paint();
}
#endif

// ============================================================================
// Scanner discovery + selection (touch, with serial fallback), remembered
// in NVS so a face-down boot needs no interaction.
// ============================================================================
static bool esclHello(const String &host, uint16_t port, String *makeModel) {
  String saveH = scHost; uint16_t saveP = scPort;
  scHost = host; scPort = port;
  size_t len; int status;
  bool ok = httpTxn("GET", "/eSCL/ScannerCapabilities", "", scanBuf, 32768, &len, &status, nullptr, 10000);
  scHost = saveH; scPort = saveP;
  if (!ok || status != 200 || !len) return false;
  scanBuf[min(len, (size_t)32767)] = 0;
  String mm = xmlTag((const char *)scanBuf, "MakeAndModel");
  *makeModel = mm.length() ? mm : String("eSCL scanner");
  return true;
}

static bool scannerTrySaved() {
  String host = prefs.getString("schost", "");
  uint16_t port = (uint16_t)prefs.getUInt("scport", 0);
  if (!host.length() || !port) return false;
  uiLine("Scanner: trying saved %s:%u ...", host.c_str(), port);
  String mm;
  if (esclHello(host, port, &mm)) {
    scHost = host; scPort = port; scName = mm;
    uiLine("Scanner ready: %s", mm.c_str());
    return true;
  }
  uiLine("Saved scanner not answering.");
  return false;
}

// Draw the discovered scanners as buttons; tap picks one (or type its
// number over serial), RESCAN repeats the search.
static void pickScanner() {
  struct Hit { String host; uint16_t port; String label; };
  static Hit hits[5];

  for (;;) {
    uiClear("FIND SCANNER");
    uiLine("Searching for eSCL/AirScan scanners (_uscan._tcp) ...");

    int nh = 0;
    for (int round = 0; round < 3 && nh == 0; round++) {
      int n = MDNS.queryService("uscan", "tcp");
      for (int i = 0; i < n && nh < 5; i++) {
        String ip = MDNS.address(i).toString();
        uint16_t port = MDNS.port(i);
        if (ip == "0.0.0.0" || !port) continue;
        bool dup = false;
        for (int j = 0; j < nh; j++)
          if (hits[j].host == ip && hits[j].port == port) dup = true;
        if (dup) continue;
        hits[nh].host = ip;
        hits[nh].port = port;
        hits[nh].label = MDNS.hostname(i);
        nh++;
      }
    }

    // ---- the pick screen ----
    const int W = epd.width(), H = epd.height();
    epd.fillScreen(0);
    epd.fillRect(0, 0, W, 36, 15);
    epd.setTextSize(3);
    epd.setTextColor(0);
    epd.setCursor(12, 7);
    epd.print(nh ? "TAP YOUR SCANNER" : "NO SCANNERS FOUND");
    epd.setTextColor(15);

    Btn rows[5];
    const int top = 52, gap = 10;
    const int rh = 74;
    for (int i = 0; i < nh; i++) {
      rows[i] = { 20, top + i * (rh + gap), W - 40, rh };
      char l2[64];
      snprintf(l2, sizeof(l2), "%s:%u", hits[i].host.c_str(), hits[i].port);
      drawBtn(rows[i], hits[i].label.c_str(), l2);
      Serial.printf("[tuneup] %d: %s at %s\n", i + 1, hits[i].label.c_str(), l2);
    }
    if (!nh) {
      epd.setTextSize(2);
      epd.setCursor(20, 70);
      epd.print("Is the scanner awake and on this network?");
    }
    Btn rescan = { 20, H - 84, W - 40, 64 };
    drawBtn(rescan, "RESCAN");
    epd.paint();
    Serial.println("[tuneup] tap a scanner, or type its number (r = rescan)");

    int sel = -1;
    bool rescanNow = false;
    while (sel < 0 && !rescanNow) {
      int px, py;
      if (touchTapped(px, py)) {
        if (hitBtn(rescan, px, py)) rescanNow = true;
        for (int i = 0; i < nh; i++)
          if (hitBtn(rows[i], px, py)) sel = i;
      }
      if (Serial.available()) {
        const int ch = Serial.read();
        if (ch == 'r') rescanNow = true;
        else if (ch >= '1' && ch <= '0' + nh) sel = ch - '1';
      }
      delay(20);
    }
    if (rescanNow) continue;

    uiClear("CHECKING");
    uiLine("Asking %s:%u for its capabilities ...", hits[sel].host.c_str(), hits[sel].port);
    String mm;
    if (esclHello(hits[sel].host, hits[sel].port, &mm)) {
      scHost = hits[sel].host;
      scPort = hits[sel].port;
      scName = mm;
      prefs.putString("schost", scHost);
      prefs.putUInt("scport", scPort);
      uiLine("Scanner ready: %s  (saved)", mm.c_str());
      delay(800);
      return;
    }
    uiLine("No eSCL answer from %s:%u.", hits[sel].host.c_str(), hits[sel].port);
    delay(2000);
  }
}

// ============================================================================
// The demo scan
// ============================================================================
static bool demoScan() {
  uiClear("SCANNING");
  uiLine("Scanner: %s  (%s:%u)", scName.c_str(), scHost.c_str(), scPort);
  uiLine("Job: %.0f x %.0f mm, %d dpi, grayscale. Lamp should be moving ...",
         SCAN_W_MM, SCAN_H_MM, SCAN_DPI);
  const size_t len = esclScan(0, 0, SCAN_W_MM, SCAN_H_MM, SCAN_DPI);
  if (!len) {
    uiLine("Scan failed - see serial log.");
    return false;
  }
  uiLine("Received %u bytes.", (unsigned)len);
#if TUNEUP_HAVE_JPEGDEC
  if (decodeGrayFromScan(len)) showScan();
#else
  uiLine("Set TUNEUP_HAVE_JPEGDEC to 1 (and install JPEGDEC) to see it here.");
#endif
  return true;
}

// ============================================================================
// The self-tuner (stage 2) — see tuner.h (16-grey) and tuner_fast.h (FAST).
// ============================================================================
#include "tuner.h"
#include "tuner_fast.h"
#include "tuner_evo.h"
#include "tuner_evo4.h"

// One entry point for all three. They share the rig — registration, the
// scanner, the results screen — and differ in what they are measuring.
// How many generations the evolutionary tuner runs. It measures 60
// candidates per scan, and on the first run reached worst 3.3 units by
// generation 13 - matching the best result this board has ever produced,
// in a third of the scans. 30 leaves room to refine without feeding the
// printer more acquisitions than it tolerates.
static int evoGenerations = 30;

static bool runSelfTune(Set which) {
#if TUNEUP_HAVE_JPEGDEC
  switch (which) {
    case SET_FAST: return runSelfTuneFast();
    case SET_HIGH: return runEvoTune(EPD_Painter::Quality::QUALITY_HIGH, evoGenerations);
    default:       return runEvoTune(EPD_Painter::Quality::QUALITY_NORMAL, evoGenerations);
  }
#else
  (void)which;
  Serial.println("[tune] JPEGDEC required - set TUNEUP_HAVE_JPEGDEC to 1");
  return false;
#endif
}

// ============================================================================
// Which quality to tune.
//
// The three are separate tunes with separate results, kept in separate
// blobs, so this is a real choice rather than a preference: tuning one
// leaves the others exactly as they were. The screen says what each one
// costs, because they differ by a factor of three in time and the user is
// about to leave the board face-down on a scanner.
// ============================================================================
static Set tunePickQuality(bool *cancelled) {
  const int W = epd.width(), H = epd.height();
  epd.fillScreen(0);
  epd.fillRect(0, 0, W, 40, 15);
  epd.setTextColor(0);
  epd.setTextSize(3);
  epd.setCursor(12, 8);
  epd.print("WHAT SHALL I TUNE?");
  epd.setTextColor(15);
  epd.setTextSize(2);
  epd.setCursor(12, 52);
  epd.print("Each quality is tuned and stored on its own. Tuning one");
  epd.setCursor(12, 72);
  epd.print("leaves the other two exactly as they are.");

  const int bw = (W - 80) / 3, by = 110, bh = 210;
  Btn bFast   = { 20,                by, bw, bh };
  Btn bNormal = { 40 + bw,           by, bw, bh };
  Btn bHigh   = { 60 + bw * 2,       by, bw, bh };
  drawBtn(bFast,   "FAST",   "4 levels + motion");
  drawBtn(bNormal, "NORMAL", "16 greys, 13 passes");
  drawBtn(bHigh,   "HIGH",   "16 greys, 20 passes");

  epd.setTextSize(1);
  // Periods from the live config, not literals — they differ per board
  // (15/15 ms on the LilyGo GPS, 20/20 on the H716), and a hardcoded
  // number here once described a different board than the one showing it.
  char nNorm[40], nHigh[40];
  snprintf(nNorm, sizeof(nNorm), "%d passes at %d ms.", EPD_Painter::DEC_WF_LEN16,
           epd.driver()._config.g16_pass_us_normal / 1000);
  snprintf(nHigh, sizeof(nHigh), "%d passes at %d ms.", EPD_Painter::DEC_WF_LEN16_HIGH,
           epd.driver()._config.g16_pass_us_high / 1000);
  struct { Btn b; const char *l1, *l2, *l3; } notes[3] = {
    { bFast,   "7 passes, no settle delay.",  "Four levels and the six",     "grey-to-grey transitions." },
    { bNormal, nNorm,                         "Evolutionary search over",    "raw waveforms. ~15 min." },
    { bHigh,   nHigh,                         "Same, with 20 slots to",      "shape. ~15 min." },
  };
  for (int i = 0; i < 3; i++) {
    epd.setCursor(notes[i].b.x + 10, by + bh - 52);
    epd.print(notes[i].l1);
    epd.setCursor(notes[i].b.x + 10, by + bh - 40);
    epd.print(notes[i].l2);
    epd.setCursor(notes[i].b.x + 10, by + bh - 28);
    epd.print(notes[i].l3);
  }

  // What is already on this board, so a re-tune is a decision rather than
  // a guess.
  epd.setTextSize(2);
  epd.setCursor(12, by + bh + 24);
  char have[96];
  snprintf(have, sizeof(have), "Already tuned:  FAST %s   NORMAL %s   HIGH %s",
           tunedPresent(epd.driver(), SET_FAST)   ? "yes" : "no ",
           tunedPresent(epd.driver(), SET_NORMAL) ? "yes" : "no ",
           tunedPresent(epd.driver(), SET_HIGH)   ? "yes" : "no ");
  epd.print(have);

  Btn back = { 20, H - 90, W - 40, 70 };
  drawBtn(back, "BACK");
  epd.paint();
  Serial.println("[tuneup] tune which? tap, or serial: f = FAST, n = NORMAL, h = HIGH, q = back");

  // The finger that tapped TUNE ME is still on the glass, and these
  // buttons have just appeared underneath it: the menu's TUNE ME spans
  // x 490..940 and HIGH lands at 646..939, NORMAL at 333..626. Without a
  // settle and a drain, the press that OPENS this page also answers it,
  // and the tune starts on whichever option happened to be under the
  // finger. Same trap tuneConfirmScreen() documents - a full-screen paint
  // also throws transients into the GT911, so polling straight away
  // catches those as well as the real press.
  while (!epd.driver().paintIdle()) delay(10);
  delay(700);
  while (Serial.available()) Serial.read();
  touchRewake();
  int drainX, drainY;
  touchTapped(drainX, drainY);        // consume anything already latched

  for (;;) {
    if (Serial.available()) {
      const int c = Serial.read();
      if (c == 'f' || c == 'F') { *cancelled = false; return SET_FAST; }
      if (c == 'n' || c == 'N') { *cancelled = false; return SET_NORMAL; }
      if (c == 'h' || c == 'H') { *cancelled = false; return SET_HIGH; }
      if (c == 'q') { *cancelled = true; return SET_NORMAL; }
    }
    int px, py;
    if (touchTapped(px, py)) {
      if (hitBtn(bFast, px, py))   { *cancelled = false; return SET_FAST; }
      if (hitBtn(bNormal, px, py)) { *cancelled = false; return SET_NORMAL; }
      if (hitBtn(bHigh, px, py))   { *cancelled = false; return SET_HIGH; }
      if (hitBtn(back, px, py))    { *cancelled = true;  return SET_NORMAL; }
    }
    delay(8);
  }
}

// Touch entry: instructions + countdown so the user can close the lid.
static void tuneCountdownAndRun() {
  bool cancelled = false;
  const Set which = tunePickQuality(&cancelled);
  if (cancelled) return;

  uiClear("SELF-TUNE");
  uiLine("I am going to tune my own %s trains against the scanner.",
         tunedSetName(which));
  uiLine("");
  uiLine("1. Open the scanner lid.");
  uiLine("2. Lay me FACE DOWN on the glass, screen flat against it.");
  uiLine("3. Keep my USB lead connected (watch progress in the serial log).");
  uiLine("4. Close the lid gently as far as the cable allows.");
  uiLine("");
  uiLine(which == SET_FAST
             ? "The FAST tune takes about 10 minutes and flashes many patterns."
             : which == SET_HIGH
             ? "The HIGH tune takes about 25 minutes and flashes many patterns."
             : "The NORMAL tune takes 10-15 minutes and flashes many patterns.");
  for (int s = 30; s > 0; s--) {
    char b[40];
    snprintf(b, sizeof(b), "Starting in %2d s ...", s);
    epd.fillRect(0, epd.height() - 60, epd.width(), 40, 0);
    epd.setCursor(12, epd.height() - 52);
    epd.setTextSize(3);
    epd.print(b);
    epd.setTextSize(2);
    epd.paint();
    Serial.printf("[tune] %s\n", b);
    delay(1000);
    // Drain everything, not peek: a stray byte at the head of the buffer (a
    // leftover newline from the command that got us here) otherwise blocks
    // 'q' for the whole countdown.
    while (Serial.available())
      if (Serial.read() == 'q') return;
  }
  runSelfTune(which);
}

// ============================================================================
// Splash view — the artwork through each quality tier, held until tapped.
//
// A MODE button cycles FAST (4-grey) / NORMAL / HIGH; each stop repaints
// the photo through that tier's live tables with its own measured curve,
// and lays the tier's native staircase along the bottom — every level the
// dither is mixing from, driven directly, so an uneven ladder is visible
// next to the banding it causes. HIGH joins the ring only if this board
// actually has a HIGH table: rendering 20 passes through the formula
// library flatters nothing and would misrepresent the panel.
// ============================================================================
static void showSplash() {
  const bool haveHigh = tunedPresent(epd.driver(), SET_HIGH);
  // 0 FAST (4-grey), 1 NORMAL 4-grey (what an app in direct mode shows),
  // 2 NORMAL 16-grey, 3 HIGH 16-grey.
  int mode = haveHigh ? 3 : 2;
  int dx, dy;
  for (;;) {
    const char *qn = (mode == 0)   ? "FAST"
                     : (mode == 1) ? "NORMAL 4-GREY"
                     : (mode == 2) ? "NORMAL"
                                   : "HIGH";
    // Order matters both ways: setQuality(FAST) is refused while 16-grey
    // is live, and setGreyLevels(16) is refused while FAST is live.
    if (mode <= 1) {
      epd.driver().setGreyLevels(4);
      epd.setQuality(mode == 0 ? EPD_Painter::Quality::QUALITY_FAST
                               : EPD_Painter::Quality::QUALITY_NORMAL);
    } else {
      epd.setQuality(mode == 2 ? EPD_Painter::Quality::QUALITY_NORMAL
                               : EPD_Painter::Quality::QUALITY_HIGH);
      epd.driver().setGreyLevels(16);
    }
    epd.clear();
    while (!epd.driver().paintIdle()) delay(10);
    tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());

    const int W = epd.width(), H = epd.height();
    const int nlv = epd.driver().greyLevels();
    const int black = nlv - 1;
    // Native staircase: one patch per live level, white left, driven
    // directly (no dither) — the ladder itself, next to the photo.
    const int barH = 64;
    epd.fillRect(0, H - barH - 2, W, 2, black);
    for (int i = 0; i < nlv; i++) {
      const int x0 = (i * W) / nlv, x1 = ((i + 1) * W) / nlv;
      epd.fillRect(x0, H - barH, x1 - x0, barH, (uint8_t)i);
    }
    Btn bMode = { W - 290, 8, 280, 60 };
    epd.fillRect(bMode.x, bMode.y, bMode.w, bMode.h, 0);
    epd.drawRect(bMode.x, bMode.y, bMode.w, bMode.h, black);
    epd.drawRect(bMode.x + 1, bMode.y + 1, bMode.w - 2, bMode.h - 2, black);
    epd.setTextColor(black);
    epd.setTextSize(2);
    epd.setCursor(bMode.x + 12, bMode.y + 10);
    char mb[24];
    snprintf(mb, sizeof(mb), "MODE: %s", qn);
    epd.print(mb);
    epd.setCursor(bMode.x + 12, bMode.y + 36);
    epd.print("tap here to switch");

    epd.paint();
    while (!epd.driver().paintIdle()) delay(10);
    Serial.printf("[tuneup] splash at %s, %d levels - MODE button or 'm' "
                  "switches, anything else returns\n", qn, nlv);

    // Settle before listening: a full-screen paint throws transients into
    // the GT911, and the tap that opened this page is still latched.
    delay(700);
    while (Serial.available()) Serial.read();
    touchRewake();
    touchTapped(dx, dy);
    bool next = false;
    for (;;) {
      if (Serial.available()) {
        const int c = Serial.read();
        while (Serial.available()) Serial.read();
        if (c == 'm' || c == 'M') next = true;
        break;
      }
      if (touchTapped(dx, dy)) {
        next = hitBtn(bMode, dx, dy);
        break;
      }
      delay(8);
    }
    if (!next) break;
    mode = (mode + 1) % 4;
    if (mode == 3 && !haveHigh) mode = 0;
  }
  // The menu draws in 16-grey; put the panel back the way it expects.
  // Quality first: setGreyLevels(16) is refused while FAST is live.
  epd.setQuality(haveHigh ? EPD_Painter::Quality::QUALITY_HIGH
                          : EPD_Painter::Quality::QUALITY_NORMAL);
  epd.driver().setGreyLevels(16);
}

// ============================================================================
// Menu
// ============================================================================
static Btn btnScan, btnTune, btnSplash, btnScanner;

static void showMenuCard() {
  const int W = epd.width(), H = epd.height();
  epd.fillScreen(0);
  epd.fillRect(0, 0, W, 36, 15);
  epd.setTextSize(3);
  epd.setTextColor(0);
  epd.setCursor(12, 7);
  epd.print("EPD TUNEUP");
  epd.setTextColor(15);
  epd.setTextSize(2);
  epd.setCursor(12, 48);
  char b[100];
  snprintf(b, sizeof(b), "WiFi %s   Scanner %s (%s:%u)", WiFi.localIP().toString().c_str(),
           scName.c_str(), scHost.c_str(), scPort);
  b[78] = 0;
  epd.print(b);
  epd.setCursor(12, 68);
  epd.print(tunedLoaded ? "Running flash-tuned trains."
                        : "Running preset trains (not yet self-tuned).");

  const int bw3 = (W - 80) / 3;
  btnScan    = { 20,                100, bw3, 150 };
  btnTune    = { 40 + bw3,          100, bw3, 150 };
  btnSplash  = { 60 + bw3 * 2,      100, bw3, 150 };
  btnScanner = { 20, 270, W - 40, 70 };
  drawBtn(btnScan, "DEMO SCAN", "scan the bed, show it here");
  drawBtn(btnTune, "TUNE ME", "pick FAST / NORMAL / HIGH");
  drawBtn(btnSplash, "SHOW PHOTO", "see the greys on real art");
  drawBtn(btnScanner, "CHANGE SCANNER");

  epd.setCursor(12, H - 130);
  epd.print("Serial: s scan  T tune  4 4-grey  d directs  e export  D ladder");
  epd.setCursor(12, H - 110);
  epd.print("        S staircase  L blobs  X erase  n scanner  r reboot");
  epd.paint();
  Serial.println("[tuneup] menu: s/T/4/d/e/D/S/L/X/n/r (full list in the sketch header)");
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Calibration rig: never shutdown-on-reset. The flasher's hard reset
  // would otherwise power the board off after every upload — and a tune
  // can run unattended for a quarter of an hour, so it must survive one.
  epd.setAutoShutdown(false);

  // Keep the panel rails UP for the whole session. The 5 s idle power-off
  // races the 5 s scan gap: the rail power-down transient lands exactly as
  // the scan's HTTP request goes out, and the supply dip cost the radio
  // the RESPONSE — requests reached the bridge all night while replies
  // vanished, and on a marginal supply the same transient browned out the
  // whole board. Rails held up, the transient never happens mid-scan.
  epd.driver().setIdleTimeout(600);

  if (!epd.begin()) {
    Serial.println("EPD begin() failed - check board/preset.");
    for (;;) delay(1000);
  }

  if (!LittleFS.begin(true)) Serial.println("[tuneup] LittleFS mount failed");
  {
    const int n = tunedLoadAll(epd.driver());
    tunedLoaded = (n > 0);
    Serial.printf("[tuneup] tuned blobs: %d installed (FAST %s, NORMAL %s, HIGH %s)\n",
                  n,
                  tunedPresent(epd.driver(), SET_FAST)   ? "yes" : "no",
                  tunedPresent(epd.driver(), SET_NORMAL) ? "yes" : "no",
                  tunedPresent(epd.driver(), SET_HIGH)   ? "yes" : "no");
  }

  epd.driver().setGreyLevels(16);   // builds trains: preset or flash-tuned

  // The panel still physically holds whatever was on it before the reset,
  // but the driver comes up assuming a blank screen — so a delta paint
  // would leave the old image ghosting underneath. Start from a real
  // hard clear.
  epd.clear();
  while (!epd.driver().paintIdle()) delay(10);

  touchInit();

  scanBuf = (uint8_t *)heap_caps_malloc(SCAN_BUF_MAX, MALLOC_CAP_SPIRAM);
  if (!scanBuf) {
    Serial.println("No PSRAM for the scan buffer.");
    for (;;) delay(1000);
  }

#if TUNEUP_HAVE_JPEGDEC
  // The decoder object goes to PSRAM too — see its declaration.
  jpegDecP = new (heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM)) JPEGDEC();
  if (!jpegDecP) {
    Serial.println("No PSRAM for the JPEG decoder.");
    for (;;) delay(1000);
  }
#endif

  // The number that decides whether WiFi RECEIVES: lwIP takes its RX
  // buffers from internal RAM. Watch this across changes — under ~10 KB
  // free, responses start vanishing while requests still send.
  Serial.printf("[tuneup] internal heap: %u free, largest %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  prefs.begin("tuneup", false);

  uiClear("EPD TUNEUP");
  uiLine("The self-tuning rig.");
  if (tunedLoaded)
    uiLine("Flash-tuned trains loaded (period %d us).",
           epd.driver()._config.g16_pass_us_normal);
  uiLine("");

  wifiConnect();
  MDNS.begin("epd-tuneup");
  if (!scannerTrySaved()) pickScanner();
  showMenuCard();
}

void loop() {
  int px, py;
  if (touchTapped(px, py)) {
    if (hitBtn(btnScan, px, py)) {
      while (demoScan()) {
        // after-scan bar: left = again, right = back
        const int W = epd.width(), H = epd.height();
        epd.fillRect(0, H - 30, W, 30, 15);
        epd.setTextColor(0);
        epd.setCursor(12, H - 23);
        epd.print("TAP LEFT: scan again");
        epd.setCursor(W / 2 + 12, H - 23);
        epd.print("TAP RIGHT: menu");
        epd.setTextColor(15);
        epd.paint();
        int qx, qy;
        for (;;) {
          if (touchTapped(qx, qy)) break;
          if (Serial.available() && Serial.peek() == 's') { Serial.read(); qx = 0; break; }
          if (Serial.available()) { Serial.read(); qx = W; break; }
          delay(20);
        }
        if (qx >= W / 2) break;
      }
      showMenuCard();
    } else if (hitBtn(btnTune, px, py)) {
      tuneCountdownAndRun();
      showMenuCard();
    } else if (hitBtn(btnSplash, px, py)) {
      showSplash();
      showMenuCard();
    } else if (hitBtn(btnScanner, px, py)) {
      pickScanner();
      showMenuCard();
    }
    return;
  }

  if (!Serial.available()) { delay(20); return; }
  const int c = Serial.read();
  switch (c) {
    case 's': demoScan(); Serial.println("[tuneup] (any key for menu)"); break;
    case 'T': {
      bool cancelled = false;
      const Set which = tunePickQuality(&cancelled);
      if (!cancelled) runSelfTune(which);
      showMenuCard();
    } break;
    case 'P': {
      tunePeriodUs = (uint32_t)Serial.parseInt();
      Serial.printf("[tuneup] tune period override: %lu us%s\n",
                    (unsigned long)tunePeriodUs,
                    tunePeriodUs ? "" : " (preset)");
    } break;
    case 'L': {
      Serial.printf("[tuneup] blobs: FAST %s  NORMAL %s  HIGH %s\n",
                    tunedPresent(epd.driver(), SET_FAST)   ? "present" : "absent",
                    tunedPresent(epd.driver(), SET_NORMAL) ? "present" : "absent",
                    tunedPresent(epd.driver(), SET_HIGH)   ? "present" : "absent");
      Serial.printf("[tuneup] 16-grey periods: NORMAL %d us x %d passes, "
                    "HIGH %d us x %d passes\n",
                    epd.driver()._config.g16_pass_us_normal, EPD_Painter::DEC_WF_LEN16,
                    epd.driver()._config.g16_pass_us_high, EPD_Painter::DEC_WF_LEN16_HIGH);
    } break;
    case '4': {
      // 4-grey tune. FAST is 7 passes and stores to flash; NORMAL is 13 and
      // prints its tables as source (no flash slot for 13-wide waveforms yet).
      // N = levels only. SAVE/SKIP is per RUN, not per phase, so a direct
      // phase that fails takes the levels down with it however good they
      // were — which is exactly what happened to the first set measured
      // against the corrected dose ladder. Separating them means a good
      // half can be kept while the other half is still being worked on.
      Serial.println("[tuneup] 4-grey tune: f = FAST, n = NORMAL (levels+directs), "
                     "N = NORMAL levels only, q = cancel");
      uint32_t t0 = millis();
      while (millis() - t0 < 30000) {
        if (!Serial.available()) { delay(20); continue; }
        const int k = Serial.read();
        if (k == 'q') break;
        if (k == 'f') { runSelfTuneFast(EPD_Painter::Quality::QUALITY_FAST); break; }
        if (k == 'n') { runSelfTuneFast(EPD_Painter::Quality::QUALITY_NORMAL); break; }
        if (k == 'N') { runSelfTuneFast(EPD_Painter::Quality::QUALITY_NORMAL,
                                        true, false); break; }
      }
      showMenuCard();
    } break;
    case 'd': {
      // Directs only, at NORMAL: re-measure the six grey-to-grey trains
      // against the levels already loaded, without disturbing them.
      runSelfTuneFast(EPD_Painter::Quality::QUALITY_NORMAL, false, true);
      showMenuCard();
    } break;
    case 'e': {
      // Print every stored set as source for src/EPD_Painter_trains.h. The
      // board that was measured becomes the board every untuned one inherits
      // from, so this is the last step of a tune rather than a debug aid.
      Serial.println("// ===== paste into src/EPD_Painter_trains.h =====");
      for (int s = 0; s <= (int)SET_HIGH; s++)
        tunedExport(epd.driver(), (Set)s, TUNED_EXPORT_PREFIX);
      Serial.println("// ===== end =====");
    } break;
    case 'X':
      tunedEraseAll();
      Serial.println("[tuneup] all tuned blobs erased - reboot ('r') for preset tables");
      break;
    case 'E': {
      // E [gens]  evolutionary tune (HIGH). Mutation-only search over the
      // raw drive sequence, 60 candidates per scan.
      int gens = Serial.parseInt();
      if (gens < 1) gens = evoGenerations;
      if (gens > 200) gens = 200;
      runEvoTune(EPD_Painter::Quality::QUALITY_HIGH, gens);
      showMenuCard();
    } break;
    case 'A': {
      // A 0|1 - alternate-slot mode: even slots pinned float.
      evoSettleMask = (Serial.parseInt() != 0);
      Serial.printf("[tuneup] settle mask: %s\n",
                    evoSettleMask ? "ON" : "off");
    } break;
    case 'R': {
      // R 0|1 - patch position randomisation off/on.
      evoRandomPos = (Serial.parseInt() != 0);
      Serial.printf("[tuneup] patch positions: %s\n",
                    evoRandomPos ? "shuffled" : "fixed");
    } break;
    case 'B': {
      // Stability sweep: which (row charge, pass period) holds a grey
      // steadiest across position and surrounding content.
      runStabilitySweep();
      showMenuCard();
    } break;
    case 'Q': {
      // Paint the registration quadrant mark and hold, so it can be scanned
      // independently when registration fails.
      epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
      epd.driver().setGreyLevels(16);
      tunePaintQuadrant();
      tuneClear();
      tunePaint();
      Serial.println("[tuneup] quadrant mark painted and held");
    } break;
    case 'f': {
      // The splash at FAST, 4 greys. Four levels are FAINT - that is the
      // mode, not a fault - but faint is not the same as wrong, and this is
      // the check that tells the two apart: the photograph should read as a
      // low-contrast photograph, not as a relief carving with the mid-tones
      // wrapped around to white.
      epd.driver().setGreyLevels(4);          // levels first: FAST is refused at 16
      epd.setQuality(EPD_Painter::Quality::QUALITY_FAST);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      const uint8_t *l4 = epd.driver()._config.levelLum(4);
      Serial.printf("[tuneup] splash painted at FAST, %d levels, curve %s",
                    epd.driver().greyLevels(), l4 ? "" : "MISSING (even ramp assumed)");
      if (l4) for (int i = 0; i < 4; i++) Serial.printf("%d ", l4[i]);
      Serial.println();
    } break;
    case 'g': {
      // Same, at NORMAL - for comparing the two qualities on identical
      // content. 13 slots against HIGH's 20.
      epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      Serial.printf("[tuneup] splash painted at NORMAL (%d passes, %d us, "
                    "row_extra %d us)\n",
                    epd.driver().g16TrainLen(),
                    epd.driver()._config.g16_pass_us_normal,
                    epd.driver()._config.rowExtraUs());
    } break;
    case 'N': {
      // N <n> — generations for the 16-grey evolutionary tune. The default
      // 30 gives each level only 30x3 = 90 candidate trials against a 13-
      // or 20-slot genome, and the H716's 30 Jul run left individual
      // levels visibly stuck (one column held -13 for a dozen
      // generations). Raise it when a run has to move every level, e.g.
      // after the reference ladder is re-spaced.
      const int n = Serial.parseInt();
      if (n >= 1 && n <= 200) evoGenerations = n;
      Serial.printf("[tuneup] evolution generations: %d\n", evoGenerations);
    } break;
    case 'Z': {
      // Z <2|3> — the ghost exponent in evoPairFitness. 2 = square
      // (default, see tuner_evo.h), 3 = the 29 Jul ghost-cubed weighting.
      const int n = Serial.parseInt();
      if (n == 2 || n == 3) evoGhostPow = (float)n;
      Serial.printf("[tuneup] ghost exponent: %.0f (%s)\n", evoGhostPow,
                    evoGhostPow >= 3.0f ? "cubed" : "square");
    } break;
    case 'u': {
      // u <us> — live row-charge override, both 16-grey tiers: the extra
      // hold after LE before the next row (Tony's capacitor-recharge
      // probe, 29 Jul). Survives until reboot or a blob reinstall. At 540
      // rows every microsecond costs 0.54 ms per pass; past the pass
      // period's slack the period itself stretches (watch the jitter).
      const int us = Serial.parseInt();
      epd.driver()._config.row_extra_us_normal = us;
      epd.driver()._config.row_extra_us_high   = us;
      Serial.printf("[tuneup] row charge override: %d us per row (16-grey)\n", us);
    } break;
    case 'v': {
      // v [3] — photo + staircase, serial-driven; 3 selects HIGH, anything
      // else NORMAL. The third leg of the context probe ('w'/'b' are the
      // others): same patches, same place, with the photograph above them
      // instead of a flat field. This is the measurement that decides
      // whether a staircase the tuner verified MONOTONIC ON ITS OWN stays
      // monotonic once real content shares the frame — i.e. whether the
      // card-vs-staircase drift the verdict prints is a rendering-context
      // effect rather than a defect in the table.
      const int q = Serial.parseInt();
      epd.setQuality(q == 3 ? EPD_Painter::Quality::QUALITY_HIGH
                            : EPD_Painter::Quality::QUALITY_NORMAL);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      const int W = epd.width(), H = epd.height();
      const int barH = 64;
      epd.fillRect(0, H - barH - 2, W, 2, 15);
      for (int i = 0; i < 16; i++) {
        const int x0 = (i * W) / 16, x1 = ((i + 1) * W) / 16;
        epd.fillRect(x0, H - barH, x1 - x0, barH, (uint8_t)i);
      }
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      Serial.printf("[tuneup] photo + staircase at %s - held, 'm' for menu\n",
                    q == 3 ? "HIGH" : "NORMAL");
    } break;
    case 'w': case 'b': {
      // The context-drift probe: the same sixteen native patches the photo
      // screen draws, in the same place, over nothing but white ('w') or
      // black ('b'). If the staircase only scrambles with the photo above
      // it, the drive is content-coupled, not mistuned.
      const uint8_t bg = (c == 'b') ? 15 : 0;
      epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      const int W = epd.width(), H = epd.height();
      memset(epd.getBuffer(), bg, (size_t)W * H);
      const int barH = 64;
      for (int i = 0; i < 16; i++) {
        const int x0 = (i * W) / 16, x1 = ((i + 1) * W) / 16;
        epd.fillRect(x0, H - barH, x1 - x0, barH, (uint8_t)i);
      }
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      Serial.printf("[tuneup] staircase over %s at NORMAL - held, 'm' for menu\n",
                    bg ? "black" : "white");
    } break;
    case 'j': {
      // The splash QUANTISED ONLY — nearest level, no error diffusion.
      // Clean-but-posterised here convicts the dither; surviving streaks
      // convict the levels.
      splashNoDither = true;
      epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      splashNoDither = false;
      Serial.println("[tuneup] splash painted at NORMAL, quantise-only (no dither)");
    } break;
    case 'h': {
      // The splash at NORMAL, dithered from every OTHER grey — 8 of 16.
      // Wider steps mean a level's context drift is half the fraction of
      // a step, so this trades residual banding for extra grain. A test
      // of where the banding/grain optimum sits, not a mode.
      splashStride = 2;
      epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      splashStride = 1;
      Serial.println("[tuneup] splash painted at NORMAL from 8 of 16 greys");
    } break;
    case 'G': {
      // Paint the splash artwork at HIGH, 16 greys. A visual check of the
      // tuned trains on real photographic content rather than on a test
      // card - which is the content that exposed the pass-budget overrun,
      // so it is the one worth looking at.
      epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
      epd.driver().setGreyLevels(16);
      epd.clear();
      while (!epd.driver().paintIdle()) delay(10);
      tuneupSplashDraw(epd.getBuffer(), epd.width(), epd.height());
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      Serial.printf("[tuneup] splash painted at HIGH (%d passes, %d us, "
                    "row_extra %d us)\n",
                    epd.driver().g16TrainLen(),
                    epd.driver()._config.g16_pass_us_high,
                    epd.driver()._config.rowExtraUs());
    } break;
    case 'W': {
      // W <us> - extra per-row charging time. Tests whether the horizontal
      // gradient is transient charging (more time helps) or steady-state
      // rail droop (it does not).
      const int us = (int)Serial.parseInt();
      const int v = constrain(us, 0, 40);
      epd.driver()._config.row_extra_us_normal = v;
      epd.driver()._config.row_extra_us_high   = v;
      Serial.printf("[tuneup] row charge = %d us both qualities (adds %.1f ms "
                    "per pass over %d rows)\n", v,
                    v * epd.height() / 1000.0f, epd.height());
    } break;
    case 'U': {
      // Positional uniformity: driven vs dithered at the same density.
      static const int LV[] = { 4, 8, 12 };
      runUniformityProbe(EPD_Painter::Quality::QUALITY_HIGH, LV, 3);
      showMenuCard();
    } break;
    case 'S': {
      // Staircase check on the INSTALLED tables. Independent of the tuner:
      // full-height solid bars, which is the worst case for sweeps per row
      // and therefore the content most likely to expose a dose problem.
      runStaircaseCheck(EPD_Painter::Quality::QUALITY_HIGH);
      showMenuCard();
    } break;
    case 'D': {
      // Dose-ladder sweep: pick the pass period from measurement.
      //
      // Swept around THIS board's own periods, not a fixed list. The list was
      // 19/15/13 ms, which are the LilyGo's numbers: on a board whose preset
      // asks for 20 and 24 they are all below the honest floor, so every pass
      // would overrun and the ladder would measure the clamp rather than the
      // panel. HIGH's period, NORMAL's period, and one step under NORMAL —
      // the first two are the candidates actually in play (matching them lets
      // a HIGH train inherit a NORMAL one slot-for-slot), the third asks
      // whether there is headroom below.
      const int pn = epd.driver()._config.g16_pass_us_normal;
      const int ph = epd.driver()._config.g16_pass_us_high;
      int periods[3], n = 0;
      periods[n++] = ph;
      if (pn != ph) periods[n++] = pn;
      const int lower = pn - 2000;
      if (lower >= 10000 && lower != ph) periods[n++] = lower;
      Serial.printf("[tuneup] dose ladder over %d periods:", n);
      for (int i = 0; i < n; i++) Serial.printf(" %d", periods[i]);
      Serial.println(" us");
      runDoseSweep(periods, n);
      showMenuCard();
    } break;
    case 'K': {
      // Compensation sweep: cells fixed, whole-panel background varied,
      // period swept. Yields lum-per-ms and the ms that cancels a
      // background shift — the coefficient a renderer would need.
      runCompSweep(COMP_KNOB_PERIOD);
      showMenuCard();
    } break;
    case 'k': {
      // Same sweep, but varying row charge time instead of pass period.
      runCompSweep(COMP_KNOB_ROW);
      showMenuCard();
    } break;
    case 'l': {
      // LE pulse width, with row charge forced to 0: does a properly held
      // latch remove the content dependence on its own?
      runCompSweep(COMP_KNOB_LE);
      showMenuCard();
    } break;
    case 'F': {
      // F 0|1 - reference-ladder correction (see evoRefLadderFix).
      evoRefLadderFix = (Serial.parseInt() != 0);
      Serial.printf("[tuneup] reference ladder correction: %s\n",
                    evoRefLadderFix ? "ON" : "off (plain density)");
    } break;
    case 'J': benchLumaScan(); benchLeHold(); break;
    case 'V': tunePreviewResults(); showMenuCard(); break;
    case 'C': {
      // Paint the tuner's own match card and leave it up, so the levels
      // it claims to have matched can be measured independently of the
      // tuner's own geometry and arithmetic.
      tuneClear();
      tunePaintMatchCard();
      epd.paint();
      while (!epd.driver().paintIdle()) delay(10);
      Serial.println("[tuneup] match card painted (top = dither refs, "
                     "bottom = native levels)");
    } break;
    case 'c': {
      // Re-measure the level curve on the staircase and store it into the
      // NVS blob, for each 16-grey quality that has one. Trains untouched.
      runStaircaseCurveStore(EPD_Painter::Quality::QUALITY_NORMAL);
      runStaircaseCurveStore(EPD_Painter::Quality::QUALITY_HIGH);
      showMenuCard();
    } break;
    case 'p': {
      // Photo-context curve: patches overlaid on the splash artwork at
      // randomised positions, 4 rounds, averaged — stored into the blob.
      runPhotoCurveStore(EPD_Painter::Quality::QUALITY_NORMAL);
      runPhotoCurveStore(EPD_Painter::Quality::QUALITY_HIGH);
      showMenuCard();
    } break;
    case 'n': pickScanner(); showMenuCard(); break;
    case 'm': showMenuCard(); break;
    case 'r': Serial.println("rebooting"); delay(300); ESP.restart(); break;
    default: break;
  }
}

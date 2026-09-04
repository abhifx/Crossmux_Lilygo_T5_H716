#include "EpdPainterDriver.h"
#include <BoardConfig.h>

#if FREEINK_DRIVER_EPD_PAINTER
#include <EPD_Painter.h>
#include <EPD_Painter_presets.h>
#include <esp_heap_caps.h>
#include <cstring>

namespace freeink {
namespace {

EPD_Painter* g_painter = nullptr;
uint8_t* g_canvas = nullptr;
uint8_t* g_lsb = nullptr;
uint8_t* g_msb = nullptr;
uint16_t g_w = 960, g_h = 540, g_wb = 120;
uint32_t g_partialCount = 0;

void allocBuffers(uint16_t w, uint16_t h) {
  g_w = w;
  g_h = h;
  g_wb = w / 8;
  const size_t canvasBytes = static_cast<size_t>(w) * h;
  const size_t planeBytes = static_cast<size_t>(g_wb) * h;
  if (!g_canvas) {
    g_canvas = static_cast<uint8_t*>(heap_caps_malloc(canvasBytes, MALLOC_CAP_SPIRAM));
    if (g_canvas) memset(g_canvas, 0x00, canvasBytes);
  }
  if (!g_lsb) {
    g_lsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
    if (g_lsb) memset(g_lsb, 0xFF, planeBytes);
  }
  if (!g_msb) {
    g_msb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
    if (g_msb) memset(g_msb, 0xFF, planeBytes);
  }
}

// In 4-level mode: 0 = White, 1 = Light Gray, 2 = Dark Gray, 3 = Black
static uint64_t lutBW[256];
static bool lutInitialized = false;

static void initLut() {
  if (lutInitialized) return;
  for (int i = 0; i < 256; i++) {
    uint8_t bytes[8];
    for (int bit = 0; bit < 8; bit++) {
      // 0 in fb is Ink (Black = 3), 1 in fb is Paper (White = 0)
      bool ink = (i & (0x80 >> bit)) == 0;
      bytes[bit] = ink ? 3 : 0;
    }
    memcpy(&lutBW[i], bytes, 8);
  }
  lutInitialized = true;
}

void fillCanvasBW(const uint8_t* fb) {
  if (!g_canvas || !fb) return;
  initLut();

  uint64_t* canvas64 = reinterpret_cast<uint64_t*>(g_canvas);
  const uint32_t bufferSize = static_cast<uint32_t>(g_wb) * g_h;
  uint32_t i = 0;
  for (; i + 3 < bufferSize; i += 4) {
    canvas64[i]     = lutBW[fb[i]];
    canvas64[i + 1] = lutBW[fb[i + 1]];
    canvas64[i + 2] = lutBW[fb[i + 2]];
    canvas64[i + 3] = lutBW[fb[i + 3]];
  }
  for (; i < bufferSize; i++) {
    canvas64[i] = lutBW[fb[i]];
  }
}

}  // namespace

EpdPainterDriver::EpdPainterDriver() {}
EpdPainterDriver::~EpdPainterDriver() {}

PanelGeometry EpdPainterDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth ? BoardConfig::ACTIVE.displayWidth : 960;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight ? BoardConfig::ACTIVE.displayHeight : 540;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

void EpdPainterDriver::begin(EpdBus& bus) {
  (void)bus;
  const uint16_t w = BoardConfig::ACTIVE.displayWidth ? BoardConfig::ACTIVE.displayWidth : 960;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight ? BoardConfig::ACTIVE.displayHeight : 540;
  allocBuffers(w, h);

  if (!g_painter) {
#if defined(EPD_PAINTER_PRESET_LILYGO_EPD47_H716)
    static EPD_Painter painter(EPD_LILYGO_EPD47_H716_PRESET);
#elif defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_H752)
    static EPD_Painter painter(EPD_LILYGO_T5_S3_H752_PRESET);
#elif defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS)
    static EPD_Painter painter(EPD_LILYGO_T5_S3_GPS_PRESET);
#else
    static EPD_Painter painter(EPD_PAINTER_PRESET);
#endif
    g_painter = &painter;
    g_painter->setAutoShutdown(false);
    g_painter->begin();
    g_painter->setGreyLevels(4);
    g_painter->clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
    g_partialCount = 0;
  }
}

void EpdPainterDriver::deepSleep(EpdBus& bus) {
  (void)bus;
  if (g_painter) {
    // Do NOT clear screen to white on deep sleep — keep the sleep cover/wallpaper
    // image visible on the E-Paper glass throughout deep sleep.
    g_painter->end();
  }
}

void EpdPainterDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
  if (!g_painter || !fb) return;
  fillCanvasBW(fb);

  static uint16_t consecutiveFastRefreshes = 0;
  bool doHardwareClear = (mode == RefreshMode::Full || mode == RefreshMode::Half);

  if (mode == RefreshMode::Fast) {
    consecutiveFastRefreshes++;
    if (consecutiveFastRefreshes >= 6) {
      doHardwareClear = true;
      consecutiveFastRefreshes = 0;
    }
  } else {
    consecutiveFastRefreshes = 0;
  }

  if (doHardwareClear) {
    g_painter->clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
    g_painter->setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  } else {
    g_painter->setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
  }

  g_painter->paint(g_canvas);

  if (turnOff) {
    g_painter->end();
  }
}

void EpdPainterDriver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                   uint16_t w, uint16_t h, bool turnOff) {
  (void)bus;
  (void)prev;
  if (!g_painter || !fb) return;
  fillCanvasBW(fb);

  if (w > 0 && h > 0 && w <= g_w && h <= g_h) {
    EPD_Painter::Rect windowRect = {
        static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
    g_painter->clear(&windowRect, 1, EPD_Painter::ClearMode::SOFT);
  }

  g_painter->setQuality(EPD_Painter::Quality::QUALITY_FAST);
  g_painter->paint(g_canvas);

  if (turnOff) {
    g_painter->end();
  }
}

void EpdPainterDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (g_lsb && lsb) memcpy(g_lsb, lsb, static_cast<size_t>(g_wb) * g_h);
}

void EpdPainterDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (g_msb && msb) memcpy(g_msb, msb, static_cast<size_t>(g_wb) * g_h);
}

void EpdPainterDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  (void)bus;
  uint8_t* dstPlane = (plane == GrayPlane::Lsb) ? g_lsb : g_msb;
  if (!dstPlane || !rows) return;
  const uint32_t offset = static_cast<uint32_t>(yStart) * g_wb;
  memcpy(dstPlane + offset, rows, static_cast<size_t>(numRows) * g_wb);
}

void EpdPainterDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)bus;
  if (!g_painter) return;

  if (g_lsb && g_msb && g_canvas) {
    const uint32_t bufferSize = static_cast<uint32_t>(g_wb) * g_h;
    for (uint32_t i = 0; i < bufferSize; i++) {
      uint8_t lsb = g_lsb[i];
      uint8_t msb = g_msb[i];
      for (int bit = 0; bit < 8; bit++) {
        bool lbit = (lsb & (0x80 >> bit)) == 0;
        bool mbit = (msb & (0x80 >> bit)) == 0;

        uint8_t val = 0;
        if (mbit && lbit) val = 3;       // Black
        else if (mbit) val = 2;          // Dark Gray
        else if (lbit) val = 1;          // Light Gray
                                         // else val = 0 (White)

        g_canvas[i * 8 + bit] = val;
      }
    }
  } else if (fb) {
    fillCanvasBW(fb);
  }

  // Use QUALITY_HIGH for wallpapers/covers, QUALITY_NORMAL otherwise
  g_painter->setQuality((factoryMode || lut != nullptr) ? EPD_Painter::Quality::QUALITY_HIGH
                                                        : EPD_Painter::Quality::QUALITY_NORMAL);
  g_painter->paint(g_canvas);

  if (turnOff) {
    g_painter->end();
  }
}

void EpdPainterDriver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bus;
  if (bw) fillCanvasBW(bw);
  const size_t planeBytes = static_cast<size_t>(g_wb) * g_h;
  if (g_lsb) memset(g_lsb, 0xFF, planeBytes);
  if (g_msb) memset(g_msb, 0xFF, planeBytes);
}

PanelDriver& epdPainterDriver() {
  static EpdPainterDriver instance;
  return instance;
}

}  // namespace freeink
#else
namespace freeink {
EpdPainterDriver::EpdPainterDriver() {}
EpdPainterDriver::~EpdPainterDriver() {}
PanelGeometry EpdPainterDriver::geometry() const { return {960, 540, 120, 64800}; }
void EpdPainterDriver::begin(EpdBus& bus) { (void)bus; }
void EpdPainterDriver::deepSleep(EpdBus& bus) { (void)bus; }
void EpdPainterDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus; (void)fb; (void)prev; (void)mode; (void)turnOff;
}
void EpdPainterDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) { (void)bus; (void)lsb; }
void EpdPainterDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) { (void)bus; (void)msb; }
void EpdPainterDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  (void)bus; (void)plane; (void)rows; (void)yStart; (void)numRows;
}
void EpdPainterDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) {
  (void)bus; (void)fb; (void)turnOff; (void)lut; (void)factoryMode;
}
void EpdPainterDriver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) { (void)bus; (void)bw; }

PanelDriver& epdPainterDriver() {
  static EpdPainterDriver instance;
  return instance;
}
}  // namespace freeink
#endif

#include "ActivityUtils.h"
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

namespace ActivityUtils {

void applyColorDepth(GfxRenderer& renderer, const uint8_t depth, const uint8_t dither) {
  uint8_t bits = 4;
  switch (depth) {
    case CrossPointSettings::COLOR_DEPTH::BIT_1:
      bits = 1;
      break;
    case CrossPointSettings::COLOR_DEPTH::BIT_2:
      bits = 2;
      break;
    case CrossPointSettings::COLOR_DEPTH::BIT_4:
      bits = 4;
      break;
    default:
      break;
  }
  renderer.setColorDepth(bits);
  renderer.setDitherMode(dither);
  LOG_INF("ACT", "Applied color depth: %d-bit dither: %d (enum=%d)", bits, dither, depth);
}

void prepareRender(GfxRenderer& renderer) {
  if (renderer.supportsGrayscale8Bit()) {
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_8BIT);
  }
  renderer.clearScreen(0xFF);
}

void finishRender(GfxRenderer& renderer, HalDisplay::RefreshMode mode) {
  if (renderer.getRenderMode() == GfxRenderer::GRAYSCALE_8BIT) {
    renderer.displayGray8Bit(renderer.getInternalGrayBuffer(), mode);
    renderer.setRenderMode(GfxRenderer::BW);
  } else {
    renderer.displayBuffer(mode);
  }
}

bool renderGrayscale8Bit(GfxRenderer& renderer, const char* logTag, std::function<void()> renderFunc, HalDisplay::RefreshMode mode) {
  if (!renderer.supportsGrayscale8Bit()) {
    return false;
  }

  // Optimized path for S3/PSRAM: Render everything in a single pass to the 8-bit buffer.
  // This avoids the 7x overhead of strip-based re-rendering.
  renderer.waitRefreshComplete();
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_8BIT);
  renderer.clearScreen(0xFF); // Clear to white

  const auto tStart = millis();
  renderFunc();
  const auto tRender = millis();

  renderer.setRenderMode(GfxRenderer::BW);
  // displayGray8Bit will handle the quantization (1/2/4 bit) and sending to hardware
  renderer.displayGray8Bit(renderer.getInternalGrayBuffer(), mode);
  const auto tDisplayEnd = millis();

  renderer.cleanupGrayscaleWithFrameBuffer();

  LOG_INF(logTag, "Page render (8-bit full): render=%lums display=%lums mode=%d total=%lums",
          tRender - tStart, tDisplayEnd - tRender, static_cast<int>(mode), tDisplayEnd - tStart);

  return true;
}

}  // namespace ActivityUtils

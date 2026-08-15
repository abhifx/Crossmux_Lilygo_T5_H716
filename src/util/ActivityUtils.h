#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <functional>

/**
 * Utility functions for Activities to handle board-specific display logic
 * while keeping core Activity code close to upstream.
 */
namespace ActivityUtils {

/**
 * Configure renderer color depth based on user settings.
 */
void applyColorDepth(GfxRenderer& renderer, uint8_t depth, uint8_t dither = 1);

/**
 * Standard setup for a rendering frame. Handles 8-bit mode for H716.
 */
void prepareRender(GfxRenderer& renderer);

/**
 * Standard display call for a completed frame.
 */
void finishRender(GfxRenderer& renderer, HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH);

/**
 * High-performance 8-bit grayscale rendering for H716-style panels.
 * Renders the page in horizontal strips to save RAM.
 * Returns true if the 8-bit path was used and handled, false if it should fall back to 2-bit planes.
 */
bool renderGrayscale8Bit(GfxRenderer& renderer, const char* logTag, std::function<void()> renderFunc, HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH);

}  // namespace ActivityUtils

#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include <DitherUtils.h>

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight, uint8_t& cachedBpp) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Version/Bpp byte added in 1.5.1
  if (cacheFile.read(&cachedBpp, 1) != 1) {
    cachedBpp = 2;  // Legacy 2-bit cache
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth * cachedBpp + 7) / 8;
  const size_t expectedSize = 5 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
#if defined(BOARD_HAS_PSRAM)
constexpr size_t PXC_MAX_CHUNKS = 48;  // 768 KB: covers full-page 8bpp H716 images (518 KB)
#else
constexpr size_t PXC_MAX_CHUNKS = 6;   // 96 KB: a full-screen 2bpp image
#endif
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 960; // Support H716 full width

std::unique_ptr<uint8_t[], void (*)(void*)> pxcChunks[PXC_MAX_CHUNKS] = {
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }},
    {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}, {nullptr, [](void* p) { free(p); }}
};
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;
uint8_t pxcSlotBpp = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
  pxcSlotBpp = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, uint8_t cachedBpp,
                 int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return false;
  }
  size_t remaining = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
#if defined(BOARD_HAS_PSRAM)
    // On S3, we have plenty of SPIRAM, use it first.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)malloc(want);
#else
    if (ESP.getFreeHeap() < remaining + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      releasePxcSlot();
      return false;
    }
    uint8_t* buf = (uint8_t*)malloc(want);
#endif
    pxcChunks[i].reset(buf);
    if (!pxcChunks[i] || cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  pxcSlotBpp = cachedBpp;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth * pxcSlotBpp + 7) / 8;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotHeight; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      uint8_t pixelValue;
      if (pxcSlotBpp == 8) {
        pixelValue = rowBuffer[col];
        if (renderer.getRenderMode() != GfxRenderer::GRAYSCALE_8BIT) {
          // Dither 8-bit raw cache down to 2-bit (0..3) for legacy B/W or plane modes.
          // This provides gradients even when the cache is high-quality raw grayscale.
          pixelValue = applyBayerDither4Level(pixelValue, x + col, y + row);
        }
      } else {
        const int byteIdx = col >> 2;            // col / 4
        const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
        pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
        if (renderer.getRenderMode() == GfxRenderer::GRAYSCALE_8BIT) {
          pixelValue *= 85;  // Expand 0..3 to 0..255
        }
      }
      pw.writePixel(x + col, pixelValue);
    }
    if (row % 100 == 0) yield();
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, const ImageBlock::PixelCachePolicy cachePolicy) {
  const bool hardwareSupportsGrayscale = renderer.supportsGrayscale8Bit();
  const uint8_t depth = renderer.getColorDepth();
  // On high-depth hardware, always prefer 8-bit cache to preserve quality.
  // On standard hardware, use 2-bit cache to save space.
  const uint8_t requiredBpp = hardwareSupportsGrayscale ? 8 : 2;

  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (cachePolicy == ImageBlock::PixelCachePolicy::LoadIntoRam && pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    if (pxcSlotBpp >= requiredBpp) {
      renderRowsFromPxcSlot(renderer, x, y);
      return true;
    }
    // Slot has lower quality cache, need to reload/redecode
    releasePxcSlot();
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  uint8_t cachedBpp;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight, cachedBpp)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // If we need 8-bit but only have 2-bit, reject the cache so we re-decode.
  if (requiredBpp == 8 && cachedBpp < 8) {
    LOG_INF("IMG", "Rejecting low-depth cache (have %d, want %d)", cachedBpp, requiredBpp);
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d, %d-bit)", cachePath.c_str(), cachedWidth, cachedHeight, cachedBpp);

  const int bytesPerRow = (cachedWidth * cachedBpp + 7) / 8;

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (cachePolicy == ImageBlock::PixelCachePolicy::LoadIntoRam && pxcSlotHash == 0 &&
      loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, cachedBpp, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y);
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    return true;
  }

  // Streaming fallback (slot didn't fit). A failed slot load may have consumed
  // part of the payload; rewind to just past the header.
  cacheFile.seek(5);

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  auto readBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer.get(), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer.get() + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      uint8_t pixelValue;
      if (cachedBpp == 8) {
        pixelValue = rowBuffer[col];
        if (renderer.getRenderMode() != GfxRenderer::GRAYSCALE_8BIT) {
          // Dither 8-bit raw cache down to 2-bit (0..3) for legacy B/W or plane modes.
          pixelValue = applyBayerDither4Level(pixelValue, x + col, destY);
        }
      } else {
        const int byteIdx = col >> 2;            // col / 4
        const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
        pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
        if (renderer.getRenderMode() == GfxRenderer::GRAYSCALE_8BIT) {
          pixelValue *= 85;  // Expand 0..3 to 0..255
        }
      }

      pw.writePixel(x + col, pixelValue);
    }
    if (row % 100 == 0) yield();
  }

  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  uint8_t cachedBpp;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight, cachedBpp);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  (void)render(renderer, x, y, PixelCachePolicy::LoadIntoRam);
}

bool ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const PixelCachePolicy cachePolicy) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return true;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return false;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return true;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return false;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height, cachePolicy)) {
    return true;
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
    if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  size_t fileSize = 0;
  {
    HalFile file;
    if (!Storage.openFileForRead("IMG", imagePath, file)) {
      LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      renderPlaceholder(renderer, x, y);
      return false;
    }
    fileSize = file.size();
  }

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  // Disable dithering when rendering for high-depth hardware (8-bit pipeline).
  // Dithering is used to simulate more levels on low-depth (1-bit/2-bit) panels.
  config.useDithering = !renderer.supportsGrayscale8Bit();
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Decode successful");
  return true;
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
}

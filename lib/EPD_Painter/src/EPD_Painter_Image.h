// ============================================================================
// EPD_Painter_Image.h — optional JPEG / PNG loading.
//
// THE LIBRARY DOES NOT DEPEND ON A DECODER. EPD_Painter::drawGray8() takes
// 8bpp greyscale and knows nothing about file formats; this header is the
// convenience layer for people who happen to have a decoder installed, and
// including it changes nothing unless you ask for one.
//
// Switch a decoder on with an explicit define BEFORE including this header:
//
//   #define EPD_PAINTER_HAVE_JPEGDEC 1     // https://github.com/bitbank2/JPEGDEC
//   #define EPD_PAINTER_HAVE_PNGDEC  1     // https://github.com/bitbank2/PNGdec
//   #include "EPD_Painter_Image.h"
//
// Explicit switches rather than __has_include, and that is deliberate: the
// Arduino builder only adds a library to the include path when it sees an
// UNCONDITIONAL #include somewhere in the sketch, so __has_include is false
// on the first pass and the feature silently never compiles in. The tuneup
// example learned this the hard way and uses the same pattern.
//
// Usage, once a decoder is enabled:
//
//   EPD_PainterImage::drawJpegFit(epd, bytes, len);                  // fit
//   EPD_PainterImage::drawJpegFit(epd, bytes, len, JPEG_SCALE_HALF); // + shrink
//   EPD_PainterImage::drawPngFit(epd, bytes, len);
//
// These STREAM. The image is dithered row by row as it decodes, so the decoded
// picture never has to exist in memory - a 1600x1200 photo costs a ~26 KB
// strip buffer instead of ~1.9 MB. Everything goes through
// EPD_Painter::GrayDither, so images quantise against the panel's MEASURED
// level curve when a tuned blob is installed rather than an assumed even
// ramp - which is the difference between grain and banding.
// ============================================================================

#pragma once

#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "EPD_Painter_Adafruit.h"

#if EPD_PAINTER_HAVE_JPEGDEC
  #include <JPEGDEC.h>
#endif
#if EPD_PAINTER_HAVE_PNGDEC
  #include <PNGdec.h>
#endif

namespace EPD_PainterImage {

// ---- streaming decode ------------------------------------------------------
// JPEGDEC hands back MCU blocks, not whole rows: a strip of the image arrives
// as several left-to-right blocks. The ditherer needs COMPLETE rows in order,
// so blocks are assembled into a strip buffer and flushed row by row once the
// strip is full.
//
// That strip is the only image memory in play - srcW x 16 bytes, ~26 KB for a
// 1600-wide photo, against ~1.9 MB to hold the decode. The panel framebuffer
// is written directly as rows are dithered.
struct Stream {
  EPD_Painter::GrayDither dither;
  uint8_t *strip = nullptr;     // srcW x stripH assembly buffer
  int srcW = 0, stripH = 0;
  int stripY = -1;              // source y of strip row 0
  bool ok = false;
};
inline Stream &stream() { static Stream s; return s; }

inline void flushStrip(int rows) {
  Stream &st = stream();
  for (int r = 0; r < rows; r++)
    st.dither.row(st.strip + (size_t)r * st.srcW, st.stripY + r);
}

#if EPD_PAINTER_HAVE_JPEGDEC
inline int jpegRowCb(JPEGDRAW *d) {
  Stream &st = stream();
  if (!st.ok) return 0;

  // A new strip: flush the previous one first. Blocks within a strip share
  // the same y, so this fires once per strip rather than once per block.
  if (d->y != st.stripY) {
    if (st.stripY >= 0) flushStrip(st.stripH);
    st.stripY = d->y;
    memset(st.strip, 255, (size_t)st.srcW * st.stripH);
    st.stripH = d->iHeight;
  }
  for (int y = 0; y < d->iHeight && y < st.stripH; y++) {
    uint8_t *drow = st.strip + (size_t)y * st.srcW;
    const uint8_t *srow = (const uint8_t *)d->pPixels + (size_t)y * d->iWidth;
    for (int x = 0; x < d->iWidth; x++) {
      const int tx = d->x + x;
      if (tx >= 0 && tx < st.srcW) drow[tx] = srow[x];
    }
  }
  return 1;
}

// Decode a JPEG from memory straight onto the panel, dithered against the
// measured level curve, scaled to fit and centred. Never allocates the
// decoded image.
//
// Returns false if the image cannot be opened or the strip buffer cannot be
// had. scaleOpt takes JPEG_SCALE_HALF / QUARTER / EIGHTH to let the decoder
// shrink large photos as it goes - cheaper than scaling afterwards.
inline bool drawJpegFit(EPD_PainterAdafruit &epd, const uint8_t *data, size_t len,
                        int scaleOpt = -1) {
  static JPEGDEC dec;              // several KB of internal buffers: a stack
                                   // instance overflows the loop task on S3
  Stream &st = stream();
  st.ok = false; st.stripY = -1;

  if (!dec.openRAM((uint8_t *)data, (int)len, jpegRowCb)) return false;
  dec.setPixelType(EIGHT_BIT_GRAYSCALE);

  const int W = epd.width(), H = epd.height();
  int sw = dec.getWidth(), sh = dec.getHeight();

  // scaleOpt < 0 means "choose for me": take the largest built-in shrink that
  // still leaves the image at least panel-sized. A phone photo carries several
  // times more detail than the panel can show, and decoding it only to throw
  // it away costs time for nothing. Decoding SMALLER than the panel would
  // cost quality, so the test is >= rather than nearest.
  if (scaleOpt < 0) {
    scaleOpt = 0;
    if      (sw >= W * 8 && sh >= H * 8) scaleOpt = JPEG_SCALE_EIGHTH;
    else if (sw >= W * 4 && sh >= H * 4) scaleOpt = JPEG_SCALE_QUARTER;
    else if (sw >= W * 2 && sh >= H * 2) scaleOpt = JPEG_SCALE_HALF;
  }
  if (scaleOpt == JPEG_SCALE_HALF)    { sw >>= 1; sh >>= 1; }
  if (scaleOpt == JPEG_SCALE_QUARTER) { sw >>= 2; sh >>= 2; }
  if (scaleOpt == JPEG_SCALE_EIGHTH)  { sw >>= 3; sh >>= 3; }
  if (sw <= 0 || sh <= 0) { dec.close(); return false; }

  const float s = (W / (float)sw < H / (float)sh) ? W / (float)sw : H / (float)sh;
  const int dw = (int)(sw * s) > 0 ? (int)(sw * s) : 1;
  const int dh = (int)(sh * s) > 0 ? (int)(sh * s) : 1;

  memset(epd.getBuffer(), 0, (size_t)W * H);        // level 0 = paper white

  st.srcW = sw;
  st.stripH = 16;                                    // JPEG MCUs are <= 16 tall
  st.strip = (uint8_t *)heap_caps_malloc((size_t)sw * st.stripH, MALLOC_CAP_SPIRAM);
  if (!st.strip)
    st.strip = (uint8_t *)heap_caps_malloc((size_t)sw * st.stripH, MALLOC_CAP_8BIT);
  if (!st.strip) { dec.close(); return false; }

  st.ok = st.dither.begin(epd.driver(), epd.getBuffer(), W, H, sw, sh,
                          (W - dw) / 2, (H - dh) / 2, dw, dh);
  int rc = 0;
  if (st.ok) {
    rc = dec.decode(0, 0, scaleOpt);
    if (st.stripY >= 0) flushStrip(st.stripH);       // last strip
    st.dither.end();
  }
  dec.close();
  heap_caps_free(st.strip);
  st.strip = nullptr;
  st.ok = false;
  return rc != 0;
}
#endif  // EPD_PAINTER_HAVE_JPEGDEC


#if EPD_PAINTER_HAVE_PNGDEC
inline void pngRowCb(PNGDRAW *d) {
  Stream &st = stream();
  if (!st.ok || !st.strip) return;
  PNG *p = (PNG *)d->pUser;
  // PNGdec hands over one complete row at a time, so no strip assembly is
  // needed - convert straight into row 0 and feed it.
  static uint16_t line[1024];
  const int w = (d->iWidth < 1024) ? d->iWidth : 1024;
  p->getLineAsRGB565(d, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  for (int x = 0; x < w && x < st.srcW; x++) {
    // Rec.601 luma from RGB565. Approximate by construction - the source is
    // already quantised to 5/6 bits - but well inside the panel's own ~17
    // unit level spacing.
    const uint16_t v = line[x];
    const int r = ((v >> 11) & 0x1F) << 3;
    const int g = ((v >>  5) & 0x3F) << 2;
    const int b = ( v        & 0x1F) << 3;
    st.strip[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
  }
  st.dither.row(st.strip, d->y);
}

// Decode a PNG from memory straight onto the panel, scaled to fit, centred and
// dithered. Streams: only one row is held at a time.
inline bool drawPngFit(EPD_PainterAdafruit &epd, const uint8_t *data, size_t len) {
  static PNG dec;
  Stream &st = stream();
  st.ok = false;
  if (dec.openRAM((uint8_t *)data, (int)len, pngRowCb) != PNG_SUCCESS) return false;
  const int sw = dec.getWidth(), sh = dec.getHeight();
  if (sw <= 0 || sh <= 0) { dec.close(); return false; }

  const int W = epd.width(), H = epd.height();
  const float s = (W / (float)sw < H / (float)sh) ? W / (float)sw : H / (float)sh;
  const int dw = (int)(sw * s) > 0 ? (int)(sw * s) : 1;
  const int dh = (int)(sh * s) > 0 ? (int)(sh * s) : 1;

  memset(epd.getBuffer(), 0, (size_t)W * H);
  st.srcW = sw;
  st.strip = (uint8_t *)heap_caps_malloc((size_t)sw, MALLOC_CAP_8BIT);
  if (!st.strip) { dec.close(); return false; }

  st.ok = st.dither.begin(epd.driver(), epd.getBuffer(), W, H, sw, sh,
                          (W - dw) / 2, (H - dh) / 2, dw, dh);
  int rc = PNG_INVALID_PARAMETER;
  if (st.ok) { rc = dec.decode(&dec, 0); st.dither.end(); }
  dec.close();
  heap_caps_free(st.strip);
  st.strip = nullptr;
  st.ok = false;
  return rc == PNG_SUCCESS;
}
#endif  // EPD_PAINTER_HAVE_PNGDEC

}  // namespace EPD_PainterImage

#include "dc_video.h"
#include <esp_heap_caps.h>

namespace dc {

// ---------------------------------------------------------------------------
// asset lookup
// ---------------------------------------------------------------------------
const DCScreen *dcFindScreenImpl(int id) {
  for (int i = 0; i < DC_SCREEN_COUNT; i++)
    if (DC_SCREENS[i].id == id) return &DC_SCREENS[i];
  return nullptr;
}
const DCSprite *dcFindSpriteImpl(int id) {
  for (int i = 0; i < DC_SPRITE_COUNT; i++)
    if (DC_SPRITES[i].id == id) return &DC_SPRITES[i];
  return nullptr;
}

bool loadCel(Cel &c, int ppctId) {
  freeCel(c);
  const DCSprite *s = dcFindSpriteImpl(ppctId);
  if (!s) return false;
  const int bytes = spriteBytes(*s);
  c.bits = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!c.bits) c.bits = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  if (!c.bits) return false;
  unpackSprite(s->data, s->len, c.bits, bytes);
  c.meta = s;
  c.rb   = s->w / 8;
  c.mask = spriteHasMask(*s);
  return true;
}

void freeCel(Cel &c) {
  if (c.bits) heap_caps_free(c.bits);
  c.bits = nullptr;
  c.meta = nullptr;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
bool Video::begin(EPD_PainterAdafruit *epd) {
  _epd = epd;
  const int n = DC_SCREEN_RB * DC_SCREEN_H;
  _frame = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  _back  = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!_frame) _frame = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_8BIT);
  if (!_back)  _back  = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_8BIT);
  if (!_frame || !_back) return false;
  memset(_frame, 0, n);
  memset(_back, 0, n);
  return true;
}

void Video::end() {
  if (_frame) heap_caps_free(_frame);
  if (_back)  heap_caps_free(_back);
  _frame = _back = nullptr;
}

void Video::setFullFrame(bool on) {
  _srcH  = on ? DC_SCREEN_H : DC_PLAY_H;
  _viewH = on ? FULL_H      : VIEW_H;
  _viewY = on ? FULL_Y      : VIEW_Y;
  // The view moved, so whatever is on the panel no longer lines up with the
  // canvas; the caller is expected to repaint everything.
  _ndirty = 0;
  dirtyAll();
}

bool Video::loadScreen(int pscrId) {
  const DCScreen *s = dcFindScreenImpl(pscrId);
  if (!s) return false;
  const int n = DC_SCREEN_RB * DC_SCREEN_H;

  if (s->fmt == 0) {
    const int got = unpackScreen(s->data, s->len, _back, n);
    if (got < n) memset(_back + got, 0, n - got);
  } else if (s->rb == DC_SCREEN_RB && s->h <= DC_SCREEN_H) {
    // A full-width PICT lands straight in the screen buffer.
    unpackSprite(s->data, s->len, _back, DC_SCREEN_RB * s->h, 0);
    if (s->h < DC_SCREEN_H)
      memset(_back + DC_SCREEN_RB * s->h, 0,
             DC_SCREEN_RB * (DC_SCREEN_H - s->h));
  } else {
    // Narrower PICT: unpack to its own pitch, then centre it.
    const int bytes = s->rb * s->h;
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!tmp) tmp = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!tmp) return false;
    unpackSprite(s->data, s->len, tmp, bytes, 0);
    memset(_back, 0, n);
    const int xoff = ((DC_SCREEN_W - s->w) / 2) & ~7;
    const int yoff = (DC_SCREEN_H - s->h) / 2;
    for (int y = 0; y < s->h; y++) {
      const int dy = y + yoff;
      if (dy < 0 || dy >= DC_SCREEN_H) continue;
      const int copy = (s->rb < DC_SCREEN_RB - xoff / 8)
                         ? s->rb : DC_SCREEN_RB - xoff / 8;
      memcpy(_back + dy * DC_SCREEN_RB + xoff / 8, tmp + y * s->rb, copy);
    }
    heap_caps_free(tmp);
  }
  restoreAll();
  return true;
}

void Video::restoreAll() {
  memcpy(_frame, _back, DC_SCREEN_RB * DC_SCREEN_H);
  _ndirty = 0;
  dirtyAll();
}

void Video::restore(const Rect &r) {
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w, y1 = r.y + r.h;
  if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
  if (y1 > _srcH)       y1 = _srcH;
  if (x1 <= x0 || y1 <= y0) return;
  const int b0 = x0 >> 3, b1 = (x1 + 7) >> 3;
  for (int y = y0; y < y1; y++) {
    const int o = y * DC_SCREEN_RB;
    memcpy(_frame + o + b0, _back + o + b0, b1 - b0);
  }
}

void Video::fillRect(const Rect &r, bool black) {
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w, y1 = r.y + r.h;
  if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
  if (y1 > _srcH)       y1 = _srcH;
  for (int y = y0; y < y1; y++) {
    uint8_t *row = _frame + y * DC_SCREEN_RB;
    for (int x = x0; x < x1; x++) {
      const uint8_t b = 0x80 >> (x & 7);
      if (black) row[x >> 3] |= b; else row[x >> 3] &= ~b;
    }
  }
  dirty(r);
}

void Video::dirty(const Rect &r) {
  if (r.empty()) return;
  Rect c = r;
  if (c.x < 0) { c.w += c.x; c.x = 0; }
  if (c.y < 0) { c.h += c.y; c.y = 0; }
  if (c.x + c.w > DC_SCREEN_W) c.w = DC_SCREEN_W - c.x;
  if (c.y + c.h > _srcH)       c.h = _srcH - c.y;
  if (c.empty()) return;

  // Merge into an overlapping entry where we can; the list is short and the
  // cost of rescaling a slightly larger box beats the cost of tracking many.
  for (int i = 0; i < _ndirty; i++) {
    Rect &d = _dirty[i];
    if (c.x < d.x + d.w + 8 && d.x < c.x + c.w + 8 &&
        c.y < d.y + d.h + 8 && d.y < c.y + c.h + 8) {
      const int x0 = c.x < d.x ? c.x : d.x;
      const int y0 = c.y < d.y ? c.y : d.y;
      const int x1 = (c.x + c.w > d.x + d.w) ? c.x + c.w : d.x + d.w;
      const int y1 = (c.y + c.h > d.y + d.h) ? c.y + c.h : d.y + d.h;
      d = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
      return;
    }
  }
  if (_ndirty < MAXDIRTY) {
    _dirty[_ndirty++] = c;
  } else {
    // Out of slots: widen the first one rather than dropping the update.
    Rect &d = _dirty[0];
    const int x0 = c.x < d.x ? c.x : d.x;
    const int y0 = c.y < d.y ? c.y : d.y;
    const int x1 = (c.x + c.w > d.x + d.w) ? c.x + c.w : d.x + d.w;
    const int y1 = (c.y + c.h > d.y + d.h) ? c.y + c.h : d.y + d.h;
    d = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
  }
}

// Masked blit. Sprite rows are whole numbers of bytes, but x is arbitrary, so
// each destination byte draws from a 16 bit window over the source -- the
// same trick the original blitter used, minus the 68k rotate.
void Video::blit(const Cel &c, int frame, int x, int y, bool flip) {
  if (!c.bits || !c.meta) return;
  if (frame < 0 || frame >= c.meta->frames) return;

  const uint8_t *img = c.image(frame);
  const uint8_t *msk = c.maskOf(frame);
  const int sw = c.meta->w, sh = c.meta->h, rb = c.rb;

  for (int sy = 0; sy < sh; sy++) {
    const int dy = y + sy;
    if (dy < 0 || dy >= _srcH) continue;
    uint8_t *drow = _frame + dy * DC_SCREEN_RB;
    const uint8_t *irow = img + sy * rb;
    const uint8_t *mrow = msk ? msk + sy * rb : nullptr;

    for (int sx = 0; sx < sw; sx++) {
      const int srcx = flip ? (sw - 1 - sx) : sx;
      const int dx = x + sx;
      if (dx < 0 || dx >= DC_SCREEN_W) continue;
      const uint8_t sbit = 0x80 >> (srcx & 7);
      const bool inMask = mrow ? (mrow[srcx >> 3] & sbit) != 0 : true;
      if (!inMask) continue;
      const uint8_t dbit = 0x80 >> (dx & 7);
      if (irow[srcx >> 3] & sbit) drow[dx >> 3] |= dbit;
      else                        drow[dx >> 3] &= ~dbit;
    }
  }
  dirty({(int16_t)x, (int16_t)y, (int16_t)sw, (int16_t)sh});
}

bool Video::anySet(const Rect &r) const {
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w, y1 = r.y + r.h;
  if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
  if (y1 > DC_SCREEN_H) y1 = DC_SCREEN_H;
  for (int y = y0; y < y1; y++) {
    const uint8_t *row = _back + y * DC_SCREEN_RB;
    for (int x = x0; x < x1; x++)
      if (row[x >> 3] & (0x80 >> (x & 7))) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 3/2 scale with area averaging.
//
// Three destination pixels span two source pixels, so the pattern repeats
// every 2 source / 3 destination. Destination pixel d within a group takes:
//   d0 : source a          (full)
//   d1 : sources a and b   (half each)
//   d2 : source b          (full)
// and the same vertically, giving each output pixel a coverage of 0..4
// quarters which COVER_LEVEL turns into one of the panel's four greys.
// ---------------------------------------------------------------------------
void Video::scaleRect(const Rect &r) {
  uint8_t *canvas = _epd->getBuffer();
  const int cw = _epd->width();

  // Destination range, in panel pixels, that this source rect touches.
  int dx0 = (r.x * 3) / 2, dx1 = ((r.x + r.w) * 3 + 1) / 2;
  int dy0 = (r.y * 3) / 2, dy1 = ((r.y + r.h) * 3 + 1) / 2;
  if (dx0 < 0) dx0 = 0;
  if (dy0 < 0) dy0 = 0;
  if (dx1 > VIEW_W) dx1 = VIEW_W;
  if (dy1 > _viewH) dy1 = _viewH;

  for (int dy = dy0; dy < dy1; dy++) {
    const int gy = dy / 3, py = dy % 3;      // group, phase
    const int sy0 = gy * 2 + (py == 2 ? 1 : 0);
    const int sy1 = (py == 1) ? sy0 + 1 : sy0;
    if (sy0 >= _srcH) break;
    const uint8_t *r0 = _frame + sy0 * DC_SCREEN_RB;
    const uint8_t *r1 = _frame + ((sy1 < _srcH) ? sy1 : sy0) * DC_SCREEN_RB;
    const int nY = (py == 1) ? 2 : 1;
    uint8_t *out = canvas + (size_t)(_viewY + dy) * cw + VIEW_X;

    for (int dx = dx0; dx < dx1; dx++) {
      const int gx = dx / 3, px = dx % 3;
      const int sx0 = gx * 2 + (px == 2 ? 1 : 0);
      const int sx1 = (px == 1) ? sx0 + 1 : sx0;
      const int nX = (px == 1) ? 2 : 1;

      int sum = 0;
      const uint8_t b0 = 0x80 >> (sx0 & 7);
      if (r0[sx0 >> 3] & b0) sum++;
      if (nX == 2) { const uint8_t b1 = 0x80 >> (sx1 & 7);
                     if (r0[sx1 >> 3] & b1) sum++; }
      if (nY == 2) {
        if (r1[sx0 >> 3] & b0) sum++;
        if (nX == 2) { const uint8_t b1 = 0x80 >> (sx1 & 7);
                       if (r1[sx1 >> 3] & b1) sum++; }
      }
      // Normalise the sample count to quarters so 1, 2 and 4 sample pixels
      // land on the same 0..4 scale.
      const int cover = sum * 4 / (nX * nY);
      out[dx] = COVER_LEVEL[cover];
    }
  }
}

int Video::present() {
  int px = 0;
  for (int i = 0; i < _ndirty; i++) {
    scaleRect(_dirty[i]);
    px += (_dirty[i].w * 3 / 2) * (_dirty[i].h * 3 / 2);
  }
  _ndirty = 0;
  return px;
}

}  // namespace dc

// Exposed with the names dc_assets.h declares.
const DCScreen *dcFindScreen(int id) { return dc::dcFindScreenImpl(id); }
const DCSprite *dcFindSprite(int id) { return dc::dcFindSpriteImpl(id); }

// ============================================================================
// dc_video.h -- composition and the trip from Mac pixels to the panel.
//
// The game is composed exactly the way the original did it: a 512x342 one bit
// buffer, 64 bytes per row, background copied in and sprites masked over the
// top. That buffer is the authority, and all game logic works in its
// coordinates.
//
// Getting it onto a 960x540 e-paper panel is the part that is not like a Mac.
// See LAYOUT.md for the reasoning; the short version:
//
//   * 512x310 of playfield scales by exactly 3/2 to 768x465, which leaves a
//     192px column for controls. Two source pixels always become three, so
//     the mapping is periodic and sprites do not shimmer as they move.
//   * The Mac drew greys as 50% dither. Scaling by 3/2 with area averaging
//     turns those patterns into the panel's real mid-greys instead of moire,
//     which is why the scaler resolves to four levels rather than two.
//   * Only dirty rectangles are rescaled, and EPD_Painter then delta-updates
//     only what actually changed, so a frame costs about what the sprites
//     cover rather than what the screen holds.
// ============================================================================

#pragma once
#include <stdint.h>
#include <string.h>
#include "EPD_Painter_Adafruit.h"
#include "dc_assets.h"
#include "dc_codec.h"

namespace dc {

// ---- panel layout ----------------------------------------------------------
static const int VIEW_W    = DC_SCREEN_W * 3 / 2;   // 768
static const int VIEW_H    = DC_PLAY_H   * 3 / 2;   // 465
static const int VIEW_X    = 0;
static const int VIEW_Y    = 38;                    // centred in 540
static const int PANEL_X   = VIEW_W;                // 768
static const int PANEL_W   = 960 - VIEW_W;          // 192

// Title cards use all 342 rows. In play the bottom 32 were the Mac's status
// strip -- the blitter clipped at 310 -- so the playfield is shorter and sits
// a little lower.
static const int FULL_H    = DC_SCREEN_H * 3 / 2;   // 513
static const int FULL_Y    = (540 - FULL_H) / 2;    // 13

// Coverage (0..4) of a destination pixel -> panel level. 0 is paper, 3 ink.
// 1/2 coverage maps to 2 rather than 1: the Mac's 50% dithers read as a
// mid-dark grey on this panel, not a light one.
static const uint8_t COVER_LEVEL[5] = { 0, 1, 2, 2, 3 };

struct Rect {
  int16_t x, y, w, h;
  bool empty() const { return w <= 0 || h <= 0; }
};

// ---------------------------------------------------------------------------
// A decoded sprite, held unpacked in PSRAM for as long as its room is loaded.
// ---------------------------------------------------------------------------
struct Cel {
  const DCSprite *meta = nullptr;
  uint8_t        *bits = nullptr;   // frames * planes, image then mask
  int             rb   = 0;         // row bytes of one frame
  bool            mask = false;

  int planeStride() const { return meta ? meta->plane : 0; }
  const uint8_t *image(int frame) const {
    return bits + (size_t)(mask ? frame * 2 : frame) * meta->plane;
  }
  const uint8_t *maskOf(int frame) const {
    return mask ? bits + (size_t)(frame * 2 + 1) * meta->plane : nullptr;
  }
};

// ---------------------------------------------------------------------------
// The 512x342 composition buffer plus the pristine room behind it.
// ---------------------------------------------------------------------------
class Video {
 public:
  bool begin(EPD_PainterAdafruit *epd);
  void end();

  // Full frame shows all 342 source rows (title cards); otherwise only the
  // 310-row playfield. Changing it re-centres the view and marks everything
  // dirty, so call it before loadScreen().
  void setFullFrame(bool on);
  int  srcH() const { return _srcH; }

  // Load a room's artwork into the background plane.
  bool loadScreen(int pscrId);

  // Restore the whole frame from the background, and mark it all dirty.
  void restoreAll();
  // Restore just one rectangle -- how sprites are erased between frames.
  void restore(const Rect &r);

  // Fill a rectangle. Used to swing a doorway open into darkness where no
  // original sprite matches that door's perspective.
  void fillRect(const Rect &r, bool black);

  // Masked sprite blit, clipped to the playfield. Coordinates are the top
  // left of the cel, in Mac pixels.
  void blit(const Cel &c, int frame, int x, int y, bool flip = false);

  // Dirty rectangle accumulation, in Mac pixels.
  void dirty(const Rect &r);
  void dirtyAll() { dirty({0, 0, DC_SCREEN_W, (int16_t)_srcH}); }

  // Rescale every dirty rectangle into the panel canvas and clear the list.
  // Returns the number of destination pixels touched, for profiling.
  int  present();

  uint8_t *frame()      { return _frame; }
  uint8_t *background() { return _back; }

  // Is any pixel in this rect set? Used for bitmap collision probes.
  bool anySet(const Rect &r) const;
  bool pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= DC_SCREEN_W || y >= DC_SCREEN_H) return false;
    return (_back[y * DC_SCREEN_RB + (x >> 3)] >> (7 - (x & 7))) & 1;
  }

 private:
  void scaleRect(const Rect &r);

  EPD_PainterAdafruit *_epd  = nullptr;
  uint8_t             *_frame = nullptr;   // composed, 512x342
  uint8_t             *_back  = nullptr;   // pristine room

  // How much of the source is shown, and where it lands on the panel.
  // Playfield by default; setFullFrame() switches to the whole 342 rows.
  int _srcH  = DC_PLAY_H;
  int _viewY = VIEW_Y;
  int _viewH = VIEW_H;
  static const int     MAXDIRTY = 24;
  Rect                 _dirty[MAXDIRTY];
  int                  _ndirty = 0;
};

// Decode a sprite out of flash into PSRAM. Returns false if it is not there.
bool loadCel(Cel &c, int ppctId);
void freeCel(Cel &c);

}  // namespace dc

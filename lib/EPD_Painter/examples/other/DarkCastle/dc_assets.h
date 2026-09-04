// ============================================================================
// dc_assets.h -- index over the extracted Dark Castle artwork.
//
// The blobs in dc_gfx_*.cpp are still in the game's own compressed form, so
// what sits in flash is byte for byte what shipped on the disk. dc_codec.h
// unpacks them.
//
// Generate the .cpp files with:  python3 tools/dc_extract.py <disk image>
// ============================================================================

#pragma once
#include <stdint.h>

// A room / title screen, 1 = black.
//
// Most are PSCR resources: 512x342, 64 bytes per row, dictionary coded. But
// the Great Hall is a PICT, not a PSCR -- the original loader asks for PICT
// first and only falls back -- and PICTs come in their own sizes. Those are
// decoded at extraction time and re-encoded in the sprite codec, so the port
// carries two decoders rather than a QuickDraw interpreter.
//
// The bottom 32 rows of a full screen were the Mac's status strip; the
// playfield is the top 310.
struct DCScreen {
  int16_t              id;      // original PSCR or PICT resource id
  const unsigned char *data;
  int32_t              len;
  uint8_t              fmt;     // 0 = PSCR dictionary, 1 = bit RLE
  int16_t              w, h, rb;
};

// An animation: `frames` cells side by side, each `w` x `h`. Modes 0 and 3
// carry a mask plane after every image plane; 1, 2 and 4 are image only.
struct DCSprite {
  int16_t              id;      // original PPCT resource id
  const unsigned char *data;    // packed stream, bit RLE
  int32_t              len;
  uint8_t              mode;
  uint8_t              frames;
  int16_t              w;       // pixels, always a multiple of 16
  int16_t              h;
  int16_t              plane;   // bytes per plane per frame
  int16_t              hotX;    // registration point, from the PPCT header
  int16_t              hotY;
};

extern const DCScreen DC_SCREENS[];
extern const int      DC_SCREEN_COUNT;
extern const DCSprite DC_SPRITES[];
extern const int      DC_SPRITE_COUNT;

const DCScreen *dcFindScreen(int id);
const DCSprite *dcFindSprite(int id);

// Native Mac geometry. Everything in the game logic is in these units; the
// renderer scales on the way to the panel.
static const int DC_SCREEN_W   = 512;
static const int DC_SCREEN_H   = 342;
static const int DC_PLAY_H     = 310;   // blitter clipped here; rest was status
static const int DC_SCREEN_RB  = 64;    // row bytes

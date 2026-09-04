// ============================================================================
// dc_rooms.h -- the world: which screen, what stands on it, where the doors go.
//
// Dark Castle shipped no collision data. The original blitter drew straight to
// the screen and the floors exist only in the artwork, so the solid map here is
// derived from each room's own bitmap at load time (see SolidMap) and then
// corrected by a short list of per-room fixups where the derivation misreads
// the picture -- black cave scenery that is not floor, or a floor drawn as a
// thin line that is not dense enough to register.
//
// That keeps the geometry honest to the art: if the picture shows a step, you
// can stand on it, without anyone typing in three hundred coordinates.
// ============================================================================

#pragma once
#include <stdint.h>
#include "dc_assets.h"

namespace dc {

// The solid map is a 4 pixel grid over the 512x310 playfield.
static const int CELL  = 4;
static const int GRID_W = DC_SCREEN_W / CELL;   // 128
static const int GRID_H = DC_PLAY_H  / CELL;    // 77 (310/4 truncates)

// A cell counts as solid when this many of its 16 pixels are ink. The walls
// are drawn as 50% dither and must NOT register; the floors are near enough
// to solid black that a high threshold separates them cleanly.
static const int SOLID_THRESH = 13;

enum FixKind : uint8_t { FIX_CLEAR = 0, FIX_SOLID = 1 };

// A rectangle, in Mac pixels, forced one way or the other after derivation.
struct Fixup {
  int16_t x, y, w, h;
  uint8_t kind;
};

enum EnemyKind : uint8_t {
  EK_NONE = 0,
  EK_BIRD,        // flies, drifts toward the hero
  EK_CREATURE,   // patrols a floor
  EK_GUARD,      // patrols and throws
  EK_FIRE,       // static hazard, animates in place
};

struct Spawn {
  uint8_t  kind;
  int16_t  x, y;       // Mac pixels; y is the creature's feet
  int16_t  x0, x1;     // patrol limits (bats use it as a drift box)
};

// Doorways. Stepping inside the rectangle moves the hero to another room.
struct Exit {
  int16_t x, y, w, h;
  uint8_t toRoom;      // index into ROOMS
  int16_t spawnX, spawnY;
};

struct RoomDef {
  const char   *name;
  int16_t       pscr;          // PSCR resource id for the backdrop
  int16_t       startX, startY;
  const Fixup  *fixups;  uint8_t nfixups;
  const Spawn  *spawns;  uint8_t nspawns;
  const Exit   *exits;   uint8_t nexits;
};

// A door in the Great Hall. The hall is drawn in perspective and you cannot
// literally walk into a door up on the back wall, so it is used by touch: tap
// a door and the hero runs to it, it opens, he goes through, it shuts.
//
// `sprite` is the original PPCT opening animation where one fits the artwork
// exactly -- 10050 is 64x159 and the centre door is 64 wide by 159 tall, which
// is not a coincidence. Where no sprite matches, the opening is drawn as the
// doorway darkening, which is what these doors look like when they swing in.
struct HallDoor {
  int16_t x, y, w, h;      // the door in the artwork
  int16_t walkX;           // where the hero stands to use it
  int16_t sprite;          // PPCT id, or 0 to draw the opening directly
  uint8_t toRoom;
  int16_t spawnX, spawnY;
};

extern const RoomDef   ROOMS[];
extern const int       ROOM_COUNT;
extern const int       ROOM_GREAT_HALL;
extern const HallDoor  HALL_DOORS[];
extern const int       HALL_DOOR_COUNT;

// ---------------------------------------------------------------------------
// Solid map derived from a room bitmap.
// ---------------------------------------------------------------------------
class SolidMap {
 public:
  void build(const uint8_t *screen, const RoomDef &room);

  bool solid(int gx, int gy) const {
    if (gx < 0 || gy < 0 || gx >= GRID_W || gy >= GRID_H) return false;
    return (_g[gy * ((GRID_W + 7) / 8) + (gx >> 3)] >> (gx & 7)) & 1;
  }
  // Mac-pixel query.
  bool solidAt(int x, int y) const { return solid(x / CELL, y / CELL); }

  // Is anything solid across [x0,x1] at pixel row y?
  bool spanSolid(int x0, int x1, int y) const {
    for (int x = x0; x <= x1; x += CELL)
      if (solidAt(x, y)) return true;
    return solidAt(x1, y);
  }

 private:
  void set(int gx, int gy, bool v);
  uint8_t _g[((GRID_W + 7) / 8) * GRID_H];
};

}  // namespace dc

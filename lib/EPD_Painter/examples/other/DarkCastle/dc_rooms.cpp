#include "dc_rooms.h"
#include <string.h>

namespace dc {

// ---------------------------------------------------------------------------
// SolidMap
// ---------------------------------------------------------------------------
void SolidMap::set(int gx, int gy, bool v) {
  if (gx < 0 || gy < 0 || gx >= GRID_W || gy >= GRID_H) return;
  const int i = gy * ((GRID_W + 7) / 8) + (gx >> 3);
  const uint8_t b = 1 << (gx & 7);
  if (v) _g[i] |= b; else _g[i] &= ~b;
}

void SolidMap::build(const uint8_t *screen, const RoomDef &room) {
  memset(_g, 0, sizeof(_g));

  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      int n = 0;
      for (int j = 0; j < CELL; j++) {
        const uint8_t *row = screen + (gy * CELL + j) * DC_SCREEN_RB;
        for (int i = 0; i < CELL; i++) {
          const int x = gx * CELL + i;
          if (row[x >> 3] & (0x80 >> (x & 7))) n++;
        }
      }
      if (n >= SOLID_THRESH) set(gx, gy, true);
    }
  }

  // Per-room corrections, applied in order so a CLEAR can be re-filled.
  for (int i = 0; i < room.nfixups; i++) {
    const Fixup &f = room.fixups[i];
    const int gx0 = f.x / CELL, gy0 = f.y / CELL;
    const int gx1 = (f.x + f.w - 1) / CELL, gy1 = (f.y + f.h - 1) / CELL;
    for (int gy = gy0; gy <= gy1; gy++)
      for (int gx = gx0; gx <= gx1; gx++)
        set(gx, gy, f.kind == FIX_SOLID);
  }
}

// ---------------------------------------------------------------------------
// The world.
//
// Coordinates below are authored against the original artwork. The floors come
// from the pictures themselves, so the fixups only cover the two cases the
// density test cannot get right on its own.
// ---------------------------------------------------------------------------

// -- Trouble ---------------------------------------------------------------
// Three staircases descending left to right; ropes down the right hand side.
// The derivation reads this room almost perfectly, so there is little to fix.
static const Fixup fx1000[] = {
  {   0, 296, 512,  14, FIX_SOLID },   // bottom floor, drawn as a thin line
  { 384,   0, 128,  96, FIX_CLEAR },   // upper right brickwork is background
};
// Nothing patrols the top landing: the hero enters there at x=40, and a
// creature sharing that ledge reaches him before the player can move.
static const Spawn sp1000[] = {
  { EK_BIRD,      300,  60,  260, 470 },
  { EK_CREATURE, 170, 160,  120, 200 },   // middle landing, out of reach at entry
  { EK_CREATURE, 200, 232,  120, 300 },
};
static const Exit ex1000[] = {
  { 496, 240,  16,  70, 1, 16, 290 },     // off the right, into 1010
};

static const Fixup fx1010[] = {
  {   0, 292, 512,  18, FIX_SOLID },
};
static const Spawn sp1010[] = {
  { EK_CREATURE, 260, 292,  140, 460 },
  { EK_BIRD,      380,  80,  200, 500 },
};
static const Exit ex1010[] = {
  {   0, 240,  16,  70, 0, 490, 290 },
  { 496, 240,  16,  70, 2,  16, 290 },
};

static const Fixup fx1020[] = {
  {   0, 292, 512,  18, FIX_SOLID },
};
static const Spawn sp1020[] = {
  { EK_GUARD,    340, 292,  200, 470 },
  { EK_CREATURE,  90, 292,   20, 200 },
};
static const Exit ex1020[] = {
  {   0, 240,  16,  70, 1, 490, 290 },
  { 496, 240,  16,  70, 16,  84, 299 },   // hall, beside the far-left door
};

// -- Shield ----------------------------------------------------------------
// Cave rooms: large genuinely solid masses, which the density test likes.
static const Fixup fx2000[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp2000[] = {
  { EK_BIRD,      260,  70,  120, 480 },
  { EK_CREATURE, 180, 296,   40, 460 },
};
static const Exit ex2000[] = {
  { 496, 230,  16,  80, 4, 16, 290 },
  {   0, 230,  16,  80, 16, 186, 299 },
};
static const Fixup fx2010[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp2010[] = { { EK_BIRD, 300, 90, 100, 480 } };
static const Exit  ex2010[] = {
  {   0, 230, 16, 80, 3, 490, 290 }, { 496, 230, 16, 80, 5, 16, 290 } };

static const Fixup fx2020[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp2020[] = { { EK_CREATURE, 240, 296, 60, 460 } };
static const Exit  ex2020[] = {
  {   0, 230, 16, 80, 4, 490, 290 }, { 496, 230, 16, 80, 7, 16, 290 } };

static const Fixup fx2022[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Exit  ex2022[] = { {   0, 230, 16, 80, 5, 490, 290 } };

static const Fixup fx2030[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp2030[] = { { EK_GUARD, 300, 296, 150, 470 } };
static const Exit  ex2030[] = { {   0, 230, 16, 80, 5, 490, 290 } };

// -- Fireball --------------------------------------------------------------
static const Fixup fx3000[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp3000[] = {
  { EK_FIRE,     220, 296,    0,   0 },
  { EK_CREATURE, 380, 296,  260, 480 },
};
static const Exit  ex3000[] = {
  { 496, 230, 16, 80, 9, 16, 290 }, { 0, 230, 16, 80, 16, 430, 299 } };

// The outdoor rampart: sky, distant hills, castle battlements along the bottom
// and a ladder up the outer wall. The crenellations are the floor and they sit
// higher than the indoor rooms' -- the walkway is at about y=272, not 296.
static const Fixup fx3010[] = {
  {   0, 272, 300,  38, FIX_SOLID },   // battlement walkway, left of the wall
  { 300, 250, 212,  60, FIX_SOLID },   // the wall's footing, stepped up
};
// This is the one you remember: out on the ramparts, throwing rocks at birds.
static const Spawn sp3010[] = {
  { EK_BIRD, 200,  70,   40, 280 },
  { EK_BIRD, 120, 110,   20, 260 },
  { EK_BIRD, 260,  50,   60, 290 },
};
static const Exit  ex3010[] = {
  { 0, 230, 16, 80, 8, 490, 290 }, { 496, 230, 16, 80, 11, 16, 290 } };

static const Fixup fx3012[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Exit  ex3012[] = { { 0, 230, 16, 80, 9, 490, 290 } };

static const Fixup fx3020[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp3020[] = { { EK_BIRD, 260, 80, 80, 470 } };
static const Exit  ex3020[] = {
  { 0, 230, 16, 80, 9, 490, 290 }, { 496, 230, 16, 80, 12, 16, 290 } };

static const Fixup fx3030[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp3030[] = { { EK_GUARD, 320, 296, 180, 470 } };
static const Exit  ex3030[] = { { 0, 230, 16, 80, 11, 490, 290 } };

static const Fixup fx3032[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Exit  ex3032[] = { { 0, 230, 16, 80, 12, 490, 290 } };

// -- Black Knight ----------------------------------------------------------
static const Fixup fx4000[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp4000[] = { { EK_GUARD, 300, 296, 140, 470 } };
static const Exit  ex4000[] = {
  { 496, 230, 16, 80, 15, 16, 290 }, { 0, 230, 16, 80, 16, 330, 299 } };

static const Fixup fx4010[] = { {   0, 296, 512, 14, FIX_SOLID } };
static const Spawn sp4010[] = { { EK_GUARD, 360, 296, 200, 480 } };
static const Exit  ex4010[] = { { 0, 230, 16, 80, 14, 490, 290 } };

// -- Great Hall (the hub) --------------------------------------------------
// This is PICT 10000, NOT a PSCR. The original loader asks for PICT first and
// only falls back to PSCR, so the hall is easy to miss when walking the PSCR
// list -- PSCR 4020 is a different room entirely (kept below as "Stone Hall").
//
// It is drawn in one-point perspective, so there is no real geometry to
// derive: the tiled floor is heavy dither that the density test would read as
// solid everywhere. The whole play area is cleared and a flat floor stated
// outright, which is what the original effectively did -- you walk along the
// front of the hall and the doorways trigger on x.
static const Fixup fxHall[] = {
  {   0,   0, 512, 300, FIX_CLEAR },   // perspective art is not geometry
  {   0, 300, 512,  10, FIX_SOLID },   // the floor you walk along
};
// The hall has NO walk-through exits: its doors are used by touch, through
// HALL_DOORS below. Leaving nexits at zero also stops the hero wandering into
// a doorway he happens to be standing near.
static const Exit exHall[] = { { -1, -1, 0, 0, 0, 0, 0 } };

// Doors, measured off the artwork. WHICH DOOR LEADS WHERE IS A GUESS: the hall
// carries no markings, and the mapping lives in Main's dispatch at +0x2192,
// which has not been traced back to what feeds it. Left to right for now.
const HallDoor HALL_DOORS[] = {
  //  x    y    w    h   walkX  sprite  room  spawnX spawnY
  {   8,  64,  48, 128,     84,      0,    0,    40,   100 },  // -> Trouble
  { 104,  96,  48,  96,    186,      0,    3,    40,   290 },  // -> Shield
  { 224,  16,  64, 159,    330,  10050,   14,   470,   290 },  // -> Black Knight
  { 464,  72,  48, 128,    430,      0,    8,   470,   290 },  // -> Fireball
};
const int HALL_DOOR_COUNT = sizeof(HALL_DOORS) / sizeof(HALL_DOORS[0]);

// PSCR 4020: the stone hall with the staircases and ladders. Not the hub.
static const Fixup fx4020[] = {
  {   0, 288, 512,  22, FIX_SOLID },
};
static const Exit ex4020[] = {
  { 496, 240,  16,  70, 16, 380, 299 },
};

#define ROOM(nm, id, sx, sy, fx, sp, ex)                                  \
  { nm, id, sx, sy, fx, (uint8_t)(sizeof(fx) / sizeof(fx[0])),            \
    sp, (uint8_t)(sizeof(sp) / sizeof(sp[0])),                            \
    ex, (uint8_t)(sizeof(ex) / sizeof(ex[0])) }
#define ROOM_NS(nm, id, sx, sy, fx, ex)                                   \
  { nm, id, sx, sy, fx, (uint8_t)(sizeof(fx) / sizeof(fx[0])),            \
    nullptr, 0, ex, (uint8_t)(sizeof(ex) / sizeof(ex[0])) }

const RoomDef ROOMS[] = {
  /*  0 */ ROOM("Trouble 1",    1000,  40, 100, fx1000, sp1000, ex1000),
  /*  1 */ ROOM("Trouble 2",    1010,  16, 290, fx1010, sp1010, ex1010),
  /*  2 */ ROOM("Trouble 3",    1020,  16, 290, fx1020, sp1020, ex1020),
  /*  3 */ ROOM("Shield 1",     2000,  16, 290, fx2000, sp2000, ex2000),
  /*  4 */ ROOM("Shield 2",     2010,  16, 290, fx2010, sp2010, ex2010),
  /*  5 */ ROOM("Shield 3",     2020,  16, 290, fx2020, sp2020, ex2020),
  /*  6 */ ROOM_NS("Shield 3b", 2022,  16, 290, fx2022,         ex2022),
  /*  7 */ ROOM("Shield 4",     2030,  16, 290, fx2030, sp2030, ex2030),
  /*  8 */ ROOM("Fireball 1",   3000,  16, 290, fx3000, sp3000, ex3000),
  /*  9 */ ROOM("Fireball 2",   3010,  40, 272, fx3010, sp3010, ex3010),
  /* 10 */ ROOM_NS("Fireball 2b", 3012, 16, 290, fx3012,        ex3012),
  /* 11 */ ROOM("Fireball 3",   3020,  16, 290, fx3020, sp3020, ex3020),
  /* 12 */ ROOM("Fireball 4",   3030,  16, 290, fx3030, sp3030, ex3030),
  /* 13 */ ROOM_NS("Fireball 4b", 3032, 16, 290, fx3032,        ex3032),
  /* 14 */ ROOM("Black Knight 1", 4000, 16, 290, fx4000, sp4000, ex4000),
  /* 15 */ ROOM("Black Knight 2", 4010, 16, 290, fx4010, sp4010, ex4010),
  /* 16 */ { "Great Hall", 10000, 380, 299, fxHall,
    (uint8_t)(sizeof(fxHall)/sizeof(fxHall[0])), nullptr, 0, nullptr, 0 },
  /* 17 */ ROOM_NS("Stone Hall",   4020, 240, 280, fx4020,       ex4020),
};
const int ROOM_COUNT      = sizeof(ROOMS) / sizeof(ROOMS[0]);
const int ROOM_GREAT_HALL = 16;

}  // namespace dc

// ============================================================================
// dc_game.h -- entities, physics and the run loop.
//
// Everything here works in the original's 512x310 pixel space. The renderer
// is the only thing that knows the panel is a different shape.
//
// The one deliberate departure from the Mac is aiming. Dark Castle threw rocks
// at the mouse pointer, which has no equivalent here, so a tap anywhere in the
// playfield throws at that point -- the same gesture, one step more direct.
// Movement, which was the keyboard, becomes the touch column on the right.
// ============================================================================

#pragma once
#include <stdint.h>
#include "dc_video.h"
#include "dc_rooms.h"

namespace dc {

// ---- sprite ids, as identified from the original artwork -------------------
enum {
  SPR_STAND_L = 128, SPR_STAND_R = 129,
  SPR_WALK_L  = 107, SPR_WALK_R  = 108,
  SPR_RUN_L   = 109, SPR_RUN_R   = 110,
  SPR_JUMP_L  = 130, SPR_JUMP_R  = 131,
  SPR_DUCK_L  = 119, SPR_DUCK_R  = 120,
  SPR_THROW_L = 117, SPR_THROW_R = 118,
  SPR_DEAD    = 137,
  SPR_ROCK    = 113,
  SPR_BIRD    = 701,   // crow: 20 frames, flap then dive
  SPR_CREATURE= 434,
  SPR_GUARD   = 350,
  SPR_FIRE    = 433,
};

enum HeroState : uint8_t {
  HS_STAND, HS_WALK, HS_RUN, HS_JUMP, HS_DUCK, HS_THROW, HS_HURT, HS_DEAD
};

struct Input {
  bool left = false, right = false, jump = false, duck = false;
  bool run  = false;
  bool fire = false;              // a tap landed in the playfield
  int16_t aimX = 0, aimY = 0;     // where, in Mac pixels
};

struct Hero {
  int16_t   x = 0, y = 0;         // feet centre
  int16_t   vx = 0, vy = 0;       // 1/16 pixel per tick
  HeroState state = HS_STAND;
  bool      faceLeft = false;
  uint8_t   frame = 0, anim = 0;
  int8_t    health = 3;
  uint8_t   invuln = 0;           // ticks of grace after a hit
  uint8_t   busy = 0;             // throw / hurt animation lock
  bool      onGround = false;
};

struct Rock {
  bool     live = false;
  int16_t  x, y, vx, vy;          // 1/16 pixel
  uint8_t  frame;
};

struct Enemy {
  uint8_t  kind = EK_NONE;
  bool     alive = false;
  int16_t  x, y, x0, x1;
  int16_t  vx, vy;
  uint8_t  frame, anim;
  int16_t  homeY;
  uint8_t  cool;                  // guard throw cooldown
};

static const int MAX_ROCKS   = 4;
static const int MAX_ENEMIES = 6;

class Game {
 public:
  bool begin(Video *video);
  void end();

  void enterRoom(int index, int spawnX = -1, int spawnY = -1);
  void restart();

  // One tick of simulation. Returns true if anything moved and the frame
  // needs presenting.
  bool update(const Input &in);
  // Draw the moving parts over the restored background.
  void draw();

  // The Great Hall is used by touch: tap a door, the hero runs to it, it
  // opens, he goes through, it shuts behind him.
  bool isHub() const { return _room == ROOM_GREAT_HALL; }
  // Pick a hall door directly, by index. Same path a tap takes; exposed so the
  // sequence can be exercised over serial without a finger on the glass.
  void hubSelect(int door);

  const Hero &hero()  const { return _hero; }
  int   room()        const { return _room; }
  const char *roomName() const { return ROOMS[_room].name; }
  uint32_t score()    const { return _score; }
  int   lives()       const { return _lives; }
  bool  dead()        const { return _lives <= 0; }

 private:
  bool groundAt(int x, int y) const;
  int  settle(int x, int y) const;           // snap feet to the floor below
  void moveHero(const Input &in);
  void updateRocks();
  void updateEnemies();
  void hurt();
  void throwRock(int aimX, int aimY);
  void hubTap(int x, int y);
  void hubUpdate();
  void hubDraw();
  void spriteFor(HeroState s, bool left, int &id, int &nframes) const;
  Cel *cel(int id);

  Video    *_v = nullptr;
  SolidMap  _solid;
  Hero      _hero;
  Rock      _rocks[MAX_ROCKS];
  Enemy     _en[MAX_ENEMIES];
  int       _room = 0;
  uint32_t  _score = 0;
  int       _lives = 3;
  uint32_t  _tick = 0;
  // Ticks before doorways arm after entering a room. Without it, spawning in
  // front of the door you just came out of walks you straight back through it.
  uint8_t   _exitLock = 0;

  // Great Hall door sequence.
  enum HubState : uint8_t { HUB_IDLE, HUB_WALK, HUB_OPEN, HUB_ENTER, HUB_SHUT };
  HubState  _hub   = HUB_IDLE;
  int8_t    _hubDoor = -1;
  uint8_t   _hubStep = 0;

  // Cel cache. Rooms share most of these, so they are loaded once and kept.
  static const int MAX_CELS = 20;
  struct CelSlot { int id; Cel cel; };
  CelSlot   _cels[MAX_CELS];
  int       _ncels = 0;

  // Rectangles touched last frame, so they can be restored before redrawing.
  Rect      _last[MAX_ROCKS + MAX_ENEMIES + 2];
  int       _nlast = 0;
  void      mark(const Rect &r) {
    if (_nlast < (int)(sizeof(_last) / sizeof(_last[0]))) _last[_nlast++] = r;
  }
};

}  // namespace dc

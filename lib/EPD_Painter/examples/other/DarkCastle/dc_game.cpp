#include "dc_game.h"
#include <stdlib.h>

namespace dc {

// Physics, in 1/16 pixel per tick. Tuned to feel like the original at the
// ~12 fps an e-paper panel will actually sustain, not at the Mac's rate.
static const int GRAVITY   = 12;
static const int WALK_VX   = 20;
static const int RUN_VX    = 34;
static const int JUMP_VY   = -84;
static const int MAX_VY    = 120;
static const int HERO_HALF = 8;      // half width of the collision box
static const int HERO_TALL = 40;
static const int STEP_UP   = 12;     // how tall a step can be walked up

// ---------------------------------------------------------------------------
Cel *Game::cel(int id) {
  for (int i = 0; i < _ncels; i++)
    if (_cels[i].id == id) return &_cels[i].cel;
  if (_ncels >= MAX_CELS) return nullptr;
  CelSlot &s = _cels[_ncels];
  if (!loadCel(s.cel, id)) return nullptr;
  s.id = id;
  _ncels++;
  return &s.cel;
}

bool Game::begin(Video *video) {
  _v = video;
  _score = 0;
  _lives = 3;
  return true;
}

void Game::end() {
  for (int i = 0; i < _ncels; i++) freeCel(_cels[i].cel);
  _ncels = 0;
}

void Game::restart() {
  _score = 0;
  _lives = 3;
  _hero.health = 3;
  enterRoom(ROOM_GREAT_HALL);
}

void Game::enterRoom(int index, int spawnX, int spawnY) {
  if (index < 0 || index >= ROOM_COUNT) return;
  _room = index;
  const RoomDef &r = ROOMS[index];

  _v->loadScreen(r.pscr);
  _solid.build(_v->background(), r);

  _hero.x = (spawnX >= 0) ? spawnX : r.startX;
  _hero.y = (spawnY >= 0) ? spawnY : r.startY;
  _hero.y = settle(_hero.x, _hero.y);
  _hero.vx = _hero.vy = 0;
  _hero.state = HS_STAND;
  _hero.busy = 0;
  _hero.onGround = true;

  for (int i = 0; i < MAX_ROCKS; i++) _rocks[i].live = false;
  for (int i = 0; i < MAX_ENEMIES; i++) _en[i].kind = EK_NONE;
  for (int i = 0; i < r.nspawns && i < MAX_ENEMIES; i++) {
    const Spawn &s = r.spawns[i];
    Enemy &e = _en[i];
    e.kind = s.kind;
    e.alive = true;
    e.x = s.x; e.y = s.y; e.x0 = s.x0; e.x1 = s.x1;
    e.homeY = s.y;
    e.vx = (s.kind == EK_BIRD) ? 12 : 8;
    e.vy = 6;
    e.frame = e.anim = 0;
    e.cool = 0;
  }
  _nlast = 0;
  // Doorways stay disarmed briefly after arriving: long enough that a finger
  // still on the glass, or a spawn point near a door, cannot bounce you
  // straight back out. At 7-40 fps this is a fraction of a second.
  _exitLock = 25;
  _v->restoreAll();
}

// ---------------------------------------------------------------------------
// terrain
// ---------------------------------------------------------------------------
bool Game::groundAt(int x, int y) const {
  return _solid.spanSolid(x - HERO_HALF + 2, x + HERO_HALF - 2, y);
}

int Game::settle(int x, int y) const {
  // Fall to the first solid row at or below y, else leave alone.
  for (int t = 0; t < DC_PLAY_H; t++) {
    const int yy = y + t;
    if (yy >= DC_PLAY_H) break;
    if (groundAt(x, yy + 1)) return yy;
  }
  return y;
}

// ---------------------------------------------------------------------------
// hero
// ---------------------------------------------------------------------------
void Game::spriteFor(HeroState s, bool left, int &id, int &n) const {
  switch (s) {
    case HS_WALK:  id = left ? SPR_WALK_L  : SPR_WALK_R;  break;
    case HS_RUN:   id = left ? SPR_RUN_L   : SPR_RUN_R;   break;
    case HS_JUMP:  id = left ? SPR_JUMP_L  : SPR_JUMP_R;  break;
    case HS_DUCK:  id = left ? SPR_DUCK_L  : SPR_DUCK_R;  break;
    case HS_THROW: id = left ? SPR_THROW_L : SPR_THROW_R; break;
    case HS_DEAD:  id = SPR_DEAD; break;
    case HS_HURT:  id = left ? SPR_JUMP_L  : SPR_JUMP_R;  break;
    default:       id = left ? SPR_STAND_L : SPR_STAND_R; break;
  }
  const DCSprite *m = dcFindSprite(id);
  n = m ? m->frames : 1;
}

void Game::throwRock(int aimX, int aimY) {
  for (int i = 0; i < MAX_ROCKS; i++) {
    if (_rocks[i].live) continue;
    Rock &r = _rocks[i];
    r.live = true;
    r.x = (_hero.x) * 16;
    r.y = (_hero.y - 28) * 16;
    // Aim straight at the tap, at a fixed speed, with a touch of lift so the
    // arc looks thrown rather than fired.
    int dx = aimX - _hero.x, dy = aimY - (_hero.y - 28);
    int len = abs(dx) + abs(dy);
    if (len < 8) len = 8;
    const int SPEED = 90;
    r.vx = (int16_t)((long)dx * SPEED / len);
    r.vy = (int16_t)((long)dy * SPEED / len) - 20;
    r.frame = 0;
    _hero.faceLeft = dx < 0;
    _hero.state = HS_THROW;
    _hero.busy = 4;
    return;
  }
}

void Game::moveHero(const Input &in) {
  Hero &h = _hero;
  if (h.state == HS_DEAD) return;

  if (h.busy) h.busy--;

  const bool ducking = in.duck && h.onGround && !h.busy;

  // horizontal
  int want = 0;
  if (!ducking && !h.busy) {
    if (in.left)  want = -1;
    if (in.right) want = +1;
  }
  const int speed = in.run ? RUN_VX : WALK_VX;
  h.vx = want * speed;
  if (want) h.faceLeft = want < 0;

  // jump
  if (in.jump && h.onGround && !ducking && !h.busy) {
    h.vy = JUMP_VY;
    h.onGround = false;
  }

  // gravity
  h.vy += GRAVITY;
  if (h.vy > MAX_VY) h.vy = MAX_VY;

  // --- horizontal move, with a step up so staircases just work -----------
  if (h.vx) {
    const int nx = h.x + (h.vx >= 0 ? (h.vx + 8) / 16 : -((-h.vx + 8) / 16));
    int cx = nx;
    if (cx < HERO_HALF) cx = HERO_HALF;
    if (cx > DC_SCREEN_W - HERO_HALF) cx = DC_SCREEN_W - HERO_HALF;

    bool blocked = false;
    for (int t = 1; t <= HERO_TALL - 6; t += 4)
      if (_solid.solidAt(cx + (h.vx > 0 ? HERO_HALF - 1 : -HERO_HALF + 1),
                         h.y - t)) { blocked = true; break; }
    if (!blocked) {
      h.x = cx;
    } else {
      // try lifting the feet; that is what makes a flight of stairs walkable
      bool climbed = false;
      for (int up = 2; up <= STEP_UP; up += 2) {
        bool clear = true;
        for (int t = 1; t <= HERO_TALL - 6; t += 4)
          if (_solid.solidAt(cx + (h.vx > 0 ? HERO_HALF - 1 : -HERO_HALF + 1),
                             h.y - up - t)) { clear = false; break; }
        if (clear && groundAt(cx, h.y - up + 1)) {
          h.x = cx; h.y -= up; climbed = true; break;
        }
      }
      if (!climbed) h.vx = 0;
    }
  }

  // --- vertical -----------------------------------------------------------
  const int dy = (h.vy >= 0) ? (h.vy + 8) / 16 : -((-h.vy + 8) / 16);
  if (dy > 0) {
    int step = 0;
    while (step < dy) {
      if (groundAt(h.x, h.y + 1)) break;
      h.y++; step++;
      if (h.y >= DC_PLAY_H - 1) break;
    }
    h.onGround = groundAt(h.x, h.y + 1);
    if (h.onGround) h.vy = 0;
  } else if (dy < 0) {
    for (int step = 0; step > dy; step--) {
      if (_solid.solidAt(h.x, h.y - HERO_TALL - 1)) { h.vy = 0; break; }
      h.y--;
      if (h.y < HERO_TALL) break;
    }
    h.onGround = false;
  }

  // Walking off the bottom of a room is a fall.
  if (h.y >= DC_PLAY_H - 2) hurt();

  // --- animation state ----------------------------------------------------
  if (h.busy && h.state == HS_THROW) {
    // hold the throw pose
  } else if (!h.onGround) {
    h.state = HS_JUMP;
  } else if (ducking) {
    h.state = HS_DUCK;
  } else if (want) {
    h.state = in.run ? HS_RUN : HS_WALK;
  } else {
    h.state = HS_STAND;
  }

  int id, n;
  spriteFor(h.state, h.faceLeft, id, n);
  if (h.state == HS_STAND || h.state == HS_DUCK) {
    h.frame = 0;
  } else if ((_tick & 1) == 0) {
    h.anim++;
    h.frame = n ? (h.anim % n) : 0;
  }

  if (h.invuln) h.invuln--;
}

void Game::hurt() {
  if (_hero.invuln || _hero.state == HS_DEAD) return;
  _hero.health--;
  _hero.invuln = 30;
  if (_hero.health <= 0) {
    _lives--;
    if (_lives > 0) {
      _hero.health = 3;
      enterRoom(_room);
    } else {
      _hero.state = HS_DEAD;
    }
  }
}

// ---------------------------------------------------------------------------
void Game::updateRocks() {
  for (int i = 0; i < MAX_ROCKS; i++) {
    Rock &r = _rocks[i];
    if (!r.live) continue;
    r.vy += GRAVITY;
    r.x += r.vx;
    r.y += r.vy;
    const int px = r.x / 16, py = r.y / 16;
    if (px < 0 || px >= DC_SCREEN_W || py < 0 || py >= DC_PLAY_H ||
        _solid.solidAt(px, py)) {
      r.live = false;
      continue;
    }
    r.frame++;
    for (int e = 0; e < MAX_ENEMIES; e++) {
      Enemy &en = _en[e];
      if (!en.alive || en.kind == EK_NONE || en.kind == EK_FIRE) continue;
      if (abs(px - en.x) < 16 && abs(py - (en.y - 12)) < 16) {
        en.alive = false;
        r.live = false;
        _score += (en.kind == EK_GUARD) ? 500 : 100;
        break;
      }
    }
  }
}

void Game::updateEnemies() {
  for (int i = 0; i < MAX_ENEMIES; i++) {
    Enemy &e = _en[i];
    if (e.kind == EK_NONE || !e.alive) continue;

    switch (e.kind) {
      case EK_CREATURE:
        e.x += (e.vx >= 0 ? (e.vx + 8) / 16 : -((-e.vx + 8) / 16));
        if (e.x <= e.x0) { e.x = e.x0; e.vx = -e.vx; }
        if (e.x >= e.x1) { e.x = e.x1; e.vx = -e.vx; }
        e.y = settle(e.x, e.y - 8);
        break;

      case EK_BIRD: {
        // drifts along its box, leaning toward the hero
        const int toward = (_hero.x > e.x) ? 1 : -1;
        e.x += toward * ((e.vx + 8) / 16);
        if (e.x < e.x0) e.x = e.x0;
        if (e.x > e.x1) e.x = e.x1;
        e.y = e.homeY + ((_tick / 3) % 24) - 12;
        break;
      }

      case EK_GUARD:
        e.x += (e.vx >= 0 ? (e.vx + 8) / 16 : -((-e.vx + 8) / 16));
        if (e.x <= e.x0) { e.x = e.x0; e.vx = -e.vx; }
        if (e.x >= e.x1) { e.x = e.x1; e.vx = -e.vx; }
        e.y = settle(e.x, e.y - 8);
        if (e.cool) e.cool--;
        break;

      case EK_FIRE:
      default:
        break;
    }

    if ((_tick & 3) == 0) e.anim++;

    // contact with the hero
    if (_hero.state != HS_DEAD && !_hero.invuln &&
        abs(_hero.x - e.x) < 18 && abs((_hero.y - 16) - (e.y - 14)) < 26)
      hurt();
  }
}

// ---------------------------------------------------------------------------
// The Great Hall.
//
// It is drawn in one-point perspective, so the doors are up on the back wall
// and cannot be walked into. Touch suits it better than the keyboard ever did:
// tap a door and the hero runs to it, it opens, he steps through, it shuts.
// ---------------------------------------------------------------------------
static const int HUB_OPEN_STEPS = 5;

void Game::hubTap(int x, int y) {
  if (_hub != HUB_IDLE) return;
  // Not for the first few ticks after arriving. A finger still resting on the
  // glass from the tap that entered the hall would otherwise pick a door
  // before the room has even been looked at.
  if (_exitLock) return;
  // Nearest door to the tap, provided the tap is anywhere near one. Judged on
  // x alone: the doors are far apart horizontally and the perspective puts
  // them at wildly different heights, so demanding a hit on the door's own
  // rect would make the small far ones fiddly.
  int best = -1, bestd = 1 << 30;
  for (int i = 0; i < HALL_DOOR_COUNT; i++) {
    const HallDoor &d = HALL_DOORS[i];
    const int cx = d.x + d.w / 2;
    const int dist = (x > cx) ? x - cx : cx - x;
    if (dist < bestd) { bestd = dist; best = i; }
  }
  if (best < 0 || bestd > 110) return;
  _hubDoor = (int8_t)best;
  _hub = HUB_WALK;
  _hubStep = 0;
}

void Game::hubSelect(int door) {
  if (!isHub() || _hub != HUB_IDLE) return;
  if (door < 0 || door >= HALL_DOOR_COUNT) return;
  _hubDoor = (int8_t)door;
  _hub = HUB_WALK;
  _hubStep = 0;
}

void Game::hubUpdate() {
  if (_hubDoor < 0) return;
  const HallDoor &d = HALL_DOORS[_hubDoor];

  switch (_hub) {
    case HUB_WALK: {
      const int dx = d.walkX - _hero.x;
      if (dx > 2 || dx < -2) {
        _hero.faceLeft = dx < 0;
        _hero.x += (dx > 0) ? 3 : -3;
        _hero.state = HS_RUN;
        if ((_tick & 1) == 0) {
          int id, n; spriteFor(HS_RUN, _hero.faceLeft, id, n);
          _hero.anim++;
          _hero.frame = n ? (_hero.anim % n) : 0;
        }
      } else {
        _hero.x = d.walkX;
        _hero.state = HS_STAND;
        _hero.frame = 0;
        _hub = HUB_OPEN;
        _hubStep = 0;
      }
      break;
    }
    case HUB_OPEN:
      if (++_hubStep >= HUB_OPEN_STEPS) { _hub = HUB_ENTER; _hubStep = 0; }
      break;
    case HUB_ENTER:
      // He walks into the dark of the doorway and is gone.
      if (++_hubStep >= 4) { _hub = HUB_SHUT; _hubStep = HUB_OPEN_STEPS; }
      break;
    case HUB_SHUT:
      if (_hubStep == 0) {
        const uint8_t to = d.toRoom;
        const int16_t sx = d.spawnX, sy = d.spawnY;
        _hub = HUB_IDLE;
        _hubDoor = -1;
        enterRoom(to, sx, sy);
      } else {
        _hubStep--;
      }
      break;
    default:
      break;
  }
}

void Game::hubDraw() {
  if (_hubDoor < 0 || _hub == HUB_IDLE) return;
  const HallDoor &d = HALL_DOORS[_hubDoor];
  const int step = (_hubStep > HUB_OPEN_STEPS) ? HUB_OPEN_STEPS : _hubStep;

  if (d.sprite) {
    // An original opening animation, where one matches the door's perspective.
    Cel *c = cel(d.sprite);
    if (c) {
      const int n = c->meta->frames;
      int f = step * n / (HUB_OPEN_STEPS + 1);
      if (f >= n) f = n - 1;
      _v->blit(*c, f, d.x, d.y);
      mark({d.x, d.y, c->meta->w, c->meta->h});
      return;
    }
  }
  // Otherwise swing it open by darkening the doorway from one edge.
  const int w = d.w * step / HUB_OPEN_STEPS;
  if (w > 0) {
    Rect r = {(int16_t)(d.x + d.w - w), d.y, (int16_t)w, d.h};
    _v->fillRect(r, true);
    mark({d.x, d.y, d.w, d.h});
  }
}

// ---------------------------------------------------------------------------
bool Game::update(const Input &in) {
  _tick++;

  // Erase last frame's moving parts.
  for (int i = 0; i < _nlast; i++) _v->restore(_last[i]);
  for (int i = 0; i < _nlast; i++) _v->dirty(_last[i]);
  _nlast = 0;

  if (_hero.state == HS_DEAD) return true;

  // In the hall a tap picks a door rather than throwing, and while the door
  // sequence runs the hero is not under manual control.
  if (isHub()) {
    if (in.fire) hubTap(in.aimX, in.aimY);
    if (_hub != HUB_IDLE) {
      hubUpdate();
      return true;
    }
  } else if (in.fire && !_hero.busy) {
    throwRock(in.aimX, in.aimY);
  }

  moveHero(in);
  updateRocks();
  updateEnemies();

  // doorways
  if (_exitLock) { _exitLock--; return true; }
  const RoomDef &r = ROOMS[_room];
  for (int i = 0; i < r.nexits; i++) {
    const Exit &x = r.exits[i];
    if (_hero.x >= x.x && _hero.x < x.x + x.w &&
        _hero.y >= x.y && _hero.y < x.y + x.h) {
      enterRoom(x.toRoom, x.spawnX, x.spawnY);
      return true;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
void Game::draw() {
  // enemies behind the hero
  for (int i = 0; i < MAX_ENEMIES; i++) {
    Enemy &e = _en[i];
    if (e.kind == EK_NONE || !e.alive) continue;
    int id = SPR_CREATURE;
    if (e.kind == EK_BIRD)   id = SPR_BIRD;
    if (e.kind == EK_GUARD) id = SPR_GUARD;
    if (e.kind == EK_FIRE)  id = SPR_FIRE;
    Cel *c = cel(id);
    if (!c) continue;
    const int n = c->meta->frames;
    const int f = n ? (e.anim % n) : 0;
    const int dx = e.x - c->meta->w / 2;
    const int dy = e.y - c->meta->h;
    _v->blit(*c, f, dx, dy, e.vx < 0);
    Rect rr = {(int16_t)dx, (int16_t)dy, c->meta->w, c->meta->h};
    mark(rr);
  }

  // rocks
  Cel *rc = cel(SPR_ROCK);
  for (int i = 0; i < MAX_ROCKS; i++) {
    Rock &r = _rocks[i];
    if (!r.live || !rc) continue;
    const int n = rc->meta->frames;
    const int dx = r.x / 16 - rc->meta->w / 2;
    const int dy = r.y / 16 - rc->meta->h / 2;
    _v->blit(*rc, n ? (r.frame % n) : 0, dx, dy);
    Rect rr = {(int16_t)dx, (int16_t)dy, rc->meta->w, rc->meta->h};
    mark(rr);
  }

  hubDraw();

  // hero, blinking while invulnerable -- and out of sight once he is through
  // the hall door
  const bool inDoor = isHub() && (_hub == HUB_ENTER || _hub == HUB_SHUT);
  if (!inDoor && !(_hero.invuln && (_tick & 2))) {
    int id, n;
    spriteFor(_hero.state, _hero.faceLeft, id, n);
    Cel *c = cel(id);
    if (c) {
      const int dx = _hero.x - c->meta->w / 2;
      const int dy = _hero.y - c->meta->h;
      _v->blit(*c, (n && _hero.frame < n) ? _hero.frame : 0, dx, dy);
      Rect rr = {(int16_t)dx, (int16_t)dy, c->meta->w, c->meta->h};
      mark(rr);
    }
  }
}

}  // namespace dc

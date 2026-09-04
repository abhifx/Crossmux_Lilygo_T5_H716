// =============================================================================
//  Dark Castle  --  a port of the 1986 Macintosh game to EPD_Painter
//
//  Not an emulator. The original 68000 game is not run in any form: its
//  artwork is extracted from a disk image you supply, and the game around it
//  is written fresh against this library.
//
//  Before this will build, generate the assets from your own copy of the game:
//
//      python3 tools/dc_extract.py /path/to/DarkCastle_1_2.dsk
//
//  which writes dc_gfx_screens.cpp and dc_gfx_sprites.cpp beside this sketch.
//  Nothing from the game is distributed here.
//
//  ---------------------------------------------------------------------------
//  Two things about this hardware are not like a Mac, and both change the
//  design rather than just the plumbing:
//
//  Shape. The Mac was 512x342 in 4:3; this panel is 960x540. Rather than
//  pillarbox and waste it, the playfield scales by exactly 3/2 to 768x465 and
//  the 192px left over becomes a permanent control column. Two source pixels
//  always become three, so the scaling is periodic and sprites do not shimmer
//  as they cross the screen.
//
//  Input. Dark Castle aimed thrown rocks at the mouse pointer and moved on the
//  keyboard. There is no pointer here, so the gesture becomes more direct:
//  TAP THE PLAYFIELD WHERE YOU WANT THE ROCK TO GO. Movement moves to the
//  touch column. Serial keys work too, which is handy for debugging.
//
//  Controls
//    touch column ......... left / right / jump / duck / run
//    tap the playfield .... throw a rock at that spot
//    serial ............... a d = left right, w = jump, s = duck,
//                           shift = run, r = restart
// =============================================================================

// Pick your board, or leave commented for auto-detect.
//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS
//#define EPD_PAINTER_PRESET_M5PAPER_S3

#include <Arduino.h>
#include <Wire.h>
#include "EPD_Painter_Adafruit.h"
#include "EPD_Painter_presets.h"
#include <gt911_lite.h>

#include "dc_video.h"
#include "dc_game.h"
#include "dc_rooms.h"
#include "dc_ui.h"

static EPD_PainterAdafruit epd(EPD_PAINTER_PRESET);
static GT911_Lite  touch;
static dc::Video   video;
static dc::Game    game;

// One quality for everything, sprites included.
//
// FAST is quick but shallow: run a Dark Castle room through it and the black
// slabs come out mid-grey. It would buy roughly 23 fps, which this game has no
// use for -- a platformer at a walking pace reads fine in the low teens, and
// the black depth is worth far more than the frames.
//
// Mixing the two was tried: scene at NORMAL, sprites at FAST. It works, because
// the delta engine only ever re-drives pixels that actually changed, but it
// leaves shallow patches on the background wherever a FAST repaint restored it
// behind a sprite, and those accumulate as you move around a room. Painting
// everything at one quality makes the problem not exist.
//
// clear() therefore happens only when the room changes, where the screen is
// being replaced anyway. On a timer it is just a flash mid-play.
static EPD_Painter::Quality GAME_QUALITY = EPD_Painter::Quality::QUALITY_NORMAL;

// NORMAL rather than HIGH, and that is not only about depth: it is the quality
// the board's scanner-tuned trains exist for.
//
// setDirectTransitions() below loads the tuned direct grey-to-grey trains from
// EPD_Painter_trains.h, which matter a lot here -- this artwork is wall-to-wall
// grey-on-grey, and without them every transition takes the two-step route via
// white, which flashes and costs passes. But TrainTables carries dir_normal and
// dir_fast and NOTHING ELSE; there is no dir_high, and setQuality() keeps the
// loaded set matched to the quality. Selecting HIGH therefore unloads the tuned
// pairs. The board says so itself at boot:
//
//   quality=NORMAL  direct=on  trains=board-tuned
//   quality=HIGH    direct=on  trains=two-step (no tuned set at this quality)
//
// Press 1/2/3 on serial to switch quality, t to toggle the direct engine; each
// forces a full re-lay so the difference is visible immediately.
static bool WANT_DIRECT = true;

static void applyQuality(EPD_Painter::Quality q) {
  GAME_QUALITY = q;
  epd.setQuality(q);
  const bool ok = epd.driver().setDirectTransitions(WANT_DIRECT);
  const auto &t = epd.getConfig().trains;
  const bool tuned = (q == EPD_Painter::Quality::QUALITY_NORMAL && t.dir_normal)
                  || (q == EPD_Painter::Quality::QUALITY_FAST   && t.dir_fast);
  Serial.printf("[dc] quality=%s  direct=%s  trains=%s\n",
                q == EPD_Painter::Quality::QUALITY_FAST   ? "FAST" :
                q == EPD_Painter::Quality::QUALITY_NORMAL ? "NORMAL" : "HIGH",
                (WANT_DIRECT && ok) ? "on" : "off",
                tuned ? "board-tuned" : "two-step (no tuned set at this quality)");
}

// ---- touch -----------------------------------------------------------------
static uint16_t tp_xmax = 960, tp_ymax = 540;
static bool     tp_swap = false, touch_ok = false;

static void touchInit() {
  TwoWire *bus = epd.getConfig().i2c.wire;
  if (!bus) return;
  touch.begin(bus);
  for (uint8_t addr : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    bus->beginTransmission(addr);
    bus->write(0x81); bus->write(0x46);
    if (bus->endTransmission(false) != 0) continue;
    if (bus->requestFrom(addr, (uint8_t)4) != 4) continue;
    uint16_t xm = bus->read(); xm |= bus->read() << 8;
    uint16_t ym = bus->read(); ym |= bus->read() << 8;
    if (xm == 0 || ym == 0 || xm == 0xFFFF) break;
    tp_xmax = xm; tp_ymax = ym;
    tp_swap = (xm < ym);
    touch_ok = true;
    Serial.printf("[dc] touch 0x%02x, range %ux%u%s\n", addr, xm, ym,
                  tp_swap ? " (swapped)" : "");
    return;
  }
  Serial.println("[dc] no touch controller - serial controls only");
}

// Current touch point in panel pixels. Returns false when nothing is down.
static bool touchPoint(int &px, int &py) {
  if (!touch_ok) return false;
  touch.read();
  if (!touch.isTouched) return false;
  const uint16_t rx = touch.x, ry = touch.y;
  if (tp_swap) {
    px = (int)ry * 960 / tp_ymax;
    py = (int)(tp_xmax - rx) * 540 / tp_xmax;
  } else {
    px = (int)rx * 960 / tp_xmax;
    py = (int)ry * 540 / tp_ymax;
  }
  return true;
}

// ---- the control column ----------------------------------------------------
// Button geometry lives in dc_ui.h; see the note there about why.
static void drawBtn(const Btn &b, bool active) {
  epd.fillRect(b.x, b.y, b.w, b.h, active ? 2 : 0);
  epd.drawRect(b.x, b.y, b.w, b.h, 3);
  epd.drawRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 3);
  epd.setTextColor(3);
  epd.setTextSize(2);
  const int tw = strlen(b.label) * 12;
  epd.setCursor(b.x + (b.w - tw) / 2, b.y + b.h / 2 - 8);
  epd.print(b.label);
}

// The panel only needs redrawing when something on it changed.
static uint32_t panel_sig = 0xFFFFFFFF;

// Set once setup() has laid the first room down, so the first pass through
// loop() does not treat it as a room change and clear the screen again.
static int last_room = -1;

// ---- title card -------------------------------------------------------------
// The game opens on the original title screen, as it did on the Mac. It is
// PSCR 12010, the night version with the filled moon -- 12000 is the daylight
// variant. Title cards use all 342 source rows rather than the 310-row
// playfield, so the view is taller and sits higher; setFullFrame() handles it.
static const int TITLE_PSCR = 12010;
enum Mode : uint8_t { MODE_TITLE, MODE_PLAY };
static Mode mode = MODE_TITLE;

static void showTitle() {
  mode = MODE_TITLE;
  video.setFullFrame(true);
  video.loadScreen(TITLE_PSCR);
  epd.fillScreen(0);
  video.present();

  epd.setTextColor(3);
  epd.setTextSize(2);
  epd.setCursor(dc::PANEL_X + 14, 250);
  epd.print("TAP TO");
  epd.setCursor(dc::PANEL_X + 14, 274);
  epd.print("BEGIN");
  epd.setTextSize(1);
  epd.setCursor(dc::PANEL_X + 14, 320);
  epd.print("EPD_Painter port");
  epd.setCursor(dc::PANEL_X + 14, 336);
  epd.print("of the 1986 Mac");
  epd.setCursor(dc::PANEL_X + 14, 352);
  epd.print("game by Silicon");
  epd.setCursor(dc::PANEL_X + 14, 368);
  epd.print("Beach Software");

  epd.clear();
  epd.paint();
  panel_sig = 0xFFFFFFFF;     // force the status column to redraw on entry
}

static void startGame() {
  mode = MODE_PLAY;
  video.setFullFrame(false);
  game.restart();
  epd.fillScreen(0);
  game.draw();
  video.present();
  last_room = game.room();
  epd.clear();
  epd.paint();
}

// Returns true when it actually redrew. Redrawing the column every frame would
// hand the delta engine a screenful of unchanged text to compare.
static bool drawPanel(bool leftHeld, bool rightHeld, bool runHeld) {
  const uint32_t sig = (game.score() << 8) ^ (game.lives() << 4) ^
                       (game.hero().health & 15) ^ (game.room() << 12) ^
                       (leftHeld ? 1u : 0) ^ (rightHeld ? 2u : 0) ^
                       (runHeld ? 4u : 0);
  if (sig == panel_sig) return false;
  panel_sig = sig;

  epd.fillRect(dc::PANEL_X, 0, dc::PANEL_W, 540, 0);
  epd.drawFastVLine(dc::PANEL_X, 0, 540, 3);

  epd.setTextColor(3);
  epd.setTextSize(2);
  epd.setCursor(dc::PANEL_X + 12, 16);
  epd.print("SCORE");
  epd.setTextSize(3);
  epd.setCursor(dc::PANEL_X + 12, 40);
  epd.printf("%lu", (unsigned long)game.score());

  epd.setTextSize(2);
  epd.setCursor(dc::PANEL_X + 12, 84);
  epd.printf("LIVES %d", game.lives());

  epd.setCursor(dc::PANEL_X + 12, 110);
  epd.print("LIFE");
  for (int i = 0; i < 3; i++) {
    const int x = dc::PANEL_X + 70 + i * 22;
    if (i < game.hero().health) epd.fillRect(x, 108, 16, 16, 3);
    else                        epd.drawRect(x, 108, 16, 16, 3);
  }

  epd.setTextSize(1);
  epd.setCursor(dc::PANEL_X + 12, 140);
  epd.print(game.roomName());

  epd.setCursor(dc::PANEL_X + 12, 160);
  epd.print("tap scene to throw");

  drawBtn(BTN_JUMP, false);
  drawBtn(BTN_LEFT, leftHeld);
  drawBtn(BTN_RIGHT, rightHeld);
  drawBtn(BTN_DUCK, false);
  drawBtn(BTN_RUN, runHeld);

  if (game.dead()) {
    epd.setTextSize(2);
    epd.setCursor(dc::PANEL_X + 12, 480);
    epd.print("GAME OVER");
    epd.setTextSize(1);
    epd.setCursor(dc::PANEL_X + 12, 505);
    epd.print("tap here to restart");
  }
  return true;
}

// ---- serial fallback -------------------------------------------------------
static bool key_left = false, key_right = false, key_jump = false;
static bool key_duck = false, key_run = false;

static void pollSerial() {
  while (Serial.available()) {
    const int c = Serial.read();
    switch (c) {
      case 'a': key_left = true;  key_right = false; break;
      case 'd': key_right = true; key_left = false;  break;
      case 'q': key_left = key_right = false;        break;
      case 'w': key_jump = true;                     break;
      case 's': key_duck = !key_duck;                break;
      case 'S': key_run = !key_run;                  break;
      case 'r': showTitle();                         return;
      // Jump straight to a room. Handy when you are testing the artwork or
      // the derived collision and do not want to walk there.
      case 'n': game.enterRoom((game.room() + 1) % dc::ROOM_COUNT);  break;
      case 'p': game.enterRoom((game.room() + dc::ROOM_COUNT - 1) %
                               dc::ROOM_COUNT);                      break;
      // Compare black depth on a given panel. Each forces a full re-lay so
      // the change is visible immediately.
      case '1': case '2': case '3':
        applyQuality(c == '1' ? EPD_Painter::Quality::QUALITY_FAST
                   : c == '2' ? EPD_Painter::Quality::QUALITY_NORMAL
                              : EPD_Painter::Quality::QUALITY_HIGH);
        epd.clear();
        epd.paint();
        break;
      // Pick a Great Hall door without touching the glass, for testing.
      case 'z': case 'x': case 'c': case 'v':
        game.hubSelect(c == 'z' ? 0 : c == 'x' ? 1 : c == 'c' ? 2 : 3);
        break;
      case 't':                       // tuned direct trains on / off
        WANT_DIRECT = !WANT_DIRECT;
        applyQuality(GAME_QUALITY);
        epd.clear();
        epd.paint();
        break;
      default: break;
    }
  }
}

// ---- setup / loop ----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[dc] Dark Castle");

  if (!epd.begin()) {
    Serial.println("[dc] EPD init failed");
    for (;;) delay(1000);
  }
  applyQuality(GAME_QUALITY);
  touchInit();

  if (!video.begin(&epd)) {
    Serial.println("[dc] out of memory for the frame buffers");
    for (;;) delay(1000);
  }
  game.begin(&video);
  Serial.println("[dc] video ok"); Serial.flush();

  epd.clear();
  epd.fillScreen(0);
  showTitle();
  Serial.println("[dc] title"); Serial.flush();


  Serial.printf("[dc] %d rooms, %d screens, %d sprites\n",
                dc::ROOM_COUNT, DC_SCREEN_COUNT, DC_SPRITE_COUNT);
}

void loop() {
  pollSerial();

  dc::Input in;
  static bool was_down = false;
  static bool run_latch = false;
  int px = 0, py = 0;
  const bool down = touchPoint(px, py);

  // Title card: anything at all starts the game.
  if (mode == MODE_TITLE) {
    if ((down && !was_down) || key_jump || key_left || key_right) {
      key_jump = key_left = key_right = false;
      startGame();
    }
    was_down = down;
    delay(30);
    return;
  }

  bool leftHeld = false, rightHeld = false;

  if (down) {
    if (px >= dc::PANEL_X) {
      if (inBtn(BTN_LEFT, px, py))  leftHeld = true;
      if (inBtn(BTN_RIGHT, px, py)) rightHeld = true;
      if (inBtn(BTN_JUMP, px, py))  in.jump = true;
      if (inBtn(BTN_DUCK, px, py))  in.duck = true;
      if (inBtn(BTN_RUN, px, py) && !was_down) run_latch = !run_latch;
      if (game.dead() && py > 460 && !was_down) { showTitle(); return; }
    } else if (!was_down) {
      // A fresh tap in the scene is a throw, aimed where it landed.
      in.fire = true;
      in.aimX = (px - dc::VIEW_X) * 2 / 3;
      in.aimY = (py - dc::VIEW_Y) * 2 / 3;
    }
  }
  was_down = down;

  in.left  = leftHeld  || key_left;
  in.right = rightHeld || key_right;
  in.jump  = in.jump   || key_jump;
  in.duck  = in.duck   || key_duck;
  in.run   = run_latch || key_run;
  key_jump = false;

  game.update(in);
  game.draw();
  video.present();
  drawPanel(in.left, in.right, in.run);

  if (game.room() != last_room) {
    // Between rooms is the one moment a full clear is free: the screen is
    // being replaced anyway, so it costs nothing and takes the ghosting with
    // it. Nowhere else.
    last_room = game.room();
    epd.clear();
  }
  epd.paint();

  // Frame rate, once a second. An e-paper panel is the budget here, not the
  // simulation, so this is the number that matters when tuning movement.
  static uint32_t fps_t = 0, fps_n = 0;
  fps_n++;
  const uint32_t now = millis();
  if (now - fps_t >= 1000) {
    Serial.printf("[dc] %lu fps  room=%s  hero=%d,%d\n",
                  (unsigned long)fps_n, game.roomName(),
                  game.hero().x, game.hero().y);
    fps_n = 0;
    fps_t = now;
  }
}

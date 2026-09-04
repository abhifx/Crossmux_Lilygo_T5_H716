// Breakout Demo — autonomous multi-ball brick basher, no paddle required.
// Balls bounce forever; bricks reset when all cleared.
// Power off via Boot button (battery only — does not work when plugged in via USB).


// Pick a board...
//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS
//#define EPD_PAINTER_PRESET_LILYGO_T5_S3_H752
//#define EPD_PAINTER_PRESET_M5PAPER_S3

#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "EPD_Painter_presets.h"

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>

EPD_PainterAdafruit epd(EPD_PAINTER_PRESET);


// Power off only works with LILYGO T5
#ifdef EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS
XPowersPPM PPM;
#endif

// --- Game constants ---
#define BALL_SIZE    23
#define BALL_SPEED   24.0f
#define NUM_BALLS    3

#define BRICK_COLS   8
#define BRICK_ROWS   6
#define BRICK_PAD    3
#define BRICK_TOP    10
#define BRICK_H      28

// --- Game state ---
struct Ball {
  float x, y;
  float vx, vy;
  uint8_t color;
};

// Two levels only: paper and ink. In 4-level NORMAL a white<->black change
// is a single drive cycle — an apply on the dark plane or a remove on the
// light plane — whereas any grey-to-grey pixel needs either a two-step
// through white or a direct train. Restricting the palette to the endpoints
// removes that case entirely rather than optimising it.
static const uint8_t PAPER = 0, INK = 3;

struct Brick {
  bool alive;
  uint8_t color;
};

Ball balls[NUM_BALLS];
Brick bricks[BRICK_ROWS][BRICK_COLS];

int screenW, screenH;
int brickW;

void initBricks() {
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < BRICK_COLS; c++) {
      bricks[r][c].alive = true;
      // One grey per row, light at the top down to near-black: the whole
      // tuned ramp on screen at once, and every ball crossing exercises a
      // different grey-to-grey pair.
      // TWO COLOUR: ink or paper, nothing between.
      //
      // These were 2,4,6,8,10,12 — SIXTEEN-grey codes in a sketch that runs
      // at FOUR levels, where the packer masks every byte with 0x03. They
      // wrapped to 2,0,2,0,2,0, so rows 1, 3 and 5 were being drawn in
      // white on white and never appeared at all. A level code authored for
      // 16 greys does not degrade in 4-level mode, it wraps — always
      // quantise against greyLevels(), never against a fixed 16.
      bricks[r][c].color = INK;
    }
  }
}

void initBalls() {
  // Each ball starts from a different position with a different angle
  balls[0] = { screenW * 0.25f, screenH - 40.0f, BALL_SPEED * 0.7f,  -BALL_SPEED,        3 };
  balls[1] = { screenW * 0.50f, screenH - 60.0f, -BALL_SPEED * 0.5f, -BALL_SPEED * 0.9f, 2 };
  balls[2] = { screenW * 0.75f, screenH - 50.0f, BALL_SPEED * 0.6f,  -BALL_SPEED * 0.8f, 1 };
}

void initGame() {
  screenW = epd.width();
  screenH = epd.height();
  brickW = (screenW - BRICK_PAD) / BRICK_COLS - BRICK_PAD;
  initBricks();
  initBalls();
}

void brickRect(int r, int c, int &bx, int &by, int &bw, int &bh) {
  bw = brickW;
  bh = BRICK_H;
  bx = BRICK_PAD + c * (brickW + BRICK_PAD);
  by = BRICK_TOP + r * (BRICK_H + BRICK_PAD);
}

void updateBall(Ball &ball) {
  ball.x += ball.vx;
  ball.y += ball.vy;

  if (ball.x <= 0) {
    ball.x = 0;
    ball.vx = -ball.vx;
  } else if (ball.x + BALL_SIZE >= screenW) {
    ball.x = screenW - BALL_SIZE;
    ball.vx = -ball.vx;
  }

  if (ball.y <= 0) {
    ball.y = 0;
    ball.vy = -ball.vy;
  }

  if (ball.y + BALL_SIZE >= screenH) {
    ball.y = screenH - BALL_SIZE;
    ball.vy = -ball.vy;
  }

  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < BRICK_COLS; c++) {
      if (!bricks[r][c].alive) continue;

      int bx, by, bw, bh;
      brickRect(r, c, bx, by, bw, bh);

      if (ball.x + BALL_SIZE > bx && ball.x < bx + bw &&
          ball.y + BALL_SIZE > by && ball.y < by + bh) {

        bricks[r][c].alive = false;

        float overlapLeft   = (ball.x + BALL_SIZE) - bx;
        float overlapRight  = (bx + bw) - ball.x;
        float overlapTop    = (ball.y + BALL_SIZE) - by;
        float overlapBottom = (by + bh) - ball.y;

        float minOverlapX = min(overlapLeft, overlapRight);
        float minOverlapY = min(overlapTop, overlapBottom);

        if (minOverlapX < minOverlapY) {
          ball.vx = -ball.vx;
        } else {
          ball.vy = -ball.vy;
        }

        return; // one brick per ball per frame
      }
    }
  }
}

void drawBricks() {
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < BRICK_COLS; c++) {
      if (!bricks[r][c].alive) continue;
      int bx, by, bw, bh;
      brickRect(r, c, bx, by, bw, bh);
      epd.fillRect(bx, by, bw, bh, bricks[r][c].color);
    }
  }
}

void drawBall(const Ball &ball) {
  epd.fillCircle((int)ball.x + BALL_SIZE / 2, (int)ball.y + BALL_SIZE / 2, BALL_SIZE / 2, INK);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  if (!epd.begin()) {
    Serial.println("EPD init failed");
    while (1);
  }

  // 16-grey HIGH. Grey-to-grey needs no separate direct engine at this
  // tier — the 16-grey engine drives remove(from) and apply(to) in ONE
  // paint (temporal partition), so a ball crossing a brick never flashes
  // through white. The tables come from the board's tuned set: the NVS
  // blob a tuneup session stored, or the per-board *_Trains.h defaults.
  // ~2.5 fps: 20 passes at the constant 20 ms period is the price of
  // sixteen calibrated greys at their best.
  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
 // if (!epd.driver().setGreyLevels(16))
  //  Serial.println("16-grey unavailable - staying at 4 levels");
  // Inter-pass gap: 4 ms -> 1 ms. Worth 13 x 3 ms = 39 ms off a ~125 ms
  // frame. Safe HERE because this sketch is two-colour — level 3 is a full
  // darken run and level 0 a full whiten run, both saturated, so a shorter
  // settle barely moves them. A 4-grey sketch would need its levels
  // re-measured after this.
  epd.driver()._config.pass_gap_us_normal = 1000;

  epd.driver().setPaintProfile(true);   // one line per drive cycle

  epd.clear();

  epd.fillScreen(INK);
  epd.paint();
  epd.fillScreen(PAPER);
  epd.paint();

  const auto& cfg = epd.getConfig();

#ifdef EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS
 bool result = PPM.init(*cfg.i2c.wire, cfg.i2c.sda, cfg.i2c.scl, BQ25896_SLAVE_ADDRESS);
 if (!result) {
   while (1) {
     Serial.println("PPM is not online...");
     delay(1000);
   }
 }
 #endif

  //pinMode(0, INPUT);

  epd.setTextSize(3);
  initGame();
}

void loop() {

  long time = esp_timer_get_time();

  epd.fillScreen(PAPER);


  for (int i = 0; i < NUM_BALLS; i++) {
    updateBall(balls[i]);
  }

  drawBricks();

  for (int i = 0; i < NUM_BALLS; i++) {
    drawBall(balls[i]);
  }

  // Reset bricks when all destroyed
  bool anyAlive = false;
  for (int r = 0; r < BRICK_ROWS && !anyAlive; r++)
    for (int c = 0; c < BRICK_COLS && !anyAlive; c++)
      if (bricks[r][c].alive) anyAlive = true;
  if (!anyAlive) initBricks();

//  long time = esp_timer_get_time();
 // long time = esp_timer_get_time();


  //epd.setCursor(30,500);
  //epd.print("Turn off by pressing Boot button.");

  epd.paint();
 // Frame rate, plus the drive-cycle count so a frame that quietly costs two
 // cycles is visible rather than just "slow".
 {
   static uint32_t lastPaints = 0;
   const uint32_t done = epd.driver().paintsCompleted();
   Serial.printf("[breakout] %.1f fps  (%lu cycle%s this frame)\n",
                 1000000.0f / (float)(esp_timer_get_time() - time),
                 (unsigned long)(done - lastPaints),
                 (done - lastPaints) == 1 ? "" : "s");
   lastPaints = done;
 }


}
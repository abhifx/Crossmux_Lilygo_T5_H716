#ifndef EPD_Painter_H
#define EPD_Painter_H

#include <stddef.h>
#include <stdint.h>

// Pin encoding: set bit 8 to indicate the pin lives on the 74HCT4094D shift
// register.  Bits 0–7 hold the output index (QP0..QP7).
// Example: EPD_SR_PIN(4) → shift-register output QP4 (EP_STV/SPV on H752).
// A value of -1 means the pin is absent / managed elsewhere.
#define EPD_SR_PIN(n)  (int16_t(0x100 | (n)))

static inline bool    epd_pin_is_sr(int16_t p)  { return (p & 0x100) != 0; }
static inline uint8_t epd_pin_sr_bit(int16_t p) { return uint8_t(p & 0xFF); }

#if !defined(EPD_PAINTER_PRESET_M5PAPER_S3)       && \
    !defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS) && \
    !defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_H752) && \
    !defined(EPD_PAINTER_PRESET_LILYGO_EPD47_H716)
  
  #ifndef EPD_PAINTER_PRESET_AUTO
    #define EPD_PAINTER_PRESET_AUTO
  #endif
#endif

// Forward declarations — avoid circular includes
class EPD_PainterShutdown;
class EPD_PinDriver;
class EPD_PowerDriver;
class EPD_ISRController;

// FreeRTOS headers — available in both Arduino-ESP32 and pure ESP-IDF
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>


#include <esp_private/gdma.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <hal/dma_types.h>
#include <esp_intr_alloc.h>

// I2C — TwoWire for Arduino only; ESP-IDF builds don't use I2C
#ifdef ARDUINO
  #include <Wire.h>
#endif


class EPD_Painter {

public:

struct I2CBusConfig {
#ifdef ARDUINO
    TwoWire* wire = nullptr;
#else
    void* i2c_bus = nullptr;  // unused in ESP-IDF builds
#endif
    int sda = -1;
    int scl = -1;
    uint32_t freq = 100000;
};
struct PowerCtlConfig {
    int pca_addr=-1;
    int tps_addr=-1;
};
  enum class Quality {
    QUALITY_HIGH,
    QUALITY_NORMAL,
    QUALITY_FAST
  };

  enum class Rotation {
    ROTATION_0,   // landscape — normal orientation
    ROTATION_CW,  // 90° clockwise — portrait drawing canvas (width↔height swapped)
    ROTATION_180  // landscape, flipped — same dims as ROTATION_0, image reversed at pack
  };

  struct Waveforms {
      uint8_t fast_lighter[3][7];
      uint8_t fast_darker[3][7];
      uint8_t normal_lighter[3][13];
      uint8_t normal_darker[3][13];
      uint8_t high_lighter[3][13];
      uint8_t high_darker[3][13];
  };

  struct Shift {
    int8_t data    = -1;
    int8_t clk     = -1;
    int8_t strobe  = -1;
    int8_t le_time = 0;
    enum Driver { H752, H716 } driver = H752;
  };

  // Scanner-tuned decision-engine tables for a board (phase D — see
  // DECISION_ENGINE.md and EPD_Painter_trains.h). All optional: a null
  // g16 pair leaves the 16-grey mode on the formula library; a null
  // direct table leaves that quality on the two-step transition path.
  // Array bounds are DEC_WF_LEN16 / DEC_WF_LEN_DIR (asserted below).
  //
  // The 16-grey tables come in one set PER QUALITY, because a train is
  // only valid at the LENGTH it was calibrated at: NORMAL spends 13
  // passes, HIGH spends 20. The pass period is the same for both, so the
  // extra seven are not extra dose — the panel saturates around pass 10
  // either way — they are extra ROOM. Every level below the saturation
  // knee has to be placed by whitening and re-darkening rather than by
  // stretching a darken run that has stopped responding, and those
  // shapes need spare slots to fit. A board may be tuned for one quality
  // and not the other; the unset pair falls back to the formula library
  // for that quality alone.
  struct TrainTables {
      const uint8_t (*g16_apply)[13]      = nullptr;  // [16][13] NORMAL applies
      const uint8_t (*g16_remove)[13]     = nullptr;  // [16][13] NORMAL removes
      const uint8_t (*g16_apply_high)[32] = nullptr;  // [16][32] HIGH applies
      const uint8_t (*g16_remove_high)[32]= nullptr;  // [16][32] HIGH removes
      const uint8_t (*dir_normal)[4][26]  = nullptr;  // [4][4][26] NORMAL direct trains
      const uint8_t (*dir_fast)[4][26]    = nullptr;  // [4][4][26] FAST direct trains
  };


  struct Config {
      uint16_t width;
      uint16_t height;
      int16_t pin_pwr;
      int16_t pin_syspwr = -1;
      int16_t pin_sph;
      int16_t pin_oe;
      int16_t pin_cl;
      int16_t pin_spv;
      int16_t pin_ckv;
      int16_t pin_le;
      Quality  quality;
      Rotation rotation = Rotation::ROTATION_0;

      // Dummy zero bytes clocked out after each row's pixel data (4 bytes =
      // 16 pixel clocks). The panel's source-driver shift chain is slightly
      // longer than the visible width; without this flush the last ~16
      // columns latch data belonging to pixels further left, repeating the
      // image at the right-hand edge. See How_It_Works.md §7.
      uint8_t row_pad_bytes = 4;
      int8_t data_pins[8];
      I2CBusConfig i2c{};
      PowerCtlConfig power{};
      Waveforms waveforms;

      Shift shift;

      // 16-grey constant pass period (microseconds). The pass loop pads
      // every pass to this so retention dose depends only on the trains,
      // and the trains are calibrated AT this period — so it is per-board:
      // it must exceed the board's worst-case row loop plus the settle
      // floor below. Boards whose control lines ride a shift register
      // (H716) have much slower rows and need a longer period.
      //
      // Times the train length, this is the frame cost: NORMAL 13 x 15 ms
      // ~ 195 ms, HIGH 20 x 15 ms ~ 300 ms.
      //
      // Both qualities run the SAME period. HIGH is not slower passes, it
      // is more of them: 20 slots against 13. Measured on a LilyGo (dose
      // ladder at 19/15/13 ms, 26 July 2026), a longer pass bought no
      // extra depth whatsoever - all three periods bottomed out at the
      // identical black - while making every slot coarser and, worse,
      // making most of them useless: at 19 ms the response stopped moving
      // after 7 passes, at 15 ms it was still resolving at 10. A longer
      // pass therefore costs accuracy AND wastes slots AND takes longer,
      // which is not a trade.
      // 4-LEVEL inter-pass gap (microseconds). Not used by 16-grey, which
      // pads to a constant period instead (g16_pass_us_* below).
      //
      // This is the single largest term in a 4-level frame that is not row
      // sending: at NORMAL it is 13 x 4 ms = 52 ms of a ~125 ms cycle,
      // measured with setPaintProfile(). It is time the panel spends
      // settling between passes, and the 4-level waveform tables are
      // CALIBRATED AT IT — shortening it shortens the dose each pass
      // delivers, so levels 1 and 2 will move and want re-tuning.
      //
      // Black and white are far less sensitive: level 3 is a full darken run
      // and level 0 a full whiten run, both well into saturation, so a
      // two-colour sketch can usually shorten this freely. A 4-grey one
      // cannot without re-measuring.
      int pass_gap_us_normal = 4000;
      int pass_gap_us_high   = 8000;

      int g16_pass_us_normal = 15000;
      int g16_pass_us_high   = 15000;

      // Minimum idle time appended to a pass when the row loop has already
      // eaten the period. It is a SAFETY FLOOR, not a tuning knob: if it
      // ever engages, that pass ran longer than the period and dose is
      // content-dependent again — the exact fault the constant period was
      // introduced to remove. Keep it comfortably below
      // (period - worst-case row loop), which on these boards is about
      // 10.5 ms at ten sweeps.
      //
      // One value for both qualities. HIGH used to hold 8000 against
      // NORMAL's 4000, which is what forced HIGH's period up to 19 ms and
      // therefore made its dose quantum 27% coarser than NORMAL's — for
      // settle time the panel demonstrably does not use. The float-gap
      // probe hands a pixel a WHOLE extra pass period of idle and measures
      // the effect as zero, thirteen times across three panels; 4 ms more
      // of the same is not going to register.
      // 1500, not 4000. The old 4 ms was pure margin, and it was expensive
      // margin: the budget for a row loop is (period - floor), so 4 ms of
      // floor meant rich content had only 11 ms to work in. A photograph
      // measures 13.3 ms, so the pass silently stretched to 17.3 ms and
      // every level on that content came out ~15% over-dosed - which is
      // exactly why tables tuned on the match card banded on real images.
      //
      // Reclaiming it is safe on the evidence: the float-gap probe hands a
      // pixel a WHOLE extra pass period of idle settle and measures the
      // effect as zero, thirteen times across three panels. Guaranteed
      // settle is not buying anything, and 1.5 ms leaves the budget at
      // 13.5 ms - above the worst row loop actually observed.
      int g16_settle_us = 1500;

      // Extra microseconds held after each row's latch, before the next
      // row's data starts clocking. Zero by default: the charging window is
      // then just the row transfer time (~9-19 us at measured row-loop
      // rates). Applied at NORMAL and HIGH only — FAST cannot afford it
      // (see sendRow).
      //
      // Exists to test WHY a panel shows a left-to-right gradient. Every
      // column of a line is latched and driven simultaneously, so the
      // gradient cannot come from any sequential drive along the row - it
      // must be the supply rail drooping with distance from its feed point
      // while the whole line draws current at once. Two mechanisms fit:
      //
      //   transient   the pixel capacitor is still charging through that
      //               resistance when the window closes. MORE TIME HELPS.
      //   steady      the rail simply sits lower for the whole window; the
      //               capacitor reaches that lower voltage and stops.
      //               More time buys nothing.
      //
      // Both look identical in a scan. Raising this and re-measuring the
      // gradient separates them. Costs height x this per pass, so it eats
      // into the pass period's padding - check the row loop still fits.
      // Default 4 us, measured. At 16-grey this is FREE: a pass is already
      // padded to a constant period with ~8 ms spare, so the 2.2 ms it costs
      // (540 rows x 4 us) is taken out of idle padding and the frame time is
      // unchanged - 195 ms at NORMAL either way. What it buys is charge that
      // was otherwise being cut off mid-transfer: on a LilyGo, L12's landing
      // moved 13 units darker and its left-to-right gradient fell from 6.0 to
      // 2.4 scan units. Beyond ~4 us the gradient stops improving, so this is
      // the knee rather than a round number.
      //
      // Tables are only valid at the row timing they were tuned at, exactly
      // as they are only valid at their pass period - so changing this
      // invalidates every tuned blob on the board.
      // Back to 0. The 4 us it briefly defaulted to genuinely improved
      // charging (L12's left-to-right gradient 6.0 -> 2.4 scan units, and
      // its landing 13 units darker), but it adds 2.2 ms to EVERY row loop -
      // and that pushed rich content over the pass budget: a photograph
      // measured 13.3 ms of row loop against a 15 ms period with a 4 ms
      // settle floor, so the pass stretched to 17.3 ms and every level on
      // that content landed 15% over-dosed. Tables tuned on the match card
      // (7-8 ms row loops, period exactly 15 ms) then band badly on real
      // images. Timing integrity first; the charging fix needs the budget
      // sorted before it can come back.
      // 4 us, measured. Held after each row's latch so the pixel finishes
      // charging: L12's left-to-right gradient fell 6.0 -> 2.4 scan units
      // and its landing moved 13 units darker. The control experiment -
      // the same 2.16 ms added as pass padding instead - moved the mean
      // only 2.5, so this is the row charging window and not dose.
      //
      // Free at 16-grey: the pass is already padded to a constant period,
      // so this comes out of idle time and the frame stays 195 ms at
      // NORMAL. Never applied at 4-level (see sendRow) - there is no
      // padding there and it would cost ~27% of the frame rate.
      // PER QUALITY, exactly like the pass period above - and for the same
      // reason. A table is only valid at the row timing it was tuned at, so
      // a board carrying a NORMAL set tuned at 0 and a HIGH set tuned at 4
      // needs both, or whichever blob loads last silently dictates the
      // timing for the other. (That is not hypothetical: load order is
      // FAST, NORMAL, HIGH, so HIGH used to win and drive NORMAL's tables
      // ~2 grey steps out at the deep end.)
      int row_extra_us_normal = 4;
      int row_extra_us_high   = 4;

      // The value in force for the quality currently selected.
      int rowExtraUs() const {
        return quality == Quality::QUALITY_HIGH ? row_extra_us_high
                                                : row_extra_us_normal;
      }

      // Held LE pulse width, in nanoseconds. 0 keeps the historical
      // behaviour, which is that the pulse has NO defined width at all: it
      // is `set(true); set(false);` back to back through a virtual call, so
      // the high time is whatever the vtable dispatch and I-cache happen to
      // cost that iteration — order 40-80 ns, unspecified and variable.
      //
      // That is close enough to a source driver's minimum latch width to be
      // worth controlling, because a marginal LE would latch LOAD-DEPENDENTLY:
      // a line switching many outputs droops the rail, moving the effective
      // logic threshold, so busy lines would latch less completely than
      // quiet ones. That is indistinguishable, from the glass, from an
      // incomplete charge — and it is the competing explanation for the
      // content-dependent darkening that row_extra_us also treats.
      //
      // Cost is per row per pass: 540 rows x 20 passes = 10800 pulses in a
      // HIGH frame, so 200 ns costs 2.2 ms of frame and 1 us costs 10.8 ms.
      // At 16-grey that comes out of the pass padding and is free.
      //
      // 1000 ns is MEASURED, and it is an optimum rather than a floor -
      // going further is actively harmful. LE is a level-transparent latch
      // and the next row's DMA is already running when it goes high, so a
      // long hold lets the source outputs track the incoming data. Solid
      // content does not care (neighbouring rows are identical) but a Bayer
      // dither's neighbouring rows are anti-correlated, so the bleed drags
      // fine detail toward its local average. Measured on the glass, the
      // dithered reference falls to a minimum at 1 us and then climbs back:
      // at 4 us it is WORSE than no hold at all (L8 104.0 vs 103.1).
      //
      // Beware of tuning this against flat patches alone: their content
      // dependence keeps improving to 4 us, so that metric on its own picks
      // a value that wrecks detail. Judge it on the dither.
      int le_hold_ns = 1000;


      // Scanner-tuned trains for this board (null members = untuned; see
      // TrainTables). setGreyLevels(16) and setDirectTransitions() load
      // these automatically.
      TrainTables trains{};

      // What each grey level ACTUALLY renders as, 255 = paper white down to
      // 0 = the deepest black this panel reaches. Measured by the tuner and
      // carried in the tuned blob; EPD_PainterTuned::install() points this at
      // it, exactly as it does the train tables.
      //
      // drawGray8() quantises against this rather than assuming levels are
      // evenly spaced - they are not. On a measured LilyGo the gaps between
      // neighbouring levels ran from 6 to 32 units against an ideal 17, and
      // diffusing error against an assumed even ramp is what banding looks
      // like. Null means the panel has never been measured, in which case
      // an even ramp is the only honest guess available.
      //
      // Per quality, for the same reason the trains are: HIGH's longer
      // trains reach a deeper black and re-space everything below it, so
      // quantising a HIGH image against the NORMAL curve diffuses error
      // into levels that are no longer where it thinks. levelLum() picks;
      // a board tuned for only one quality falls back to the set it has.
      const uint8_t *level_lum      = nullptr;
      const uint8_t *level_lum_high = nullptr;

      // The four levels of 4-level mode, same convention, measured by the
      // FAST tune. Separate because 4-level mode does NOT render levels
      // 0..3 of the 16-grey curve — it drives its own waveform tables to
      // its own four landings, and reading the first four entries of a
      // 16-entry curve (which is what happened before this existed) hands
      // the dither four near-white values for a full black-to-white ramp.
      const uint8_t *level_lum4 = nullptr;

      // The measured level curve to quantise against, for the mode and
      // quality currently selected. Null = never measured; assume even
      // spacing, which is the only honest guess and also exactly the
      // assumption that produces banding.
      const uint8_t *levelLum(int levels = 16) const {
        if (levels == 4) return level_lum4;
        if (quality == Quality::QUALITY_HIGH && level_lum_high) return level_lum_high;
        return level_lum ? level_lum : level_lum_high;
      }

      // Returns a copy of this config with rotation set — lets you write:
      //   EPD_PainterAdafruit epd(EPD_PAINTER_PRESET.withRotation(EPD_Painter::Rotation::ROTATION_CW));
      Config withRotation(Rotation r) const { Config c = *this; c.rotation = r; return c; }
  };

  struct ProbeSettings {
    Config *preset;
    int i2c_sda;
    int i2c_scl;
    int i2c_addr;
    bool found = false;
  };

  Config _config;
  const Config* _preset = nullptr;

  EPD_Painter(const Config &config, bool portrait = false);
  bool begin();
  bool end();

  // Install any scanner-tuned table blobs stored in NVS (namespace
  // "epd_painter", one blob per quality under keys "fast" / "normal" /
  // "high" — written by examples/other/tuneup). Called automatically at
  // the end of begin(), AFTER board detection, so a board tuned once runs
  // its own measured tables in every sketch; the per-board train files
  // (see EPD_Painter_trains.h) are the fallback for anything not stored.
  // Blobs are board-keyed and CRC-guarded, so a flash image copied to a
  // different board is refused rather than painted. Returns how many sets
  // installed. Define EPD_PAINTER_NO_NVS to compile the lookup out.
  int loadTunedFromNVS();

  struct Rect {
    int16_t x, y, w, h;
  };

  enum class ClearMode {
    HARD,  // 4 phases {6,2,4,8} passes — full ghosting removal
    SOFT   // 2 phases {2,2} passes — faster, light refresh
  };

  void clear(const Rect* rects = nullptr, int num_rects = 0, ClearMode mode = ClearMode::HARD);

  // Compare screenbuffer (current display state) against paintbuffer (desired state)
  // and return dirty rectangles. tolerance = max clean pixels a rect may absorb
  // when bridging gaps between dirty rows (0 = merge only adjacent dirty rows,
  // larger values merge more, screen_width * screen_height = single rect).
  int computeDirtyRects(Rect* out_rects, int max_rects, int tolerance = 0) const;

  // Convenience: compact framebuffer → paintbuffer, compute dirty rects, then
  // clear only those areas. tolerance is passed to computeDirtyRects.
  void clearDirtyAreas(uint8_t* framebuffer, int tolerance = 0, ClearMode mode = ClearMode::SOFT);

  void fxClear();
  void clearBuffers();  // zero all packed buffers (call before power-off to reset DC-balance baseline)
  void paint(uint8_t* framebuffer);

  // Paint from a pre-packed 2bpp buffer (4 pixels/byte, first pixel in the
  // MSBs). line_repeat > 1 treats `packed` as a reduced-height buffer of
  // height/line_repeat rows and drives each source row line_repeat times —
  // e.g. line_repeat=2 paints a 960×270 buffer as 960×540, halving the
  // bandwidth needed when streaming frames from storage. height must be an
  // exact multiple of line_repeat.
  void paintPacked(const uint8_t* packed, int line_repeat = 1);
  void unpaintPacked(const uint8_t* packed);
  void paintLater(uint8_t* framebuffer);

  // Completed drive cycles since begin(). paintLater() drops frames when
  // submissions outpace the panel (only the latest render is painted), so
  // effective paint rate = delta of this counter over wall time, never the
  // submit-loop rate. No-op cycles (nothing changed) are not counted.
  uint32_t paintsCompleted() const { return _paints_done.load(); }
  bool paintIdle() const { return paintStage.load() == 0; }
  // No-op, kept for API compatibility. Interlace mode existed to spatially
  // dither the old either-direction-per-chunk deferral artifact; the
  // dual-plane ink engine drives both directions per pixel in one cycle,
  // so there is nothing left to interlace.
  void setInterlaceMode(bool) {}

  // Per-drive-cycle profiling, off by default and free when off.
  //
  // Prints one line per cycle: the number of PASSES actually run, the number
  // of ROW-SENDS against how many the pass count implies, how the time split
  // between the row loops and the inter-pass settle, and how many lines
  // carried darkening AND lightening together.
  //
  // It answers three questions that are easy to guess wrong about:
  //   - is a frame one drive cycle, or secretly two?  (compare
  //     paintsCompleted() across frames — each cycle prints once)
  //   - is a row ever driven twice?  (rows vs expected)
  //   - is erase-and-paint happening in ONE pass?  ("merged" lines carry
  //     both directions in the same transmission; a nonzero count with rows
  //     == expected is proof that it is)
  void setPaintProfile(bool on) { _profile = on; }

  void setQuality(Quality quality);

  // Select the paint engine: false (default) = SIMD dual-plane assembly,
  // true = the decision engine's C discovery path (see DECISION_ENGINE.md).
  // Phase B: both produce identical output on 4-level content; the C path
  // exists to be verified against the assembly before it grows 16 levels.
  void setDecisionEngine(bool on) { _decision_engine = on; }

  // 16-grey mode (decision engine phase C — see DECISION_ENGINE.md).
  // levels = 4 (default) or 16. While 16-grey is on, paint() reads the 8bpp
  // canvas as level codes 0..15 (0 = white … 15 = black; use dither16() for
  // greyscale sources) and paintPacked()/unpaintPacked() are refused — the
  // packed 2bpp path stays a 4-level feature. NORMAL and HIGH only: FAST's
  // 7 undelayed passes lack the dose resolution, so the call fails while
  // quality is FAST. First enable allocates the 4bpp state and spill slot
  // planes (~1.6 MB PSRAM). The physical screen state is carried across the
  // switch in both directions, so ghost tracking survives without a clear.
  // Call after begin(). Returns false on refusal or allocation failure.
  bool setGreyLevels(int levels);

  // 16-grey train length, per quality. NORMAL spends 13 passes; HIGH
  // spends 20. (Briefly 25, with alternate slots pinned float to give the
  // supply recovery time - dropped once row_extra_us fixed the charging
  // directly, which is both simpler and cheaper than spending slots on it.) This is the whole difference in kind between the two
  // modes: a 13-pass pure-darken run saturates well before the glass is
  // as dark as it can get, and every level below the saturation knee has
  // to be placed by re-darkening after a whiten — a knob that needs
  // spare passes to turn. HIGH buys both with seven more.
  //
  // Public because the calibration rig and the blob format are both
  // sized by them: a stored table read at the wrong stride is silently
  // the wrong tables.
  static constexpr int DEC_WF_LEN16      = 13;  // NORMAL
  static constexpr int DEC_WF_LEN16_HIGH = 20;  // HIGH
  // Array bound, deliberately larger than either length in use. Storage is
  // sized by THIS, so widening the max changes the blob layout and
  // invalidates saved tables - leaving headroom now avoids doing that
  // again the next time a train wants to get longer.
  static constexpr int DEC_WF_LEN16_MAX  = 32;
  static constexpr int DEC_WF_LEN_DIR    = 26;  // max direct-train length

  // How many passes a 16-grey train has at the current quality: 13 at
  // NORMAL, 20 at HIGH. Everything that builds, uploads or stores a
  // 16-grey train must agree with this — a train tuned at one length is
  // meaningless at the other, and so is a table read at the wrong stride.
  int g16TrainLen() const {
    return _config.quality == Quality::QUALITY_HIGH ? DEC_WF_LEN16_HIGH
                                                    : DEC_WF_LEN16;
  }

  // Override one 16-grey decision train (id = (level << 1) | dir, dir 0 =
  // apply / 1 = remove; train = g16TrainLen() drive codes). The calibration
  // rig's hook: probe sketches load candidate trains and the scanner
  // measures what the glass actually does with them. Only meaningful after
  // setGreyLevels(16) has built the default library.
  void setDecisionTrain(uint8_t id, const uint8_t *train) {
    if (id < 2 || id >= DEC_IDS || !train) return;
    const int len = g16TrainLen();
    for (int p = 0; p < len; p++) dec_trains16[id][p] = train[p];
    // Tail beyond the quality's length must read as float, or a train
    // left behind by a longer HIGH upload keeps driving under NORMAL.
    for (int p = len; p < DEC_WF_LEN16_MAX; p++) dec_trains16[id][p] = 0;
  }

  // Give back the buffers the CURRENT configuration does not need.
  //
  // setGreyLevels() and setDirectTransitions() allocate and never free, so a
  // mode toggle costs nothing the second time — which is the right default
  // when an app is switching per frame, and the wrong one when the next
  // allocation is the one that matters. 16-grey holds 64.8 KB of sweep list
  // in INTERNAL RAM (12 bytes x height x 10) and keeps holding it in 4-level
  // mode where it is dead weight; the direct engine then asks for 25.9 KB of
  // the same scarce pool and is refused. That is not hypothetical — it is why
  // the tuner's grey-to-grey phase reported "direct engine would not enable"
  // on a board with megabytes of PSRAM free.
  //
  // Call this when about to need memory more than speed. Everything freed
  // here is reallocated on demand by the call that needs it.
  //
  // NOT SAFE WHILE PAINTING: the paint task reads these. Wait for
  // paintIdle() first.
  void releaseUnusedBuffers();

  // How many grey levels are live: 16 after setGreyLevels(16), else 4.
  //
  // Anything that RENDERS needs this, because the canvas is level-indexed and
  // the 4-level packer masks each byte with 0x03 rather than rescaling it. So
  // a level code authored for 16 greys does not degrade in 4-level mode, it
  // WRAPS — 4 and 8 and 12 all land on white — which turns a smooth ramp into
  // a sawtooth and a photograph into a relief carving. Quantise against this
  // count and Config::levelLum(count), never against a fixed 16.
  int greyLevels() const { return _grey16 ? 16 : 4; }

  // Read back an installed 16-grey train (same id encoding). The counterpart
  // to setDecisionTrain, and the reason it exists: a tuner that starts from
  // scratch throws away everything the board already knows. What is loaded
  // here is the best current answer — a flash blob if one installed, else the
  // preset's tuned tables, else the formula — so a search that begins from it
  // begins at the incumbent rather than at a guess.
  //
  // Valid for g16TrainLen() entries; the tail to DEC_WF_LEN16_MAX reads as
  // float. Null for an out-of-range id, or before setGreyLevels(16) has built
  // the library at all.
  const uint8_t *decisionTrain(uint8_t id) const {
    if (id < 2 || id >= DEC_IDS || !_grey16) return nullptr;
    return dec_trains16[id];
  }

  // Rebuild the 16-grey train library to its defaults: the formula trains
  // overlaid with the preset's scanner-tuned tables for the CURRENT
  // quality (Config::trains) when the board has them. Runs automatically
  // on the first setGreyLevels(16) and on every setQuality() while
  // 16-grey is on; call it manually to undo experimental
  // setDecisionTrain() uploads.
  void rebuildDecisionTrains() { _grey16_build_trains(); }

  // Re-read Config::trains for the direct grey-to-grey engine — the hook
  // for installing tuned direct tables after begin(). No-op unless the
  // direct engine has been enabled.
  void reloadDirectTrains() { if (dec_spill_dir) _load_preset_direct_trains(); }

  // Direct grey-to-grey transitions (4-level mode — see DECISION_ENGINE.md
  // "direct grey-to-grey transitions"). When on, a changed occupied pixel
  // whose (from, to) pair has a loaded train is driven straight to its new
  // level in ONE paint — no erase-to-white two-step. Pairs without a train
  // keep the legacy two-step, so the engine comes up one tuned pair at a
  // time. First enable allocates 2 spill slot planes (~260 KB PSRAM) and
  // loads the preset's tuned tables for the current quality
  // (Config::trains); setQuality() then keeps the loaded set matched to
  // the quality (HIGH has no tuned set — pairs unload to the two-step).
  // On a tuned board manual setDirectTrain() uploads therefore survive
  // engine toggles but are replaced on a quality change.
  // Call after begin(). Returns false on allocation failure.
  bool setDirectTransitions(bool on);

  // Decision id of a direct transition: from/to = 2bpp levels 1..3.
  static constexpr uint8_t directId(uint8_t from, uint8_t to) {
    return (uint8_t)(16 | (from << 2) | to);
  }

  // Load one direct train (len drive codes, up to DEC_WF_LEN_DIR = 26 —
  // a direct train may be LONGER than the quality's pass count: deep
  // lightening transitions need erase + re-darken room, and the frame's
  // pass count extends to the longest direct train in play, so content
  // without deep transitions pays nothing). nullptr unloads the pair back
  // to the two-step. The DC constraint is the tuner's job, not enforced
  // here: the train's net darkens must equal Q(to) - Q(from), where Q(g)
  // is the net darkens of level g's apply train — that makes the charge
  // ledger path-independent.
  void setDirectTrain(uint8_t from, uint8_t to, const uint8_t *train,
                      int len = DEC_WF_LEN16) {
    if (from < 1 || from > 3 || to < 1 || to > 3 || from == to) return;
    const int key = (from << 2) | to;
    if (!train) { _dir_loaded &= (uint16_t)~(1u << key); return; }
    if (len > DEC_WF_LEN_DIR) len = DEC_WF_LEN_DIR;
    for (int p = 0; p < DEC_WF_LEN_DIR; p++)
      dec_trains_dir[key][p] = (p < len) ? train[p] : 0;
    _dir_loaded |= (uint16_t)(1u << key);
  }

  // Static template layer (Tony's banded-quality pattern, July 2026).
  // setTemplate() paints fb (8bpp canvas, same format as paint()) at the
  // given quality — independent of the current quality — then PROTECTS
  // its non-white pixels: no subsequent paint can touch them, so
  // animation runs in the template's white areas at any speed while the
  // template keeps its high-quality rendering. (Unchanged pixels are
  // never re-driven anyway; protection turns that from app discipline
  // into a guarantee.) releaseTemplate() unpaints the template with the
  // SAME quality that painted it — a NORMAL pixel erased by a FAST
  // train would strand charge — and lifts the protection. clear()
  // auto-releases first; setGreyLevels() is refused while a template is
  // active. ~260 KB PSRAM (double in 16-grey). Returns false on
  // allocation failure or FAST+16-grey.
  bool setTemplate(const uint8_t *fb, Quality quality);
  void releaseTemplate();
  bool hasTemplate() const { return _has_template; }

  // Draw an 8bpp greyscale image (0 = black … 255 = white) into an 8bpp
  // canvas, converting it to the driver's level codes with Floyd-Steinberg
  // error diffusion. This is the replacement for the old dither(): that one
  // hardcoded {255,170,85,0} — an evenly spaced 4-level ramp no panel
  // actually produces — so it quantised against levels that were not where
  // it believed them to be.
  //
  // Quantises against Config::level_lum when the panel has been measured,
  // falling back to an even ramp otherwise, and targets whichever level count
  // is active (16 after setGreyLevels(16), else 4). Error diffusion copes
  // perfectly well with unevenly spaced levels — it just produces more grain
  // where the gaps are wide. It only produces visible BANDS when it is wrong
  // about where the levels sit, which is exactly what an assumed ramp does.
  //
  // src is not modified, so the same image can be drawn repeatedly. Clipped
  // to the destination. Allocates one row of int16 scratch internally
  // (~2 KB for a 960-wide panel).
  //
  // The library deliberately has no image decoder. Feed it 8bpp greyscale
  // from wherever you like — see EPD_Painter_Image.h for optional helpers
  // that use JPEGDEC or PNGdec if you have them installed.
  // STREAMING dither. Floyd-Steinberg only ever needs the current error row,
  // never the whole image, so an image can be converted as it decodes and the
  // full-size buffer never has to exist. A 1600x1200 photo costs ~4 KB of
  // error rows here against ~1.9 MB decoded.
  //
  //   EPD_Painter::GrayDither d;
  //   d.begin(painter, fb, W, H, srcW, srcH, dx, dy, dw, dh);
  //   for each source row, top to bottom:  d.row(pixels, y);
  //   d.end();
  //
  // Rows MUST arrive top-to-bottom and complete (a partial row would diffuse
  // error from pixels that are not there yet). Rows may be skipped or
  // repeated by the scaler, which handles both shrinking and enlarging.
  // end() is safe to call twice and is called by the destructor.
  class GrayDither {
   public:
    ~GrayDither() { end(); }
    bool begin(const EPD_Painter &p, uint8_t *dst, int dstW, int dstH,
               int srcW, int srcH, int dx = 0, int dy = 0,
               int dw = 0, int dh = 0);
    void row(const uint8_t *src, int srcY);
    void end();
   private:
    uint8_t *_dst = nullptr;
    int16_t *_cur = nullptr, *_next = nullptr;
    uint8_t  _lum[16] = {0};
    int _dstW = 0, _dstH = 0, _srcW = 0, _srcH = 0;
    int _dx = 0, _dy = 0, _dw = 0, _dh = 0;
    int _levels = 4, _nextY = 0;
    void emit(const uint8_t *src, int y);
  };

  // dw/dh scale the image into a destination rectangle (nearest-neighbour
  // sampling), 0 meaning "same size as the source". Scaling happens in the
  // SAME pass as the dither, so a photo larger than the panel never needs an
  // intermediate full-size buffer — which matters when the source is a phone
  // photo and the board has 8 MB of PSRAM to work with.
  void drawGray8(uint8_t *dst, int dstW, int dstH,
                 const uint8_t *src, int srcW, int srcH,
                 int dx = 0, int dy = 0, int dw = 0, int dh = 0) const;

  // Pack an 8bpp framebuffer to the driver's 2bpp packed format, respecting
  // the current rotation setting. Returns a PSRAM-allocated buffer the caller
  // must free with heap_caps_free(). Returns nullptr on allocation failure.
  // Use this to obtain a packed buffer suitable for EPD_BootCtl::IImageProvider.
  uint8_t* packBuffer(const uint8_t* fb) const;

  const Config& getConfig(){
    return _config;
  }
  const Config* getPreset() const {
    return _preset;
  }

  // One-line engine state + heap dump for field debugging: call it from
  // the app when the display misbehaves (e.g. a debug key). Ghosts that
  // ordinary paints cannot erase but clear() can mean either a phantom
  // template (tpl=1 here without ever calling setTemplate = something
  // scribbled on this object) or screenbuffer/glass divergence.
  // sbhits / tplhits are RETAINED tripwire counters — nonzero means the
  // event happened at some point since boot, even if serial was not
  // attached at the moment it fired.
  // Micro-benchmark the control-pin paths (LE = possibly shift-register,
  // CKV = direct GPIO): 1000 pulses each, prints us/pulse. Call after
  // begin(), panel power state irrelevant.
  void debugPinBench();

  void debugState() const {
    printf("[EPD] tpl=%d dir=%d ceng=%d g16=%d tplp=%p/%p | sbhits=%lu "
           "tplhits=%lu | int free=%u largest=%u | psram free=%u\n",
           (int)_has_template, (int)_decision_direct, (int)_decision_engine,
           (int)_grey16, tpl_data, tpl_mask,
           (unsigned long)_sb_guard_hits, (unsigned long)_tpl_trip_hits,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }

  // Screenbuffer state guard: checksums the physical-state buffer at the
  // end of every drive and verifies it at the start of the next. The
  // library is the only legitimate writer between drives, so a mismatch
  // proves an EXTERNAL write (heap-neighbour overflow) corrupted the
  // delta engine's map of the glass — the exact mechanism behind
  // "ghosts ordinary paints cannot erase but clear() can". Announces
  // loudly with heap watermarks. Costs ~1-2 ms per paint, so it is OFF
  // by default — setStateGuard(true) enables it when chasing corruption.
  void setStateGuard(bool on) { _sb_guard_on = on; _sb_guard_valid = false; }

  // Idle panel power-off: rails power down after `seconds` without a
  // paint (next paint powers them back up); 0 keeps rails up forever.
  // Default 5 s — the battery-friendly setting e-paper deserves.
  // (During the 2026 ghost hunt each rail cycle shifted content on the
  // glass and this default was raised to 60 s as a mitigation; the root
  // cause was the source driver's start logic never being re-armed —
  // fixed in powerOn()'s SPH sweep — so cycles are safe again. Raise
  // the timeout only if the ~100 ms power-up latency on the first paint
  // after an idle gap bothers your app.)
  void setIdlePowerOff(bool on) { _idle_timeout_s = on ? 5 : 0; }
  void setIdleTimeout(int seconds) { _idle_timeout_s = seconds; }

  void setAutoShutdown(bool v) { _autoShutdown = v; }
  EPD_PainterShutdown* shutdown() { return _shutdown; }


private:
  // ---- LCD_CAM / DMA ----
  gdma_channel_handle_t dma_chan = nullptr;
  int                   _dma_channel_id = 0;
  dma_descriptor_t      dma_desc1 = {};
  dma_descriptor_t      dma_desc2 = {};
  intr_handle_t         _lcd_intr_handle = nullptr;
  volatile TaskHandle_t _dma_notify_task = nullptr;
  bool                  _dma_pending = false;

  static void IRAM_ATTR _lcd_isr(void *arg);

  // ---- Buffers ----

  uint8_t* dma_buffer        = nullptr;  // Points at one of the buffers below
  uint8_t* dma_buffer1       = nullptr;  //  Row Double buffer A
  uint8_t* dma_buffer2       = nullptr;  //. Row Double buffer B

  uint8_t* packed_fastbuffer  = nullptr;  // dark-plane drive data (internal RAM)
  uint8_t* packed_lightbuffer = nullptr;  // light-plane drive data (PSRAM, sparse)
  uint8_t* packed_screenbuffer = nullptr; // 2bpp physical screen state (PSRAM)
  uint8_t* packed_paintbuffer = nullptr; // 2bpp desired frame (PSRAM)

  uint32_t* bitmask = nullptr;        // per-row dark-plane chunk mask
  uint32_t* bitmask_light = nullptr;  // per-row light-plane chunk mask

  // ---- Decision engine (see DECISION_ENGINE.md) ----
  // A decision is a (grey level, direction) pair, id = (level << 1) | dir
  // (dir 0 = apply, 1 = remove). Discovery fills per-line todo words and
  // sweep lists; the pass loop consumes the lists generically. Phase B:
  // 4-level compatibility — decisions 2..7, at most 2 sweeps per line.
  // The direct-transition engine adds ids 16..30 (16 | (from << 2) | to)
  // and can put 12 distinct decisions on a line (3 applies + 3 removes +
  // 6 directs) = 4 sweeps — in its OWN lazily-allocated list, so apps
  // that never enable it pay no internal RAM beyond the compat pair.
  static constexpr int DEC_MAX_SWEEPS     = 2; // 4-level compat sweep lists
  static constexpr int DEC_MAX_SWEEPS_DIR = 4; // direct-transition lists
  static constexpr int DEC_MAX_SWEEPS16 = 10;  // 16-grey: ceil(30 decisions / 3)
  static constexpr int DEC_IDS          = 32;  // 16 levels x 2 directions
  // A grey-to-grey pixel's apply is shifted right past its remove
  // (temporal partition), so a shifted train can be as long as two
  // 16-grey trains back to back.
  static constexpr int DEC_WF_LEN_SHIFT  = 2 * DEC_WF_LEN16_MAX;
  // TrainTables (declared before these constants exist) hard-codes the
  // same lengths in its pointer types — keep them in lock-step. (Member
  // access through a null pointer, not construction: an unevaluated
  // TrainTables{} here would need its default member initializers, which
  // are not parsed until the enclosing class is complete.)
  static_assert(sizeof(*static_cast<TrainTables*>(nullptr)->g16_apply) ==
                DEC_WF_LEN16,
                "TrainTables g16 row length must equal DEC_WF_LEN16");
  static_assert(sizeof(*static_cast<TrainTables*>(nullptr)->g16_apply_high) ==
                DEC_WF_LEN16_MAX,
                "TrainTables g16 HIGH row length must equal DEC_WF_LEN16_MAX");
  static_assert(sizeof(*static_cast<TrainTables*>(nullptr)->dir_normal) ==
                4 * DEC_WF_LEN_DIR,
                "TrainTables direct row length must equal DEC_WF_LEN_DIR");
  // 16-grey constant pass period (row loop + padding), per quality. A row
  // converts k sweeps, so the row loop's duration varies with content —
  // and pass duration IS the retention dose. Padding every pass to a
  // fixed period makes dose depend only on the trains (phase D; see
  // DECISION_ENGINE.md "dose is content-dependent").
  static constexpr int DEC_PASS_US16_NORMAL = 15000;
  static constexpr int DEC_PASS_US16_HIGH   = 15000;
  struct LineSweep {
    const uint8_t *plane_row;  // slot-encoded 2bpp row
    uint32_t       mask;       // chunk mask for this row
    uint8_t        dec[3];     // decision id per slot 1..3 (0 = unused)
  };
  uint32_t  *dec_todo    = nullptr;  // per-line pending-decision words
  LineSweep *dec_sweeps  = nullptr;  // [height * DEC_MAX_SWEEPS]
  uint8_t   *dec_nsweeps = nullptr;  // per-line sweep count
  const uint8_t *dec_train[DEC_IDS]; // decision id -> waveform train (wf_len codes)
  // C discovery: paintbuffer vs screenbuffer -> planes, masks, todo, sweep
  // lists; updates screenbuffer. Returns nonzero if any line has work.
  uint32_t _decision_discover();
  uint32_t _decision_discover_row(const uint8_t *pb_row, uint8_t *fbD_row,
                                  uint8_t *fbL_row, uint8_t *sb_row,
                                  uint32_t *maskL_out, uint32_t *todo_out);
  void _decision_batch_compat();
  bool _decision_engine = false;   // C discovery path off until phase C needs it

  // ---- per-cycle paint profiling (see setPaintProfile) ------------------
  bool     _profile = false;
  uint32_t _prof_rows = 0, _prof_merged = 0;
  int64_t  _prof_rowloop_us = 0, _prof_cycle_t0 = 0;

  // ---- direct grey-to-grey transitions (4-level; DECISION_ENGINE.md) ----
  // key = (from << 2) | to, decision id = 16 | key. _dir_loaded gates
  // per-pair engagement so unloaded pairs fall back to the two-step.
  uint8_t   dec_trains_dir[16][DEC_WF_LEN_DIR];
  uint16_t  _dir_loaded = 0;
  bool      _decision_direct = false;
  uint8_t  *dec_spill_dir = nullptr;   // slot planes for sweeps 2..3 (PSRAM)
  LineSweep *dec_sweeps_dir = nullptr; // [height * DEC_MAX_SWEEPS_DIR], lazy
  uint32_t _decision_discover_direct();
  void _decision_discover_direct_row(int row);
  uint8_t *_dec_plane_row_dir(int sweep, int row);
  // Load the preset's direct tables for the current quality (no-op on
  // boards with no tuned direct tables at all; a tuned board with no set
  // for this quality — HIGH — unloads every pair to the two-step).
  void _load_preset_direct_trains();

  // ---- 16-grey mode (phase C) ----
  // 4bpp desired/physical state, spill slot planes for sweeps 2..9 (sweep 0
  // = fastbuffer, sweep 1 = lightbuffer), wide sweep lists, and a formula
  // train library (placeholder — calibration is phase D). All PSRAM,
  // allocated lazily on the first setGreyLevels(16).
  uint8_t   *packed4_paintbuffer  = nullptr;  // w*h/2
  uint8_t   *packed4_screenbuffer = nullptr;  // w*h/2
  uint8_t   *dec_spill            = nullptr;  // (DEC_MAX_SWEEPS16-2) slot planes
  LineSweep *dec_sweeps16         = nullptr;  // [height * DEC_MAX_SWEEPS16]
  uint8_t    dec_trains16[DEC_IDS][DEC_WF_LEN16_MAX];
  // Temporal partition state: dec_gg_from[to] = bitmask of from-levels
  // that transitioned to `to` this frame (set by discovery). Each apply
  // id shifts by the longest remove among ITS OWN from-partners — not a
  // global R — into the per-frame dec_shifted scratch.
  uint16_t   dec_gg_from[16];
  uint8_t    dec_shifted[DEC_IDS][DEC_WF_LEN_SHIFT];
  bool       _grey16 = false;
  uint32_t _decision_discover16();
  void _decision_discover16_row(int row);
  uint8_t *_dec_plane_row(int sweep, int row);
  void _grey16_build_trains();

  // ---- template layer ----
  // Packed template + protection mask (11 / 1111 fields where a template
  // pixel is non-white), stored for the mode active at setTemplate().
  uint8_t *tpl_data = nullptr;
  uint8_t *tpl_mask = nullptr;
  bool     _has_template = false;
  bool     _tpl_grey16   = false;
  Quality  _tpl_quality  = Quality::QUALITY_NORMAL;

  // ---- screenbuffer state guard ----
  bool     _sb_guard_on = false;
  bool     _sb_guard_valid = false;
  uint8_t *_sb_shadow = nullptr; // full copy: fingerprints foreign writes
  uint32_t _sb_guard_hits = 0;   // retained: external screenbuffer writes
  uint32_t _tpl_trip_hits = 0;   // retained: overlay active / bad pointers
  void     _sb_guard_update();
  void     _sb_guard_check();

  int _idle_timeout_s = 5;

  int packed_row_bytes = 0;
  std::atomic<int> paintStage{0};
  std::atomic<uint32_t> _paints_done{0};
  bool shouldSkipRow = false;
  bool _autoShutdown = true;
  EPD_PainterShutdown* _shutdown = nullptr;


  // ---- Hardware drivers (created in begin()) ----
  EPD_PowerDriver*   _powerDriver = nullptr;
  EPD_ISRController* _shiftReg    = nullptr;  // non-null only on SR boards
  EPD_PinDriver* _pin_spv = nullptr;
  EPD_PinDriver* _pin_ckv = nullptr;
  EPD_PinDriver* _pin_le  = nullptr;
  EPD_PinDriver* _pin_sph = nullptr;

  // ---- Internal helpers ----
  void powerOn();
  void powerOff();
  void _pushRow();   // transmit-only row: flush the source shift chain
  bool autoDetectBoard();
  // noAdvance: pulse LE to latch the previously transmitted data onto the
  // source outputs but do NOT clock CKV — the gate stays on the current row.
  // Used to drive one row with two data sets (dark plane, then light plane).
  void sendRow(bool firstLine, bool lastLine=false, bool noAdvance=false);

  // ---- Dual-core paint task ----
  SemaphoreHandle_t _paint_active_sem = nullptr;  // signals task to start
  SemaphoreHandle_t _paint_buffer_sem  = nullptr;  // signals task has finished
  TaskHandle_t      _paint_task_h    = nullptr;

  static void _paint_task_entry(void *arg);
  void _paint_task_body();

  // ---- Power management ----
  class PanelPowerGuard {
  public:
    PanelPowerGuard(EPD_Painter& d) : disp(d) {
      initOnce(d);
      xSemaphoreTake(power_mtx, portMAX_DELAY);
      if (state == 0) d.powerOn();
      state = d._idle_timeout_s;
      xSemaphoreGive(power_mtx);
    }

  private:
    EPD_Painter& disp;

    static inline TaskHandle_t      task      = nullptr;
    static inline EPD_Painter*      owner     = nullptr;
    static inline SemaphoreHandle_t power_mtx = nullptr;
    static inline uint8_t           state     = 0;

    static void initOnce(EPD_Painter& d) {
      struct Init {
        Init(EPD_Painter& d) {
          power_mtx = xSemaphoreCreateMutex();
          owner = &d;
          xTaskCreate(taskEntry, "panel_idle_off", 2048, nullptr, 1, &task);
        }
      };
      static Init init{ d };
    }

    static void taskEntry(void*) {
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(power_mtx, portMAX_DELAY);
        if (state > 0 && owner->_idle_timeout_s > 0) {
          state--;
          if (state == 0) owner->powerOff();
        }
        xSemaphoreGive(power_mtx);
      }
    }
  };
};

#include "EPD_Painter_presets.h"

#endif

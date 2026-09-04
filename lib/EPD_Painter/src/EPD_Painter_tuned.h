#pragma once

// ============================================================================
// EPD_Painter_tuned.h — the on-glass-tuned waveform tables, as blobs.
//
// The tuneup example tunes a board's tables against its own panel with a
// flatbed scanner (the same closed optical loop that produced the shipped
// tables in EPD_Painter_trains.h). This header defines how that result is
// packaged, validated and installed.
//
// ONE BLOB PER QUALITY. The three qualities are different machines, not
// three settings of one:
//
//   FAST    4 levels, 7 undelayed passes. Tables are the 4-level
//           waveforms plus the six grey-to-grey direct trains.
//   NORMAL  16 levels, 13 passes at g16_pass_us_normal.
//   HIGH    16 levels, 20 passes at g16_pass_us_high.
//
// A train is only meaningful at the length and pass period it was
// calibrated at, so there is nothing to be gained by mixing them in one
// record — and something real to lose: a single blob makes tuning HIGH
// overwrite a good NORMAL set. Separate blobs mean each quality can be
// tuned, re-tuned or abandoned on its own, and a board can carry any
// subset. Everything a quality does not use is simply left zero; the
// waste is a few hundred bytes of flash against a much simpler
// invariant.
//
// STORAGE IS NOT OUR BUSINESS. This header deliberately knows nothing
// about LittleFS, SD, NVS or HTTP — it moves bytes in and out of a plain
// buffer and leaves the keeping of them to the sketch. That way including
// it never drags a filesystem into a project that doesn't want one, and
// you are free to hold the tables wherever suits: flash, an SD card, an
// EEPROM, or a download.
//
//   // ---- installing tables you have loaded from somewhere ----
//   uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
//   size_t n = my_read_bytes(buf, sizeof(buf));         // your storage
//   if (EPD_PainterTuned::install(epd.driver(), buf, n))
//     Serial.println("using tuned trains");
//
//   // ---- packaging 16-grey tables to store ----
//   uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
//   EPD_PainterTuned::build16(epd.driver(), SET_HIGH, period_us,
//                             apply, remove, lum, buf);
//   my_write_bytes(buf, sizeof(buf));                   // your storage
//
// See examples/other/tuneup/tuned_storage.h for a LittleFS
// implementation of "your storage".
//
// Every blob is keyed to the board's pins and geometry and CRC-guarded,
// so tables from a different board — or a corrupted read — are refused
// rather than painted.
// ============================================================================

#include <stdint.h>
#include <string.h>
#include "EPD_Painter.h"

namespace EPD_PainterTuned {

// Which quality's tables a blob carries.
enum Set : uint8_t { SET_FAST = 0, SET_NORMAL = 1, SET_HIGH = 2 };

// Blob rows are stored at this width, so the on-flash record size follows
// DEC_WF_LEN16_MAX. Changing that constant changes the blob layout and
// invalidates every saved set — which is what BlobV3 below exists to read.
static const int LEN16_MAX = EPD_Painter::DEC_WF_LEN16_MAX;   // 32
static const int LEN_DIR   = EPD_Painter::DEC_WF_LEN_DIR;     // 26

struct __attribute__((packed)) Blob {
  uint32_t magic;           // 'EPT4'
  uint8_t  set;             // Set: which quality these tables are for
  uint8_t  train_len;       // passes per train: 7 FAST, 13 NORMAL, 20 HIGH
  // Row charging time the tables were tuned at (Config::row_extra_us).
  // As load-bearing as the pass period: 4 us against 0 moves a deep level
  // by ~13 scan units, two whole grey steps. Occupies what used to be
  // padding, so the record size is unchanged and blobs written before this
  // field existed read back as 0 — exactly what they were tuned at.
  uint16_t row_extra_us;
  uint32_t board_key;       // hash of pins + geometry — blob is board-specific
  uint32_t pass_us;         // 16-grey pass period tuned at (0 for FAST)

  // ---- 16-grey sets (NORMAL, HIGH) ----
  // Trains are stored at full width whatever the quality's length; the
  // tail past train_len is zero (float) and drives nothing.
  uint8_t  apply[16][LEN16_MAX];    // apply trains (level 0 unused)
  uint8_t  remove[16][LEN16_MAX];   // charge-matched removes

  // ---- 4-level set (FAST) ----
  uint8_t  darker[3][7];            // Waveforms::fast_darker
  uint8_t  lighter[3][7];           // Waveforms::fast_lighter
  uint8_t  direct[4][4][LEN_DIR];   // grey-to-grey direct trains
  uint16_t direct_loaded;           // bit (from << 2) | to set = pair tuned
  // Held LE pulse width the tables were tuned at (Config::le_hold_ns).
  // RECORDED BUT NOT APPLIED: the width is a fixed 1000 ns, chosen as the
  // middle of a measured plateau, and is not a per-board tunable. Applying
  // it on install would let any blob written before this field existed —
  // they all read back 0 — silently drop the board back to the old
  // uncontrolled latch, which is the one thing this must never do. It is
  // kept so a stored set says what it was calibrated against.
  uint16_t le_hold_ns;

  // What each level ACTUALLY renders as, measured on the glass at the end
  // of the tune and normalised so level 0 = 255 (paper white) and the
  // deepest level = 0. The levels are not evenly spaced — on a real panel
  // the light end bunches up — so anything converting a greyscale image
  // should quantise against THIS curve rather than assuming even steps,
  // or it diffuses error into levels that cannot express it. A FAST blob
  // fills the first four entries only. All-zero means "not measured";
  // treat the levels as even.
  uint8_t  level_lum[16];

  uint32_t crc;             // CRC32 of everything above
};

static const uint32_t MAGIC    = 0x45505434;   // 'EPT4'
static const uint32_t MAGIC_V3 = 0x45505433;   // 'EPT3' — 20-wide train rows
static const uint32_t MAGIC_V2 = 0x45505432;   // 'EPT2' — 16-grey NORMAL only
static const size_t   BLOB_SIZE = sizeof(Blob);

// The v3 record: identical in shape but with 20-wide train rows, written
// before HIGH grew to 25 slots. Read so a board tuned under v3 keeps its
// tables rather than silently reverting to the formula library — the train
// STORAGE got wider, the trains themselves did not change meaning.
struct __attribute__((packed)) BlobV3 {
  uint32_t magic;
  uint8_t  set;
  uint8_t  train_len;
  uint8_t  pad[2];
  uint32_t board_key;
  uint32_t pass_us;
  uint8_t  apply[16][20];
  uint8_t  remove[16][20];
  uint8_t  darker[3][7];
  uint8_t  lighter[3][7];
  uint8_t  direct[4][4][LEN_DIR];
  uint16_t direct_loaded;
  uint8_t  pad2[2];
  uint8_t  level_lum[16];
  uint32_t crc;
};

// The v2 record, still read so a board tuned before HIGH existed keeps
// its tables. It is exactly today's NORMAL set with no room for anything
// else, which is what prompted the split.
struct __attribute__((packed)) BlobV2 {
  uint32_t magic;
  uint32_t board_key;
  uint32_t pass_us_normal;
  uint8_t  apply[16][13];
  uint8_t  remove[16][13];
  uint8_t  level_lum[16];
  uint32_t crc;
};

inline uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c ^ 0xFFFFFFFFu;
}

// Board identity: enough to stop tables tuned on one board being applied
// to another via a copied file or a shared card.
inline uint32_t boardKey(const EPD_Painter::Config &c) {
  uint32_t k = 2166136261u;
  auto mix = [&](int v) { k = (k ^ (uint32_t)(v & 0xFF)) * 16777619u; };
  for (int i = 0; i < 8; i++) mix(c.data_pins[i]);
  mix(c.i2c.sda);
  mix(c.i2c.scl);
  mix(c.width & 0xFF);
  mix(c.width >> 8);
  mix(c.height & 0xFF);
  mix(c.height >> 8);
  return k;
}

// Which quality a buffer holds tables for, or -1 if it is not a blob we
// trust for this board. Lets a caller sort a directory of blobs without
// installing them.
inline int setOf(const EPD_Painter &p, const uint8_t *data, size_t len) {
  if (!data) return -1;
  if (len == sizeof(Blob)) {
    static Blob b;                    // ~1.1 KB: not on the caller's stack
    memcpy(&b, data, sizeof(b));
    if (b.magic != MAGIC) return -1;
    if (b.crc != crc32(data, sizeof(b) - sizeof(b.crc))) return -1;
    if (b.board_key != boardKey(p._config)) return -1;
    if (b.set > SET_HIGH) return -1;
    if (b.set != SET_FAST && !(b.pass_us >= 5000 && b.pass_us <= 60000)) return -1;
    return (int)b.set;
  }
  if (len == sizeof(BlobV3)) {
    static BlobV3 b;
    memcpy(&b, data, sizeof(b));
    if (b.magic != MAGIC_V3) return -1;
    if (b.crc != crc32(data, sizeof(b) - sizeof(b.crc))) return -1;
    if (b.board_key != boardKey(p._config)) return -1;
    if (b.set > SET_HIGH) return -1;
    if (b.set != SET_FAST && !(b.pass_us >= 5000 && b.pass_us <= 60000)) return -1;
    return (int)b.set;
  }
  if (len == sizeof(BlobV2)) {
    static BlobV2 b;
    memcpy(&b, data, sizeof(b));
    if (b.magic != MAGIC_V2) return -1;
    if (b.crc != crc32(data, sizeof(b) - sizeof(b.crc))) return -1;
    if (b.board_key != boardKey(p._config)) return -1;
    if (!(b.pass_us_normal >= 5000 && b.pass_us_normal <= 60000)) return -1;
    return SET_NORMAL;
  }
  return -1;
}

// Is this buffer a blob we should trust for this board?
inline bool valid(const EPD_Painter &p, const uint8_t *data, size_t len) {
  return setOf(p, data, len) >= 0;
}

// The installed tables live here for the lifetime of the program, because
// Config::trains points at them rather than copying. One store per
// quality: installing HIGH must not disturb a NORMAL set already in use.
inline uint8_t (*installedApply())[13]       { static uint8_t t[16][13]; return t; }
inline uint8_t (*installedRemove())[13]      { static uint8_t t[16][13]; return t; }
inline uint8_t (*installedApplyHigh())[32]   { static uint8_t t[16][32]; return t; }
inline uint8_t (*installedRemoveHigh())[32]  { static uint8_t t[16][32]; return t; }
inline uint8_t (*installedDirFast())[4][LEN_DIR] {
  static uint8_t t[4][4][LEN_DIR]; return t;
}
inline uint8_t *installedLum()      { static uint8_t t[16]; return t; }
inline uint8_t *installedLumHigh()  { static uint8_t t[16]; return t; }
inline uint8_t *installedLum4()     { static uint8_t t[4];  return t; }

// The measured render level of each grey, 255 = paper white .. 0 = black,
// for the quality named — or nullptr if that set has never been measured.
inline bool &haveLumFlag()      { static bool f = false; return f; }
inline bool &haveLumHighFlag()  { static bool f = false; return f; }
inline bool &haveLum4Flag()     { static bool f = false; return f; }
inline const uint8_t *levelLuminance()     { return haveLumFlag()     ? installedLum()     : nullptr; }
inline const uint8_t *levelLuminanceHigh() { return haveLumHighFlag() ? installedLumHigh() : nullptr; }
inline const uint8_t *levelLuminance4()    { return haveLum4Flag()    ? installedLum4()    : nullptr; }

// Install tables from a blob into the quality they belong to. Returns
// false — changing nothing — if the blob is not valid for this board, so
// it is safe to call on any hardware with any buffer. Call it for each
// blob you hold; the sets are independent.
inline bool install(EPD_Painter &p, const uint8_t *data, size_t len) {
  const int set = setOf(p, data, len);
  if (set < 0) return false;

  // Legacy v2: 16-grey NORMAL, 13-wide, no other sets.
  if (len == sizeof(BlobV2)) {
    static BlobV2 b;
    memcpy(&b, data, sizeof(b));
    memcpy(installedApply(),  b.apply,  sizeof(b.apply));
    memcpy(installedRemove(), b.remove, sizeof(b.remove));
    memcpy(installedLum(),    b.level_lum, sizeof(b.level_lum));
    bool any = false;
    for (int i = 0; i < 16; i++) any |= (b.level_lum[i] != 0);
    haveLumFlag() = any;
    p._config.trains.g16_apply  = installedApply();
    p._config.trains.g16_remove = installedRemove();
    p._config.level_lum = levelLuminance();
    p._config.g16_pass_us_normal = (int)b.pass_us_normal;
    p.rebuildDecisionTrains();
    return true;
  }

  // A v3 blob is widened into the current layout first, then installed by
  // the normal path — same drive codes, narrower rows.
  static Blob b;
  if (len == sizeof(BlobV3)) {
    static BlobV3 v3;
    memcpy(&v3, data, sizeof(v3));
    memset(&b, 0, sizeof(b));
    b.magic = MAGIC;  b.set = v3.set;  b.train_len = v3.train_len;
    b.board_key = v3.board_key;  b.pass_us = v3.pass_us;
    for (int g = 0; g < 16; g++) {
      memcpy(b.apply[g],  v3.apply[g],  20);
      memcpy(b.remove[g], v3.remove[g], 20);
    }
    memcpy(b.darker,  v3.darker,  sizeof(b.darker));
    memcpy(b.lighter, v3.lighter, sizeof(b.lighter));
    memcpy(b.direct,  v3.direct,  sizeof(b.direct));
    b.direct_loaded = v3.direct_loaded;
    memcpy(b.level_lum, v3.level_lum, sizeof(b.level_lum));
  } else {
    memcpy(&b, data, sizeof(b));
  }
  bool anyLum = false;
  for (int i = 0; i < 16; i++) anyLum |= (b.level_lum[i] != 0);

  switch ((Set)b.set) {
    case SET_NORMAL: {
      if (b.train_len && b.train_len != EPD_Painter::DEC_WF_LEN16) {
        printf("[EPD_PainterTuned] NORMAL blob is %d passes, engine is %d — refused\n",
               (int)b.train_len, EPD_Painter::DEC_WF_LEN16);
        return false;
      }
      p._config.row_extra_us_normal = (int)b.row_extra_us;
      // Narrow from the blob's full-width rows to the 13 NORMAL uses.
      for (int g = 0; g < 16; g++) {
        memcpy(installedApply()[g],  b.apply[g],  13);
        memcpy(installedRemove()[g], b.remove[g], 13);
      }
      memcpy(installedLum(), b.level_lum, 16);
      haveLumFlag() = anyLum;
      p._config.trains.g16_apply  = installedApply();
      p._config.trains.g16_remove = installedRemove();
      p._config.level_lum = levelLuminance();
      p._config.g16_pass_us_normal = (int)b.pass_us;
      p.rebuildDecisionTrains();
      return true;
    }
    case SET_HIGH: {
      // A table is only meaningful at the length it was tuned at: loading a
      // 25-slot set into a 20-pass engine truncates its last five slots and
      // silently changes every level it drives.
      if (b.train_len && b.train_len != EPD_Painter::DEC_WF_LEN16_HIGH) {
        printf("[EPD_PainterTuned] HIGH blob is %d passes, engine is %d — refused\n",
               (int)b.train_len, EPD_Painter::DEC_WF_LEN16_HIGH);
        return false;
      }
      p._config.row_extra_us_high = (int)b.row_extra_us;
      memcpy(installedApplyHigh(),  b.apply,  sizeof(b.apply));
      memcpy(installedRemoveHigh(), b.remove, sizeof(b.remove));
      memcpy(installedLumHigh(), b.level_lum, 16);
      haveLumHighFlag() = anyLum;
      p._config.trains.g16_apply_high  = installedApplyHigh();
      p._config.trains.g16_remove_high = installedRemoveHigh();
      p._config.level_lum_high = levelLuminanceHigh();
      p._config.g16_pass_us_high = (int)b.pass_us;
      p.rebuildDecisionTrains();
      return true;
    }
    case SET_FAST: {
      memcpy(p._config.waveforms.fast_darker,  b.darker,  sizeof(b.darker));
      memcpy(p._config.waveforms.fast_lighter, b.lighter, sizeof(b.lighter));
      memcpy(installedDirFast(), b.direct, sizeof(b.direct));
      // Only pairs the tune actually converged on are published; the rest
      // stay null so the engine keeps the DC-correct two-step for them.
      p._config.trains.dir_fast = b.direct_loaded ? installedDirFast() : nullptr;
      memcpy(installedLum4(), b.level_lum, 4);
      haveLum4Flag() = anyLum;
      p._config.level_lum4 = levelLuminance4();
      // Direct trains are bound at setDirectTransitions()/setQuality()
      // time, so nothing to rebuild here unless the engine is already up.
      p.reloadDirectTrains();
      return true;
    }
  }
  return false;
}

// Package 16-grey tables into `out` (must be BLOB_SIZE bytes). `set` is
// SET_NORMAL or SET_HIGH; train_len is that quality's pass count and
// period_us the pass period they were tuned at — both are what make the
// numbers mean anything later. level_lum may be null when the levels
// were not measured.
inline size_t build16(const EPD_Painter &p, Set set, uint32_t period_us,
                      int train_len,
                      const uint8_t apply[16][LEN16_MAX],
                      const uint8_t remove[16][LEN16_MAX],
                      const uint8_t *level_lum, uint8_t *out) {
  // Static: sizeof(Blob) is ~1.1 KB and build*() is called from deep
  // inside the tuner's call stack.
  static Blob b;
  memset(&b, 0, sizeof(b));
  b.magic = MAGIC;
  b.set = (uint8_t)set;
  b.train_len = (uint8_t)train_len;
  b.row_extra_us = (uint16_t)(set == SET_HIGH ? p._config.row_extra_us_high
                                              : p._config.row_extra_us_normal);
  b.le_hold_ns = (uint16_t)p._config.le_hold_ns;
  b.board_key = boardKey(p._config);
  b.pass_us = period_us;
  memcpy(b.apply,  apply,  sizeof(b.apply));
  memcpy(b.remove, remove, sizeof(b.remove));
  if (level_lum) memcpy(b.level_lum, level_lum, sizeof(b.level_lum));
  b.crc = crc32((const uint8_t *)&b, sizeof(b) - sizeof(b.crc));
  memcpy(out, &b, sizeof(b));
  return sizeof(b);
}

// Package the FAST 4-level set: the two waveform tables, the direct
// grey-to-grey trains, and which of the six pairs are real. level_lum4
// is the measured landing of the four levels, or null.
inline size_t buildFast(const EPD_Painter &p,
                        const uint8_t darker[3][7], const uint8_t lighter[3][7],
                        const uint8_t direct[4][4][LEN_DIR],
                        uint16_t direct_loaded,
                        const uint8_t *level_lum4, uint8_t *out) {
  // Static: sizeof(Blob) is ~1.1 KB and build*() is called from deep
  // inside the tuner's call stack.
  static Blob b;
  memset(&b, 0, sizeof(b));
  b.magic = MAGIC;
  b.set = (uint8_t)SET_FAST;
  b.train_len = 7;
  b.row_extra_us = 0;             // never applied at 4-level
  b.board_key = boardKey(p._config);
  b.pass_us = 0;
  memcpy(b.darker,  darker,  sizeof(b.darker));
  memcpy(b.lighter, lighter, sizeof(b.lighter));
  if (direct) memcpy(b.direct, direct, sizeof(b.direct));
  b.direct_loaded = direct_loaded;
  if (level_lum4) memcpy(b.level_lum, level_lum4, 4);
  b.crc = crc32((const uint8_t *)&b, sizeof(b) - sizeof(b.crc));
  memcpy(out, &b, sizeof(b));
  return sizeof(b);
}

}  // namespace EPD_PainterTuned

// ============================================================================
// tuned_storage.h — where THIS sketch keeps its tuned tables: NVS.
//
// The library (src/EPD_Painter_tuned.h) only knows how to build, validate
// and install a blob. Storage is NVS — namespace "epd_painter", one blob
// per quality under keys "fast" / "normal" / "high" — because that is the
// store EPD_Painter::begin() reads automatically: a board tuned here runs
// its measured tables in EVERY sketch from then on, not only in this one.
//
// One key per quality, because the three tunes are independent. Tuning
// HIGH must not cost you a good NORMAL set, and a board is perfectly
// entitled to carry NORMAL and nothing else.
//
// Earlier versions of this sketch stored the blobs as LittleFS files;
// tunedLoadAll() migrates any it finds into NVS once, so an already-tuned
// board keeps its tables across the change.
// ============================================================================

#pragma once

#include <nvs.h>
#include <LittleFS.h>            // legacy migration only
#include "EPD_Painter_tuned.h"

using EPD_PainterTuned::Set;
using EPD_PainterTuned::SET_FAST;
using EPD_PainterTuned::SET_NORMAL;
using EPD_PainterTuned::SET_HIGH;

// Must match what EPD_Painter::loadTunedFromNVS() reads.
static const char *TUNED_NVS_NS = "epd_painter";

static const char *tunedKey(Set s) {
  switch (s) {
    case SET_FAST:   return "fast";
    case SET_HIGH:   return "high";
    default:         return "normal";
  }
}

static const char *tunedSetName(Set s) {
  return s == SET_FAST ? "FAST" : s == SET_HIGH ? "HIGH" : "NORMAL";
}

// Legacy LittleFS paths written by older versions of this sketch.
static const char *tunedLegacyPath(Set s) {
  switch (s) {
    case SET_FAST:   return "/epd_tuned_fast.bin";
    case SET_HIGH:   return "/epd_tuned_high.bin";
    default:         return "/epd_tuned_normal.bin";
  }
}
// Older still: one file, predating the per-quality split; it IS a NORMAL set.
static const char *TUNED_PATH_LEGACY = "/epd_g16_tuned.bin";

// ---- NVS primitives --------------------------------------------------------

static size_t tunedNvsRead(const char *key, uint8_t *buf, size_t cap) {
  nvs_handle_t h;
  if (nvs_open(TUNED_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  size_t len = cap;
  const bool ok = (nvs_get_blob(h, key, buf, &len) == ESP_OK);
  nvs_close(h);
  return ok ? len : 0;
}

static bool tunedNvsWrite(const char *key, const uint8_t *buf, size_t len) {
  nvs_handle_t h;
  if (nvs_open(TUNED_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = (nvs_set_blob(h, key, buf, len) == ESP_OK) &&
                  (nvs_commit(h) == ESP_OK);
  nvs_close(h);
  return ok;
}

static void tunedNvsErase(const char *key) {
  nvs_handle_t h;
  if (nvs_open(TUNED_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

// ---- saving ---------------------------------------------------------------

// Keep one generation of history before overwriting.
//
// A save is destructive and irreversible, and a tune that took half an hour
// to produce a WORSE table will happily replace a good one - which has now
// cost two good tables in a single session, once to a mis-tapped SAVE
// button. The blob is ~1.5 KB and NVS has room, so there is no reason
// the previous set should not survive its replacement (key "<set>_bak").
static bool tunedWrite(Set s, const uint8_t *buf, size_t len) {
  const char *key = tunedKey(s);
  static uint8_t old[EPD_PainterTuned::BLOB_SIZE];
  const size_t n = tunedNvsRead(key, old, sizeof(old));
  if (n) {
    char bak[16];
    snprintf(bak, sizeof(bak), "%s_bak", key);
    if (!tunedNvsWrite(bak, old, n))
      printf("[tuned] WARNING: could not back up '%s' before overwriting\n", key);
  }
  return tunedNvsWrite(key, buf, len);
}

// 16-grey sets (NORMAL, HIGH). train_len is the quality's pass count and
// period_us the pass period they were tuned at — a train means nothing
// without both.
static bool tunedSave16(EPD_Painter &p, Set s, uint32_t period_us, int train_len,
                        const uint8_t apply[16][EPD_Painter::DEC_WF_LEN16_MAX],
                        const uint8_t remove[16][EPD_Painter::DEC_WF_LEN16_MAX],
                        const uint8_t *level_lum = nullptr) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  EPD_PainterTuned::build16(p, s, period_us, train_len, apply, remove,
                            level_lum, buf);
  return tunedWrite(s, buf, sizeof(buf));
}

// The FAST set: 4-level waveforms plus the six grey-to-grey direct trains.
static bool tunedSaveFast(EPD_Painter &p,
                          const uint8_t darker[3][7], const uint8_t lighter[3][7],
                          const uint8_t direct[4][4][EPD_Painter::DEC_WF_LEN_DIR],
                          uint16_t direct_loaded,
                          const uint8_t *level_lum4 = nullptr) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  EPD_PainterTuned::buildFast(p, darker, lighter, direct, direct_loaded,
                              level_lum4, buf);
  return tunedWrite(SET_FAST, buf, sizeof(buf));
}

// Replace ONLY the level curve inside an already-stored blob, leaving the
// trains and timing untouched, and install the result. For re-measuring
// the curve on different content (the staircase) without a retune: two
// scans instead of ten minutes. Current-format blobs only — a legacy blob
// would need widening first, and a full retune does that anyway.
static bool tunedStoreCurve(EPD_Painter &p, Set s, const uint8_t lum16[16]) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  const size_t n = tunedNvsRead(tunedKey(s), buf, sizeof(buf));
  if (n != sizeof(EPD_PainterTuned::Blob)) return false;
  if (EPD_PainterTuned::setOf(p, buf, n) != (int)s) return false;
  EPD_PainterTuned::Blob *b = (EPD_PainterTuned::Blob *)buf;
  memcpy(b->level_lum, lum16, 16);
  b->crc = EPD_PainterTuned::crc32(buf, sizeof(*b) - sizeof(b->crc));
  if (!tunedWrite(s, buf, n)) return false;
  return EPD_PainterTuned::install(p, buf, n);      // live immediately
}

// ---- loading --------------------------------------------------------------

// Read one set from NVS and install it. False (changing nothing) if
// absent, corrupt, or tuned on a different board.
static bool tunedLoad(EPD_Painter &p, Set s) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  const size_t n = tunedNvsRead(tunedKey(s), buf, sizeof(buf));
  return n && EPD_PainterTuned::install(p, buf, n);
}

// One-time migration: any valid blob still sitting in LittleFS (written by
// the file-based versions of this sketch) is copied into NVS, unless NVS
// already holds that set — a newer NVS save must never be clobbered by an
// old file. Files are left in place; 'X' erases them along with the keys.
static void tunedMigrateLegacy(EPD_Painter &p) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  for (int i = 0; i <= (int)SET_HIGH; i++) {
    const Set s = (Set)i;
    if (tunedNvsRead(tunedKey(s), buf, sizeof(buf))) continue;   // NVS wins
    const char *path = tunedLegacyPath(s);
    File f = LittleFS.open(path, "r");
    if (!f && s == SET_NORMAL) { path = TUNED_PATH_LEGACY; f = LittleFS.open(path, "r"); }
    if (!f) continue;
    const size_t n = f.read(buf, sizeof(buf));
    f.close();
    if (EPD_PainterTuned::setOf(p, buf, n) != (int)s) continue;
    if (tunedNvsWrite(tunedKey(s), buf, n))
      printf("[tuned] migrated %s from %s into NVS\n", tunedSetName(s), path);
  }
}

// Install every set this board has, migrating file-era blobs first.
// Returns how many sets are installed. (EPD_Painter::begin() has already
// installed whatever NVS held at boot; this call picks up migrated sets
// and is harmlessly repeat-safe for the rest.)
static int tunedLoadAll(EPD_Painter &p) {
  tunedMigrateLegacy(p);
  int n = 0;
  for (int s = 0; s <= (int)SET_HIGH; s++)
    if (tunedLoad(p, (Set)s)) n++;
  return n;
}

// ---- status ---------------------------------------------------------------

// Does NVS hold a valid blob for THIS board and THIS quality? Checking the
// set matters: the blobs are the same size and shape, so a plain validity
// test would call a HIGH blob written under the NORMAL key "present".
static bool tunedPresent(EPD_Painter &p, Set s) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  const size_t n = tunedNvsRead(tunedKey(s), buf, sizeof(buf));
  return n && EPD_PainterTuned::setOf(p, buf, n) == (int)s;
}

// ============================================================================
// Export — turn what this board measured into source for the per-board
// train files (src/*_Trains.h).
// ============================================================================
// A tuned blob is only on the board that produced it, which is fine for that
// board and no use to anyone else: a board that has never seen a scanner still
// needs tables, and the only reference that exists for a panel is a panel that
// has been measured. So the shipped defaults ARE one board's measurements,
// promoted. This prints them in the exact shape the train files expect, so the
// promotion is a paste rather than a transcription.
//
// Everything the tables are only valid AT is printed with them — pass period,
// row charge, latch width, train length. A table divorced from its timing is
// not a calibration, and the header has no other way to know.
static void tunedPrintTable(const char *name, const uint8_t (*rows)[EPD_Painter::DEC_WF_LEN16_MAX],
                            int width, const uint8_t *lum) {
  Serial.printf("inline const uint8_t %s[16][%d] = {\n", name, width);
  for (int g = 0; g < 16; g++) {
    Serial.printf("  /* %2d%s */ {", g, g == 0 ? " (white)" : "");
    for (int p = 0; p < width; p++)
      Serial.printf("%d%s", rows[g][p], p < width - 1 ? "," : "");
    Serial.print("},");
    if (lum) Serial.printf("  // measured %3d", lum[g]);
    Serial.println();
  }
  Serial.println("};");
}

static void tunedPrintLum(const char *name, const uint8_t *lum, int n) {
  Serial.printf("inline const uint8_t %s[%d] = {", name, n);
  for (int i = 0; i < n; i++) Serial.printf("%d%s", lum[i], i < n - 1 ? "," : "");
  Serial.println("};");
}

// prefix names the board, e.g. "H716" -> TUNED16_H716_HIGH.
static bool tunedExport(EPD_Painter &p, Set s, const char *prefix) {
  static uint8_t buf[EPD_PainterTuned::BLOB_SIZE];
  const size_t n = tunedNvsRead(tunedKey(s), buf, sizeof(buf));
  if (!n) { Serial.printf("// %s: no blob stored\n", tunedSetName(s)); return false; }
  if (EPD_PainterTuned::setOf(p, buf, n) != (int)s) {
    Serial.printf("// %s: stored blob is not a valid %s set for this board\n",
                  tunedSetName(s), tunedSetName(s));
    return false;
  }
  static EPD_PainterTuned::Blob b;
  memcpy(&b, buf, sizeof(b));

  char name[64];
  Serial.println();
  Serial.printf("// ---- %s, measured on this panel ----\n", tunedSetName(s));
  Serial.printf("// %d passes, pass period %lu us, row charge %u us, LE %u ns.\n",
                b.train_len, (unsigned long)b.pass_us, b.row_extra_us, b.le_hold_ns);
  Serial.println("// Valid ONLY at that timing - see Config::g16_pass_us_* / row_extra_us.");

  if (s == SET_FAST) {
    Serial.printf("inline const uint8_t FAST_%s_DARKER[3][7] = {\n", prefix);
    for (int r = 0; r < 3; r++) {
      Serial.print("  {");
      for (int c = 0; c < 7; c++) Serial.printf("%d%s", b.darker[r][c], c < 6 ? "," : "");
      Serial.println("},");
    }
    Serial.println("};");
    Serial.printf("inline const uint8_t FAST_%s_LIGHTER[3][7] = {\n", prefix);
    for (int r = 0; r < 3; r++) {
      Serial.print("  {");
      for (int c = 0; c < 7; c++) Serial.printf("%d%s", b.lighter[r][c], c < 6 ? "," : "");
      Serial.println("},");
    }
    Serial.println("};");
    // Direct trains. An all-float row means the pair was never converged and
    // must stay absent, so the loader falls back to the DC-correct two-step
    // rather than driving a train that does nothing.
    Serial.printf("inline const uint8_t DIRECT_%s_FAST[4][4][%d] = {\n",
                  prefix, EPD_Painter::DEC_WF_LEN_DIR);
    for (int fr = 0; fr < 4; fr++) {
      Serial.println("  {");
      for (int to = 0; to < 4; to++) {
        const bool loaded = (b.direct_loaded >> ((fr << 2) | to)) & 1;
        Serial.print("    {");
        for (int q = 0; q < EPD_Painter::DEC_WF_LEN_DIR; q++)
          Serial.printf("%d%s", loaded ? b.direct[fr][to][q] : 0,
                        q < EPD_Painter::DEC_WF_LEN_DIR - 1 ? "," : "");
        Serial.printf("},%s\n", loaded ? "" : "   // not converged - absent");
      }
      Serial.println("  },");
    }
    Serial.println("};");
    snprintf(name, sizeof(name), "LEVEL_LUM4_%s", prefix);
    tunedPrintLum(name, b.level_lum, 4);
    return true;
  }

  const int width = (s == SET_HIGH) ? EPD_Painter::DEC_WF_LEN16_MAX
                                    : EPD_Painter::DEC_WF_LEN16;
  snprintf(name, sizeof(name), "TUNED16_%s_%s", prefix,
           s == SET_HIGH ? "HIGH" : "NORMAL");
  tunedPrintTable(name, b.apply, width, b.level_lum);
  snprintf(name, sizeof(name), "TUNED16_%s_%s_REMOVE", prefix,
           s == SET_HIGH ? "HIGH" : "NORMAL");
  tunedPrintTable(name, b.remove, width, nullptr);
  snprintf(name, sizeof(name), "LEVEL_LUM_%s_%s", prefix,
           s == SET_HIGH ? "HIGH" : "NORMAL");
  tunedPrintLum(name, b.level_lum, 16);
  return true;
}

static void tunedEraseAll() {
  for (int i = 0; i <= (int)SET_HIGH; i++) {
    const Set s = (Set)i;
    tunedNvsErase(tunedKey(s));
    char bak[16];
    snprintf(bak, sizeof(bak), "%s_bak", tunedKey(s));
    tunedNvsErase(bak);
    LittleFS.remove(tunedLegacyPath(s));   // stop an erased set migrating back
  }
  LittleFS.remove(TUNED_PATH_LEGACY);
}

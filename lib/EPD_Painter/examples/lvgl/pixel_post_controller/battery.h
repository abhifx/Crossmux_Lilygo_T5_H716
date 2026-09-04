#pragma once

#include <Arduino.h>
#include <lvgl.h>

class TwoWire;  // forward-declare so battery.h doesn't pull in Wire.h

// ─────────────────────────────────────────────────────────────────────────────
// Battery abstraction
//
// Supports two backends, selected at runtime by which begin_* you call:
//
//   ADC   – GPIO voltage-divider (M5PaperS3, GPIO 4)
//   BQ25896 – LilyGo T5 S3 charger IC over I2C (not yet implemented)
//
// Typical usage:
//
//   // setup():
//   battery_begin_adc(4, 2.0f);   // M5PaperS3
//   battery_set_label(bat_lbl);
//   battery_update();              // show real value immediately
//   lv_timer_create(battery_timer_cb, 60000, nullptr);
// ─────────────────────────────────────────────────────────────────────────────

// ── Initialisation (call exactly one) ────────────────────────────────────────

// ADC voltage-divider backend (M5PaperS3).
//   pin       – ADC GPIO pin
//   div_ratio – Vbat = Vadc * div_ratio  (2.0 for a 100k/100k divider)
//   mv_full   – battery voltage at 100% charge (default 4200 mV)
//   mv_empty  – battery voltage at   0% charge (default 3000 mV)
void battery_begin_adc(uint8_t pin, float div_ratio,
                       int mv_full = 4200, int mv_empty = 3000);

// BQ25896 charger-IC backend (LilyGo T5 S3) – stub, not yet implemented.
void battery_begin_bq25896(TwoWire *wire, uint8_t addr = 0x6B);

// ── Runtime ───────────────────────────────────────────────────────────────────

// Returns battery charge 0-100, or -1 if uninitialised / unavailable.
int battery_read_pct();

// Set the LVGL label that displays battery state in the header.
void battery_set_label(lv_obj_t *lbl);

// Read battery and update the label immediately.
void battery_update();

// LVGL timer callback – pass to lv_timer_create(battery_timer_cb, 60000, nullptr).
void battery_timer_cb(lv_timer_t *);

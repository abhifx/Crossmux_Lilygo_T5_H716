#include "battery.h"
#include <Wire.h>

// ── Shared state ──────────────────────────────────────────────────────────────

static lv_obj_t *_bat_lbl = nullptr;

// charging=true swaps the battery icon for the charge symbol
static void _set_label_text(int pct, bool charging = false) {
    if (!_bat_lbl) return;
    const char *sym = charging
                    ? LV_SYMBOL_CHARGE
                    : pct > 75 ? LV_SYMBOL_BATTERY_FULL
                    : pct > 50 ? LV_SYMBOL_BATTERY_3
                    : pct > 25 ? LV_SYMBOL_BATTERY_2
                    : pct >  5 ? LV_SYMBOL_BATTERY_1
                               : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    lv_snprintf(buf, sizeof(buf), "%d%%  %s", pct, sym);
    lv_label_set_text(_bat_lbl, buf);
}

void battery_set_label(lv_obj_t *lbl) {
    _bat_lbl = lbl;
}

// ── Backend ───────────────────────────────────────────────────────────────────

static enum { NONE, ADC_DIV, BQ25896_I2C } _backend = NONE;

// ── ADC backend (M5PaperS3) ───────────────────────────────────────────────────

static uint8_t _adc_pin;
static float   _adc_div;
static int     _adc_mv_full;
static int     _adc_mv_empty;

void battery_begin_adc(uint8_t pin, float div_ratio, int mv_full, int mv_empty) {
    _adc_pin      = pin;
    _adc_div      = div_ratio;
    _adc_mv_full  = mv_full;
    _adc_mv_empty = mv_empty;
    analogSetPinAttenuation(pin, ADC_11db);
    _backend = ADC_DIV;
}

static int _read_adc() {
    // Average 4 readings to reduce noise
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(_adc_pin);
    int bat_mv = (int)(sum / 4 * _adc_div);
    return constrain((bat_mv - _adc_mv_empty) * 100 / (_adc_mv_full - _adc_mv_empty), 0, 100);
}

// ── BQ25896 backend (LilyGo T5 S3) ───────────────────────────────────────────
//
// Reg 0x0E — Battery Voltage (BATV):
//   bits [6:0]  BATV  →  Vbat (mV) = 2304 + BATV × 20
//   bit  [7]    THERM_STAT (ignored)
//
// Reg 0x0B — Charger Status 1:
//   bit  [7]    VBUS_GD   – power good
//   bits [6:3]  VBUS_STAT – VBUS status (non-zero = input present)
//   bits [2:0]  CHRG_STAT – 0=idle, 1=pre-charge, 2=fast charge, 3=done
//
// Same I²C pattern as EPD_PainterShutdown::isUsbConnected() in
// src/epd_painter_shutdown.cpp — do not merge; that code is internal to
// the library and should remain untouched.

static TwoWire *_wire    = nullptr;
static uint8_t  _bq_addr = 0x6B;

void battery_begin_bq25896(TwoWire *wire, uint8_t addr) {
    _wire    = wire;
    _bq_addr = addr;
    _backend = BQ25896_I2C;
}

// Read one register from the BQ25896. Returns false on bus error.
static bool _bq_read_reg(uint8_t reg, uint8_t &out) {
    _wire->beginTransmission(_bq_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(_bq_addr, (uint8_t)1) == 0) return false;
    out = _wire->read();
    return true;
}

static int _read_bq25896_pct() {
    if (!_wire) return -1;
    uint8_t reg0e;
    if (!_bq_read_reg(0x0E, reg0e)) return -1;
    int bat_mv = 2304 + (int)(reg0e & 0x7F) * 20;
    return constrain((bat_mv - 3000) * 100 / (4200 - 3000), 0, 100);
}

// Returns true when VBUS is present (USB connected / charging).
// Same logic as EPD_PainterShutdown::isUsbConnected().
static bool _bq25896_usb_present() {
    if (!_wire) return false;
    uint8_t reg0b;
    if (!_bq_read_reg(0x0B, reg0b)) return false;
    return (reg0b & 0xF8) != 0;  // VBUS_GD (bit 7) or any VBUS_STAT (bits 6:3)
}

// ── Public API ────────────────────────────────────────────────────────────────

int battery_read_pct() {
    switch (_backend) {
        case ADC_DIV:     return _read_adc();
        case BQ25896_I2C: return _read_bq25896_pct();
        default:          return -1;
    }
}

void battery_update() {
    int pct = battery_read_pct();
    if (pct < 0) return;
    bool charging = (_backend == BQ25896_I2C) && _bq25896_usb_present();
    _set_label_text(pct, charging);
}

void battery_timer_cb(lv_timer_t *) {
    battery_update();
}

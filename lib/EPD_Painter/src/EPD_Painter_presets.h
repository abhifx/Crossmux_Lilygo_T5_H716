#ifndef EPD_PAINTER_DEVICES_H
#define EPD_PAINTER_DEVICES_H

#include "EPD_Painter.h"
#include "EPD_Painter_trains.h"

// -----------------------------------------------------------------------
// LilyGo T5 4.7" EPD47 H716 / H715 (ESP32-S3 variant, capacitive touch)
//   74HCT4094D shift register with H716-specific bit layout:
//     QP0=EP_LE, QP1=PWR_DIS, QP2=POS_PWR, QP3=NEG_PWR,
//     QP4=EP_STV, QP5=SCAN_DIR, QP6=EP_MODE, QP7=EP_OE
//   CKV, STH, CKH and the 8-bit data bus remain direct GPIOs.
// -----------------------------------------------------------------------
inline EPD_Painter::Config EPD_LILYGO_EPD47_H716_PRESET = {
    .width    = 960,
    .height   = 540,
    .pin_pwr  = -1,             // shift register (power managed by H716 power driver)
    .pin_sph  = 40,             // STH  — direct GPIO
    .pin_oe   = -1,             // shift register QP7
    .pin_cl   = 41,             // CKH  — direct GPIO
    .pin_spv  = EPD_SR_PIN(4),  // EP_STV — shift register QP4
    .pin_ckv  = 38,             // CKV   — direct GPIO
    .pin_le   = EPD_SR_PIN(0),  // EP_LE — shift register QP0
    .quality  = EPD_Painter::Quality::QUALITY_NORMAL,
    .data_pins = { 8, 1, 2, 3, 4, 5, 6, 7 },  // D0–D7
    .i2c = { .sda = 18, .scl = 17, .freq = 100000 },
    .power = { .pca_addr = -1, .tps_addr = -1 },
    .waveforms = EPD_WF_H716,        // LilyGo_H716_Trains.h
    .shift = { .data = 13, .clk = 12, .strobe = 0, .le_time = 0,
               .driver = EPD_Painter::Shift::H716 },
    .g16_pass_us_normal = 20000,
    .g16_pass_us_high   = 20000,
    .trains = {
        .g16_apply       = TUNED16_H716_NORMAL,
        .g16_remove      = TUNED16_H716_NORMAL_REMOVE,
        .g16_apply_high  = TUNED16_H716_HIGH,
        .g16_remove_high = TUNED16_H716_HIGH_REMOVE,
        .dir_normal      = DIRECT_H716_NORMAL,
        .dir_fast        = DIRECT_H716_FAST,
    },
    .level_lum      = LEVEL_LUM_H716_NORMAL,
    .level_lum_high = LEVEL_LUM_H716_HIGH,
    .level_lum4     = LEVEL_LUM4_H716_FAST,
};

inline EPD_Painter::Config EPD_M5PAPER_S3_PRESET = {
    .width    = 960,
    .height   = 540,
    .pin_pwr    = 46,
    .pin_syspwr = 44,
    .pin_sph  = 13,
    .pin_oe   = 45,
    .pin_cl   = 16,
    .pin_spv  = 17,
    .pin_ckv  = 18,
    .pin_le   = 15,
    .quality  = EPD_Painter::Quality::QUALITY_NORMAL,
    .data_pins = { 6, 14, 7, 12, 9, 11, 8, 10 },
    .i2c = {
        .sda = 41,
        .scl = 42,
        .freq = 100000
    },
    .waveforms = EPD_WF_M5PAPERS3,
    .trains = {
        .g16_apply  = TUNED16_M5PAPERS3_NORMAL,
        .g16_remove = TUNED16_M5PAPERS3_NORMAL_REMOVE,
        .dir_normal = DIRECT_M5PAPERS3_NORMAL,
        .dir_fast   = DIRECT_M5PAPERS3_FAST,
    },
    .level_lum4 = LEVEL_LUM4_M5PAPERS3,
};

inline EPD_Painter::Config EPD_LILYGO_T5_S3_GPS_PRESET = {
    .width    = 960,
    .height   = 540,
    .pin_pwr  = -1,
    .pin_sph  = 41,
    .pin_oe   = -1,
    .pin_cl   = 4,
    .pin_spv  = 45,
    .pin_ckv  = 48,
    .pin_le   = 42,
    .quality  = EPD_Painter::Quality::QUALITY_NORMAL,
    .data_pins = { 5,6,7,15,16,17,18,8 },
    .i2c = {
        .sda = 39,
        .scl = 40,
        .freq = 100000
    },
    .power = {
        .pca_addr = 0x20,
        .tps_addr = 0x68,
    },
    .waveforms = EPD_WF_LILYGO_T5S3_GPS,
    .trains = {
        .g16_apply       = TUNED16_LILYGO_T5S3_NORMAL,
        .g16_remove      = TUNED16_LILYGO_T5S3_NORMAL_REMOVE,
        .g16_apply_high  = TUNED16_LILYGO_T5S3_HIGH,
        .g16_remove_high = TUNED16_LILYGO_T5S3_HIGH_REMOVE,
        .dir_normal      = DIRECT_LILYGO_T5S3_NORMAL,
        .dir_fast        = DIRECT_LILYGO_T5S3_FAST,
    },
    .level_lum      = LEVEL_LUM_LILYGO_T5S3_NORMAL,
    .level_lum_high = LEVEL_LUM_LILYGO_T5S3_HIGH,
    .level_lum4     = LEVEL_LUM4_LILYGO_T5S3,
};

inline EPD_Painter::Config EPD_LILYGO_T5_S3_H752_PRESET = {
    .width    = 960,
    .height   = 540,
    .pin_pwr  = -1,
    .pin_sph  = 9,
    .pin_oe   = -1,
    .pin_cl   = 10,
    .pin_spv  = EPD_SR_PIN(4),
    .pin_ckv  = 39,
    .pin_le   = EPD_SR_PIN(0),
    .quality  = EPD_Painter::Quality::QUALITY_NORMAL,
    .data_pins = { 11, 12, 13, 14, 21, 47, 45, 38 },
    .i2c = { .sda = 6, .scl = 5, .freq = 100000 },
    .power = { .pca_addr = -1, .tps_addr = -1 },
    .waveforms = EPD_WF_H752,
    .shift = { .data = 2, .clk = 42, .strobe = 1, .le_time = 0 },
};

#if defined(EPD_PAINTER_PRESET_M5PAPER_S3)
    inline EPD_Painter::Config& EPD_PAINTER_PRESET = EPD_M5PAPER_S3_PRESET;
#elif defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS)
    inline EPD_Painter::Config& EPD_PAINTER_PRESET = EPD_LILYGO_T5_S3_GPS_PRESET;
#elif defined(EPD_PAINTER_PRESET_LILYGO_T5_S3_H752)
    inline EPD_Painter::Config& EPD_PAINTER_PRESET = EPD_LILYGO_T5_S3_H752_PRESET;
#elif defined(EPD_PAINTER_PRESET_LILYGO_EPD47_H716)
    inline EPD_Painter::Config& EPD_PAINTER_PRESET = EPD_LILYGO_EPD47_H716_PRESET;
#else
    inline EPD_Painter::Config& EPD_PAINTER_PRESET = EPD_LILYGO_EPD47_H716_PRESET;
#endif

inline EPD_Painter::ProbeSettings Probe[] = {
    { &EPD_LILYGO_T5_S3_GPS_PRESET,      39, 40, 0x20, false },
    { &EPD_LILYGO_EPD47_H716_PRESET,     18, 17, 0x5D, false },
    { &EPD_LILYGO_EPD47_H716_PRESET,     18, 17, 0x14, false },
    { &EPD_LILYGO_EPD47_H716_PRESET,     18, 17, 0x6B, false },
    { &EPD_LILYGO_T5_S3_H752_PRESET,      6,  5, 0x51, false },
    { &EPD_M5PAPER_S3_PRESET,            41, 42, 0x51, false },
};

#endif

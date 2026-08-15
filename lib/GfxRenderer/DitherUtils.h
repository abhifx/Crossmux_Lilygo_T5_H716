#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering (0-15)
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// 8x8 Bayer matrix for higher quality ordered dithering (0-63)
inline const uint8_t bayer8x8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21}
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y, uint8_t mode = 1) {
  if (mode == 0) return gray / 64; // DITHER_NONE

  int bayer = (mode == 2) ? (bayer8x8[y & 7][x & 7] / 4) : bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 20; // Aggressive noise strength

  int adjusted = static_cast<int>(gray) + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  return static_cast<uint8_t>(adjusted / 64);
}

// Apply Bayer dithering for 16 levels (4-bit)
inline uint8_t applyBayerDither16Level(uint8_t gray, int x, int y, uint8_t mode = 1) {
  if (mode == 0) return gray; // DITHER_NONE

  int bayer = (mode == 2) ? (bayer8x8[y & 7][x & 7] / 4) : bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 3; // Visible texture for 16-level

  int adjusted = static_cast<int>(gray) + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  return static_cast<uint8_t>(adjusted);
}

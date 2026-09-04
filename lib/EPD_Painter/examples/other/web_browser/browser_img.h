// A dithered image, held as LEVEL CODES ready to blit.
//
// This lives in a header rather than the .ino for a mechanical reason: the
// Arduino build generates prototypes for the sketch's functions and inserts
// them directly after the last #include, which is ABOVE anything the .ino
// declares itself. A function taking `Img &` therefore gets a prototype that
// mentions a type the compiler has not seen yet. Types used in sketch
// function signatures have to arrive via an include.
#pragma once
#include <stdint.h>

struct Img {
  int      w, h;
  uint8_t *bw;   // one-bit halftone: ink or paper only
};

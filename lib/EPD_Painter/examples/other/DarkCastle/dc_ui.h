// ============================================================================
// dc_ui.h -- the touch column beside the playfield.
//
// The 192px the 16:9 panel has spare over the Mac's 4:3 is not padding, it is
// the controls. Buttons live here rather than in the sketch because the
// Arduino builder emits its prototypes ahead of anything declared in the .ino,
// so a struct used in a function signature has to come from a header.
// ============================================================================

#pragma once
#include <stdint.h>
#include "dc_video.h"

struct Btn {
  int16_t     x, y, w, h;
  const char *label;
};

// Laid out for thumbs: direction low and split, jump wide above it so it can
// be hit without looking, the two modifiers along the bottom.
static const Btn BTN_JUMP  = { dc::PANEL_X +  8, 200, 176,  90, "JUMP" };
static const Btn BTN_LEFT  = { dc::PANEL_X +  8, 300,  86,  86, "<" };
static const Btn BTN_RIGHT = { dc::PANEL_X + 98, 300,  86,  86, ">" };
static const Btn BTN_DUCK  = { dc::PANEL_X +  8, 396,  86,  70, "DUCK" };
static const Btn BTN_RUN   = { dc::PANEL_X + 98, 396,  86,  70, "RUN" };

static inline bool inBtn(const Btn &b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

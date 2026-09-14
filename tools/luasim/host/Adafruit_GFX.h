// Stand-in for Adafruit_GFX.h in host builds of src/lua (tools/luasim/fxhost).
// src/fonts/picopixel_fb.h and the stock Fonts/Picopixel.h need only PROGMEM
// and the two font structs, and those come from the library's own gfxfont.h,
// found on the include path in .pio/libdeps - so the host reads the same
// glyph tables the firmware does.
#pragma once
#include <stdint.h>
#define PROGMEM
#include "gfxfont.h"

#pragma once
// Host stand-in for the Arduino header that Adafruit's font files include:
// only the glyph structs, so src/market/market_pico.cpp reads the Picopixel
// metrics on the host from the same table the panel uses. Nothing else.
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#include "gfxfont.h"

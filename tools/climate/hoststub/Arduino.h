#pragma once
// Host stand-in for the Arduino core: only what Adafruit GFX's Adafruit_GFX.h
// and Adafruit_GFX.cpp need to compile, so tools/climate/weather_screen_host.cpp
// can run the weather screen's layout on the real library. Nothing else.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Print.h"

#ifndef PROGMEM
#define PROGMEM
#endif

typedef bool boolean;

// Arduino.h's angle macros, used by a GFX function the layout never calls.
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)

// Named by getTextBounds() overloads the layout never calls.
class __FlashStringHelper {};
class String {
 public:
  const char *c_str() const { return ""; }
  unsigned length() const { return 0; }
};

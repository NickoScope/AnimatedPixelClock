#pragma once
// The condition groups the weather clock's icon renderer understands, mapped
// from WMO codes by weatherIconFromCode() (weather.cpp). Plain C++, so the
// weather screen's layout (src/clocks/weather_layout.h) compiles on the host.

#include <stdint.h>

enum WeatherIconKind : uint8_t {
  WICON_SUN = 0,
  WICON_PARTCLOUD,
  WICON_CLOUD,
  WICON_FOG,
  WICON_RAIN,
  WICON_SNOW,
  WICON_STORM,
};

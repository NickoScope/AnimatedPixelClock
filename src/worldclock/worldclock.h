#pragma once
// World clock: a dotted map where daylight is lit and night is dim.
//
// 128x64 divides exactly into 64x32 dots on a 2 px pitch, so the look needs no
// rescaling. Land in daylight is bright, night dim, and civil twilight blends
// between them, so the terminator draws itself and creeps across the day.
// The prototype this was tuned in is tools/luasim/scripts/world_clock.lua;
// both read the same generated mask and city list.

#include <stdint.h>
#include <ArduinoJson.h>

#if defined(WORLDCLOCK_ENABLED)

#if !defined(CONTROL_ENCODER_ENABLED)
#error "WORLDCLOCK_ENABLED needs CONTROL_ENCODER_ENABLED: the page is reached with the knob"
#endif

// Draws one frame. The caller has already cleared the screen.
void worldClockRender();

// Home is the city whose dot breathes. The generated list puts it first; the
// web portal can pick another one (persisted by src/panel).
uint8_t worldClockCityCount();
uint8_t worldClockHome();
void    worldClockSetHome(uint8_t city);

// The map and the cities, for the portal's preview: the same mask and the same
// list the page draws, so the two cannot disagree.
void    worldClockMapJson(JsonObject out);

#endif

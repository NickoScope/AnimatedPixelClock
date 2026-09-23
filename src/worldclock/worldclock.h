#pragma once
// World clock: a dotted map where daylight is lit and night is dim.
//
// 128x64 divides exactly into 64x32 dots on a 2 px pitch, so the look needs no
// rescaling. Land in daylight is bright, night dim, and civil twilight blends
// between them, so the terminator draws itself and creeps across the day.
// The big time is home's, in home's own zone and summer time; home's name sits
// beside it and pulses with home's dot for a while each time home changes.
//
// The prototype this was tuned in is tools/luasim/scripts/world_clock.lua;
// both read the same generated mask and city list, and tools/luasim/fx_parity.py
// renders this file on the host against it, pixel for pixel.

#include <stddef.h>
#include <stdint.h>

#if defined(WORLDCLOCK_ENABLED)

#if !defined(CONTROL_ENCODER_ENABLED)
#error "WORLDCLOCK_ENABLED needs CONTROL_ENCODER_ENABLED: the page is reached with the knob"
#endif

// ---------------------------------------------------------------- cities
// Ids: 0.. the built-in list (worldmap.h), WC_ID_CUSTOM + slot for a city the
// portal added, WC_ID_AUTO for one made at the panel's own location. An id
// never moves: deleting one custom city leaves the others' ids alone, so a
// stored home still means the same place.
#define WC_CUSTOM_MAX  6
#define WC_ID_CUSTOM   100
#define WC_ID_AUTO     200
#define WC_NAME_MAX    20       // bytes: 20 Latin letters, 10 Cyrillic
#define WC_NAME_PX     72       // Picopixel advance; see NAME_X in worldclock.cpp
#define WC_POSIX_MAX   63       // as settings.timezoneString
#define WC_IANA_MAX    39

struct WcCity {
  char  name[WC_NAME_MAX + 1];      // capitals A-Z А-Я, 0-9, space . - ' (src/fonts/name_chars.h)
  float lat, lon;
  char  posix[WC_POSIX_MAX + 1];
  char  iana[WC_IANA_MAX + 1];      // "" when the zone is the panel's own setting
};

uint8_t     worldClockDefaultCount();
bool        worldClockCity(uint8_t id, WcCity *out);     // false: no such city now
// Why a city cannot be kept, in words the portal can show; nullptr if it can.
const char *worldClockCheck(const WcCity &c);
int         worldClockNameWidth(const char *name);        // pixels, as the page draws it
// Fill or empty (nullptr) a custom slot. False: bad slot, or refused by worldClockCheck.
bool        worldClockSetCustom(uint8_t slot, const WcCity *city);
bool        worldClockSlotUsed(uint8_t slot);
// The city at the panel's location, made when nobody chose a home and no city
// is near it; nullptr removes it.
void        worldClockSetAuto(const WcCity *city);
// A place name from outside - UTF-8, any case - as the page can draw it: Latin
// letters without their accents, capitals, and cut after a word or before a
// hyphen to fit the room; mid-word only when not even one word fits.
// out is "" when nothing drawable is left.
void        worldClockFitName(const char *utf8, char *out, size_t n);

// ---------------------------------------------------------------- home
uint8_t  worldClockHome();
bool     worldClockHomeChosen();
// chosen: picked by the owner; false: worked out from where the panel is. An
// id that is not a city falls back to the first built-in one. A different city
// than before starts the name's pulse on the page's next frame.
void     worldClockSetHome(uint8_t id, bool chosen);
int32_t  worldClockHomeOffset(int64_t utc);                // seconds east of UTC
uint32_t worldClockPulseLeftMs();                          // 0: settled, or not yet shown

// ---------------------------------------------------------------- drawing
// One frame, now. The caller has already cleared the screen.
void worldClockRender();
// The frame for a given moment, which worldClockRender() and tools/luasim/wchost
// both call: UTC seconds, the breathing clock in seconds (period 1 s), and
// seconds since home changed (below 0: long ago, the name holds still).
void worldClockDraw(int64_t utc, bool synced, float breath, float since);

#if !defined(WORLDCLOCK_HOST)
#include <ArduinoJson.h>
// The map and the cities, for the portal's preview: the same mask and the same
// list the page draws, so the two cannot disagree.
void worldClockMapJson(JsonObject out);
#endif

#endif

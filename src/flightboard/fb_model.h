#pragma once
// The flight board's data model: sizes and the status vocabulary.
//
// Plain C++, no Arduino core: flightboard.cpp, the AeroAPI transform and the
// host tests in tools/flightboard/ all include it.

#include <cstdint>

#define FB_MAX_ROWS 15   // the most a board keeps, as Home Assistant sends at most
#define FB_FN_LEN   9
#define FB_TM_LEN   6
#define FB_CT_LEN   5
#define FB_CY_LEN   15   // city name, or ct when there is no name
#define FB_CITY_MAX 10   // characters shown in the destination column

enum FbStatus : uint8_t {
  FB_SCHED = 0, FB_BOARD, FB_DEP, FB_LAND, FB_DELAY, FB_CANC, FB_UNKNOWN
};

// ── airports ────────────────────────────────────────────────────────────────
// Ids: 0.. the built-in list, FB_APT_CUSTOM + slot for one added in the portal.
// An id never moves, so a stored selection still means the same airport after
// another custom one is deleted. The built-in ids are the old list indices, so
// NVS "panel"/fbApt written before custom airports existed keeps its meaning.
#define FB_APT_CUSTOM_MAX 6
#define FB_APT_CUSTOM     100
#define FB_APT_NAME_MAX   12     // bytes: 12 Latin letters, 6 Cyrillic
// Picopixel advance. The header is the name, 6 px, DEPARTURES (40 px) and the
// clock right-aligned to x 126 (18 px, from x 108): a name up to 56 px leaves
// 4 px before the clock. AMSTERDAM, the widest built-in name, is 40.
#define FB_APT_NAME_PX    56
#define FB_APT_TZ_MAX     39     // as WC_IANA_MAX

struct FbAirport {
  char icao[5];                        // 4 of A-Z 0-9, starting with a letter
  char iata[4];                        // 3 of A-Z, or ""
  char name[FB_APT_NAME_MAX + 1];      // capitals A-Z А-Я, 0-9, space . - ' (src/fonts/name_chars.h)
  char tz[FB_APT_TZ_MAX + 1];          // IANA name, "" when unknown
};

// ── tracked flights ─────────────────────────────────────────────────────────
#define FB_TRACK_MAX   3
#define FB_IDENT_LEN   9     // "EZY4567" and room for a suffix letter

enum FbTrackState : uint8_t {
  FB_TRK_WAIT = 0,     // not asked yet
  FB_TRK_NOTFOUND,     // AeroAPI knows no flight by that ident in the window
  FB_TRK_SCHED,        // not left the gate
  FB_TRK_DELAYED,      // not left the gate, departure delay past the threshold
  FB_TRK_TAXI,         // left the gate, not airborne
  FB_TRK_ENROUTE,      // airborne
  FB_TRK_LANDED,       // on the runway or at the gate
  FB_TRK_CANCELLED,
  FB_TRK_DIVERTED,
  FB_TRK_COUNT
};

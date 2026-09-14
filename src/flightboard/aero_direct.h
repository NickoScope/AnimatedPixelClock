#pragma once
// Flight board, direct: the panel asks FlightAware AeroAPI itself.
//
// Home Assistant's MQTT boards remain only as the fallback for a build without
// this flag, or for a panel with no key stored. AeroAPI is paid per call, so
// every call goes through the guards in fb_settings.h and the scheduler in
// aero_direct.cpp: the selected airport only, board lists only while the page
// is on screen or was a moment ago, a floor per list, a day and a month cap
// held in NVS before each call is made, one call at a time at least 7 s apart,
// and a growing wait after any failure.
//
// A call runs in a FreeRTOS task of its own, created when it is due and gone
// when it ends (the rail board's pattern, src/railboard/rtt_direct.cpp). The
// loop task starts it and takes the result.
//
// NVS:
//   "aero"   key    string   the AeroAPI key, written by env:provision; never
//                            logged, printed, published or returned by the portal
//   "fbuse"  u      blob     the call counters (fbs::Usage), written before each call
//   "fbcfg"  floor, day, mon   u16  the budget; absent = the defaults
//            trk0-trk2          blob tracked flights: ident and when it was added
//
// src/flightboard/README.md "Direct from AeroAPI" has the sources and the costs.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#if defined(FLIGHTBOARD_DIRECT_ENABLED)

#if !defined(FLIGHTBOARD_ENABLED)
#error "FLIGHTBOARD_DIRECT_ENABLED needs FLIGHTBOARD_ENABLED: it fills that page"
#endif
#if !defined(CONTROL_ENCODER_ENABLED)
#error "FLIGHTBOARD_DIRECT_ENABLED needs CONTROL_ENCODER_ENABLED: its airports, trackers and budget are set in the portal's panel routes"
#endif

#include "aero_transform.h"
#include "fb_settings.h"

// Board lists stay wanted for this long after the page leaves the screen, so a
// knob turned past the page and back does not count as a new visit. A choice.
static const uint32_t AERO_VISIBLE_GRACE_S = 120;

struct AeroWant {
  const char *icao;        // the selected airport
  bool        arr, dep;    // the halves the page shows
  bool        visible;     // on screen now, or within AERO_VISIBLE_GRACE_S
};

void aeroDirectBegin();                  // setup(): reads the key's presence, the counters, budget and trackers
void aeroDirectLoop(const AeroWant &w);  // loop task, every pass

bool aeroDirectHasKey();                 // a key is stored: the page's source is AeroAPI, not Home Assistant

// The lists for w.icao as last fetched; nullptr when not fetched since the
// airport was selected. *ageS: seconds since that fetch.
const aero::ListResult *aeroDirectList(aero::List l, uint32_t *ageS);
uint32_t aeroDirectListGen();            // changes whenever a list arrives or the lists are dropped
void     aeroDirectAirportChanged();     // drop the lists; the new airport's are due at once

// A word for the page when it has no board: "NO KEY", "FETCHING", "DAY CAP", "AUTH 401", ...
const char *aeroDirectState();

// ── budget ──────────────────────────────────────────────────────────────────
const fbs::Budget &aeroDirectBudget();
void aeroDirectSetBudget(const fbs::Budget &b);   // in force now, in NVS 2.5 s later

// ── tracked flights ─────────────────────────────────────────────────────────
struct AeroTracker {
  char        ident[FB_IDENT_LEN];
  uint32_t    added;       // UTC epoch; 0 when added before the clock was set
  aero::Track t;           // t.state FB_TRK_WAIT until the first answer
  int64_t     nextAt;      // UTC epoch of the next call; 0 = due, INT64_MAX = no more
  uint32_t    fetchedMs;   // millis() of the last answer, 0 = none
  int16_t     http;        // the last call's status, 0 = none
};

uint8_t            aeroTrackCount();
const AeroTracker *aeroTrack(uint8_t i);
// nullptr on success, else why not in words the portal can show. The ident is
// normalised first (aero::normaliseIdent).
const char *aeroTrackAdd(const char *ident, bool *full);
bool        aeroTrackRemove(const char *ident);   // the normalised ident; false when not tracked

// For /api/flightboard: key presence (never the key), state, budget, usage and
// estimated cost, and the last call's diagnostics.
void aeroDirectStatusJson(JsonObject out);
void aeroDirectTracksJson(JsonArray out);

#endif  // FLIGHTBOARD_DIRECT_ENABLED

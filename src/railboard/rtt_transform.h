#pragma once
// Realtime Trains /gb-nr/location -> the rail board's two lists, in C++.
//
// A port of the "Normalise both lists" template in
// tools/railboard/ha_package_railboard.yaml, rule for rule, so the direct
// fetch fills exactly the board state the MQTT ingest fills. Field sources are
// cited there, with line numbers in realtimetrains/api-specification main.yml.
//
// Plain C++ and ArduinoJson, no Arduino core: tools/railboard/check_direct.py
// compiles and tests it on the host.

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

#include "rb_model.h"

namespace rtt {

// The window and grace the package uses (lookback_min, window_min, grace_s).
static const uint16_t kLookbackMin = 30;
static const uint16_t kWindowMin   = 90;
static const uint16_t kGraceS      = 60;

// RFC 3339 date-time to UTC epoch seconds: "Z" or a +hh:mm / -hh:mm / +hhmm
// zone, optional fractional seconds. A time with no zone is refused rather
// than guessed: RTT's schema says RFC 3339 (main.yml line 183). Home
// Assistant's as_timestamp would read such a time in its own zone instead, so
// that case - which the spec rules out - is where the two ports can differ.
bool parseTime(const char *s, int64_t *out);

// UTC epoch seconds to "YYYY-MM-DDTHH:MM:SSZ". `cap` must be at least 21.
void formatTime(int64_t utc, char *out, size_t cap);

// The query the package sends: "code=GLD&timeFrom=<now - 30 min>Z&timeWindow=90",
// or "code=GLD" alone when there is no clock to count from.
void buildQuery(const char *crs, int64_t nowUtc, bool haveClock, char *out, size_t cap);

// Filters for deserializeJson: only what the transform reads is kept.
void locationFilter(JsonDocument &filter);
void tokenFilter(JsonDocument &filter);

struct Lists {
  RbBoard  board[2];      // RB_DEP, RB_ARR; the caller sets ts, rxMs and have
  char     stn[RB_STN_LEN];
  char     rt[RB_RT_LEN];
  uint16_t seen;          // services in the answer, before any filtering
  // Why services or their events were left out - for a board that fetches and
  // shows nothing. firstT/firstNow: the first advertised time met, and now.
  uint16_t skipDisp, skipPax, noEvent, noSched, skipCall, skipTime;
  int64_t  firstT, firstNow;
};

// A 200 answer. False when the document has no services array at all, which
// the package reports as BAD; the lists are then left empty.
bool transform(JsonVariantConst root, int64_t nowUtc, const char *crs, Lists *out);

// A 204 answer - no services in the window - is valid and empty. The package
// then names the station by its code, and so does this.
void emptyLists(const char *crs, Lists *out);

// /api/get_access_token answers {token, entitlements, validUntil}
// (main.yml lines 1644-1669). False without a token. validUntil is 0 when it
// is missing or unreadable.
bool accessToken(JsonVariantConst root, char *token, size_t cap, int64_t *validUntil);

}  // namespace rtt

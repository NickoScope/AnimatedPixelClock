#pragma once
// Rail board, direct: the panel asks Realtime Trains itself.
//
// Home Assistant's MQTT boards stay the fallback. A fetch runs in a FreeRTOS
// task of its own, created when a poll is due and gone when it ends, so the
// TLS handshake and the download never stall a frame, the knob or the web
// server, and nothing large lives between polls. The loop task starts it and
// takes the finished lists; parsing into the board state stays on the loop
// task, where the MQTT ingest and the render already are.
//
// The token lives in NVS, namespace "rb", written by env:provision:
//   token   string  the RTT token as issued, without "Bearer "
//   kind    string  optional: "refresh" (exchange it first) or "access" (never
//                   exchange); absent = try it as an access token and exchange
//                   it on a 401
// A refresh token is exchanged at GET /api/get_access_token for a short-life
// access token, kept in PSRAM only until its validUntil less 5 minutes. No
// token is ever logged, printed, published or returned by the web portal.
//
// Why, how much memory, and what was verified: src/railboard/README.md "Direct".

#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

#if defined(RAILBOARD_DIRECT_ENABLED)

#include "rtt_transform.h"

void rttDirectBegin();                 // setup(): reads whether a token is stored

// Loop task, every pass: starts a fetch when one is due, and hands over a
// finished one. True when `out` holds fresh lists for `crs`, fetched at
// *fetchedAt (UTC epoch seconds).
bool rttDirectLoop(const char *crs, rtt::Lists *out, int64_t *fetchedAt);

void rttDirectStationChanged();        // fetch the new station now
bool rttDirectHasToken();
bool rttDirectAuthRefused();           // RTT refused the stored token

// Diagnostics: "DIRECT OK 200" / "DIRECT AUTH 401" / "HA ONLY", and the day's
// remaining requests as RTT last reported them ("" when unknown).
void rttDirectLine(char *value, size_t valueCap, char *quota, size_t quotaCap);

// For /api/railboard. Token presence, kind and expiry - never the token.
void rttDirectStatusJson(JsonObject out);

#endif  // RAILBOARD_DIRECT_ENABLED

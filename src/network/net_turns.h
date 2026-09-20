#pragma once
// Taking turns on the network, and who yields to whom.
//
// Why this is needed, measured 2026-09-20 (the knowledge base,
// drafts/28-portal-hang-2026-09-20.md): a background fetch costs 12-16 KB of
// internal RAM - its task stack, the socket, and what mbedTLS keeps internal -
// and the web server costs 9-15 KB more while it serves a browser. The board
// has about 34 KB of internal heap. Together they do not fit: internal free
// reached **144 bytes**, the Wi-Fi task could not get its 1,626 B buffers, and
// the panel left the network. The fetchers have taken turns among themselves
// since 2026-09-14 (net_lock.h); the web server never joined them, and it is
// the largest consumer of the three.
//
// Two rules, and they point in opposite directions on purpose.
//
//   1. While a fetch holds the lock, the portal's **large** responses are
//      refused with 503 and a Retry-After. The small diagnostics are never
//      refused - /api/info is how anyone finds out what is happening, and a
//      diagnostic that goes quiet exactly when it is needed is worse than
//      useless. The web must not WAIT for the lock: handleClient() runs inside
//      loop(), and blocking there costs frames and eventually the watchdog.
//
//   2. While a person is using the portal, a background fetch stands aside.
//      A human clicking beats a refresh that can happen a minute later. But
//      only so far: a browser left open on the portal polls for ever, so a
//      fetch that always yields would never run again and the page it feeds
//      would go quietly stale. After NET_TURN_MAX_YIELDS refusals in a row the
//      fetch goes anyway.
//
// Both windows are **our choice, not measured figures**, in the manner of
// fx3d_profile.h's retry count. The quiet window is set against the portal's
// own measured cadence, not an assumed one: the page asks every 5 s
// (/api/status, /api/panel), every 3 s (/metrics, and only on its page) and
// every 2 s while the network log is on (web_pages.h). So 1.5 s of quiet is a
// gap the page leaves routinely - except with the log on, where the gaps close
// and the yield ceiling above is what lets the data through.

#include <stdint.h>

// What netMsSinceHttp() returns when nothing has ever asked. Defined here as
// well as in network.h so the host test can drive the rule without Arduino.
#ifndef NET_HTTP_NEVER
#define NET_HTTP_NEVER 0x7FFFFFFFUL
#endif

// How quiet the web has to have been before a fetch starts.
#define NET_TURN_QUIET_MS 1500UL

// ...and how long a fetch may be asked to stand aside before it goes anyway.
//
// This counted yields, not time, and that was wrong: the starters run on every
// pass of loop(), which has no delay in it, so forty passes burned in
// milliseconds - faster than the quiet window itself could close. The mechanism
// was inert for exactly the two fetches that hold the network longest. Time
// does not care how often loop() runs.
#define NET_TURN_MAX_YIELD_MS 60000UL

// Should a background fetch stand aside right now?
//   msSinceHttp     - how long since we served any HTTP request
//   yieldingSinceMs - when this fetch first stood aside, 0 if it is not waiting
//   nowMs           - millis()
// Pure, so the host test drives it without a panel. Unsigned elapsed time, so
// the millis() wrap needs no special case.
static inline bool netTurnYield(uint32_t msSinceHttp, uint32_t yieldingSinceMs, uint32_t nowMs) {
  if (msSinceHttp >= NET_TURN_QUIET_MS) return false;         // quiet: go
  if (!yieldingSinceMs) return true;                          // first time: wait
  return (uint32_t)(nowMs - yieldingSinceMs) < NET_TURN_MAX_YIELD_MS;
}

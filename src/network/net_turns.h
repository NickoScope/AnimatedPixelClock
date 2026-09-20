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
// fx3d_profile.h's retry count: a page load's burst is over in well under a
// second, so a second and a half of quiet means the browser has stopped asking.

#include <stdint.h>

// What netMsSinceHttp() returns when nothing has ever asked. Defined here as
// well as in network.h so the host test can drive the rule without Arduino.
#ifndef NET_HTTP_NEVER
#define NET_HTTP_NEVER 0x7FFFFFFFUL
#endif

// How quiet the web has to have been before a fetch starts.
#define NET_TURN_QUIET_MS 1500UL

// ...and how many times in a row a fetch may be asked to stand aside before it
// stops asking. At one poll a second from an open portal this is about a
// minute of deference, after which the data wins.
#define NET_TURN_MAX_YIELDS 40

// Should a background fetch stand aside right now? Pure, so the host test drives
// it without a panel.
static inline bool netTurnYield(uint32_t msSinceHttp, uint32_t yieldsSoFar) {
  return msSinceHttp < NET_TURN_QUIET_MS && yieldsSoFar < NET_TURN_MAX_YIELDS;
}

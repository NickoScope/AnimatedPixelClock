#pragma once
// The portal steps back while the radio is starving.
//
// Measured on the panel 2026-09-20 (the knowledge base,
// drafts/28-portal-hang-2026-09-20.md). A browser opening the portal makes six
// to twelve requests at once. The web server is synchronous, so the rest queue
// in lwIP, each holding internal RAM - the board has about 34 KB of it free.
// Internal free RAM fell to 7,232 B, and the **Wi-Fi task** could then not
// allocate its 1,626-byte DMA buffers (caps 0x80c): 25 failures in eleven
// seconds. Those are the buffers packets arrive in. One `GET /` took 4,224 ms,
// and the panel left the network for minutes - with the firmware still running,
// the clock still on the screen, which is why only a reset appears to fix it.
//
// What this does NOT do: invent a heap threshold. The failing allocations were
// seen with the largest free internal block anywhere from 7,668 to 12,788 B and
// not seen at 14,324 B and above, which is four samples - too few to make a
// number a rule (the project's rule: a threshold comes from a distribution, or
// it is reference only). So the panel reacts to the radio's own failure signal
// instead, which it already counts, and no threshold is guessed.
//
// While the signal is fresh the portal's large blobs - the page and its assets,
// kilobytes each, seconds to send under load - are refused with 503. The small
// JSON endpoints are never refused: /api/info is how anyone finds out what is
// wrong, and a diagnostic that goes quiet exactly when it is needed is worse
// than useless.

#include <stdint.h>

// What allocFailWifiAgeMs() returns when the Wi-Fi task has never failed an
// allocation. Far larger than any backoff, so "never" always reads as calm.
#define ALLOC_FAIL_WIFI_NEVER 0x7FFFFFFFUL

// How long after the radio's last failed allocation the portal holds back its
// big blobs. **Our choice, not a measured figure**, in the manner of
// src/fx3d/fx3d_profile.h's retry count: the observed bursts ran about eleven
// seconds, and each further blob is what feeds them, so the window only has to
// outlast one browser's retry - it re-arms on every new failure.
#define WEB_HEAP_BACKOFF_MS 3000UL

// ...but it must not re-arm for ever. Any traffic can starve those buffers -
// mDNS, MQTT, a broadcast - not only the portal, so a panel that is chronically
// short would answer 503 with no way back and no sign of it on the screen. After
// this many refusals in a row one blob goes through regardless, and the count
// starts again: the symptom then degrades to a slow portal rather than a silent
// one. **Our choice as well**; a browser's first load asks for about half a
// dozen, so this is roughly "one page load refused, then let someone in".
#define WEB_HEAP_REFUSE_STREAK 8

// Elapsed milliseconds between two millis() readings, clamped below the "never"
// sentinel. Unsigned subtraction, so the 49.7-day wrap needs no special case;
// the clamp is what keeps a genuine age from ever colliding with the sentinel.
static inline uint32_t webHeapFailAgeMs(uint32_t nowMs, uint32_t stampMs) {
  const uint32_t age = nowMs - stampMs;
  return age < ALLOC_FAIL_WIFI_NEVER ? age : ALLOC_FAIL_WIFI_NEVER - 1;
}

// The refusal streak belongs to one episode of starvation. A refusal older than
// the window is from a previous one - the window is three seconds - so the
// count starts again rather than spending the escape hatch on the first blob of
// the next episode. Pure, and keyed on the time of the last refusal rather than
// on sampling "was it calm when someone last asked": a sampled flag cannot
// notice an episode nobody asked during, which is the case this is for.
static inline uint32_t webHeapStreakNow(uint32_t streak, uint32_t sinceLastRefuseMs) {
  return sinceLastRefuseMs >= WEB_HEAP_BACKOFF_MS ? 0 : streak;
}

// True while the portal should refuse large responses: the radio failed
// recently, and we have not already refused a whole page load's worth in a row.
// Pure, so the host test drives both halves without a panel.
static inline bool webHeapBackoffActive(uint32_t wifiFailAgeMs, uint32_t refusedInARow) {
  return wifiFailAgeMs < WEB_HEAP_BACKOFF_MS && refusedInARow < WEB_HEAP_REFUSE_STREAK;
}

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
// kilobytes each, seconds to send under load - are refused with 503 and the
// socket is dropped at once, which frees the queue. The small JSON endpoints are
// never refused: /api/info is how anyone finds out what is wrong, and a
// diagnostic that goes quiet exactly when it is needed is worse than useless.

#include <stdint.h>

// What allocFailWifiAgeMs() returns when the Wi-Fi task has never failed an
// allocation. Far larger than any backoff, so "never" always reads as calm.
#define ALLOC_FAIL_WIFI_NEVER 0x7FFFFFFFUL

// How long after the radio's last failed allocation the portal holds back its
// big blobs. **Our choice, not a measured figure**, in the manner of
// src/fx3d/fx3d_profile.h's retry count: the observed bursts ran about eleven
// seconds, and each further blob is what feeds them, so the window only has to
// outlast one browser's retry - it re-arms on every new failure, so a panel
// that keeps starving keeps refusing.
#define WEB_HEAP_BACKOFF_MS 3000UL

// True while the portal should refuse large responses. Pure, so the host test
// drives it without a panel.
static inline bool webHeapBackoffActive(uint32_t wifiFailAgeMs) {
  return wifiFailAgeMs < WEB_HEAP_BACKOFF_MS;
}

#pragma once
// A contiguous block of internal RAM, held from boot, so the radio has
// somewhere to land when it needs to come back.
//
// Why this exists, measured on the panel 2026-09-20 (the knowledge base,
// drafts/28-portal-hang-2026-09-20.md, part three). After a dozen concurrent
// portal requests drove internal free RAM to 3,960 B, the link dropped - and
// then the firmware tried to bring it back **every sixty seconds for seven
// minutes and failed every time**:
//
//   [mem] allocation failed: 3072 B, caps 0x804, task loopTask, before wifi reconnect
//   E (227684) ping_sock: esp_ping_new_session(233): create ping task failed
//
// Free internal heap at that moment was 14,744 B. The allocation was 3,072 B,
// internal, 8-bit, not DMA - and it still failed, because no contiguous 3 KB
// block existed. The heap was fragmented, not empty. **The recovery path itself
// starves, so the panel never comes back and only a reset clears it.** That is
// the owner's symptom, and refusing web traffic does not fix it: by the time the
// radio is failing, the contiguous space is already gone.
//
// So the panel takes the space early, while the heap is still whole, and gives
// it back at exactly the moment the radio starts failing - which is also the
// moment the web path stops sending its big responses (web_heap_backoff.h), so
// the hole that opens is far more likely to go to the reconnect than to a page.
//
// Sizing, from the two measured allocations that failed:
//   1,626 B - the Wi-Fi task's receive buffer, caps 0x80c (internal+8-bit+DMA)
//   3,072 B - the reconnect, caps 0x804 (internal+8-bit)
// 8 KB covers both with room for the pair to be live at once. It is taken from
// the DMA-capable pool because that is the stricter of the two: a DMA-capable
// block satisfies a plain internal request as well, and the buffer that failed
// most often needs DMA.

#include <stdint.h>

// **Zero, and why.** Taking this at boot carves it out of the one large
// contiguous region the heap has while it is still whole. Measured 2026-09-20:
// the largest free internal block after boot went from **25,588 B** without the
// reserve to **11,252 B** with it - and the rail board's direct fetch needs a
// 13 KB contiguous block for its task stack, so holding this reserve stopped
// both boards fetching on a panel where they had worked that morning.
//
// It was aimed at the symptom. The cause was fixed separately and better: the
// send path now offers lwIP one segment at a time instead of a whole 38 KB file
// (web.cpp, WEB_SEND_CHUNK), and after that a portal load moves the heap not at
// all. So the reserve is off, and the code stays because the measurement that
// justified it is real: if the radio starves again on some future build, this is
// the lever, and the number above says what it costs.
#define NET_RESERVE_BYTES 0

// How long the radio must have been quiet before the panel takes the space back.
// **Our choice, not a measured figure**: long enough that a burst of failures
// arriving in waves - the measured bursts ran about eleven seconds - does not
// see the reserve taken back between two waves of the same episode.
#define NET_RESERVE_REARM_MS 30000UL

// Should the reserve be held right now? It is given up while the radio's last
// failure is recent, and taken back once things have been quiet for the re-arm
// time. Pure, so the host test drives it without a panel.
//
// `wifiFailAgeMs` is ALLOC_FAIL_WIFI_NEVER when the radio has never failed, which
// is far larger than the re-arm time and so reads as calm - the reserve is held
// from boot, which is the whole point.
static inline bool netReserveWanted(uint32_t wifiFailAgeMs) {
  return wifiFailAgeMs >= NET_RESERVE_REARM_MS;
}

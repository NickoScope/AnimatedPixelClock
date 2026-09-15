#pragma once
// The page switches after a firmware update that adds page keys.
//
// NVS "pages" holds a bit per PanelPageKey, set = visited. A key appended
// later reads as 0 from a mask written before it existed, which would switch
// the new page off on the first boot after the update: the Lua pages (bit 6),
// the media page (bit 7) and the market pages (bit 8) all hit it. So panel.cpp
// stores "pgKnown" beside "pages": one u32, the keys the writing build knew in
// the high half and the "pages" value it wrote in the low half. A key that
// build did not know starts on.
//
// The low half is what catches a round trip through an older firmware: flash
// this one, then one without "pgKnown", then this one again. The older build
// rewrites "pages" (dropping the bits it does not know) and leaves "pgKnown"
// as it was, so the stored "pages" no longer equals the copy - and a mask not
// written against its "pgKnown" is read as a legacy one.
//
// Plain C++: tested on the host (tools/market/panel).
//
//   stored       NVS "pages"
//   haveStamp    NVS "pgKnown" exists
//   stamp        NVS "pgKnown": known << 16 | pages
//   all          every key this build knows, (1 << PANEL_KEY_COUNT) - 1
//   legacyKnown  what a mask without a matching stamp is taken to know
//   alwaysOn     bits that cannot be switched off (the clock)

#include <stdint.h>

static inline uint32_t panelPagesStamp(uint16_t all, uint16_t pages) { return ((uint32_t)all << 16) | pages; }

static inline uint16_t panelPagesKnown(uint16_t stored, bool haveStamp, uint32_t stamp, uint16_t legacyKnown) {
  return (haveStamp && (uint16_t)(stamp & 0xFFFFu) == stored) ? (uint16_t)(stamp >> 16) : legacyKnown;
}

static inline uint16_t panelPagesMigrate(uint16_t stored, bool haveStamp, uint32_t stamp, uint16_t all,
                                         uint16_t legacyKnown, uint16_t alwaysOn) {
  const uint16_t k = (uint16_t)(panelPagesKnown(stored, haveStamp, stamp, legacyKnown) & all);
  return (uint16_t)(((stored & k) | (all & ~k) | alwaysOn) & all);
}

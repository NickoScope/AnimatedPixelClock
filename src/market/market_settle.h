#pragma once
// Stock market dashboard: the knob's settings changes, settled.
//
// A knob detent that changes the window or the ticker must not write NVS or
// publish the retained config by itself: a turn through four tickers would
// otherwise be four flash writes and four recomputes in Home Assistant. Each
// change notes the time; once MARKET_NVS_SETTLE_MS pass without another, one
// save, and one publish if a change the app hears about (the ticker) was
// among them. A portal save writes and publishes at once and clears this.
//
// Plain C++: tools/market/panel/market_host_test.cpp drives it with a clock.
// The time and the flag are kept apart on purpose: `millis() | 1` as "dirty"
// makes `now - at` wrap to 4 294 967 295 when the check runs in the same
// millisecond as the change, and the settle is skipped.

#include <stddef.h>
#include <stdint.h>

namespace market {

// The config last published, by length and CRC-32 of its serialised form. A
// save that changes only panel-only rows (the window, the lines, the tape...)
// or a knob turn that comes back to the same ticker produces the same bytes,
// and the app ignores an identical config anyway: it is not sent again unless
// forced (the boot, a reconnect, {"republish":true}), since the broker may
// have lost the retained copy.
struct ConfigGate {
  bool     have;
  size_t   len;
  uint32_t crc;
  void clear() { have = false; len = 0; crc = 0; }
  bool differs(size_t n, uint32_t c) const { return !have || n != len || c != crc; }
  void sent(size_t n, uint32_t c) { have = true; len = n; crc = c; }
};

// A reconnect of the bus, by its connect counter (mqttBusConnects): true once
// for each connect not seen yet, including one made and finished inside a
// single mqttBusLoop() call, which the edge of mqttBusConnected() misses.
struct ConnectWatch {
  uint32_t seen;
  bool fresh(uint32_t connects) {
    if (connects == seen) return false;
    seen = connects;
    return true;
  }
};

enum : uint8_t { SETTLE_SAVE = 1, SETTLE_PUBLISH = 2 };

struct KnobSettle {
  uint32_t at;        // millis() of the last knob change
  bool     dirty;     // a change is waiting
  bool     publish;   // one of them is heard by the app

  void clear() { at = 0; dirty = false; publish = false; }
  void note(uint32_t ms, bool pub) {
    at = ms;
    dirty = true;
    publish = publish || pub;
  }
  bool pending() const { return dirty; }
  // SETTLE_SAVE, with SETTLE_PUBLISH when the app has to hear, once the knob
  // has been quiet for settleMs; 0 before that and after.
  uint8_t tick(uint32_t ms, uint32_t settleMs) {
    if (!dirty || (uint32_t)(ms - at) < settleMs) return 0;
    const uint8_t act = (uint8_t)(SETTLE_SAVE | (publish ? SETTLE_PUBLISH : 0));
    clear();
    return act;
  }
};

}  // namespace market

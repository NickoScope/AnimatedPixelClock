#pragma once
// One outbound HTTPS session at a time for the background fetch tasks.
//
// Measured on the panel 2026-09-14, after the direct rail board, the direct
// flight board and the world clock's IP lookup joined the weather fetch: each
// starts its own TLS session on its own 8-12 KB internal stack, and when they
// overlapped the internal heap bottomed out at 2 376 B, a TLS handshake failed
// with 17 KB free and Wi-Fi dropped for a while. tls_psram.cpp already moves
// mbedTLS's own allocations to PSRAM; task stacks, sockets and Wi-Fi buffers
// stay internal. So the fetchers take turns: a starter in the loop does not
// even create its task while another fetch holds the lock, and each task takes
// the lock before it connects and releases it after the connection is closed.
//
// The yacht radar's AIS websocket is not under the lock: it is one long-lived
// session that runs from loop() only while its page is on screen.

#include <stdint.h>

static const uint32_t NET_LOCK_WAIT_MS = 30000;

bool netLockBusy();                 // a fetch holds the lock right now
bool netLockTake(uint32_t waitMs);  // true once held
void netLockGive();

class NetLockGuard {
public:
  explicit NetLockGuard(uint32_t waitMs) : held_(netLockTake(waitMs)) {}
  ~NetLockGuard() { if (held_) netLockGive(); }
  bool held() const { return held_; }
  NetLockGuard(const NetLockGuard &) = delete;
  NetLockGuard &operator=(const NetLockGuard &) = delete;
private:
  bool held_;
};

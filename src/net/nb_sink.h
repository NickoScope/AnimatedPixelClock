#pragma once
// How many bytes of an incoming block fit in the mailbox - the whole of the
// bounds arithmetic, on its own, so the host test can drive every edge without
// an Arduino, a socket or a panel (tools/netbroker/check_nb.py).
//
// It lives apart from the broker because getting it wrong is silent: one byte
// of underflow here is a memcpy past the end of a PSRAM buffer, which on this
// chip does not fault, it corrupts whatever is next. The audit of 2026-09-21
// asked for exactly this and it was right to.

#include <stdint.h>

// `cap` is the buffer's size; one byte of it is reserved for the terminator,
// so the usable length is cap - 1. `len` is how much is already in it.
// Sets *truncated when anything had to be dropped, and never returns more than
// the room that genuinely exists.
static inline uint32_t nbSinkTake(uint32_t cap, uint32_t len, uint32_t size, bool *truncated) {
  // A one-byte buffer has room for the terminator and nothing else; a zero-byte
  // one does not even have that. Both are handled here rather than by the
  // caller, because `cap - 1` below would wrap for cap == 0 and the subtraction
  // after it would then wrap again - the exact shape of an overflow that gets
  // discovered months later.
  if (cap < 2) { *truncated = true; return 0; }
  // Safe because the only writer keeps len <= cap - 1: every path through this
  // function returns at most `room`, and len only ever grows by what it returns.
  const uint32_t room = (cap - 1) - len;
  const uint32_t n = size < room ? size : room;
  if (n < size) *truncated = true;
  return n;
}

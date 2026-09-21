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
// the room that genuinely exists - for ANY cap, len and size, including a len
// past the end. The function is total on purpose.
static inline uint32_t nbSinkTake(uint32_t cap, uint32_t len, uint32_t size, bool *truncated) {
  // A one-byte buffer has room for the terminator and nothing else; a zero-byte
  // one does not even have that. Both are handled here rather than by the
  // caller, because `cap - 1` below would wrap for cap == 0 and the subtraction
  // after it would then wrap again - the exact shape of an overflow that gets
  // discovered months later.
  // Two guards, and the second is the one that matters. `cap < 2` is a buffer
  // with no room for anything but the terminator. `len + 1 >= cap` is a caller
  // that has already filled it - and if that were allowed through, `(cap-1)-len`
  // would wrap to about four billion, `n` would become the whole incoming size,
  // and the memcpy would run off the end of a PSRAM buffer. Making the function
  // total here, rather than documenting a precondition, is the difference
  // between a test that can prove something and a comment that hopes.
  //
  // `size == 0` is not truncation: nothing was dropped, so the flag stays as it
  // was. A caller passing an empty block should not be told its answer was cut.
  if (cap < 2 || len + 1 >= cap) { if (size) *truncated = true; return 0; }
  const uint32_t room = (cap - 1) - len;
  const uint32_t n = size < room ? size : room;
  if (n < size) *truncated = true;
  return n;
}

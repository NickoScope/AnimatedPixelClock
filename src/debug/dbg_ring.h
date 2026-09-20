#pragma once
// The remote log's ring, as a plain model: no Arduino, no PSRAM, no web server,
// so the host test drives every edge without a panel (tools/dbglog/).
//
// Why a ring and a cursor rather than "give me the log": the panel is asked over
// HTTP, the asking costs internal RAM, and the interesting lines arrive in
// bursts (2026-09-20: twenty-five allocation failures in eleven seconds). A
// reader polls with the cursor it last saw and gets only what is new, so a quiet
// panel answers a handful of bytes.
//
// `seq` counts every byte ever written and never resets while the log is on. It
// is what a reader passes back. When a reader has fallen behind further than the
// buffer holds, it is told the oldest byte still kept and reads from there - the
// gap is reported rather than hidden, because a debug log that silently skips is
// worse than one that admits it skipped.

#include <stddef.h>
#include <stdint.h>

struct DbgRing {
  char *buf;        // caller's storage; null means the log is off
  uint32_t cap;     // bytes of it
  uint32_t seq;     // total bytes ever written
  uint32_t kept;    // bytes currently in the ring (<= cap)
  uint32_t dropped; // bytes thrown away to make room, since the log was turned on
};

static inline void dbgRingInit(DbgRing *r, char *buf, uint32_t cap) {
  r->buf = buf;
  r->cap = buf ? cap : 0;
  r->seq = 0;
  r->kept = 0;
  r->dropped = 0;
}

// The sequence number of the oldest byte still held.
static inline uint32_t dbgRingOldest(const DbgRing *r) { return r->seq - r->kept; }

// Append n bytes. When they do not fit, the oldest go - a debug log is worth
// more at its newest end. Writing more than the whole ring keeps the tail of it.
static inline void dbgRingWrite(DbgRing *r, const char *p, uint32_t n) {
  // A log that is off does nothing at all - not even count. There is no reader
  // to keep honest, and turning it on starts the sequence from zero anyway.
  if (!r->buf || !r->cap || !n) return;
  if (n >= r->cap) {           // only the last cap bytes can survive
    p += n - r->cap;
    r->dropped += n - r->cap + r->kept;
    n = r->cap;
    r->kept = 0;
  }
  if (r->kept + n > r->cap) {
    r->dropped += r->kept + n - r->cap;
    r->kept = r->cap - n;
  }
  const uint32_t head = r->seq % r->cap;   // the ring is indexed by the absolute sequence
  const uint32_t first = (r->cap - head) < n ? (r->cap - head) : n;
  for (uint32_t i = 0; i < first; i++) r->buf[head + i] = p[i];
  for (uint32_t i = first; i < n; i++) r->buf[i - first] = p[i];
  r->seq += n;
  r->kept += n;
}

// Copy up to `max` bytes that follow `since` into `out`. Returns how many, and
// sets `*from` to the sequence number the first copied byte actually has - equal
// to `since` when nothing was missed, larger when the reader fell behind.
static inline uint32_t dbgRingRead(const DbgRing *r, uint32_t since, char *out, uint32_t max, uint32_t *from) {
  const uint32_t oldest = dbgRingOldest(r);
  uint32_t start = since;
  // Unsigned compare against the window, so a cursor from before a restart of
  // the log (seq back to 0) reads as "behind" and is pulled to the oldest byte.
  if ((uint32_t)(start - oldest) > r->kept) start = oldest;
  *from = start;
  uint32_t n = r->seq - start;
  if (n > max) n = max;
  if (!r->buf || !r->cap) return 0;
  for (uint32_t i = 0; i < n; i++) out[i] = r->buf[(start + i) % r->cap];
  return n;
}

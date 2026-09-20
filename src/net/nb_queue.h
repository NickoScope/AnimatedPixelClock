#pragma once
// The broker's queue, as a plain model: no Arduino, no sockets, no TLS, so the
// host test drives every rule without a panel (tools/netbroker/check_nb.py).
// The task that does the fetching is src/net/net_broker.cpp; this file is only
// about whose turn it is.
//
// Why a queue at all: docs/32-net-broker.md in the knowledge base. Four modules
// each start their own 9-12 KB task with its own TLS session on a board with
// 34 KB of internal RAM, and the memory they need is not gone so much as broken
// into pieces too small to use - measured 2026-09-20, the rail board refusing
// to fetch because it was 524 bytes short of a contiguous 13 KB with 30,828 B
// free. One owner, one stack taken at boot, nothing allocated per fetch.
//
// Fixed slots, no allocation. The queue is a static array in the broker; this
// header only says how the slots are chosen.

#include <stddef.h>
#include <stdint.h>

// One slot per consumer, so a module can never be crowded out by another's
// backlog, and a second request from the same module replaces its own pending
// one rather than queueing behind it. A refresh that has been overtaken by a
// newer refresh of the same thing is worthless.
enum NbCaller : uint8_t { NB_WEATHER, NB_WORLDCLOCK, NB_RAIL, NB_FLIGHT, NB_CALLER_COUNT };

// What a slot is doing.
enum NbSlotState : uint8_t {
  NB_EMPTY = 0,   // nothing here
  NB_READY,       // waiting for its turn
  NB_ONAIR,       // the broker is fetching this one
};

// Interactive means a person is waiting: the owner changed the station, or
// picked a city. It goes ahead of scheduled refreshes, and it is the only
// reason the queue is not plain first-come.
struct NbSlot {
  uint8_t  state;        // NbSlotState
  bool     interactive;
  uint32_t queuedMs;     // when it was put in
};

struct NbQueue {
  NbSlot slot[NB_CALLER_COUNT];
  uint8_t onAir;         // NB_CALLER_COUNT when nothing is on the wire
};

static inline void nbInit(NbQueue *q) {
  for (uint8_t i = 0; i < NB_CALLER_COUNT; i++) { q->slot[i].state = NB_EMPTY; q->slot[i].interactive = false; q->slot[i].queuedMs = 0; }
  q->onAir = NB_CALLER_COUNT;
}

static inline bool nbBusy(const NbQueue *q) { return q->onAir < NB_CALLER_COUNT; }

static inline uint8_t nbWaiting(const NbQueue *q) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NB_CALLER_COUNT; i++) if (q->slot[i].state == NB_READY) n++;
  return n;
}

// Put a request in, or replace this caller's own pending one. Returns false
// only when this caller's request is already on the wire - then the caller
// tries again when it ends, rather than having two of its own in flight.
// An interactive request replaces a scheduled one and keeps its interactive
// flag; a scheduled one never clears an interactive flag already set, because
// the person is still waiting for an answer about the thing they changed.
static inline bool nbSubmit(NbQueue *q, uint8_t who, bool interactive, uint32_t nowMs) {
  if (who >= NB_CALLER_COUNT) return false;
  if (q->slot[who].state == NB_ONAIR) return false;
  const bool wasInteractive = q->slot[who].state == NB_READY && q->slot[who].interactive;
  q->slot[who].state = NB_READY;
  q->slot[who].interactive = interactive || wasInteractive;
  q->slot[who].queuedMs = nowMs;
  return true;
}

// Whose turn it is, or NB_CALLER_COUNT when nobody's. Interactive first, then
// the one that has waited longest; the caller order in the enum breaks a tie,
// so the choice is deterministic and the host test can pin it.
static inline uint8_t nbPick(const NbQueue *q, uint32_t nowMs) {
  if (nbBusy(q)) return NB_CALLER_COUNT;
  uint8_t best = NB_CALLER_COUNT;
  bool bestInteractive = false;
  uint32_t bestWaited = 0;
  for (uint8_t i = 0; i < NB_CALLER_COUNT; i++) {
    if (q->slot[i].state != NB_READY) continue;
    const uint32_t waited = nowMs - q->slot[i].queuedMs;   // unsigned: the millis() wrap is fine
    const bool inter = q->slot[i].interactive;
    if (best == NB_CALLER_COUNT || (inter && !bestInteractive) ||
        (inter == bestInteractive && waited > bestWaited)) {
      best = i; bestInteractive = inter; bestWaited = waited;
    }
  }
  return best;
}

// Hand the picked slot to the broker.
static inline void nbStart(NbQueue *q, uint8_t who) {
  if (who >= NB_CALLER_COUNT || q->slot[who].state != NB_READY) return;
  q->slot[who].state = NB_ONAIR;
  q->onAir = who;
}

// The fetch ended, however it ended. The slot is free; whether the caller asks
// again is the caller's own business, with its own back-off.
static inline void nbFinish(NbQueue *q) {
  if (!nbBusy(q)) return;
  q->slot[q->onAir].state = NB_EMPTY;
  q->slot[q->onAir].interactive = false;
  q->onAir = NB_CALLER_COUNT;
}

// How long the slot on the wire has been there. The broker enforces the
// deadline itself rather than trusting a caller to: one stalled host must not
// hold the other three.
static inline uint32_t nbOnAirMs(const NbQueue *q, uint32_t nowMs) {
  return nbBusy(q) ? (uint32_t)(nowMs - q->slot[q->onAir].queuedMs) : 0;
}

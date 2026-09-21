#pragma once
// One task owns the outbound socket. Everybody else asks it, and collects the
// answer from its own mailbox on the loop task.
//
// The problem, measured on the panel 2026-09-20 and written up in the knowledge
// base as docs/32-net-broker.md: four modules each start their own fetch task
// with its own TLS session, on a board with about 34 KB of internal RAM.
//
//   weather       8 KB stack        world clock   8 KB stack
//   rail board    9 KB stack        flight board 12 KB stack
//
// Each is transient, so nothing shows as leaked - and each needs its stack as
// one **contiguous** internal block at whatever moment it happens to want it.
// That is the failure we kept hitting: the rail board refusing to fetch while
// 30,828 B were free, because it was 524 B short of a contiguous 13 KB.
//
// **The shape is NetGate's**, from NickoScope32 v1B Main-S3 v33.64.0 (ADD-62),
// which solved the same problem on the same family of chip and has been in
// production for months. What we took and what we did not is in
// docs/32-net-broker.md. The three load-bearing pieces:
//
//   1. One permanent task, its stack taken at boot, no transient tasks at all.
//   2. The TLS client is built per request and destroyed BEFORE the answer is
//      published, so the memory is back before the consumer can act on it.
//   3. **The body is copied into the caller's PSRAM mailbox and parsed later,
//      on the loop task.** The broker never runs consumer code. That is what
//      makes its stack a bounded, knowable quantity rather than a hostage to
//      whatever the deepest consumer's parser happens to need.
//
// Where we differ from NetGate, deliberately: its queue copies a ~1 KB job
// struct by value into a FreeRTOS queue, twelve slots, about 12 KB of internal
// RAM - its own ADD-62 admits that blew the budget roughly fivefold. Here the
// URL and the credential live in PSRAM and the queue is four fixed slots, so
// the queue costs almost no internal RAM at all.

#include <atomic>
#include <stdint.h>

#include "nb_queue.h"

// The most a caller's URL and credential may be. Both live in PSRAM (see
// nbBegin), so the sizes cost no internal RAM.
#define NB_URL_MAX     384
#define NB_AUTH_MAX    128
#define NB_HEADER_MAX   48
// How many response headers one request may keep. Five, because the rail board
// needs exactly that many: Retry-After plus four rate-limit counters, and
// dropping any of them would mean guessing at the budget it is given.
#define NB_COLLECT_MAX   5

// Failures that happen before, or instead of, an HTTP status. Outside
// HTTPClient's own range, which runs -1 to -11.
#define NB_ERR_NO_TURN  (-101)   // never got the network's turn in time
#define NB_ERR_BAD_URL  (-102)   // the URL was refused before a connection
#define NB_ERR_TRUNC    (-103)   // the body did not fit the mailbox; it is cut
#define NB_ERR_TIMEOUT  (-104)   // the transfer did not finish within the deadline
// The server declared more bytes than it sent. **Only detectable when it
// declared a length at all**: without Content-Length the end of the body IS
// the close of the connection, so a connection cut short is indistinguishable
// from a normal ending. That is HTTP, not a gap here.
#define NB_ERR_SHORT    (-105)

// Where a caller's answer lands.
//
// **Single producer, single consumer, no lock.** Only the broker task writes
// it; only the loop task reads it. The broker fills every field and THEN
// increments `seq` - that increment is the release point, and a consumer that
// sees a new `seq` is guaranteed to see the payload that belongs to it. Read
// `seq` ONCE into a local, act, and do not read it again until you have
// finished with the body.
//
// **And do not submit again until you have finished with the body.** That is
// the real invariant, and it is stronger than "do not re-read seq": the broker
// writes into this same buffer as soon as it takes this caller's next request,
// so a consumer that parses across several passes of loop(), or that submits
// from a different task than the one that reads, will have the bytes rewritten
// underneath it. One slot per caller makes the broker unable to start a second
// request without a new submit, so the rule is enforceable by the caller alone
// - but only if the caller keeps it.
// This is NetGate's `ng_mailbox_t` (netgate.h:87-97) with wider length fields,
// because two of our consumers deal in bodies far larger than its 64 KB.
struct NbMailbox {
  // Atomic rather than volatile, and that is the difference between meaning it
  // and getting away with it. A pair of fences around a volatile only
  // synchronises-with by accident of the compiler; a release store and an
  // acquire load on an atomic say it. On this target it is lock-free and the
  // same width, so it costs nothing. Store it with
  // seq.store(v, std::memory_order_release) and read it with
  // seq.load(std::memory_order_acquire) - nothing else.
  std::atomic<uint32_t> seq;     // ++ on every completion, success or not
  volatile int32_t  code;        // HTTP status, or one of the negatives above
  volatile uint32_t tag;         // whatever the caller put in NbRequest::tag
  volatile uint32_t durationMs;
  volatile uint32_t bodyLen;
  uint32_t          bodyCap;     // the buffer's size; USABLE length is one less,
                                 // because a byte is kept for the terminator
  char             *body;        // PSRAM, always NUL-terminated
  // The collected headers, in the order they were asked for; each "" if the
  // server did not send it. Index with the same position used in
  // NbRequest::collect.
  char              header[NB_COLLECT_MAX][NB_HEADER_MAX];
  uint8_t           headers;     // how many were asked for
};

struct NbRequest {
  const char *url;        // copied; NB_URL_MAX including the terminator
  const char *auth;       // optional credential value; copied. NOT logged,
                          // NOT echoed, and zeroed when the request is done.
  // Which header `auth` goes in. nullptr means "Authorization", which is what
  // the rail board's bearer token uses; the flight board's AeroAPI key goes in
  // "x-apikey" instead. Not copied - a literal. The broker will not invent a
  // header name, because sending a credential under the wrong one either fails
  // the call or, worse, leaks it to a server with no business seeing it.
  const char *authHeader;
  // The PEM roots to verify the server against. **NOT copied** - a static or
  // PROGMEM string that outlives the request. nullptr means setInsecure(),
  // which is what the open public endpoints use.
  const char *caCert;
  // Response headers to keep, by name, e.g. {"Retry-After", "X-RateLimit-Remaining-Day"}.
  // Not copied - literals. They arrive in NbMailbox::header at the same index.
  // nullptr entries and anything past `collectCount` are ignored.
  const char *collect[NB_COLLECT_MAX];
  uint8_t     collectCount;
  uint32_t    tag;        // handed back untouched; use it to tell one of your
                          // own requests from another (which city, which page)
  // 0 takes the broker's default. **This is a deadline on the whole transfer,
  // not on one step of it** - it changed meaning when the broker started
  // reading the body itself. For weather's 746 B the distinction is academic;
  // for the rail board, whose own buffer is 1.5 MB, it is not.
  uint32_t    timeoutMs;
};

// Create the task. Called once from setup(), before the heap has been used by
// anything transient.
//
// **A caller must not assume this succeeded.** Ask nbUp() and keep your own
// fetch path for when it is false - a module whose only path is the broker
// goes silent until a reboot if the broker fails to start, which is a worse
// failure than the one the broker exists to fix. Weather shows the shape.
bool nbBegin();

// Is the broker running at all?
bool nbUp();

// Has this caller been migrated - that is, does it have a mailbox? A consumer
// whose body size has not been measured yet has no mailbox and is refused, so
// the migration state of each module is one table in net_broker.cpp rather
// than something to remember.
bool nbReady(uint8_t who);

// Ask for a fetch. `interactive` means a person is waiting on it - the owner
// changed the station, picked a city - and it goes ahead of scheduled work.
// False when this caller already has one queued or on the wire, when it has no
// mailbox, or when the broker is not up. **False is "wait", not "it broke"**:
// the caller tries again on its own schedule.
bool nbSubmitRequest(uint8_t who, const NbRequest &req, bool interactive);

// Is this caller's request still queued or on the wire?
bool nbPending(uint8_t who);

// This caller's mailbox, or nullptr if it has none. The pointer is stable for
// the life of the firmware; the contents are not.
NbMailbox *nbMailbox(uint8_t who);

// For the diagnostics page. `onAir` is NB_CALLER_COUNT when the wire is idle.
struct NbStats {
  uint8_t  onAir;
  uint8_t  waiting;
  uint32_t onAirMs;       // how long the current one has been going
  uint32_t served;        // fetches finished since boot, however they ended
  uint32_t failed;
  uint32_t stackFreeMin;  // the broker stack's high-water mark, bytes still free
};
void nbGetStats(NbStats *out);

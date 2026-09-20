#pragma once
// One task owns the outbound socket. Everybody else asks it.
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
// 30,828 B were free, because it was 524 B short of a contiguous 13 KB. Adding
// a browser on the portal to that mix is what takes the panel off the network.
//
// So: one task, created at boot while the heap is still whole, with its stack
// in .bss - not on the heap at all, so it can neither fail to be allocated nor
// leave a hole when it is freed. Four callers, one slot each, one at a time on
// the wire. The peak internal demand of a fetch drops from 12-16 KB needing
// contiguity to the TLS session's own couple of kilobytes.
//
// What this does NOT change: the request bodies and the JSON still cost what
// they cost. They are in PSRAM already.

#include <stdint.h>

#include "nb_queue.h"

class Stream;

// The most a caller's URL and authorization header may be. Weather builds a
// 320-byte URL today, which is the longest of the four; the header carries a
// bearer token for the rail board. Both live in PSRAM (see nbBegin), so the
// sizes cost no internal RAM and there is no reason to be mean with them.
#define NB_URL_MAX   384
#define NB_AUTH_MAX  128

// Two failures that happen before HTTP is reached at all, so they cannot
// collide with HTTPClient's own error codes (which run -1 to -11).
#define NB_ERR_NO_TURN  (-101)   // never got the network's turn in time
#define NB_ERR_BAD_URL  (-102)   // the URL was refused before a connection

// What the broker hands to the caller's parse function.
//
// **`body` is live.** It is the open socket, valid only while the call is on
// the stack; the caller reads what it needs - typically straight into a PSRAM
// JsonDocument, exactly as it does today - and does not keep the pointer.
struct NbReply {
  int     code;    // HTTP status, or a negative error: HTTPClient's, or the two above
  Stream *body;    // the body, including an error page; nullptr whenever code < 0
  bool    tls;     // the failure was in the handshake, not in HTTP
  void   *ctx;     // whatever the caller passed in
};

// **Runs on the broker task, not on loop().** It has to: the body is only on
// the wire during the call. So it may touch only what a fetch task touches
// today - its own module's published data, under that module's own lock. The
// loop side learns the request is over through nbTake().
//
// **The contract, and it is kept literally.** It is called EXACTLY ONCE for
// every accepted request, whatever the outcome - including the two failures
// above, where `code` is negative and `body` is nullptr. So a caller has one
// place to handle everything, and can never be left without an answer. What it
// returns IS the outcome nbTake() reports: return false on a code you do not
// want treated as a success. Check `code` before touching `body`.
typedef bool (*NbParseFn)(const NbReply &reply);

struct NbRequest {
  const char *url;        // copied; NB_URL_MAX including the terminator
  const char *auth;       // optional Authorization value; copied. NOT logged,
                          // NOT echoed, and zeroed when the slot is finished.
  uint32_t    timeoutMs;  // 0 takes the broker's default
  NbParseFn   parse;
  void       *ctx;
};

// Create the task. Called once from setup(), before the heap has been used by
// anything transient. Returns false if the PSRAM request storage or the TLS
// client could not be had.
//
// **A caller must not assume this succeeded.** Ask nbUp() and keep your own
// fetch path for when it is false - a module whose only path is the broker
// goes silent until a reboot if the broker fails to start, which is a worse
// failure than the one the broker exists to fix. Weather shows the shape.
bool nbBegin();

// Is the broker actually running? The one thing a caller must branch on.
bool nbUp();

// Ask for a fetch. `interactive` means a person is waiting on it - the owner
// changed the station, picked a city - and it goes ahead of scheduled work.
// False when this caller already has one on the wire, or the broker is not up.
bool nbSubmitRequest(uint8_t who, const NbRequest &req, bool interactive);

// Is this caller's request still queued or on the wire?
bool nbPending(uint8_t who);

// Collect the outcome, once. Returns false while there is nothing to collect.
// `ok` is what the parse function returned, or false if the fetch never got
// far enough to call it. Called from loop(): this is where a caller sets its
// next refresh time and clears its own busy flag.
bool nbTake(uint8_t who, bool *ok);

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

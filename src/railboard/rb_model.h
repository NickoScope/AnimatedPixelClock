#pragma once
// The rail board's data model: one list of services, as the page draws it.
//
// Shared by the MQTT ingest and the render (railboard.cpp) and by the direct
// Realtime Trains fetch (rtt_direct.cpp, rtt_transform.cpp), so a board the
// panel fetched itself and a board Home Assistant published are the same
// thing. Plain C++ with no Arduino dependency: tools/railboard/check_direct.py
// compiles the transform against it on the host.

#include <cstdint>

#define RB_MAX_SVC  8     // services per list, as the package sends them
#define RB_PLAT_LEN 4     // "10A"
#define RB_OP_LEN   4     // "SW", "BUS"
#define RB_NAME_LEN 25    // names are cut to 24 characters on a word boundary
#define RB_STN_LEN  32    // RTT's description, e.g. London Road (Guildford)
#define RB_RT_LEN   24    // RTT systemStatus.realtimeNetworkRail

// Status is the package's closed vocabulary, in this order on both ends.
enum RbStatus : uint8_t { RB_OK = 0, RB_LATE, RB_CANC, RB_NOREPORT, RB_ARRIVED, RB_STATUS_COUNT };

enum : uint8_t { RB_DEP = 0, RB_ARR = 1 };

struct RbService {
  uint32_t t;             // scheduled (advertised), UTC epoch seconds
  uint32_t x;             // expected, or actual once reported; 0 = none
  uint8_t  st;            // RbStatus
  uint8_t  d;             // minutes late
  char     p[RB_PLAT_LEN];
  char     o[RB_OP_LEN];
  char     n[RB_NAME_LEN];
};

struct RbBoard {
  RbService s[RB_MAX_SVC];
  uint8_t   count;
  bool      have;
  uint32_t  ts;           // when the answer was fetched, UTC epoch seconds
  uint32_t  rxMs;         // millis() when it reached the page
  char      stn[RB_STN_LEN];
  char      rt[RB_RT_LEN];
};

#pragma once
// Flight board times in an airport's own local time, summer time included.
//
// AeroAPI sends every time in UTC. The board shows them as a station board
// does: in the time of the airport it is the board of, and the tracked flight's
// departure in its origin's time and its arrival in its destination's, as
// airline apps show them. The zone arithmetic is the world clock's
// (src/worldclock/posix_tz.h, the tzdata 2026c table in tzdb.h), not a
// second copy: an IANA name is looked up once and its POSIX string evaluated
// for each instant.
//
// Plain C++: tools/flightboard/aero_host_test.cpp checks it across summer time
// changes against Python's zoneinfo.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../worldclock/posix_tz.h"

namespace fbz {

struct Zone {
  PosixTz tz;
  bool    known;       // false: no name, or a name the table does not have
};

// The zone for an IANA name; known is false when there is none.
inline bool load(const char *iana, Zone *z) {
  const char *posix = (iana && *iana) ? tzdbPosix(iana) : nullptr;
  z->known = posix && posixTzParse(posix, &z->tz);
  return z->known;
}

// Seconds east of UTC at that instant; 0 (UTC) for an unknown zone.
inline int32_t offset(const Zone &z, int64_t utc) { return z.known ? posixTzOffset(z.tz, utc) : 0; }

// "HH:MM" at utc + off; "--:--" for no time.
inline void hmAt(int64_t utc, int32_t off, char *out, size_t cap) {
  if (utc <= 0) { snprintf(out, cap, "--:--"); return; }
  int64_t s = (utc + off) % 86400;
  if (s < 0) s += 86400;
  snprintf(out, cap, "%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
}

inline void hm(int64_t utc, const Zone &z, char *out, size_t cap) { hmAt(utc, offset(z, utc), out, cap); }

// The offset the panel's own clock is running at, from its broken-down local
// time (localtime_r / getLocalTime) and the same instant in UTC.
inline int32_t offsetOfLocal(int year, unsigned mon1, unsigned mday, int hour, int min, int sec, int64_t utc) {
  const int64_t local = posixDaysFromCivil(year, mon1, mday) * 86400 + hour * 3600 + min * 60 + sec;
  return (int32_t)(local - utc);
}

// The header's cue: how far the airport's clock is from the panel's, "-1",
// "+6", "+5:30", "-9:45"; "" when they agree.
inline void cue(int32_t aptOff, int32_t panelOff, char *out, size_t cap) {
  const int32_t d = aptOff - panelOff;
  if (d == 0) { if (cap) out[0] = '\0'; return; }
  const int32_t a = d < 0 ? -d : d;
  const int h = (int)(a / 3600), m = (int)(a % 3600 / 60);
  if (m) snprintf(out, cap, "%c%d:%02d", d < 0 ? '-' : '+', h, m);
  else   snprintf(out, cap, "%c%d", d < 0 ? '-' : '+', h);
}

// Where a tracked flight's end gets its zone, in this order:
enum Source : uint8_t {
  SRC_SENT = 0,   // the timezone AeroAPI sent with the airport, when the table has it
  SRC_LIST,       // else the zone of an airport in the panel's own list with that code
  SRC_UTC         // else UTC, and the time is drawn dim: a guessed zone could be hours out unseen
};

// listZone: the IANA name of a listed airport whose ICAO or IATA code is
// `code`, or nullptr.
inline Source pick(const char *sent, const char *code, const char *(*listZone)(const char *code), Zone *out) {
  if (load(sent, out)) return SRC_SENT;
  const char *listed = (listZone && code && *code) ? listZone(code) : nullptr;
  if (load(listed, out)) return SRC_LIST;
  out->known = false;
  return SRC_UTC;
}

inline const char *sourceName(Source s) { return s == SRC_SENT ? "sent" : s == SRC_LIST ? "list" : "utc"; }

}  // namespace fbz

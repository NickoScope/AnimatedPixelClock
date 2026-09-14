#pragma once
// POSIX TZ strings, evaluated for any instant without touching the process TZ.
//
// The world clock shows the home city's time, which is usually not the panel's
// own zone. setenv("TZ") and tzset() would switch the zone for every task at
// once - the clock page, the weather task and the web server all call
// localtime_r - so the page parses the city's string once and does the
// arithmetic itself.
//
// Grammar: std offset [dst [offset] ,start[/time],end[/time]] as POSIX
// (XBD 8.3) and tzset(3) give it, including <+03>-style quoted designations,
// plus the extension RFC 9636 (3.3.2) allows and the IANA footers use: rule
// times from -167 to 167 hours ("M3.5.0/-1" in America/Nuuk, "M3.4.4/26" in
// Asia/Jerusalem).
// A dst without a rule is refused: POSIX leaves that rule to the
// implementation, and no string in the table needs one.
//
// Host-portable: compiled into the firmware and into tools/luasim/wchost,
// which fx_parity.py checks against Python's zoneinfo for every string in
// tzdb.h.

#include <stdint.h>

struct PosixTzRule {
  char     kind;          // 'M' month.week.day, 'J' 1..365 never counting Feb 29, 'D' 0..365
  uint8_t  month, week, wday;
  uint16_t day;
  int32_t  secs;          // local time of day of the change; may be below 0 or past 24 h
};

struct PosixTz {
  int32_t     stdOff;     // seconds EAST of UTC; the string writes them west
  int32_t     dstOff;
  bool        hasDst;
  PosixTzRule start, end;
};

// False: not a string this accepts, and *out is left alone.
bool    posixTzParse(const char *s, PosixTz *out);
// Seconds east of UTC in force at the UTC instant utc.
int32_t posixTzOffset(const PosixTz &tz, int64_t utc);

// The zone table, src/worldclock/tzdb.h, written by tools/luasim/gen_tz.py.
const char *tzdbPosix(const char *iana);    // nullptr: not a zone in the table
const char *tzdbVersion();
uint16_t    tzdbCount();

// Days since 1970-01-01 and back: Howard Hinnant's algorithms, which he
// donated to the public domain (howardhinnant.github.io/date_algorithms.html).
int64_t posixDaysFromCivil(int64_t y, unsigned m, unsigned d);
void    posixCivilFromDays(int64_t days, int64_t *y, unsigned *m, unsigned *d);

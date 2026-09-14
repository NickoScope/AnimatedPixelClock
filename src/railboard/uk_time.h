#pragma once
// UK civil time from UTC, by the rule in force since 2002.
//
// The Summer Time Order 2002 (SI 2002/262), article 2(2): summer time is "the
// period beginning at one o'clock, Greenwich mean time, in the morning of the
// last Sunday in March and ending at one o'clock, Greenwich mean time, in the
// morning of the last Sunday in October". Outside it the UK keeps GMT.
//   https://www.legislation.gov.uk/uksi/2002/262/article/2/made
//
// Worked out here from UTC instead of through the device's TZ setting: that
// setting belongs to the clock pages and says where the panel hangs, not where
// the trains run.
//
// Plain C++ with no Arduino dependency, so tools/railboard/check_uk_time.py can
// compile it on the host and compare it hour by hour with the tz database.

#include <cstdint>

namespace uktime {

// Calendar <-> day number, days since 1970-01-01, proleptic Gregorian.
// Howard Hinnant's algorithms: http://howardhinnant.github.io/date_algorithms.html
inline int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t  era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void civilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
}

// 0 = Sunday. 1970-01-01 was a Thursday.
inline unsigned weekdayFromDays(int64_t z) {
  return static_cast<unsigned>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

inline int64_t floorDiv(int64_t a, int64_t b) {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Day number of the last Sunday of a 31-day month (March and October both are).
inline int64_t lastSunday31(int64_t y, unsigned m) {
  const int64_t d31 = daysFromCivil(y, m, 31);
  return d31 - weekdayFromDays(d31);
}

struct Civil {
  int32_t year;
  uint8_t month;    // 1..12
  uint8_t day;      // 1..31
  uint8_t hour, minute, second;
  uint8_t wday;     // 0 = Sunday
  bool    bst;      // true while British Summer Time is in force
};

// Both changes happen at 01:00 GMT, so the year can be taken from UTC: neither
// is anywhere near a new year.
inline bool isBst(int64_t utc) {
  int64_t y; unsigned m, d;
  civilFromDays(floorDiv(utc, 86400), y, m, d);
  const int64_t start = lastSunday31(y, 3)  * 86400 + 3600;
  const int64_t end   = lastSunday31(y, 10) * 86400 + 3600;
  return utc >= start && utc < end;
}

inline Civil london(int64_t utc) {
  Civil c{};
  c.bst = isBst(utc);
  const int64_t local = utc + (c.bst ? 3600 : 0);
  const int64_t days  = floorDiv(local, 86400);
  const int64_t secs  = local - days * 86400;
  int64_t y; unsigned m, d;
  civilFromDays(days, y, m, d);
  c.year   = static_cast<int32_t>(y);
  c.month  = static_cast<uint8_t>(m);
  c.day    = static_cast<uint8_t>(d);
  c.hour   = static_cast<uint8_t>(secs / 3600);
  c.minute = static_cast<uint8_t>((secs / 60) % 60);
  c.second = static_cast<uint8_t>(secs % 60);
  c.wday   = static_cast<uint8_t>(weekdayFromDays(days));
  return c;
}

}  // namespace uktime

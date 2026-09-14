#include "posix_tz.h"

// Only the world clock looks zones up; other builds carry none of this.
#if defined(WORLDCLOCK_ENABLED)

#include <string.h>

#include "tzdb.h"

// ---------------------------------------------------------------- parsing
static bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A bare designation is letters only; a quoted one may add digits, + and -.
// Three or more characters either way (tzset(3)).
static const char *parseName(const char *p) {
  if (*p == '<') {
    const char *b = ++p;
    while (isAlpha(*p) || isDigit(*p) || *p == '+' || *p == '-') p++;
    if (*p != '>' || p - b < 3) return nullptr;
    return p + 1;
  }
  const char *b = p;
  while (isAlpha(*p)) p++;
  return p - b >= 3 ? p : nullptr;
}

// [+|-]h[h[h]][:mm[:ss]] with the hours at most maxH: 24 for an offset
// (tzset(3)), 167 for a rule time (RFC 9636).
static const char *parseTime(const char *p, int32_t maxH, int32_t *out) {
  int32_t sign = 1;
  if (*p == '+' || *p == '-') {
    if (*p == '-') sign = -1;
    p++;
  }
  if (!isDigit(*p)) return nullptr;
  int32_t h = 0;
  for (int n = 0; isDigit(*p); n++) {
    if (n == 3) return nullptr;
    h = h * 10 + (*p++ - '0');
  }
  if (h > maxH) return nullptr;
  int32_t v = h * 3600;
  for (int part = 0; part < 2 && *p == ':'; part++) {
    if (!isDigit(p[1]) || !isDigit(p[2])) return nullptr;
    const int32_t x = (p[1] - '0') * 10 + (p[2] - '0');
    if (x > 59) return nullptr;
    v += part ? x : x * 60;
    p += 3;
  }
  *out = sign * v;
  return p;
}

static const char *parseNum(const char *p, int lo, int hi, int *out) {
  if (!isDigit(*p)) return nullptr;
  int v = 0;
  for (int n = 0; isDigit(*p); n++) {
    if (n == 3) return nullptr;
    v = v * 10 + (*p++ - '0');
  }
  if (v < lo || v > hi) return nullptr;
  *out = v;
  return p;
}

static const char *parseRule(const char *p, PosixTzRule *r) {
  int a, b, c;
  memset(r, 0, sizeof(*r));
  if (*p == 'M') {
    if (!(p = parseNum(p + 1, 1, 12, &a)) || *p != '.') return nullptr;
    if (!(p = parseNum(p + 1, 1, 5, &b)) || *p != '.') return nullptr;
    if (!(p = parseNum(p + 1, 0, 6, &c))) return nullptr;
    r->kind = 'M';
    r->month = (uint8_t)a;
    r->week  = (uint8_t)b;
    r->wday  = (uint8_t)c;
  } else if (*p == 'J') {
    if (!(p = parseNum(p + 1, 1, 365, &a))) return nullptr;
    r->kind = 'J';
    r->day  = (uint16_t)a;
  } else {
    if (!(p = parseNum(p, 0, 365, &a))) return nullptr;
    r->kind = 'D';
    r->day  = (uint16_t)a;
  }
  r->secs = 2 * 3600;                        // 02:00:00 when the rule gives no time
  if (*p == '/') p = parseTime(p + 1, 167, &r->secs);
  return p;
}

bool posixTzParse(const char *s, PosixTz *out) {
  if (!s) return false;
  PosixTz tz;
  memset(&tz, 0, sizeof(tz));
  int32_t west;
  const char *p = parseName(s);
  if (!p || !(p = parseTime(p, 24, &west))) return false;
  tz.stdOff = -west;
  if (*p) {
    if (!(p = parseName(p))) return false;
    tz.hasDst = true;
    tz.dstOff = tz.stdOff + 3600;            // an hour ahead when no offset follows
    if (*p && *p != ',') {
      if (!(p = parseTime(p, 24, &west))) return false;
      tz.dstOff = -west;
    }
    if (*p != ',') return false;
    if (!(p = parseRule(p + 1, &tz.start)) || *p != ',') return false;
    if (!(p = parseRule(p + 1, &tz.end))) return false;
  }
  if (*p) return false;
  *out = tz;
  return true;
}

// ---------------------------------------------------------------- dates
int64_t posixDaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t  era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void posixCivilFromDays(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
  z += 719468;
  const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

// ---------------------------------------------------------------- evaluation
static int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
  return q;
}

static bool leapYear(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The UTC second at which rule r fires in year y. Its time of day is local
// time in the offset in force until then (tzset(3): "local time").
static int64_t changeAt(const PosixTzRule &r, int64_t y, int32_t offBefore) {
  int64_t day;
  if (r.kind == 'M') {
    static const uint8_t kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int64_t first = posixDaysFromCivil(y, r.month, 1);
    const int wdFirst = (int)(((first % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    int dom = 1 + (r.wday - wdFirst + 7) % 7 + (r.week - 1) * 7;
    const int inMonth = kMonthDays[r.month - 1] + (r.month == 2 && leapYear(y) ? 1 : 0);
    while (dom > inMonth) dom -= 7;          // week 5 means the last one
    day = first + dom - 1;
  } else if (r.kind == 'J') {
    day = posixDaysFromCivil(y, 1, 1) + (r.day - 1) + (leapYear(y) && r.day >= 60 ? 1 : 0);
  } else {
    day = posixDaysFromCivil(y, 1, 1) + r.day;
  }
  return day * 86400 + r.secs - offBefore;
}

int32_t posixTzOffset(const PosixTz &tz, int64_t utc) {
  if (!tz.hasDst) return tz.stdOff;
  int64_t y;
  unsigned m, d;
  posixCivilFromDays(floorDiv(utc + tz.stdOff, 86400), &y, &m, &d);
  const int64_t start = changeAt(tz.start, y, tz.stdOff);
  const int64_t end   = changeAt(tz.end, y, tz.dstOff);
  // Southern zones start in the spring of their year and end in its autumn,
  // so the summer straddles the new year: outside [end, start) rather than inside.
  const bool dst = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
  return dst ? tz.dstOff : tz.stdOff;
}

// ---------------------------------------------------------------- the table
const char *tzdbPosix(const char *iana) {
  if (!iana) return nullptr;
  int lo = 0, hi = (int)TZDB_COUNT - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const int c = strcmp(iana, kTzNames + kTzIndex[mid][0]);
    if (c == 0) return kTzPosix + kTzIndex[mid][1];
    if (c < 0) hi = mid - 1;
    else       lo = mid + 1;
  }
  return nullptr;
}

const char *tzdbVersion() { return TZDB_VERSION; }
uint16_t    tzdbCount()   { return (uint16_t)TZDB_COUNT; }

#endif  // WORLDCLOCK_ENABLED

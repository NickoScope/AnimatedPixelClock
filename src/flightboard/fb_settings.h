#pragma once
// Flight board settings that the web portal changes: the AeroAPI budget, the
// checks a custom airport and a tracked ident go through, and the query
// counters the budget is held against.
//
// Plain C++ and ArduinoJson, no Arduino core: the firmware and
// tools/flightboard/settings_host_test.cpp both use it.

#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "fb_model.h"
#include "../fonts/name_chars.h"

namespace fbs {

// ── price ───────────────────────────────────────────────────────────────────
// https://www.flightaware.com/commercial/aeroapi/ (read 2026-09-14): every
// endpoint this panel calls - /airports/{id}/flights/arrivals, departures,
// scheduled_arrivals, scheduled_departures and /flights/{ident} - is
// "$0.005/result set", "one set equaling 15 records", the same on every tier.
// Every call here sends max_pages=1, so it is at most one result set; the
// counters count every call made, answered or not, because the page does not
// say whether an error or an empty answer is billed.
static const uint32_t kMicroUsdPerCall = 5000;
// Personal tier, same page: "up to $5 free per month", "10 result sets/minute".
static const uint32_t kCallSpacingS = 7;       // above 60 / 10 = 6 s, so a burst never meets the limit

// ── budget ──────────────────────────────────────────────────────────────────
// Choices, not FlightAware's numbers. Defaults keep a month inside the
// Personal tier's $5 credit with $0.50 to spare for Home Assistant, which
// calls AeroAPI with the same account.
struct Budget {
  uint16_t floorMin;   // a board list is not asked again within this; past lists wait twice as long
  uint16_t dayCap;     // calls per UTC day, board and trackers together; 0 = no direct calls
  uint16_t monthCap;   // calls per UTC calendar month
};

static const uint16_t kFloorMin = 5, kFloorMax = 240, kFloorDefault = 15;
static const uint16_t kDayMax = 1000, kDayDefault = 30;           // $5.00 a day at most
static const uint16_t kMonthMax = 20000, kMonthDefault = 900;     // $100 a month at most, $4.50 by default

inline Budget defaults() { return {kFloorDefault, kDayDefault, kMonthDefault}; }

inline bool valid(const Budget &b) {
  return b.floorMin >= kFloorMin && b.floorMin <= kFloorMax && b.dayCap <= kDayMax && b.monthCap <= kMonthMax;
}

inline bool intIn(JsonVariantConst v, long lo, long hi, long *out) {
  if (!v.is<long>() || v.is<bool>()) return false;
  const long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = x;
  return true;
}

// The portal's {"budget":{...}}: every key known and every value in range, or
// nothing changes. Returns nullptr on success, else a message naming the field.
inline const char *apply(JsonObjectConst in, Budget &b, char *err, size_t cap) {
  Budget n = b;
  for (JsonPairConst kv : in) {
    const char *k = kv.key().c_str();
    long x;
    if (!strcmp(k, "floor_min")) {
      if (!intIn(kv.value(), kFloorMin, kFloorMax, &x)) { snprintf(err, cap, "budget.floor_min must be %u to %u", kFloorMin, kFloorMax); return err; }
      n.floorMin = (uint16_t)x;
    } else if (!strcmp(k, "day_cap")) {
      if (!intIn(kv.value(), 0, kDayMax, &x)) { snprintf(err, cap, "budget.day_cap must be 0 (off) to %u", kDayMax); return err; }
      n.dayCap = (uint16_t)x;
    } else if (!strcmp(k, "month_cap")) {
      if (!intIn(kv.value(), 0, kMonthMax, &x)) { snprintf(err, cap, "budget.month_cap must be 0 (off) to %u", kMonthMax); return err; }
      n.monthCap = (uint16_t)x;
    } else {
      snprintf(err, cap, "budget.%.24s is not a setting", k);
      return err;
    }
  }
  b = n;
  return nullptr;
}

inline void toJson(const Budget &b, JsonObject o) {
  o["floor_min"] = b.floorMin;
  o["day_cap"]   = b.dayCap;
  o["month_cap"] = b.monthCap;
}

inline void boundsJson(JsonObject o) {
  auto range = [&o](const char *k, long lo, long hi) {
    JsonArray a = o[k].to<JsonArray>();
    a.add(lo);
    a.add(hi);
  };
  range("floor_min", kFloorMin, kFloorMax);
  range("day_cap", 0, kDayMax);
  range("month_cap", 0, kMonthMax);
}

// ── usage ───────────────────────────────────────────────────────────────────
// Counted per UTC day and UTC calendar month. FlightAware's page does not say
// when its month turns over; UTC is this panel's choice.
struct Usage {
  uint32_t day;      // days since 1970-01-01, UTC
  uint32_t month;    // year * 12 + month - 1, UTC
  uint16_t dayN;
  uint16_t monthN;
  uint32_t total;    // since the counters were first written
};

inline uint32_t monthOf(int64_t utc) {
  int64_t z = (utc >= 0 ? utc / 86400 : (utc - 86399) / 86400) + 719468;   // Hinnant, public domain
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
  return (uint32_t)(y * 12 + m - 1);
}

// Start a new day or month when the clock has moved into one. False without a
// clock: the counters are then left as they are, and no call may be made.
inline bool roll(Usage &u, int64_t nowUtc) {
  if (nowUtc < 1700000000) return false;
  const uint32_t d = (uint32_t)(nowUtc / 86400), m = monthOf(nowUtc);
  if (d != u.day)   { u.day = d; u.dayN = 0; }
  if (m != u.month) { u.month = m; u.monthN = 0; }
  return true;
}

// The day's calls the board may use while trackers exist: all but a fifth, at
// least one, kept for them. A choice: a board refresh can wait, a tracked
// flight's take-off cannot.
inline uint16_t boardDayCap(const Budget &b, bool haveTracks) {
  if (!haveTracks || b.dayCap == 0) return b.dayCap;
  const uint16_t keep = b.dayCap / 5 ? b.dayCap / 5 : 1;
  return b.dayCap > keep ? (uint16_t)(b.dayCap - keep) : 0;
}

// Why a call may not be made now, or nullptr if it may. `u` must be rolled.
inline const char *refuse(const Usage &u, const Budget &b, bool board, bool haveTracks) {
  if (b.dayCap == 0 || b.monthCap == 0) return "OFF";
  if (u.monthN >= b.monthCap) return "MONTH CAP";
  if (u.dayN >= b.dayCap) return "DAY CAP";
  if (board && u.dayN >= boardDayCap(b, haveTracks)) return "DAY CAP";
  return nullptr;
}

// ── custom airports ─────────────────────────────────────────────────────────
inline bool upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool digit(char c) { return c >= '0' && c <= '9'; }

inline bool validIcao(const char *s) {
  if (!s || strlen(s) != 4 || !upper(s[0])) return false;
  for (int i = 1; i < 4; i++)
    if (!upper(s[i]) && !digit(s[i])) return false;
  return true;
}

inline bool validIata(const char *s) {
  if (!s) return false;
  if (!*s) return true;
  return strlen(s) == 3 && upper(s[0]) && upper(s[1]) && upper(s[2]);
}

// The shape of an IANA zone name ("Europe/Paris", "America/Argentina/Buenos_Aires",
// "UTC"), or "". checkAirport also requires the zone table to know it.
inline bool validTz(const char *s) {
  if (!s) return false;
  const size_t n = strlen(s);
  if (n == 0) return true;
  if (n > FB_APT_TZ_MAX) return false;
  if (!strcmp(s, "UTC")) return true;
  if (!upper(s[0]) || s[n - 1] == '/' || !strchr(s, '/')) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    const bool ok = upper(c) || (c >= 'a' && c <= 'z') || digit(c) || c == '_' || c == '-' || c == '+' || c == '/';
    if (!ok || (c == '/' && s[i + 1] == '/')) return false;
  }
  return true;
}

// Why a custom airport cannot be kept, or nullptr. `width` gives a name's
// width in pixels as the page draws it; `knownTz` whether the panel's zone
// table has a name. The zone is required: the board shows times in it. Every
// airport with an IATA code in mwgg/Airports, the portal's search, has a zone
// tzdata 2026c knows (7 908 of 7 908, checked 2026-09-14).
inline const char *checkAirport(const FbAirport &a, int (*width)(const char *), bool (*knownTz)(const char *)) {
  if (!validIcao(a.icao)) return "add.icao must be 4 capitals or digits, starting with a letter";
  if (!validIata(a.iata)) return "add.iata must be 3 capitals, or empty";
  const size_t n = strlen(a.name);
  if (n == 0 || n > FB_APT_NAME_MAX) return "add.name must be 1 to 12 bytes (a Cyrillic letter is 2)";
  if (!nameCharsOk(a.name)) return "add.name may hold only capitals A-Z or А-Я, digits, space and . - '";
  for (size_t i = 0; i < n; i++)
    if (a.name[i] == ' ' && (i == 0 || i == n - 1 || a.name[i + 1] == ' '))
      return "add.name must not start or end with a space, or have two in a row";
  if (width && width(a.name) > FB_APT_NAME_PX) return "add.name is too wide for the panel's header";
  if (!a.tz[0] || !validTz(a.tz)) return "add.tz must be an IANA zone name such as Europe/Paris";
  if (knownTz && !knownTz(a.tz)) return "add.tz is not a zone this panel knows";
  return nullptr;
}

}  // namespace fbs

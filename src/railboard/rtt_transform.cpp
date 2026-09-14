#include "rtt_transform.h"

#include <cstdio>
#include <cstring>

#include "uk_time.h"

namespace rtt {
namespace {

bool digits(const char *s, int n, int *v) {
  int x = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    x = x * 10 + (s[i] - '0');
  }
  *v = x;
  return true;
}

// Jinja truthiness for the values this document holds: a non-empty string.
const char *truthyStr(JsonVariantConst v) {
  const char *s = v.as<const char *>();
  return (s && *s) ? s : nullptr;
}

bool isString(JsonVariantConst v, const char *want) {
  const char *s = v.as<const char *>();
  return s && !strcmp(s, want);
}

// `value | string` of a truthy JSON value, for the platform: a string as it
// is, a non-zero integer in decimal. Empty for anything falsy.
void platformText(JsonVariantConst v, char *out, size_t cap) {
  out[0] = '\0';
  if (const char *s = truthyStr(v)) {
    snprintf(out, cap, "%s", s);
  } else if (v.is<long long>() && v.as<long long>() != 0) {
    snprintf(out, cap, "%lld", v.as<long long>());
  }
}

// Copy at most `maxChars` characters of UTF-8, never splitting one.
void copyChars(char *dst, size_t cap, const char *src, size_t maxChars) {
  size_t n = 0, chars = 0;
  while (src[n] && chars < maxChars) {
    size_t len = 1;
    const unsigned char c = (unsigned char)src[n];
    if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
    if (n + len >= cap) break;
    n += len;
    chars++;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

size_t charCount(const char *s) {
  size_t chars = 0;
  for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) chars++;
  return chars;
}

// Jinja's truncate(24, false, '', 0): a string longer than 24 characters is
// cut to its first 24, then back to the last space in them, if there is one.
void truncate24(char *dst, size_t cap, const char *src) {
  if (charCount(src) <= 24) {
    copyChars(dst, cap, src, 24);
    return;
  }
  char slice[4 * 24 + 1];
  copyChars(slice, sizeof(slice), src, 24);
  char *sp = strrchr(slice, ' ');
  if (sp) *sp = '\0';
  copyChars(dst, cap, slice, 24);
}

// "London Waterloo & Reading", joined as the package joins them.
void joinPlaces(JsonVariantConst list, char *out, size_t cap) {
  out[0] = '\0';
  size_t used = 0;
  for (JsonVariantConst e : list.as<JsonArrayConst>()) {
    if (!e.is<JsonObjectConst>()) continue;
    const char *desc = truthyStr(e["location"]["description"]);
    if (!desc) continue;
    const int n = snprintf(out + used, cap - used, "%s%s", used ? " & " : "", desc);
    if (n < 0 || (size_t)n >= cap - used) { out[cap - 1] = '\0'; return; }
    used += (size_t)n;
  }
}

// Keep the earliest RB_MAX_SVC by scheduled time; an equal time goes after the
// ones already there, as Python's stable sort leaves them.
void insertSorted(RbBoard &b, const RbService &sv) {
  uint8_t pos = b.count;
  while (pos > 0 && b.s[pos - 1].t > sv.t) pos--;
  if (pos >= RB_MAX_SVC) return;
  const uint8_t last = b.count < RB_MAX_SVC ? b.count : RB_MAX_SVC - 1;
  for (uint8_t i = last; i > pos; i--) b.s[i] = b.s[i - 1];
  b.s[pos] = sv;
  if (b.count < RB_MAX_SVC) b.count++;
}

}  // namespace

bool parseTime(const char *s, int64_t *out) {
  if (!s || strlen(s) < 19) return false;
  int Y, M, D, h, m, sec;
  if (!digits(s, 4, &Y) || s[4] != '-' || !digits(s + 5, 2, &M) || s[7] != '-' ||
      !digits(s + 8, 2, &D))
    return false;
  if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') return false;
  if (!digits(s + 11, 2, &h) || s[13] != ':' || !digits(s + 14, 2, &m) || s[16] != ':' ||
      !digits(s + 17, 2, &sec))
    return false;
  const char *p = s + 19;
  if (*p == '.' || *p == ',') {
    p++;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
  }
  int offset = 0;
  bool london = false;
  if (*p == 'Z' || *p == 'z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    const int sign = (*p == '-') ? -1 : 1;
    int oh, om;
    if (!digits(p + 1, 2, &oh)) return false;
    const char *q = p + 3;
    if (*q == ':') q++;
    if (!digits(q, 2, &om) || oh > 23 || om > 59) return false;
    offset = sign * (oh * 3600 + om * 60);
    p = q + 2;
  } else {
    // No zone. The spec's StandardisedDateTime says UTC or an offset, but the
    // live gb-nr answer gives the station's own clock time with neither -
    // "scheduleAdvertised":"2026-09-14T18:31:00" for Guildford, read on the
    // panel 2026-09-14 at 19:03 BST. Refusing it dropped every service. It is
    // taken as London civil time.
    london = true;
  }
  if (*p) return false;
  if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 59) return false;
  const int64_t wall = uktime::daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + sec;
  if (london) {
    // BST if the hour before is in summer time. In the autumn repeat this picks
    // the first (BST) instance; a spring-gap time, which no timetable uses,
    // falls back to GMT.
    const int64_t bst = wall - 3600;
    *out = uktime::isBst(bst) ? bst : wall;
  } else {
    *out = wall - offset;
  }
  return true;
}

void formatTime(int64_t utc, char *out, size_t cap) {
  const int64_t days = uktime::floorDiv(utc, 86400);
  const int64_t secs = utc - days * 86400;
  int64_t y;
  unsigned mo, d;
  uktime::civilFromDays(days, y, mo, d);
  snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", (int)y, mo, d, (unsigned)(secs / 3600),
           (unsigned)((secs / 60) % 60), (unsigned)(secs % 60));
}

void buildQuery(const char *crs, int64_t nowUtc, bool haveClock, char *out, size_t cap) {
  if (!haveClock) {
    snprintf(out, cap, "code=%s", crs);
    return;
  }
  char from[24];
  formatTime(nowUtc - (int64_t)kLookbackMin * 60, from, sizeof(from));
  snprintf(out, cap, "code=%s&timeFrom=%s&timeWindow=%u", crs, from, (unsigned)kWindowMin);
}

void locationFilter(JsonDocument &filter) {
  filter.clear();
  filter["query"]["location"]["description"] = true;
  filter["systemStatus"]["realtimeNetworkRail"] = true;
  JsonObject sv = filter["services"].add<JsonObject>();
  JsonObject td = sv["temporalData"].to<JsonObject>();
  td["displayAs"] = true;
  td["scheduledCallType"] = true;
  for (const char *ev : {"departure", "arrival"}) {
    JsonObject e = td[ev].to<JsonObject>();
    e["scheduleAdvertised"] = true;
    e["realtimeActual"]     = true;
    e["realtimeForecast"]   = true;
    e["realtimeEstimate"]   = true;
    e["isCancelled"]        = true;
  }
  JsonObject md = sv["scheduleMetadata"].to<JsonObject>();
  md["inPassengerService"] = true;
  md["modeType"]           = true;
  md["operator"]["code"]   = true;
  JsonObject pm = sv["locationMetadata"]["platform"].to<JsonObject>();
  pm["actual"]  = true;
  pm["planned"] = true;
  sv["origin"].add<JsonObject>()["location"]["description"] = true;
  sv["destination"].add<JsonObject>()["location"]["description"] = true;
}

void tokenFilter(JsonDocument &filter) {
  filter.clear();
  filter["token"]      = true;
  filter["validUntil"] = true;
}

void emptyLists(const char *crs, Lists *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->stn, sizeof(out->stn), "%s", crs);
}

bool transform(JsonVariantConst root, int64_t nowUtc, const char *crs, Lists *out) {
  emptyLists(crs, out);
  JsonArrayConst services = root["services"].as<JsonArrayConst>();
  if (services.isNull()) return false;

  // stn: query.location.description or the code; rt: systemStatus.realtimeNetworkRail or ''.
  if (const char *d = truthyStr(root["query"]["location"]["description"])) {
    snprintf(out->stn, sizeof(out->stn), "%s", d);
  }
  if (const char *r = truthyStr(root["systemStatus"]["realtimeNetworkRail"])) {
    snprintf(out->rt, sizeof(out->rt), "%s", r);
  }

  const int64_t grace = kGraceS;
  for (JsonVariantConst svv : services) {
    out->seen++;
    if (!svv.is<JsonObjectConst>()) continue;
    JsonObjectConst sv = svv.as<JsonObjectConst>();
    JsonVariantConst td = sv["temporalData"];
    JsonVariantConst disp = td["displayAs"];
    JsonVariantConst md = sv["scheduleMetadata"];
    // disp not in [none, 'PASS'] and md.inPassengerService (default true) is not false
    if (disp.isNull() || isString(disp, "PASS")) { out->skipDisp++; continue; }
    JsonVariantConst pax = md["inPassengerService"];
    if (pax.is<bool>() && !pax.as<bool>()) { out->skipPax++; continue; }

    char op[RB_OP_LEN];
    if (isString(md["modeType"], "BUS") || isString(md["modeType"], "SCHEDULED_BUS") ||
        isString(md["modeType"], "REPLACEMENT_BUS")) {
      snprintf(op, sizeof(op), "BUS");
    } else {
      op[0] = '\0';
      if (const char *code = truthyStr(md["operator"]["code"])) copyChars(op, sizeof(op), code, 3);
    }

    JsonVariantConst pm = sv["locationMetadata"]["platform"];
    char platFull[16];
    platformText(pm["actual"], platFull, sizeof(platFull));
    if (!platFull[0]) platformText(pm["planned"], platFull, sizeof(platFull));

    const char *call = td["scheduledCallType"].as<const char *>();
    for (uint8_t side = RB_DEP; side <= RB_ARR; side++) {
      JsonVariantConst ev = td[side == RB_DEP ? "departure" : "arrival"];
      if (!ev.is<JsonObjectConst>()) { out->noEvent++; continue; }
      const char *sched = truthyStr(ev["scheduleAdvertised"]);
      if (!sched) { out->noSched++; continue; }
      const char *wrongCall = side == RB_DEP ? "ADVERTISED_SET_DOWN" : "ADVERTISED_PICK_UP";
      if (call && (!strcmp(call, "OPERATIONAL_ONLY") || !strcmp(call, wrongCall))) { out->skipCall++; continue; }

      int64_t t = 0, x = 0;
      if (!parseTime(sched, &t)) t = 0;
      if (!out->firstT) { out->firstT = t ? t : -1; out->firstNow = nowUtc; }
      const char *xs = truthyStr(ev["realtimeActual"]);
      if (!xs) xs = truthyStr(ev["realtimeForecast"]);
      if (!xs) xs = truthyStr(ev["realtimeEstimate"]);
      if (xs && !parseTime(xs, &x)) x = 0;
      const bool actual = !ev["realtimeActual"].isNull();
      const bool cancelled = (ev["isCancelled"].is<bool>() && ev["isCancelled"].as<bool>()) ||
                             isString(disp, "CANCELLED") || isString(disp, "DIVERTED") ||
                             isString(disp, side == RB_DEP ? "TERMINATES" : "STARTS");
      const int64_t when = x ? x : t;
      bool show;
      if (side == RB_DEP) {
        show = t > 0 && !(actual && !cancelled) && when >= nowUtc - grace;
      } else {
        show = t > 0 && when >= nowUtc - (actual ? 2 * grace : grace);
      }
      if (!show) { out->skipTime++; continue; }

      RbService row;
      memset(&row, 0, sizeof(row));
      char names[4 * RB_NAME_LEN * 4];
      joinPlaces(side == RB_DEP ? sv["destination"] : sv["origin"], names, sizeof(names));
      truncate24(row.n, sizeof(row.n), names);
      const int64_t late = x ? (x / 60 - t / 60) : 0;
      uint8_t st;
      if (cancelled)                        st = RB_CANC;
      else if (side == RB_ARR && actual)    st = RB_ARRIVED;
      else if (!x)                          st = RB_NOREPORT;
      else if (late >= 1)                   st = RB_LATE;
      else                                  st = RB_OK;
      row.t  = (uint32_t)t;
      row.x  = (uint32_t)x;
      row.st = st;
      row.d  = (st == RB_LATE || st == RB_ARRIVED) ? (uint8_t)(late < 0 ? 0 : (late > 255 ? 255 : late)) : 0;
      copyChars(row.p, sizeof(row.p), platFull, 3);
      snprintf(row.o, sizeof(row.o), "%s", op);
      insertSorted(out->board[side], row);
    }
  }
  for (uint8_t side = RB_DEP; side <= RB_ARR; side++) {
    memcpy(out->board[side].stn, out->stn, sizeof(out->stn));
    memcpy(out->board[side].rt, out->rt, sizeof(out->rt));
  }
  return true;
}

bool accessToken(JsonVariantConst root, char *token, size_t cap, int64_t *validUntil) {
  const char *tok = truthyStr(root["token"]);
  *validUntil = 0;
  if (!tok || strlen(tok) >= cap) return false;
  memcpy(token, tok, strlen(tok) + 1);
  int64_t until;
  if (parseTime(root["validUntil"].as<const char *>(), &until)) *validUntil = until;
  return true;
}

}  // namespace rtt

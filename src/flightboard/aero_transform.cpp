#include "aero_transform.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace aero {

namespace {

// ── time ────────────────────────────────────────────────────────────────────
// Howard Hinnant's days_from_civil, public domain
// (howardhinnant.github.io/date_algorithms.html).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

bool digits(const char *s, int n, int *out) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

// A time field: a string that parses, else 0. Null, absent and unreadable are
// all "not known", which is how the automation's Jinja treats them.
int64_t timeOf(JsonVariantConst v) {
  int64_t t = 0;
  const char *s = v.as<const char *>();
  return (s && parseTime(s, &t)) ? t : kNoTime;
}

const char *strOf(JsonVariantConst v) {
  const char *s = v.as<const char *>();
  return s ? s : "";
}

void copy(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  size_t n = 0;
  if (src)
    for (; src[n] && n < cap - 1; n++) dst[n] = src[n];
  dst[n] = '\0';
}

uint32_t fnv(uint32_t h, const char *s) {
  for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

// Jinja's "a or b or c": the first non-empty string.
const char *firstOf(const char *a, const char *b, const char *c = "") {
  if (a && *a) return a;
  if (b && *b) return b;
  return c ? c : "";
}

// origin / destination as the code the page shows: IATA, else ICAO.
//
// Differs from the automation on purpose. It writes
// `code_iata | default(code_icao)`, and Jinja's default() replaces only an
// undefined value: AeroAPI sends a null code_iata for an airport without one,
// so the automation published "ct": null and the page showed no code at all.
const char *codeOf(JsonVariantConst apt) {
  return firstOf(strOf(apt["code_iata"]), strOf(apt["code_icao"]), strOf(apt["code"]));
}

// ── text ────────────────────────────────────────────────────────────────────
// U+00C0..U+00FF and U+0100..U+017F by their base letter, as capitals. A few
// are rough (AE, IJ, OE become one letter): a name that fits is worth more
// than a digraph on a 128 px board.
const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTSAAAAAAACEEEEIIIIDNOOOOO OUUUUYTY";
const char kLatinA[] = "AAAAAACCCCCCCCDDDDEEEEEEEEEEGGGGGGGGHHHHIIIIIIIIIIJJJJKKKLLLLLLLLLL"
                       "NNNNNNNNNOOOOOOOORRRRRRSSSSSSSSTTTTTTUUUUUUUUUUUUWWYYYZZZZZZS";
static_assert(sizeof(kLatin1) == 64 + 1, "one letter for each of U+00C0..U+00FF");
static_assert(sizeof(kLatinA) == 128 + 1, "one letter for each of U+0100..U+017F");

bool drawable(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '\'';
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
const char *listName(List l) {
  switch (l) {
  case ARR_PAST: return "arrivals";
  case ARR_NEXT: return "scheduled_arrivals";
  case DEP_PAST: return "departures";
  case DEP_NEXT: return "scheduled_departures";
  default:       return "";
  }
}

bool parseTime(const char *s, int64_t *out) {
  if (!s) return false;
  int y, mo, d, h, mi, se;
  if (strlen(s) < 20 || !digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' ||
      !digits(s + 8, 2, &d) || (s[10] != 'T' && s[10] != 't') || !digits(s + 11, 2, &h) || s[13] != ':' ||
      !digits(s + 14, 2, &mi) || s[16] != ':' || !digits(s + 17, 2, &se))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
  const char *p = s + 19;
  if (*p == '.') {
    p++;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
  }
  int64_t off = 0;
  if (*p == 'Z' || *p == 'z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    int oh, om;
    const int sign = *p == '-' ? -1 : 1;
    p++;
    if (!digits(p, 2, &oh)) return false;
    p += 2;
    if (*p == ':') p++;
    if (!digits(p, 2, &om)) return false;
    p += 2;
    if (oh > 23 || om > 59) return false;
    off = sign * (oh * 3600 + om * 60);
  } else {
    return false;
  }
  if (*p) return false;
  *out = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se - off;
  return true;
}

void formatTime(int64_t utc, char *out, size_t cap) {
  int64_t days = utc / 86400, secs = utc % 86400;
  if (secs < 0) { secs += 86400; days--; }
  int64_t y;
  unsigned m, d;
  civilFromDays(days, &y, &m, &d);
  snprintf(out, cap, "%04d-%02u-%02uT%02d:%02d:%02dZ", (int)y, m, d, (int)(secs / 3600), (int)(secs / 60 % 60),
           (int)(secs % 60));
}

bool listQuery(const char *icao, List l, int64_t nowUtc, char *out, size_t cap) {
  // type=Airline and max_pages=1 are what the automation's REST sensors send
  // (their resource URLs, counted on nickohome 2026-09-14). start and end are
  // new: the automation left them at AeroAPI's defaults, which reach back 24 h
  // (arrivals, departures), 2 h (scheduled_departures) and 48 h
  // (scheduled_arrivals). With one page of 15 records a board then fills with
  // flights far outside +-2 h. Asking for the window itself costs the same.
  int n = snprintf(out, cap, "/airports/%s/flights/%s?type=Airline&max_pages=1", icao, listName(l));
  if (n < 0 || (size_t)n >= cap) return false;
  if (nowUtc > 0) {
    char a[24], b[24];
    formatTime(nowUtc - kWindowS, a, sizeof(a));
    formatTime(nowUtc + kWindowS, b, sizeof(b));
    // The past lists end at AeroAPI's default, now.
    const int m = listPast(l) ? snprintf(out + n, cap - n, "&start=%s", a)
                              : snprintf(out + n, cap - n, "&start=%s&end=%s", a, b);
    if (m < 0 || (size_t)(n + m) >= cap) return false;
  }
  return true;
}

void listFilter(JsonDocument &filter, List l) {
  filter["links"]["next"] = true;
  JsonObject f = filter[listName(l)].add<JsonObject>();
  for (const char *k : {"ident", "ident_iata", "ident_icao", "fa_flight_id", "cancelled", "scheduled_out",
                        "estimated_out", "actual_out", "actual_off", "actual_on", "scheduled_in", "estimated_in",
                        "actual_in"})
    f[k] = true;
  for (const char *apt : {"origin", "destination"})
    for (const char *k : {"code", "code_iata", "code_icao", "city"}) f[apt][k] = true;
}

bool parseList(JsonVariantConst root, List l, ListResult *out) {
  memset(out, 0, sizeof(*out));
  JsonArrayConst arr = root[listName(l)].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  out->more = !root["links"]["next"].isNull();
  const bool dep = listDeparts(l);
  for (JsonObjectConst f : arr) {
    out->seen++;
    const char *fn = firstOf(strOf(f["ident_iata"]), strOf(f["ident_icao"]), strOf(f["ident"]));
    if (!*fn) { out->noIdent++; continue; }
    int64_t t = dep ? timeOf(f["actual_out"]) : timeOf(f["actual_in"]);
    if (!t) t = dep ? timeOf(f["estimated_out"]) : timeOf(f["estimated_in"]);
    if (!t) t = dep ? timeOf(f["scheduled_out"]) : timeOf(f["scheduled_in"]);
    if (!t) { out->noTime++; continue; }
    if (out->n >= kListMax) { out->overflow++; continue; }
    Cand &c = out->c[out->n++];
    copy(c.fn, sizeof(c.fn), fn);
    JsonVariantConst other = dep ? f["destination"] : f["origin"];
    copy(c.ct, sizeof(c.ct), codeOf(other));
    cleanCity(strOf(other["city"]), c.cy, sizeof(c.cy));
    c.t = t;

    // The automation's status, in its order - with one difference, on purpose:
    // landed is actual_in OR actual_on. The automation looked at actual_in
    // only, so a flight on the runway and not yet at its gate read IN AIR. In
    // Home Assistant's own LHR arrivals on 2026-09-14, 10 of the 15 records
    // were "Landed / Taxiing": actual_on set, actual_in null.
    const int64_t so = timeOf(f["scheduled_out"]), eo = timeOf(f["estimated_out"]);
    const int64_t si = timeOf(f["scheduled_in"]), ei = timeOf(f["estimated_in"]);
    if (f["cancelled"].as<bool>())                         c.st = FB_CANC;
    else if (timeOf(f["actual_in"]) || timeOf(f["actual_on"])) c.st = FB_LAND;
    else if (timeOf(f["actual_off"]))                      c.st = FB_DEP;
    else if (timeOf(f["actual_out"]))                      c.st = FB_BOARD;
    else if ((so && eo && eo - so > kDelayS) || (si && ei && ei - si > kDelayS)) c.st = FB_DELAY;
    else                                                   c.st = FB_SCHED;

    const char *fid = strOf(f["fa_flight_id"]);
    if (*fid) {
      c.id = fnv(2166136261u, fid);
    } else {
      // The automation's stand-in: ident ~ '|' ~ (scheduled_out | default(scheduled_in)).
      c.id = fnv(fnv(fnv(2166136261u, strOf(f["ident"])), "|"),
                 firstOf(strOf(f["scheduled_out"]), strOf(f["scheduled_in"])));
    }
  }
  return true;
}

void buildBoard(const ListResult *past, const ListResult *next, int64_t nowUtc, Board *out) {
  memset(out, 0, sizeof(*out));
  // Everything in the window, both lists, first record of each flight kept.
  Cand all[2 * kListMax];
  uint8_t n = 0;
  for (const ListResult *lr : {past, next}) {
    if (!lr) continue;
    for (uint8_t i = 0; i < lr->n; i++) {
      const Cand &c = lr->c[i];
      if (c.t < nowUtc - kWindowS || c.t > nowUtc + kWindowS) { out->outside++; continue; }
      bool seen = false;
      for (uint8_t j = 0; j < n && !seen; j++) seen = all[j].id == c.id;
      if (seen) { out->dup++; continue; }
      // Insertion keeps it sorted by time and, like Jinja's sort, stable.
      uint8_t at = n;
      while (at > 0 && all[at - 1].t > c.t) { all[at] = all[at - 1]; at--; }
      all[at] = c;
      n++;
    }
  }
  uint8_t nidx = n;
  for (uint8_t i = 0; i < n; i++)
    if (all[i].t >= nowUtc) { nidx = i; break; }
  const uint8_t start = nidx > kHalf ? (uint8_t)(nidx - kHalf) : 0;
  uint8_t count = n > start ? (uint8_t)(n - start) : 0;
  if (count > FB_MAX_ROWS) count = FB_MAX_ROWS;
  for (uint8_t i = 0; i < count; i++) out->rows[i] = all[start + i];
  out->count  = count;
  out->nowIdx = (uint8_t)(nidx - start) < count ? (uint8_t)(nidx - start) : count;
}

// ── tracked flights ─────────────────────────────────────────────────────────
bool trackQuery(const char *ident, int64_t nowUtc, char *out, size_t cap) {
  int n = snprintf(out, cap, "/flights/%s?ident_type=designator&max_pages=1", ident);
  if (n < 0 || (size_t)n >= cap) return false;
  if (nowUtc > 0) {
    // The spec: start and end no further than 10 days back and 2 days ahead.
    // 47 h leaves the request an hour to arrive before "2 days" is passed.
    char a[24], b[24];
    formatTime(nowUtc - 24 * 3600, a, sizeof(a));
    formatTime(nowUtc + 47 * 3600, b, sizeof(b));
    const int m = snprintf(out + n, cap - n, "&start=%s&end=%s", a, b);
    if (m < 0 || (size_t)(n + m) >= cap) return false;
  }
  return true;
}

void trackFilter(JsonDocument &filter) {
  JsonObject f = filter["flights"].add<JsonObject>();
  for (const char *k : {"ident", "ident_iata", "ident_icao", "fa_flight_id", "cancelled", "diverted",
                        "scheduled_out", "estimated_out", "actual_out", "scheduled_off", "estimated_off",
                        "actual_off", "scheduled_on", "estimated_on", "actual_on", "scheduled_in",
                        "estimated_in", "actual_in", "departure_delay", "arrival_delay", "gate_origin"})
    f[k] = true;
  for (const char *apt : {"origin", "destination"})
    for (const char *k : {"code", "code_iata", "code_icao"}) f[apt][k] = true;
}

namespace {

struct Pick {
  JsonObjectConst f;
  int64_t key;
};

void fill(JsonObjectConst f, Track *t) {
  copy(t->fn, sizeof(t->fn), firstOf(strOf(f["ident_iata"]), strOf(f["ident_icao"]), strOf(f["ident"])));
  copy(t->from, sizeof(t->from), codeOf(f["origin"]));
  copy(t->to, sizeof(t->to), codeOf(f["destination"]));
  copy(t->gate, sizeof(t->gate), strOf(f["gate_origin"]));
  t->schedOut = timeOf(f["scheduled_out"]);
  if (!t->schedOut) t->schedOut = timeOf(f["scheduled_off"]);
  t->estOut = timeOf(f["estimated_out"]);
  if (!t->estOut) t->estOut = timeOf(f["estimated_off"]);
  t->actOut = timeOf(f["actual_out"]);
  t->actOff = timeOf(f["actual_off"]);
  t->schedIn = timeOf(f["scheduled_in"]);
  if (!t->schedIn) t->schedIn = timeOf(f["scheduled_on"]);
  t->estIn  = timeOf(f["estimated_in"]);
  t->estOn  = timeOf(f["estimated_on"]);
  t->actIn  = timeOf(f["actual_in"]);
  t->actOn  = timeOf(f["actual_on"]);
  // Seconds, negative when early (spec: departure_delay, arrival_delay).
  t->depDelayS = f["departure_delay"].is<int>() ? f["departure_delay"].as<int>() : 0;
  t->arrDelayS = f["arrival_delay"].is<int>() ? f["arrival_delay"].as<int>() : 0;
  t->id = fnv(2166136261u, strOf(f["fa_flight_id"]));

  if (f["diverted"].as<bool>())        t->state = FB_TRK_DIVERTED;
  else if (f["cancelled"].as<bool>())  t->state = FB_TRK_CANCELLED;
  else if (t->actIn || t->actOn)       t->state = FB_TRK_LANDED;
  else if (t->actOff)                  t->state = FB_TRK_ENROUTE;
  else if (t->actOut)                  t->state = FB_TRK_TAXI;
  else {
    int64_t late = t->depDelayS;
    if (!late && t->schedOut && t->estOut) late = t->estOut - t->schedOut;
    t->state = late > kDelayS ? FB_TRK_DELAYED : FB_TRK_SCHED;
  }
}

}  // namespace

bool pickTrack(JsonVariantConst root, int64_t nowUtc, int64_t addedUtc, Track *out) {
  memset(out, 0, sizeof(*out));
  JsonArrayConst arr = root["flights"].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  Pick moving{}, landed{}, ahead{}, latest{};
  for (JsonObjectConst f : arr) {
    if (out->flights < 255) out->flights++;
    int64_t sched = timeOf(f["scheduled_out"]);
    if (!sched) sched = timeOf(f["scheduled_off"]);
    const bool cancelled = f["cancelled"].as<bool>();
    int64_t land = timeOf(f["actual_in"]);
    if (!land) land = timeOf(f["actual_on"]);
    const bool left = timeOf(f["actual_out"]) || timeOf(f["actual_off"]);
    if (latest.f.isNull() || sched > latest.key) latest = {f, sched};
    if (left && !land && !cancelled) {
      if (moving.f.isNull() || sched > moving.key) moving = {f, sched};
    } else if (land) {
      if (land >= nowUtc - 3 * 3600 && land >= addedUtc - 3600 && (landed.f.isNull() || land > landed.key))
        landed = {f, land};
    } else if (!left) {
      int64_t dep = timeOf(f["estimated_out"]);
      if (!dep) dep = timeOf(f["estimated_off"]);
      if (!dep) dep = sched;
      if (dep && dep >= nowUtc - 6 * 3600 && (ahead.f.isNull() || dep < ahead.key)) ahead = {f, dep};
    }
  }
  const Pick &p = !moving.f.isNull() ? moving : !landed.f.isNull() ? landed : !ahead.f.isNull() ? ahead : latest;
  if (p.f.isNull()) {
    out->state = FB_TRK_NOTFOUND;
    return true;
  }
  fill(p.f, out);
  out->current = &p != &latest;
  return true;
}

uint32_t trackNextS(const Track &t, int64_t nowUtc) {
  const uint32_t kMin = 10 * 60, kMax = 6 * 3600;
  switch (t.state) {
  case FB_TRK_NOTFOUND:  return kMax;
  case FB_TRK_LANDED:
  case FB_TRK_CANCELLED: return t.current ? 0 : kMax;
  case FB_TRK_DIVERTED:  return 30 * 60;
  case FB_TRK_TAXI:      return kMin;
  case FB_TRK_ENROUTE: {
    const int64_t due = t.estOn ? t.estOn : (t.estIn ? t.estIn : t.schedIn);
    return (due && due - nowUtc <= 45 * 60) ? kMin : 20 * 60;
  }
  case FB_TRK_SCHED:
  case FB_TRK_DELAYED: {
    if (!t.current) return kMax;
    const int64_t dep = t.estOut ? t.estOut : t.schedOut;
    if (!dep) return kMax;
    const int64_t lead = dep - 3600 - nowUtc;
    if (lead <= 0) return kMin;
    const int64_t s = lead / 3;
    return s < kMin ? kMin : (s > kMax ? kMax : (uint32_t)s);
  }
  default:               return kMin;
  }
}

int64_t trackExpiresAt(const Track &t) {
  if (!t.current) return 0;
  if (t.state == FB_TRK_LANDED) return (t.actIn ? t.actIn : t.actOn) + 2 * 3600;
  if (t.state == FB_TRK_CANCELLED && t.schedOut) return t.schedOut + 6 * 3600;
  return 0;
}

int64_t trackShownTime(const Track &t) {
  if (t.actOff || t.actOn || t.actIn || t.state == FB_TRK_LANDED || t.state == FB_TRK_ENROUTE ||
      t.state == FB_TRK_DIVERTED) {
    if (t.actIn) return t.actIn;
    if (t.actOn) return t.actOn;
    if (t.estIn) return t.estIn;
    if (t.estOn) return t.estOn;
    return t.schedIn;
  }
  if (t.actOut) return t.actOut;
  return t.estOut ? t.estOut : t.schedOut;
}

int32_t trackDelayMin(const Track &t) {
  const bool arrival = t.actOff || t.state == FB_TRK_ENROUTE || t.state == FB_TRK_LANDED;
  int64_t s = arrival ? t.arrDelayS : t.depDelayS;
  if (!s && !arrival && t.schedOut && t.estOut) s = t.estOut - t.schedOut;
  // Rounded to the nearest minute, halves away from zero.
  return (int32_t)(s >= 0 ? (s + 30) / 60 : -((-s + 30) / 60));
}

// ── text ────────────────────────────────────────────────────────────────────
void cleanCity(const char *utf8, char *out, size_t cap) {
  if (!cap) return;
  size_t n = 0;
  bool space = true;              // at the start, and after a space: drop further spaces
  for (const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : ""); *p && *p != '(' && *p != '/'; p++) {
    char c;
    if (*p < 0x80) {
      c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : (char)*p;
    } else if ((*p == 0xC3 || *p == 0xC4 || *p == 0xC5) && p[1] >= 0x80 && p[1] <= 0xBF) {
      c = *p == 0xC3 ? kLatin1[p[1] - 0x80] : kLatinA[(*p - 0xC4) * 64 + (p[1] - 0x80)];
      p++;
    } else {
      // Any other sequence: skip its continuation bytes, leave a space.
      while ((p[1] & 0xC0) == 0x80) p++;
      c = ' ';
    }
    if (!drawable(c)) c = ' ';
    if (c == ' ' && space) continue;
    space = c == ' ';
    if (n < cap - 1) out[n++] = c;
  }
  while (n && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

bool normaliseIdent(const char *in, char *out, size_t cap) {
  char s[16];
  size_t n = 0;
  for (const char *p = in ? in : ""; *p; p++) {
    if (*p == ' ') continue;
    char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    if (n >= sizeof(s) - 1) return false;
    s[n++] = c;
  }
  s[n] = '\0';
  // Airline code, then 1-4 digits, then at most one letter. Two tries: a
  // 3-letter ICAO code, else a 2-character IATA code with a letter in it.
  auto rest = [&](size_t from) {
    size_t i = from, d = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { i++; d++; }
    if (d < 1 || d > 4) return false;
    if (i < n && s[i] >= 'A' && s[i] <= 'Z') i++;
    return i == n;
  };
  auto letter = [](char c) { return c >= 'A' && c <= 'Z'; };
  const bool icao = n >= 4 && letter(s[0]) && letter(s[1]) && letter(s[2]) && rest(3);
  const bool iata = n >= 3 && (letter(s[0]) || letter(s[1])) && rest(2);
  if (!icao && !iata) return false;
  if (n + 1 > cap || n + 1 > FB_IDENT_LEN) return false;
  memcpy(out, s, n + 1);
  return true;
}

}  // namespace aero

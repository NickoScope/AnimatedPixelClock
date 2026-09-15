#include "market_model.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace market {

const char *const kPresetKeys[kPresets]  = {"WTD", "MTD", "1M", "3M", "6M", "YTD", "1Y", "3Y", "5Y", "10Y", "MAX"};
const char *const kFeedKeys[F_COUNT]     = {"", "LIVE", "DELAYED", "STALE", "CLOSED"};
const char *const kExStateKeys[X_COUNT]  = {"--", "OPEN", "PRE", "POST", "CLOSED", "STALE"};
const char *const kModeKeys[M_COUNT]     = {"hold", "rebal"};
const char *const kAppStateKeys[A_COUNT] = {"ok", "stale", "error"};

// ── text ────────────────────────────────────────────────────────────────────
size_t copyText(char *dst, size_t cap, const char *src) {
  if (!dst || !cap) return 0;
  size_t k = 0;
  for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && k + 1 < cap; p++)
    dst[k++] = (*p >= 0x20 && *p <= 0x7E) ? (char)*p : '?';
  dst[k] = '\0';
  return k;
}

void upper(char *s) {
  for (; s && *s; s++)
    if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static bool alphabet(const char *s, size_t min, size_t max, const char *extra) {
  if (!s) return false;
  size_t n = 0;
  for (; s[n]; n++) {
    const char c = s[n];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (extra && strchr(extra, c)))) return false;
  }
  return n >= min && n <= max;
}

bool validSymbol(const char *s)   { return alphabet(s, 1, kSymLen - 1, ".^=-"); }
bool validName(const char *s)     { return alphabet(s, 1, kNameLen - 1, ".^=-"); }
bool validCurrency(const char *s) { return s && strlen(s) == 3 && alphabet(s, 3, 3, nullptr) && !strpbrk(s, "0123456789"); }
bool validExchange(const char *s) { return alphabet(s, 1, kExNameLen - 1, nullptr); }

static int keyIndex(const char *s, const char *const *keys, int n) {
  if (!s) return -1;
  for (int i = 0; i < n; i++)
    if (!strcmp(s, keys[i])) return i;
  return -1;
}

int presetIndex(const char *s)  { return keyIndex(s, kPresetKeys, kPresets); }
int modeIndex(const char *s)    { return keyIndex(s, kModeKeys, M_COUNT); }
int exStateIndex(const char *s) {
  const int i = keyIndex(s, kExStateKeys, X_COUNT);
  return i > 0 ? i : -1;   // "--" is the panel's word for none, never the app's
}

int feedIndex(const char *s) {
  const int i = keyIndex(s, kFeedKeys, F_COUNT);
  return i > 0 ? i : -1;
}

// ── dates ───────────────────────────────────────────────────────────────────
// Howard Hinnant's algorithms, valid across the whole int32 range we use.
int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const int32_t yoe = y - era * 400;
  const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int32_t z, int *y, int *m, int *d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int32_t doe = z - era * 146097;
  const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int32_t mp  = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

static bool digits(const char *s, int n) {
  for (int i = 0; i < n; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

int32_t parseDate(const char *iso) {
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return kNoDate;
  if (!digits(iso, 4) || !digits(iso + 5, 2) || !digits(iso + 8, 2)) return kNoDate;
  const int y = atoi(iso), m = atoi(iso + 5), d = atoi(iso + 8);
  if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return kNoDate;
  const int32_t z = daysFromCivil(y, m, d);
  int yy, mm, dd;
  civilFromDays(z, &yy, &mm, &dd);
  return (yy == y && mm == m && dd == d) ? z : kNoDate;   // 2026-02-30 is not a date
}

int weekday(int32_t days) {
  // 1970-01-01 was a Thursday (3, Monday = 0).
  const int32_t w = (days + 3) % 7;
  return (int)(w < 0 ? w + 7 : w);
}

int weekdaysAfter(int32_t a, int32_t b) {
  if (a == kNoDate || b == kNoDate || b <= a) return 0;
  int n = 0;
  // Whole weeks first, then the tail, so a long gap costs nothing.
  const int32_t span = b - a;
  n += (int)(span / 7) * 5;
  for (int32_t d = a + (span / 7) * 7 + 1; d <= b; d++) n += weekday(d) < 5;
  return n;
}

bool isStale(int32_t asof, int32_t today, int tradingDays) {
  if (asof == kNoDate || today == kNoDate) return false;
  return weekdaysAfter(asof, today) > tradingDays;
}

// ── numbers ─────────────────────────────────────────────────────────────────
static size_t fit(int n, size_t cap) { return (n > 0 && (size_t)n < cap) ? (size_t)n : (cap ? cap - 1 : 0); }

// Python's round(): half to even, on the double itself.
static long pyRound(double v) { return (long)std::nearbyint(v); }

size_t thousands(char *out, size_t cap, long n) {
  char digs[24];
  const int len = snprintf(digs, sizeof(digs), "%ld", n < 0 ? -n : n);
  size_t k = 0;
  if (n < 0 && k + 1 < cap) out[k++] = '-';
  for (int i = 0; i < len && k + 1 < cap; i++) {
    if (i && (len - i) % 3 == 0 && k + 1 < cap) out[k++] = ' ';
    if (k + 1 < cap) out[k++] = digs[i];
  }
  out[k] = '\0';
  return k;
}

// The magnitude's text for fmtValue / fmtAmount: `plain` first, then K, M (and
// B for a value) with one decimal, the first that fits `slot` less the sign.
static size_t scaled(char *out, size_t cap, double a, const char *plain, int slot, bool negative, bool billions) {
  const size_t room = slot > (negative ? 1 : 0) ? (size_t)(slot - (negative ? 1 : 0)) : 0;
  char body[32];
  snprintf(body, sizeof(body), "%s", plain);
  if (strlen(body) > room) {
    static const char units[] = {'K', 'M', 'B'};
    static const double divs[] = {1e3, 1e6, 1e9};
    const int n = billions ? 3 : 2;
    for (int i = 0; i < n; i++) {
      snprintf(body, sizeof(body), "%.1f%c", a / divs[i], units[i]);
      if (strlen(body) <= room) break;
    }
  }
  return fit(snprintf(out, cap, "%s%s", negative ? "-" : "", body), cap);
}

size_t fmtValue(char *out, size_t cap, double v, int slot) {
  const bool neg = v < 0;
  const double a = neg ? -v : v;
  char plain[32];
  if (a < 1000) snprintf(plain, sizeof(plain), "%.1f", a);
  else          thousands(plain, sizeof(plain), pyRound(a));
  return scaled(out, cap, a, plain, slot, neg, true);
}

// Whole units, then K / M with one decimal, or whole K / M when the decimal
// does not fit: 21 663 in four characters is 22K (render.py's fmt_amount of
// 2026-09-15; before, it fell through to 0.0M).
size_t fmtAmount(char *out, size_t cap, double v, int slot) {
  const bool neg = v < 0;
  const double a = neg ? -v : v;
  const int sign = neg ? 1 : 0;
  char body[32];
  thousands(body, sizeof(body), pyRound(a));
  if ((int)strlen(body) + sign <= slot) return fit(snprintf(out, cap, "%s%s", neg ? "-" : "", body), cap);
  static const char units[] = {'K', 'M'};
  static const double divs[] = {1e3, 1e6};
  for (int i = 0; i < 2; i++)
    for (int dec = 1; dec >= 0; dec--) {
      snprintf(body, sizeof(body), "%.*f%c", dec, a / divs[i], units[i]);
      if ((int)strlen(body) + sign <= slot) return fit(snprintf(out, cap, "%s%s", neg ? "-" : "", body), cap);
    }
  return fit(snprintf(out, cap, "%s%s", neg ? "-" : "", body), cap);   // the last tried, as render.py
}

// The percent as printed: one decimal under 100, none above. Python's
// round(p, 1) is the exact decimal of the double, which %.1f also gives.
static double pctRound(double x) {
  const double p = x * 100.0;
  if (std::fabs(p) < 100) {
    char t[32];
    snprintf(t, sizeof(t), "%.1f", p);
    return atof(t);
  }
  return (double)pyRound(p);
}

size_t fmtPct(char *out, size_t cap, double x, bool have) {
  if (!have) return fit(snprintf(out, cap, "--"), cap);
  const double p = pctRound(x);
  if (p == 0) return fit(snprintf(out, cap, "0.0%%"), cap);
  if (std::fabs(p) < 100) return fit(snprintf(out, cap, "%+.1f%%", p), cap);
  char t[24];
  thousands(t, sizeof(t), (long)std::fabs(p));
  return fit(snprintf(out, cap, "%c%s%%", p > 0 ? '+' : '-', t), cap);
}

int pctSign(double x, bool have) {
  if (!have || pctRound(x) == 0) return 0;
  return x > 0 ? 1 : -1;
}

size_t fmtShare(char *out, size_t cap, double x) {
  const double p = x * 100.0;
  if (p < 10) return fit(snprintf(out, cap, "%.1f%%", p), cap);
  return fit(snprintf(out, cap, "%.0f%%", p), cap);
}

size_t fmtYm(char *out, size_t cap, int32_t days) {
  if (days == kNoDate) return fit(snprintf(out, cap, "--"), cap);
  int y, m, d;
  civilFromDays(days, &y, &m, &d);
  return fit(snprintf(out, cap, "%02d-%02d", y % 100, m), cap);
}

size_t fmtDate(char *out, size_t cap, int32_t days) {
  static const char *const kMon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  if (days == kNoDate) return fit(snprintf(out, cap, "--"), cap);
  int y, m, d;
  civilFromDays(days, &y, &m, &d);
  return fit(snprintf(out, cap, "%02d %s", d, kMon[m - 1]), cap);
}

// ── fonts ───────────────────────────────────────────────────────────────────
int16_t w5(const char *s, uint8_t k, bool thin) {
  if (!s || !*s) return 0;
  int16_t w = 0;
  for (; *s; s++) w += (thin && (*s == ' ' || *s == '.')) ? 3 : 6;
  return (int16_t)(w * k - k);
}

// ── base64 ──────────────────────────────────────────────────────────────────
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool decodePts(const char *b64, uint16_t out[kPoints]) {
  if (!b64 || strlen(b64) != kPtsB64) return false;
  uint8_t raw[kPoints * 2];
  size_t k = 0;
  for (size_t i = 0; i < kPtsB64; i += 4) {
    const bool last = i + 4 == kPtsB64;
    int v[4];
    for (int j = 0; j < 4; j++) {
      const char c = b64[i + j];
      if (last && j >= 2 && c == '=') { v[j] = 0; continue; }   // "xx==" closes 256 bytes
      v[j] = b64val(c);
      if (v[j] < 0) return false;
    }
    if (last && (b64[i + 2] != '=' || b64[i + 3] != '=')) return false;
    const uint32_t bits = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    raw[k++] = (uint8_t)(bits >> 16);
    if (!last) { raw[k++] = (uint8_t)(bits >> 8); raw[k++] = (uint8_t)bits; }
  }
  if (k != kPoints * 2) return false;
  for (int i = 0; i < kPoints; i++) out[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
  return true;
}

// ── JSON fields ─────────────────────────────────────────────────────────────
static bool num(JsonVariantConst v, float *out) {
  if (v.isNull() || v.is<bool>() || !v.is<float>()) return false;
  *out = v.as<float>();
  return std::isfinite(*out);
}

// null or absent reads as "none"; a present value must be a number.
static bool optNum(JsonVariantConst v, float *out, bool *have) {
  if (v.isNull()) { *out = 0; *have = false; return true; }
  *have = true;
  return num(v, out);
}

static bool str(JsonVariantConst v, const char **out) {
  if (v.isNull() || !v.is<const char *>()) return false;
  *out = v.as<const char *>();
  return *out != nullptr;
}

static bool optStr(JsonVariantConst v, const char **out) {
  if (v.isNull()) { *out = ""; return true; }
  return str(v, out);
}

static bool u32(JsonVariantConst v, uint32_t max, uint32_t *out) {
  if (v.isNull() || v.is<bool>() || !v.is<uint32_t>()) return false;
  *out = v.as<uint32_t>();
  return *out <= max;
}

static bool date(JsonVariantConst v, int32_t *out) {
  const char *s;
  if (!str(v, &s)) return false;
  *out = parseDate(s);
  return *out != kNoDate;
}

static bool optDate(JsonVariantConst v, int32_t *out) {
  if (v.isNull()) { *out = kNoDate; return true; }
  return date(v, out);
}

static bool pts(JsonVariantConst v, uint16_t out[kPoints]) {
  const char *s;
  return str(v, &s) && decodePts(s, out);
}

static const char *header(JsonObjectConst in) {
  if (in.isNull()) return "not an object";
  JsonVariantConst v = in["v"];
  if (v.isNull() || v.is<bool>() || !v.is<long>() || v.as<long>() != kSchema) return "v";
  return nullptr;
}

static const char *symField(JsonObjectConst in, const char *sym) {
  const char *s;
  if (!str(in["sym"], &s) || !validSymbol(s)) return "sym";
  if (sym && strcmp(s, sym)) return "sym differs from the topic";
  return nullptr;
}

static void countDrop(uint8_t *dropped) {
  if (*dropped < 255) (*dropped)++;
}

// ── decoders ────────────────────────────────────────────────────────────────
const char *seriesFrom(JsonObjectConst in, const char *sym, uint8_t preset, Series *out) {
  Series &s = *out;
  memset(&s, 0, sizeof(s));
  if (const char *e = header(in)) return e;
  if (const char *e = symField(in, sym)) return e;
  const char *p, *name, *cur;
  if (!str(in["preset"], &p) || presetIndex(p) != (int)preset) return "preset";
  if (!optStr(in["name"], &name) || (name[0] && !validName(name))) return "name";
  if (!str(in["cur"], &cur) || !validCurrency(cur)) return "cur";
  if (!date(in["from"], &s.from)) return "from";
  if (!date(in["to"], &s.to)) return "to";
  if (s.from > s.to) return "from after to";
  if (!num(in["last"], &s.last)) return "last";
  if (!num(in["chg"], &s.chg)) return "chg";
  if (!num(in["hi"], &s.hi)) return "hi";
  if (!num(in["lo"], &s.lo)) return "lo";
  if (s.lo > s.hi) return "lo above hi";
  if (!num(in["min"], &s.min)) return "min";
  if (!num(in["max"], &s.max)) return "max";
  if (s.min > s.max) return "min above max";
  bool haveMdd;
  if (!optNum(in["cagr"], &s.cagr, &s.hasCagr)) return "cagr";
  if (!optNum(in["ann"], &s.ann, &s.hasAnn)) return "ann";
  if (!optNum(in["mdd"], &s.mdd, &haveMdd)) return "mdd";
  uint32_t n;
  if (!u32(in["n"], kPoints, &n) || n != kPoints) return "n";
  if (!pts(in["pts"], s.pts)) return "pts";
  copyText(s.sym, sizeof(s.sym), in["sym"].as<const char *>());
  copyText(s.name, sizeof(s.name), name[0] ? name : s.sym);
  copyText(s.cur, sizeof(s.cur), cur);
  s.preset = preset;
  s.have   = true;
  return nullptr;
}

const char *portfolioFrom(JsonObjectConst in, uint8_t mode, uint8_t preset, Portfolio *out) {
  Portfolio &p = *out;
  memset(&p, 0, sizeof(p));
  if (const char *e = header(in)) return e;
  const char *m, *pr, *cur;
  if (!str(in["mode"], &m) || modeIndex(m) != (int)mode) return "mode";
  if (!str(in["preset"], &pr) || presetIndex(pr) != (int)preset) return "preset";
  if (!str(in["cur"], &cur) || !validCurrency(cur)) return "cur";
  if (!date(in["from"], &p.from)) return "from";
  if (!date(in["to"], &p.to)) return "to";
  if (p.from > p.to) return "from after to";
  if (!num(in["value"], &p.value)) return "value";
  if (!num(in["chg"], &p.chg)) return "chg";
  if (!num(in["sinceStart"], &p.sinceStart)) return "sinceStart";
  bool haveMdd, haveDiv, haveTer, haveCash;
  if (!optNum(in["cagr"], &p.cagr, &p.hasCagr)) return "cagr";
  if (!optNum(in["mdd"], &p.mdd, &haveMdd)) return "mdd";
  if (!optNum(in["div"], &p.div, &haveDiv)) return "div";
  if (!optNum(in["terDrag"], &p.terDrag, &haveTer)) return "terDrag";
  if (!optNum(in["cash"], &p.cash, &haveCash)) return "cash";
  // Docs/18's decisions of 11:43; every one optional, so an app from before them still draws.
  if (!optNum(in["twr"], &p.twr, &p.hasTwr)) return "twr";
  if (!optNum(in["ann"], &p.ann, &p.hasAnn)) return "ann";
  if (!optNum(in["xirr_ann"], &p.xirrAnn, &p.hasXirr)) return "xirr_ann";
  if (!optNum(in["fx_effect"], &p.fxEffect, &p.hasFx)) return "fx_effect";
  if (!optNum(in["contrib"], &p.contrib, &p.hasContrib)) return "contrib";
  if (!in["flows"].isNull()) {
    if (!in["flows"].is<bool>()) return "flows";
    p.flows = in["flows"].as<bool>();
  }
  if (!optDate(in["mdd_peak"], &p.mddPeak)) return "mdd_peak";
  if (!optDate(in["mdd_trough"], &p.mddTrough)) return "mdd_trough";
  if (!optDate(in["mdd_recovery"], &p.mddRecovery)) return "mdd_recovery";
  if (p.mddPeak != kNoDate && p.mddTrough != kNoDate && p.mddPeak > p.mddTrough) return "mdd_peak after mdd_trough";
  if (!num(in["min"], &p.min)) return "min";
  if (!num(in["max"], &p.max)) return "max";
  if (p.min > p.max) return "min above max";
  uint32_t n;
  if (!u32(in["n"], kPoints, &n) || n != kPoints) return "n";
  if (!pts(in["pts"], p.pts)) return "pts";
  if (!pts(in["px"], p.px)) return "px";
  if (!in["bench"].isNull()) { if (!pts(in["bench"], p.bench)) return "bench"; p.hasBench = true; }
  if (!in["gross"].isNull()) { if (!pts(in["gross"], p.gross)) return "gross"; p.hasGross = true; }
  copyText(p.cur, sizeof(p.cur), cur);
  p.mode   = mode;
  p.preset = preset;
  p.have   = true;
  return nullptr;
}

const char *holdingsFrom(JsonObjectConst in, uint8_t mode, Holdings *out) {
  Holdings &h = *out;
  memset(&h, 0, sizeof(h));
  if (const char *e = header(in)) return e;
  const char *m;
  if (!str(in["mode"], &m) || modeIndex(m) != (int)mode) return "mode";
  if (!date(in["asof"], &h.asof)) return "asof";
  JsonArrayConst rows = in["rows"].as<JsonArrayConst>();
  if (rows.isNull()) return "rows";
  JsonObjectConst cash = in["cash"].as<JsonObjectConst>();
  if (cash.isNull() || !num(cash["tgt"], &h.cashTgt) || !num(cash["now"], &h.cashNow)) return "cash";
  for (JsonVariantConst item : rows) {
    JsonObjectConst o = item.as<JsonObjectConst>();
    HoldRow r;
    memset(&r, 0, sizeof(r));
    const char *sym = nullptr;
    bool ok = !o.isNull() && str(o["sym"], &sym) && validSymbol(sym) && num(o["tgt"], &r.tgt) && num(o["now"], &r.now) &&
              optNum(o["ret"], &r.ret, &r.hasRet) && optDate(o["entry"], &r.entry);
    for (uint8_t i = 0; ok && i < h.n; i++) ok = strcmp(h.rows[i].sym, sym) != 0;
    if (!ok || h.n >= kMaxPositions) { countDrop(&h.dropped); continue; }
    copyText(r.sym, sizeof(r.sym), sym);
    h.rows[h.n++] = r;
  }
  h.mode = mode;
  h.have = true;
  return nullptr;
}

const char *liveFrom(JsonObjectConst in, Live *out) {
  Live &l = *out;
  memset(&l, 0, sizeof(l));
  if (const char *e = header(in)) return e;
  if (!u32(in["ts"], 0xFFFFFFFFUL, &l.ts) || l.ts < 1000000000UL) return "ts";
  JsonObjectConst q = in["q"].as<JsonObjectConst>();
  if (q.isNull()) return "q";
  for (JsonPairConst kv : q) {
    JsonObjectConst o = kv.value().as<JsonObjectConst>();
    Quote e;
    memset(&e, 0, sizeof(e));
    const char *st = nullptr;
    int si = -1;
    bool ok = !o.isNull() && validSymbol(kv.key().c_str()) && num(o["last"], &e.last) && optNum(o["prev"], &e.prev, &e.hasDay) &&
              // The app omits a state it does not know rather than send "--".
              (o["state"].isNull() ? ((si = X_NONE), true) : (str(o["state"], &st) && (si = exStateIndex(st)) >= 0)) &&
              optDate(o["asof"], &e.asof);
    if (ok) {
      bool haveDay;
      ok = optNum(o["day"], &e.day, &haveDay);
      e.hasDay = e.hasDay && haveDay;
    }
    if (ok && !o["feed"].isNull()) {           // docs/19: LIVE | DELAYED | STALE | CLOSED, plus delay_s
      const char *fd;
      int fi;
      ok = str(o["feed"], &fd) && (fi = feedIndex(fd)) >= 0;
      if (ok) e.feed = (uint8_t)fi;
    }
    if (ok && !o["delay_s"].isNull()) {
      uint32_t ds;
      ok = u32(o["delay_s"], 65535, &ds);
      if (ok) { e.delayS = (uint16_t)ds; e.hasDelay = true; }
    }
    for (uint8_t i = 0; ok && i < l.n; i++) ok = strcmp(l.q[i].sym, kv.key().c_str()) != 0;
    if (!ok || l.n >= kMaxSymbols) { countDrop(&l.dropped); continue; }
    copyText(e.sym, sizeof(e.sym), kv.key().c_str());
    e.state = (uint8_t)si;
    l.q[l.n++] = e;
  }
  l.have = true;
  return nullptr;
}

const char *intradayFrom(JsonObjectConst in, const char *sym, Intraday *out) {
  Intraday &d = *out;
  memset(&d, 0, sizeof(d));
  if (const char *e = header(in)) return e;
  if (const char *e = symField(in, sym)) return e;
  if (!u32(in["ts"], 0xFFFFFFFFUL, &d.ts) || d.ts < 1000000000UL) return "ts";
  uint32_t n;
  if (!u32(in["n"], kPoints, &n)) return "n";
  if (!num(in["min"], &d.min)) return "min";
  if (!num(in["max"], &d.max)) return "max";
  if (d.min > d.max) return "min above max";
  if (!pts(in["pts"], d.pts)) return "pts";
  copyText(d.sym, sizeof(d.sym), in["sym"].as<const char *>());
  d.n    = (uint8_t)n;
  d.have = true;
  return nullptr;
}

const char *tapeFrom(JsonObjectConst in, Tape *out) {
  Tape &t = *out;
  memset(&t, 0, sizeof(t));
  if (const char *e = header(in)) return e;
  if (!u32(in["ts"], 0xFFFFFFFFUL, &t.ts) || t.ts < 1000000000UL) return "ts";
  JsonArrayConst x = in["x"].as<JsonArrayConst>();
  if (x.isNull()) return "x";
  for (JsonVariantConst item : x) {
    JsonObjectConst o = item.as<JsonObjectConst>();
    const char *n = nullptr, *s = nullptr, *next = "";
    int si = -1;
    bool ok = !o.isNull() && str(o["n"], &n) && validExchange(n) && (o["s"].isNull() ? ((si = X_NONE), true) : (str(o["s"], &s) && (si = exStateIndex(s)) >= 0)) &&
              optStr(o["t"], &next) && strlen(next) < kNextLen;
    for (uint8_t i = 0; ok && i < t.n; i++) ok = strcmp(t.x[i].name, n) != 0;
    if (!ok || t.n >= kMaxTape) { countDrop(&t.dropped); continue; }
    TapeItem &e = t.x[t.n++];
    copyText(e.name, sizeof(e.name), n);
    copyText(e.next, sizeof(e.next), next);
    e.state = (uint8_t)si;
  }
  t.have = true;
  return nullptr;
}

const char *statusFrom(JsonObjectConst in, Status *out) {
  Status &s = *out;
  memset(&s, 0, sizeof(s));
  if (const char *e = header(in)) return e;
  if (!optDate(in["asof"], &s.asof)) return "asof";
  const char *fetched, *state, *err;
  if (!optStr(in["fetched"], &fetched) || strlen(fetched) >= kFetchedLen) return "fetched";
  int si;
  if (!str(in["state"], &state) || (si = keyIndex(state, kAppStateKeys, A_COUNT)) < 0) return "state";
  if (!optStr(in["err"], &err)) return "err";
  JsonObjectConst syms = in["symbols"].as<JsonObjectConst>();
  if (!in["symbols"].isNull() && syms.isNull()) return "symbols";
  for (JsonPairConst kv : syms) {
    JsonObjectConst o = kv.value().as<JsonObjectConst>();
    SymStatus e;
    memset(&e, 0, sizeof(e));
    const char *src = "";
    bool ok = !o.isNull() && validSymbol(kv.key().c_str()) && optDate(o["asof"], &e.asof) &&
              (o["bars"].isNull() || u32(o["bars"], 0xFFFFFFFFUL, &e.bars)) && optNum(o["ter"], &e.ter, &e.hasTer) &&
              optStr(o["terSrc"], &src);
    for (uint8_t i = 0; ok && i < s.n; i++) ok = strcmp(s.s[i].sym, kv.key().c_str()) != 0;
    if (!ok || s.n >= kMaxSymbols) { countDrop(&s.dropped); continue; }
    copyText(e.sym, sizeof(e.sym), kv.key().c_str());
    copyText(e.terSrc, sizeof(e.terSrc), src);
    s.s[s.n++] = e;
  }
  copyText(s.fetched, sizeof(s.fetched), fetched);
  copyText(s.err, sizeof(s.err), err);
  upper(s.err);
  s.state = (uint8_t)si;
  s.have  = true;
  return nullptr;
}

// ── ingest ──────────────────────────────────────────────────────────────────
static Symbol *findSymbol(Symbol *list, uint8_t n, const char *sym) {
  for (uint8_t i = 0; i < n; i++)
    if (!strcmp(list[i].sym, sym)) return &list[i];
  return nullptr;
}

void setWatch(Model &m, Spare &sp, bool tickers, const char *const *syms, const char *const *names, uint8_t n) {
  Symbol *list = tickers ? m.tk : m.idx;
  uint8_t &count = tickers ? m.nTk : m.nIdx;
  if (n > kMaxWatch) n = kMaxWatch;
  // Position k takes the old slot of syms[k] if it is still ahead (positions
  // before k are final), else starts empty; one swap through the spare.
  for (uint8_t k = 0; k < n; k++) {
    int j = -1;
    for (uint8_t i = k; i < kMaxWatch; i++)
      if (list[i].sym[0] && !strcmp(list[i].sym, syms[k])) { j = i; break; }
    if (j < 0) {
      // Nothing to keep: whatever sits here belongs to a later position or to
      // nobody. Move it out of the way only if a later symbol still wants it.
      bool wanted = false;
      for (uint8_t l = (uint8_t)(k + 1); l < n && !wanted; l++) wanted = list[k].sym[0] && !strcmp(list[k].sym, syms[l]);
      if (wanted) {
        for (uint8_t i = (uint8_t)(k + 1); i < kMaxWatch; i++) {
          bool taken = false;
          for (uint8_t l = 0; l < n && !taken; l++) taken = list[i].sym[0] && !strcmp(list[i].sym, syms[l]);
          if (!taken) { memcpy(&sp.sym, &list[i], sizeof(Symbol)); memcpy(&list[i], &list[k], sizeof(Symbol)); memcpy(&list[k], &sp.sym, sizeof(Symbol)); break; }
        }
      }
      memset(&list[k], 0, sizeof(Symbol));
      copyText(list[k].sym, sizeof(list[k].sym), syms[k]);
    } else if (j != k) {
      memcpy(&sp.sym, &list[k], sizeof(Symbol));
      memcpy(&list[k], &list[j], sizeof(Symbol));
      memcpy(&list[j], &sp.sym, sizeof(Symbol));
    }
    copyText(list[k].name, sizeof(list[k].name), (names && names[k] && names[k][0]) ? names[k] : syms[k]);
  }
  for (uint8_t k = n; k < kMaxWatch; k++) memset(&list[k], 0, sizeof(Symbol));
  count = n;
}

// leaf split at '/': up to three parts.
static int split(const char *leaf, char parts[3][kSymLen + 4]) {
  int n = 0;
  size_t k = 0;
  parts[0][0] = '\0';
  for (const char *p = leaf; ; p++) {
    if (*p == '/' || !*p) {
      parts[n][k] = '\0';
      n++;
      if (!*p || n == 3) return *p ? -1 : n;   // more than three parts: not ours
      k = 0;
      parts[n][0] = '\0';
    } else {
      if (k + 1 >= sizeof(parts[0])) return -1;
      parts[n][k++] = *p;
    }
  }
}

const char *ingest(Model &m, Spare &sp, const char *leaf, JsonObjectConst in, const char *ticker) {
  char part[3][kSymLen + 4];
  const int n = split(leaf, part);
  if (n < 1) return "unknown topic";
  const char *why;
  if (n == 1 && !strcmp(part[0], "status")) {
    if ((why = statusFrom(in, &sp.st))) return why;
    memcpy(&m.status, &sp.st, sizeof(Status));
  } else if (n == 1 && !strcmp(part[0], "live")) {
    if ((why = liveFrom(in, &sp.l))) return why;
    memcpy(&m.live, &sp.l, sizeof(Live));
  } else if (n == 1 && !strcmp(part[0], "tape")) {
    if ((why = tapeFrom(in, &sp.t))) return why;
    memcpy(&m.tape, &sp.t, sizeof(Tape));
  } else if (n == 2 && !strcmp(part[0], "intraday")) {
    if (!validSymbol(part[1])) return "unwatched symbol";
    // Only the ticker on screen: a late session of the previous one, or another
    // panel's, would draw under the wrong chart.
    if (!ticker || strcmp(part[1], ticker)) return "not the selected ticker";
    if ((why = intradayFrom(in, part[1], &sp.i))) return why;
    memcpy(&m.intra, &sp.i, sizeof(Intraday));
  } else if (n == 2 && !strcmp(part[0], "holdings")) {
    const int mode = modeIndex(part[1]);
    if (mode < 0) return "bad mode";
    if ((why = holdingsFrom(in, (uint8_t)mode, &sp.h))) return why;
    memcpy(&m.hd[mode], &sp.h, sizeof(Holdings));
  } else if (n == 3 && !strcmp(part[0], "portfolio")) {
    const int mode = modeIndex(part[1]), preset = presetIndex(part[2]);
    if (mode < 0) return "bad mode";
    if (preset < 0) return "bad preset";
    if ((why = portfolioFrom(in, (uint8_t)mode, (uint8_t)preset, &sp.p))) return why;
    memcpy(&m.pf[mode][preset], &sp.p, sizeof(Portfolio));
  } else if (n == 3 && (!strcmp(part[0], "index") || !strcmp(part[0], "ticker"))) {
    const bool tickers = part[0][0] == 't';
    const int preset = presetIndex(part[2]);
    if (preset < 0) return "bad preset";
    Symbol *slot = validSymbol(part[1]) ? findSymbol(tickers ? m.tk : m.idx, tickers ? m.nTk : m.nIdx, part[1]) : nullptr;
    if (!slot) return "unwatched symbol";
    if ((why = seriesFrom(in, part[1], (uint8_t)preset, &sp.s))) return why;
    memcpy(&slot->p[preset], &sp.s, sizeof(Series));
  } else {
    return "unknown topic";
  }
  m.gen++;
  return nullptr;
}

// ── the record ──────────────────────────────────────────────────────────────
uint32_t crc32(const uint8_t *p, size_t n, uint32_t seed) {
  uint32_t c = ~seed;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0u - (c & 1)));
  }
  return ~c;
}

struct RecordHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t pad;
  uint32_t size;
};

size_t recordSize() { return sizeof(RecordHeader) + sizeof(Model) + sizeof(uint32_t); }

size_t recordWrite(const Model &m, uint8_t *out, size_t cap) {
  const size_t n = recordSize();
  if (!out || cap < n) return 0;
  RecordHeader h;
  h.magic = kRecordMagic;
  h.version = kRecordVersion;
  h.pad = 0;
  h.size = (uint32_t)sizeof(Model);
  memcpy(out, &h, sizeof(h));
  memcpy(out + sizeof(h), &m, sizeof(Model));
  const uint32_t c = crc32(out, sizeof(h) + sizeof(Model));
  memcpy(out + sizeof(h) + sizeof(Model), &c, sizeof(c));
  return n;
}

const char *recordRead(const uint8_t *in, size_t len, Model *out) {
  if (!in || len != recordSize()) return "length";
  RecordHeader h;
  memcpy(&h, in, sizeof(h));
  if (h.magic != kRecordMagic) return "magic";
  if (h.version != kRecordVersion) return "version";
  if (h.size != sizeof(Model)) return "size";
  uint32_t c;
  memcpy(&c, in + sizeof(h) + sizeof(Model), sizeof(c));
  if (c != crc32(in, sizeof(h) + sizeof(Model))) return "crc";
  memcpy(out, in + sizeof(h), sizeof(Model));
  return nullptr;
}

}  // namespace market

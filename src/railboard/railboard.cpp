#include "railboard.h"

#if defined(RAILBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"   // Picopixel with a legible U
#include "../mqtt/mqtt_bus.h"
#include "uk_time.h"

#define RB_TOPIC_BASE MQTT_BASE "/railboard/" RB_CRS "/"
static const long RB_SCHEMA = 1;

// ── layout ──────────────────────────────────────────────────────────────────
// One 7 px grid of Picopixel lines, nine of them, which is exactly the panel's
// 64 px: header, list title, three services of two lines each, and a last line
// that stays empty until the data goes stale. One list per 64 px panel.
// tools/railboard/render.py reads every number here, so change them only here.
//
// A service takes two lines because 64 px holds about fifteen Picopixel
// capitals and LONDON WATERLOO alone is 62 px:
//
//     14:08 EXP 14:15     5        scheduled, status, platform
//     WATERLOO           SW        place, operator
//
// Picopixel draws from its baseline, 4 px below the top of a capital, so a line
// whose top is y is printed with the cursor at y + RB_ASCENT.
static const int16_t RB_W          = 64;   // one panel, one list
static const int16_t RB_ASCENT     = 4;
static const int16_t RB_PITCH      = 7;    // 5 px capitals, 2 px of air
static const int16_t RB_Y_HEAD     = 1;    // station, date, clock
static const int16_t RB_Y_TITLE    = 8;    // DEPARTURES / ARRIVALS
static const int16_t RB_Y_ROW0     = 15;   // first service
static const int16_t RB_Y_FOOT     = 57;   // DATA UPDATING, when it is
static const int16_t RB_X_STATUS   = 20;   // HH:MM is at most 17 px wide
static const int16_t RB_X_VALUE    = 32;   // diagnostics: after the longest label, UPDATED
static const int16_t RB_GAP        = 3;    // least air between two fields
// Two lists side by side meet at x = 64 with nothing between them: a platform
// 5 against the next list's 14:04 read as 514:04 in the first preview. Every
// list keeps its last three columns dark.
static const int16_t RB_GUTTER     = 3;
static const int16_t RB_ROWS_SMALL = 3;    // (57 - 15) / 14
// Large type is the built-in 5x7 font: 6 px a character, 8 px a line, ten
// characters to a panel. That leaves no room for an operator code or a status
// word, so a large row is time and platform over the place name; a delay shows
// the expected time (a late arrival, the actual one) in amber in place of the
// scheduled one, and a cancellation says CANC.
static const int16_t RB_L_ADV      = 6;
static const int16_t RB_L_PITCH    = 8;
static const int16_t RB_L_ROW0     = 15;
static const int16_t RB_ROWS_LARGE = 2;    // 15 + 2 * 16 = 47, clear of the last line
// A service whose time has passed by this much leaves the list even while no
// new data arrives, so an outage shows DATA UPDATING over trains still to come
// rather than over ones long gone. Matches grace_s in the Home Assistant package.
static const int16_t RB_GRACE_S    = 60;

// ── colour ──────────────────────────────────────────────────────────────────
// Black ground and warm white type. Amber is kept for what should catch the
// eye - the list title, a delay, stale data - and red for a cancellation.
static const uint8_t RB_COL_TEXT[3]  = {230, 226, 214};
static const uint8_t RB_COL_WHITE[3] = {255, 255, 255};
static const uint8_t RB_COL_AMBER[3] = {255, 150,   0};
static const uint8_t RB_COL_RED[3]   = {255,  36,  24};
static const uint8_t RB_COL_DIM[3]   = {120, 126, 132};

// ── words ───────────────────────────────────────────────────────────────────
// Status is Home Assistant's closed vocabulary, in this order on both ends.
enum RbStatus : uint8_t { RB_OK = 0, RB_LATE, RB_CANC, RB_NOREPORT, RB_ARRIVED, RB_STATUS_COUNT };
static const char *const RB_ST_KEYS[]  = {"ok", "late", "canc", "nr", "arr"};
// A service with no realtime report says nothing rather than ON TIME.
static const char *const RB_ST_WORDS[] = {"ON TIME", "EXP", "CANCELLED", "", "ARRIVED"};
static const char *const RB_CANC_LARGE = "CANC";
static const char *const RB_ARR_AT     = "ARR";   // ARR 14:06: arrived, late, at
static const char *const RB_TITLES[]   = {"DEPARTURES", "ARRIVALS"};
static const char *const RB_DIAG_DIR[] = {"DEP", "ARR"};
static const char *const RB_STALE      = "DATA UPDATING";
static const char *const RB_EMPTY      = "NO SERVICES";
static const char *const RB_DAYS[]     = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const RB_MONTHS[]   = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// ── data ────────────────────────────────────────────────────────────────────
#define RB_MAX_SVC  8     // the package sends at most 8 a list
#define RB_PLAT_LEN 4     // "10A"
#define RB_OP_LEN   4     // "SW", "BUS"
#define RB_NAME_LEN 25    // the package cuts names to 24 on a word boundary

struct RbService {
  uint32_t t;             // scheduled (advertised), UTC epoch seconds
  uint32_t x;             // expected, or actual once reported; 0 = none
  uint8_t  st;            // RbStatus
  uint8_t  d;             // minutes late, as Home Assistant counted them
  char     p[RB_PLAT_LEN];
  char     o[RB_OP_LEN];
  char     n[RB_NAME_LEN];
};

struct RbBoard {
  RbService s[RB_MAX_SVC];
  uint8_t   count;
  bool      have;
  uint32_t  ts;           // when Home Assistant fetched it, UTC epoch seconds
  uint32_t  rxMs;         // millis() when it reached the panel
  char      stn[20];
  char      rt[24];       // RTT systemStatus.realtimeNetworkRail
};

struct RbHaStatus {
  bool     have;
  uint32_t at;            // Home Assistant's last attempt, UTC epoch seconds
  uint32_t retry;         // Retry-After, seconds, on a rate limit
  uint16_t code;          // HTTP status, 0 when the request never completed
  char     err[8];        // "" | AUTH | RATE | HTTP | BAD | NET
  char     rl[12];        // X-RateLimit-Remaining-Day, as sent
};

struct RbConfig {
  uint8_t  panels;
  uint8_t  rows;
  uint8_t  level;
  bool     large;
  bool     diag;
  uint16_t switchS;
  uint16_t staleS;
};

enum : uint8_t { RB_DEP = 0, RB_ARR = 1 };

static RbBoard    s_board[2];
static RbBoard    s_scratch;          // parsed into first, so a bad payload never shows
static RbHaStatus s_ha;
static RbConfig   s_cfg = {RB_PANELS, RB_ROWS, RB_LEVEL, false, false, RB_SWITCH_S, RB_STALE_S};
static bool       s_diag     = false;
static uint8_t    s_flip     = 0;
static uint32_t   s_altSince = 0;
static uint16_t   s_refused  = 0;
static size_t     s_jsonPeak = 0;
static uint8_t    s_cfgFrom  = 0;     // 0 build defaults, 1 Home Assistant, 2 web portal

// ── JSON memory ─────────────────────────────────────────────────────────────
// Every allocation ArduinoJson makes for a payload comes from here: PSRAM first,
// and never more than RB_JSON_CAP in total. Internal SRAM bottoms out near
// 43 KB with the panel running and the HUB75 frame buffers live in it, so a
// parse does not get to borrow any while PSRAM is there. The cap is what makes a
// malformed or oversized payload harmless: past it deserializeJson() reports
// NoMemory, the payload is refused, and the board already on screen stays.
static const size_t RB_JSON_CAP = 8192;   // measured need and margin: README "Cost"

class RbJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override { return reallocate(nullptr, size); }

  void deallocate(void *ptr) override {
    if (!ptr) return;
    Hdr *h = static_cast<Hdr *>(ptr) - 1;
    used_ -= h->size;
    heap_caps_free(h);
  }

  void *reallocate(void *ptr, size_t size) override {
    Hdr *old = ptr ? static_cast<Hdr *>(ptr) - 1 : nullptr;
    const size_t was = old ? old->size : 0;
    if (used_ - was + size > RB_JSON_CAP) return nullptr;
    Hdr *h = static_cast<Hdr *>(heap_caps_realloc(old, sizeof(Hdr) + size,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!h) h = static_cast<Hdr *>(heap_caps_realloc(old, sizeof(Hdr) + size, MALLOC_CAP_8BIT));
    if (!h) return nullptr;
    h->size = size;
    used_   = used_ - was + size;
    if (used_ > peak_) peak_ = used_;
    return h + 1;
  }

  size_t peak() const { return peak_; }

 private:
  // The size lives in front of each block. Sized like max_align_t so what is
  // handed back keeps the alignment the heap itself guarantees.
  union Hdr { size_t size; max_align_t align; };
  size_t used_ = 0;
  size_t peak_ = 0;
};

static RbJsonAllocator s_alloc;

// ── parsing ─────────────────────────────────────────────────────────────────
static void copyUpper(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (src) {
    for (; src[i] && i + 1 < cap; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  }
  dst[i] = '\0';
}

static uint8_t parseStatus(const char *s) {
  for (uint8_t i = 0; i < RB_STATUS_COUNT; i++) {
    if (!strcmp(s, RB_ST_KEYS[i])) return i;
  }
  return RB_NOREPORT;   // an unknown word claims nothing
}

static long clampL(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void noteJson() {
  if (s_alloc.peak() > s_jsonPeak) s_jsonPeak = s_alloc.peak();
}

static bool ingestBoard(uint8_t which, const char *json, uint16_t len) {
  JsonDocument doc(&s_alloc);
  const DeserializationError err = deserializeJson(doc, json, len);
  noteJson();
  if (err) {
    Serial.printf("[railboard] %s: %s\n", RB_TITLES[which], err.c_str());
    return false;
  }
  if ((doc["v"] | 0L) != RB_SCHEMA) return false;
  if (strcmp(doc["dir"] | "", which == RB_DEP ? "dep" : "arr")) return false;
  JsonArrayConst list = doc["s"].as<JsonArrayConst>();
  if (list.isNull()) return false;

  RbBoard &b = s_scratch;
  memset(&b, 0, sizeof(b));
  for (JsonObjectConst o : list) {
    if (b.count >= RB_MAX_SVC) break;
    const uint32_t t = o["t"] | 0UL;
    if (!t) continue;
    RbService &sv = b.s[b.count++];
    sv.t  = t;
    sv.x  = o["x"] | 0UL;
    sv.st = parseStatus(o["st"] | "");
    sv.d  = (uint8_t)clampL(o["d"] | 0L, 0, 255);
    copyUpper(sv.p, sizeof(sv.p), o["p"] | "");
    copyUpper(sv.o, sizeof(sv.o), o["o"] | "");
    copyUpper(sv.n, sizeof(sv.n), o["n"] | "");
  }
  b.ts   = doc["ts"] | 0UL;
  b.rxMs = millis();
  b.have = true;
  copyUpper(b.stn, sizeof(b.stn), doc["stn"] | "");
  copyUpper(b.rt,  sizeof(b.rt),  doc["rt"]  | "");
  // Parse and render both run on the loop task, so there is no torn read; the
  // scratch copy is what keeps the last good board when a payload is refused.
  s_board[which] = b;
  return true;
}

static bool ingestStatus(const char *json, uint16_t len) {
  JsonDocument doc(&s_alloc);
  const bool bad = deserializeJson(doc, json, len) != DeserializationError::Ok;
  noteJson();
  if (bad || (doc["v"] | 0L) != RB_SCHEMA) return false;
  s_ha.at    = doc["at"] | 0UL;
  s_ha.retry = doc["retry"] | 0UL;
  s_ha.code  = (uint16_t)clampL(doc["code"] | 0L, 0, 999);
  copyUpper(s_ha.err, sizeof(s_ha.err), doc["err"] | "");
  copyUpper(s_ha.rl,  sizeof(s_ha.rl),  doc["rl"]  | "");
  s_ha.have  = true;
  return true;
}

// Bounds are what the page can draw, not recommendations.
static bool ingestConfig(const char *json, uint16_t len) {
  JsonDocument doc(&s_alloc);
  const bool bad = deserializeJson(doc, json, len) != DeserializationError::Ok;
  noteJson();
  if (bad || (doc["v"] | 0L) != RB_SCHEMA) return false;
  RbConfig c = s_cfg;
  c.panels  = (uint8_t)clampL(doc["panels"] | (long)c.panels, 1, 2);
  c.rows    = (uint8_t)clampL(doc["rows"] | (long)c.rows, 1, RB_ROWS_SMALL);
  c.level   = (uint8_t)clampL(doc["level"] | (long)c.level, 10, 100);
  c.switchS = (uint16_t)clampL(doc["switch_s"] | (long)c.switchS, 3, 600);
  c.staleS  = (uint16_t)clampL(doc["stale_s"] | (long)c.staleS, 30, 3600);
  c.large   = !strcmp(doc["font"] | (c.large ? "large" : "small"), "large");
  c.diag    = doc["diag"] | c.diag;
  s_cfg = c;
  return true;
}

bool railboardIngest(const char *topic, const char *payload, uint16_t len) {
  static const size_t baseLen = sizeof(RB_TOPIC_BASE) - 1;
  if (!topic || strncmp(topic, RB_TOPIC_BASE, baseLen)) return false;
  if (!payload || !len) return false;   // a cleared retained topic: keep what is shown
  const char *leaf = topic + baseLen;
  bool ok;
  if      (!strcmp(leaf, "departures")) ok = ingestBoard(RB_DEP, payload, len);
  else if (!strcmp(leaf, "arrivals"))   ok = ingestBoard(RB_ARR, payload, len);
  else if (!strcmp(leaf, "status"))     ok = ingestStatus(payload, len);
  else if (!strcmp(leaf, "config"))     { ok = ingestConfig(payload, len); if (ok) s_cfgFrom = 1; }
  else return false;
  if (!ok) {
    s_refused++;
    Serial.printf("[railboard] refused %s, %u B\n", leaf, len);
  }
  return ok;
}

static void onMessage(const char *topic, const uint8_t *payload, uint16_t len) {
  railboardIngest(topic, (const char *)payload, len);
}

void railboardBegin() {
  const bool routed = mqttBusOnMessage(RB_TOPIC_BASE, onMessage);
  const bool subbed = mqttBusSubscribe(RB_TOPIC_BASE "+");
  if (!routed || !subbed) {
    Serial.printf("[railboard] MQTT bus is full: handler %s, subscription %s\n",
                  routed ? "ok" : "REFUSED", subbed ? "ok" : "REFUSED");
  }
  s_altSince = millis();
}

void railboardPress() { s_diag = !s_diag; }

// Off means off: a diag:true from Home Assistant's config is cleared too, until
// that config arrives again.
void railboardSetDiag(bool on) {
  s_diag = on;
  if (!on) s_cfg.diag = false;
}

bool railboardApplyConfig(const char *json, uint16_t len) {
  if (!json || !len || !ingestConfig(json, len)) return false;
  s_cfgFrom = 2;
  return true;
}

void railboardTurn(int8_t delta) {
  (void)delta;              // two lists: either direction means "the other one"
  s_flip ^= 1;
  s_altSince = millis();    // and it gets a full turn from now
}

// ── drawing ─────────────────────────────────────────────────────────────────
static uint16_t col(const uint8_t c[3]) {
  const uint16_t lv = s_cfg.level;
  return display.color565((uint8_t)(c[0] * lv / 100), (uint8_t)(c[1] * lv / 100),
                          (uint8_t)(c[2] * lv / 100));
}

// Lit width in small type, as GFX measures it.
static int16_t textW(const char *s) {
  if (!s || !*s) return 0;
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return (int16_t)bw;
}

// Lit width in large type: 6 px a character, the last column of the cell blank.
static int16_t largeW(const char *s) {
  const size_t n = strlen(s);
  return n ? (int16_t)(n * RB_L_ADV - 1) : 0;
}

static void put(int16_t x, int16_t top, const char *s, uint16_t c) {
  display.setTextColor(c);
  display.setCursor(x, top + RB_ASCENT);
  display.print(s);
}

// `right` is exclusive: the last lit column is right - 1.
static void putRight(int16_t right, int16_t top, const char *s, uint16_t c) {
  put(right - textW(s), top, s, c);
}

static void putLarge(int16_t x, int16_t top, const char *s, uint16_t c) {
  display.setFont(NULL);                // the built-in font positions from the top
  display.setTextColor(c);
  display.setCursor(x, top);
  display.print(s);
  display.setFont(&PicopixelFB);
}

static void hhmm(char *out, size_t n, uint32_t utc) {
  const uktime::Civil c = uktime::london(utc);
  snprintf(out, n, "%02u:%02u", (unsigned)c.hour, (unsigned)c.minute);
}

// Fit a place name into `room` pixels, whole words only: a cut word reads as a
// typo rather than as an abbreviation, which the flight board found on live
// data. A London terminus loses LONDON first, but only when one word is left -
// WATERLOO is unambiguous, ROAD (GUILDFORD) is not. Then trailing words go,
// with any & or short connector they leave dangling. A single word that still
// does not fit is the one case that gets cut, since a blank is worse.
static void fitName(char *out, size_t cap, const char *name, int16_t room, bool large) {
  strncpy(out, name, cap - 1);
  out[cap - 1] = '\0';
  auto width = [large](const char *s) { return large ? largeW(s) : textW(s); };
  if (width(out) <= room) return;
  if (!strncmp(out, "LONDON ", 7) && out[7] && !strchr(out + 7, ' ')) {
    memmove(out, out + 7, strlen(out + 7) + 1);
    if (width(out) <= room) return;
  }
  while (width(out) > room) {
    char *sp = strrchr(out, ' ');
    if (!sp) {
      size_t n = strlen(out);
      while (n > 1 && width(out) > room) out[--n] = '\0';
      return;
    }
    *sp = '\0';
    char *tail = strrchr(out, ' ');
    if (tail && (strlen(tail + 1) <= 2 || !strcmp(tail + 1, "AND"))) *tail = '\0';
  }
}

static bool isLate(const RbService &sv) {
  return sv.st == RB_LATE || (sv.st == RB_ARRIVED && sv.d > 0);
}

// A second time worth printing: when a late train is expected, or when a late
// one actually arrived.
static bool hasTime(const RbService &sv) {
  return sv.x && isLate(sv);
}

static void drawServiceSmall(int16_t x0, int16_t top, const RbService &sv) {
  char buf[RB_NAME_LEN + 8];
  const bool canc = sv.st == RB_CANC;
  const uint16_t body = col(canc ? RB_COL_RED : RB_COL_TEXT);
  const int16_t right = x0 + RB_W - RB_GUTTER;
  const int16_t xPlat = (!canc && sv.p[0]) ? right - textW(sv.p) : right;

  hhmm(buf, sizeof(buf), sv.t);
  put(x0, top, buf, body);
  if (hasTime(sv)) {
    char at[8];
    hhmm(at, sizeof(at), sv.x);
    snprintf(buf, sizeof(buf), "%s %s", sv.st == RB_LATE ? RB_ST_WORDS[RB_LATE] : RB_ARR_AT, at);
    // A wide platform (10A) beside a wide time leaves no air: the word goes, the time stays.
    if (x0 + RB_X_STATUS + textW(buf) + RB_GAP > xPlat) snprintf(buf, sizeof(buf), "%s", at);
  } else {
    snprintf(buf, sizeof(buf), "%s", RB_ST_WORDS[sv.st < RB_STATUS_COUNT ? sv.st : RB_NOREPORT]);
  }
  put(x0 + RB_X_STATUS, top, buf, canc ? body : col(isLate(sv) ? RB_COL_AMBER : RB_COL_TEXT));
  if (xPlat < right) put(xPlat, top, sv.p, col(RB_COL_WHITE));

  const int16_t top2 = top + RB_PITCH;
  int16_t room = RB_W - RB_GUTTER;
  if (sv.o[0]) {
    putRight(right, top2, sv.o, col(RB_COL_DIM));
    room -= textW(sv.o) + RB_GAP;
  }
  fitName(buf, sizeof(buf), sv.n, room, false);
  put(x0, top2, buf, body);
}

static void drawServiceLarge(int16_t x0, int16_t top, const RbService &sv) {
  char buf[RB_NAME_LEN + 8];
  const bool canc = sv.st == RB_CANC;
  const uint16_t c1 = col(canc ? RB_COL_RED : (isLate(sv) ? RB_COL_AMBER : RB_COL_TEXT));
  const int16_t right = x0 + RB_W - RB_GUTTER;

  hhmm(buf, sizeof(buf), hasTime(sv) ? sv.x : sv.t);
  putLarge(x0, top, buf, c1);
  if (canc)          putLarge(right - largeW(RB_CANC_LARGE), top, RB_CANC_LARGE, c1);
  else if (sv.p[0])  putLarge(right - largeW(sv.p), top, sv.p, col(RB_COL_WHITE));

  fitName(buf, sizeof(buf), sv.n, RB_W - RB_GUTTER, true);
  putLarge(x0, top + RB_L_PITCH, buf, col(canc ? RB_COL_RED : RB_COL_TEXT));
}

// Stale when Home Assistant's own timestamp is older than stale_s. Without a
// clock that cannot be shown either way, so it counts as stale too.
static bool isStale(const RbBoard &b, time_t now, bool synced) {
  if (!b.have || !synced || !b.ts) return true;
  const int64_t age = (int64_t)now - (int64_t)b.ts;
  if (age < -300) return (millis() - b.rxMs) / 1000UL > s_cfg.staleS;  // clocks disagree
  return age > (int64_t)s_cfg.staleS;
}

// Past its time by more than the grace, judged by our own clock.
static bool isGone(const RbService &sv, time_t now, bool synced) {
  if (!synced) return false;
  const int64_t when = sv.x ? sv.x : sv.t;
  const int64_t grace = sv.st == RB_ARRIVED ? 2 * RB_GRACE_S : RB_GRACE_S;
  return (int64_t)now > when + grace;
}

static const char *stationName() {
  if (s_board[RB_DEP].have && s_board[RB_DEP].stn[0]) return s_board[RB_DEP].stn;
  if (s_board[RB_ARR].have && s_board[RB_ARR].stn[0]) return s_board[RB_ARR].stn;
  return RB_STATION;
}

static void drawList(int16_t x0, uint8_t which, time_t now, bool synced) {
  const RbBoard &b = s_board[which];
  if (!b.have) {
    put(x0, RB_Y_ROW0, mqttBusStatus(), col(RB_COL_DIM));   // NO WIFI, CONNECTING, ...
    put(x0, RB_Y_FOOT, RB_STALE, col(RB_COL_AMBER));
    return;
  }
  const bool stale = isStale(b, now, synced);
  if (b.count == 0 && !stale) put(x0, RB_Y_ROW0, RB_EMPTY, col(RB_COL_DIM));

  const uint8_t cap  = s_cfg.large ? RB_ROWS_LARGE : RB_ROWS_SMALL;
  const uint8_t rows = s_cfg.rows < cap ? s_cfg.rows : cap;
  uint8_t shown = 0;
  for (uint8_t i = 0; i < b.count && shown < rows; i++) {
    if (isGone(b.s[i], now, synced)) continue;
    if (s_cfg.large) drawServiceLarge(x0, RB_L_ROW0 + shown * 2 * RB_L_PITCH, b.s[i]);
    else             drawServiceSmall(x0, RB_Y_ROW0 + shown * 2 * RB_PITCH, b.s[i]);
    shown++;
  }
  if (stale) put(x0, RB_Y_FOOT, RB_STALE, col(RB_COL_AMBER));
}

static void drawHeaderWide(const uktime::Civil &c, bool synced) {
  char clk[12], date[16], stn[20];
  if (synced) {
    snprintf(clk, sizeof(clk), "%02u:%02u:%02u", (unsigned)c.hour, (unsigned)c.minute, (unsigned)c.second);
    snprintf(date, sizeof(date), "%s %u %s", RB_DAYS[c.wday], (unsigned)c.day, RB_MONTHS[c.month - 1]);
  } else {
    strcpy(clk, "--:--:--");
    date[0] = '\0';
  }
  const int16_t xClk = 2 * RB_W - textW(clk);
  put(xClk, RB_Y_HEAD, clk, col(RB_COL_WHITE));
  const int16_t xDate = xClk - 2 * RB_GAP - textW(date);
  fitName(stn, sizeof(stn), stationName(), xClk - RB_GAP, false);
  put(0, RB_Y_HEAD, stn, col(RB_COL_WHITE));
  if (date[0] && xDate >= textW(stn) + 2 * RB_GAP) put(xDate, RB_Y_HEAD, date, col(RB_COL_DIM));
  put(0,    RB_Y_TITLE, RB_TITLES[RB_DEP], col(RB_COL_AMBER));
  put(RB_W, RB_Y_TITLE, RB_TITLES[RB_ARR], col(RB_COL_AMBER));
}

static void drawHeaderNarrow(uint8_t which, const uktime::Civil &c, bool synced) {
  char clk[8], date[12], stn[20];
  if (synced) {
    snprintf(clk, sizeof(clk), "%02u:%02u", (unsigned)c.hour, (unsigned)c.minute);
    snprintf(date, sizeof(date), "%u %s", (unsigned)c.day, RB_MONTHS[c.month - 1]);
  } else {
    strcpy(clk, "--:--");
    date[0] = '\0';
  }
  putRight(RB_W, RB_Y_HEAD, clk, col(RB_COL_WHITE));
  fitName(stn, sizeof(stn), stationName(), RB_W - textW(clk) - RB_GAP, false);
  put(0, RB_Y_HEAD, stn, col(RB_COL_WHITE));
  put(0, RB_Y_TITLE, RB_TITLES[which], col(RB_COL_AMBER));
  if (date[0] && textW(RB_TITLES[which]) + RB_GAP + textW(date) <= RB_W)
    putRight(RB_W, RB_Y_TITLE, date, col(RB_COL_DIM));
}

static void diagLine(uint8_t line, const char *label, const char *value, uint16_t valueCol,
                     const char *right) {
  const int16_t top = RB_Y_HEAD + line * RB_PITCH;
  put(0, top, label, col(RB_COL_DIM));
  int16_t end = RB_X_VALUE;
  if (value && *value) {
    put(RB_X_VALUE, top, value, valueCol);
    end += textW(value);
  }
  // The right-hand note is the one that gives way when a long value needs the room.
  if (right && *right && 2 * RB_W - textW(right) >= end + RB_GAP)
    putRight(2 * RB_W, top, right, col(RB_COL_DIM));
}

// An age as a reader wants it, with a space so an S never passes for a 5 -
// 607S AGO read as 6075 AGO in the first preview.
static void ago(char *out, size_t n, const char *prefix, uint32_t s) {
  if (s < 120)       snprintf(out, n, "%s%lu S AGO",   prefix, (unsigned long)s);
  else if (s < 7200) snprintf(out, n, "%s%lu MIN AGO", prefix, (unsigned long)(s / 60));
  else               snprintf(out, n, "%s%lu H AGO",   prefix, (unsigned long)(s / 3600));
}

static void drawDiag(time_t now, bool synced) {
  char v[40], r[24];
  const uint16_t white = col(RB_COL_WHITE);

  put(0, RB_Y_HEAD, "RAIL BOARD " RB_CRS, col(RB_COL_AMBER));
  putRight(2 * RB_W, RB_Y_HEAD, "DIAGNOSTICS", col(RB_COL_DIM));

  // The spec's one hard requirement for this view: when did data last arrive.
  const uint32_t last = s_board[RB_DEP].ts > s_board[RB_ARR].ts ? s_board[RB_DEP].ts : s_board[RB_ARR].ts;
  if (last) {
    const uktime::Civil c = uktime::london(last);
    snprintf(v, sizeof(v), "%02u:%02u:%02u %s", (unsigned)c.hour, (unsigned)c.minute,
             (unsigned)c.second, c.bst ? "BST" : "GMT");
    if (synced && (int64_t)now >= (int64_t)last) ago(r, sizeof(r), "", (uint32_t)(now - last));
    else r[0] = '\0';
  } else {
    strcpy(v, "NEVER");
    r[0] = '\0';
  }
  diagLine(1, "UPDATED", v, white, r);

  for (uint8_t d = 0; d < 2; d++) {
    const RbBoard &b = s_board[d];
    if (b.have) {
      snprintf(v, sizeof(v), "%u SERVICES", (unsigned)b.count);
      ago(r, sizeof(r), "RX ", (millis() - b.rxMs) / 1000UL);
    } else {
      strcpy(v, "NOTHING YET");
      r[0] = '\0';
    }
    diagLine(2 + d, RB_DIAG_DIR[d], v, white, r);
  }

  if (!s_ha.have) {
    diagLine(4, "HA", "NO STATUS", col(RB_COL_DIM), "");
  } else {
    const bool ok = !s_ha.err[0];
    if (ok)                 snprintf(v, sizeof(v), "OK %u", (unsigned)s_ha.code);
    else if (s_ha.retry)    snprintf(v, sizeof(v), "%s %u RETRY %lu S", s_ha.err, (unsigned)s_ha.code, (unsigned long)s_ha.retry);
    else if (s_ha.code)     snprintf(v, sizeof(v), "%s HTTP %u", s_ha.err, (unsigned)s_ha.code);
    else                    snprintf(v, sizeof(v), "%s", s_ha.err);
    if (s_ha.at) hhmm(r, sizeof(r), s_ha.at); else r[0] = '\0';
    diagLine(4, "HA", v, col(ok ? RB_COL_TEXT : (strcmp(s_ha.err, "RATE") ? RB_COL_RED : RB_COL_AMBER)), r);
  }

  const char *rt = s_board[RB_DEP].rt[0] ? s_board[RB_DEP].rt : s_board[RB_ARR].rt;
  if (!strncmp(rt, "REALTIME_DATA_", 14)) rt += 14;   // LIMITED, NONE
  if (s_ha.rl[0]) snprintf(r, sizeof(r), "LEFT TODAY %s", s_ha.rl);
  else            r[0] = '\0';
  diagLine(5, "RTT", rt[0] ? rt : "-", col(strcmp(rt, "OK") ? RB_COL_AMBER : RB_COL_TEXT), r);

  snprintf(r, sizeof(r), "REFUSED %u", (unsigned)s_refused);
  diagLine(6, "MQTT", mqttBusConnected() ? "CONNECTED" : mqttBusStatus(),
           col(mqttBusConnected() ? RB_COL_TEXT : RB_COL_AMBER), r);

  snprintf(v, sizeof(v), "%.1fK MIN %.1fK", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
  snprintf(r, sizeof(r), "JSON %.1fK", s_jsonPeak / 1024.0);
  diagLine(7, "HEAP", v, white, r);

  if (synced) {
    const uktime::Civil c = uktime::london((int64_t)now);
    snprintf(v, sizeof(v), "%s  NTP OK", c.bst ? "BST" : "GMT");
  } else {
    strcpy(v, "NO NTP YET");
  }
  diagLine(8, "LONDON", v, col(synced ? RB_COL_TEXT : RB_COL_AMBER), "");
}

void railboardRender() {
  const time_t now    = time(nullptr);
  const bool   synced = now > 1700000000;   // before NTP the clock reads 1970; same test as the world clock
  const uktime::Civil c = uktime::london((int64_t)now);

  display.setFont(&PicopixelFB);
  display.setTextSize(1);
  display.setTextWrap(false);

  if (s_diag || s_cfg.diag) {
    drawDiag(now, synced);
  } else if (s_cfg.panels >= 2) {
    drawHeaderWide(c, synced);
    drawList(0,    RB_DEP, now, synced);
    drawList(RB_W, RB_ARR, now, synced);
  } else {
    const uint32_t turns = (millis() - s_altSince) / (s_cfg.switchS * 1000UL);
    const uint8_t which  = (uint8_t)((turns + s_flip) % 2);
    drawHeaderNarrow(which, c, synced);
    drawList(0, which, now, synced);
  }

  display.setFont(NULL);   // other pages draw with the built-in font and would inherit this one
}

void railboardStatusJson(JsonObject out) {
  const time_t now    = time(nullptr);
  const bool   synced = now > 1700000000;
  out["crs"]     = RB_CRS;
  out["station"] = stationName();
  out["synced"]  = synced;
  out["now"]     = synced ? (uint32_t)now : 0;
  out["diag"]    = s_diag || s_cfg.diag;
  out["diagKnob"] = s_diag;
  out["diagCfg"]  = s_cfg.diag;

  JsonObject cfg = out["cfg"].to<JsonObject>();
  cfg["panels"]   = s_cfg.panels;
  cfg["rows"]     = s_cfg.rows;
  cfg["font"]     = s_cfg.large ? "large" : "small";
  cfg["level"]    = s_cfg.level;
  cfg["switch_s"] = s_cfg.switchS;
  cfg["stale_s"]  = s_cfg.staleS;
  cfg["from"]     = s_cfgFrom == 1 ? "ha" : (s_cfgFrom == 2 ? "web" : "build");

  static const char *const keys[] = {"dep", "arr"};
  for (uint8_t d = 0; d < 2; d++) {
    const RbBoard &b = s_board[d];
    JsonObject l = out[keys[d]].to<JsonObject>();
    l["have"]  = b.have;
    l["stale"] = isStale(b, now, synced);
    if (!b.have) continue;
    l["count"] = b.count;
    l["ts"]    = b.ts;
    l["rx"]    = (millis() - b.rxMs) / 1000UL;   // seconds since it reached the panel
    l["rt"]    = (const char *)b.rt;
  }

  JsonObject ha = out["ha"].to<JsonObject>();
  ha["have"] = s_ha.have;
  if (s_ha.have) {
    ha["at"]    = s_ha.at;
    ha["code"]  = s_ha.code;
    ha["err"]   = (const char *)s_ha.err;
    ha["retry"] = s_ha.retry;
    ha["left"]  = (const char *)s_ha.rl;
  }
  out["refused"]  = s_refused;
  out["jsonPeak"] = (uint32_t)s_jsonPeak;
}

#endif  // RAILBOARD_ENABLED

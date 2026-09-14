#include "railboard.h"

#if defined(RAILBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"   // Picopixel with a legible U
#include "../mqtt/mqtt_bus.h"
#include "uk_time.h"

#define RB_TOPIC_ROOT   MQTT_BASE "/railboard/"
#define RB_TOPIC_SELECT RB_TOPIC_ROOT "select"
static const long RB_SCHEMA = 1;

// A build-time default that is not three characters would never match a topic.
static_assert(sizeof(RB_CRS) == 4, "RB_CRS must be a three-letter station code");

// ── layout ──────────────────────────────────────────────────────────────────
// A UK station screen, from the owner's photograph of one: one list on the
// whole 128 x 64, a white title and white column headings, amber rows in mixed
// case, and a large amber clock at the foot. When the services do not fit, the
// list pages; page 2 starts with a Continued...... row.
// tools/railboard/render.py reads every number and word here, so change them only here.
//
//     Departures           Plat Expt      Arrivals        Time Plat Expt
//     Time Destination      Guildford     From                 Guildford
//     14:08 London Waterloo   5 On time   Redhill        14:04    2 14:06
//     14:12 Portsmouth        3 14:19     Reading        14:14      Cancelled
//     ...                                 ...
//     Page 1 of 2               14:05:14  Page 1 of 1              14:05:14
//
// Two faces. The title and the clock are the built-in 5x7 font (7 px capitals,
// positioned from the top). Everything else is Picopixel (5 px capitals, 3 px
// lower case, 1 px descenders), which draws from its baseline, 4 px below the
// top of a capital: a line whose top is y is printed with the cursor at
// y + RB_ASCENT.
static const int16_t RB_ASCENT     = 4;
static const int16_t RB_PITCH      = 7;    // 5 px capitals, a 1 px descender, 1 px of air
static const int16_t RB_BIG_ADV    = 6;    // built-in font: 6 px a character
static const int16_t RB_X_LEFT     = 2;    // the flight board's margins
static const int16_t RB_X_RIGHT    = 126;  // exclusive: the last lit column is 125
static const int16_t RB_GAP        = 3;    // least air between two fields
static const int16_t RB_Y_TITLE    = 0;    // big: capitals on rows 0-6, p's tail on 7
static const int16_t RB_Y_HEAD1    = 2;    // small, on the title's baseline (row 6): Plat Expt
static const int16_t RB_Y_HEAD2    = 9;    // small: Time Destination / From
static const int16_t RB_Y_ROW0     = 16;
static const int16_t RB_ROWS_PAGE  = 6;    // 16 + 5 * 7 = 51, its descenders on row 56
static const int16_t RB_Y_CLOCK    = 57;   // big: rows 57-63, clear of that descender
static const int16_t RB_Y_FOOT     = 58;   // small: Page 1 of 2, its g's tail on row 63
// Columns. Expt is left-aligned, and Cancelled, its widest word (33 px), ends
// on the last lit column. Plat is right-aligned a gap short of it; on arrivals
// Time is right-aligned a gap short of Plat's heading.
static const int16_t RB_X_DEST     = 22;   // departures: HH:MM is at most 17 px, then 3 px of air
static const int16_t RB_X_EXPT     = 93;
static const int16_t RB_X_PLAT_R   = 89;   // exclusive right edge of Plat
static const int16_t RB_X_TIME_R   = 72;   // arrivals: exclusive right edge of Time
static const int16_t RB_X_VALUE    = 34;   // diagnostics: after the longest label, UPDATED
// A service whose time has passed by this much leaves the list even while no
// new data arrives, so an outage shows Data updating over trains still to come
// rather than over ones long gone. Matches grace_s in the Home Assistant package.
static const int16_t RB_GRACE_S    = 60;
// A page that has not been drawn for this long is being opened again: it
// starts on departures rather than wherever the alternation happened to be.
static const int16_t RB_REOPEN_MS  = 2000;

// ── colour ──────────────────────────────────────────────────────────────────
// Black ground with nothing lit behind the text. White for the headings, amber
// for every row and the clock - 255,150,0 rather than the photograph's
// 255,170,0, which on these panels leans yellow - and red for Cancelled.
static const uint8_t RB_COL_WHITE[3] = {255, 255, 255};
static const uint8_t RB_COL_AMBER[3] = {255, 150,   0};
static const uint8_t RB_COL_RED[3]   = {255,  36,  24};
static const uint8_t RB_COL_DIM[3]   = {120, 126, 132};
static const uint8_t RB_COL_TEXT[3]  = {230, 226, 214};   // diagnostics values

// ── words ───────────────────────────────────────────────────────────────────
// Status is Home Assistant's closed vocabulary, in this order on both ends.
enum RbStatus : uint8_t { RB_OK = 0, RB_LATE, RB_CANC, RB_NOREPORT, RB_ARRIVED, RB_STATUS_COUNT };
static const char *const RB_ST_KEYS[]  = {"ok", "late", "canc", "nr", "arr"};
// Expt shows the expected time for a late train, and the actual time for one
// that arrived late. A service with no realtime report says nothing rather
// than On time.
static const char *const RB_ST_WORDS[] = {"On time", "", "Cancelled", "", "Arrived"};
static const char *const RB_TITLES[]   = {"Departures", "Arrivals"};
static const char *const RB_H_PLAT     = "Plat";
static const char *const RB_H_EXPT     = "Expt";
static const char *const RB_H_TIME     = "Time";
static const char *const RB_H_DEST     = "Destination";
static const char *const RB_H_FROM     = "From";
static const char *const RB_CONTINUED  = "Continued......";
static const char *const RB_PAGE       = "Page";
static const char *const RB_OF         = "of";
static const char *const RB_STALE      = "Data updating";
static const char *const RB_EMPTY      = "No services";
static const char *const RB_WAITING    = "Waiting for";
static const char *const RB_DIAG_DIR[] = {"DEP", "ARR"};

// ── data ────────────────────────────────────────────────────────────────────
#define RB_MAX_SVC  8     // the package sends at most 8 a list
#define RB_PLAT_LEN 4     // "10A"
#define RB_OP_LEN   4     // "SW", "BUS"
#define RB_NAME_LEN 25    // the package cuts names to 24 on a word boundary
#define RB_STN_LEN  32    // RTT's description, e.g. London Road (Guildford)

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
  char      stn[RB_STN_LEN];
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
  uint8_t  rows;
  uint8_t  level;
  bool     diag;
  uint16_t switchS;
  uint16_t staleS;
};

enum : uint8_t { RB_DEP = 0, RB_ARR = 1 };
// What the knob chose. AUTO is the alternation.
enum RbView : uint8_t { RB_VIEW_AUTO = 0, RB_VIEW_DEP, RB_VIEW_ARR, RB_VIEW_DIAG };

static RbBoard    s_board[2];
static RbBoard    s_scratch;          // parsed into first, so a bad payload never shows
static RbHaStatus s_ha;
static RbConfig   s_cfg = {RB_ROWS, RB_LEVEL, false, RB_SWITCH_S, RB_STALE_S};
static uint16_t   s_refused  = 0;
static size_t     s_jsonPeak = 0;
static uint8_t    s_cfgFrom  = 0;     // 0 build defaults, 1 Home Assistant, 2 web portal

static char       s_crs[4]       = RB_CRS;
static char       s_sub[48]      = "";      // the subscription in force, "" = none
static bool       s_begun        = false;
static bool       s_selectDirty  = true;    // the retained selection still has to go out
static bool       s_wasConnected = false;

static uint8_t    s_view       = RB_VIEW_AUTO;
static uint32_t   s_viewAt     = 0;
static bool       s_diagPinned = false;     // the portal's switch
static uint32_t   s_altSince   = 0;
static uint8_t    s_autoFirst  = RB_DEP;    // the list the alternation starts from
static uint32_t   s_lastFrame  = 0;

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
static void copyText(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (src) {
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
  }
  dst[i] = '\0';
}

static void copyUpper(char *dst, size_t cap, const char *src) {
  copyText(dst, cap, src);
  for (char *c = dst; *c; c++) *c = (char)toupper((unsigned char)*c);
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
  const char *crs = doc["crs"] | "";
  if (crs[0] && strcmp(crs, s_crs)) return false;   // on our topic, but for another station
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
    copyText(sv.p, sizeof(sv.p), o["p"] | "");
    copyText(sv.o, sizeof(sv.o), o["o"] | "");
    copyText(sv.n, sizeof(sv.n), o["n"] | "");   // mixed case, as the board prints it
  }
  b.ts   = doc["ts"] | 0UL;
  b.rxMs = millis();
  b.have = true;
  copyText(b.stn, sizeof(b.stn), doc["stn"] | "");
  copyUpper(b.rt, sizeof(b.rt), doc["rt"] | "");
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

// Bounds are what the page can draw, not recommendations. "panels" and "font"
// from the earlier two-list layout are ignored: the station screen has one list
// and one set of type sizes.
static bool ingestConfig(const char *json, uint16_t len) {
  JsonDocument doc(&s_alloc);
  const bool bad = deserializeJson(doc, json, len) != DeserializationError::Ok;
  noteJson();
  if (bad || (doc["v"] | 0L) != RB_SCHEMA) return false;
  RbConfig c = s_cfg;
  c.rows    = (uint8_t)clampL(doc["rows"] | (long)c.rows, 1, RB_MAX_SVC);
  c.level   = (uint8_t)clampL(doc["level"] | (long)c.level, 10, 100);
  c.switchS = (uint16_t)clampL(doc["switch_s"] | (long)c.switchS, 3, 600);
  c.staleS  = (uint16_t)clampL(doc["stale_s"] | (long)c.staleS, 30, 3600);
  c.diag    = doc["diag"] | c.diag;
  s_cfg = c;
  return true;
}

bool railboardIngest(const char *topic, const char *payload, uint16_t len) {
  static const size_t rootLen = sizeof(RB_TOPIC_ROOT) - 1;
  if (!topic || strncmp(topic, RB_TOPIC_ROOT, rootLen)) return false;
  const char *rest = topic + rootLen;                    // "GLD/departures"
  // Another station's: still in flight from just before a change of station.
  if (strncmp(rest, s_crs, 3) || rest[3] != '/') return false;
  if (!payload || !len) return false;   // a cleared retained topic: keep what is shown
  const char *leaf = rest + 4;
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

// ── station ─────────────────────────────────────────────────────────────────
bool railboardValidStation(const char *crs) {
  if (!crs) return false;
  for (uint8_t i = 0; i < 3; i++) {
    if (crs[i] < 'A' || crs[i] > 'Z') return false;
  }
  return crs[3] == '\0';
}

const char *railboardStation() { return s_crs; }

// One subscription whatever the station: unsubscribe the old one first, so a
// change never costs a slot of mqtt_bus's six.
static void subscribeStation() {
  char want[sizeof(s_sub)];
  snprintf(want, sizeof(want), RB_TOPIC_ROOT "%s/+", s_crs);
  if (!strcmp(want, s_sub)) return;
  if (s_sub[0]) mqttBusUnsubscribe(s_sub);
  if (mqttBusSubscribe(want)) {
    strcpy(s_sub, want);
  } else {
    s_sub[0] = '\0';
    Serial.printf("[railboard] MQTT bus refused the subscription to %s\n", want);
  }
}

bool railboardSetStation(const char *crs) {
  if (!railboardValidStation(crs)) return false;
  if (!strcmp(crs, s_crs)) return true;
  memcpy(s_crs, crs, sizeof(s_crs));
  // The old station's boards must not sit under the new station's name. The
  // new one's retained boards, if Home Assistant ever fetched it, arrive within
  // a moment of subscribing; otherwise the page says it is waiting.
  memset(s_board, 0, sizeof(s_board));
  memset(&s_ha, 0, sizeof(s_ha));
  s_selectDirty = true;
  s_altSince    = millis();
  s_autoFirst   = RB_DEP;
  if (s_begun) subscribeStation();
  return true;
}

void railboardBegin() {
  // The prefix without the station, so a change of station needs no new handler.
  if (!mqttBusOnMessage(RB_TOPIC_ROOT, onMessage))
    Serial.println("[railboard] MQTT bus is full: handler REFUSED");
  s_begun = true;
  subscribeStation();
  s_altSince = millis();
}

// Retained, so Home Assistant learns the station after its own restart as well
// as at the moment it changes. Sent again on every reconnect: the broker may
// have lost it, and a repeat of the same value changes nothing.
void railboardLoop() {
  const bool up = mqttBusConnected();
  if (up && !s_wasConnected) s_selectDirty = true;
  s_wasConnected = up;
  if (!up || !s_selectDirty) return;
  char body[20];
  snprintf(body, sizeof(body), "{\"crs\":\"%s\"}", s_crs);
  if (mqttBusPublish(RB_TOPIC_SELECT, body, true)) s_selectDirty = false;
}

// ── views ───────────────────────────────────────────────────────────────────
static uint8_t currentView(uint32_t nowMs) {
  if (s_view != RB_VIEW_AUTO && nowMs - s_viewAt >= (uint32_t)RB_HOLD_S * 1000UL) {
    // The hold is over: alternate again, starting from the list that was chosen.
    s_autoFirst = (s_view == RB_VIEW_ARR) ? RB_ARR : RB_DEP;
    s_altSince  = nowMs;
    s_view      = RB_VIEW_AUTO;
  }
  if (s_view != RB_VIEW_AUTO) return s_view;
  if (s_diagPinned || s_cfg.diag) return RB_VIEW_DIAG;
  const uint32_t turns = (nowMs - s_altSince) / ((uint32_t)s_cfg.switchS * 1000UL);
  return ((turns + s_autoFirst) % 2) ? RB_VIEW_ARR : RB_VIEW_DEP;
}

// How far into the current list's turn we are: page 1 has the first half,
// page 2 the second. A list the knob chose pages on the same rhythm.
static uint32_t turnElapsedMs(uint32_t nowMs) {
  const uint32_t turn = (uint32_t)s_cfg.switchS * 1000UL;
  return (nowMs - (s_view != RB_VIEW_AUTO ? s_viewAt : s_altSince)) % turn;
}

void railboardKnob(int8_t delta) {
  static const uint8_t order[] = {RB_VIEW_DEP, RB_VIEW_ARR, RB_VIEW_DIAG};
  const uint32_t nowMs = millis();
  const uint8_t cur = currentView(nowMs);
  uint8_t i = (cur == RB_VIEW_DEP) ? 0 : (cur == RB_VIEW_ARR ? 1 : 2);
  i = (uint8_t)((i + (delta > 0 ? 1 : 2)) % 3);
  s_view   = order[i];
  s_viewAt = nowMs;
  // Turning off diagnostics with the knob turns it off, wherever it was pinned:
  // otherwise the knob could never leave it.
  if (s_view != RB_VIEW_DIAG) {
    s_diagPinned = false;
    s_cfg.diag   = false;
  }
}

// Off means off: a diag:true from Home Assistant's config is cleared too, until
// that config arrives again.
void railboardSetDiag(bool on) {
  s_diagPinned = on;
  if (!on) s_cfg.diag = false;
  s_view = RB_VIEW_AUTO;          // the portal's choice wins over a knob hold
}

bool railboardApplyConfig(const char *json, uint16_t len) {
  if (!json || !len || !ingestConfig(json, len)) return false;
  s_cfgFrom = 2;
  return true;
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

// Lit width in big type: 6 px a character, the last column of the cell blank.
static int16_t bigW(const char *s) {
  const size_t n = strlen(s);
  return n ? (int16_t)(n * RB_BIG_ADV - 1) : 0;
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

static void putBig(int16_t x, int16_t top, const char *s, uint16_t c) {
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

// Fit a place name into `room` pixels. Whole letters always; whole words where
// a word boundary allows, because a cut word reads as a typo rather than as an
// abbreviation - the flight board found that on live data (EUROAIRPOR). So:
// the whole name; for a London terminus the name without London, when one word
// is left (Waterloo is unambiguous, Road (Guildford) is not); the name less
// trailing words and any connector they leave dangling; and only when even the
// first word does not fit, that word cut to as many letters as fit.
static void fitName(char *out, size_t cap, const char *name, int16_t room) {
  copyText(out, cap, name);
  if (textW(out) <= room) return;
  if (!strncasecmp(out, "London ", 7) && out[7] && !strchr(out + 7, ' ')) {
    memmove(out, out + 7, strlen(out + 7) + 1);
    if (textW(out) <= room) return;
  }
  char words[RB_STN_LEN];
  copyText(words, sizeof(words), out);
  while (words[0] && textW(words) > room) {
    char *sp = strrchr(words, ' ');
    if (!sp) { words[0] = '\0'; break; }
    *sp = '\0';
    char *tail = strrchr(words, ' ');
    if (tail && (strlen(tail + 1) <= 2 || !strcasecmp(tail + 1, "and"))) *tail = '\0';
  }
  if (words[0]) { copyText(out, cap, words); return; }
  char *sp = strchr(out, ' ');
  if (sp) *sp = '\0';
  size_t n = strlen(out);
  while (n > 1 && textW(out) > room) out[--n] = '\0';
}

static bool isLate(const RbService &sv) {
  return sv.st == RB_LATE || (sv.st == RB_ARRIVED && sv.d > 0);
}

// Expt shows a time instead of a word: a late train's expected time, or a late
// arrival's actual one.
static bool hasTime(const RbService &sv) {
  return sv.x && isLate(sv);
}

static void drawHeadings(uint8_t which) {
  const uint16_t white = col(RB_COL_WHITE);
  putBig(RB_X_LEFT, RB_Y_TITLE, RB_TITLES[which], white);
  if (which == RB_ARR) putRight(RB_X_TIME_R, RB_Y_HEAD1, RB_H_TIME, white);
  putRight(RB_X_PLAT_R, RB_Y_HEAD1, RB_H_PLAT, white);
  put(RB_X_EXPT, RB_Y_HEAD1, RB_H_EXPT, white);

  int16_t used;
  if (which == RB_DEP) {
    put(RB_X_LEFT, RB_Y_HEAD2, RB_H_TIME, white);
    put(RB_X_DEST, RB_Y_HEAD2, RB_H_DEST, white);
    used = RB_X_DEST + textW(RB_H_DEST);
  } else {
    put(RB_X_LEFT, RB_Y_HEAD2, RB_H_FROM, white);
    used = RB_X_LEFT + textW(RB_H_FROM);
  }
  // The station, right-aligned on the second heading line and dim, so it reads
  // as a label rather than as a column heading. The payload names it; until one
  // has, its code.
  const char *stn = s_board[RB_DEP].stn[0] ? s_board[RB_DEP].stn
                  : (s_board[RB_ARR].stn[0] ? s_board[RB_ARR].stn : s_crs);
  char fit[RB_STN_LEN];
  fitName(fit, sizeof(fit), stn, RB_X_RIGHT - used - 2 * RB_GAP);
  putRight(RB_X_RIGHT, RB_Y_HEAD2, fit, col(RB_COL_DIM));
}

static void drawService(uint8_t which, int16_t top, const RbService &sv) {
  char tm[8], expt[12], name[RB_STN_LEN];
  const bool canc = sv.st == RB_CANC;
  const uint16_t amber = col(RB_COL_AMBER);

  if (hasTime(sv)) hhmm(expt, sizeof(expt), sv.x);
  else             copyText(expt, sizeof(expt), RB_ST_WORDS[sv.st < RB_STATUS_COUNT ? sv.st : RB_NOREPORT]);
  put(RB_X_EXPT, top, expt, canc ? col(RB_COL_RED) : amber);

  // A cancelled train has no platform worth showing.
  int16_t platLeft = RB_X_PLAT_R;
  if (!canc && sv.p[0]) {
    platLeft = RB_X_PLAT_R - textW(sv.p);
    put(platLeft, top, sv.p, amber);
  }

  hhmm(tm, sizeof(tm), sv.t);
  if (which == RB_DEP) {
    put(RB_X_LEFT, top, tm, amber);
    fitName(name, sizeof(name), sv.n, platLeft - RB_GAP - RB_X_DEST);
    put(RB_X_DEST, top, name, amber);
  } else {
    const int16_t xTime = RB_X_TIME_R - textW(tm);
    put(xTime, top, tm, amber);
    fitName(name, sizeof(name), sv.n, xTime - RB_GAP - RB_X_LEFT);
    put(RB_X_LEFT, top, name, amber);
  }
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

static void drawBoard(uint8_t which, time_t now, bool synced, uint32_t nowMs) {
  const RbBoard &b = s_board[which];
  const uint16_t amber = col(RB_COL_AMBER);
  char foot[24];

  if (!b.have) {
    char why[32];
    if (mqttBusConnected()) snprintf(why, sizeof(why), "%s %s", RB_WAITING, s_crs);
    else                    snprintf(why, sizeof(why), "%s", mqttBusStatus());   // NO WIFI, CONNECTING, ...
    put(RB_X_LEFT, RB_Y_ROW0, why, amber);
    put(RB_X_LEFT, RB_Y_FOOT, RB_STALE, amber);
    return;
  }

  // The services still to come, up to the configured count.
  uint8_t idx[RB_MAX_SVC], n = 0;
  for (uint8_t i = 0; i < b.count && n < s_cfg.rows; i++)
    if (!isGone(b.s[i], now, synced)) idx[n++] = i;

  // Page 2, when there is one, gives its first line to Continued......
  const uint8_t pages = n > RB_ROWS_PAGE ? 2 : 1;
  const uint8_t page  = (pages > 1 && turnElapsedMs(nowMs) >= (uint32_t)s_cfg.switchS * 500UL) ? 1 : 0;
  const bool stale = isStale(b, now, synced);

  if (n == 0) {
    if (!stale) put(RB_X_LEFT, RB_Y_ROW0, RB_EMPTY, amber);
  } else if (page == 0) {
    for (uint8_t r = 0; r < n && r < RB_ROWS_PAGE; r++)
      drawService(which, RB_Y_ROW0 + r * RB_PITCH, b.s[idx[r]]);
  } else {
    put(RB_X_LEFT, RB_Y_ROW0, RB_CONTINUED, amber);
    for (uint8_t r = 1, k = RB_ROWS_PAGE; k < n && r < RB_ROWS_PAGE; r++, k++)
      drawService(which, RB_Y_ROW0 + r * RB_PITCH, b.s[idx[k]]);
  }

  if (stale) snprintf(foot, sizeof(foot), "%s", RB_STALE);
  else       snprintf(foot, sizeof(foot), "%s %u %s %u", RB_PAGE, (unsigned)(page + 1), RB_OF, (unsigned)pages);
  put(RB_X_LEFT, RB_Y_FOOT, foot, amber);
}

static void drawClock(const uktime::Civil &c, bool synced) {
  char clk[12];
  if (synced) snprintf(clk, sizeof(clk), "%02u:%02u:%02u", (unsigned)c.hour, (unsigned)c.minute, (unsigned)c.second);
  else        strcpy(clk, "--:--:--");
  putBig(RB_X_RIGHT - bigW(clk), RB_Y_CLOCK, clk, col(RB_COL_AMBER));
}

static void diagLine(uint8_t line, const char *label, const char *value, uint16_t valueCol,
                     const char *right) {
  const int16_t top = 1 + line * RB_PITCH;
  put(RB_X_LEFT, top, label, col(RB_COL_DIM));
  int16_t end = RB_X_VALUE;
  if (value && *value) {
    put(RB_X_VALUE, top, value, valueCol);
    end += textW(value);
  }
  // The right-hand note is the one that gives way when a long value needs the room.
  if (right && *right && RB_X_RIGHT - textW(right) >= end + RB_GAP)
    putRight(RB_X_RIGHT, top, right, col(RB_COL_DIM));
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

  snprintf(v, sizeof(v), "RAIL BOARD %s", s_crs);
  put(RB_X_LEFT, 1, v, col(RB_COL_AMBER));
  putRight(RB_X_RIGHT, 1, "DIAGNOSTICS", col(RB_COL_DIM));

  // The one hard requirement for this view: when did data last arrive.
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

  // The selection Home Assistant follows, and whether it has gone out.
  snprintf(v, sizeof(v), "%s %s", s_crs, s_selectDirty ? "NOT SENT" : "SENT");
  diagLine(8, "SELECT", v, col(s_selectDirty ? RB_COL_AMBER : RB_COL_TEXT), synced ? "NTP OK" : "NO NTP");
}

void railboardRender() {
  const time_t   now    = time(nullptr);
  const bool     synced = now > 1700000000;   // before NTP the clock reads 1970; same test as the world clock
  const uint32_t nowMs  = millis();

  if (s_view == RB_VIEW_AUTO && nowMs - s_lastFrame > (uint32_t)RB_REOPEN_MS) {
    s_altSince  = nowMs;
    s_autoFirst = RB_DEP;
  }
  s_lastFrame = nowMs;
  const uint8_t view = currentView(nowMs);

  display.setFont(&PicopixelFB);
  display.setTextSize(1);
  display.setTextWrap(false);

  if (view == RB_VIEW_DIAG) {
    drawDiag(now, synced);
  } else {
    const uint8_t which = (view == RB_VIEW_ARR) ? RB_ARR : RB_DEP;
    drawHeadings(which);
    drawBoard(which, now, synced, nowMs);
    drawClock(uktime::london((int64_t)now), synced);
  }

  display.setFont(NULL);   // other pages draw with the built-in font and would inherit this one
}

void railboardStatusJson(JsonObject out) {
  const time_t   now    = time(nullptr);
  const bool     synced = now > 1700000000;
  const uint32_t nowMs  = millis();
  static const char *const VIEWS[] = {"auto", "dep", "arr", "diag"};
  const uint8_t view = currentView(nowMs);
  const char *stn = s_board[RB_DEP].stn[0] ? s_board[RB_DEP].stn
                  : (s_board[RB_ARR].stn[0] ? s_board[RB_ARR].stn : "");

  out["crs"]     = (const char *)s_crs;
  out["station"] = stn[0] ? stn : (const char *)s_crs;
  out["named"]   = stn[0] != '\0';               // false: no payload has named it yet
  out["synced"]  = synced;
  out["now"]     = synced ? (uint32_t)now : 0;
  out["list"]    = VIEWS[view];                  // what the page shows: dep, arr or diag
  out["knob"]    = VIEWS[s_view];                // what the knob chose; auto = alternating
  if (s_view != RB_VIEW_AUTO) out["holdS"] = RB_HOLD_S - (nowMs - s_viewAt) / 1000UL;
  out["diag"]    = s_diagPinned || s_cfg.diag;   // pinned, by the portal or Home Assistant
  out["diagCfg"] = s_cfg.diag;

  JsonObject sel = out["select"].to<JsonObject>();
  sel["topic"] = RB_TOPIC_SELECT;
  sel["sent"]  = !s_selectDirty;

  JsonObject cfg = out["cfg"].to<JsonObject>();
  cfg["rows"]     = s_cfg.rows;
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

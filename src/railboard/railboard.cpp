#include "railboard.h"

#if defined(RAILBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"   // Picopixel with a legible U
#include "../mqtt/mqtt_bus.h"
#include "rb_model.h"
#include "rb_settings.h"
#include "uk_time.h"
#if defined(RAILBOARD_DIRECT_ENABLED)
#include "rtt_direct.h"
#endif

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
// Black ground with nothing lit behind the text. The rows, the headings and the
// "due soon" rows take their colours from the settings (rb_settings.h palette:
// amber rows, white headings, green due rows by default). These are fixed:
// red for Cancelled, and the diagnostics view's own.
static const uint8_t RB_COL_WHITE[3] = {255, 255, 255};
static const uint8_t RB_COL_AMBER[3] = {255, 150,   0};
static const uint8_t RB_COL_RED[3]   = {255,  36,  24};
static const uint8_t RB_COL_DIM[3]   = {120, 126, 132};
static const uint8_t RB_COL_TEXT[3]  = {230, 226, 214};   // diagnostics values

// ── words ───────────────────────────────────────────────────────────────────
// Status words on the wire, in RbStatus order (rb_model.h).
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
static const char *const RB_REFUSED    = "RTT token refused";
static const char *const RB_DIAG_DIR[] = {"DEP", "ARR"};

// ── data ────────────────────────────────────────────────────────────────────
// RbService, RbBoard and their sizes: rb_model.h, shared with the direct fetch.

struct RbHaStatus {
  bool     have;
  uint32_t at;            // Home Assistant's last attempt, UTC epoch seconds
  uint32_t retry;         // Retry-After, seconds, on a rate limit
  uint16_t code;          // HTTP status, 0 when the request never completed
  char     err[8];        // "" | AUTH | RATE | HTTP | BAD | NET
  char     rl[12];        // X-RateLimit-Remaining-Day, as sent
};

// Settings in force. The web portal's once it has saved any - they are then
// kept in NVS "rbcfg" and win over Home Assistant - else Home Assistant's
// .../config over the build defaults, else the build defaults. Home Assistant's
// config is still read and remembered while the portal's are in force, so
// "back to Home Assistant" needs no wait for it to publish again.
static const rbs::Settings RB_BUILD = rbs::defaults(RB_ROWS, RB_SWITCH_S, RB_LEVEL, RB_STALE_S);

// Where a list came from. Higher wins while it is fresh.
enum : uint8_t { RB_SRC_NONE = 0, RB_SRC_HA, RB_SRC_DIRECT };
// What the knob chose. AUTO is the alternation.
enum RbView : uint8_t { RB_VIEW_AUTO = 0, RB_VIEW_DEP, RB_VIEW_ARR, RB_VIEW_DIAG };

static RbBoard    s_board[2];
static uint32_t   s_lastRenderMs = 0;   // the page was drawn this recently
static RbBoard    s_scratch;          // parsed into first, so a bad payload never shows
static RbHaStatus s_ha;
static rbs::Settings s_cfg   = RB_BUILD;   // in force
static rbs::Settings s_haCfg = RB_BUILD;   // build defaults with Home Assistant's fields over them
static rbs::Settings s_web   = RB_BUILD;   // the portal's
static rbs::Settings s_saved = RB_BUILD;   // the portal's as NVS holds them
static bool       s_haHave     = false;
static bool       s_webSet     = false;
static bool       s_savedWeb   = false;     // NVS says the portal owns the settings
static uint32_t   s_webDirtyAt = 0;         // 0 = NVS has caught up
static bool       s_cfgDiag    = false;     // Home Assistant's diag; not a web setting
static uint16_t   s_refused  = 0;
static size_t     s_jsonPeak = 0;
static uint8_t    s_src[2]   = {RB_SRC_NONE, RB_SRC_NONE};
static uint16_t   s_shadowed = 0;     // Home Assistant boards set aside for a fresher direct one
#if defined(RAILBOARD_DIRECT_ENABLED)
static rtt::Lists *s_direct  = nullptr;   // PSRAM: where the fetch hands its lists over
#endif

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

static bool isStale(const RbBoard &b, time_t now, bool synced);

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
  // A board the panel fetched itself, while still fresh, wins: Home Assistant's
  // is the fallback. This one was well formed, so it counts as accepted.
  if (s_src[which] == RB_SRC_DIRECT) {
    const time_t now = time(nullptr);
    if (!isStale(s_board[which], now, now > 1700000000)) {
      s_shadowed++;
      return true;
    }
  }
  // Parse and render both run on the loop task, so there is no torn read; the
  // scratch copy is what keeps the last good board when a payload is refused.
  s_board[which] = b;
  s_src[which]   = RB_SRC_HA;
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
  rbs::Settings c = s_haCfg;
  c.rows    = (uint8_t)clampL(doc["rows"] | (long)c.rows, rbs::kRowsMin, rbs::kRowsMax);
  c.level   = (uint8_t)clampL(doc["level"] | (long)c.level, rbs::kLevelMin, rbs::kLevelMax);
  c.switchS = (uint16_t)clampL(doc["switch_s"] | (long)c.switchS, rbs::kSwitchMin, rbs::kSwitchMax);
  c.staleS  = (uint16_t)clampL(doc["stale_s"] | (long)c.staleS, rbs::kStaleMin, rbs::kStaleMax);
  s_cfgDiag = doc["diag"] | s_cfgDiag;
  s_haCfg   = c;
  s_haHave  = true;
  if (!s_webSet) s_cfg = s_haCfg;           // the portal's, once set, win
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
  else if (!strcmp(leaf, "config"))     ok = ingestConfig(payload, len);
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
  memset(s_src, 0, sizeof(s_src));
  s_selectDirty = true;
  s_altSince    = millis();
  s_autoFirst   = RB_DEP;
  if (s_begun) subscribeStation();
#if defined(RAILBOARD_DIRECT_ENABLED)
  rttDirectStationChanged();
#endif
  return true;
}

// ── web settings in NVS ─────────────────────────────────────────────────────
// Namespace "rbcfg": web (u8, 1 while the portal owns the settings), rows (u8),
// swS (u16), lvl (u8), stale (u16), dueMin (u8), clkSec (u8), and rowCol,
// headCol, dueCol (strings: palette names, so a reordered palette in a later
// build still reads the colour chosen). Written 2.5 s after the last change, as
// src/panel does, and only the fields that differ from what NVS holds.
static const char *const RB_NVS       = "rbcfg";
static const uint32_t    RB_SETTLE_MS = 2500;

static void loadWebSettings() {
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error on
  // every boot (src/panel/panel.cpp, arduino-esp32 2.0.17).
  if (!p.begin(RB_NVS, false)) return;
  if (p.getUChar("web", 0) == 1) {
    rbs::Settings s = RB_BUILD;
    s.rows         = p.getUChar("rows", s.rows);
    s.switchS      = p.getUShort("swS", s.switchS);
    s.level        = p.getUChar("lvl", s.level);
    s.staleS       = p.getUShort("stale", s.staleS);
    s.dueMin       = p.getUChar("dueMin", s.dueMin);
    s.clockSeconds = p.getUChar("clkSec", s.clockSeconds) != 0;
    static const char *const keys[] = {"rowCol", "headCol", "dueCol"};
    uint8_t *const dst[] = {&s.rowColour, &s.headColour, &s.dueColour};
    for (uint8_t i = 0; i < 3; i++) {
      char name[12];
      // isKey() first: getString() logs an error for a key never written.
      if (p.isKey(keys[i]) && p.getString(keys[i], name, sizeof(name))) {
        const int c = rbs::paletteIndex(name);
        if (c >= 0) *dst[i] = (uint8_t)c;
      }
    }
    if (rbs::valid(s)) {                      // a set from another build's bounds: ignored
      s_web = s_saved = s_cfg = s;
      s_webSet = s_savedWeb = true;
    }
  }
  p.end();
}

static void saveWebSettings() {
  if (!s_webDirtyAt || millis() - s_webDirtyAt < RB_SETTLE_MS) return;
  Preferences p;
  if (!p.begin(RB_NVS, false)) { s_webDirtyAt = millis() | 1; return; }   // try again after another settle
  s_webDirtyAt = 0;
  if (!s_webSet) {
    if (s_savedWeb && p.clear()) s_savedWeb = false;
    p.end();
    return;
  }
  const rbs::Settings &c = s_web;
  rbs::Settings &w = s_saved;
  const bool all = !s_savedWeb;               // nothing stored yet: every field
  bool ok = true;
  auto u8 = [&](const char *k, uint8_t v, uint8_t &was) {
    if (!all && v == was) return;
    if (p.putUChar(k, v)) was = v; else ok = false;
  };
  auto u16 = [&](const char *k, uint16_t v, uint16_t &was) {
    if (!all && v == was) return;
    if (p.putUShort(k, v)) was = v; else ok = false;
  };
  auto colour = [&](const char *k, uint8_t v, uint8_t &was) {
    if (!all && v == was) return;
    if (p.putString(k, rbs::kPalette[v].name)) was = v; else ok = false;
  };
  u8("rows", c.rows, w.rows);
  u16("swS", c.switchS, w.switchS);
  u8("lvl", c.level, w.level);
  u16("stale", c.staleS, w.staleS);
  u8("dueMin", c.dueMin, w.dueMin);
  uint8_t sec = w.clockSeconds;
  u8("clkSec", c.clockSeconds ? 1 : 0, sec);
  w.clockSeconds = sec != 0;
  colour("rowCol", c.rowColour, w.rowColour);
  colour("headCol", c.headColour, w.headColour);
  colour("dueCol", c.dueColour, w.dueColour);
  // Last, and only when every field went in: a half-written set is never taken.
  if (ok && !s_savedWeb && p.putUChar("web", 1)) s_savedWeb = true;
  if (!ok) s_webDirtyAt = millis() | 1;
  p.end();
}

rbs::Settings railboardSettings() { return s_cfg; }

void railboardSetSettings(const rbs::Settings &s) {
  if (!rbs::valid(s)) return;
  s_web    = s;
  s_cfg    = s;
  s_webSet = true;
  s_webDirtyAt = millis() | 1;
}

void railboardResetSettings() {
  s_webSet = false;
  s_cfg    = s_haHave ? s_haCfg : RB_BUILD;
  s_webDirtyAt = millis() | 1;
}

void railboardBegin() {
  loadWebSettings();
  // The prefix without the station, so a change of station needs no new handler.
  if (!mqttBusOnMessage(RB_TOPIC_ROOT, onMessage))
    Serial.println("[railboard] MQTT bus is full: handler REFUSED");
  s_begun = true;
  subscribeStation();
  s_altSince = millis();
#if defined(RAILBOARD_DIRECT_ENABLED)
  s_direct = static_cast<rtt::Lists *>(heap_caps_calloc(1, sizeof(rtt::Lists), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rttDirectBegin();
#endif
}

#if defined(RAILBOARD_DIRECT_ENABLED)
// One answer fills both lists at once, as Home Assistant's two payloads do.
static void applyDirect(const rtt::Lists &l, int64_t fetchedAt) {
  const uint32_t nowMs = millis();
  for (uint8_t side = RB_DEP; side <= RB_ARR; side++) {
    RbBoard &b = s_board[side];
    b      = l.board[side];
    b.ts   = (uint32_t)fetchedAt;
    b.rxMs = nowMs;
    b.have = true;
    memcpy(b.stn, l.stn, sizeof(b.stn));
    memcpy(b.rt, l.rt, sizeof(b.rt));
    s_src[side] = RB_SRC_DIRECT;
  }
}
#endif

// Something may bring data without Home Assistant: then "waiting" is the truth
// even while the broker is away.
static bool directArmed() {
#if defined(RAILBOARD_DIRECT_ENABLED)
  return rttDirectHasToken();
#else
  return false;
#endif
}

// Retained, so Home Assistant learns the station after its own restart as well
// as at the moment it changes. Sent again on every reconnect: the broker may
// have lost it, and a repeat of the same value changes nothing.
void railboardLoop() {
  saveWebSettings();
#if defined(RAILBOARD_DIRECT_ENABLED)
  int64_t fetchedAt = 0;
  rttDirectOnScreen(s_lastRenderMs && millis() - s_lastRenderMs < 3000UL);
  if (s_direct && rttDirectLoop(s_crs, s_direct, &fetchedAt)) applyDirect(*s_direct, fetchedAt);
#endif
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
  if (s_diagPinned || s_cfgDiag) return RB_VIEW_DIAG;
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
    s_cfgDiag    = false;
  }
}

// Off means off: a diag:true from Home Assistant's config is cleared too, until
// that config arrives again.
void railboardSetDiag(bool on) {
  s_diagPinned = on;
  if (!on) s_cfgDiag = false;
  s_view = RB_VIEW_AUTO;          // the portal's choice wins over a knob hold
}


// ── drawing ─────────────────────────────────────────────────────────────────
static uint16_t col(const uint8_t c[3]) {
  const uint16_t lv = s_cfg.level;
  return display.color565((uint8_t)(c[0] * lv / 100), (uint8_t)(c[1] * lv / 100),
                          (uint8_t)(c[2] * lv / 100));
}

// A palette colour (rb_settings.h) at this page's level.
static uint16_t pal(uint8_t i) {
  const rbs::Colour &p = rbs::kPalette[i < rbs::kPaletteCount ? i : 0];
  const uint8_t rgb[3] = {p.r, p.g, p.b};
  return col(rgb);
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
  const uint16_t head = pal(s_cfg.headColour);
  putBig(RB_X_LEFT, RB_Y_TITLE, RB_TITLES[which], head);
  if (which == RB_ARR) putRight(RB_X_TIME_R, RB_Y_HEAD1, RB_H_TIME, head);
  putRight(RB_X_PLAT_R, RB_Y_HEAD1, RB_H_PLAT, head);
  put(RB_X_EXPT, RB_Y_HEAD1, RB_H_EXPT, head);

  int16_t used;
  if (which == RB_DEP) {
    put(RB_X_LEFT, RB_Y_HEAD2, RB_H_TIME, head);
    put(RB_X_DEST, RB_Y_HEAD2, RB_H_DEST, head);
    used = RB_X_DEST + textW(RB_H_DEST);
  } else {
    put(RB_X_LEFT, RB_Y_HEAD2, RB_H_FROM, head);
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

// `now` is 0 without a clock, which turns "due soon" off.
static void drawService(uint8_t which, int16_t top, const RbService &sv, time_t now) {
  char tm[8], expt[12], name[RB_STN_LEN];
  const bool canc = sv.st == RB_CANC;
  const uint16_t amber = pal(rbs::dueSoon(sv, (int64_t)now, s_cfg.dueMin) ? s_cfg.dueColour : s_cfg.rowColour);

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
  const uint16_t amber = pal(s_cfg.rowColour);
  char foot[24];

  if (!b.have) {
    char why[32];
#if defined(RAILBOARD_DIRECT_ENABLED)
    if (rttDirectAuthRefused()) snprintf(why, sizeof(why), "%s", RB_REFUSED);
    else
#endif
    if (mqttBusConnected() || directArmed()) snprintf(why, sizeof(why), "%s %s", RB_WAITING, s_crs);
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
      drawService(which, RB_Y_ROW0 + r * RB_PITCH, b.s[idx[r]], synced ? now : 0);
  } else {
    put(RB_X_LEFT, RB_Y_ROW0, RB_CONTINUED, amber);
    for (uint8_t r = 1, k = RB_ROWS_PAGE; k < n && r < RB_ROWS_PAGE; r++, k++)
      drawService(which, RB_Y_ROW0 + r * RB_PITCH, b.s[idx[k]], synced ? now : 0);
  }

  if (stale) snprintf(foot, sizeof(foot), "%s", RB_STALE);
  else       snprintf(foot, sizeof(foot), "%s %u %s %u", RB_PAGE, (unsigned)(page + 1), RB_OF, (unsigned)pages);
  put(RB_X_LEFT, RB_Y_FOOT, foot, amber);
}

static void drawClock(const uktime::Civil &c, bool synced) {
  char clk[12];
  if (!synced)                snprintf(clk, sizeof(clk), "%s", s_cfg.clockSeconds ? "--:--:--" : "--:--");
  else if (s_cfg.clockSeconds) snprintf(clk, sizeof(clk), "%02u:%02u:%02u", (unsigned)c.hour, (unsigned)c.minute, (unsigned)c.second);
  else                        snprintf(clk, sizeof(clk), "%02u:%02u", (unsigned)c.hour, (unsigned)c.minute);
  putBig(RB_X_RIGHT - bigW(clk), RB_Y_CLOCK, clk, pal(s_cfg.rowColour));
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
#if defined(RAILBOARD_DIRECT_ENABLED)
  // The panel's own fetch and its last answer; RTT's realtime status after it.
  char quota[16];
  rttDirectLine(v, sizeof(v), quota, sizeof(quota));
  const bool directFine = !strncmp(v, "DIRECT OK", 9) || !strcmp(v, "HA ONLY");
  diagLine(5, "RTT", v, col(directFine ? RB_COL_TEXT : (rttDirectAuthRefused() ? RB_COL_RED : RB_COL_AMBER)), rt);
#else
  if (s_ha.rl[0]) snprintf(r, sizeof(r), "LEFT TODAY %s", s_ha.rl);
  else            r[0] = '\0';
  diagLine(5, "RTT", rt[0] ? rt : "-", col(strcmp(rt, "OK") ? RB_COL_AMBER : RB_COL_TEXT), r);
#endif

  snprintf(r, sizeof(r), "REFUSED %u", (unsigned)s_refused);
  diagLine(6, "MQTT", mqttBusConnected() ? "CONNECTED" : mqttBusStatus(),
           col(mqttBusConnected() ? RB_COL_TEXT : RB_COL_AMBER), r);

  snprintf(v, sizeof(v), "%.1fK MIN %.1fK", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
  snprintf(r, sizeof(r), "JSON %.1fK", s_jsonPeak / 1024.0);
  diagLine(7, "HEAP", v, white, r);

  // The selection Home Assistant follows, and whether it has gone out.
  snprintf(v, sizeof(v), "%s %s", s_crs, s_selectDirty ? "NOT SENT" : "SENT");
#if defined(RAILBOARD_DIRECT_ENABLED)
  // The day's remaining requests, which both fetchers spend: the panel's own
  // count when it has one, Home Assistant's otherwise.
  if (!synced)         snprintf(r, sizeof(r), "NO NTP");
  else if (quota[0])   snprintf(r, sizeof(r), "%s", quota);
  else if (s_ha.rl[0]) snprintf(r, sizeof(r), "LEFT %s", s_ha.rl);
  else                 snprintf(r, sizeof(r), "NTP OK");
  diagLine(8, "SELECT", v, col(s_selectDirty ? RB_COL_AMBER : RB_COL_TEXT), r);
#else
  diagLine(8, "SELECT", v, col(s_selectDirty ? RB_COL_AMBER : RB_COL_TEXT), synced ? "NTP OK" : "NO NTP");
#endif
}

void railboardRender() {
  const time_t   now    = time(nullptr);
  const bool     synced = now > 1700000000;   // before NTP the clock reads 1970; same test as the world clock
  const uint32_t nowMs  = millis();
  s_lastRenderMs = nowMs ? nowMs : 1;

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
  out["diag"]    = s_diagPinned || s_cfgDiag;    // pinned, by the portal or Home Assistant
  out["diagCfg"] = s_cfgDiag;

  JsonObject sel = out["select"].to<JsonObject>();
  sel["topic"] = RB_TOPIC_SELECT;
  sel["sent"]  = !s_selectDirty;

  JsonObject cfg = out["cfg"].to<JsonObject>();
  rbs::toJson(s_cfg, cfg);
  cfg["from"]   = s_webSet ? "web" : (s_haHave ? "ha" : "build");
  cfg["haHave"] = s_haHave;               // Home Assistant has sent a config since boot
  cfg["saved"]  = s_webDirtyAt == 0;      // NVS has caught up
  rbs::boundsJson(cfg["bounds"].to<JsonObject>());

  // The source on screen: the best one that is still fresh.
  static const char *const SOURCES[] = {"none", "ha", "direct"};
  uint8_t best = RB_SRC_NONE;
  for (uint8_t d = 0; d < 2; d++)
    if (s_board[d].have && !isStale(s_board[d], now, synced) && s_src[d] > best) best = s_src[d];
  out["source"]     = SOURCES[best];
  out["haShadowed"] = s_shadowed;
#if defined(RAILBOARD_DIRECT_ENABLED)
  rttDirectStatusJson(out["direct"].to<JsonObject>());
#else
  out["direct"]["built"] = false;
#endif

  static const char *const keys[] = {"dep", "arr"};
  for (uint8_t d = 0; d < 2; d++) {
    const RbBoard &b = s_board[d];
    JsonObject l = out[keys[d]].to<JsonObject>();
    l["have"]  = b.have;
    l["stale"] = isStale(b, now, synced);
    l["src"]   = SOURCES[s_src[d]];
    if (!b.have) continue;
    l["count"] = b.count;
    l["ts"]    = b.ts;
    l["rx"]    = (millis() - b.rxMs) / 1000UL;   // seconds since it reached the panel
    l["rt"]    = (const char *)b.rt;
    // The rows as the page lists them, for the portal's preview.
    JsonArray rows = l["rows"].to<JsonArray>();
    for (uint8_t i = 0; i < b.count; i++) {
      const RbService &sv = b.s[i];
      if (isGone(sv, now, synced)) continue;
      JsonObject o = rows.add<JsonObject>();
      o["t"]   = sv.t;
      o["x"]   = sv.x;
      o["p"]   = (const char *)sv.p;
      o["n"]   = (const char *)sv.n;
      o["st"]  = RB_ST_KEYS[sv.st < RB_STATUS_COUNT ? sv.st : RB_NOREPORT];
      o["due"] = synced && rbs::dueSoon(sv, (int64_t)now, s_cfg.dueMin);
    }
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

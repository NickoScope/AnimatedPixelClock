#include "flightboard.h"

#if defined(FLIGHTBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "../config/config.h"
#include "../display/display.h"
#include "../fonts/picopixel_fb.h"   // Picopixel with a legible U
#include "fb_mqtt.h"

// ── layout ──────────────────────────────────────────────────────────────────
// Picopixel, not TomThumb. Both are 3x5-class faces that fit 32-ish characters
// across 128 px, but TomThumb is monospaced at 3 px wide and there M and N
// differ by a single pixel - fine for a 3-letter code, unreadable once the
// column holds COPENHAGEN. Picopixel is proportional: M and W get 5 px, N gets
// 4, narrow letters stay at 3. The whole row costs 3 px more and one row of
// height, and the city names actually read.
//
// A GFX custom font positions from the BASELINE. Picopixel's glyphs sit 4 px
// above it, so a row whose top edge is at y is drawn with the cursor at
// y + FB_ASCENT.
static const int16_t FB_ASCENT   = 4;
static const int16_t FB_X_TIME   = 2;
static const int16_t FB_X_FLIGHT = 22;
static const int16_t FB_X_DEST   = 50;
static const int16_t FB_X_RIGHT  = 126;   // status is right-aligned to here
static const int16_t FB_GAP      = 2;     // minimum air between city and status
static const int16_t FB_CODE_GAP = 2;     // air between the IATA code and the city
static const int16_t FB_Y_HEADER = 1;
static const int16_t FB_Y_RULE   = 8;
static const int16_t FB_Y_ROW0   = 11;
static const int16_t FB_ROW_H    = 7;
static const uint8_t FB_VISIBLE  = 7;     // (64 - 11) / 7 = 7 whole rows

struct FbRow {
  char     fn[FB_FN_LEN];
  char     tm[FB_TM_LEN];
  char     ct[FB_CT_LEN];
  char     cy[FB_CY_LEN];
  FbStatus st;
};

// Both directions are kept, so the page can swap between them every
// FB_ALT_SECONDS without a round trip (owner, 2026-09-14: arrivals and
// departures, changing every 10 s).
struct FbBoard {
  FbRow    rows[FB_MAX_ROWS];
  uint8_t  count;
  uint8_t  nowIdx;
  char     upd[FB_TM_LEN];
  uint32_t stamp;
  bool     have;
};
static FbBoard s_b[2];            // [0] arrivals, [1] departures

static const uint32_t FB_ALT_MS = FB_ALT_SECONDS * 1000UL;
// A half fetched longer ago than this is worth asking for again. Retained
// boards arrive within milliseconds of subscribing however old they are: on the
// panel, 2026-09-14 18:30, departures came back stamped 09:34.
static const int16_t  FB_FRESH_MIN = 30;
static uint8_t  s_mode       = FB_DIR_ALT;
static uint32_t s_altT0      = 0;   // start of the current swap cycle
static uint32_t s_lastDrawMs = 0;

// Whitelist enforced on the HA side; kept in the same canonical order so the
// index means the same thing on both ends.
static const char *const FB_AIRPORTS[] = {"LFMD","LFMN","LFPG","EGLL","EDDF","EHAM"};
// Shown instead of the ICAO code. The list is closed and enforced on the Home
// Assistant side, so a table here is enough and costs no round trip.
static const char *const FB_AIRPORT_NAMES[] = {"CANNES","NICE","PARIS CDG",
                                               "LONDON","FRANKFURT","AMSTERDAM"};
static const uint8_t FB_AIRPORT_COUNT = sizeof(FB_AIRPORTS)/sizeof(FB_AIRPORTS[0]);
static uint8_t s_aptIdx = 1;   // LFMN

// Minutes since Home Assistant fetched this half, from its "upd" stamp and the
// local clock; -1 when either is unknown.
static int16_t updAgeMin(const FbBoard &b) {
  struct tm lt;
  if (!b.have || !getLocalTime(&lt, 0)) return -1;
  int h, m;
  if (sscanf(b.upd, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return (int16_t)(((lt.tm_hour * 60 + lt.tm_min) - (h * 60 + m) + 1440) % 1440);
}

static void clearBoards() {
  for (FbBoard &b : s_b) { b.have = false; b.count = 0; }
}

// Which half is on screen. A cycle starts on arrivals whenever the page comes
// back into view; with only one half received, that half stays up.
static bool shownDep() {
  if (s_mode != FB_DIR_ALT) return s_mode == FB_DIR_DEP;
  bool dep = (((millis() - s_altT0) / FB_ALT_MS) & 1) != 0;
  if (!s_b[dep].have && s_b[!dep].have) dep = !dep;
  return dep;
}

// The same state means different things depending on which way you are looking.
// For arrivals, "dep" means the aircraft has taken off and is on its way here.
static const char *statusWord(FbStatus st, bool departures) {
  switch (st) {
  // Widths matter here: every pixel this column takes is a pixel the city
  // name does not get. Measured against 30 live Nice rows, ENROUTE (29 px) and
  // DELAYED (28 px) alone cost four destinations their name, so those two are
  // shortened and the rest - the ones a passenger actually scans for - keep
  // their full spelling. See docs/09 for the measurement.
  case FB_SCHED: return departures ? "ON TIME" : "DUE";
  case FB_BOARD: return "GATE";
  case FB_DEP:   return departures ? "DEPART" : "IN AIR";
  case FB_LAND:  return "LANDED";
  case FB_DELAY: return "DELAY";
  case FB_CANC:  return "CANX";
  default:       return "";
  }
}

static FbStatus parseStatus(const char *s) {
  if (!s) return FB_UNKNOWN;
  if (!strcmp(s, "sched")) return FB_SCHED;
  if (!strcmp(s, "board")) return FB_BOARD;
  if (!strcmp(s, "dep"))   return FB_DEP;
  if (!strcmp(s, "land"))  return FB_LAND;
  if (!strcmp(s, "delay")) return FB_DELAY;
  if (!strcmp(s, "canc"))  return FB_CANC;
  return FB_UNKNOWN;
}

// Status carries meaning by colour, which frees the whole status column.
static uint16_t statusColor(FbStatus st) {
  switch (st) {
  case FB_SCHED: return display.color565(200, 200, 200);  // white
  case FB_BOARD: return display.color565(0,   220, 220);  // cyan
  case FB_DEP:   return display.color565(70,  120, 255);  // blue
  case FB_LAND:  return display.color565(0,   200,  60);  // green
  case FB_DELAY: return display.color565(255, 170,   0);  // amber
  case FB_CANC:  return display.color565(255,  40,  40);  // red
  default:       return display.color565(120, 120, 120);
  }
}

static void copyField(char *dst, size_t cap, const char *src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

bool flightboardIngest(const char *json, uint16_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;   // keep the old board

  JsonArrayConst arr = doc["f"].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  // A retained board for the airport just left can still be on its way when
  // the selection moves on; it must not land under the new name.
  const char *apt = doc["apt"] | "";
  if (apt[0] && strcmp(apt, FB_AIRPORTS[s_aptIdx])) return false;
  const char *dir = doc["dir"] | "";
  if (strcmp(dir, "arr") && strcmp(dir, "dep")) return false;
  FbBoard &b = s_b[strcmp(dir, "dep") == 0];

  uint8_t n = 0;
  for (JsonObjectConst f : arr) {
    if (n >= FB_MAX_ROWS) break;
    copyField(b.rows[n].fn, FB_FN_LEN, f["fn"] | "");
    copyField(b.rows[n].tm, FB_TM_LEN, f["tm"] | "--:--");
    copyField(b.rows[n].ct, FB_CT_LEN, f["ct"] | "");
    // Prefer the city name; fall back to the code when HA does not send one.
    copyField(b.rows[n].cy, FB_CY_LEN, f["cy"] | (const char *)(f["ct"] | ""));
    b.rows[n].st = parseStatus(f["st"] | "");
    n++;
  }
  b.count  = n;
  b.nowIdx = doc["now_idx"] | 0;
  if (b.nowIdx >= b.count && b.count) b.nowIdx = b.count - 1;
  copyField(b.upd, FB_TM_LEN, doc["upd"] | "--:--");
  b.stamp = millis();
  b.have  = true;
  return true;
}

bool flightboardHasData() { return s_b[0].have || s_b[1].have; }

uint32_t flightboardAge() {
  const FbBoard &b = s_b[shownDep()];
  return b.have ? (millis() - b.stamp) / 1000UL : 0;
}

const char *flightboardAirport() { return FB_AIRPORTS[s_aptIdx]; }

void flightboardStepAirport(int8_t delta) {
  int16_t i = (int16_t)s_aptIdx + delta;
  while (i < 0) i += FB_AIRPORT_COUNT;
  const uint8_t next = (uint8_t)(i % FB_AIRPORT_COUNT);
  if (next != s_aptIdx) { s_aptIdx = next; clearBoards(); }
}

uint8_t     flightboardAirportCount()      { return FB_AIRPORT_COUNT; }
uint8_t     flightboardAirportIndex()      { return s_aptIdx; }
FbDirMode   flightboardDirMode()           { return (FbDirMode)s_mode; }
const char *flightboardModeKey()           { return s_mode == FB_DIR_ARR ? "arr" : s_mode == FB_DIR_DEP ? "dep" : "alt"; }
bool        flightboardShowingDepartures() { return shownDep(); }
bool        flightboardWants(bool dep)     { return s_mode == FB_DIR_ALT || (s_mode == FB_DIR_DEP) == dep; }
// Without a clock the age is unknown; a board is then taken as it comes rather
// than paid for again.
bool flightboardHasFreshBoard(bool dep) {
  const int16_t age = updAgeMin(s_b[dep]);
  return s_b[dep].have && age <= FB_FRESH_MIN;
}
const char *flightboardAirportCode(uint8_t i)  { return i < FB_AIRPORT_COUNT ? FB_AIRPORTS[i] : ""; }
const char *flightboardAirportLabel(uint8_t i) { return i < FB_AIRPORT_COUNT ? FB_AIRPORT_NAMES[i] : ""; }

void flightboardSelect(uint8_t airport, FbDirMode mode) {
  if (airport < FB_AIRPORT_COUNT && airport != s_aptIdx) { s_aptIdx = airport; clearBoards(); }
  if (mode <= FB_DIR_ALT) s_mode = mode;
}

// The wire word for a status, the inverse of parseStatus().
static const char *statusKey(FbStatus st) {
  switch (st) {
  case FB_SCHED: return "sched";
  case FB_BOARD: return "board";
  case FB_DEP:   return "dep";
  case FB_LAND:  return "land";
  case FB_DELAY: return "delay";
  case FB_CANC:  return "canc";
  default:       return "";
  }
}

void flightboardStatusJson(JsonObject out) {
  const bool departures = shownDep();
  const FbBoard &b = s_b[departures];
  out["have"] = b.have;
  JsonObject sides = out["sides"].to<JsonObject>();
  for (uint8_t i = 0; i < 2; i++) {
    JsonObject s = sides[i ? "dep" : "arr"].to<JsonObject>();
    s["have"] = s_b[i].have;
    if (!s_b[i].have) continue;
    s["n"]   = s_b[i].count;
    s["upd"] = (const char *)s_b[i].upd;
    s["age"] = (millis() - s_b[i].stamp) / 1000UL;
  }
  if (!b.have) return;
  out["apt"]  = FB_AIRPORTS[s_aptIdx];
  out["name"] = FB_AIRPORT_NAMES[s_aptIdx];
  out["dir"]  = departures ? "dep" : "arr";
  out["upd"]  = (const char *)b.upd;
  out["age"]  = flightboardAge();
  out["now"]  = b.nowIdx;
  JsonArray rows = out["rows"].to<JsonArray>();
  for (uint8_t i = 0; i < b.count; i++) {
    const FbRow &r = b.rows[i];
    JsonObject o = rows.add<JsonObject>();
    o["tm"] = (const char *)r.tm;
    o["fn"] = (const char *)r.fn;
    o["ct"] = (const char *)r.ct;
    o["cy"] = (const char *)r.cy;
    o["st"] = statusKey(r.st);
    o["w"]  = statusWord(r.st, departures);   // the word the panel prints
  }
}

void flightboardRender() {
  // Back in view after a while elsewhere: start the cycle on arrivals.
  const uint32_t nowMs = millis();
  if (nowMs - s_lastDrawMs > 1500) s_altT0 = nowMs;
  s_lastDrawMs = nowMs;
  const bool departures = shownDep();
  const FbBoard &b = s_b[departures];

  display.setFont(&PicopixelFB);
  display.setTextSize(1);
  display.setTextWrap(false);

  // ── header ────────────────────────────────────────────────────────────────
  const int16_t hbase = FB_Y_HEADER + FB_ASCENT;
  int16_t bx, by; uint16_t bw, bh;

  const char *apt = FB_AIRPORT_NAMES[s_aptIdx];
  display.setCursor(FB_X_TIME, hbase);
  display.setTextColor(display.color565(255, 255, 255));
  display.print(apt);

  // Direction sits a fixed gap after the name, which varies in width.
  display.getTextBounds(apt, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(FB_X_TIME + (int16_t)bw + 6, hbase);
  display.setTextColor(display.color565(255, 180, 0));
  display.print(departures ? "DEPARTURES" : "ARRIVALS");

  // Top right is the time now, as a station board shows it. It used to be the
  // payload's "upd" - when Home Assistant last fetched - which stands still
  // between fetches and on the panel read as a clock that had stopped (owner,
  // on the bench, 2026-09-14). Freshness stays in the colour: amber once the
  // data is more than ten minutes old.
  bool stale = flightboardAge() > 600 || updAgeMin(b) > FB_FRESH_MIN;
  char nowHm[6] = "--:--";
  struct tm lt;
  if (getLocalTime(&lt, 0)) snprintf(nowHm, sizeof(nowHm), "%02d:%02d", lt.tm_hour, lt.tm_min);
  display.getTextBounds(nowHm, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(FB_X_RIGHT - (int16_t)bw, hbase);
  display.setTextColor(stale ? display.color565(150, 90, 0)
                             : display.color565(120, 132, 138));
  display.print(nowHm);

  display.drawFastHLine(0, FB_Y_RULE, 128, display.color565(52, 60, 64));
  // While both halves are here and swapping, the rule fills in dim amber
  // towards the next swap, so a change never comes as a surprise.
  if (s_mode == FB_DIR_ALT && s_b[0].have && s_b[1].have) {
    const int16_t w = (int16_t)(((nowMs - s_altT0) % FB_ALT_MS) * 128UL / FB_ALT_MS);
    if (w > 0) display.drawFastHLine(0, FB_Y_RULE, w, display.color565(110, 78, 0));
  }

  if (!b.have || b.count == 0) {
    display.setCursor(40, 34);
    display.setTextColor(display.color565(120, 120, 120));
#if defined(FB_MQTT_ENABLED)
    // Say which of the several ways to have no data this is. "NO DATA" alone
    // sends you looking at Home Assistant when the panel never reached WiFi.
    display.print(fbMqttStatus());
#else
    display.print("NO DATA");
#endif
    display.setFont(NULL);
    return;
  }

  // ── window ────────────────────────────────────────────────────────────────
  int16_t start = (int16_t)b.nowIdx - 1;
  if (start + FB_VISIBLE > b.count) start = b.count - FB_VISIBLE;
  if (start < 0) start = 0;

  for (uint8_t i = 0; i < FB_VISIBLE; i++) {
    uint8_t idx = start + i;
    if (idx >= b.count) break;
    const FbRow &r = b.rows[idx];
    int16_t top = FB_Y_ROW0 + i * FB_ROW_H;
    int16_t base = top + FB_ASCENT;
    uint16_t col = statusColor(r.st);

    if (idx == b.nowIdx) {
      display.fillRect(0, top, 1, FB_ROW_H - 1, display.color565(255, 180, 0));
    }

    display.setTextColor(col);
    display.setCursor(FB_X_TIME, base);
    display.print(r.tm);
    display.setCursor(FB_X_FLIGHT, base);
    display.print(r.fn);
    // Status word first: it is right-aligned and fixed, so it decides how much
    // room the city has left. Colour alone says something is wrong; only the
    // word says what.
    const char *word = statusWord(r.st, departures);
    uint16_t wordW = 0;
    if (word[0]) {
      display.getTextBounds(word, 0, 0, &bx, &by, &bw, &bh);
      wordW = bw;
      display.setCursor(FB_X_RIGHT - (int16_t)bw, base);
      display.print(word);
    }

    // Destination: the IATA code always, then the city name if it fits.
    //
    // The code is the part that is never ambiguous and never wrong - AeroAPI's
    // "city" for TLS is BLAGNAC and for BSL is EUROAIRPORT - so it is drawn
    // first and unconditionally. The name is context on top of it.
    display.setCursor(FB_X_DEST, base);
    display.print(r.ct);
    display.getTextBounds(r.ct, 0, 0, &bx, &by, &bw, &bh);
    const int16_t xCity = FB_X_DEST + (int16_t)bw + FB_CODE_GAP;

    // City, fitted to the gap that actually remains rather than to a fixed
    // character count - a proportional font makes those two different things.
    // Fitting drops whole trailing words (FRANKFURT AM MAIN -> FRANKFURT), then
    // any dangling connector left behind (AM, DE, SUR), and if the first word
    // still will not fit the row simply stops at the code. A half word is worse
    // than a code: live data rendered EUROAIRPORT as EUROAIRPOR, which reads as
    // a typo rather than as an abbreviation.
    const int16_t cityRoom = (FB_X_RIGHT - (int16_t)wordW - FB_GAP) - xCity;
    char city[FB_CY_LEN];
    strncpy(city, r.cy, sizeof(city) - 1);
    city[sizeof(city) - 1] = '\0';
    for (;;) {
      if (city[0] == '\0') break;
      display.getTextBounds(city, 0, 0, &bx, &by, &bw, &bh);
      if ((int16_t)bw <= cityRoom) break;
      char *sp = strrchr(city, ' ');
      if (sp == NULL) { city[0] = '\0'; break; }   // code alone says it already
      *sp = '\0';
    }
    char *tail = strrchr(city, ' ');
    if (tail != NULL && strlen(tail + 1) <= 2) *tail = '\0';
    if (city[0]) {
      display.setCursor(xCity, base);
      display.print(city);
    }
  }

  // Other pages draw with the built-in font and would inherit this one.
  display.setFont(NULL);
}

#endif  // FLIGHTBOARD_ENABLED

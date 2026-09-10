#include "flightboard.h"

#if defined(FLIGHTBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "../config/config.h"
#include "../display/display.h"
#include "picopixel_fb.h"   // Picopixel with a legible U

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

static FbRow    s_rows[FB_MAX_ROWS];
static uint8_t  s_count    = 0;
static uint8_t  s_nowIdx   = 0;
static char     s_apt[8]   = "LFMN";
static char     s_dir[4]   = "arr";
static char     s_upd[FB_TM_LEN] = "--:--";
static uint32_t s_stamp    = 0;
static bool     s_haveData = false;

// Whitelist enforced on the HA side; kept in the same canonical order so the
// index means the same thing on both ends.
static const char *const FB_AIRPORTS[] = {"LFMD","LFMN","LFPG","EGLL","EDDF","EHAM"};
// Shown instead of the ICAO code. The list is closed and enforced on the Home
// Assistant side, so a table here is enough and costs no round trip.
static const char *const FB_AIRPORT_NAMES[] = {"CANNES","NICE","PARIS CDG",
                                               "LONDON","FRANKFURT","AMSTERDAM"};
static const uint8_t FB_AIRPORT_COUNT = sizeof(FB_AIRPORTS)/sizeof(FB_AIRPORTS[0]);
static uint8_t s_aptIdx = 1;   // LFMN
static bool    s_dirDep = false;

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

// ICAO code to the name people actually use. Falls back to the code for
// anything unexpected, so a widened whitelist degrades instead of breaking.
static const char *airportName(const char *icao) {
  for (uint8_t i = 0; i < FB_AIRPORT_COUNT; i++) {
    if (!strcmp(icao, FB_AIRPORTS[i])) return FB_AIRPORT_NAMES[i];
  }
  return icao;
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

  uint8_t n = 0;
  for (JsonObjectConst f : arr) {
    if (n >= FB_MAX_ROWS) break;
    copyField(s_rows[n].fn, FB_FN_LEN, f["fn"] | "");
    copyField(s_rows[n].tm, FB_TM_LEN, f["tm"] | "--:--");
    copyField(s_rows[n].ct, FB_CT_LEN, f["ct"] | "");
    // Prefer the city name; fall back to the code when HA does not send one.
    copyField(s_rows[n].cy, FB_CY_LEN, f["cy"] | (const char *)(f["ct"] | ""));
    s_rows[n].st = parseStatus(f["st"] | "");
    n++;
  }
  s_count  = n;
  s_nowIdx = doc["now_idx"] | 0;
  if (s_nowIdx >= s_count && s_count) s_nowIdx = s_count - 1;
  copyField(s_apt, sizeof(s_apt), doc["apt"] | "----");
  copyField(s_dir, sizeof(s_dir), doc["dir"] | "arr");
  copyField(s_upd, FB_TM_LEN,     doc["upd"] | "--:--");
  s_stamp    = millis();
  s_haveData = true;
  return true;
}

bool flightboardHasData() { return s_haveData; }

uint32_t flightboardAge() {
  return s_haveData ? (millis() - s_stamp) / 1000UL : 0;
}

const char *flightboardAirport()   { return FB_AIRPORTS[s_aptIdx]; }
const char *flightboardDirection() { return s_dirDep ? "dep" : "arr"; }

void flightboardStepAirport(int8_t delta) {
  int16_t i = (int16_t)s_aptIdx + delta;
  while (i < 0) i += FB_AIRPORT_COUNT;
  s_aptIdx = (uint8_t)(i % FB_AIRPORT_COUNT);
}

void flightboardToggleDirection() { s_dirDep = !s_dirDep; }

void flightboardRender() {
  display.setFont(&PicopixelFB);
  display.setTextSize(1);
  display.setTextWrap(false);

  // ── header ────────────────────────────────────────────────────────────────
  const int16_t hbase = FB_Y_HEADER + FB_ASCENT;
  int16_t bx, by; uint16_t bw, bh;

  const char *apt = airportName(s_apt);
  display.setCursor(FB_X_TIME, hbase);
  display.setTextColor(display.color565(255, 255, 255));
  display.print(apt);

  // Direction sits a fixed gap after the name, which varies in width.
  display.getTextBounds(apt, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(FB_X_TIME + (int16_t)bw + 6, hbase);
  display.setTextColor(display.color565(255, 180, 0));
  display.print(strcmp(s_dir, "dep") == 0 ? "DEPARTURES" : "ARRIVALS");

  bool stale = flightboardAge() > 600;
  display.getTextBounds(s_upd, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(FB_X_RIGHT - (int16_t)bw, hbase);
  display.setTextColor(stale ? display.color565(150, 90, 0)
                             : display.color565(120, 132, 138));
  display.print(s_upd);

  display.drawFastHLine(0, FB_Y_RULE, 128, display.color565(52, 60, 64));

  if (!s_haveData || s_count == 0) {
    display.setCursor(40, 34);
    display.setTextColor(display.color565(120, 120, 120));
    display.print("NO DATA");
    display.setFont(NULL);
    return;
  }

  // ── window ────────────────────────────────────────────────────────────────
  const bool departures = (strcmp(s_dir, "dep") == 0);

  int16_t start = (int16_t)s_nowIdx - 1;
  if (start + FB_VISIBLE > s_count) start = s_count - FB_VISIBLE;
  if (start < 0) start = 0;

  for (uint8_t i = 0; i < FB_VISIBLE; i++) {
    uint8_t idx = start + i;
    if (idx >= s_count) break;
    const FbRow &r = s_rows[idx];
    int16_t top = FB_Y_ROW0 + i * FB_ROW_H;
    int16_t base = top + FB_ASCENT;
    uint16_t col = statusColor(r.st);

    if (idx == s_nowIdx) {
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

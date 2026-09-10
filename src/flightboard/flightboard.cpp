#include "flightboard.h"

#if defined(FLIGHTBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "../config/config.h"
#include "../display/display.h"

// ── layout ──────────────────────────────────────────────────────────────────
// 128x64 with the built-in 5x7 GFX font (6 px advance, 8 px line pitch).
// A row is "HH:MM FFFFFFF CCC" = 17 glyphs = 102 px, leaving 26 px spare, so
// the readable 5x7 font fits and the cramped 4x6 is not needed.
static const int16_t FB_X_TIME   = 1;
static const int16_t FB_X_FLIGHT = 37;
static const int16_t FB_X_CODE   = 91;
static const int16_t FB_Y_HEADER = 0;    // airport + direction + update time
static const int16_t FB_Y_RULE   = 9;
static const int16_t FB_Y_ROW0   = 12;
static const int16_t FB_ROW_H    = 8;
static const uint8_t FB_VISIBLE  = 6;    // (64 - 12) / 8 = 6 whole rows

struct FbRow {
  char     fn[FB_FN_LEN];
  char     tm[FB_TM_LEN];
  char     ct[FB_CT_LEN];
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
static const uint8_t FB_AIRPORT_COUNT = sizeof(FB_AIRPORTS)/sizeof(FB_AIRPORTS[0]);
static uint8_t s_aptIdx = 1;   // LFMN
static bool    s_dirDep = false;

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

  uint8_t n = 0;
  for (JsonObjectConst f : arr) {
    if (n >= FB_MAX_ROWS) break;
    copyField(s_rows[n].fn, FB_FN_LEN, f["fn"] | "");
    copyField(s_rows[n].tm, FB_TM_LEN, f["tm"] | "--:--");
    copyField(s_rows[n].ct, FB_CT_LEN, f["ct"] | "");
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
  display.setTextSize(1);
  display.setTextWrap(false);

  // ── header ────────────────────────────────────────────────────────────────
  display.setCursor(FB_X_TIME, FB_Y_HEADER);
  display.setTextColor(display.color565(255, 255, 255));
  display.print(s_apt);

  display.setCursor(FB_X_TIME + 30, FB_Y_HEADER);
  display.setTextColor(display.color565(255, 200, 0));
  display.print(strcmp(s_dir, "dep") == 0 ? "DEP" : "ARR");

  // Update time greys out once the payload is old enough to be misleading.
  bool stale = flightboardAge() > 600;
  display.setCursor(FB_X_CODE + 6, FB_Y_HEADER);
  display.setTextColor(stale ? display.color565(140, 90, 0)
                             : display.color565(120, 120, 120));
  display.print(s_upd);

  display.drawFastHLine(0, FB_Y_RULE, 128, display.color565(60, 60, 60));

  if (!s_haveData || s_count == 0) {
    display.setCursor(20, 30);
    display.setTextColor(display.color565(120, 120, 120));
    display.print("NO DATA");
    return;
  }

  // ── window ────────────────────────────────────────────────────────────────
  // Put the next flight on the second visible line: one past flight for
  // context, the rest of the window looking forward.
  int16_t start = (int16_t)s_nowIdx - 1;
  if (start + FB_VISIBLE > s_count) start = s_count - FB_VISIBLE;
  if (start < 0) start = 0;

  for (uint8_t i = 0; i < FB_VISIBLE; i++) {
    uint8_t idx = start + i;
    if (idx >= s_count) break;
    const FbRow &r = s_rows[idx];
    int16_t y = FB_Y_ROW0 + i * FB_ROW_H;
    uint16_t col = statusColor(r.st);

    // The next flight gets a marker column of its own; everything else keeps
    // the full width for data.
    if (idx == s_nowIdx) {
      display.fillRect(0, y - 1, 1, 7, display.color565(255, 200, 0));
    }

    display.setTextColor(col);
    display.setCursor(FB_X_TIME + 2, y);
    display.print(r.tm);
    display.setCursor(FB_X_FLIGHT, y);
    display.print(r.fn);
    display.setCursor(FB_X_CODE, y);
    display.print(r.ct);
  }
}

#endif  // FLIGHTBOARD_ENABLED

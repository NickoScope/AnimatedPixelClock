#pragma once
// Rail board settings: what they are, their bounds, the colour palette, the
// "due soon" rule, and the validation the web portal's POST goes through.
//
// Plain C++ and ArduinoJson, no Arduino core: railboard.cpp and web_panel.cpp
// use it on the panel, tools/railboard/settings_host_test.cpp on the host, and
// tools/railboard/render.py reads the palette and bounds out of this file.

#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "rb_model.h"

namespace rbs {

// ── bounds ──────────────────────────────────────────────────────────────────
// What the page can draw, not recommendations.
static const uint8_t  kRowsMin    = 1;
static const uint8_t  kRowsMax    = RB_MAX_SVC;   // services listed; six to a page
static const uint16_t kSwitchMin  = 3;
static const uint16_t kSwitchMax  = 600;
static const uint8_t  kLevelMin   = 10;
static const uint8_t  kLevelMax   = 100;
static const uint16_t kStaleMin   = 30;
static const uint16_t kStaleMax   = 3600;
// 0 is off. Fifteen minutes is the most that still means "go now": a busy
// station lists a train every few minutes, so a wider window paints most of the
// first page green. A design choice, not a railway rule.
static const uint8_t  kDueMax     = 15;

// ── palette ─────────────────────────────────────────────────────────────────
// Named, not free RGB: a handful that read on a HUB75 panel against black, and
// no red, which means Cancelled. amber is the board's own (255,150,0 - the
// photograph's 255,170,0 leans yellow on these panels). green is set for the
// "due soon" rows: its green channel sits well above amber's so the two differ
// in hue and in brightness, with a little red kept so it is not a harsh
// pure green at full level.
struct Colour { const char *name; uint8_t r, g, b; };
static const Colour kPalette[] = {
  {"amber",  255, 150,   0},
  {"yellow", 255, 214,   0},
  {"orange", 255,  96,   0},
  {"white",  255, 255, 255},
  {"green",   48, 232,  64},
  {"cyan",     0, 214, 232},
};
static const uint8_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);
enum : uint8_t { AMBER = 0, YELLOW, ORANGE, WHITE, GREEN, CYAN };

inline int paletteIndex(const char *name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < kPaletteCount; i++)
    if (!strcmp(name, kPalette[i].name)) return i;
  return -1;
}

// ── the settings ────────────────────────────────────────────────────────────
struct Settings {
  uint8_t  rows;       // services listed, over one or two pages
  uint16_t switchS;    // seconds per list before the other one
  uint8_t  level;      // percent of full colour on this page
  uint16_t staleS;     // Data updating after this long without a fresh board
  uint8_t  rowColour;  // palette index: every row, the footer and the clock
  uint8_t  headColour; // palette index: the title and the column headings
  uint8_t  dueColour;  // palette index: rows due within dueMin
  uint8_t  dueMin;     // 0 = off
  bool     clockSeconds;
};

inline Settings defaults(uint8_t rows, uint16_t switchS, uint8_t level, uint16_t staleS) {
  Settings s;
  s.rows = rows; s.switchS = switchS; s.level = level; s.staleS = staleS;
  s.rowColour = AMBER; s.headColour = WHITE; s.dueColour = GREEN;
  s.dueMin = 3; s.clockSeconds = true;
  return s;
}

inline bool valid(const Settings &s) {
  return s.rows >= kRowsMin && s.rows <= kRowsMax && s.switchS >= kSwitchMin && s.switchS <= kSwitchMax &&
         s.level >= kLevelMin && s.level <= kLevelMax && s.staleS >= kStaleMin && s.staleS <= kStaleMax &&
         s.rowColour < kPaletteCount && s.headColour < kPaletteCount && s.dueColour < kPaletteCount &&
         s.dueMin <= kDueMax && !(s.dueMin && s.dueColour == s.rowColour);
}

inline bool same(const Settings &a, const Settings &b) {
  return a.rows == b.rows && a.switchS == b.switchS && a.level == b.level && a.staleS == b.staleS &&
         a.rowColour == b.rowColour && a.headColour == b.headColour && a.dueColour == b.dueColour &&
         a.dueMin == b.dueMin && a.clockSeconds == b.clockSeconds;
}

// An integer in [lo, hi]. Refuses 1.5, "3", true and anything out of range.
inline bool intIn(JsonVariantConst v, long lo, long hi, long *out) {
  if (!v.is<long>() || v.is<bool>()) return false;
  const long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = x;
  return true;
}

// The web portal's {"config":{...}}: every key must be known and every value
// valid, or nothing changes. Keys that are absent keep their value in `s`.
// Returns nullptr on success, else a message naming the field; `err` holds it.
inline const char *apply(JsonObjectConst in, Settings &s, char *err, size_t cap) {
  Settings n = s;
  for (JsonPairConst kv : in) {
    const char *k = kv.key().c_str();
    JsonVariantConst v = kv.value();
    long x;
    int c;
    if (!strcmp(k, "rows")) {
      if (!intIn(v, kRowsMin, kRowsMax, &x)) { snprintf(err, cap, "config.rows must be %u to %u", kRowsMin, kRowsMax); return err; }
      n.rows = (uint8_t)x;
    } else if (!strcmp(k, "switch_s")) {
      if (!intIn(v, kSwitchMin, kSwitchMax, &x)) { snprintf(err, cap, "config.switch_s must be %u to %u", kSwitchMin, kSwitchMax); return err; }
      n.switchS = (uint16_t)x;
    } else if (!strcmp(k, "level")) {
      if (!intIn(v, kLevelMin, kLevelMax, &x)) { snprintf(err, cap, "config.level must be %u to %u", kLevelMin, kLevelMax); return err; }
      n.level = (uint8_t)x;
    } else if (!strcmp(k, "stale_s")) {
      if (!intIn(v, kStaleMin, kStaleMax, &x)) { snprintf(err, cap, "config.stale_s must be %u to %u", kStaleMin, kStaleMax); return err; }
      n.staleS = (uint16_t)x;
    } else if (!strcmp(k, "due_min")) {
      if (!intIn(v, 0, kDueMax, &x)) { snprintf(err, cap, "config.due_min must be 0 (off) to %u", kDueMax); return err; }
      n.dueMin = (uint8_t)x;
    } else if (!strcmp(k, "clock_seconds")) {
      if (!v.is<bool>()) { snprintf(err, cap, "config.clock_seconds must be true or false"); return err; }
      n.clockSeconds = v.as<bool>();
    } else if (!strcmp(k, "row_color") || !strcmp(k, "head_color") || !strcmp(k, "due_color")) {
      if ((c = paletteIndex(v.as<const char *>())) < 0) {
        snprintf(err, cap, "config.%s must be amber, yellow, orange, white, green or cyan", k);
        return err;
      }
      if (k[0] == 'r') n.rowColour = (uint8_t)c;
      else if (k[0] == 'h') n.headColour = (uint8_t)c;
      else n.dueColour = (uint8_t)c;
    } else {
      snprintf(err, cap, "config.%.24s is not a setting", k);
      return err;
    }
  }
  if (n.dueMin && n.dueColour == n.rowColour) {
    snprintf(err, cap, "config.due_color must differ from row_color, or due_min must be 0");
    return err;
  }
  s = n;
  return nullptr;
}

inline void toJson(const Settings &s, JsonObject o) {
  o["rows"]          = s.rows;
  o["switch_s"]      = s.switchS;
  o["level"]         = s.level;
  o["stale_s"]       = s.staleS;
  o["row_color"]     = kPalette[s.rowColour].name;
  o["head_color"]    = kPalette[s.headColour].name;
  o["due_color"]     = kPalette[s.dueColour].name;
  o["due_min"]       = s.dueMin;
  o["clock_seconds"] = s.clockSeconds;
}

inline void boundsJson(JsonObject o) {
  auto range = [&o](const char *k, long lo, long hi) {
    JsonArray a = o[k].to<JsonArray>();
    a.add(lo);
    a.add(hi);
  };
  range("rows", kRowsMin, kRowsMax);
  range("switch_s", kSwitchMin, kSwitchMax);
  range("level", kLevelMin, kLevelMax);
  range("stale_s", kStaleMin, kStaleMax);
  range("due_min", 0, kDueMax);
  JsonArray p = o["colors"].to<JsonArray>();
  for (uint8_t i = 0; i < kPaletteCount; i++) {
    JsonObject c = p.add<JsonObject>();
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", kPalette[i].r, kPalette[i].g, kPalette[i].b);
    c["name"] = kPalette[i].name;
    c["hex"]  = hex;
  }
}

// ── due soon ────────────────────────────────────────────────────────────────
// Green from dueMin minutes before the service's time until the board drops
// it. The time is the expected one (actual, forecast, estimate) when there is
// one, else the scheduled one. There is no lower bound: a train at or past its
// time that has not been reported gone may still be at the platform, and the
// list itself removes it 60 s after that time (120 s after an arrival). A
// departure reported as departed never reaches the list. Cancelled never is.
inline bool dueSoon(const RbService &sv, int64_t now, uint8_t dueMin) {
  if (!dueMin || sv.st == RB_CANC) return false;
  const int64_t when = sv.x ? sv.x : sv.t;
  if (!when) return false;
  return when - now <= (int64_t)dueMin * 60;
}

}  // namespace rbs

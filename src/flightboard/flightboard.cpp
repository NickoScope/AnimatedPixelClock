#include "flightboard.h"
#include "../util/psram_state.h"

#if defined(FLIGHTBOARD_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

#include "../config/config.h"
#include "../display/display.h"
#include "../fonts/picopixel_fb.h"   // Picopixel with a legible U
#include "aero_direct.h"
#include "fb_mqtt.h"
#include "fb_settings.h"
#include "fb_zone.h"

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

// The tracked flight's row: the first of the seven, on a dark blue band the
// width of the panel with a bright blue bar at its left edge. Blue because
// no status uses it as a background and amber already marks "now".
static const uint8_t FB_PIN_BG_R  = 0;
static const uint8_t FB_PIN_BG_G  = 26;
static const uint8_t FB_PIN_BG_B  = 70;
static const uint8_t FB_PIN_BAR_R = 0;
static const uint8_t FB_PIN_BAR_G = 170;
static const uint8_t FB_PIN_BAR_B = 255;
static const int16_t FB_PIN_GATE_MIN = 60;   // the gate replaces ON TIME this long before departure
// Air between the ident and the route. A 3-letter ICAO ident (AFR7301, 27 px)
// ends at x 49 and the route column starts at 50: one pixel read as one word
// in the preview, so the route moves right until four are clear.
static const int16_t FB_PIN_ROUTE_GAP = 4;
// The header's zone cue - how far the airport's clock is from the panel's,
// "-1", "+6", "+5:30", or "HA" while Home Assistant's boards are shown - sits
// this far left of the clock's widest form (88:88, 18 px), so it does not move
// each minute. It is drawn only when it clears DEPARTURES, the wider of the
// two words, by the same gap: the page swaps words every 10 s and a cue that
// came and went with them would read as a fault.
static const int16_t FB_CUE_GAP   = 3;
static const int16_t FB_CLOCK_MAX_W = 18;

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
static PSRAM_ARRAY(FbBoard, s_b, [2]);            // [0] arrivals, [1] departures

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
// IATA codes and zones of the same six, as the mwgg/Airports dataset (MIT) the
// portal searches lists them, read 2026-09-14.
static const char *const FB_AIRPORT_IATA_CODES[] = {"CEQ","NCE","CDG","LHR","FRA","AMS"};
static const char *const FB_AIRPORT_ZONES[] = {"Europe/Paris","Europe/Paris","Europe/Paris",
                                               "Europe/London","Europe/Berlin","Europe/Amsterdam"};
static const uint8_t FB_AIRPORT_COUNT = sizeof(FB_AIRPORTS)/sizeof(FB_AIRPORTS[0]);
static uint8_t s_aptId = 1;   // LFMN

// Airports added in the portal, kept by src/panel in NVS "panel"/fbA0-fbA5.
static PSRAM_ARRAY(FbAirport, s_custom, [FB_APT_CUSTOM_MAX]);
static bool      s_customUsed[FB_APT_CUSTOM_MAX];

#if defined(FLIGHTBOARD_DIRECT_ENABLED)
// The selected airport's zone, looked up again when the selection or that
// airport changes (clearBoards bumps s_aptGen).
static fbz::Zone s_zone;
static uint8_t   s_zoneId  = 0xFF;
static uint32_t  s_zoneGen = 0;
static uint32_t  s_aptGen  = 1;
static uint32_t s_directGen   = 0xFFFFFFFFUL;
static uint32_t s_rebuiltMs   = 0;
static uint32_t s_onScreenMs  = 0;
static bool     s_everOn      = false;
static uint32_t s_directAgeS[2] = {0, 0};   // the older of each half's two lists
#endif

// Minutes since Home Assistant fetched this half, from its "upd" stamp and the
// local clock; -1 when either is unknown.
static int16_t updAgeMin(const FbBoard &b) {
  struct tm lt;
  if (!b.have || !getLocalTime(&lt, 0)) return -1;
  int h, m;
  if (sscanf(b.upd, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  const int16_t age = (int16_t)(((lt.tm_hour * 60 + lt.tm_min) - (h * 60 + m) + 1440) % 1440);
  // A stamp up to an hour "in the future" is Home Assistant's clock running a
  // little ahead of ours, not a board from yesterday. The payload carries no
  // date, so a day-old board fetched within the same half hour still passes.
  return age > 1440 - 60 ? 0 : age;
}

static void clearBoards() {
  for (FbBoard &b : s_b) { b.have = false; b.count = 0; }
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  s_aptGen++;
  aeroDirectAirportChanged();
  s_directGen = 0xFFFFFFFFUL;
#endif
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

// ── airports ────────────────────────────────────────────────────────────────
static bool customId(uint8_t id) {
  return id >= FB_APT_CUSTOM && id < FB_APT_CUSTOM + FB_APT_CUSTOM_MAX && s_customUsed[id - FB_APT_CUSTOM];
}

bool flightboardAirportById(uint8_t id, FbAirport *out) {
  if (id < FB_AIRPORT_COUNT) {
    memset(out, 0, sizeof(*out));
    copyField(out->icao, sizeof(out->icao), FB_AIRPORTS[id]);
    copyField(out->iata, sizeof(out->iata), FB_AIRPORT_IATA_CODES[id]);
    copyField(out->name, sizeof(out->name), FB_AIRPORT_NAMES[id]);
    copyField(out->tz, sizeof(out->tz), FB_AIRPORT_ZONES[id]);
    return true;
  }
  if (customId(id)) { *out = s_custom[id - FB_APT_CUSTOM]; return true; }
  return false;
}

bool        flightboardAirportBuiltin(uint8_t id) { return id < FB_AIRPORT_COUNT; }
uint8_t     flightboardBuiltinCount()             { return FB_AIRPORT_COUNT; }
uint8_t     flightboardAirportId()                { return s_aptId; }
bool        flightboardCustomUsed(uint8_t slot)   { return slot < FB_APT_CUSTOM_MAX && s_customUsed[slot]; }

const char *flightboardAirport() {
  return s_aptId < FB_AIRPORT_COUNT ? FB_AIRPORTS[s_aptId]
       : customId(s_aptId)          ? s_custom[s_aptId - FB_APT_CUSTOM].icao
                                    : FB_AIRPORTS[1];
}

const char *flightboardAirportLabel(uint8_t id) {
  if (id < FB_AIRPORT_COUNT) return FB_AIRPORT_NAMES[id];
  return customId(id) ? s_custom[id - FB_APT_CUSTOM].name : "";
}

int flightboardNameWidth(const char *name) {
  int w = 0;
  for (const char *s = name ? name : ""; *s; s++) {
    unsigned c = (unsigned char)*s;
    if (c < PicopixelFB.first || c > PicopixelFB.last) c = ' ';
    w += PicopixelFB.glyph[c - PicopixelFB.first].xAdvance;
  }
  return w;
}

#if defined(FLIGHTBOARD_DIRECT_ENABLED)
static bool knownZone(const char *iana) { return tzdbPosix(iana) != nullptr; }
const char *flightboardAirportCheck(const FbAirport &a) { return fbs::checkAirport(a, flightboardNameWidth, knownZone); }
#else
const char *flightboardAirportCheck(const FbAirport &a) { return fbs::checkAirport(a, flightboardNameWidth, nullptr); }
#endif

bool flightboardSetCustomAirport(uint8_t slot, const FbAirport *a) {
  if (slot >= FB_APT_CUSTOM_MAX) return false;
  if (!a) {
    s_customUsed[slot] = false;
    if (s_aptId == FB_APT_CUSTOM + slot) { s_aptId = 1; clearBoards(); }   // back to Nice, the default
    return true;
  }
  if (flightboardAirportCheck(*a)) return false;
  s_custom[slot] = *a;
  s_custom[slot].name[FB_APT_NAME_MAX] = '\0';
  s_customUsed[slot] = true;
  if (s_aptId == FB_APT_CUSTOM + slot) clearBoards();
  return true;
}

// The knob walks the built-in airports, then the custom ones, by id.
void flightboardStepAirport(int8_t delta) {
  uint8_t ids[FB_AIRPORT_COUNT + FB_APT_CUSTOM_MAX];
  uint8_t n = 0, at = 0;
  for (uint8_t i = 0; i < FB_AIRPORT_COUNT; i++) ids[n++] = i;
  for (uint8_t i = 0; i < FB_APT_CUSTOM_MAX; i++)
    if (s_customUsed[i]) ids[n++] = (uint8_t)(FB_APT_CUSTOM + i);
  for (uint8_t i = 0; i < n; i++)
    if (ids[i] == s_aptId) at = i;
  const uint8_t next = ids[(at + n + (delta > 0 ? 1 : n - 1)) % n];
  if (next != s_aptId) { s_aptId = next; clearBoards(); }
}

bool flightboardDirectOwns() {
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  return aeroDirectHasKey();
#else
  return false;
#endif
}

bool flightboardIngest(const char *json, uint16_t len) {
  // With a key stored the page is AeroAPI's; a retained board still arriving
  // from Home Assistant must not overwrite it.
  if (flightboardDirectOwns()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;   // keep the old board

  JsonArrayConst arr = doc["f"].as<JsonArrayConst>();
  if (arr.isNull()) return false;
  // A retained board for the airport just left can still be on its way when
  // the selection moves on; it must not land under the new name.
  // A payload that does not name its airport is refused too: it cannot be
  // told from one for another airport.
  const char *apt = doc["apt"] | "";
  if (strcmp(apt, flightboardAirport())) return false;
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

bool flightboardSelect(uint8_t airport, FbDirMode mode) {
  FbAirport a;
  if (!flightboardAirportById(airport, &a) || mode > FB_DIR_ALT) return false;
  if (airport != s_aptId) { s_aptId = airport; clearBoards(); }
  s_mode = mode;
  return true;
}

// ── direct ──────────────────────────────────────────────────────────────────
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
static const fbz::Zone &aptZone() {
  if (s_zoneId != s_aptId || s_zoneGen != s_aptGen) {
    FbAirport a;
    if (!flightboardAirportById(s_aptId, &a) || !fbz::load(a.tz, &s_zone)) s_zone.known = false;
    s_zoneId  = s_aptId;
    s_zoneGen = s_aptGen;
  }
  return s_zone;
}

// The IANA zone of a listed airport with that ICAO or IATA code, for a
// tracked flight's end that AeroAPI sent no usable zone for.
static const char *listedZone(const char *code) {
  for (uint8_t i = 0; i < FB_AIRPORT_COUNT; i++)
    if (!strcmp(code, FB_AIRPORTS[i]) || !strcmp(code, FB_AIRPORT_IATA_CODES[i])) return FB_AIRPORT_ZONES[i];
  for (uint8_t i = 0; i < FB_APT_CUSTOM_MAX; i++)
    if (s_customUsed[i] && (!strcmp(code, s_custom[i].icao) || (s_custom[i].iata[0] && !strcmp(code, s_custom[i].iata))))
      return s_custom[i].tz;
  return nullptr;
}

// The zone for one end of a tracked flight: departures in the origin's time,
// arrivals in the destination's.
static fbz::Source trackZone(const aero::Track &t, bool arrival, fbz::Zone *z) {
  return fbz::pick(arrival ? t.toTz : t.fromTz, arrival ? t.to : t.from, listedZone, z);
}

// The panel's own offset now, rounded to the minute: getLocalTime and time()
// can straddle a second.
static bool panelOffset(int64_t now, int32_t *off) {
  struct tm lt;
  if (!getLocalTime(&lt, 0)) return false;
  const int32_t o = fbz::offsetOfLocal(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday, lt.tm_hour,
                                       lt.tm_min, lt.tm_sec, now);
  *off = (o + (o >= 0 ? 30 : -30)) / 60 * 60;
  return true;
}

// Both halves from the lists as they stand, every time in the airport's own
// local time. (Home Assistant's MQTT boards arrive as HH:MM in its zone and
// cannot be converted; they are shown as they come.)
static void rebuildDirect() {
  static aero::Board board;           // 750 B; kept off the loop task's stack
  const int64_t now = (int64_t)time(nullptr);
  const uint32_t nowMs = millis();
  for (uint8_t d = 0; d < 2; d++) {
    uint32_t agePast = 0, ageNext = 0;
    const aero::ListResult *past = aeroDirectList(d ? aero::DEP_PAST : aero::ARR_PAST, &agePast);
    const aero::ListResult *next = aeroDirectList(d ? aero::DEP_NEXT : aero::ARR_NEXT, &ageNext);
    FbBoard &b = s_b[d];
    if ((!past && !next) || now < 1700000000) { b.have = false; b.count = 0; continue; }
    aero::buildBoard(past, next, now, &board);
    for (uint8_t i = 0; i < board.count; i++) {
      FbRow &r = b.rows[i];
      const aero::Cand &c = board.rows[i];
      copyField(r.fn, sizeof(r.fn), c.fn);
      copyField(r.ct, sizeof(r.ct), c.ct);
      copyField(r.cy, sizeof(r.cy), c.cy[0] ? c.cy : c.ct);   // no city: the code, as the MQTT ingest does
      r.st = (FbStatus)c.st;
      fbz::hm(c.t, aptZone(), r.tm, sizeof(r.tm));
    }
    b.count  = board.count;
    b.nowIdx = board.nowIdx >= board.count && board.count ? board.count - 1 : board.nowIdx;
    const uint32_t age = past && next ? (agePast > ageNext ? agePast : ageNext) : (past ? agePast : ageNext);
    s_directAgeS[d] = age;
    fbz::hm(now - age, aptZone(), b.upd, sizeof(b.upd));
    b.stamp = nowMs - age * 1000UL;
    b.have  = true;
  }
  s_rebuiltMs = nowMs;
}
#endif

void flightboardTick(bool onScreen) {
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  const uint32_t nowMs = millis();
  if (onScreen) { s_onScreenMs = nowMs; s_everOn = true; }
  AeroWant w;
  w.icao    = flightboardAirport();
  w.arr     = flightboardWants(false);
  w.dep     = flightboardWants(true);
  w.visible = onScreen || (s_everOn && nowMs - s_onScreenMs < AERO_VISIBLE_GRACE_S * 1000UL);
  aeroDirectLoop(w);
  if (!aeroDirectHasKey()) return;
  // Again whenever a list arrives, and every 30 s so that now_idx and the
  // +-2 h window move with the clock between calls.
  const uint32_t gen = aeroDirectListGen();
  if (gen != s_directGen || nowMs - s_rebuiltMs > 30000UL) {
    s_directGen = gen;
    rebuildDirect();
  }
#else
  (void)onScreen;
#endif
}

// ── tracked flight row ──────────────────────────────────────────────────────
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
// The word and its colour. Short on purpose: the row also holds a time, the
// ident and the route. There is no BOARDING: no AeroAPI field says boarding
// has started, so the gate stands in for it in the last hour.
static const char *trackWord(const AeroTracker &k, char *buf, size_t cap, uint16_t *col) {
  const aero::Track &t = k.t;
  const int32_t late = aero::trackDelayMin(t);
  const int64_t now = (int64_t)time(nullptr);
  switch (t.state) {
  case FB_TRK_WAIT:      *col = display.color565(120, 132, 138); return "...";
  case FB_TRK_NOTFOUND:  *col = display.color565(120, 132, 138); return "NO FLIGHT";
  case FB_TRK_SCHED: {
    const int64_t dep = t.estOut ? t.estOut : t.schedOut;
    if (t.gate[0] && dep && dep - now <= FB_PIN_GATE_MIN * 60) {
      snprintf(buf, cap, "GATE %s", t.gate);
      *col = display.color565(0, 220, 220);
      return buf;
    }
    *col = display.color565(210, 210, 210);
    return "ON TIME";
  }
  case FB_TRK_DELAYED:
    snprintf(buf, cap, "DELAY %d", (int)late);
    *col = display.color565(255, 170, 0);
    return buf;
  case FB_TRK_TAXI:      *col = display.color565(0, 220, 220);  return "TAXI";
  case FB_TRK_ENROUTE:
    if (late >= (int32_t)(aero::kDelayS / 60)) {
      snprintf(buf, cap, "LATE %d", (int)late);
      *col = display.color565(255, 170, 0);
      return buf;
    }
    *col = display.color565(70, 140, 255);
    return "IN AIR";
  case FB_TRK_LANDED:    *col = display.color565(0, 200, 60);   return "LANDED";
  case FB_TRK_CANCELLED: *col = display.color565(255, 40, 40);  return "CANX";
  case FB_TRK_DIVERTED:  *col = display.color565(255, 60, 200); return "DIVERT";
  default:               *col = display.color565(120, 132, 138); return "";
  }
}

// Which tracked flight has the row now: they take turns with each swap.
static uint8_t pinnedIndex(uint32_t nowMs) {
  const uint8_t n = aeroTrackCount();
  return n ? (uint8_t)(((nowMs - s_altT0) / FB_ALT_MS) % n) : 0;
}

void flightboardTrackLine(uint8_t i, char *word, size_t wordCap, char *hm, size_t hmCap) {
  const AeroTracker *k = aeroTrack(i);
  word[0] = '\0';
  snprintf(hm, hmCap, "--:--");
  if (!k) return;
  char buf[16];
  uint16_t col;
  snprintf(word, wordCap, "%s", trackWord(*k, buf, sizeof(buf), &col));
  if (k->t.state == FB_TRK_WAIT || k->t.state == FB_TRK_NOTFOUND) return;
  fbz::Zone z;
  trackZone(k->t, aero::trackShowsArrival(k->t), &z);
  fbz::hm(aero::trackShownTime(k->t), z, hm, hmCap);
}

void flightboardTrackTimesJson(uint8_t i, JsonObject o) {
  const AeroTracker *k = aeroTrack(i);
  if (!k || k->t.state == FB_TRK_WAIT || k->t.state == FB_TRK_NOTFOUND) return;
  const aero::Track &t = k->t;
  for (bool arrival : {false, true}) {
    JsonObject e = o[arrival ? "arr" : "dep"];
    if (e.isNull()) continue;
    fbz::Zone z;
    const fbz::Source src = trackZone(t, arrival, &z);
    e["tz"]   = src == fbz::SRC_UTC ? "UTC" : (src == fbz::SRC_SENT ? (arrival ? t.toTz : t.fromTz) : listedZone(arrival ? t.to : t.from));
    e["zone"] = fbz::sourceName(src);                     // sent | list | utc
    char hm[6];
    for (const char *key : {"sched", "est", "act"}) {
      if (e[key].isNull()) continue;
      fbz::hm(e[key].as<long long>(), z, hm, sizeof(hm));
      char name[8];
      snprintf(name, sizeof(name), "%sHm", key);
      e[name] = hm;
    }
  }
  o["tmEnd"] = aero::trackShowsArrival(t) ? "arr" : "dep";   // which end the pinned time is
}

static void drawTracked(int16_t top, uint32_t nowMs) {
  const AeroTracker *k = aeroTrack(pinnedIndex(nowMs));
  if (!k) return;
  const aero::Track &t = k->t;
  const int16_t base = top + FB_ASCENT;
  int16_t bx, by; uint16_t bw, bh;
  display.fillRect(0, top, 128, FB_ROW_H - 1, display.color565(FB_PIN_BG_R, FB_PIN_BG_G, FB_PIN_BG_B));
  display.fillRect(0, top, 1, FB_ROW_H - 1, display.color565(FB_PIN_BAR_R, FB_PIN_BAR_G, FB_PIN_BAR_B));

  const bool known = t.state != FB_TRK_WAIT && t.state != FB_TRK_NOTFOUND;
  char hm[6] = "--:--";
  bool guessed = false;
  if (known) {
    // Departure in the origin's time, arrival in the destination's. Without a
    // zone for that end the time is UTC, drawn dim, rather than a guess.
    fbz::Zone z;
    guessed = trackZone(t, aero::trackShowsArrival(t), &z) == fbz::SRC_UTC;
    fbz::hm(aero::trackShownTime(t), z, hm, sizeof(hm));
  }
  display.setTextColor(guessed ? display.color565(120, 132, 138) : display.color565(235, 240, 245));
  display.setCursor(FB_X_TIME, base);
  display.print(hm);
  display.setTextColor(display.color565(235, 240, 245));

  const char *fn = known && t.fn[0] ? t.fn : k->ident;
  display.setCursor(FB_X_FLIGHT, base);
  display.print(fn);
  display.getTextBounds(fn, 0, 0, &bx, &by, &bw, &bh);
  const int16_t xRoute = FB_X_FLIGHT + (int16_t)bw + FB_PIN_ROUTE_GAP > FB_X_DEST
                           ? FB_X_FLIGHT + (int16_t)bw + FB_PIN_ROUTE_GAP
                           : FB_X_DEST;

  char buf[16];
  uint16_t col;
  const char *word = trackWord(*k, buf, sizeof(buf), &col);
  display.getTextBounds(word, 0, 0, &bx, &by, &bw, &bh);
  const int16_t xWord = FB_X_RIGHT - (int16_t)bw;
  display.setTextColor(col);
  display.setCursor(xWord, base);
  display.print(word);

  // The route, both ends if they fit, else where it is going, else nothing.
  if (!known) return;
  char route[12];
  snprintf(route, sizeof(route), "%s-%s", t.from, t.to);
  display.getTextBounds(route, 0, 0, &bx, &by, &bw, &bh);
  if (xRoute + (int16_t)bw + FB_GAP > xWord) {
    snprintf(route, sizeof(route), "%s", t.to);
    display.getTextBounds(route, 0, 0, &bx, &by, &bw, &bh);
    if (xRoute + (int16_t)bw + FB_GAP > xWord) return;
  }
  display.setTextColor(display.color565(170, 190, 200));
  display.setCursor(xRoute, base);
  display.print(route);
}
#endif

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

// The header's clock and cue as the page draws them now.
static void headerTime(bool direct, char *hm, size_t hmCap, char *cueTxt, size_t cueCap) {
  snprintf(hm, hmCap, "--:--");
  if (cueCap) cueTxt[0] = '\0';
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  if (direct) {
    // A station board's clock: the airport's time.
    const int64_t now = (int64_t)time(nullptr);
    if (now < 1700000000) return;
    fbz::hm(now, aptZone(), hm, hmCap);
    int32_t panel;
    if (panelOffset(now, &panel)) fbz::cue(fbz::offset(aptZone(), now), panel, cueTxt, cueCap);
    return;
  }
#endif
  // Home Assistant's rows are in its zone, so the clock stays in the panel's.
  struct tm lt;
  if (getLocalTime(&lt, 0)) snprintf(hm, hmCap, "%02d:%02d", lt.tm_hour, lt.tm_min);
#if defined(FB_MQTT_ENABLED)
  if (!direct) snprintf(cueTxt, cueCap, "HA");
#else
  (void)direct;
#endif
}

void flightboardStatusJson(JsonObject out) {
  const bool departures = shownDep();
  const FbBoard &b = s_b[departures];
  const bool direct = flightboardDirectOwns();
  out["have"] = b.have;
  // Which clock the times are in: the airport's, or Home Assistant's as it sent them.
  out["times"] = direct ? "airport" : "home-assistant";
  {
    FbAirport a;
    if (flightboardAirportById(s_aptId, &a)) out["tz"] = (const char *)a.tz;
    char hm[6], cueTxt[8];
    headerTime(direct, hm, sizeof(hm), cueTxt, sizeof(cueTxt));
    out["clock"] = hm;
    out["cue"]   = cueTxt;
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
    const int64_t now = (int64_t)time(nullptr);
    if (direct && now >= 1700000000 && aptZone().known) out["utcOffset"] = fbz::offset(aptZone(), now);
#endif
  }
#if defined(FB_MQTT_ENABLED)
  out["source"] = direct ? "aeroapi" : (flightboardAirportBuiltin(s_aptId) ? "mqtt" : "none");
#else
  out["source"] = direct ? "aeroapi" : "none";
#endif
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
  out["apt"]  = flightboardAirport();
  out["name"] = flightboardAirportLabel(s_aptId);
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

// What the page says when a half has nothing to show.
static const char *noBoardWord() {
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  if (flightboardDirectOwns()) return aeroDirectState();
#endif
  // Only the six built-in airports are served by Home Assistant.
  if (!flightboardAirportBuiltin(s_aptId)) return "NEEDS A KEY";
#if defined(FB_MQTT_ENABLED)
  // Say which of the several ways to have no data this is. "NO DATA" alone
  // sends you looking at Home Assistant when the panel never reached WiFi.
  return fbMqttStatus();
#else
  return "NO DATA";
#endif
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

  const char *apt = flightboardAirportLabel(s_aptId);
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
  // data is older than it should be - ten minutes for Home Assistant's boards;
  // for AeroAPI's, twice the past lists' floor and five minutes more, since
  // the budget, not a fault, is what keeps them that old.
  const bool direct = flightboardDirectOwns();
  bool stale;
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  if (direct)
    stale = b.have && s_directAgeS[departures] > (uint32_t)aeroDirectBudget().floorMin * 60UL * 4UL + 300UL;
  else
#endif
    stale = flightboardAge() > 600 || updAgeMin(b) > FB_FRESH_MIN;
  char nowHm[6], cueTxt[8];
  headerTime(direct, nowHm, sizeof(nowHm), cueTxt, sizeof(cueTxt));
  display.getTextBounds(nowHm, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(FB_X_RIGHT - (int16_t)bw, hbase);
  display.setTextColor(stale ? display.color565(150, 90, 0)
                             : display.color565(120, 132, 138));
  display.print(nowHm);
  if (cueTxt[0]) {
    display.getTextBounds(apt, 0, 0, &bx, &by, &bw, &bh);
    int16_t clear = FB_X_TIME + (int16_t)bw + 6;
    display.getTextBounds("DEPARTURES", 0, 0, &bx, &by, &bw, &bh);
    clear += (int16_t)bw + FB_CUE_GAP;
    display.getTextBounds(cueTxt, 0, 0, &bx, &by, &bw, &bh);
    const int16_t x = FB_X_RIGHT - FB_CLOCK_MAX_W - FB_CUE_GAP - (int16_t)bw;
    if (x >= clear) {
      display.setCursor(x, hbase);
      display.setTextColor(display.color565(120, 132, 138));
      display.print(cueTxt);
    }
  }

  display.drawFastHLine(0, FB_Y_RULE, 128, display.color565(52, 60, 64));
  // While both halves are here and swapping, the rule fills in dim amber
  // towards the next swap, so a change never comes as a surprise.
  if (s_mode == FB_DIR_ALT && s_b[0].have && s_b[1].have) {
    const int16_t w = (int16_t)(((nowMs - s_altT0) % FB_ALT_MS) * 128UL / FB_ALT_MS);
    if (w > 0) display.drawFastHLine(0, FB_Y_RULE, w, display.color565(110, 78, 0));
  }

  // A tracked flight takes the first row, on both halves.
  uint8_t firstRow = 0;
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  if (aeroTrackCount()) {
    drawTracked(FB_Y_ROW0, nowMs);
    firstRow = 1;
  }
#endif

  if (!b.have || b.count == 0) {
    display.setCursor(40, firstRow ? 38 : 34);
    display.setTextColor(display.color565(120, 120, 120));
    display.print(b.have ? "NO FLIGHTS" : noBoardWord());
    display.setFont(NULL);
    return;
  }

  // ── window ────────────────────────────────────────────────────────────────
  const uint8_t rowsFree = FB_VISIBLE - firstRow;
  int16_t start = (int16_t)b.nowIdx - 1;
  if (start + rowsFree > b.count) start = b.count - rowsFree;
  if (start < 0) start = 0;

  for (uint8_t i = 0; i < rowsFree; i++) {
    uint8_t idx = start + i;
    if (idx >= b.count) break;
    const FbRow &r = b.rows[idx];
    int16_t top = FB_Y_ROW0 + (i + firstRow) * FB_ROW_H;
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
    // The same name as the code (a city HA or AeroAPI did not send) says nothing more.
    if (!strcmp(city, r.ct)) city[0] = '\0';
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

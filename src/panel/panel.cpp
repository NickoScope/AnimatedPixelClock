#include "panel.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "../control/carousel.h"
#include "../control/control.h"
#include "../flightboard/fb_mqtt.h"
#include "../flightboard/flightboard.h"
#include "../railboard/railboard.h"
#include "../worldclock/worldclock.h"

// Same reasoning as the clock style's deferred save: a slider dragged across its
// range or a knob spun through six airports is one write, not dozens.
static const uint32_t SETTLE_MS = 2500;
static const char *const NS = "panel";

struct PanelState {
  uint16_t      pages;
  PanelCarousel car;
  PanelKnob     knob;
  uint8_t       fbAirport;
  bool          fbDep;
  uint8_t       wcHome;
  char          rbStn[4];        // "" in a build without the rail board
};

static const uint16_t ALL_PAGES = (uint16_t)((1u << PANEL_KEY_COUNT) - 1);

static PanelState s_cur;
static PanelState s_saved;       // what NVS holds, field by field
static uint32_t   s_dirtyAt = 0;

static void markDirty() {
  s_dirtyAt = millis();
  if (!s_dirtyAt) s_dirtyAt = 1;   // 0 means clean
}

PanelKnob panelKnobDefaults() {
  PanelKnob k;
  k.reverse    = CTRL_REVERSE != 0;
  k.lockoutMs  = CTRL_ENC_LOCKOUT_MS;
  k.debounceMs = CTRL_SW_DEBOUNCE_MS;
  k.detent     = (int8_t)CTRL_ENC_HALF_DETENT;
  return k;
}

static PanelCarousel carouselDefaults() {
  PanelCarousel c;
#if defined(CAROUSEL_ENABLED)
  c.enabled = true;
  c.idleS   = CAROUSEL_IDLE_S;
#if defined(CAROUSEL_ALL_STYLES)
  c.slotS     = CAROUSEL_SLOT_S;
  c.allStyles = true;
#else
  c.slotS     = 0;                  // the pages keep their own times, as before
  c.allStyles = false;
#endif
#else
  c.enabled   = false;              // no carousel compiled: nothing reads these
  c.idleS     = PANEL_IDLE_MIN_S;
  c.slotS     = 0;
  c.allStyles = false;
#endif
  return c;
}

static bool carouselValid(const PanelCarousel &c) {
  if (c.idleS < PANEL_IDLE_MIN_S || c.idleS > PANEL_IDLE_MAX_S) return false;
  if (c.slotS != 0 && (c.slotS < PANEL_SLOT_MIN_S || c.slotS > PANEL_SLOT_MAX_S)) return false;
  return true;
}

static bool knobValid(const PanelKnob &k) {
  return k.lockoutMs <= PANEL_LOCKOUT_MAX_MS &&
         k.debounceMs >= PANEL_DEBOUNCE_MIN_MS && k.debounceMs <= PANEL_DEBOUNCE_MAX_MS &&
         k.detent >= -1 && k.detent <= 1;
}

static void applyKnob() {
  controlConfigure(s_cur.knob.reverse, s_cur.knob.lockoutMs, s_cur.knob.debounceMs, s_cur.knob.detent);
}

static void applyCarousel() {
#if defined(CAROUSEL_ENABLED)
  carouselConfigure(s_cur.car.enabled, s_cur.car.idleS);
#endif
}

void panelBegin() {
  s_cur.pages = ALL_PAGES;
  s_cur.car   = carouselDefaults();
  s_cur.knob  = panelKnobDefaults();
#if defined(FLIGHTBOARD_ENABLED)
  s_cur.fbAirport = flightboardAirportIndex();
  s_cur.fbDep     = flightboardDeparturesSelected();
#else
  s_cur.fbAirport = 0;
  s_cur.fbDep     = false;
#endif
  s_cur.wcHome = 0;
#if defined(RAILBOARD_ENABLED)
  memcpy(s_cur.rbStn, RB_CRS, sizeof(s_cur.rbStn));
#else
  s_cur.rbStn[0] = '\0';
#endif

  // Read-write on purpose: a read-only open of a namespace that does not exist
  // yet fails with a logged error on every boot until the first save. Opening
  // read-write creates the namespace once; the numeric getters below only log
  // at verbose level for a missing key (Preferences.cpp, arduino-esp32 2.0.17).
  Preferences p;
  if (p.begin(NS, false)) {
    PanelState d = s_cur;
    s_cur.pages = (uint16_t)((p.getUShort("pages", d.pages) & ALL_PAGES) | (1u << PANEL_KEY_CLOCK));

    PanelCarousel c;
    c.enabled   = p.getUChar("carOn", d.car.enabled) != 0;
    c.idleS     = p.getUShort("carIdle", d.car.idleS);
    c.slotS     = p.getUShort("carSlot", d.car.slotS);
    c.allStyles = p.getUChar("carAll", d.car.allStyles) != 0;
    if (carouselValid(c)) s_cur.car = c;   // a value from another build's bounds: defaults

    PanelKnob k;
    k.reverse    = p.getUChar("knRev", d.knob.reverse) != 0;
    k.lockoutMs  = p.getUShort("knLock", d.knob.lockoutMs);
    k.debounceMs = p.getUShort("knDeb", d.knob.debounceMs);
    k.detent     = p.getChar("knDet", d.knob.detent);
    if (knobValid(k)) s_cur.knob = k;

#if defined(FLIGHTBOARD_ENABLED)
    const uint8_t apt = p.getUChar("fbApt", d.fbAirport);
    if (apt < flightboardAirportCount()) s_cur.fbAirport = apt;
    s_cur.fbDep = p.getUChar("fbDep", d.fbDep) != 0;
#endif
#if defined(WORLDCLOCK_ENABLED)
    const uint8_t home = p.getUChar("wcHome", d.wcHome);
    if (home < worldClockCityCount()) s_cur.wcHome = home;
#endif
#if defined(RAILBOARD_ENABLED)
    // isKey() first: unlike the numeric getters, getString() logs at error level
    // for a key never written (Preferences.cpp, arduino-esp32 2.0.17), which
    // would be every boot until the portal first sets a station. isKey() probes
    // with nvs_get_* and logs nothing.
    if (p.isKey("rbStn")) {
      const String stn = p.getString("rbStn", "");
      if (railboardValidStation(stn.c_str())) memcpy(s_cur.rbStn, stn.c_str(), sizeof(s_cur.rbStn));
    }
#endif
    p.end();
  }
  // What was read is what NVS holds; a field never stored holds its default,
  // and writing the default back would change nothing.
  s_saved = s_cur;

  applyKnob();
  applyCarousel();
#if defined(FLIGHTBOARD_ENABLED)
  flightboardSelect(s_cur.fbAirport, s_cur.fbDep);
#endif
#if defined(WORLDCLOCK_ENABLED)
  worldClockSetHome(s_cur.wcHome);
#endif
#if defined(RAILBOARD_ENABLED)
  railboardSetStation(s_cur.rbStn);   // before railboardBegin() subscribes
#endif
}

void panelTick() {
  if (!s_dirtyAt || (millis() - s_dirtyAt) < SETTLE_MS) return;
  Preferences p;
  if (!p.begin(NS, false)) { markDirty(); return; }   // try again after another settle
  s_dirtyAt = 0;
  // Each put commits by itself. A failed one leaves its field marked unsaved,
  // and the next change retries it.
  PanelState &c = s_cur, &w = s_saved;
  if (c.pages != w.pages && p.putUShort("pages", c.pages)) w.pages = c.pages;
  if (c.car.enabled != w.car.enabled && p.putUChar("carOn", c.car.enabled)) w.car.enabled = c.car.enabled;
  if (c.car.idleS != w.car.idleS && p.putUShort("carIdle", c.car.idleS)) w.car.idleS = c.car.idleS;
  if (c.car.slotS != w.car.slotS && p.putUShort("carSlot", c.car.slotS)) w.car.slotS = c.car.slotS;
  if (c.car.allStyles != w.car.allStyles && p.putUChar("carAll", c.car.allStyles)) w.car.allStyles = c.car.allStyles;
  if (c.knob.reverse != w.knob.reverse && p.putUChar("knRev", c.knob.reverse)) w.knob.reverse = c.knob.reverse;
  if (c.knob.lockoutMs != w.knob.lockoutMs && p.putUShort("knLock", c.knob.lockoutMs)) w.knob.lockoutMs = c.knob.lockoutMs;
  if (c.knob.debounceMs != w.knob.debounceMs && p.putUShort("knDeb", c.knob.debounceMs)) w.knob.debounceMs = c.knob.debounceMs;
  if (c.knob.detent != w.knob.detent && p.putChar("knDet", c.knob.detent)) w.knob.detent = c.knob.detent;
  if (c.fbAirport != w.fbAirport && p.putUChar("fbApt", c.fbAirport)) w.fbAirport = c.fbAirport;
  if (c.fbDep != w.fbDep && p.putUChar("fbDep", c.fbDep)) w.fbDep = c.fbDep;
  if (c.wcHome != w.wcHome && p.putUChar("wcHome", c.wcHome)) w.wcHome = c.wcHome;
  if (strcmp(c.rbStn, w.rbStn) && p.putString("rbStn", c.rbStn)) memcpy(w.rbStn, c.rbStn, sizeof(w.rbStn));
  p.end();
}

bool panelPageEnabled(uint8_t key) {
  if (key >= PANEL_KEY_COUNT) return true;
  return (s_cur.pages >> key) & 1u;
}

bool panelSetPageEnabled(uint8_t key, bool on) {
  if (key >= PANEL_KEY_COUNT) return false;
  if (key == PANEL_KEY_CLOCK && !on) return false;   // the knob needs somewhere to land
  const uint16_t bit = (uint16_t)(1u << key);
  const uint16_t next = on ? (uint16_t)(s_cur.pages | bit) : (uint16_t)(s_cur.pages & ~bit);
  if (next != s_cur.pages) { s_cur.pages = next; markDirty(); }
  return true;
}

const PanelCarousel &panelCarousel() { return s_cur.car; }

bool panelSetCarousel(const PanelCarousel &c) {
  if (!carouselValid(c)) return false;
  const PanelCarousel &o = s_cur.car;
  if (c.enabled == o.enabled && c.idleS == o.idleS && c.slotS == o.slotS && c.allStyles == o.allStyles)
    return true;
  s_cur.car = c;
  applyCarousel();
  markDirty();
  return true;
}

const PanelKnob &panelKnob() { return s_cur.knob; }

bool panelSetKnob(const PanelKnob &k) {
  if (!knobValid(k)) return false;
  const PanelKnob &o = s_cur.knob;
  if (k.reverse == o.reverse && k.lockoutMs == o.lockoutMs && k.debounceMs == o.debounceMs &&
      k.detent == o.detent)
    return true;
  s_cur.knob = k;
  applyKnob();
  markDirty();
  return true;
}

bool panelSetFlightboard(uint8_t airport, bool departures) {
#if defined(FLIGHTBOARD_ENABLED)
  if (airport >= flightboardAirportCount()) return false;
  if (airport == flightboardAirportIndex() && departures == flightboardDeparturesSelected()) return true;
  flightboardSelect(airport, departures);
#if defined(FB_MQTT_ENABLED)
  fbMqttSelectionChanged();   // resubscribes once the selection settles
#endif
  panelNoteFlightboard();
  return true;
#else
  (void)airport; (void)departures;
  return false;
#endif
}

void panelNoteFlightboard() {
#if defined(FLIGHTBOARD_ENABLED)
  s_cur.fbAirport = flightboardAirportIndex();
  s_cur.fbDep     = flightboardDeparturesSelected();
  if (s_cur.fbAirport != s_saved.fbAirport || s_cur.fbDep != s_saved.fbDep) markDirty();
#endif
}

bool panelSetWorldHome(uint8_t city) {
#if defined(WORLDCLOCK_ENABLED)
  if (city >= worldClockCityCount()) return false;
  if (city == s_cur.wcHome) return true;
  s_cur.wcHome = city;
  worldClockSetHome(city);
  markDirty();
  return true;
#else
  (void)city;
  return false;
#endif
}

uint8_t panelWorldHome() { return s_cur.wcHome; }

bool panelSetRailStation(const char *crs) {
#if defined(RAILBOARD_ENABLED)
  if (!railboardSetStation(crs)) return false;   // validates: three capitals A-Z
  if (strcmp(s_cur.rbStn, crs)) {
    memcpy(s_cur.rbStn, crs, sizeof(s_cur.rbStn));
    markDirty();
  }
  return true;
#else
  (void)crs;
  return false;
#endif
}

const char *panelRailStation() { return s_cur.rbStn; }

#endif  // CONTROL_ENCODER_ENABLED

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
#include "../worldclock/wc_home.h"
#include "../worldclock/worldclock.h"

// Same reasoning as the clock style's deferred save: a slider dragged across its
// range or a knob spun through six airports is one write, not dozens.
static const uint32_t SETTLE_MS = 2500;
static const char *const NS = "panel";
static const uint8_t WC_HOME_UNSET = 255;

#if defined(WORLDCLOCK_ENABLED)
// One custom city as NVS keeps it: fixed size and versioned, so a record from
// another layout is recognised by its length or its first byte and skipped.
// Six of these are 804 bytes of blob. Six is the cap because the map is 64x32
// dots and Europe alone already holds three of the built-in cities within a
// few dots of each other: twelve orange dots is about what the map carries
// before they stop reading as places, and the portal's list stays one screen.
static const uint8_t WC_RECORD_V = 1;
struct __attribute__((packed)) WcRecord {
  uint8_t v;
  char    name[WC_NAME_MAX + 1];
  float   lat, lon;
  char    posix[WC_POSIX_MAX + 1];
  char    iana[WC_IANA_MAX + 1];
};

static void toRecord(const WcCity &c, WcRecord *r) {
  memset(r, 0, sizeof(*r));
  r->v = WC_RECORD_V;
  memcpy(r->name, c.name, sizeof(r->name));
  r->lat = c.lat;
  r->lon = c.lon;
  memcpy(r->posix, c.posix, sizeof(r->posix));
  memcpy(r->iana, c.iana, sizeof(r->iana));
}

static void fromRecord(const WcRecord &r, WcCity *c) {
  memset(c, 0, sizeof(*c));
  memcpy(c->name, r.name, sizeof(c->name) - 1);      // the last byte stays 0 whatever was stored
  c->lat = r.lat;
  c->lon = r.lon;
  memcpy(c->posix, r.posix, sizeof(c->posix) - 1);
  memcpy(c->iana, r.iana, sizeof(c->iana) - 1);
}

static bool sameCity(const WcCity &a, const WcCity &b) {
  return !strcmp(a.name, b.name) && a.lat == b.lat && a.lon == b.lon && !strcmp(a.posix, b.posix) &&
         !strcmp(a.iana, b.iana);
}

static void slotKey(uint8_t slot, char key[6]) { snprintf(key, 6, "wcC%u", (unsigned)slot); }

static WcCity s_wcSaved[WC_CUSTOM_MAX];     // what NVS holds, slot by slot
static bool   s_wcSavedUsed[WC_CUSTOM_MAX];
#endif

struct PanelState {
  uint16_t      pages;
  PanelCarousel car;
  PanelKnob     knob;
  uint8_t       fbAirport;
  uint8_t       fbDir;
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
  s_cur.fbDir     = flightboardDirMode();
#else
  s_cur.fbAirport = 0;
  s_cur.fbDir     = 2;
#endif
  s_cur.wcHome = WC_HOME_UNSET;     // never chosen: home follows the panel's location
#if defined(RAILBOARD_ENABLED)
  memcpy(s_cur.rbStn, RB_CRS, sizeof(s_cur.rbStn));
#else
  s_cur.rbStn[0] = '\0';
#endif

  // Read-write on purpose: a read-only open of a namespace that does not exist
  // yet fails with a logged error on every boot until the first save. Opening
  // read-write creates the namespace once; the numeric getters below only log
  // at verbose level for a missing key (Preferences.cpp, arduino-esp32 2.0.17).
  uint8_t storedHome = WC_HOME_UNSET;   // what NVS holds, even when it is no city any more
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
    // fbDir replaces the old arrivals-or-departures key, so a choice saved
    // before the board could swap does not pin it to one half.
    const uint8_t dir = p.getUChar("fbDir", d.fbDir);
    if (dir <= FB_DIR_ALT) s_cur.fbDir = dir;
#endif
#if defined(WORLDCLOCK_ENABLED)
    // Custom cities before home: a stored home may be one of them. isKey()
    // first, since getBytes() logs at error level for a key never written
    // (Preferences.cpp, arduino-esp32 2.0.17). Each record is checked again
    // on the way in; one that fails is left out, not repaired.
    for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++) {
      char key[6];
      slotKey(i, key);
      WcRecord r;
      if (!p.isKey(key) || p.getBytesLength(key) != sizeof(r) || p.getBytes(key, &r, sizeof(r)) != sizeof(r) ||
          r.v != WC_RECORD_V)
        continue;
      WcCity c;
      fromRecord(r, &c);
      if (worldClockSetCustom(i, &c)) {
        s_wcSaved[i] = c;
        s_wcSavedUsed[i] = true;
      }
    }
    // Absent means never chosen, which is no longer the same thing as city 0.
    if (p.isKey("wcHome")) {
      WcCity c;
      storedHome = p.getUChar("wcHome", WC_HOME_UNSET);
      if (worldClockCity(storedHome, &c)) s_cur.wcHome = storedHome;
    }
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
  // A stored home that is no city any more (its slot failed its check) is
  // removed at the first save, so a city later added to that slot is not
  // mistaken for the owner's choice.
  s_saved.wcHome = storedHome;
  if (s_saved.wcHome != s_cur.wcHome) markDirty();

  applyKnob();
  applyCarousel();
#if defined(FLIGHTBOARD_ENABLED)
  flightboardSelect(s_cur.fbAirport, (FbDirMode)s_cur.fbDir);
#endif
#if defined(WORLDCLOCK_ENABLED)
  if (s_cur.wcHome != WC_HOME_UNSET) worldClockSetHome(s_cur.wcHome, true);
  wcHomeBegin();                        // nobody chose: the location, or the zone
#endif
#if defined(RAILBOARD_ENABLED)
  railboardSetStation(s_cur.rbStn);   // before railboardBegin() subscribes
#endif
}

void panelTick() {
#if defined(WORLDCLOCK_ENABLED)
  wcHomeTick();
#endif
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
  if (c.fbDir != w.fbDir && p.putUChar("fbDir", c.fbDir)) w.fbDir = c.fbDir;
  if (c.wcHome != w.wcHome) {
    // remove() only when NVS holds the key: it logs at error level otherwise.
    const bool ok = c.wcHome == WC_HOME_UNSET ? p.remove("wcHome") : p.putUChar("wcHome", c.wcHome) != 0;
    if (ok) w.wcHome = c.wcHome;
  }
#if defined(WORLDCLOCK_ENABLED)
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++) {
    WcCity now;
    const bool used = worldClockCity((uint8_t)(WC_ID_CUSTOM + i), &now);
    if (used == s_wcSavedUsed[i] && (!used || sameCity(now, s_wcSaved[i]))) continue;
    char key[6];
    slotKey(i, key);
    bool ok;
    if (used) {
      WcRecord r;
      toRecord(now, &r);
      ok = p.putBytes(key, &r, sizeof(r)) == sizeof(r);
    } else {
      ok = p.remove(key);
    }
    if (ok) {
      s_wcSavedUsed[i] = used;
      if (used) s_wcSaved[i] = now;
    }
  }
#endif
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

bool panelSetFlightboard(uint8_t airport, uint8_t dirMode) {
#if defined(FLIGHTBOARD_ENABLED)
  if (airport >= flightboardAirportCount() || dirMode > FB_DIR_ALT) return false;
  if (airport == flightboardAirportIndex() && dirMode == flightboardDirMode()) return true;
  flightboardSelect(airport, (FbDirMode)dirMode);
#if defined(FB_MQTT_ENABLED)
  fbMqttSelectionChanged();   // resubscribes once the selection settles
#endif
  panelNoteFlightboard();
  return true;
#else
  (void)airport; (void)dirMode;
  return false;
#endif
}

void panelNoteFlightboard() {
#if defined(FLIGHTBOARD_ENABLED)
  s_cur.fbAirport = flightboardAirportIndex();
  s_cur.fbDir     = flightboardDirMode();
  if (s_cur.fbAirport != s_saved.fbAirport || s_cur.fbDir != s_saved.fbDir) markDirty();
#endif
}

#if defined(WORLDCLOCK_ENABLED)
static bool nameTaken(const char *name) {
  WcCity c;
  for (uint8_t i = 0; i < worldClockDefaultCount(); i++)
    if (worldClockCity(i, &c) && !strcmp(c.name, name)) return true;
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++)
    if (worldClockCity((uint8_t)(WC_ID_CUSTOM + i), &c) && !strcmp(c.name, name)) return true;
  return false;
}

const char *panelAddWorldCity(const WcCity &c, uint8_t *id) {
  if (const char *why = worldClockCheck(c)) return why;
  // Two dots under one name would leave the portal's list with two rows nobody
  // can tell apart.
  if (nameTaken(c.name)) return "a city with that name is already on the map";
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++) {
    if (worldClockSlotUsed(i)) continue;
    worldClockSetCustom(i, &c);
    *id = (uint8_t)(WC_ID_CUSTOM + i);
    markDirty();
    wcHomeRethink();                    // an unchosen home may now have a nearer city
    return nullptr;
  }
  return "all six custom cities are in use: delete one first";
}

const char *panelSetWorldHome(uint8_t id) {
  WcCity c;
  if (!worldClockCity(id, &c)) return "no city with that id";
  if (id == WC_ID_AUTO) {
    // Made at the location and never stored: choosing it keeps it, or the
    // choice would point at nothing after a reboot.
    if (const char *why = panelAddWorldCity(c, &id)) return why;
  }
  worldClockSetHome(id, true);
  if (s_cur.wcHome != id) {
    s_cur.wcHome = id;
    markDirty();
  }
  wcHomeRethink();
  return nullptr;
}

const char *panelRemoveWorldCity(uint8_t id) {
  if (id < WC_ID_CUSTOM || id >= WC_ID_CUSTOM + WC_CUSTOM_MAX || !worldClockSlotUsed((uint8_t)(id - WC_ID_CUSTOM)))
    return "only a custom city can be deleted";
  if (s_cur.wcHome == id) {             // the choice goes with the city
    s_cur.wcHome = WC_HOME_UNSET;
    worldClockSetHome(id, false);
  }
  worldClockSetCustom((uint8_t)(id - WC_ID_CUSTOM), nullptr);
  markDirty();
  wcHomeRethink();
  return nullptr;
}

void panelForgetWorldHome() {
  worldClockSetHome(worldClockHome(), false);   // shown until the location decides, a moment later
  if (s_cur.wcHome != WC_HOME_UNSET) {
    s_cur.wcHome = WC_HOME_UNSET;
    markDirty();
  }
  wcHomeRethink();
}
#endif

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

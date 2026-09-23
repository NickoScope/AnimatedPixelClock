#pragma once
// Panel settings the web portal changes while the panel runs.
//
// Which pages the knob and the carousel visit, the carousel's timing, the flight
// board's selection, the world clock's home city and the knob's feel. Each one
// starts at the compile-time value it replaces, so a panel that never opens the
// portal behaves exactly as it did before this existed.
//
// Values live in RAM and reach NVS namespace "panel" only once changes stop
// arriving: a web request, a knob detent or the carousel never writes flash by
// itself. Only fields that differ from what was last written are written.
//
//   pages    u16  bit per PanelPageKey, set = the knob and the carousel visit it
//   pgKnown  u32  known << 16 | pages: the keys the build that wrote "pages" knew, and the value it wrote.
//                 A key added since starts on; absent, or its pages half not the stored "pages" (an older
//                 firmware rewrote it), and the mask is taken to know CLOCK..CARDS only (panel_pages.h)
//   carOn    u8   carousel advances on its own
//   carIdle  u16  seconds without the knob before it starts
//   carSlot  u16  seconds per page, 0 = each page its own time
//   carAll   u8   on the clock page, walk every style first
//   fbApt    u8   flight board airport, an id (fb_model.h: 0-5 the built-in list, the
//                 indices it held before custom airports; 100-105 a custom one)
//   fbA0-5   blob flight board custom airport in slot 0-5, an FbRecord (panel.cpp); absent = empty
//   fbDir    u8   0 arrivals, 1 departures, 2 both in turn (default)
//   wcHome   u8   world clock home, a city id (worldclock.h: 0.. built in, 100.. custom).
//                 Absent = never chosen, and home follows the panel's location (wc_home.h)
//   wcC0-5   blob world clock custom city in slot 0-5, a WcRecord (panel.cpp); absent = empty
//   rbStn    str  rail board station, exactly three capitals A-Z (default RB_CRS)
//   knRev    u8   knob direction reversed
//   knLock   u16  ms after a step during which the decoder ignores the knob
//   knDeb    u16  ms the switch must be steady
//   knDet    i8   -1 learn from the knob, 0 detents at 11, 1 at 11 and 00

#include <stdint.h>

#if defined(CONTROL_ENCODER_ENABLED)

// Stable page keys, stored as bits: never renumber. A build without a module
// simply never lists its key.
enum PanelPageKey : uint8_t {
  PANEL_KEY_CLOCK = 0,      // the fallback page: cannot be switched off
  PANEL_KEY_WORLD,
  PANEL_KEY_FLIGHTS,
  PANEL_KEY_TRAINS,
  PANEL_KEY_YACHTS,
  PANEL_KEY_CARDS,          // every live card, as one switch
  // Appended rather than placed by page order: the switches are bits in NVS
  // "pages", and a key inserted earlier would move every bit after it.
  PANEL_KEY_LUA,            // every Lua effect page, as one switch
  PANEL_KEY_MEDIA,          // the media player's now-playing page
  PANEL_KEY_MARKET,         // the four market pages, as one switch
  PANEL_KEY_COUNT,
  PANEL_KEY_NONE = 0xFF,    // a page this module does not know: always visited
};

// Input bounds. Guards against nonsense from a script, not tuned values: the
// defaults are the compile-time ones in control.h and carousel.h.
#define PANEL_IDLE_MIN_S        5
#define PANEL_IDLE_MAX_S        3600
#define PANEL_SLOT_MIN_S        5      // 0 is also accepted: each page its own time
#define PANEL_SLOT_MAX_S        3600
#define PANEL_LOCKOUT_MAX_MS    250
#define PANEL_DEBOUNCE_MIN_MS   2
#define PANEL_DEBOUNCE_MAX_MS   200

struct PanelCarousel {
  bool     enabled;
  uint16_t idleS;
  uint16_t slotS;
  bool     allStyles;
};

struct PanelKnob {
  bool     reverse;
  uint16_t lockoutMs;
  uint16_t debounceMs;
  int8_t   detent;
};

void panelBegin();          // setup(): before controlBegin() and fbMqttBegin()
void panelTick();           // loop(): the deferred write

bool     panelPageEnabled(uint8_t key);          // PANEL_KEY_NONE is always true
bool     panelSetPageEnabled(uint8_t key, bool on);
// Each Lua effect can be left out of the knob's walk and the carousel on its
// own, by its name, beside the switch for all of them (PANEL_KEY_LUA). Kept in
// NVS. A name, not a number: an upload or a delete renumbers the effects.
// "Show" still shows an effect that is switched off.
bool panelEffectOn(const char *name);
void panelSetEffectOn(const char *name, bool on);
void panelEffectsPrune();   // forget names no effect has any more (after a delete)

const PanelCarousel &panelCarousel();
bool     panelSetCarousel(const PanelCarousel &c);   // false: out of range, nothing changed

const PanelKnob &panelKnob();
PanelKnob panelKnobDefaults();
bool     panelSetKnob(const PanelKnob &k);

bool     panelSetFlightboard(uint8_t airport, uint8_t dirMode);   // an airport id; FbDirMode
void     panelNoteFlightboard();   // the knob moved the selection: keep it, later

#if defined(FLIGHTBOARD_DIRECT_ENABLED)
#include "../flightboard/fb_model.h"
// Custom airports. nullptr when done, else why not in words the portal can
// show: the airport's own check, a code already listed, or a full list.
const char *panelAddFlightAirport(const FbAirport &a, uint8_t *id);
const char *panelRemoveFlightAirport(uint8_t id);   // custom ids only
#endif

#if defined(WORLDCLOCK_ENABLED)
#include "../worldclock/worldclock.h"
// The world clock's home and custom cities. Each returns nullptr when done, or
// why not in words the portal can show; the caller has already checked the
// input with worldClockCheck, so what is left is a full list or a name taken.
const char *panelSetWorldHome(uint8_t id);    // chosen; the city made at the location is kept first
const char *panelAddWorldCity(const WcCity &c, uint8_t *id);
const char *panelRemoveWorldCity(uint8_t id); // custom ids only; a removed home is forgotten
void        panelForgetWorldHome();           // home follows the panel's location again
#endif
uint8_t  panelWorldHome();                    // the chosen city's id, 255 when none is

// The rail board's station: validated, handed to the rail board (which
// resubscribes and tells Home Assistant), and kept. False: not three capitals
// A-Z, or no rail board in this build.
bool        panelSetRailStation(const char *crs);
const char *panelRailStation();      // "" without a rail board

// ---- implemented in main.cpp, where the page enum and the knob state live
// A page is an index in knob order: the fixed pages, then one per live card.
uint8_t     panelPageCount();
uint8_t     panelPageKey(uint8_t page);          // PANEL_KEY_CARDS for a card
const char *panelPageName(uint8_t page);
uint8_t     panelCurrentPage();
bool        panelEnteredPage();                  // a click has entered the page
bool        panelShowPage(uint8_t page);         // on screen now, held like a knob turn
bool        panelShowStyle(uint8_t styleId);     // the clock page, in this style
uint16_t    panelPageSeconds(uint8_t page);      // what the carousel gives it

#endif  // CONTROL_ENCODER_ENABLED

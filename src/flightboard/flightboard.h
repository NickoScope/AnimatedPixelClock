/*
 * AnimatedPixelClock - airport flight board page
 *
 * Renders an arrivals/departures board from data pushed by Home Assistant.
 *
 * Transport-agnostic on purpose: the page owns parsing and rendering, and takes
 * its data through flightboardIngest(). MQTT is the intended source, but a test
 * or a serial command can feed it the same way.
 *
 * Payload contract (retained on nickoscope_watch/flightboard/state/<apt>/<dir>):
 *   {"apt":"LFMN","dir":"arr","n":15,"upd":"08:30","now_idx":7,
 *    "f":[{"fn":"SK791","tm":"08:34","st":"dep","ct":"CPH","cy":"COPENHAGEN"}, ...]}
 *
 * st is a closed vocabulary: sched | board | dep | land | delay | canc
 * ct is the 3-letter IATA code. cy is an optional city name - when Home
 * Assistant sends it the page shows it, otherwise it falls back to ct. The
 * column is 14 characters wide, so nothing is lost by sending names.
 */
#ifndef FLIGHTBOARD_H
#define FLIGHTBOARD_H

#if defined(FLIGHTBOARD_ENABLED)

#include <stdint.h>
#include <ArduinoJson.h>

#include "fb_model.h"   // sizes, FbStatus, FbAirport, the tracked-flight states

// Feed the page a payload. Returns false and leaves the previous board intact
// if the JSON is malformed - a bad message must not blank a working display.
bool flightboardIngest(const char *json, uint16_t len);

// True once a payload has been accepted.
bool flightboardHasData();

// Seconds since the last accepted payload, for the staleness indicator.
uint32_t flightboardAge();

// Draw the whole page. Assumes the caller has cleared the display.
void flightboardRender();

// Arrivals, departures, or both taking turns on screen.
enum FbDirMode : uint8_t { FB_DIR_ARR = 0, FB_DIR_DEP = 1, FB_DIR_ALT = 2 };
#define FB_ALT_SECONDS 10

// The selected airport (ICAO), stepped by the knob through the built-in ones
// and then the custom ones. Moving it drops both boards, so the old airport's
// rows never show under the new name.
const char *flightboardAirport();
void flightboardStepAirport(int8_t delta);

// Airports by id (fb_model.h): 0.. built in, FB_APT_CUSTOM + slot added in the
// portal. The caller tells the transport (fbMqttSelectionChanged) - these only
// record the choice.
uint8_t     flightboardAirportId();
bool        flightboardAirportById(uint8_t id, FbAirport *out);   // false: no such airport now
bool        flightboardAirportBuiltin(uint8_t id);                // one Home Assistant serves
uint8_t     flightboardBuiltinCount();
const char *flightboardAirportLabel(uint8_t id);                  // the name the page shows, "" if none
bool        flightboardSelect(uint8_t airport, FbDirMode mode);   // false: no such airport, nothing changed

// Custom airports. The portal checks with flightboardAirportCheck first; src/panel
// keeps them in NVS. Emptying the selected one selects Nice.
const char *flightboardAirportCheck(const FbAirport &a);          // nullptr, or why not, naming the field
bool        flightboardSetCustomAirport(uint8_t slot, const FbAirport *a);   // nullptr empties the slot
bool        flightboardCustomUsed(uint8_t slot);
int         flightboardNameWidth(const char *name);               // pixels, as the header draws it

FbDirMode   flightboardDirMode();
const char *flightboardModeKey();                 // "arr", "dep" or "alt"
bool        flightboardShowingDepartures();       // the half on screen now
bool        flightboardWants(bool departures);    // the mode shows this half
bool        flightboardHasFreshBoard(bool departures); // arrived, fetched within 30 min

// With an AeroAPI key stored the page is filled from AeroAPI and the MQTT
// transport stands aside. Always false without FLIGHTBOARD_DIRECT_ENABLED.
bool flightboardDirectOwns();

// Loop task, every pass: the direct fetch and the boards built from it.
// onScreen: the page is being drawn now.
void flightboardTick(bool onScreen);

#if defined(FLIGHTBOARD_DIRECT_ENABLED)
// Tracked flight i as the pinned row prints it: the status word and HH:MM, the
// departure in the origin's local time or the arrival in the destination's.
void flightboardTrackLine(uint8_t i, char *word, size_t wordCap, char *hm, size_t hmCap);
// Adds to aeroDirectTracksJson's dep and arr objects each end's zone (tz, and
// zone: sent | list | utc) and schedHm / estHm / actHm in it, and tmEnd.
void flightboardTrackTimesJson(uint8_t i, JsonObject o);
#endif

// The board as it stands: what the last payload was for (which can lag the
// selection by one fetch), its age, and its rows.
void flightboardStatusJson(JsonObject out);

#endif  // FLIGHTBOARD_ENABLED
#endif  // FLIGHTBOARD_H

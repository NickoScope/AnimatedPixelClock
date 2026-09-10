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

#define FB_MAX_ROWS 15   // the HA side sends at most 15
#define FB_FN_LEN   9
#define FB_TM_LEN   6
#define FB_CT_LEN   5
#define FB_CY_LEN   15   // city name, or ct when HA sends no name
#define FB_CITY_MAX 10   // characters shown in the destination column

enum FbStatus : uint8_t {
  FB_SCHED = 0, FB_BOARD, FB_DEP, FB_LAND, FB_DELAY, FB_CANC, FB_UNKNOWN
};

// Feed the page a payload. Returns false and leaves the previous board intact
// if the JSON is malformed - a bad message must not blank a working display.
bool flightboardIngest(const char *json, uint16_t len);

// True once a payload has been accepted.
bool flightboardHasData();

// Seconds since the last accepted payload, for the staleness indicator.
uint32_t flightboardAge();

// Draw the whole page. Assumes the caller has cleared the display.
void flightboardRender();

// Selector, driven by the encoder later. Returns the value to request.
const char *flightboardAirport();
const char *flightboardDirection();
void flightboardStepAirport(int8_t delta);
void flightboardToggleDirection();

#endif  // FLIGHTBOARD_ENABLED
#endif  // FLIGHTBOARD_H

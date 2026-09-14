#include "fb_mqtt.h"

#if defined(FLIGHTBOARD_ENABLED) && defined(FB_MQTT_ENABLED)

#include <Arduino.h>
#include <string.h>

#include "../mqtt/mqtt_bus.h"
#include "flightboard.h"

#define FB_TOPIC_REQ   "nickoscope_watch/flightboard/req"
#define FB_TOPIC_STATE "nickoscope_watch/flightboard/state"

// The knob steps one airport per detent. Acting on each would resubscribe six
// times and could fire six AeroAPI fetches at roughly a cent each, so the
// selection has to stop moving first.
static const uint32_t FB_SETTLE_MS = 1200;

// A retained payload lands within milliseconds of subscribing - measured
// against the live broker, not assumed. Only when none does is the board
// actually missing, and only then is a request worth paying for.
static const uint32_t FB_GRACE_MS = 1500;

// Paid requests. Home Assistant's flight board automation fetches AeroAPI for
// a request it does not throttle, and its own description puts one direction
// at about $0.01. Nothing here relies on the caller being careful - a knob, a
// flapping broker or another site's page posting to the portal: one airport
// and direction is asked for at most once per FB_ASK_FLOOR_MS, and all of them
// together at most FB_ASK_MAX_PER_HOUR times an hour. Both numbers are budget
// choices, not norms; the hourly cap bounds the worst case near $0.12 an hour.
static const uint32_t FB_ASK_FLOOR_MS     = 15UL * 60UL * 1000UL;
static const uint8_t  FB_ASK_MAX_PER_HOUR = 12;
static const uint8_t  FB_ASK_AIRPORTS     = 8;
static uint32_t s_askedAt[FB_ASK_AIRPORTS][2] = {};   // millis() of the last ask, 0 = never
static uint32_t s_hourStart = 0;
static uint8_t  s_hourAsks  = 0;

static bool mayAsk(uint8_t apt, bool dep, uint32_t now) {
  // Home Assistant serves the six built-in airports only: its automation
  // refuses any other code, so asking for one would only fill the log.
  if (apt >= FB_ASK_AIRPORTS || !flightboardAirportBuiltin(apt)) return false;
  if (now - s_hourStart >= 3600000UL) { s_hourStart = now; s_hourAsks = 0; }
  if (s_hourAsks >= FB_ASK_MAX_PER_HOUR) return false;
  const uint32_t at = s_askedAt[apt][dep];
  return !at || (now - at) >= FB_ASK_FLOOR_MS;
}

static void noteAsked(uint8_t apt, bool dep, uint32_t now) {
  s_askedAt[apt][dep] = now ? now : 1;
  s_hourAsks++;
}

static char     s_subbed[96] = "";      // topic currently subscribed, "" = none
static uint32_t s_dirtyAt    = 0;
static uint32_t s_graceUntil = 0;
static bool     s_wasUp      = false;

// Both directions on one subscription: the page swaps between them, and a
// wildcard keeps the flight board to a single slot of the bus's six.
static void topicFor(char *out, size_t n) {
  snprintf(out, n, "%s/%s/+", FB_TOPIC_STATE, flightboardAirport());
}

static void onMessage(const char *topic, const uint8_t *payload, uint16_t len) {
  (void)topic;              // the payload names its airport and direction
  flightboardIngest((const char *)payload, len);
}

static void resubscribe() {
  char want[96];
  topicFor(want, sizeof(want));
  if (!strcmp(want, s_subbed)) {
    if (!flightboardHasData()) {
      // Back on the same airport after the knob went away and returned: both
      // halves were dropped on the way, and a broker sends retained messages
      // only for a new subscription. Subscribe afresh instead of paying for
      // boards it already holds.
      mqttBusUnsubscribe(s_subbed);
      if (mqttBusSubscribe(want)) s_graceUntil = millis() + FB_GRACE_MS;
      else s_subbed[0] = '\0';
      return;
    }
    // Same airport, a different choice of direction: whatever is retained is
    // already here, so only look for what is still missing.
    s_graceUntil = millis();
    return;
  }
  if (s_subbed[0]) mqttBusUnsubscribe(s_subbed);
  if (mqttBusSubscribe(want)) {
    strncpy(s_subbed, want, sizeof(s_subbed) - 1);
    s_subbed[sizeof(s_subbed) - 1] = '\0';
    s_graceUntil = millis() + FB_GRACE_MS;
  } else {
    s_subbed[0] = '\0';
  }
}

void fbMqttBegin() {
  mqttBusOnMessage(FB_TOPIC_STATE, onMessage);
  if (!flightboardDirectOwns()) resubscribe();
}

void fbMqttLoop() {
  const uint32_t now = millis();
  // With an AeroAPI key the page is the direct fetch's: no subscription, and
  // above all no request that would have Home Assistant pay for the same board.
  if (flightboardDirectOwns()) {
    if (s_subbed[0]) { mqttBusUnsubscribe(s_subbed); s_subbed[0] = '\0'; }
    s_dirtyAt = s_graceUntil = 0;
    return;
  }
  if (s_dirtyAt && (now - s_dirtyAt) >= FB_SETTLE_MS) {
    s_dirtyAt = 0;
    resubscribe();
  }
  // The retained boards arrive only once the broker session is up. Counting the
  // grace from before that would ask - and pay - for boards already retained.
  const bool up = mqttBusConnected();
  if (up && !s_wasUp && s_subbed[0]) s_graceUntil = now + FB_GRACE_MS;
  s_wasUp = up;
  // Nothing retained turned up for a half this page shows, or what did is a
  // fetch from hours ago. Ask once for each; the answers arrive on the
  // subscription already in place.
  if (s_graceUntil && (int32_t)(now - s_graceUntil) >= 0 && up) {
    s_graceUntil = 0;
    for (uint8_t i = 0; i < 2; i++) {
      const bool dep = (i == 1);
      const uint8_t apt = flightboardAirportId();
      if (!flightboardWants(dep) || flightboardHasFreshBoard(dep) || !mayAsk(apt, dep, now)) continue;
      char body[64];
      snprintf(body, sizeof(body), "{\"apt\":\"%s\",\"dir\":\"%s\"}",
               flightboardAirport(), dep ? "dep" : "arr");
      if (mqttBusPublish(FB_TOPIC_REQ, body)) noteAsked(apt, dep, now);
    }
  }
}

void fbMqttSelectionChanged() { s_dirtyAt = millis(); }

bool fbMqttConnected() { return mqttBusConnected(); }

const char *fbMqttStatus() {
  if (flightboardDirectOwns()) return "AEROAPI";
  if (s_dirtyAt || s_graceUntil) return "FETCHING";
  return mqttBusStatus();
}

#endif  // FLIGHTBOARD_ENABLED && FB_MQTT_ENABLED

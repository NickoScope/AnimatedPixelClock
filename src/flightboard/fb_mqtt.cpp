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
  resubscribe();
}

void fbMqttLoop() {
  const uint32_t now = millis();
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
      if (!flightboardWants(dep) || flightboardHasFreshBoard(dep)) continue;
      char body[64];
      snprintf(body, sizeof(body), "{\"apt\":\"%s\",\"dir\":\"%s\"}",
               flightboardAirport(), dep ? "dep" : "arr");
      mqttBusPublish(FB_TOPIC_REQ, body);
    }
  }
}

void fbMqttSelectionChanged() { s_dirtyAt = millis(); }

bool fbMqttConnected() { return mqttBusConnected(); }

const char *fbMqttStatus() {
  if (s_dirtyAt || s_graceUntil) return "FETCHING";
  return mqttBusStatus();
}

#endif  // FLIGHTBOARD_ENABLED && FB_MQTT_ENABLED

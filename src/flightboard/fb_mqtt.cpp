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

static void topicFor(char *out, size_t n) {
  snprintf(out, n, "%s/%s/%s", FB_TOPIC_STATE,
           flightboardAirport(), flightboardDirection());
}

static void onMessage(const char *topic, const uint8_t *payload, uint16_t len) {
  (void)topic;                       // only one flight board subscription exists
  flightboardIngest((const char *)payload, len);
  s_graceUntil = 0;                  // data arrived; no request needed
}

static void resubscribe() {
  char want[96];
  topicFor(want, sizeof(want));
  if (!strcmp(want, s_subbed)) return;
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
  // Nothing retained turned up for this selection, so Home Assistant has never
  // fetched it. Ask once; the answer arrives on the topic already subscribed.
  if (s_graceUntil && (int32_t)(now - s_graceUntil) >= 0 && mqttBusConnected()) {
    s_graceUntil = 0;
    char body[64];
    snprintf(body, sizeof(body), "{\"apt\":\"%s\",\"dir\":\"%s\"}",
             flightboardAirport(), flightboardDirection());
    mqttBusPublish(FB_TOPIC_REQ, body);
  }
}

void fbMqttSelectionChanged() { s_dirtyAt = millis(); }

bool fbMqttConnected() { return mqttBusConnected(); }

const char *fbMqttStatus() {
  if (s_dirtyAt || s_graceUntil) return "FETCHING";
  return mqttBusStatus();
}

#endif  // FLIGHTBOARD_ENABLED && FB_MQTT_ENABLED

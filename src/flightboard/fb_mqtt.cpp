#include "fb_mqtt.h"

#if defined(FLIGHTBOARD_ENABLED) && defined(FB_MQTT_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <string.h>

#include "flightboard.h"

#ifndef FB_MQTT_HOST_DEFAULT
#define FB_MQTT_HOST_DEFAULT "homeassistant.local"
#endif
#ifndef FB_MQTT_PORT_DEFAULT
#define FB_MQTT_PORT_DEFAULT 1883
#endif

#define FB_TOPIC_REQ   "nickoscope_watch/flightboard/req"
#define FB_TOPIC_STATE "nickoscope_watch/flightboard/state"

// PubSubClient's buffer is 256 bytes by default and it does not truncate an
// oversized PUBLISH - it drops it, silently. A full board measured 1051 bytes
// on live Nice data, so the default would have produced a page that simply
// never updated, with nothing in the log to say why.
static const uint16_t FB_MQTT_BUFFER = 2048;

// The knob steps one airport per detent. Acting on each would resubscribe six
// times and, worse, could fire six AeroAPI fetches at roughly a cent each, so
// the selection has to stop moving first.
static const uint32_t FB_SETTLE_MS = 1200;

// A retained payload lands within milliseconds of subscribing. Only when none
// does is the board actually missing, and only then is a request worth paying
// for. Generous enough to cover a broker under load.
static const uint32_t FB_GRACE_MS = 1500;

static const uint32_t FB_RETRY_MS = 5000;

static WiFiClient   s_net;
static PubSubClient s_mq(s_net);
static Preferences  s_nvs;

static String   s_host, s_user, s_pass;
static uint16_t s_port = FB_MQTT_PORT_DEFAULT;
static char     s_clientId[24];
static char     s_subbed[96] = "";      // topic currently subscribed, "" = none
static uint32_t s_dirtyAt    = 0;       // selection moved at
static uint32_t s_graceUntil = 0;       // waiting for a retained payload until
static uint32_t s_lastTry    = 0;
static bool     s_noBroker   = false;

static void topicFor(char *out, size_t n) {
  snprintf(out, n, "%s/%s/%s", FB_TOPIC_STATE,
           flightboardAirport(), flightboardDirection());
}

static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  (void)topic;                       // only one subscription is ever active
  flightboardIngest((const char *)payload, (uint16_t)len);
  s_graceUntil = 0;                  // data arrived; no request needed
}

// Subscriptions do not survive a reconnect, so this runs from the connect path
// as well as on a selection change.
static void resubscribe() {
  char want[96];
  topicFor(want, sizeof(want));
  if (!strcmp(want, s_subbed)) return;
  if (s_subbed[0]) s_mq.unsubscribe(s_subbed);
  if (s_mq.subscribe(want)) {
    strncpy(s_subbed, want, sizeof(s_subbed) - 1);
    s_subbed[sizeof(s_subbed) - 1] = '\0';
    s_graceUntil = millis() + FB_GRACE_MS;
  } else {
    s_subbed[0] = '\0';
  }
}

static void publishRequest() {
  char body[64];
  snprintf(body, sizeof(body), "{\"apt\":\"%s\",\"dir\":\"%s\"}",
           flightboardAirport(), flightboardDirection());
  s_mq.publish(FB_TOPIC_REQ, body);
}

void fbMqttBegin() {
  if (s_nvs.begin("fb", true)) {
    s_host = s_nvs.getString("host", FB_MQTT_HOST_DEFAULT);
    s_user = s_nvs.getString("user", "");
    s_pass = s_nvs.getString("pass", "");
    s_port = (uint16_t)s_nvs.getUShort("port", FB_MQTT_PORT_DEFAULT);
    s_nvs.end();
  } else {
    s_host = FB_MQTT_HOST_DEFAULT;
  }
  s_noBroker = s_host.isEmpty();
  if (s_noBroker) return;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(s_clientId, sizeof(s_clientId), "apc-%02X%02X%02X", mac[3], mac[4], mac[5]);

  s_mq.setServer(s_host.c_str(), s_port);
  // setBufferSize can fail: it reallocs, and on failure the buffer stays at
  // 256 bytes and every board is dropped silently - exactly the failure this
  // call exists to prevent. Refuse to run rather than run blind.
  if (!s_mq.setBufferSize(FB_MQTT_BUFFER)) { s_noBroker = true; return; }
  s_mq.setKeepAlive(30);
  // PubSubClient::connect() busy-waits for CONNACK with no yield, bounded by
  // this timeout (default 15 s) on top of a 3 s TCP connect - past the 15 s
  // watchdog. A broker that accepts TCP but never answers is not exotic.
  s_mq.setSocketTimeout(5);
  s_mq.setCallback(onMessage);
}

void fbMqttLoop() {
  if (s_noBroker || WiFi.status() != WL_CONNECTED) return;

  if (!s_mq.connected()) {
    s_subbed[0] = '\0';
    const uint32_t now = millis();
    if (now - s_lastTry < FB_RETRY_MS) return;
    s_lastTry = now;
    esp_task_wdt_delete(NULL);          // connect() does not yield
    const bool ok = s_user.length()
        ? s_mq.connect(s_clientId, s_user.c_str(), s_pass.c_str())
        : s_mq.connect(s_clientId);
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
    if (!ok) return;
    resubscribe();
    return;
  }

  s_mq.loop();

  const uint32_t now = millis();
  if (s_dirtyAt && (now - s_dirtyAt) >= FB_SETTLE_MS) {
    s_dirtyAt = 0;
    resubscribe();
  }
  // Nothing retained turned up for this selection, so Home Assistant has never
  // fetched it. Ask once; the answer arrives on the topic already subscribed.
  if (s_graceUntil && (int32_t)(now - s_graceUntil) >= 0) {
    s_graceUntil = 0;
    publishRequest();
  }
}

void fbMqttSelectionChanged() { s_dirtyAt = millis(); }

bool fbMqttConnected() { return s_mq.connected(); }

const char *fbMqttStatus() {
  if (s_noBroker)                     return "NO BROKER";
  if (WiFi.status() != WL_CONNECTED)  return "NO WIFI";
  if (!s_mq.connected())              return "CONNECTING";
  if (s_dirtyAt || s_graceUntil)      return "FETCHING";
  return "NO DATA";
}

#endif  // FLIGHTBOARD_ENABLED && FB_MQTT_ENABLED

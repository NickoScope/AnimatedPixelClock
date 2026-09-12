#include "mqtt_bus.h"

#if defined(MQTT_BUS_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <string.h>

#ifndef MQTT_BUS_HOST_DEFAULT
#define MQTT_BUS_HOST_DEFAULT "homeassistant.local"
#endif
#ifndef MQTT_BUS_PORT_DEFAULT
#define MQTT_BUS_PORT_DEFAULT 1883
#endif

// PubSubClient's buffer is 256 bytes by default and it does not truncate an
// oversized PUBLISH - it drops it, silently. A full flight board measured 1060
// bytes on live data, so the default would produce a page that never updates
// with nothing in the log to say why.
static const uint16_t MQTT_BUS_BUFFER = 2048;
static const uint32_t MQTT_BUS_RETRY_MS = 5000;

#define MQTT_MAX_SUBS     6
#define MQTT_MAX_HANDLERS 4
#define MQTT_TOPIC_LEN    96

static WiFiClient   s_net;
static PubSubClient s_mq(s_net);

static char     s_subs[MQTT_MAX_SUBS][MQTT_TOPIC_LEN];
static uint8_t  s_subCount = 0;

static struct { const char *prefix; MqttBusHandler fn; } s_handlers[MQTT_MAX_HANDLERS];
static uint8_t  s_handlerCount = 0;

static String   s_host, s_user, s_pass;
static uint16_t s_port = MQTT_BUS_PORT_DEFAULT;
static char     s_clientId[24];
static uint32_t s_lastTry  = 0;
static bool     s_noBroker = false;

static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  for (uint8_t i = 0; i < s_handlerCount; i++) {
    const size_t n = strlen(s_handlers[i].prefix);
    if (!strncmp(topic, s_handlers[i].prefix, n)) {
      s_handlers[i].fn(topic, payload, (uint16_t)len);
      return;
    }
  }
}

bool mqttBusOnMessage(const char *prefix, MqttBusHandler fn) {
  if (s_handlerCount >= MQTT_MAX_HANDLERS) return false;
  s_handlers[s_handlerCount].prefix = prefix;
  s_handlers[s_handlerCount].fn     = fn;
  s_handlerCount++;
  return true;
}

bool mqttBusSubscribe(const char *topic) {
  for (uint8_t i = 0; i < s_subCount; i++)
    if (!strcmp(s_subs[i], topic)) return true;         // already ours
  if (s_subCount >= MQTT_MAX_SUBS) return false;
  strncpy(s_subs[s_subCount], topic, MQTT_TOPIC_LEN - 1);
  s_subs[s_subCount][MQTT_TOPIC_LEN - 1] = '\0';
  s_subCount++;
  if (s_mq.connected()) s_mq.subscribe(topic);
  return true;
}

void mqttBusUnsubscribe(const char *topic) {
  for (uint8_t i = 0; i < s_subCount; i++) {
    if (!strcmp(s_subs[i], topic)) {
      if (s_mq.connected()) s_mq.unsubscribe(topic);
      for (uint8_t j = i; j + 1 < s_subCount; j++)
        memcpy(s_subs[j], s_subs[j + 1], MQTT_TOPIC_LEN);
      s_subCount--;
      return;
    }
  }
}

bool mqttBusPublish(const char *topic, const char *payload, bool retain) {
  return s_mq.connected() && s_mq.publish(topic, payload, retain);
}

void mqttBusBegin() {
  Preferences p;
  if (p.begin("fb", true)) {
    s_host = p.getString("host", MQTT_BUS_HOST_DEFAULT);
    s_user = p.getString("user", "");
    s_pass = p.getString("pass", "");
    s_port = (uint16_t)p.getUShort("port", MQTT_BUS_PORT_DEFAULT);
    p.end();
  } else {
    s_host = MQTT_BUS_HOST_DEFAULT;
  }
  s_noBroker = s_host.isEmpty();
  if (s_noBroker) return;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(s_clientId, sizeof(s_clientId), "apc-%02X%02X%02X", mac[3], mac[4], mac[5]);

  s_mq.setServer(s_host.c_str(), s_port);
  // setBufferSize reallocs and can fail; on failure the buffer stays at 256
  // bytes and every large payload is dropped silently - exactly what this call
  // exists to prevent. Refuse to run rather than run blind.
  if (!s_mq.setBufferSize(MQTT_BUS_BUFFER)) { s_noBroker = true; return; }
  s_mq.setKeepAlive(30);
  // connect() busy-waits for CONNACK with no yield, bounded by this timeout on
  // top of a 3 s TCP connect. The default 15 s is past the task watchdog.
  s_mq.setSocketTimeout(5);
  s_mq.setCallback(onMessage);
}

void mqttBusLoop() {
  if (s_noBroker || WiFi.status() != WL_CONNECTED) return;

  if (!s_mq.connected()) {
    const uint32_t now = millis();
    if (now - s_lastTry < MQTT_BUS_RETRY_MS) return;
    s_lastTry = now;
    esp_task_wdt_delete(NULL);          // connect() does not yield
    const bool ok = s_user.length()
        ? s_mq.connect(s_clientId, s_user.c_str(), s_pass.c_str())
        : s_mq.connect(s_clientId);
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
    if (!ok) return;
    // Subscriptions do not survive a reconnect, so the whole remembered set is
    // re-applied here rather than by whoever asked for them.
    for (uint8_t i = 0; i < s_subCount; i++) s_mq.subscribe(s_subs[i]);
    return;
  }
  s_mq.loop();
}

bool mqttBusConnected() { return s_mq.connected(); }

const char *mqttBusStatus() {
  if (s_noBroker)                    return "NO BROKER";
  if (WiFi.status() != WL_CONNECTED) return "NO WIFI";
  if (!s_mq.connected())             return "CONNECTING";
  return "NO DATA";
}

#endif  // MQTT_BUS_ENABLED

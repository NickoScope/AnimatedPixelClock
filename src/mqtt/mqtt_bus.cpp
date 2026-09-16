#include "mqtt_bus.h"

#if defined(MQTT_BUS_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ESPmDNS.h>
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
// D3, 2026-09-16. A broker that stops answering used to hold loop() for 3 s out
// of every 5: WiFiClient's default connect timeout is 3000 ms (WiFiClient.cpp:26)
// and the retry was fixed at 5 s, so the panel froze for 60 % of the time. The
// gap now doubles to a minute, and the name is resolved once, with a bound:
// WiFiClient::connect() resolves inside itself with no timeout of its own, and a
// dead .local name took about 15 s a try in the log of 2026-09-15 23:11.
static const uint32_t MQTT_BUS_RETRY_MAX_MS = 60000;
static const uint32_t MQTT_BUS_MDNS_MS = 2000;   // bound on one mDNS lookup
static const uint8_t  MQTT_BUS_FAILS_BEFORE_RERESOLVE = 3;

// In use with every page built in: cards 3 handlers and 3 subscriptions, the
// flight board 1 and 1, the rail board 1 and 1, the media player 1 and 1 - six
// of each, which was the whole table, so it is eight: room for one more page
// without another round of this. Past these limits a register or subscribe
// call is refused, and a refused one is a page that never updates.
#define MQTT_MAX_SUBS     8
#define MQTT_MAX_HANDLERS 8
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
static uint32_t s_retryMs  = MQTT_BUS_RETRY_MS;   // grows to MQTT_BUS_RETRY_MAX_MS while the broker is away
static uint8_t  s_fails    = 0;                   // connect attempts in a row that failed
static IPAddress s_ip;
static bool     s_haveIp   = false;               // s_ip resolved and given to PubSubClient
static bool     s_noBroker = false;
static uint32_t s_connects = 0;   // successful connects since boot (mqttBusConnects)

// The host as configured may be an address, a .local name or a plain name.
// An address needs no lookup at all; a .local name is resolved through mDNS,
// which takes a timeout, unlike WiFiClient's own lookup. A plain name is left
// to WiFiClient: lwIP caches a successful DNS answer.
static void resolveHost() {
  if (s_haveIp) return;
  IPAddress ip;
  if (ip.fromString(s_host)) {
    s_ip = ip;
    s_haveIp = true;
    s_mq.setServer(s_ip, s_port);
    return;
  }
  const int dot = s_host.lastIndexOf(".local");
  if (dot > 0 && dot == (int)s_host.length() - 6) {
    const String name = s_host.substring(0, dot);
    ip = MDNS.queryHost(name.c_str(), MQTT_BUS_MDNS_MS);
    if (ip != IPAddress((uint32_t)0)) {
      s_ip = ip;
      s_haveIp = true;
      s_mq.setServer(s_ip, s_port);
      Serial.printf("[mqtt] %s is %s\n", s_host.c_str(), s_ip.toString().c_str());
    }
  }
}

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

bool mqttBusPublishLarge(const char *topic, const uint8_t *payload, size_t len, bool retain) {
  if (!s_mq.connected() || !topic || !payload || !len || len > 65535) return false;
  // A short write leaves part of a packet on the socket, and the broker would
  // take the next packet's bytes for the rest of this one. WiFiClient::write
  // returns what it sent after ten 1 s selects without progress (arduino-esp32
  // 2.0.17, WiFiClient.cpp:27-28 and 389-434), so a short count is real: the
  // socket is closed and mqttBusLoop() reconnects and re-subscribes. Closed,
  // not disconnect(): that writes a DISCONNECT packet, which would land inside
  // the half-sent PUBLISH. PubSubClient::connected() then sees the closed
  // client and reports the connection lost.
  if (!s_mq.beginPublish(topic, (unsigned int)len, retain)) {   // the header, written short
    s_net.stop();
    return false;
  }
  if (s_mq.write(payload, len) != len) {
    s_net.stop();
    return false;
  }
  s_mq.endPublish();   // PubSubClient 2.8: sends nothing, returns 1
  return true;
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
  // WiFiClient::setTimeout() takes seconds and sets the connect timeout used by
  // connect() (WiFiClient.cpp:325, :302). The broker is on the LAN: 1 s is
  // plenty, and it is what loop() pays for an attempt that fails.
  s_net.setTimeout(1);
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
    if (now - s_lastTry < s_retryMs) return;
    s_lastTry = now;
    resolveHost();
    esp_task_wdt_delete(NULL);          // connect() does not yield
    const bool ok = s_user.length()
        ? s_mq.connect(s_clientId, s_user.c_str(), s_pass.c_str())
        : s_mq.connect(s_clientId);
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
    if (!ok) {
      if (s_retryMs < MQTT_BUS_RETRY_MAX_MS) {
        s_retryMs *= 2;
        if (s_retryMs > MQTT_BUS_RETRY_MAX_MS) s_retryMs = MQTT_BUS_RETRY_MAX_MS;
      }
      // A cached address that no longer answers is worth resolving again: the
      // broker may have moved, or mDNS answered before the network settled.
      if (++s_fails >= MQTT_BUS_FAILS_BEFORE_RERESOLVE) {
        s_fails = 0;
        s_haveIp = false;
        s_mq.setServer(s_host.c_str(), s_port);
      }
      return;
    }
    s_retryMs = MQTT_BUS_RETRY_MS;
    s_fails = 0;
    s_connects++;
    // Subscriptions do not survive a reconnect, so the whole remembered set is
    // re-applied here rather than by whoever asked for them.
    for (uint8_t i = 0; i < s_subCount; i++) s_mq.subscribe(s_subs[i]);
    return;
  }
  s_mq.loop();
}

bool mqttBusConnected() { return s_mq.connected(); }

uint32_t mqttBusConnects() { return s_connects; }

bool mqttBusConfigured() { return !s_host.isEmpty(); }

const char *mqttBusStatus() {
  if (s_noBroker)                    return "NO BROKER";
  if (WiFi.status() != WL_CONNECTED) return "NO WIFI";
  if (!s_mq.connected())             return "CONNECTING";
  return "NO DATA";
}

#endif  // MQTT_BUS_ENABLED

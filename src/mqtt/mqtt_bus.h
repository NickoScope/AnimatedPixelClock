#pragma once
// One MQTT connection, shared by everything that needs one.
//
// The flight board owned the client when it was the only consumer. Generic
// cards need the same broker, and a second PubSubClient would mean a second
// socket and a second TLS-free-but-still-real chunk of heap for no reason.
//
// Subscriptions are remembered here and re-applied after a reconnect, which is
// the bug PubSubClient hands you otherwise: subscribe() does not survive a
// dropped connection, and nothing tells you.
//
// Broker credentials live in NVS, namespace "fb": host, port, user, pass.
// Nothing is compiled in.

#include <stdint.h>

#if defined(MQTT_BUS_ENABLED)

// Prefix every topic this device owns. Kept short: it is repeated on the wire.
#ifndef MQTT_BASE
#define MQTT_BASE "nickoscope_matrix"
#endif

typedef void (*MqttBusHandler)(const char *topic, const uint8_t *payload, uint16_t len);

void mqttBusBegin();
void mqttBusLoop();

// Route messages whose topic starts with `prefix` to `fn`. First match wins,
// so register the more specific prefix first.
bool mqttBusOnMessage(const char *prefix, MqttBusHandler fn);

// Both are remembered, so a reconnect restores exactly the current set.
bool mqttBusSubscribe(const char *topic);
void mqttBusUnsubscribe(const char *topic);

bool mqttBusPublish(const char *topic, const char *payload, bool retain = false);

bool        mqttBusConnected();
const char *mqttBusStatus();   // short label for a page that has nothing to show

#endif  // MQTT_BUS_ENABLED

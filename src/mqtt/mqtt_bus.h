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

#include <stddef.h>
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

// A payload larger than the bus buffer allows (2048 B less the header and the
// topic), streamed with PubSubClient's beginPublish / write / endPublish so it
// never sits in the buffer. For what this device publishes only: what it
// receives still has to fit the buffer, and a copy of this that comes back on
// a wildcard subscription of its own is read to the end and ignored by
// PubSubClient (readPacket: idx > bufferSize). The market page's config.
bool mqttBusPublishLarge(const char *topic, const uint8_t *payload, size_t len, bool retain = false);

bool        mqttBusConnected();
// Successful connects since boot. A consumer that republishes retained state
// after a reconnect compares this with the value it last saw: the edge of
// mqttBusConnected() misses a reconnect made inside one mqttBusLoop() call,
// when WiFiClient::connected() had not yet seen the broker's FIN.
uint32_t    mqttBusConnects();
const char *mqttBusStatus();   // short label for a page that has nothing to show
bool        mqttBusConfigured();  // a broker host is stored; never says which

#endif  // MQTT_BUS_ENABLED

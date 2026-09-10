#pragma once
// The flight board's transport.
//
// Home Assistant already serves this board over MQTT for another device in the
// house, so the panel is a second subscriber to a deployed contract, not a new
// feature:
//
//   request   nickoscope_watch/flightboard/req            {"apt":"LFMN","dir":"arr"}
//   response  nickoscope_watch/flightboard/state/<apt>/<dir>   retained
//
// Keyed on (airport, direction) rather than on the client, so two devices
// watching different airports neither overwrite each other's payload nor pay
// twice for the same AeroAPI fetch. See docs/09 in the knowledge base.
//
// Broker credentials live in NVS, namespace "fb": host, port, user, pass.
// Nothing is compiled in.

#include <stdint.h>

#if defined(FLIGHTBOARD_ENABLED) && defined(FB_MQTT_ENABLED)

void fbMqttBegin();
void fbMqttLoop();

// Call when the knob moves the selection. Cheap and idempotent: the resubscribe
// and any request wait for the knob to settle, so spinning through six airports
// costs one subscribe and at most one fetch.
void fbMqttSelectionChanged();

bool fbMqttConnected();
const char *fbMqttStatus();     // short label for the page when there is no data

#endif

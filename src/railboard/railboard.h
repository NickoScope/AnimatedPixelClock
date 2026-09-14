#pragma once
// Rail board: a UK station's departures and arrivals, fed by Home Assistant.
//
// Home Assistant polls Realtime Trains every 20 s, holds the API token, and
// publishes one compact retained payload per direction. The panel only
// subscribes and draws, the same split the flight board uses: no TLS and no
// token on a device whose internal SRAM bottoms out near 43 KB.
//
//   MQTT_BASE/railboard/<crs>/departures   retained, schema v1
//   MQTT_BASE/railboard/<crs>/arrivals     retained, schema v1
//   MQTT_BASE/railboard/<crs>/status       retained, Home Assistant's last attempt
//   MQTT_BASE/railboard/<crs>/config       retained, optional run-time settings
//
// Schema, layout and what was verified: src/railboard/README.md.
// The Home Assistant side: tools/railboard/ha_package_guildford.yaml.

#include <stdint.h>

#if defined(RAILBOARD_ENABLED)

#if !defined(MQTT_BUS_ENABLED)
#error "RAILBOARD_ENABLED needs MQTT_BUS_ENABLED: the data arrives over src/mqtt/mqtt_bus"
#endif
#if !defined(CONTROL_ENCODER_ENABLED)
#error "RAILBOARD_ENABLED needs CONTROL_ENCODER_ENABLED: the page is reached with the knob"
#endif

// Build-time defaults. Everything except the station can be changed at run time
// by the retained .../config payload, so Home Assistant owns the settings.
#ifndef RB_CRS
#define RB_CRS        "GLD"        // Guildford; also the topic segment
#endif
#ifndef RB_STATION
#define RB_STATION    "GUILDFORD"  // header, until a payload names the station
#endif
#ifndef RB_PANELS
#define RB_PANELS     2            // 2: both lists side by side; 1: alternate
#endif
#ifndef RB_ROWS
#define RB_ROWS       3            // services per list, capped by the type size
#endif
#ifndef RB_SWITCH_S
#define RB_SWITCH_S   10           // one panel: seconds per list
#endif
#ifndef RB_LEVEL
#define RB_LEVEL      100          // percent of full colour, for this page only
#endif
#ifndef RB_STALE_S
#define RB_STALE_S    80           // see README: four polls, not a standard
#endif

void railboardBegin();             // registers the MQTT handler and subscribes
void railboardRender();            // one frame; the caller has cleared the screen
void railboardPress();             // knob press: board <-> diagnostics
void railboardTurn(int8_t delta);  // knob turn: on one panel, the other list now

// The same path the MQTT handler takes, for a test or a serial command.
// Returns false and leaves the current board untouched on anything malformed.
bool railboardIngest(const char *topic, const char *payload, uint16_t len);

#endif  // RAILBOARD_ENABLED

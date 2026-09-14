#pragma once
// Rail board: one UK station's departures or arrivals, fed by Home Assistant.
//
// The page shows one list on the whole panel and alternates between the two
// every 10 s, laid out like a UK station screen: a white title and column
// headings, amber rows, pages, and a large clock at the foot. Any station: its
// three-letter code is chosen in the web portal and kept in NVS by src/panel.
//
// Home Assistant polls Realtime Trains for the selected station, holds the API
// token, and publishes compact retained payloads. The panel only subscribes and
// draws - no TLS and no token on a device whose internal SRAM bottoms out near
// 43 KB - and tells Home Assistant which station it wants:
//
//   MQTT_BASE/railboard/select              {"crs":"GLD"}  retained, published BY the panel
//   MQTT_BASE/railboard/<crs>/departures    retained, schema v1
//   MQTT_BASE/railboard/<crs>/arrivals      retained, schema v1
//   MQTT_BASE/railboard/<crs>/status        retained, Home Assistant's last attempt
//   MQTT_BASE/railboard/<crs>/config        retained, optional run-time settings
//
// With RAILBOARD_DIRECT_ENABLED the panel also asks Realtime Trains itself
// (rtt_direct.h); a fresh board of its own wins over Home Assistant's.
//
// Schema, layout, knob and what was verified: src/railboard/README.md.
// The Home Assistant side: tools/railboard/ha_package_railboard.yaml.

#include <stdint.h>
#include <ArduinoJson.h>

// Outside the RAILBOARD_ENABLED block, so it fires when that flag is missing.
#if defined(RAILBOARD_DIRECT_ENABLED) && !defined(RAILBOARD_ENABLED)
#error "RAILBOARD_DIRECT_ENABLED needs RAILBOARD_ENABLED: it fills the rail board's lists"
#endif
#if defined(RAILBOARD_DIRECT_ENABLED) && !defined(BOARD_HAS_PSRAM)
#error "RAILBOARD_DIRECT_ENABLED needs BOARD_HAS_PSRAM: the TLS session, the body and the JSON live there"
#endif

#if defined(RAILBOARD_ENABLED)

#if !defined(MQTT_BUS_ENABLED)
#error "RAILBOARD_ENABLED needs MQTT_BUS_ENABLED: the data arrives over src/mqtt/mqtt_bus"
#endif
#if !defined(CONTROL_ENCODER_ENABLED)
#error "RAILBOARD_ENABLED needs CONTROL_ENCODER_ENABLED: the page is reached with the knob"
#endif

// Build-time defaults. The station is a run-time setting (NVS "panel"/"rbStn");
// the rest can be changed by the retained .../config payload or the portal.
#ifndef RB_CRS
#define RB_CRS        "GLD"   // Guildford, until the portal chooses another
#endif
#ifndef RB_ROWS
#define RB_ROWS       8       // services listed, over one or two pages
#endif
#ifndef RB_SWITCH_S
#define RB_SWITCH_S   10      // seconds per list before the other one
#endif
#ifndef RB_LEVEL
#define RB_LEVEL      100     // percent of full colour, for this page only
#endif
#ifndef RB_STALE_S
#define RB_STALE_S    80      // see README: four polls, not a standard
#endif
#ifndef RB_HOLD_S
#define RB_HOLD_S     60      // a list chosen with the knob stays this long
#endif

void railboardBegin();             // registers the MQTT handler and subscribes
void railboardLoop();              // publishes the station selection when it is due
void railboardRender();            // one frame; the caller has cleared the screen

// Inside the page the knob steps departures -> arrivals -> diagnostics and
// round again, and the choice holds the alternation for RB_HOLD_S.
void railboardKnob(int8_t delta);

// The same path the MQTT handler takes, for a test or a serial command.
// Returns false and leaves the current board untouched on anything malformed.
bool railboardIngest(const char *topic, const char *payload, uint16_t len);

// The station: exactly three capitals A-Z. Setting it drops the boards of the
// old one, resubscribes, and publishes the selection retained. It does not
// write NVS - src/panel does, through panelSetRailStation().
bool        railboardValidStation(const char *crs);
bool        railboardSetStation(const char *crs);
const char *railboardStation();

// For the web portal. The status carries no token or broker detail - the panel
// never holds either. A config applied here is the .../config payload, same
// schema, same clamps; it lasts until Home Assistant's retained config arrives
// again (every reconnect) or the panel reboots, because Home Assistant owns it.
void railboardStatusJson(JsonObject out);
bool railboardApplyConfig(const char *json, uint16_t len);
void railboardSetDiag(bool on);    // pin the diagnostics view, or release it

#endif  // RAILBOARD_ENABLED

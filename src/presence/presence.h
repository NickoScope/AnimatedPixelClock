#pragma once
// The presence radar: up to three people, as an Apollo MTR-1 (LD2450) on the
// wall sees them, arriving over the shared MQTT bus and drawn by the
// room_radar Lua scene. docs/16-presence-radar.md in the knowledge base has
// the sensor, the contract and the traps.
//
// Two tasks reach this module. The loop task on core 1 writes: presenceLoop(),
// the MQTT callback the bus makes from inside mqttBusLoop(), the web handlers.
// The Lua effect task on core 0 reads, through the bindings, during an
// effect's draw(). A spinlock in presence.cpp covers every entry point; the
// model itself holds no lock, so the host tests still build it on the Mac.
//
// Cost: one Model and one parse arena, both static, both in this module. No
// allocation on any path, and none at all per frame - the internal heap is the
// scarce resource on this board (docs/22 §12.3).

#include <ArduinoJson.h>
#include <stdint.h>

#if defined(PRESENCE_ENABLED)

#if !defined(MQTT_BUS_ENABLED)
#error "PRESENCE_ENABLED needs MQTT_BUS_ENABLED: the targets arrive on the shared MQTT bus (src/mqtt/mqtt_bus.h)"
#endif
#if !defined(LUA_EFFECTS_ENABLED)
#error "PRESENCE_ENABLED needs LUA_EFFECTS_ENABLED: the radar is the room_radar Lua effect (src/lua/README.md)"
#endif

void presenceBegin();               // in setup(), after loadSettings() and mqttBusBegin()
void presenceLoop();                // every loop() pass: the ring, at 10 Hz. No I/O.
void presenceSettingsChanged();     // after the portal or an import changed one of ours
void presenceInfoJson(JsonObject out);   // /api/info's "presence"

// ── what the Lua scene reads ────────────────────────────────────────────────
// All of these are O(1), allocate nothing and are safe to call from draw(),
// which asks for them 19 times a frame.
uint8_t presenceSourceCode();       // 0 the scripted story, 1 live, 2 the feed stopped
uint8_t presenceScaleM();           // metres the fan covers: 2, 4 or 6
bool    presenceTarget(uint8_t slot, int32_t *x, int32_t *y, int16_t *speedCms);
bool    presenceTrail(uint8_t slot, uint8_t stepsBack, int32_t *x, int32_t *y);
uint8_t presenceCountBack(uint8_t stepsBack);

struct lua_State;
void presenceLuaOpen(lua_State *L);   // binds the global `presence` table

#endif  // PRESENCE_ENABLED

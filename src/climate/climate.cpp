// The board's SHTC3, read from loop().
//
// The reading cycle is climate_reader.h: a state machine that makes at most one
// I2C transaction per loop() pass, looks at SDA and SCL before each cycle, and
// after a transaction that stalled sends nothing for a minute (a held bus costs
// the I2C driver a second per transaction whatever Wire's timeout says; the
// source lines are in src/board/board_i2c.h). This file gives it Wire on the
// board's shared bus and turns its readings into what the panel reports: the
// settings' corrections, /api/info, the weather screen's snapshot, Home
// Assistant.
//
// Everything here runs on the loop task: climateLoop(), the web handlers that
// call climateGet() and climateInfoJson(), the weather screen and the MQTT bus.
// No lock of its own; what Wire's mutex covers is in board_i2c.h.

#include "climate.h"

#if defined(CLIMATE_ENABLED)

#include <Wire.h>

#include <climits>
#include <cmath>

#include "../board/board_i2c.h"
#include "../config/config.h"
#include "climate_model.h"
#include "climate_reader.h"
#include "shtc3.h"

#if defined(MQTT_BUS_ENABLED)
#include <WiFi.h>

#include "../mqtt/mqtt_bus.h"
#endif

namespace {

// Wire on the board's bus, for the reader, which times every call.
class WirePort : public climate::Port {
 public:
  int write(uint16_t cmd) override {
    Wire.beginTransmission(shtc3::kAddress);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)(cmd & 0xFF));
    return Wire.endTransmission(true);
  }
  size_t read(uint8_t *buf, size_t n) override {
    const size_t got = Wire.requestFrom((uint16_t)shtc3::kAddress, n, true);
    for (size_t i = 0; i < got && i < n; i++) buf[i] = (uint8_t)Wire.read();
    return got;
  }
  bool linesHigh() override { return boardI2cLinesHigh(); }
  uint32_t ms() override { return millis(); }
  uint32_t us() override { return micros(); }
};

WirePort s_port;
climate::Reader s_reader(s_port);
bool s_wire = false;   // the board's bus is running (board_i2c.h)

uint32_t intervalMs() { return (uint32_t)climate::clampInterval(settings.climateIntervalS) * 1000UL; }

double hundredths(float v) { return std::round((double)v * 100.0) / 100.0; }

#if defined(MQTT_BUS_ENABLED)
// ── Home Assistant ──────────────────────────────────────────────────────────
// Two sensors by MQTT discovery (home-assistant.io/integrations/mqtt/ and
// /integrations/sensor.mqtt/): <prefix>/<component>/<node_id>/<object_id>/config,
// prefix "homeassistant", node_id and object_id of [a-zA-Z0-9_-]. An empty
// retained payload removes an entity.
//
// Retained configs, sent again on every connect. Home Assistant's docs prefer
// a device that listens for HA's birth message instead, but that is one more
// subscription and one more handler on a bus whose tables (8 each,
// mqtt_bus.cpp) already hold seven of each with every page built in.
// expire_after marks the values unavailable when the panel stops publishing.
const uint32_t kHaHeartbeatMs = 60000;   // a state goes out at least this often while readings arrive
const uint32_t kHaRetryMs     = 5000;

char     s_node[40]       = "";   // MQTT_BASE "_" and the MAC's last three bytes: node_id and the device's identifier
char     s_stateTopic[72] = "";
uint32_t s_haConnects = 0;        // mqttBusConnects() when the configs (or their removal) last went out
bool     s_haDirty = true;
bool     s_haOn    = false;       // the broker holds the configs, not their removal
uint32_t s_haTryMs = 0, s_haPubMs = 0, s_haPublishes = 0;
int32_t  s_haT = INT32_MIN, s_haH = INT32_MIN;
bool     s_haEver = false;       // the reader's ever() when haLoop() last looked

bool haTopics() {
  if (s_node[0]) return true;
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  if (!(mac[3] | mac[4] | mac[5])) return false;
  snprintf(s_node, sizeof(s_node), MQTT_BASE "_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(s_stateTopic, sizeof(s_stateTopic), MQTT_BASE "/%02x%02x%02x/climate/state", mac[3], mac[4], mac[5]);
  return true;
}

uint32_t haExpireS() {
  const uint32_t period = intervalMs() > kHaHeartbeatMs ? intervalMs() : kHaHeartbeatMs;
  return 3UL * period / 1000UL;
}

bool haConfig(const char *object, const char *name, const char *deviceClass, const char *unit,
              const char *key, uint8_t precision, bool on) {
  char topic[112];
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", s_node, object);
  if (!on) return mqttBusPublish(topic, "", true);

  char uid[64], tpl[32];
  snprintf(uid, sizeof(uid), "%s_%s", s_node, object);
  snprintf(tpl, sizeof(tpl), "{{ value_json.%s }}", key);
  JsonDocument d;
  d["name"] = name;
  d["unique_id"] = (const char *)uid;
  d["state_topic"] = (const char *)s_stateTopic;
  d["value_template"] = (const char *)tpl;
  d["device_class"] = deviceClass;
  d["unit_of_measurement"] = unit;
  d["state_class"] = "measurement";
  d["suggested_display_precision"] = precision;
  d["expire_after"] = haExpireS();
  JsonObject dev = d["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add((const char *)s_node);
  dev["name"] = (const char *)settings.deviceName;
  dev["model"] = "ESP32-S3-RGB-Matrix";
  dev["manufacturer"] = "Waveshare";
  dev["sw_version"] = FIRMWARE_VERSION;

  char buf[768];
  const size_t n = serializeJson(d, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf) - 1) return false;
  return mqttBusPublish(topic, buf, true);
}

void haLoop() {
  if (s_reader.ever() != s_haEver) {   // found (the entities can go out now), or forgotten
    s_haEver = s_reader.ever();
    s_haDirty = true;
  }
  if (!mqttBusConnected() || !haTopics()) return;
  const bool want = settings.climateEnabled && settings.climateHa;
  const uint32_t now = millis();
  const uint32_t connects = mqttBusConnects();

  if (connects != s_haConnects || s_haDirty) {
    if (want && !s_reader.ever()) return;                          // no entities for a sensor not found yet
    if (s_haTryMs && now - s_haTryMs < kHaRetryMs) return;
    s_haTryMs = now | 1;
    bool ok = haConfig("indoor_temperature", "Indoor temperature", "temperature", "\xC2\xB0" "C", "t", 1, want);
    ok = haConfig("indoor_humidity", "Indoor humidity", "humidity", "%", "rh", 0, want) && ok;
    if (!ok) return;
    s_haConnects = connects;
    s_haDirty = false;
    s_haOn = want;
    s_haT = s_haH = INT32_MIN;                            // the state goes out with the next reading
  }

  if (!s_haOn || !s_reader.takeFresh()) return;
  const ClimateReading r = climateGet();
  if (r.state != ClimateState::Ok) return;
  const int32_t t10 = (int32_t)lroundf(r.tempC * 10.0f);
  const int32_t h = (int32_t)lroundf(r.humidity);
  if (t10 == s_haT && h == s_haH && now - s_haPubMs < kHaHeartbeatMs) return;
  char payload[48];
  snprintf(payload, sizeof(payload), "{\"t\":%.1f,\"rh\":%.1f}", r.tempC, r.humidity);
  if (mqttBusPublish(s_stateTopic, payload, false)) {
    s_haT = t10;
    s_haH = h;
    s_haPubMs = now;
    s_haPublishes++;
  }
}
#endif  // MQTT_BUS_ENABLED

// The corrected reading, recomputed only when the smoothed values or the
// settings behind it change: the weather screen asks for it every frame, and
// the humidity compensation is two exp() calls.
struct Corrected {
  bool valid = false;
  float st = 0.0f, srh = 0.0f;
  int16_t tOff = 0, hOff = 0;
  bool follow = false;
  climate::Reading r{};
};
Corrected s_corr;

const climate::Reading &corrected() {
  const float st = s_reader.temperature(), srh = s_reader.humidity();
  if (!s_corr.valid || s_corr.st != st || s_corr.srh != srh ||
      s_corr.tOff != settings.climateTempOffset || s_corr.hOff != settings.climateHumOffset ||
      s_corr.follow != settings.climateRhFollowsT) {
    s_corr.st = st;
    s_corr.srh = srh;
    s_corr.tOff = settings.climateTempOffset;
    s_corr.hOff = settings.climateHumOffset;
    s_corr.follow = settings.climateRhFollowsT;
    s_corr.r = climate::correct(st, srh, s_corr.tOff, s_corr.hOff, s_corr.follow);
    s_corr.valid = true;
  }
  return s_corr.r;
}

}  // namespace

void climateBegin() {
  boardI2cBegin();   // main.cpp has begun the bus before any module; this costs nothing then
  s_wire = boardI2cReady();
  if (!s_wire) Serial.println("[climate] the board's I2C bus is not running: no sensor readings");
}

void climateLoop() {
#if defined(MQTT_BUS_ENABLED)
  haLoop();   // before the switch below: switching off also removes the entities
#endif
  if (!settings.climateEnabled) {
    if (s_reader.running()) s_reader.stop();
    return;
  }
  if (!s_wire) return;
  s_reader.loop(settings.climateIntervalS);
}

ClimateReading climateGet() {
  ClimateReading r{};
  const uint32_t now = millis();
  if (!settings.climateEnabled) {
    r.state = ClimateState::Off;
  } else if (!s_wire) {
    r.state = ClimateState::Absent;
  } else {
    r.state = s_reader.state(now, settings.climateIntervalS);
  }
  if (s_reader.have() && settings.climateEnabled) {
    r.have = true;
    r.sensorTempC = s_reader.temperature();
    r.sensorHumidity = s_reader.humidity();
    const climate::Reading &c = corrected();   // cached: no exp() on a frame that changed nothing
    r.tempC = c.tempC;
    r.humidity = c.humidity;
    r.ageMs = now - s_reader.lastOkMs();
  }
  return r;
}

const char *climateStateName(ClimateState s) {
  switch (s) {
    case ClimateState::Off:     return "off";
    case ClimateState::Probing: return "probing";
    case ClimateState::Ok:      return "ok";
    case ClimateState::Stale:   return "stale";
    case ClimateState::Absent:  return "absent";
  }
  return "?";
}

void climateSettingsChanged() {
  s_reader.settingsChanged(settings.climateIntervalS);
#if defined(MQTT_BUS_ENABLED)
  s_haDirty = true;   // the switch may have moved, and expire_after follows the interval
  s_haTryMs = 0;
#endif
}

void climatePause(uint32_t seconds) { s_reader.pause(seconds); }

void climateInfoJson(JsonObject out) {
  const ClimateReading r = climateGet();
  const climate::Counters &c = s_reader.counters();
  out["state"] = climateStateName(r.state);
  out["intervalS"] = climate::clampInterval(settings.climateIntervalS);
  if (r.have) {
    out["tempC"] = hundredths(r.tempC);
    out["humidity"] = hundredths(r.humidity);
    out["sensorTempC"] = hundredths(r.sensorTempC);
    out["sensorHumidity"] = hundredths(r.sensorHumidity);
    out["ageS"] = r.ageMs / 1000UL;
  }
  out["reads"] = c.reads;
  out["crcErrors"] = c.crcErrors;
  out["i2cErrors"] = c.i2cErrors;
  out["softResets"] = c.softResets;
  out["busStuck"] = c.busStuck;     // cycles skipped: SDA or SCL read low
  out["busStalls"] = c.busStalls;   // transactions that ran into the driver's one-second ceiling
  if (s_reader.foreign()) out["foreignDevice"] = true;
  if (s_reader.idRead()) {
    char id[8];
    snprintf(id, sizeof(id), "0x%04X", s_reader.id());
    out["id"] = id;   // an SHTC3 has (id & 0x083F) == 0x0807: datasheet Table 15
  }
  if (s_reader.paused()) {
    const int32_t left = (int32_t)(s_reader.pauseUntilMs() - millis());
    out["pausedS"] = left > 0 ? (uint32_t)left / 1000UL : 0UL;
  }
#if defined(MQTT_BUS_ENABLED)
  out["ha"] = settings.climateHa;
  out["haPublishes"] = s_haPublishes;
#endif
}

#endif  // CLIMATE_ENABLED

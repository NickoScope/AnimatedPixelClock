// The board's SHTC3, read from loop().
//
// One reading is four I2C transactions (SHTC3 datasheet, section 5.4): wake
// up, start a measurement, read six bytes, sleep. climateLoop() makes at most
// one of them per pass and returns; the waits between them (at most 240 us
// after the wake-up, 12.1 ms for a normal-mode measurement, Table 5) pass
// while loop() does its other work. The measurement asks for clock stretching
// off, so a read that comes too early is refused with a NACK instead of the
// sensor holding SCL, and the reader tries again a few milliseconds later.
//
// Everything here runs on the loop task: climateLoop(), the web handlers that
// call climateGet() and climateInfoJson(), and the MQTT bus. No lock.

#include "climate.h"

#if defined(CLIMATE_ENABLED)

#include <Wire.h>

#include <climits>
#include <cmath>

#include "../board/board_i2c.h"
#include "../config/config.h"
#include "climate_model.h"
#include "shtc3.h"

#if defined(MQTT_BUS_ENABLED)
#include <WiFi.h>

#include "../mqtt/mqtt_bus.h"
#endif

namespace {

// ── the cycle ───────────────────────────────────────────────────────────────
const uint32_t kWakeWaitUs     = 1000;    // after a wake-up or soft reset: tPU and tSR are 240 us at most (Table 5)
const uint32_t kMeasureWaitUs  = 15000;   // normal mode: tMEAS is 12.1 ms at most (Table 5)
const uint32_t kReadRetryUs    = 5000;
const uint8_t  kReadTries      = 4;       // 15 ms + 3 x 5 ms, then the measurement counts as lost
const uint8_t  kFailsToReset   = 3;       // every third failed cycle in a row starts with a soft reset (Table 12)
const uint8_t  kProbesToAbsent = 3;       // a sensor not found in three tries is absent
const uint32_t kReprobeMs      = 60000;   // and is looked for again once a minute
const uint32_t kRetryMs        = 2000;    // the first failed cycles are retried this soon

enum class Step : uint8_t { Off, Due, Awake, Measuring };

Step     s_step    = Step::Off;
bool     s_wire    = false;   // the board's bus is running (board_i2c.h)
bool     s_found   = false;   // the ID register matched since the last soft reset
bool     s_ever    = false;   // found at least once since it was switched on
bool     s_foreign = false;   // something at 0x70 answered with another ID
bool     s_reset   = false;   // the next cycle starts with a soft reset
bool     s_have    = false;   // a good reading exists
bool     s_fresh   = false;   // a reading the MQTT side has not published yet
bool     s_idRead  = false;   // the ID register was read with a good CRC
uint16_t s_id      = 0;       // its value, whatever answered at 0x70 (/api/info)
bool     s_paused  = false;   // climatePause(): no cycle starts before s_pauseUntilMs
uint32_t s_pauseUntilMs = 0;
uint32_t s_stepUs = 0, s_waitUs = 0;
uint32_t s_dueMs = 0, s_lastOkMs = 0, s_foundMs = 0;   // s_foundMs: when the ID first matched
uint8_t  s_tries = 0, s_fails = 0, s_probes = 0;
uint32_t s_reads = 0, s_crcErrors = 0, s_i2cErrors = 0, s_resets = 0;
climate::Smoother s_smooth;

uint32_t intervalMs() { return (uint32_t)climate::clampInterval(settings.climateIntervalS) * 1000UL; }

bool send(uint16_t cmd) {
  Wire.beginTransmission(shtc3::kAddress);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  // arduino-esp32 2.0.17 Wire.cpp: 0 sent, 2 no ACK (ESP_FAIL), 5 timeout, 4 anything else.
  return Wire.endTransmission(true) == 0;
}

bool receive(uint8_t *buf, size_t n) {
  if (Wire.requestFrom((uint16_t)shtc3::kAddress, n, true) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

void waitUs(uint32_t us) {
  s_stepUs = micros();
  s_waitUs = us;
}

bool waited() { return (uint32_t)(micros() - s_stepUs) >= s_waitUs; }

// A cycle that ended without a reading. `awake`: the sensor was woken and is
// sent back to sleep, so a failure does not leave it idling at 45 uA (Table 3).
void failed(bool noAnswer, bool awake) {
  if (awake) send(shtc3::kSleep);
  // A look for a sensor that was never found is not an error: counting it would
  // grow i2cErrors by one a minute on a board without the part.
  if (noAnswer && s_ever) s_i2cErrors++;
  const uint32_t now = millis();
  s_step = Step::Due;
  if (!s_ever) {
    if (s_probes < 255) s_probes++;
    s_dueMs = now + (s_probes >= kProbesToAbsent ? kReprobeMs : kRetryMs);
    return;
  }
  if (s_fails < 255) s_fails++;
  if (s_fails % kFailsToReset == 0) {
    s_reset = true;
    s_found = false;   // and read the ID again after it
  }
  s_dueMs = now + (s_fails < kFailsToReset ? kRetryMs : intervalMs());
}

void accept(uint16_t st, uint16_t srh) {
  const uint32_t now = millis();
  const float t  = shtc3::centiDegrees(st) / 100.0f;
  const float rh = shtc3::centiPercent(srh) / 100.0f;
  // After a stale spell the old average must not drag the new value.
  if (s_have && climate::isStale(now, s_lastOkMs, settings.climateIntervalS)) s_smooth.reset();
  s_smooth.add(t, rh, s_have ? (now - s_lastOkMs) / 1000.0f : 0.0f);
  s_have = true;
  s_fresh = true;
  s_lastOkMs = now;
  s_reads++;
  s_fails = 0;
  s_step = Step::Due;
  s_dueMs = now + intervalMs();
}

void switchOff() {
  if (s_step == Step::Awake || s_step == Step::Measuring) send(shtc3::kSleep);
  s_step = Step::Off;
  s_found = s_ever = s_foreign = s_reset = s_have = s_fresh = false;
  s_fails = s_probes = 0;
  s_smooth.reset();
}

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
  if (!mqttBusConnected() || !haTopics()) return;
  const bool want = settings.climateEnabled && settings.climateHa;
  const uint32_t now = millis();
  const uint32_t connects = mqttBusConnects();

  if (connects != s_haConnects || s_haDirty) {
    if (want && !s_ever) return;                          // no entities for a sensor not found yet
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

  if (!s_haOn || !s_fresh) return;
  s_fresh = false;
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
  if (!s_corr.valid || s_corr.st != s_smooth.t || s_corr.srh != s_smooth.rh ||
      s_corr.tOff != settings.climateTempOffset || s_corr.hOff != settings.climateHumOffset ||
      s_corr.follow != settings.climateRhFollowsT) {
    s_corr.st = s_smooth.t;
    s_corr.srh = s_smooth.rh;
    s_corr.tOff = settings.climateTempOffset;
    s_corr.hOff = settings.climateHumOffset;
    s_corr.follow = settings.climateRhFollowsT;
    s_corr.r = climate::correct(s_smooth.t, s_smooth.rh, s_corr.tOff, s_corr.hOff, s_corr.follow);
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
    if (s_step != Step::Off) switchOff();
    return;
  }
  if (!s_wire) return;

  switch (s_step) {
    case Step::Off:
      s_step = Step::Due;
      s_dueMs = millis();
      return;

    case Step::Due:
      if (s_paused) {
        if ((int32_t)(millis() - s_pauseUntilMs) < 0) return;
        s_paused = false;
      }
      if ((int32_t)(millis() - s_dueMs) < 0) return;
      if (!send(shtc3::kWakeup)) {
        failed(true, false);
        return;
      }
      s_step = Step::Awake;
      waitUs(kWakeWaitUs);
      return;

    case Step::Awake: {
      if (!waited()) return;
      if (s_reset) {                                   // section 5.7: from idle, not during a measurement
        s_reset = false;
        s_resets++;
        if (!send(shtc3::kSoftReset)) {
          failed(true, true);
          return;
        }
        waitUs(kWakeWaitUs);
        return;
      }
      if (!s_found) {                                  // section 5.9: presence and proper communication
        uint8_t id[3];
        if (!send(shtc3::kReadId) || !receive(id, sizeof(id))) {
          failed(true, true);
          return;
        }
        if (!shtc3::wordValid(id)) {
          s_crcErrors++;
          failed(false, true);
          return;
        }
        s_id = shtc3::wordValue(id);
        s_idRead = true;
        if (!shtc3::isShtc3Id(s_id)) {
          // Something else answers at 0x70: send it nothing more than a look once a minute.
          s_foreign = true;
          s_step = Step::Due;
          s_probes = kProbesToAbsent;
          s_dueMs = millis() + kReprobeMs;
          return;
        }
        s_found = true;
        s_foreign = false;
        s_probes = 0;
        if (!s_ever) {
          s_ever = true;
          s_foundMs = millis();
#if defined(MQTT_BUS_ENABLED)
          s_haDirty = true;                            // the entities can go out now
#endif
        }
      }
      if (!send(shtc3::kMeasureNormal)) {
        failed(true, true);
        return;
      }
      s_step = Step::Measuring;
      s_tries = 0;
      waitUs(kMeasureWaitUs);
      return;
    }

    case Step::Measuring: {
      if (!waited()) return;
      uint8_t b[6];
      if (!receive(b, sizeof(b))) {                    // section 5.5: refused while it still measures
        if (++s_tries < kReadTries) {
          waitUs(kReadRetryUs);
          return;
        }
        failed(true, true);
        return;
      }
      send(shtc3::kSleep);                             // section 5.4, the fourth command
      uint16_t st = 0, srh = 0;
      if (shtc3::decodeTemperatureFirst(b, &st, &srh) != shtc3::Frame::Ok) {
        s_crcErrors++;
        failed(false, false);
        return;
      }
      accept(st, srh);
      return;
    }
  }
}

ClimateReading climateGet() {
  ClimateReading r{};
  const uint32_t now = millis();
  const uint16_t ivl = settings.climateIntervalS;
  if (!settings.climateEnabled) {
    r.state = ClimateState::Off;
  } else if (!s_wire) {
    r.state = ClimateState::Absent;
  } else if (s_have) {
    r.state = climate::isStale(now, s_lastOkMs, ivl) ? ClimateState::Stale : ClimateState::Ok;
  } else if (s_ever) {
    // Found, but no good reading since: stale once that has lasted as long as a reading may.
    r.state = climate::isStale(now, s_foundMs, ivl) ? ClimateState::Stale : ClimateState::Probing;
  } else {
    r.state = (s_foreign || s_probes >= kProbesToAbsent) ? ClimateState::Absent : ClimateState::Probing;
  }
  if (s_have && settings.climateEnabled) {
    r.have = true;
    r.sensorTempC = s_smooth.t;
    r.sensorHumidity = s_smooth.rh;
    const climate::Reading &c = corrected();   // cached: no exp() on a frame that changed nothing
    r.tempC = c.tempC;
    r.humidity = c.humidity;
    r.ageMs = now - s_lastOkMs;
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
  // A shorter interval takes effect now rather than after the long wait already set.
  if (s_step == Step::Due && s_ever) {
    const uint32_t next = millis() + intervalMs();
    if ((int32_t)(s_dueMs - next) > 0) s_dueMs = next;
  }
#if defined(MQTT_BUS_ENABLED)
  s_haDirty = true;   // the switch may have moved, and expire_after follows the interval
  s_haTryMs = 0;
#endif
}

void climatePause(uint32_t seconds) {
  s_paused = seconds > 0;
  s_pauseUntilMs = millis() + seconds * 1000UL;
}

void climateInfoJson(JsonObject out) {
  const ClimateReading r = climateGet();
  out["state"] = climateStateName(r.state);
  out["intervalS"] = climate::clampInterval(settings.climateIntervalS);
  if (r.have) {
    out["tempC"] = hundredths(r.tempC);
    out["humidity"] = hundredths(r.humidity);
    out["sensorTempC"] = hundredths(r.sensorTempC);
    out["sensorHumidity"] = hundredths(r.sensorHumidity);
    out["ageS"] = r.ageMs / 1000UL;
  }
  out["reads"] = s_reads;
  out["crcErrors"] = s_crcErrors;
  out["i2cErrors"] = s_i2cErrors;
  out["softResets"] = s_resets;
  if (s_foreign) out["foreignDevice"] = true;
  if (s_idRead) {
    char id[8];
    snprintf(id, sizeof(id), "0x%04X", s_id);
    out["id"] = id;   // an SHTC3 has (id & 0x083F) == 0x0807: datasheet Table 15
  }
  if (s_paused) {
    const int32_t left = (int32_t)(s_pauseUntilMs - millis());
    out["pausedS"] = left > 0 ? (uint32_t)left / 1000UL : 0UL;
  }
#if defined(MQTT_BUS_ENABLED)
  out["ha"] = settings.climateHa;
  out["haPublishes"] = s_haPublishes;
#endif
}

#endif  // CLIMATE_ENABLED

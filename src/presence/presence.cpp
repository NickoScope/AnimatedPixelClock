// The presence radar's MQTT side: subscribe, parse, and hand the model what
// arrived. Everything the model then decides is presence_model.h, and the
// payload it decides on is presence_parse.h; both are host-tested.
//
// Two topics, and they are not treated alike:
//
//   <base>/presence/targets   NOT retained, ~1 Hz while somebody is present
//                             and every 5 s when the room is empty. This is
//                             the only thing that draws people. Because it is
//                             not retained, anything that arrives is live.
//   <base>/presence           RETAINED, the same fields plus "online". Read
//                             for /api/info only. A retained message can be
//                             minutes old - the broker hands it over on
//                             connect whatever its age - and stamping it with
//                             millis() on arrival would draw people who left
//                             long ago. It never moves a dot.

#include "presence.h"

#if defined(PRESENCE_ENABLED)

#include <Arduino.h>
#include <string.h>

#include "../config/config.h"
#include "../mqtt/mqtt_bus.h"
#include "presence_model.h"
#include "presence_parse.h"

namespace {

const char kTopicSummary[] = MQTT_BASE "/presence";            // also the prefix of both
const char kTopicTargets[] = MQTT_BASE "/presence/targets";

presence::Model    s_model;

// The Lua effect task runs on core 0 (src/lua/lua_effects.cpp) and reads the
// model through the bindings while the loop task on core 1 writes it from the
// MQTT callback and from presenceLoop(). The audit of 2026-09-16 found the
// claim of "no second task" false. Every entry point below holds this spinlock
// for the few integer operations it needs; the model itself stays free of
// FreeRTOS so the host tests keep building it on the Mac.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
presence::JsonPool s_pool;     // the parse's memory, capped, from PSRAM: presence_parse.h
presence::Summary  s_sum;

uint32_t s_messages = 0, s_summaries = 0, s_parseFails = 0;

// The feed is heard only while a page reads it (owner, 2026-09-23: the panel
// works on what is on its screen and nothing else). The room radar reads the
// model every frame through the Lua bindings below, so the last read says
// whether it is on screen. The Lua task notes the read under s_mux; the loop
// task subscribes and unsubscribes, because the MQTT bus is the loop task's.
// kIdleMs is our choice, not a measured norm: a script draws at 15 fps or more,
// so three seconds without a read means the page has gone, with room for a
// slow frame and a carousel transition.
const uint32_t kIdleMs = 3000;
uint32_t s_lastReadMs = 0;      // under s_mux
bool     s_subscribed = false;  // loop task only: this visit's subscriptions were asked for
bool     s_subOk = false;       // ... and the bus took both
uint32_t s_visits = 0;

// Once, at the first connection to the broker: is there a publisher at all?
// The summary is retained, so the broker hands it over on subscribe; a panel
// without the sensor has none, and then the radar keeps its scripted story
// rather than an empty room (presence_model.h, source()). Ten seconds is the
// wait for that one retained message, generous for a broker on the LAN.
const uint32_t kProbeMs = 10000;
enum class Probe : uint8_t { Waiting, Listening, Done } s_probe = Probe::Waiting;
uint32_t s_probeMs = 0;
uint32_t s_probeConnects = 0;   // mqttBusConnects() when it looked; a new connection looks again until a summary came

// Under s_mux, from every binding that reads the model. With the demo chosen
// in the settings nothing is subscribed: the story needs no feed.
inline void noteRead(uint32_t now) {
  s_lastReadMs = now;
  if (!s_model.listening() && s_model.wantsLive()) s_model.listenFrom(now);
}

void onMqtt(const char *topic, const uint8_t *payload, uint16_t len) {
  const bool targets = strcmp(topic, kTopicTargets) == 0;
  const bool summary = strcmp(topic, kTopicSummary) == 0;
  if (!targets && !summary) return;          // the prefix caught something else
  if (!len) return;                          // a cleared retained message

  presence::Report r[presence::kSlots];
  presence::Summary sum;
  if (!presence::parse(s_pool, (const char *)payload, len, r, &sum, targets)) {
    s_parseFails++;
    return;
  }
  // "online" rides on the retained summary only, so taking the new summary
  // wholesale would let every targets message erase it a second after it
  // arrived, and /api/info would blink between knowing and not knowing.
  const bool hadOnline = s_sum.haveOnline, wasOnline = s_sum.online;
  s_sum = sum;
  if (!s_sum.haveOnline && hadOnline) {
    s_sum.haveOnline = true;
    s_sum.online = wasOnline;
  }
  if (summary) {
    s_summaries++;
    portENTER_CRITICAL(&s_mux);
    s_model.onSummary();                     // a publisher exists
    portEXIT_CRITICAL(&s_mux);
    return;                                  // never draws anybody: see the head of this file
  }
  s_messages++;
  portENTER_CRITICAL(&s_mux);
  s_model.onMessage(millis(), r);
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace

void presenceBegin() {
  portENTER_CRITICAL(&s_mux);
  s_model.reset();
  portEXIT_CRITICAL(&s_mux);
  presenceSettingsChanged();
  // One handler for both: the summary topic is a prefix of the targets topic,
  // and onMqtt tells them apart by the whole name. The bus remembers the
  // subscriptions and re-applies them after a reconnect (src/mqtt/mqtt_bus.h),
  // so there is nothing to do on the way back.
  mqttBusOnMessage(kTopicSummary, onMqtt);
  // The subscriptions themselves are taken in presenceLoop(), while the radar
  // is on screen.
}

void presenceLoop() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  const bool reading = s_model.listening() && presence::since(now, s_lastReadMs) <= (int32_t)kIdleMs;
  if (reading) s_model.tick(now);   // the trail's ring, at 10 Hz. No I/O, no allocation.
  else if (s_model.listening()) s_model.stopListening();
  portEXIT_CRITICAL(&s_mux);

  if (reading && !s_subscribed) {
    // A refused subscription is a radar that never draws anybody, or a summary
    // that never arrives, with nothing in the log to say why (audit 2026-09-16).
    // The bus re-applies what it holds after a reconnect (src/mqtt/mqtt_bus.h).
    // A refusal is tried again on the next visit, not on every pass.
    const bool t = mqttBusSubscribe(kTopicTargets);
    const bool s = mqttBusSubscribe(kTopicSummary);
    if (!t) Serial.printf("[presence] the MQTT bus refused %s: the table is full\n", kTopicTargets);
    if (!s) Serial.printf("[presence] the MQTT bus refused %s: the table is full\n", kTopicSummary);
    s_subscribed = true;
    s_subOk = t && s;
    s_visits++;
  } else if (!reading && s_subscribed) {
    mqttBusUnsubscribe(kTopicTargets);
    if (s_probe != Probe::Listening) mqttBusUnsubscribe(kTopicSummary);
    s_subscribed = s_subOk = false;
  }

  // The look for a publisher (see kProbeMs): once, and again after a
  // reconnection for as long as no summary has ever arrived.
  if (s_probe == Probe::Done && !s_summaries && mqttBusConnects() != s_probeConnects)
    s_probe = Probe::Waiting;
  if (s_probe == Probe::Waiting && mqttBusConnected()) {
    s_probe = Probe::Listening;
    s_probeMs = now;
    s_probeConnects = mqttBusConnects();
    if (!mqttBusSubscribe(kTopicSummary)) s_probe = Probe::Done;
  } else if (s_probe == Probe::Listening && (s_summaries || now - s_probeMs > kProbeMs)) {
    s_probe = Probe::Done;
    if (!s_subscribed) mqttBusUnsubscribe(kTopicSummary);
  }
}

void presenceSettingsChanged() {
  portENTER_CRITICAL(&s_mux);
  s_model.setMirror(settings.presenceMirrorX);
  s_model.setWanted(presence::clampSource(settings.presenceSource));
  portEXIT_CRITICAL(&s_mux);
}

uint8_t presenceSourceCode() {
  portENTER_CRITICAL(&s_mux);
  noteRead(millis());
  const uint8_t src = (uint8_t)s_model.source(millis());
  portEXIT_CRITICAL(&s_mux);
  return src;
}

uint8_t presenceScaleM() { return presence::clampScaleM(settings.presenceScaleM); }

bool presenceTarget(uint8_t slot, int32_t *x, int32_t *y, int16_t *speedCms) {
  portENTER_CRITICAL(&s_mux);          // the Lua task calls this 19 times a frame
  const uint32_t now = millis();
  noteRead(now);
  const bool ok = s_model.target(now, slot, x, y, speedCms);
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

bool presenceTrail(uint8_t slot, uint8_t stepsBack, int32_t *x, int32_t *y) {
  portENTER_CRITICAL(&s_mux);
  noteRead(millis());
  const bool ok = s_model.trail(slot, stepsBack, x, y);
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

uint8_t presenceCountBack(uint8_t stepsBack) {
  portENTER_CRITICAL(&s_mux);
  noteRead(millis());
  const uint8_t n = s_model.count(stepsBack);
  portEXIT_CRITICAL(&s_mux);
  return n;
}

void presenceInfoJson(JsonObject out) {
  const uint32_t now = millis();
  // Read the model once under the lock, then build the document outside it:
  // ArduinoJson allocates, and a spinlock holds off interrupts on this core.
  portENTER_CRITICAL(&s_mux);
  const presence::Source src = s_model.source(now);
  const uint8_t targets = s_model.countNow(now);
  const bool ever = s_model.ever();
  const uint32_t lastMs = s_model.lastMessageMs();
  const bool listening = s_model.listening();
  portEXIT_CRITICAL(&s_mux);

  // "idle": the radar is not on screen, so the feed is not being heard at all.
  out["source"] = src == presence::Source::Demo && !s_model.wantsLive() ? "demo"
                : !listening ? "idle"
                : src == presence::Source::Demo ? "demo"
                : src == presence::Source::Live ? "live" : "lost";
  out["subscribed"] = s_subOk;
  out["visits"] = s_visits;
  out["scaleM"] = presenceScaleM();
  out["mirrorX"] = settings.presenceMirrorX;
  out["targets"] = targets;
  if (ever) {
    const int32_t age = presence::since(now, lastMs);
    out["lastMessageS"] = age > 0 ? (uint32_t)age / 1000UL : 0UL;
  }
  out["messages"] = s_messages;
  out["summaries"] = s_summaries;
  out["parseFailures"] = s_parseFails;
  // The parse pool's high-water mark against its cap: if these ever meet,
  // payloads are being refused, and the number says so before anyone has to
  // guess. The memory is PSRAM, and none of it is held between messages.
  out["jsonPeak"] = (uint32_t)s_pool.peak();
  out["jsonBytes"] = (uint32_t)s_pool.cap();
  if (s_sum.have) {
    out["people"] = s_sum.people;
    out["moving"] = s_sum.moving;
    out["still"] = s_sum.still;
    if (s_sum.haveLux) out["lux"] = s_sum.lux;
    if (s_sum.haveOnline) out["online"] = s_sum.online;
  }
}

#endif  // PRESENCE_ENABLED

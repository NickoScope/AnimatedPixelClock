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
presence::JsonPool s_pool;     // the parse's memory, capped, from PSRAM: presence_parse.h
presence::Summary  s_sum;

uint32_t s_messages = 0, s_summaries = 0, s_parseFails = 0;

void onMqtt(const char *topic, const uint8_t *payload, uint16_t len) {
  const bool targets = strcmp(topic, kTopicTargets) == 0;
  const bool summary = strcmp(topic, kTopicSummary) == 0;
  if (!targets && !summary) return;          // the prefix caught something else
  if (!len) return;                          // a cleared retained message

  presence::Report r[presence::kSlots];
  presence::Summary sum;
  if (!presence::parse(s_pool, (const char *)payload, len, r, &sum)) {
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
    return;                                  // never draws anybody: see the head of this file
  }
  s_messages++;
  s_model.onMessage(millis(), r);
}

}  // namespace

void presenceBegin() {
  s_model.reset();
  presenceSettingsChanged();
  // One handler for both: the summary topic is a prefix of the targets topic,
  // and onMqtt tells them apart by the whole name. The bus remembers the
  // subscriptions and re-applies them after a reconnect (src/mqtt/mqtt_bus.h),
  // so there is nothing to do on the way back.
  mqttBusOnMessage(kTopicSummary, onMqtt);
  mqttBusSubscribe(kTopicTargets);
  mqttBusSubscribe(kTopicSummary);
}

void presenceLoop() {
  s_model.tick(millis());   // the trail's ring, at 10 Hz. No I/O, no allocation.
}

void presenceSettingsChanged() {
  s_model.setMirror(settings.presenceMirrorX);
  s_model.setWanted(presence::clampSource(settings.presenceSource));
}

uint8_t presenceSourceCode() { return (uint8_t)s_model.source(millis()); }

uint8_t presenceScaleM() { return presence::clampScaleM(settings.presenceScaleM); }

bool presenceTarget(uint8_t slot, int32_t *x, int32_t *y, int16_t *speedCms) {
  return s_model.target(millis(), slot, x, y, speedCms);
}

bool presenceTrail(uint8_t slot, uint8_t stepsBack, int32_t *x, int32_t *y) {
  return s_model.trail(slot, stepsBack, x, y);
}

uint8_t presenceCountBack(uint8_t stepsBack) { return s_model.count(stepsBack); }

void presenceInfoJson(JsonObject out) {
  const uint32_t now = millis();
  const presence::Source src = s_model.source(now);
  out["source"] = src == presence::Source::Demo ? "demo"
                : src == presence::Source::Live ? "live" : "lost";
  out["scaleM"] = presenceScaleM();
  out["mirrorX"] = settings.presenceMirrorX;
  out["targets"] = s_model.countNow(now);
  if (s_model.ever()) {
    const int32_t age = presence::since(now, s_model.lastMessageMs());
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

#pragma once
// What the panel believes about the room between MQTT messages: which slots
// hold someone, where to draw them, and when to stop pretending. Plain C++ -
// no Arduino, no ArduinoJson - so tools/presence/check_presence.py compiles and
// runs it on a Mac. presence.cpp owns the subscription and the parse; every
// decision after the parse is here.
//
// The rules and their numbers are the ones tools/luasim/presence_panel.py
// renders, and the previews the owner approved ARE those renders. Changing one
// without the other makes the previews a lie.
//
// Memory: every byte below is in the object, which presence.cpp makes exactly
// one of, statically. Nothing here allocates, at any point, on any path - the
// internal heap is the scarce resource on this board (docs/22 §12.3 in the
// knowledge base: the audio visualizer exhausted it and took the network down).

#include <stdint.h>

namespace presence {

static const uint8_t kSlots = 3;         // the LD2450 tracks three, and says so

// A slot whose newest sample is older than this is not in the room. This is
// for messages that STOP; a message that names a slot null empties it at once.
static const uint32_t kFreshMs = 5000;
// No message at all for this long: the screen says so instead of drawing an
// empty room it has no evidence for.
static const uint32_t kLostMs = 30000;
// Targets are drawn this far behind the newest sample, so a drawn position
// always falls between two samples that really arrived. At the feed's 1 Hz a
// dot drawn at the newest sample teleports once a second; extrapolating past
// it would invent motion, and a target that stopped reporting would keep
// gliding. Clamped at both ends: when the feed slows, the dot stops where it
// was last really seen.
static const uint32_t kLagMs = 1000;

// The ring: the short history per slot the scene's trail reads. The LD2450's
// own rate is 10 Hz and the scene asks for 14 steps of 0.1 s, plus count() at
// 3, 6, 9 and 12 steps back - so 16 covers both with room to spare.
static const uint32_t kStepMs = 100;
static const uint8_t  kRing   = 16;

// What the scene is drawing. Demo keeps its scripted story, so the panel works
// with no Home Assistant behind it.
enum class Source : uint8_t { Demo = 0, Live = 1, Lost = 2 };

// ── the settings' bounds ────────────────────────────────────────────────────
// Scale: the fan is 60 px deep whatever it covers. 6 m is the sensor's range,
// 4 m is the owner's choice of 2026-09-16, and 2 m is refused as a setting
// because in_fan() would drop a person standing 2.1 m away and the panel would
// show an empty room with somebody in it (docs/16). It is offered anyway - the
// owner asked for 2/4/6 - but 4 is the default and what the previews show.
//
// 2 m costs memory as well, which is the other half of docs/16's warning and is
// measured, not assumed: driving the scene through fxhost with nothing but a
// scale() binding, the Lua heap peaks at 238,905 B at 2 m against 162,376 at
// 4 m and 162,224 at 6 m. The fan crosses 4,096 pixels there and Lua doubles
// the array that holds it. That heap is PSRAM, so it is affordable - but it is
// 76 KB bought for a worse picture.
static const uint8_t kScaleDefaultM = 4;
inline uint8_t clampScaleM(long m) { return (m == 2 || m == 4 || m == 6) ? (uint8_t)m : kScaleDefaultM; }

// Which way +X points in this room is not known until somebody walks in, so
// the mirror is a switch and it is off until that is settled.
static const bool kMirrorDefault = false;

// 0 the scripted story, 1 the radar. Live by default: a build with the flag on
// and a broker configured is asking for the real thing.
static const uint8_t kSourceDemo = 0, kSourceLive = 1;
inline uint8_t clampSource(long v) { return v == kSourceDemo ? kSourceDemo : kSourceLive; }

// The feed reports a signed speed in mm/s - negative is approaching. The scene
// wants cm/s and tests `t.speed > 12`. Passed through raw, every target reads
// as moving and an approaching one fails the test and gets the "sitting still"
// ring: the two mistakes cancel into nonsense. docs/16, "Three things the real
// data exposed".
inline int16_t speedCms(int32_t mmps) {
  // Widened before the magnitude is taken: -INT32_MIN does not fit in an
  // int32_t and negating it is undefined, which is a poor way to meet a sensor
  // glitch. Clamped at the top so a wild value reads as "moving", never as a
  // wrapped negative that would read as "sitting still".
  int64_t a = mmps < 0 ? -(int64_t)mmps : (int64_t)mmps;
  a /= 10;
  return (int16_t)(a > 32767 ? 32767 : a);
}

// One slot of one message, as the sensor reported it: raw, unmirrored.
struct Report {
  bool    present;
  int32_t x, y;    // mm, x signed sideways, y forward
  int32_t v;       // mm/s, signed
};

// millis() wraps every 49.7 days, so every comparison goes through this.
inline int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

class Model {
 public:
  void reset() {
    for (uint8_t i = 0; i < kSlots; i++) s_[i] = Slot();
    for (uint8_t k = 0; k < kRing; k++) {
      rcount_[k] = 0;
      for (uint8_t i = 0; i < kSlots; i++) r_[i][k].ok = false;
    }
    head_ = 0;
    lastMs_ = 0;
    stepMs_ = 0;
    ever_ = false;
    messages_ = 0;
  }

  void setMirror(bool on) { mirror_ = on; }
  void setWanted(uint8_t source) { wantLive_ = source != kSourceDemo; }

  // A message arrived and parsed. The newest message decides who is in the
  // room: a slot it reports null is empty now, not in five seconds. Absence is
  // something the publisher states; the contract sends a message every 5 s
  // while the room is empty, so inferring absence from silence would race that
  // heartbeat exactly.
  void onMessage(uint32_t nowMs, const Report r[kSlots]) {
    lastMs_ = nowMs;
    ever_ = true;
    messages_++;
    for (uint8_t i = 0; i < kSlots; i++) {
      Slot &s = s_[i];
      if (!r[i].present) {
        s.present = false;    // the history stays, and is dropped on the way back
        continue;
      }
      const int32_t x = mirror_ ? -r[i].x : r[i].x;
      if (s.present && s.have) {          // still here: the previous sample is a real predecessor
        s.t0 = s.t1; s.x0 = s.x1; s.y0 = s.y1;
        s.have0 = true;
      } else {
        s.have0 = false;                  // first after an absence: nobody walked that line
      }
      s.t1 = nowMs; s.x1 = x; s.y1 = r[i].y; s.v1 = speedCms(r[i].v);
      s.have = true;
      s.present = true;
    }
  }

  // Every kStepMs from loop(): one ring slot, so the trail is the radar's own
  // 10 Hz whatever rate the scene is drawn at. Costs three interpolations.
  void tick(uint32_t nowMs) {
    if (stepMs_ && since(nowMs, stepMs_) < (int32_t)kStepMs) return;
    stepMs_ = stepMs_ ? stepMs_ + kStepMs : nowMs;
    if (since(nowMs, stepMs_) > (int32_t)(4 * kStepMs)) stepMs_ = nowMs;   // after a long stall, resync
    head_ = (uint8_t)((head_ + 1) % kRing);
    uint8_t n = 0;
    for (uint8_t i = 0; i < kSlots; i++) {
      Cell &c = r_[i][head_];
      c.ok = target(nowMs, i, &c.x, &c.y, nullptr);
      if (c.ok) n++;
    }
    rcount_[head_] = n;
  }

  Source source(uint32_t nowMs) const {
    if (!wantLive_ || !ever_) return Source::Demo;
    return since(nowMs, lastMs_) > (int32_t)kLostMs ? Source::Lost : Source::Live;
  }

  // Where slot i should be drawn now, or false for an empty slot. v may be null.
  bool target(uint32_t nowMs, uint8_t slot, int32_t *x, int32_t *y, int16_t *v) const {
    if (slot >= kSlots) return false;
    const Slot &s = s_[slot];
    if (!s.present || !s.have) return false;
    if (since(nowMs, s.t1) > (int32_t)kFreshMs) return false;   // the feed stopped
    const uint32_t at = nowMs - kLagMs;
    int32_t px = s.x1, py = s.y1;
    if (s.have0) {
      const int32_t span = since(s.t1, s.t0);
      const int32_t into = since(at, s.t0);
      if (into <= 0 || span <= 0) {
        px = s.x0; py = s.y0;                       // not yet past the older sample
      } else if (into < span) {
        // |dx| <= 2 * 4860 mm and into < span <= kFreshMs, so this stays well
        // inside int32; a longer gap cannot reach here, because a slot that
        // was absent clears have0.
        px = s.x0 + (s.x1 - s.x0) * into / span;
        py = s.y0 + (s.y1 - s.y0) * into / span;
      }                                             // else: clamped at the newest sample
    }
    if (x) *x = px;
    if (y) *y = py;
    if (v) *v = s.v1;
    return true;
  }

  // Where slot i was k ring steps ago (k = 1 is 0.1 s), out of the ring.
  bool trail(uint8_t slot, uint8_t k, int32_t *x, int32_t *y) const {
    if (slot >= kSlots || k == 0 || k >= kRing) return false;
    const Cell &c = r_[slot][(uint8_t)((head_ + kRing - k) % kRing)];
    if (!c.ok) return false;
    if (x) *x = c.x;
    if (y) *y = c.y;
    return true;
  }

  // How many targets the ring held k steps ago; k = 0 is the newest step.
  uint8_t count(uint8_t k) const {
    if (k >= kRing) return 0;
    return rcount_[(uint8_t)((head_ + kRing - k) % kRing)];
  }

  uint8_t countNow(uint32_t nowMs) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kSlots; i++) if (target(nowMs, i, nullptr, nullptr, nullptr)) n++;
    return n;
  }

  bool     ever() const { return ever_; }
  uint32_t lastMessageMs() const { return lastMs_; }
  uint32_t messages() const { return messages_; }

 private:
  struct Slot {
    bool     have = false;       // a sample has ever arrived for this slot
    bool     have0 = false;      // ... and the one before it is a real predecessor
    bool     present = false;    // the newest message filled this slot
    uint32_t t0 = 0, t1 = 0;
    int32_t  x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int16_t  v1 = 0;
  };
  struct Cell { bool ok = false; int32_t x = 0, y = 0; };

  Slot     s_[kSlots];
  Cell     r_[kSlots][kRing];
  uint8_t  rcount_[kRing] = {0};
  uint8_t  head_ = 0;
  uint32_t lastMs_ = 0, stepMs_ = 0, messages_ = 0;
  bool     ever_ = false, mirror_ = kMirrorDefault, wantLive_ = true;
};

}  // namespace presence

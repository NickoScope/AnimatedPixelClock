#pragma once
// The remote's rules, with no hardware in them: the slots a button can be
// learned into, the learned map itself, and the state machine that turns
// decoded frames into what the knob produces - detents and a button level.
//
// Plain C++: no Arduino, no receiver library, no allocation. Compiled by
// tools/ir/check_ir.py on the host and checked against the timings below, the
// way climate_model.h and presence_model.h are. src/ir/ir.cpp owns everything
// this file deliberately does not know: the GPIO, the decoder library, NVS and
// the portal.
//
// ── where the numbers come from ─────────────────────────────────────────────
// Vishay application note 80071 rev 2.3 (26-Jun-2024), "Data Formats for IR
// Remote Control", THE NEC CODE, read at
// https://www.vishay.com/docs/80071/dataform.pdf:
//   - the carrier is 38 kHz;
//   - a frame opens with a 9 ms leader burst and a 4.5 ms pause;
//   - "After transmitting the data word, only the leader code and a single bit
//     are transmitted repeatedly as long as a key is pressed", and the special
//     version repeats "in each 108 ms time slot as long as the key is pressed".
// So a held key produces a frame every 108 ms, and that one number sets both
// timings here. Other protocols repeat on their own schedules; 108 ms is the
// one this module is dimensioned for, because it is the format of the owner's
// remote (NickoScope32 ADD-79 learned NEC codes from it).

#include <stdint.h>

namespace ir {

// ── the slots ───────────────────────────────────────────────────────────────
// A slot is a thing the panel can do, not a button: the remote is learned into
// these, so any remote works and a replacement is re-learned rather than
// reflashed. The first three are what the knob has, and they are the whole of
// phase 1 - they reach the encoder's own state machine through the seam in
// src/control/control.cpp, so single press, long press, browse and enter keep
// one implementation. The rest are accepted, counted and reported; what they
// do is a later phase, and adding one is a line in each table below.
enum Slot : uint8_t {
  kCcw = 0,       // turn left  = one detent anticlockwise
  kCw,            // turn right = one detent clockwise
  kOk,            // the knob's button: click, or hold for a long press
  kBack,          // reserved
  kBrightDown,    // reserved
  kBrightUp,      // reserved
  kPower,         // reserved
  kAux,           // reserved
  kSlotCount
};

// Machine names (the NVS key is built from the index, so these are free to
// change) and the line the portal shows next to each.
inline const char *slotName(uint8_t s) {
  static const char *const kNames[kSlotCount] = {
      "CCW", "CW", "OK", "BACK", "BRIGHT_DOWN", "BRIGHT_UP", "POWER", "AUX"};
  return s < kSlotCount ? kNames[s] : "?";
}
inline const char *slotHint(uint8_t s) {
  static const char *const kHints[kSlotCount] = {
      "Turn left - one detent anticlockwise",
      "Turn right - one detent clockwise",
      "The knob's button: a click, or hold it for a long press",
      "Back (reserved)",
      "Dimmer (reserved)",
      "Brighter (reserved)",
      "Power (reserved)",
      "Spare (reserved)",
  };
  return s < kSlotCount ? kHints[s] : "?";
}
inline bool slotDrivesKnob(uint8_t s) { return s == kCcw || s == kCw || s == kOk; }

// A slot by its machine name, case-insensitive ("ok", "OK", "Bright_Up"). The
// console and the portal's handlers both go through this, so a name means the
// same thing typed on serial and posted from a browser.
inline bool slotByName(const char *w, uint8_t *out) {
  if (!w || !*w || !out) return false;
  for (uint8_t i = 0; i < kSlotCount; i++) {
    const char *a = w, *b = slotName(i);
    for (;; a++, b++) {
      const char la = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
      const char lb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
      if (la != lb) break;
      if (!la) { *out = i; return true; }
    }
  }
  return false;
}

// ── the timings ─────────────────────────────────────────────────────────────
// A held key sends a frame every 108 ms (the note, above). Everything else is
// derived from that one measured period:
//   kHoldMs      the button is released once frames stop. 250 ms outlives one
//                dropped repeat (2 x 108 = 216 < 250) and not two, so a real
//                release is seen inside a third of a second.
//   kRepeatFreshMs  a NEC repeat frame carries no code of its own, so it
//                extends the last slot - but only while that slot is still the
//                one being held: 200 ms is one period plus most of a second.
//   kRotAccMax   the seam drains at 1 kHz, so the queue only fills if the
//                sampling task is starved; three detents is what a fast turn
//                of the real knob delivers in that window, and dropping the
//                rest beats spinning the display after the remote stopped.
static const uint32_t kNecRepeatMs   = 108;    // the note's time slot: the source of the two below
static const uint32_t kHoldMs        = 250;
static const uint32_t kRepeatFreshMs = 200;
static const int8_t   kRotAccMax     = 3;
static const uint32_t kLearnWindowMs = 15000;  // long enough to pick the remote up and aim it

// A protocol the decoder could not name. The library's own "unknown" value is
// not visible here; ir.cpp passes the flag.
struct Frame {
  uint8_t  proto;    // the library's decode_type_t, as a byte
  uint64_t value;
  bool     repeat;   // a repeat frame: no code of its own
  bool     unknown;  // the decoder could not name the protocol - noise, or somebody else's remote
};

// What a frame did. ir.cpp writes NVS on kLearned and logs the rest; nothing
// else leaves this file.
struct Outcome {
  enum Kind : uint8_t {
    kNothing = 0,   // noise, an unlearned button, a stale repeat
    kRotate,        // a detent was queued
    kButton,        // the button's level was extended
    kReserved,      // a learned slot with no action yet
    kLearned,       // the open learn window bound this code
  } kind;
  int8_t slot;       // the slot involved, -1 when none
  int8_t movedFrom;  // learning took this code away from that slot, -1 if it did not
};

class Map {
 public:
  void reset() {
    for (uint8_t i = 0; i < kSlotCount; i++) { m_value[i] = 0; m_proto[i] = 0; m_bound[i] = false; }
  }
  bool     bound(uint8_t s) const { return s < kSlotCount && m_bound[s]; }
  uint64_t value(uint8_t s) const { return s < kSlotCount ? m_value[s] : 0; }
  uint8_t  proto(uint8_t s) const { return s < kSlotCount ? m_proto[s] : 0; }
  uint8_t  boundCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kSlotCount; i++) n += m_bound[i] ? 1 : 0;
    return n;
  }
  void bind(uint8_t s, uint8_t proto, uint64_t value) {
    if (s >= kSlotCount) return;
    m_value[s] = value; m_proto[s] = proto; m_bound[s] = true;
  }
  bool clear(uint8_t s) {
    if (s >= kSlotCount) return false;
    m_value[s] = 0; m_proto[s] = 0; m_bound[s] = false;
    return true;
  }
  // The slot a code is bound to, -1 for none. A code lives in one slot only:
  // learning it again moves it (Decoder::frame reports movedFrom), so a remote
  // whose buttons were mixed up cannot end up firing two things at once.
  int match(uint8_t proto, uint64_t value) const {
    for (uint8_t i = 0; i < kSlotCount; i++)
      if (m_bound[i] && m_value[i] == value && m_proto[i] == proto) return (int)i;
    return -1;
  }

 private:
  uint64_t m_value[kSlotCount] = {};
  uint8_t  m_proto[kSlotCount] = {};
  bool     m_bound[kSlotCount] = {};
};

// ── the state machine ───────────────────────────────────────────────────────
// Every comparison against millis() is a signed difference, never `a < b` on
// the raw values: the panel runs for months, millis() wraps at 49.7 days, and
// an unsigned comparison across that wrap would hold the button down for weeks.
// NickoScope32 paid for this one in an audit (v33.48.0 HIGH-1) before the port.
class Decoder {
 public:
  void reset() {
    m_map.reset();
    m_rot = 0; m_okUntilMs = 0; m_hitSlot = -1; m_hitMs = 0;
    m_learnSlot = -1; m_learnUntilMs = 0;
    m_seenAny = false; m_seenProto = 0; m_seenValue = 0; m_seenRepeat = false; m_seenMs = 0;
    m_frames = 0; m_ignored = 0;
    for (uint8_t i = 0; i < kSlotCount; i++) m_hits[i] = 0;
  }

  Map       &map()       { return m_map; }
  const Map &map() const { return m_map; }

  Outcome frame(uint32_t nowMs, const Frame &f) {
    Outcome o{Outcome::kNothing, -1, -1};
    m_frames++;

    // The live journal, for the portal's "press a button and watch": a repeat
    // frame carries no code, so it never overwrites the value shown.
    m_seenAny = true;
    m_seenMs = nowMs;
    m_seenRepeat = f.repeat;
    if (!f.repeat) { m_seenValue = f.value; m_seenProto = f.proto; }

    // Learning consumes the frame: a button being taught must not also act.
    if (learnActive(nowMs) >= 0) {
      if (f.repeat || f.unknown || f.value == 0) { m_ignored++; return o; }
      const int dup = m_map.match(f.proto, f.value);
      const int8_t into = m_learnSlot;
      if (dup >= 0 && dup != into) { m_map.clear((uint8_t)dup); o.movedFrom = (int8_t)dup; }
      m_map.bind((uint8_t)into, f.proto, f.value);
      m_learnSlot = -1;
      m_hitSlot = into; m_hitMs = nowMs;        // the portal lights the slot it just learned
      o.kind = Outcome::kLearned; o.slot = into;
      return o;
    }

    int slot;
    if (f.repeat) {
      // No code of its own: it extends the slot that is being held, and only
      // while that slot is still fresh.
      if (m_hitSlot < 0 || (int32_t)(nowMs - m_hitMs) > (int32_t)kRepeatFreshMs) { m_ignored++; return o; }
      slot = m_hitSlot;
    } else {
      if (f.unknown) { m_ignored++; return o; }
      slot = m_map.match(f.proto, f.value);
      if (slot < 0) { m_ignored++; return o; }   // somebody else's remote, or a button nobody taught
    }
    return dispatch(nowMs, (uint8_t)slot, f.repeat);
  }

  // The simulator: the same dispatch a decoded frame reaches, so a serial or
  // portal test exercises the seam, the encoder's state machine and the pages,
  // with no receiver soldered and no codes learned. holdMs applies to the
  // button only - it is how a long press is tested.
  Outcome simulate(uint32_t nowMs, uint8_t slot, uint32_t holdMs) {
    if (slot >= kSlotCount) return Outcome{Outcome::kNothing, -1, -1};
    if (slot == kOk && holdMs > 0) {
      m_hitSlot = (int8_t)slot; m_hitMs = nowMs; m_hits[slot]++;
      m_okUntilMs = nowMs + holdMs;
      return Outcome{Outcome::kButton, (int8_t)slot, -1};
    }
    return dispatch(nowMs, slot, false);
  }

  // ── the seam (called from the encoder's sampling task) ────────────────────
  int8_t takeRotate() { const int8_t v = m_rot; m_rot = 0; return v; }
  bool   okDown(uint32_t nowMs) const { return (int32_t)(nowMs - m_okUntilMs) < 0; }

  // ── learning ──────────────────────────────────────────────────────────────
  bool learnArm(uint8_t slot, uint32_t nowMs) {
    if (slot >= kSlotCount) return false;
    m_learnSlot = (int8_t)slot;
    m_learnUntilMs = nowMs + kLearnWindowMs;
    return true;
  }
  void learnCancel() { m_learnSlot = -1; }
  int  learnActive(uint32_t nowMs) {
    if (m_learnSlot >= 0 && (int32_t)(nowMs - m_learnUntilMs) >= 0) m_learnSlot = -1;   // the window closed
    return m_learnSlot;
  }
  uint32_t learnRemainMs(uint32_t nowMs) {
    if (learnActive(nowMs) < 0) return 0;
    const int32_t d = (int32_t)(m_learnUntilMs - nowMs);
    return d > 0 ? (uint32_t)d : 0;
  }

  // ── what the portal and /api/info show ────────────────────────────────────
  bool     lastSeen(uint8_t *proto, uint64_t *value, bool *repeat, uint32_t *ageMs, uint32_t nowMs) const {
    if (!m_seenAny) return false;
    if (proto) *proto = m_seenProto;
    if (value) *value = m_seenValue;
    if (repeat) *repeat = m_seenRepeat;
    if (ageMs) *ageMs = (uint32_t)(nowMs - m_seenMs);
    return true;
  }
  int      lastHitSlot(uint32_t nowMs, uint32_t withinMs) const {
    if (m_hitSlot < 0) return -1;
    return (uint32_t)(nowMs - m_hitMs) <= withinMs ? m_hitSlot : -1;
  }
  uint32_t hits(uint8_t s) const { return s < kSlotCount ? m_hits[s] : 0; }
  uint32_t frames() const { return m_frames; }
  uint32_t ignored() const { return m_ignored; }

 private:
  Outcome dispatch(uint32_t nowMs, uint8_t slot, bool repeat) {
    Outcome o{Outcome::kNothing, (int8_t)slot, -1};
    m_hitSlot = (int8_t)slot;
    m_hitMs = nowMs;
    m_hits[slot]++;
    switch (slot) {
      case kCcw:
        if (m_rot > -kRotAccMax) m_rot--;
        o.kind = Outcome::kRotate;
        break;
      case kCw:
        if (m_rot < kRotAccMax) m_rot++;
        o.kind = Outcome::kRotate;
        break;
      case kOk:
        // A level, not an event: it is held while frames keep arriving, and the
        // encoder's own debounce then decides click or long press. This is why
        // a held remote button behaves exactly like a held knob.
        m_okUntilMs = nowMs + kHoldMs;
        o.kind = Outcome::kButton;
        break;
      default:
        o.kind = repeat ? Outcome::kNothing : Outcome::kReserved;
        break;
    }
    return o;
  }

  Map      m_map;
  int8_t   m_rot = 0;
  uint32_t m_okUntilMs = 0;
  int8_t   m_hitSlot = -1;
  uint32_t m_hitMs = 0;
  int8_t   m_learnSlot = -1;
  uint32_t m_learnUntilMs = 0;
  bool     m_seenAny = false;
  uint8_t  m_seenProto = 0;
  uint64_t m_seenValue = 0;
  bool     m_seenRepeat = false;
  uint32_t m_seenMs = 0;
  uint32_t m_frames = 0;
  uint32_t m_ignored = 0;
  uint32_t m_hits[kSlotCount] = {};
};

}  // namespace ir

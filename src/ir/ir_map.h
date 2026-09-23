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

// ── the buttons and their functions ─────────────────────────────────────────
// Ten buttons, each a code learned from the remote and a function chosen for
// it in the portal (owner's list and layout, approved 2026-09-23). A button is
// a place on the remote; a function is a thing the panel does, and they are
// kept apart so any remote works and any button can do anything on the list.
//
// "Slot" in the code below is a button's index, 0..9; people see 1..10.
static const uint8_t kSlotCount = 10;

enum Fn : uint8_t {
  kFnNone = 0,
  // what the knob does - through the seam into src/control's state machine, so
  // browse, select and long press keep one implementation
  kFnCcw, kFnCw, kFnOk, kFnLong,
  // the rest run in loop(), through src/ir/ir_actions.cpp
  kFnPower, kFnBrightUp, kFnBrightDown,
  kFnHome, kFnNextStyle, kFnCarousel, kFnAmbient, kFnPage,
  kFnMediaToggle, kFnMediaNext, kFnMediaPrev, kFnVolUp, kFnVolDown,
  kFnDismiss,
  kFnCount
};

struct FnInfo {
  const char *name;    // machine name: NVS-independent, used by the console and the API
  const char *label;   // what the portal's drop-down shows
  const char *group;
  bool        repeats; // holding the button repeats it
};
inline const FnInfo &fnInfo(uint8_t f) {
  static const FnInfo kInfo[kFnCount + 1] = {
      {"none",         "No action",                 "Other",      false},
      {"ccw",          "Back (turn left)",          "Navigation", true},
      {"cw",           "Forward (turn right)",      "Navigation", true},
      {"ok",           "OK / select (press)",       "Navigation", false},
      {"long",         "Long press",                "Navigation", false},
      {"power",        "Screen on / off",           "Screen",     false},
      {"bright_up",    "Brightness +10%",           "Screen",     true},
      {"bright_down",  "Brightness -10%",           "Screen",     true},
      {"home",         "Home: the clock",           "Pages",      false},
      {"next_style",   "Next clock style",          "Pages",      true},
      {"carousel",     "Carousel on / off",         "Pages",      false},
      {"ambient",      "Screensaver on / off",      "Pages",      false},
      {"page",         "Go to page...",             "Pages",      false},
      {"media_toggle", "Play / pause",              "Media",      false},
      {"media_next",   "Next track",                "Media",      false},
      {"media_prev",   "Previous track",            "Media",      false},
      {"vol_up",       "Volume +",                  "Media",      true},
      {"vol_down",     "Volume -",                  "Media",      true},
      {"dismiss",      "Dismiss the notification",  "Other",      false},
      {"?",            "?",                         "?",          false},
  };
  return kInfo[f < kFnCount ? f : kFnCount];
}
inline bool fnDrivesKnob(uint8_t f) { return f == kFnCcw || f == kFnCw || f == kFnOk || f == kFnLong; }

// A function by its machine name, case-insensitive. The console and the API
// both go through this, so a name means the same typed or posted.
inline bool fnByName(const char *w, uint8_t *out) {
  if (!w || !*w || !out) return false;
  for (uint8_t i = 0; i < kFnCount; i++) {
    const char *a = w, *b = fnInfo(i).name;
    for (;; a++, b++) {
      const char la = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
      if (la != *b) break;
      if (!la) { *out = i; return true; }
    }
  }
  return false;
}

// The layout a fresh panel starts with (owner, 2026-09-23).
inline uint8_t defaultFn(uint8_t slot) {
  static const uint8_t kDefault[kSlotCount] = {
      kFnCcw, kFnCw, kFnOk, kFnLong, kFnPower,
      kFnBrightUp, kFnBrightDown, kFnHome, kFnCarousel, kFnMediaToggle};
  return slot < kSlotCount ? kDefault[slot] : kFnNone;
}

// A button by its number as people see it, 1..10.
inline bool slotByNumber(const char *w, uint8_t *out) {
  if (!w || !*w || !out) return false;
  uint32_t v = 0;
  for (const char *p = w; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (uint32_t)(*p - '0');
    if (v > 1000) return false;
  }
  if (v < 1 || v > kSlotCount) return false;
  *out = (uint8_t)(v - 1);
  return true;
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
// A button set to "long press" holds the knob's switch down this long: past
// src/control's 1000 ms threshold with a debounce and a lost repeat to spare.
// Our choice.
static const uint32_t kLongSpanMs    = 1300;
// Holding a repeating action (brightness, volume, next style): it fires on the
// press, waits kActionDelayMs, then fires at most every kActionRepeatMs as the
// 108 ms repeat frames arrive - about three steps a second rather than nine.
// Our choice, the pace a TV remote's volume key keeps.
static const uint32_t kActionDelayMs  = 500;
static const uint32_t kActionRepeatMs = 300;
// The ceiling on a simulated hold. The portal's /api/ir/sim takes it from a
// query string and the serial console from a typed line, so it is clamped where
// both meet the state machine. Five seconds is four times the encoder's
// long-press threshold - everything a test could want, and nothing that could
// pin the button down.
static const uint32_t kSimHoldMaxMs = 5000;

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
    kReserved,      // a learned button set to "No action"
    kLearned,       // the open learn window bound this code
    kAction,        // run fn with arg in loop() - src/ir/ir_actions.cpp
  } kind;
  int8_t slot;       // the button involved, -1 when none
  int8_t movedFrom;  // learning took this code away from that button, -1 if it did not
  // Left out of a brace initializer, these two are zero - kFnNone and page 0.
  // No default member initializers: the firmware builds as C++11, where they
  // would stop Outcome{...} from being an aggregate.
  uint8_t fn;
  uint8_t arg;       // the page, for kFnPage
};

class Map {
 public:
  Map() { reset(); }
  void reset() {
    for (uint8_t i = 0; i < kSlotCount; i++) {
      m_value[i] = 0; m_proto[i] = 0; m_bound[i] = false;
      m_fn[i] = defaultFn(i); m_arg[i] = 0;
    }
  }
  // What a button does. Kept apart from its code: forgetting a code keeps the
  // function, and choosing a function needs no remote in the room.
  uint8_t fn(uint8_t s) const { return s < kSlotCount ? m_fn[s] : (uint8_t)kFnNone; }
  uint8_t arg(uint8_t s) const { return s < kSlotCount ? m_arg[s] : 0; }
  bool setFn(uint8_t s, uint8_t f, uint8_t a) {
    if (s >= kSlotCount || f >= kFnCount) return false;
    m_fn[s] = f; m_arg[s] = f == kFnPage ? a : 0;
    return true;
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
  uint8_t  m_fn[kSlotCount] = {};
  uint8_t  m_arg[kSlotCount] = {};
};

// ── the state machine ───────────────────────────────────────────────────────
// millis() wraps at 49.7 days and this panel runs for months, so no comparison
// here is a plain `a < b` on raw timestamps. The rule is not "always signed",
// though - that was the first version of this file and an audit found three
// bugs in it on 2026-09-16:
//
//   An AGE - how long ago something happened - is an UNSIGNED elapsed time.
//   A signed difference against a timestamp that is genuinely old reads as
//   negative and calls it recent: a repeat frame revived a slot last seen a
//   month before.
//
//   A DEADLINE - "held until" - is not compared at all here. It is stored as a
//   start plus a span, and the span is zeroed the moment it runs out. A signed
//   comparison against a deadline that was never set (zero) called the button
//   held from day 24.85 onwards, on any panel, with no remote in the room.
//
// Both forms are exercised at 25.5 days by tools/ir/check_ir.py.
class Decoder {
 public:
  void reset() {
    m_map.reset();
    m_rot = 0; m_okSinceMs = 0; m_okSpanMs = 0; m_hitSlot = -1; m_hitMs = 0;
    m_pressMs = 0; m_fireMs = 0;
    m_learnSlot = -1; m_learnAtMs = 0;
    m_seenAny = false; m_seenProto = 0; m_seenValue = 0; m_seenRepeat = false; m_seenMs = 0;
    m_frames = 0; m_ignored = 0;
    for (uint8_t i = 0; i < kSlotCount; i++) m_hits[i] = 0;
  }

  Map       &map()       { return m_map; }
  const Map &map() const { return m_map; }

  Outcome frame(uint32_t nowMs, const Frame &f) {
    Outcome o{Outcome::kNothing, -1, -1, kFnNone, 0};
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
      if (m_hitSlot < 0 || (uint32_t)(nowMs - m_hitMs) > kRepeatFreshMs) { m_ignored++; return o; }
      slot = m_hitSlot;
    } else {
      if (f.unknown) { m_ignored++; return o; }
      slot = m_map.match(f.proto, f.value);
      if (slot < 0) { m_ignored++; return o; }   // somebody else's remote, or a button nobody taught
    }
    m_hitSlot = (int8_t)slot;
    m_hitMs = nowMs;
    m_hits[slot]++;
    return run(nowMs, m_map.fn((uint8_t)slot), m_map.arg((uint8_t)slot), f.repeat, (int8_t)slot);
  }

  // The simulator: the same path a decoded frame takes, so a serial or portal
  // test exercises the seam, the encoder's state machine, the actions and the
  // pages with no receiver soldered and no codes learned.
  //   simulate(slot)   presses a button - whatever function it is set to;
  //   simulateFn(fn)   runs a function directly, no button needed.
  // holdMs holds the knob's switch for kFnOk (how a long press is tested) and
  // is clamped here, the one place every simulated press goes through: the
  // portal takes it from a query string, and an unbounded one would hold the
  // button down for weeks.
  Outcome simulate(uint32_t nowMs, uint8_t slot, uint32_t holdMs) {
    if (slot >= kSlotCount) return Outcome{Outcome::kNothing, -1, -1, kFnNone, 0};
    m_hitSlot = (int8_t)slot; m_hitMs = nowMs; m_hits[slot]++;
    return runHeld(nowMs, m_map.fn(slot), m_map.arg(slot), holdMs, (int8_t)slot);
  }
  Outcome simulateFn(uint32_t nowMs, uint8_t fn, uint8_t arg, uint32_t holdMs) {
    if (fn >= kFnCount) return Outcome{Outcome::kNothing, -1, -1, kFnNone, 0};
    return runHeld(nowMs, fn, fn == kFnPage ? arg : 0, holdMs, -1);
  }

  // ── the seam (called from the encoder's sampling task) ────────────────────
  int8_t takeRotate() { const int8_t v = m_rot; m_rot = 0; return v; }
  // Polled by the encoder's task at 1 kHz, so the span is cleared within a
  // millisecond of running out and can never be mistaken for a fresh press.
  bool okDown(uint32_t nowMs) {
    if (!m_okSpanMs) return false;
    if ((uint32_t)(nowMs - m_okSinceMs) >= m_okSpanMs) { m_okSpanMs = 0; return false; }
    return true;
  }

  // ── learning ──────────────────────────────────────────────────────────────
  bool learnArm(uint8_t slot, uint32_t nowMs) {
    if (slot >= kSlotCount) return false;
    m_learnSlot = (int8_t)slot;
    m_learnAtMs = nowMs;
    return true;
  }
  void learnCancel() { m_learnSlot = -1; }
  int  learnActive(uint32_t nowMs) {
    if (m_learnSlot >= 0 && (uint32_t)(nowMs - m_learnAtMs) >= kLearnWindowMs) m_learnSlot = -1;
    return m_learnSlot;
  }
  uint32_t learnRemainMs(uint32_t nowMs) {
    if (learnActive(nowMs) < 0) return 0;
    return kLearnWindowMs - (uint32_t)(nowMs - m_learnAtMs);
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
  Outcome runHeld(uint32_t nowMs, uint8_t fn, uint8_t arg, uint32_t holdMs, int8_t slot) {
    if (fn == kFnOk && holdMs > 0) {
      if (holdMs > kSimHoldMaxMs) holdMs = kSimHoldMaxMs;
      m_okSinceMs = nowMs; m_okSpanMs = holdMs;
      Outcome o{Outcome::kButton, slot, -1, kFnNone, 0};
      o.fn = fn;
      return o;
    }
    return run(nowMs, fn, arg, false, slot);
  }

  // One press or one repeat frame of a function.
  Outcome run(uint32_t nowMs, uint8_t fn, uint8_t arg, bool repeat, int8_t slot) {
    Outcome o{Outcome::kNothing, slot, -1, kFnNone, 0};
    o.fn = fn;
    switch (fn) {
      case kFnCcw:
        // A detent per frame, repeats included: holding the button walks, as
        // turning the knob does.
        if (m_rot > -kRotAccMax) m_rot--;
        o.kind = Outcome::kRotate;
        break;
      case kFnCw:
        if (m_rot < kRotAccMax) m_rot++;
        o.kind = Outcome::kRotate;
        break;
      case kFnOk:
        // A level, not an event: held while frames keep arriving, and the
        // encoder's own debounce then decides click or long press. This is why
        // a held remote button behaves exactly like a held knob.
        m_okSinceMs = nowMs; m_okSpanMs = kHoldMs;
        o.kind = Outcome::kButton;
        break;
      case kFnLong:
        // One long press per press of the button: its repeats change nothing.
        if (!repeat) { m_okSinceMs = nowMs; m_okSpanMs = kLongSpanMs; }
        o.kind = repeat ? Outcome::kNothing : Outcome::kButton;
        break;
      case kFnNone:
        o.kind = repeat ? Outcome::kNothing : Outcome::kReserved;
        break;
      default: {
        // An action: fired on the press; on a held button, again only after
        // kActionDelayMs and then at most every kActionRepeatMs, and only for
        // the functions that repeat. Ages are unsigned (see above).
        if (!repeat) {
          m_pressMs = nowMs; m_fireMs = nowMs;
        } else if (!fnInfo(fn).repeats ||
                   (uint32_t)(nowMs - m_pressMs) < kActionDelayMs ||
                   (uint32_t)(nowMs - m_fireMs) < kActionRepeatMs) {
          return o;                                // held, not yet due: nothing
        } else {
          m_fireMs = nowMs;
        }
        o.kind = Outcome::kAction;
        o.arg = fn == kFnPage ? arg : 0;
        break;
      }
    }
    return o;
  }

  Map      m_map;
  int8_t   m_rot = 0;
  uint32_t m_okSinceMs = 0;
  uint32_t m_okSpanMs = 0;    // 0 = not held; never compared against a bare timestamp
  int8_t   m_hitSlot = -1;
  uint32_t m_hitMs = 0;
  int8_t   m_learnSlot = -1;
  uint32_t m_learnAtMs = 0;
  bool     m_seenAny = false;
  uint8_t  m_seenProto = 0;
  uint64_t m_seenValue = 0;
  bool     m_seenRepeat = false;
  uint32_t m_seenMs = 0;
  uint32_t m_pressMs = 0;     // when the held action's button went down
  uint32_t m_fireMs = 0;      // when the held action last fired
  uint32_t m_frames = 0;
  uint32_t m_ignored = 0;
  uint32_t m_hits[kSlotCount] = {};
};

}  // namespace ir

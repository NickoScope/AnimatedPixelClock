#pragma once
// Physical control: one EC11 rotary encoder with a push switch.
//
// The enclosure puts it at the right-hand end of the 20 mm bottom strip, on the
// swappable insert. This module owns the pins, the decoding and the gesture
// timing; pages receive events and never touch a GPIO.
//
// Gestures - the owner's call on 2026-09-14, "the click is only select":
//
//   rotate       browse: every clock style, then every page, then the cards
//   click        select: enter a page that has controls of its own, where
//                rotation then acts inside it; click again to come back out
//   long press   the same as a click - the switch has one meaning
//
// Page switching used to ride on a long press. With the click kept for
// selecting, rotation carries it, and a long press adds nothing of its own.

#include <stdint.h>

#if defined(CONTROL_ENCODER_ENABLED)

// Build-time defaults. The web portal can change all four while the panel runs
// (controlConfigure, persisted by src/panel); these are what it starts from.
// CTRL_REVERSE 1 if clockwise walks backwards - instead of swapping A and B.
#ifndef CTRL_REVERSE
#define CTRL_REVERSE 0
#endif
#ifndef CTRL_ENC_LOCKOUT_MS
#define CTRL_ENC_LOCKOUT_MS 10
#endif
#ifndef CTRL_SW_DEBOUNCE_MS
#define CTRL_SW_DEBOUNCE_MS 20
#endif
// -1 find out from the knob (default), 0 detents at 11 only, 1 at 11 and 00.
#ifndef CTRL_ENC_HALF_DETENT
#define CTRL_ENC_HALF_DETENT -1
#endif

enum CtrlEvent : uint8_t {
  CTRL_NONE = 0,
  CTRL_CW,          // one detent clockwise
  CTRL_CCW,         // one detent anticlockwise
  CTRL_PRESS,       // released before the long-press threshold
  CTRL_LONG,        // held past it; fires once, on the threshold, not on release
};

void      controlBegin();
void      controlLoop();          // poll; cheap, safe to call every frame
CtrlEvent controlTake();          // pop one event, CTRL_NONE when the queue is empty

// True while the knob is held. Pages use it to show a hint before the long
// press completes, so the gesture is discoverable rather than folklore.
bool      controlHeld();
uint32_t  controlHeldMs();

// Called from loop() (the web handlers run there). The sampling timer picks the
// values up on its next tick; a changed detent mode restarts the learning.
void      controlConfigure(bool reverse, uint16_t lockoutMs, uint16_t debounceMs, int8_t detent);

// What the decoder has seen since boot, for the portal's knob tester. Counts
// are of decoded gestures, including any the full queue had to drop.
struct CtrlStats {
  uint32_t cw, ccw, press, longPress;
  uint8_t  last;           // CtrlEvent
  uint32_t lastMs;         // millis() of the last event, 0 = none yet
  int8_t   detent;         // in force now: -1 still learning, 0 at 11, 1 at 11 and 00
  bool     timer;          // sampled at 1 kHz; false = at loop speed
};
void      controlStats(CtrlStats *out);

#endif  // CONTROL_ENCODER_ENABLED

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

#endif  // CONTROL_ENCODER_ENABLED

#pragma once
// Physical control: one EC11 rotary encoder with a push switch.
//
// The enclosure puts it at the right-hand end of the 20 mm bottom strip, on the
// swappable insert. This module owns the pins, the decoding and the gesture
// timing; pages receive events and never touch a GPIO.
//
// Gestures, and what they mean everywhere:
//
//   rotate       change the value this page is about
//   short press  toggle this page's second axis
//   long press   go to the next page
//
// Long press carries page switching rather than the "force refresh" the
// original flight board note gave it. That note predates there being more than
// one page, and switching is the one action every page needs; a refresh is
// better handled by the page itself when a selection settles, with no gesture
// at all.

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

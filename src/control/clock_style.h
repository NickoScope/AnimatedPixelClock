#pragma once
// The clock page's knob: step through styles, and say which one you landed on.

#include <stdint.h>

#if defined(CONTROL_ENCODER_ENABLED)

void clockStyleStep(int8_t delta);      // rotate: next/previous style
void clockStyleToggleRotation();        // press: Custom rotation, and back again
void clockStyleTick();                  // call from loop(): handles the deferred save
void clockStyleOverlay();               // call after the clock draws: the name toast

#endif

#pragma once
// The clock page's knob: step through styles, and say which one you landed on.

#include <stdint.h>

#if defined(CONTROL_ENCODER_ENABLED)

void clockStyleStep(int8_t delta);      // rotate: next/previous style
void clockStyleToggleRotation();        // press: Custom rotation, and back again
void clockStyleTick();                  // call from loop(): handles the deferred save
void clockStyleOverlay();               // call after the clock draws: the name toast
// The carousel's step on the clock page: next style, shown but not saved.
// Returns false once the last style has had its turn - time for the next page.
bool clockStyleCarouselNext();
// The knob's browse: one real style per detent, saved like any knob turn.
// Returns false when that would step off either end - the caller then moves
// to the neighbouring page instead.
bool clockStyleBrowse(int8_t delta);
// Arriving on the clock page while browsing: the first style coming forward,
// the last coming back.
void clockStyleBrowseEnter(int8_t delta);
// A banner along the bottom over any page, as long as a style name shows. The
// text must outlive it: a literal or a static buffer.
void ctrlToast(const char *text);

#endif

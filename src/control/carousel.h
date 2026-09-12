#pragma once
// The pages advance on their own when nobody is at the panel.
//
// No new gesture and no setting: touching the knob puts you in charge, and the
// carousel picks up again once you have walked away. A wall panel is looked at
// from across a room far more often than it is operated, and a device that only
// changes when you touch it shows you the same thing all evening.

#include <stdint.h>

#if defined(CAROUSEL_ENABLED) && !defined(CONTROL_ENCODER_ENABLED)
#error "CAROUSEL_ENABLED needs CONTROL_ENCODER_ENABLED: it advances the knob's pages"
#endif

#if defined(CAROUSEL_ENABLED)

// Seconds of no knob activity before the panel starts advancing by itself.
#define CAROUSEL_IDLE_S 60

void carouselNote();                     // the knob was used; hold everything
bool carouselDue(uint16_t pageSeconds);  // true once, when this page has had its turn
bool carouselRunning();                  // for a page that wants to show it

#endif

#pragma once
// The pages advance on their own when nobody is at the panel.
//
// No new gesture: touching the knob puts you in charge, and the carousel picks
// up again once you have walked away. The numbers below are defaults; the web
// portal can change them at run time (src/panel). A wall panel is looked at
// from across a room far more often than it is operated, and a device that only
// changes when you touch it shows you the same thing all evening.

#include <stdint.h>

#if defined(CAROUSEL_ENABLED) && !defined(CONTROL_ENCODER_ENABLED)
#error "CAROUSEL_ENABLED needs CONTROL_ENCODER_ENABLED: it advances the knob's pages"
#endif
#if defined(CAROUSEL_ALL_STYLES) && !defined(CAROUSEL_ENABLED)
#error "CAROUSEL_ALL_STYLES needs CAROUSEL_ENABLED: it changes what the carousel walks"
#endif

#if defined(CAROUSEL_ENABLED)

// Seconds of no knob activity before the panel starts advancing by itself.
#define CAROUSEL_IDLE_S 60

#if defined(CAROUSEL_ALL_STYLES)
// Show everything, one slot each: every page, and on the clock page every clock
// style in turn, before moving on. The owner's call on 2026-09-14, while the
// panel ran without its knob. Without the flag the pages keep their own times.
#define CAROUSEL_SLOT_S 15
#endif

void carouselNote();                     // the knob was used; hold everything
bool carouselDue(uint16_t pageSeconds);  // true once, when this page has had its turn
bool carouselRunning();                  // for a page that wants to show it
void carouselConfigure(bool enabled, uint16_t idleS);
uint32_t carouselHoldMs();               // until it starts again; 0 when running or off
uint32_t carouselPageMs();               // how long the current page has had

#endif

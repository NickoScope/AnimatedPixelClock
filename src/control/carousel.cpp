#include "carousel.h"

#if defined(CAROUSEL_ENABLED)

#include <Arduino.h>

static uint32_t s_lastTouch = 0;
static uint32_t s_pageSince = 0;
// Run-time copies of the defaults in carousel.h; src/panel sets them from NVS.
static bool     s_enabled   = true;
static uint32_t s_idleMs    = (uint32_t)CAROUSEL_IDLE_S * 1000UL;

void carouselNote() {
  const uint32_t now = millis();
  s_lastTouch = now;
  s_pageSince = now;      // a page you chose gets its full turn from now
}

void carouselConfigure(bool enabled, uint16_t idleS) {
  if (enabled && !s_enabled) s_pageSince = millis();   // switched on: a full turn, not an instant jump
  s_enabled = enabled;
  s_idleMs  = (uint32_t)idleS * 1000UL;
}

bool carouselRunning() {
  return s_enabled && (millis() - s_lastTouch) > s_idleMs;
}

uint32_t carouselHoldMs() {
  if (!s_enabled) return 0;
  const uint32_t idle = millis() - s_lastTouch;
  return idle >= s_idleMs ? 0 : s_idleMs - idle;
}

uint32_t carouselPageMs() {
  return s_pageSince ? millis() - s_pageSince : 0;
}

bool carouselDue(uint16_t pageSeconds) {
  const uint32_t now = millis();
  if (!s_pageSince) s_pageSince = now;
  if (!carouselRunning()) return false;
  if (!pageSeconds) return false;                       // 0 means "stay here"
  if ((now - s_pageSince) < (uint32_t)pageSeconds * 1000UL) return false;
  s_pageSince = now;
  return true;
}

#endif

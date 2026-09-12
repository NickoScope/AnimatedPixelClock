#include "carousel.h"

#if defined(CAROUSEL_ENABLED)

#include <Arduino.h>

static uint32_t s_lastTouch = 0;
static uint32_t s_pageSince = 0;

void carouselNote() {
  const uint32_t now = millis();
  s_lastTouch = now;
  s_pageSince = now;      // a page you chose gets its full turn from now
}

bool carouselRunning() {
  return (millis() - s_lastTouch) > (uint32_t)CAROUSEL_IDLE_S * 1000UL;
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

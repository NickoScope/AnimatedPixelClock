#include "clock_style.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <Arduino.h>
#include <string.h>

#include "../clocks/clocks.h"
#include "../config/config.h"
#include "../config/settings.h"
#include "../display/display.h"
#include "../fonts/picopixel_fb.h"
#include "clock_styles.h"

// The name shows for long enough to read and no longer. Styles like Snake and
// Pac-Man are a second or two of animation apart, so without this the knob
// tells you nothing until the clock happens to do something recognisable.
static const uint32_t TOAST_MS = 1600;

// saveSettings() rewrites the whole settings blob in NVS. Spinning through
// fifteen styles would be fifteen writes for fourteen choices nobody made, so
// the write waits until the knob stops moving. Flash wear is the reason; the
// side benefit is that a fast spin costs one write, not fifteen.
static const uint32_t SETTLE_MS = 2500;

static uint32_t s_toastAt  = 0;
static uint32_t s_dirtyAt  = 0;
static uint8_t  s_beforeRotation = 0xFF;   // style to come back to, 0xFF = none

static int8_t indexOf(uint8_t id) {
  for (uint8_t i = 0; i < CLOCK_STYLE_COUNT; i++)
    if (kClockStyles[i].id == id) return (int8_t)i;
  return -1;
}

static const char *nameOf(uint8_t id) {
  const int8_t i = indexOf(id);
  return i < 0 ? "?" : kClockStyles[i].name;
}

static void applyStyle(uint8_t id) {
  if (settings.clockStyle == id) return;
  settings.clockStyle = id;
  resetClockAnimationState();   // every clock keeps animation state; start clean
  s_toastAt = millis();
  s_dirtyAt = millis();
}

void clockStyleStep(int8_t delta) {
  int8_t i = indexOf(settings.clockStyle);
  // An unknown id means the settings hold a style the UI no longer offers -
  // a retired one, or the 4/3 alias. Step onto the list rather than refusing.
  if (i < 0) i = 0;
  else       i = (int8_t)((i + delta + CLOCK_STYLE_COUNT) % CLOCK_STYLE_COUNT);
  s_beforeRotation = 0xFF;      // an explicit choice cancels the press-to-return
  applyStyle(kClockStyles[i].id);
}

void clockStyleToggleRotation() {
  const uint8_t ROTATION = 9;   // "Custom rotation" in the web UI
  if (settings.clockStyle == ROTATION) {
    applyStyle(s_beforeRotation == 0xFF ? kClockStyles[0].id : s_beforeRotation);
    s_beforeRotation = 0xFF;
  } else {
    s_beforeRotation = settings.clockStyle;
    applyStyle(ROTATION);
  }
}

void clockStyleTick() {
  if (s_dirtyAt && (millis() - s_dirtyAt) > SETTLE_MS) {
    s_dirtyAt = 0;
    saveSettings();
  }
}

void clockStyleOverlay() {
  if (!s_toastAt) return;
  if ((millis() - s_toastAt) > TOAST_MS) { s_toastAt = 0; return; }

  const char *name = nameOf(settings.clockStyle);
  display.setFont(&PicopixelFB);
  display.setTextWrap(false);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(name, 0, 0, &bx, &by, &bw, &bh);

  // A band across the bottom rather than a floating label: the clocks fill the
  // panel and there is no reliably empty corner to put text in.
  const int16_t h = 8;
  const int16_t y = display.height() - h;
  display.fillRect(0, y, display.width(), h, display.color565(0, 0, 0));
  display.drawFastHLine(0, y, display.width(), display.color565(40, 48, 54));
  display.setTextColor(display.color565(255, 180, 0));
  display.setCursor((display.width() - (int16_t)bw) / 2, y + 6);
  display.print(name);
  display.setFont(NULL);
}

#endif  // CONTROL_ENCODER_ENABLED

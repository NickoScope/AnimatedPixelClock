// What a remote button does when it is not being the knob.
//
// Each action calls exactly what the matching HTTP route calls, so a button
// and a GET behave the same and there is one implementation of each:
//
//   power         setDisplayForcedOff()          /api/display/on|off
//   bright_*      setDisplayBrightnessPercent()  /api/display/brightness
//   home, style   panelShowStyle()               /api/panel {"style"}
//   page          panelShowPage()                /api/panel {"show":{"page"}}
//   carousel      panelSetCarousel()             /api/panel {"carousel"}
//   ambient       the httpForce* flags           /api/mode/ambient, /api/mode/auto
//   media_*       media::sendSimple / nudgeVolume  the media page's own remote
//   dismiss       notifyDismiss()                /api/notify/dismiss
//
// Run from loop() only (irLoop and the portal's handlers), like those routes.
// The list and the default layout are the owner's (2026-09-23); ir_map.h has
// them. A function whose module is not in this build is not offered.

#if defined(IR_ENABLED)

#include "ir.h"

#include <Arduino.h>

#include "../config/config.h"
#include "../config/settings.h"
#include "../display/display.h"
#include "../notify/notify.h"

#if defined(CONTROL_ENCODER_ENABLED)
#include "../control/clock_styles.h"
#include "../panel/panel.h"
#endif
#if defined(MEDIAPLAYER_ENABLED)
#include "../media/media.h"
#endif

extern bool httpForceClock;
extern bool httpForceAmbient;
extern bool httpForceViz;

// Brightness steps, in percent. The stored value is 0..255 and rounds both ways
// (display.cpp), so ten steps of 10 land on the same values going up and down.
static const int kBrightStep = 10;
// Never below 1 %: 0 means "off" to the firmware (setDisplayBrightnessPercent
// forces the panel off), and a dimmer key should dim, not switch off.
static const int kBrightMin = 1;

bool irActionBuilt(uint8_t fn) {
  switch (fn) {
    case ir::kFnCcw: case ir::kFnCw: case ir::kFnOk: case ir::kFnLong:
    case ir::kFnHome: case ir::kFnNextStyle: case ir::kFnPage:
#if defined(CONTROL_ENCODER_ENABLED)
      return true;
#else
      return false;
#endif
    case ir::kFnCarousel:
#if defined(CAROUSEL_ENABLED)
      return true;
#else
      return false;
#endif
    case ir::kFnMediaToggle: case ir::kFnMediaNext: case ir::kFnMediaPrev:
    case ir::kFnVolUp: case ir::kFnVolDown:
#if defined(MEDIAPLAYER_ENABLED)
      return true;
#else
      return false;
#endif
    default:
      return fn < ir::kFnCount;
  }
}

static int brightnessPercent() {
  return settings.displayBrightness ? (settings.displayBrightness * 100 + 127) / 255 : 0;
}

#if defined(CONTROL_ENCODER_ENABLED)
// The style after the current one, in the order the knob walks them. The last
// table entry is Custom rotation, a mode rather than a look, so the walk stops
// before it and wraps to the first style, as clockStyleCarouselNext() does.
static uint8_t nextStyleId() {
  const int last = CLOCK_STYLE_COUNT - 2;
  int at = -1;
  for (int i = 0; i <= last; i++)
    if (kClockStyles[i].id == settings.clockStyle) at = i;
  return kClockStyles[(at < 0 || at >= last) ? 0 : at + 1].id;
}
#endif

// Brightness from the remote is kept across a power cycle, as the portal's is
// (the owner, 2026-09-23). Written once the buttons have been left alone for
// kBrightSaveMs, not on every press: one NVS key, but a row of presses is one
// write rather than ten. 0 = nothing to save.
static const uint32_t kBrightSaveMs = 3000;
static uint32_t s_brightSaveAt = 0;

void irActionsTick() {
  if (s_brightSaveAt && (uint32_t)(millis() - s_brightSaveAt) >= kBrightSaveMs) {
    s_brightSaveAt = 0;
    saveBrightnessSetting();
  }
}

void irRunAction(uint8_t fn, uint8_t arg) {
  if (!irActionBuilt(fn)) return;
  switch (fn) {
    case ir::kFnPower:
      setDisplayForcedOff(!isDisplayForcedOff());
      break;
    case ir::kFnBrightUp: {
      int p = brightnessPercent() + kBrightStep;
      setDisplayBrightnessPercent((uint8_t)(p > 100 ? 100 : p));
      s_brightSaveAt = millis() | 1;   // saved once the presses stop (irActionsTick)
      break;
    }
    case ir::kFnBrightDown: {
      int p = brightnessPercent() - kBrightStep;
      setDisplayBrightnessPercent((uint8_t)(p < kBrightMin ? kBrightMin : p));
      s_brightSaveAt = millis() | 1;
      break;
    }
#if defined(CONTROL_ENCODER_ENABLED)
    case ir::kFnHome:
      panelShowStyle(settings.clockStyle);   // the clock page, in the style it has
      break;
    case ir::kFnNextStyle:
      panelShowStyle(nextStyleId());
      break;
    case ir::kFnPage:
      if (arg < panelPageCount()) panelShowPage(arg);
      break;
#endif
#if defined(CAROUSEL_ENABLED)
    case ir::kFnCarousel: {
      PanelCarousel c = panelCarousel();
      c.enabled = !c.enabled;
      panelSetCarousel(c);
      break;
    }
#endif
    case ir::kFnAmbient:
      if (httpForceAmbient) {            // on: back to automatic, as /api/mode/auto
        httpForceAmbient = false;
        httpForceClock = false;
        httpForceViz = false;
      } else {                           // off: on, as /api/mode/ambient
        httpForceAmbient = true;
        httpForceClock = false;
        httpForceViz = false;
      }
      break;
#if defined(MEDIAPLAYER_ENABLED)
    case ir::kFnMediaToggle: media::sendSimple("toggle"); break;
    case ir::kFnMediaNext:   media::sendSimple("next");   break;
    case ir::kFnMediaPrev:   media::sendSimple("prev");   break;
    // The media page knob's step, MEDIA_VOL_STEP per press.
    case ir::kFnVolUp:       media::nudgeVolume(MEDIA_VOL_STEP);  break;
    case ir::kFnVolDown:     media::nudgeVolume(-MEDIA_VOL_STEP); break;
#endif
    case ir::kFnDismiss:
      notifyDismiss();
      break;
    default:
      break;   // the knob's four go through the seam, not here; "none" does nothing
  }
}

#endif  // IR_ENABLED

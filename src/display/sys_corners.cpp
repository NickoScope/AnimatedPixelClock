#include "sys_corners.h"

#include <Arduino.h>
#include <WiFi.h>

#include "display.h"
#include "../config/config.h"
#include "../control/carousel.h"
#if defined(IR_ENABLED)
#include "../ir/ir.h"
#endif
#if defined(CONTROL_ENCODER_ENABLED)
#include "../panel/panel.h"   // panelEnteredPage(): a click has entered the page
#endif

// While the remote is heard: the dot shows for this long after the last frame,
// blinking at half this period. 108 ms NEC repeats keep it alive while held.
static const uint32_t kIrShowMs  = 700;
static const uint32_t kIrBlinkMs = 250;

// RSSI bands, see sys_corners.h for the source.
static const int kRssiGood = -67;
static const int kRssiFair = -80;

// 5 x 5, one byte a row, bit 4 the leftmost column.
static const uint8_t kA[5] = {0x0E, 0x11, 0x1F, 0x11, 0x11};
static const uint8_t kM[5] = {0x11, 0x1B, 0x15, 0x11, 0x11};
// 5 x 5 arrow: inside a page, the knob and the remote act on it.
static const uint8_t kIn[5] = {0x10, 0x18, 0x1C, 0x18, 0x10};
// 7 x 5 Wi-Fi fan, bit 6 the leftmost column.
static const uint8_t kWifi[5] = {0x3E, 0x41, 0x1C, 0x22, 0x08};
// 5 x 5 cross, for no Wi-Fi.
static const uint8_t kCross[5] = {0x11, 0x0A, 0x04, 0x0A, 0x11};
// 6 x 6 dot, bit 5 the leftmost column.
static const uint8_t kDot[6] = {0x1E, 0x3F, 0x3F, 0x3F, 0x3F, 0x1E};

static void glyph(int x, int y, const uint8_t *rows, int w, int h, uint16_t color) {
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
      if ((rows[r] >> (w - 1 - c)) & 1) display.drawPixel(x + c, y + r, color);
}

// The corners show only for this long after the remote was last heard, then
// leave the picture alone: the owner, 2026-09-24, "они мешают картинкам".
// Every frame of a press, repeats included, starts it again.
static const uint32_t kShowAfterIrMs = 5000;

void sysCornersDraw() {
  const int W = display.width();
#if defined(IR_ENABLED)
  {
    const int32_t age = irMsSinceFrame();
    if (age < 0 || (uint32_t)age >= kShowAfterIrMs) return;
  }
#else
  return;   // nothing wakes them without a remote
#endif

  // ── left: A / M ──
  // A: the carousel is on (automatic); dim while it is held after a hand
  // turned a page, until it resumes. M: it is off (manual). The first version
  // showed A only while it was walking, so switching it on from the remote
  // still read M for the whole 60 s hold - the owner took it for broken.
#if defined(CAROUSEL_ENABLED)
  const bool walking = carouselRunning();
  const bool autoWalk = walking || carouselHoldMs() > 0;
#else
  const bool walking = false, autoWalk = false;
#endif
  display.fillRect(0, 0, 6, 6, 0);
#if defined(CONTROL_ENCODER_ENABLED)
  if (panelEnteredPage()) {
    glyph(0, 0, kIn, 5, 5, display.color565(255, 150, 0));
  } else
#endif
  glyph(0, 0, autoWalk ? kA : kM, 5, 5,
        !autoWalk ? display.color565(255, 150, 0)
        : walking ? display.color565(0, 200, 110) : display.color565(0, 90, 50));

  // ── right: the remote, else Wi-Fi ──
  bool irNow = false;
#if defined(IR_ENABLED)
  const int32_t irAge = irMsSinceFrame();
  irNow = irAge >= 0 && (uint32_t)irAge < kIrShowMs;
#endif
  if (irNow) {
    display.fillRect(W - 8, 0, 8, 7, 0);
    if ((millis() / kIrBlinkMs) % 2 == 0) glyph(W - 6, 0, kDot, 6, 6, display.color565(255, 0, 0));
    return;
  }
  display.fillRect(W - 8, 0, 8, 6, 0);
  if (WiFi.status() != WL_CONNECTED) {
    glyph(W - 5, 0, kCross, 5, 5, display.color565(255, 0, 0));
    return;
  }
  const int rssi = WiFi.RSSI();
  const uint16_t color = rssi >= kRssiGood ? display.color565(0, 220, 60)
                       : rssi >= kRssiFair ? display.color565(255, 150, 0)
                                           : display.color565(255, 0, 0);
  glyph(W - 7, 0, kWifi, 7, 5, color);
}

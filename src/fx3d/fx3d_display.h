#pragma once
// The panel's display with a place for 3D (docs/27-fx3d.md in the knowledge
// base). Under FX3D_ENABLED the global `display` is one of these.
//
// Every drawing call a page makes goes straight to the panel, as before -
// unless a 3D look is on. Then the calls fill a copy of the frame in PSRAM
// instead, and display() hands that copy to fx3d, which puts the 3D picture on
// the panel just before the flip. No page knows about it.
//
// What is caught: the library's five virtual drawing calls, which Adafruit
// GFX's text, lines and shapes all end in, and its non-virtual ones, hidden by
// name - `display` is always this type, and no code in src/ holds a base
// pointer to it. The colours are converted exactly as the library does
// (MatrixDisplay::color565to888). Rotation is not supported: the
// firmware never calls setRotation(), so the library's transform() is the
// identity here.

#if defined(FX3D_ENABLED)

#include <stdint.h>
#include <string.h>

#include "../display/matrix_display.h"

class Fx3dDisplay : public MatrixDisplay {
 public:
  explicit Fx3dDisplay(const HUB75_I2S_CFG &cfg) : MatrixDisplay(cfg) {}

  // fx3d turns the capture on (a 128 x 64 x 3 buffer of codes, and what to do
  // with it at the flip) and off (nullptr).
  void capture(uint8_t *codes, void (*present)()) {
    cap_ = codes;
    present_ = present;
  }
  bool capturing() const { return cap_ != nullptr; }
  // fx3d's own pixels go to the panel past the capture.
  void panelPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    MatrixDisplay::drawPixelRGB888(x, y, r, g, b);
  }
  void panelHLineRGB888(int16_t x, int16_t y, int16_t w, uint8_t r, uint8_t g, uint8_t b) {
    MatrixDisplay::drawFastHLine(x, y, w, r, g, b);
  }

  // Adafruit GFX's virtual entry points, as the library overrides them.
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (!cap_) return MatrixDisplay::drawPixel(x, y, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    put(x, y, r, g, b);
  }
  void fillScreen(uint16_t c) override {
    if (!cap_) return MatrixDisplay::fillScreen(c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(0, 0, kW, kH, r, g, b);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    if (!cap_) return MatrixDisplay::fillRect(x, y, w, h, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, w, h, r, g, b);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) override {
    if (!cap_) return MatrixDisplay::fillRect(x, y, w, h, r, g, b);
    rect(x, y, w, h, r, g, b);
  }
  // A line shorter than one is one pixel, as the library draws it: its
  // drawFastVLine and drawFastHLine (3.0.14, the .h, lines 517-567) fall back
  // to a line of length 1 when the other side is not longer.
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    if (!cap_) return MatrixDisplay::drawFastVLine(x, y, h, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, 1, h < 1 ? 1 : h, r, g, b);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint8_t r, uint8_t g, uint8_t b) override {
    if (!cap_) return MatrixDisplay::drawFastVLine(x, y, h, r, g, b);
    rect(x, y, 1, h < 1 ? 1 : h, r, g, b);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    if (!cap_) return MatrixDisplay::drawFastHLine(x, y, w, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, w < 1 ? 1 : w, 1, r, g, b);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint8_t r, uint8_t g, uint8_t b) override {
    if (!cap_) return MatrixDisplay::drawFastHLine(x, y, w, r, g, b);
    rect(x, y, w < 1 ? 1 : w, 1, r, g, b);
  }

  // The library's non-virtual ones, hidden by name.
  void drawPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (!cap_) return MatrixDisplay::drawPixelRGB888(x, y, r, g, b);
    put(x, y, r, g, b);
  }
  void fillScreenRGB888(uint8_t r, uint8_t g, uint8_t b) {
    if (!cap_) return MatrixDisplay::fillScreenRGB888(r, g, b);
    rect(0, 0, kW, kH, r, g, b);
  }
  void clearScreen() {
    if (!cap_) return MatrixDisplay::clearScreen();
    memset(cap_, 0, (size_t)kW * kH * 3);
  }
  void clearDisplay() { clearScreen(); }
  // The flip: a look first puts its 3D picture of the captured frame on the panel.
  void display() {
    if (cap_ && present_) present_();
    MatrixDisplay::display();
  }

 private:
  static const int kW = HUB75_PANEL_W * HUB75_CHAIN, kH = HUB75_PANEL_H;
  uint8_t *cap_ = nullptr;
  void (*present_)() = nullptr;

  void put(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if ((uint16_t)x >= (uint16_t)kW || (uint16_t)y >= (uint16_t)kH) return;
    uint8_t *p = cap_ + 3 * (y * kW + x);
    p[0] = r;
    p[1] = g;
    p[2] = b;
  }
  // A fillRect with a side below one is caught as nothing. The library does
  // not draw nothing there: its fillRectDMA (3.0.14, the .cpp, lines
  // 986-1010) counts the side down in a do/while, wraps int16_t and paints a
  // band across the whole panel. That is a fault, not a look to copy.
  void rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) {
    if (w < 1 || h < 1) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;   // [x0, x1) x [y0, y1)
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kW) x1 = kW;
    if (y1 > kH) y1 = kH;
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) {
        uint8_t *p = cap_ + 3 * (yy * kW + xx);
        p[0] = r;
        p[1] = g;
        p[2] = b;
      }
  }
};

#endif  // FX3D_ENABLED

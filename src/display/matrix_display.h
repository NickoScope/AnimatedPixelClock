/*
 * AnimatedPixelClock - HUB75 RGB matrix display shim
 *
 * Thin Adafruit-GFX-compatible wrapper around ESP32-HUB75-MatrixPanel-DMA. The
 * animation code calls a global `display` object with the OLED-era frame model
 * (clearDisplay / draw / display), which this class maps onto the DMA panel.
 *
 * Verified hardware config baked in (Phase 1, real panels):
 *   - 2x Waveshare P2.5 64x64 HUB75E chained = 128x64
 *   - driver FM6126A, clkphase=false (fixes dropped rightmost column)
 *   - internal-SRAM DMA only (NOT PSRAM: stripes, flicker and failed TLS
 *     certificate checks when tried on 2026-09-14), double-buffered
 *   - pin map identical to bringup/hello_matrix.cpp
 *
 * Board variants: define BOARD_WAVESHARE_RGB_MATRIX to select the pin map of
 * the Waveshare ESP32-S3-RGB-Matrix driver board instead of the hand-wired one.
 */

#ifndef MATRIX_DISPLAY_H
#define MATRIX_DISPLAY_H

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "soc/gdma_struct.h"     // the scan probe: which descriptor the DMA is on

#define HUB75_PANEL_W 64
#define HUB75_PANEL_H 64
#define HUB75_CHAIN   2   // two panels chained -> 128x64

// Build the verified panel configuration. Returned by value at static-init time;
// no hardware is touched until display.begin() (called from initDisplay()).
// The frame a page draws lives in PSRAM, and display() copies the pixels that
// changed onto the panel's single DMA frame. Measured need, 2026-09-22/23: the
// Wi-Fi driver's 1,626 B receive buffers come from the DMA-capable internal
// pool, and with two DMA frames (131,072 B) that pool ran down to a few hundred
// bytes under portal load or TLS, and the radio dropped off the network. One
// frame frees 65,536 B. Not SPIRAM_DMA_BUFFER, which moves the DMA frame itself
// to PSRAM and striped the picture (tried 2026-09-14, platformio.ini); here the
// DMA still reads internal RAM, and the colour depth is untouched (owner,
// 2026-09-21). Knowledge base: HANDOFF, 2026-09-23.
// On for the Waveshare board, where the shortage was measured; the other
// boards keep double buffering until someone measures them (gate audit).
#ifndef HUB75_FRAME_IN_PSRAM
#if defined(BOARD_WAVESHARE_RGB_MATRIX)
#define HUB75_FRAME_IN_PSRAM 1
#else
#define HUB75_FRAME_IN_PSRAM 0
#endif
#endif

inline HUB75_I2S_CFG makeMatrixConfig() {
  // i2s_pins field order is FIXED: r1,g1,b1,r2,g2,b2,a,b,c,d,e,lat,oe,clk
#if defined(BOARD_WAVESHARE_RGB_MATRIX)
  // Waveshare ESP32-S3-RGB-Matrix driver board (SKU 34422).
  // Verified against Waveshare's own sources: the ESP-IDF BSP
  // (components/bsp/esp32_s3_matrix/include/bsp/config.h), sdkconfig.defaults,
  // and the Arduino examples' platforms/esp32s3/esp32s3-default-pins.hpp.
  // The board is laid out on this library's default ESP32-S3 pin map; the only
  // deviation is E, unassigned upstream and routed to GPIO9 by Waveshare.
  HUB75_I2S_CFG::i2s_pins pins = {
      4, 5, 6,             // R1, G1, B1
      7, 15, 16,           // R2, G2, B2
      18, 8, 3, 42, 9,     // A, B, C, D, E
      40, 2, 41};          // LAT, OE, CLK
#else
  HUB75_I2S_CFG::i2s_pins pins = {
      1, 2, 4,             // R1, G1, B1
      5, 6, 7,             // R2, G2, B2
      8, 9, 10, 11, 12,    // A, B, C, D, E
      14, 38, 13};         // LAT, OE, CLK
#endif
  HUB75_I2S_CFG cfg(HUB75_PANEL_W, HUB75_PANEL_H, HUB75_CHAIN, pins);
  cfg.driver = HUB75_I2S_CFG::FM6126A;  // verified Phase 1
  cfg.clkphase = false;                 // verified: fixes dropped rightmost column
  // One DMA frame, not two: the frame a page draws is in PSRAM (see
  // MatrixDisplay below), so the second DMA frame bought nothing but the
  // tear-free flip - and cost 65,536 B of the internal RAM the Wi-Fi driver's
  // receive buffers come from. HUB75_FRAME_IN_PSRAM 0 puts it back.
  cfg.double_buff = !HUB75_FRAME_IN_PSRAM;
  return cfg;
}

// Adds the non-GFX frame methods the animation code uses (clearDisplay /
// display) on top of the GFX-derived matrix panel.
//
// With HUB75_FRAME_IN_PSRAM every drawing call fills a 128 x 64 x 3 frame in
// PSRAM instead of the DMA buffer, and display() puts on the panel only the
// pixels that differ from what it put there last time. Pages keep the model
// they had with double buffering - clear, draw, display() - and nothing they
// draw is seen before display(). What is caught, the same set src/fx3d
// catches (audited 2026-09-18): the library's virtual drawing calls, which
// Adafruit GFX's text, lines and shapes all end in, and its non-virtual ones
// hidden by name - `display` is always this type or one derived from it, and
// no code in src/ holds a base pointer. Colours are kept as the RGB888 the
// library would compute (MatrixPanel_I2S_DMA::color565to888), so what reaches
// the DMA buffer is exactly what the direct calls wrote.
class MatrixDisplay : public MatrixPanel_I2S_DMA {
public:
  explicit MatrixDisplay(const HUB75_I2S_CFG &cfg) : MatrixPanel_I2S_DMA(cfg) {}

#if HUB75_FRAME_IN_PSRAM
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (!ready()) return MatrixPanel_I2S_DMA::drawPixel(x, y, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    put(x, y, r, g, b);
  }
  void fillScreen(uint16_t c) override {
    if (!ready()) return MatrixPanel_I2S_DMA::fillScreen(c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(0, 0, kW, kH, r, g, b);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    if (!ready()) return MatrixPanel_I2S_DMA::fillRect(x, y, w, h, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, w, h, r, g, b);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) override {
    if (!ready()) return MatrixPanel_I2S_DMA::fillRect(x, y, w, h, r, g, b);
    rect(x, y, w, h, r, g, b);
  }
  // A line shorter than one is one pixel, as the library draws it (3.0.14,
  // the .h, lines 517-567: a length below 2 falls back to one pixel).
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    if (!ready()) return MatrixPanel_I2S_DMA::drawFastVLine(x, y, h, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, 1, h < 1 ? 1 : h, r, g, b);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint8_t r, uint8_t g, uint8_t b) override {
    if (!ready()) return MatrixPanel_I2S_DMA::drawFastVLine(x, y, h, r, g, b);
    rect(x, y, 1, h < 1 ? 1 : h, r, g, b);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    if (!ready()) return MatrixPanel_I2S_DMA::drawFastHLine(x, y, w, c);
    uint8_t r, g, b;
    color565to888(c, r, g, b);
    rect(x, y, w < 1 ? 1 : w, 1, r, g, b);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint8_t r, uint8_t g, uint8_t b) override {
    if (!ready()) return MatrixPanel_I2S_DMA::drawFastHLine(x, y, w, r, g, b);
    rect(x, y, w < 1 ? 1 : w, 1, r, g, b);
  }
  // The library's non-virtual ones, hidden by name.
  void drawPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (!ready()) return MatrixPanel_I2S_DMA::drawPixelRGB888(x, y, r, g, b);
    put(x, y, r, g, b);
  }
  void fillScreenRGB888(uint8_t r, uint8_t g, uint8_t b) {
    if (!ready()) return MatrixPanel_I2S_DMA::fillScreenRGB888(r, g, b);
    rect(0, 0, kW, kH, r, g, b);
  }
  void clearScreen() {
    if (!ready()) return MatrixPanel_I2S_DMA::clearScreen();
    memset(frame_, 0, kBytes);
  }

  inline void clearDisplay() { clearScreen(); }
  // The flip: the pixels that changed since the last one go to the DMA frame.
  //
  // One DMA frame is scanned while it is written, so a copy that overlaps a
  // scan pass can show that pass half old, half new - which double buffering
  // never did: its flip re-points the last descriptor of the chain
  // (gdma_lcd_parallel16.cpp, flip_dma_output_buffer), so the new frame starts
  // at a pass boundary. To keep that property the copy is timed to the scan:
  // it waits until the DMA is on the last row pair, then rewrites the row
  // pairs 0..31 in the order the scan will read them. As long as each row
  // pair is written before the scan reaches it, the next pass is entirely new
  // and no pass is mixed. The scan probe below measures, on the panel, whether
  // that held: every changed row pair's write is logged against the scan's
  // position, and a pass that showed some changed rows new and others old - or
  // a row rewritten while it was being output - is counted as mixed.
  //
  // Rows y and y + 32 are one DMA row (the upper and lower halves are clocked
  // out together), so they are copied together.
  inline void display() {
    if (ready()) {
      const bool probe = scanReady();
      uint32_t waited = 0;
      if (probe && !full_ && syncCopy) waited = waitForLastRow();
      const uint32_t t0 = micros();
      uint32_t n = 0;
      // For each changed row pair: the pass (counted from the copy's start)
      // it was written in, and where the scan was then: 1 ahead of it (the
      // row shows new in that pass), -1 behind it (old in that pass, new from
      // the next), 0 on it (rewritten while being output).
      int8_t wpass[kRowPairs], side[kRowPairs];
      bool touched[kRowPairs];
      int pass = 0, lastScan = probe ? scanRow() : -1;
      for (int r = 0; r < kRowPairs; r++) {
        const uint32_t changed = copyRow(r) + copyRow(r + kRowPairs);
        n += changed;
        touched[r] = probe && changed;
        if (!touched[r]) continue;
        const int sc = scanRow();
        if (sc < lastScan) pass++;              // the scan wrapped: a new pass
        lastScan = sc;
        wpass[r] = (int8_t)(pass > 100 ? 100 : pass);
        side[r] = sc < r ? 1 : (sc > r ? -1 : 0);
      }
      if (probe && n) {
        // Pass q showed row r new if r was written in an earlier pass, or in q
        // ahead of the scan; old if written later, or in q behind the scan.
        // A pass with both, or with a row rewritten under the scan, was mixed.
        bool mixed = false;
        uint32_t inflight = 0;
        for (int q = 0; q <= pass && q <= 100; q++) {
          bool shownNew = false, shownOld = false;
          for (int r = 0; r < kRowPairs; r++) {
            if (!touched[r]) continue;
            if (wpass[r] < q || (wpass[r] == q && side[r] > 0)) shownNew = true;
            else if (wpass[r] > q || side[r] < 0) shownOld = true;
            else { mixed = true; if (q == wpass[r]) inflight++; }
          }
          if (shownNew && shownOld) mixed = true;
        }
        changedFrames++;
        if (mixed) mixedFrames++;
        inflightRows += inflight;
      }
      if (probe) {
        syncWaitUs = waited;
        if (!full_ && waited > syncWaitMaxUs) syncWaitMaxUs = waited;
      }
      blitUs = micros() - t0;
      // The first copy after boot writes every pixel; it is not a frame in
      // service, so it is kept out of the worst case (gate audit).
      if (!full_ && blitUs > blitMaxUs) blitMaxUs = blitUs;
      full_ = false;
      blitPixels = n;
    }
    lastFlipUs = micros();
    hasFlipped = true;
  }
  // The scan probe's counts, since boot: frames that changed anything, and of
  // those the ones that showed a mixed pass; rows rewritten while output; and
  // how long the copy waited for the scan.
  uint32_t changedFrames = 0, mixedFrames = 0, inflightRows = 0;
  uint32_t syncWaitUs = 0, syncWaitMaxUs = 0;
  // Off only to prove the probe can see a mixed pass (the negative control):
  // GET /api/frame?sync=0, then back with sync=1. Counts reset on a change.
  bool syncCopy = true;
  void resetFrameStats() { changedFrames = mixedFrames = inflightRows = 0; syncWaitMaxUs = blitMaxUs = 0; }
  bool scanProbeOk() { return scanReady(); }
  int scanDescPerRow() const { return descPerRow_; }
  // Frame stats for /api/info: the last copy's time and pixel count, the worst.
  uint32_t blitUs = 0, blitMaxUs = 0, blitPixels = 0;
  bool frameInPsram() const { return frame_ != nullptr; }
#else
  inline void clearDisplay() { clearScreen(); }      // clear the (back) draw buffer
  inline void display() {
    flipDMABuffer();
    lastFlipUs = micros();
    hasFlipped = true;
  }
  bool frameInPsram() const { return false; }
#endif

  // The S3 driver queues a DMA chain switch but does not wait for EOF.
  // A full scan after the request guarantees the old front buffer is free.
  inline void waitForScanCompletion() {
#if HUB75_FRAME_IN_PSRAM
    return;   // one DMA frame, no flip: nothing to wait for
#endif
    if (!hasFlipped || calculated_refresh_rate <= 0) return;
    const uint32_t scanUs = (1000000UL + calculated_refresh_rate - 1) /
                            calculated_refresh_rate + 100;
    const uint32_t elapsed = micros() - lastFlipUs;
    if (elapsed < scanUs) delayMicroseconds(scanUs - elapsed);
  }

  // No readable framebuffer on the DMA panel; nothing samples it anymore
  // (Pong's digit shatter reads the 5x7 glyph font instead), kept only so any
  // future caller fails safe on a null check rather than a build error.
  inline uint8_t *getBuffer() { return nullptr; }

  // The scan rate the driver settled on at begin(), for diagnostics.
  inline int refreshRateHz() const { return calculated_refresh_rate; }
private:
  uint32_t lastFlipUs = 0;
  bool hasFlipped = false;
#if HUB75_FRAME_IN_PSRAM
  static const int kW = HUB75_PANEL_W * HUB75_CHAIN, kH = HUB75_PANEL_H;
  static const size_t kBytes = (size_t)kW * kH * 3;
  uint8_t *frame_ = nullptr;   // what the pages draw
  uint8_t *shown_ = nullptr;   // what display() last put on the panel
  bool full_ = true;           // the first copy writes every pixel
  bool tried_ = false;
  static const int kRowPairs = kH / 2;

  // One row of the drawn frame onto the DMA frame, as runs of one colour
  // through the library's drawFastHLine (hlineDMA). Returns the pixels written.
  uint32_t copyRow(int y) {
    uint8_t *f = frame_ + 3 * y * kW, *sh = shown_ + 3 * y * kW;
    uint32_t n = 0;
    for (int x = 0; x < kW;) {
      uint8_t *c = f + 3 * x;
      if (!full_ && c[0] == sh[3 * x] && c[1] == sh[3 * x + 1] && c[2] == sh[3 * x + 2]) { x++; continue; }
      int e = x + 1;
      while (e < kW && f[3 * e] == c[0] && f[3 * e + 1] == c[1] && f[3 * e + 2] == c[2]) e++;
      MatrixPanel_I2S_DMA::drawFastHLine(x, y, e - x, c[0], c[1], c[2]);
      memcpy(sh + 3 * x, f + 3 * x, (size_t)3 * (e - x));
      n += e - x;
      x = e;
    }
    return n;
  }

  // ── the scan probe ──────────────────────────────────────────────────────
  // The GDMA out channel feeding LCD_CAM (peri_sel 5, SOC_GDMA_TRIG_PERIPH_LCD0)
  // holds the address of the descriptor it is on (out.dscr, gdma_struct.h).
  // The driver lays its descriptors out as one array, row pair by row pair,
  // the same number per row pair (ESP32-HUB75-MatrixPanel-I2S-DMA.cpp, the
  // loop over ROWS_PER_FRAME), and closes the ring with suc_eof on the last.
  // So the row pair being output is (descriptor index) / (descriptors per row).
  struct Desc { uint32_t dw0; void *buffer; Desc *next; };   // dma_descriptor_t's layout
  int dmaCh_ = -1;
  const Desc *first_ = nullptr;
  int descCount_ = 0, descPerRow_ = 0;
  bool probeTried_ = false;
  bool scanReady() {
    if (first_) return true;
    if (probeTried_) return false;
    probeTried_ = true;
    for (int ch = 0; ch < 5; ch++)
      if (GDMA.channel[ch].out.peri_sel.sel == 5) dmaCh_ = ch;
    if (dmaCh_ < 0) return false;
    const Desc *d = (const Desc *)GDMA.channel[dmaCh_].out.dscr;
    for (int i = 0; d && i < 8192; i++, d = d->next)            // find the ring's end
      if (d->dw0 & (1u << 30)) { first_ = d->next; break; }      // suc_eof
    if (!first_) return false;
    int count = 0;
    for (const Desc *e = first_; count < 8192; count++) {
      if (e != first_ + count) { first_ = nullptr; return false; }   // not one array
      e = e->next;
      if (e == first_) { count++; break; }
    }
    if (count % kRowPairs) { first_ = nullptr; return false; }
    descCount_ = count;
    descPerRow_ = count / kRowPairs;
    return true;
  }
  int scanRow() const {
    const Desc *d = (const Desc *)GDMA.channel[dmaCh_].out.dscr;
    const int i = (int)(d - first_);
    return (i >= 0 && i < descCount_) ? i / descPerRow_ : -1;
  }
  // Waits for the scan to be on the last row pair, at most one pass: from
  // there the copy has the whole next pass ahead of it.
  uint32_t waitForLastRow() {
    const uint32_t t0 = micros();
    const uint32_t limit = calculated_refresh_rate > 0 ? 1000000UL / calculated_refresh_rate + 200 : 20000;
    while (scanRow() != kRowPairs - 1 && micros() - t0 < limit) {}
    return micros() - t0;
  }
  // PSRAM is allocated on first use, not in the constructor: `display` is a
  // global. If it cannot be had the calls go straight to the DMA frame, which
  // is only ever a flickery picture, never a missing one.
  bool ready() {
    if (frame_) return true;
    if (tried_) return false;
    tried_ = true;
    frame_ = (uint8_t *)heap_caps_calloc(1, kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    shown_ = (uint8_t *)heap_caps_calloc(1, kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_ || !shown_) {
      heap_caps_free(frame_); heap_caps_free(shown_);
      frame_ = shown_ = nullptr;
      Serial.println("[display] no PSRAM for the drawn frame: drawing straight to the single DMA frame");
      return false;
    }
    return true;
  }
  void put(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if ((uint16_t)x >= (uint16_t)kW || (uint16_t)y >= (uint16_t)kH) return;
    uint8_t *p = frame_ + 3 * (y * kW + x);
    p[0] = r; p[1] = g; p[2] = b;
  }
  // A side below one is nothing: the library's fillRectDMA would wrap and
  // paint a band (see src/fx3d/fx3d_display.h), which is a fault, not a look.
  void rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) {
    if (w < 1 || h < 1) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kW) x1 = kW;
    if (y1 > kH) y1 = kH;
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) {
        uint8_t *q = frame_ + 3 * (yy * kW + xx);
        q[0] = r; q[1] = g; q[2] = b;
      }
  }
#endif
};

#endif  // MATRIX_DISPLAY_H

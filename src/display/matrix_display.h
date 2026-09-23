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
  // at a pass boundary. To keep that property the copy is timed to the scan,
  // one of two ways (syncMode, switchable at run time so both are measured on
  // the same firmware and page):
  //
  //   kSyncFollow  a changed row pair is written only once the scan has
  //                passed it in this pass, so this pass shows every changed
  //                row old and the next shows them all new. The scan wrapping
  //                ends the waiting: rows written after it are ahead of the
  //                scan and new in that pass, like the rest. Slack: nearly a
  //                whole pass before the scan can come round and catch it.
  //   kSyncAhead   wait for the scan to be on the last row pair, then write
  //                every changed row pair ahead of it. Slack: what is left of
  //                the last row pair's time, a fraction of a millisecond.
  //
  // The scan probe measures, on the panel, whether a pass stayed whole: the
  // scan's position is unwrapped into a count of rows since the copy began,
  // every changed row pair's write is logged between the count before and
  // after it, and pass k showed row r new if k*32 + r came after the write,
  // old if before it, and was output during the write otherwise. A pass with
  // changed rows both new and old, or one written while output, is mixed.
  //
  // Rows y and y + 32 are one DMA row (the upper and lower halves are clocked
  // out together), so they are copied together.
  inline void display() {
    if (ready()) {
      const bool probe = scanReady();
      const uint8_t mode = (probe && !full_) ? syncMode : kSyncOff;
      const uint32_t t0 = micros();
      const uint32_t limit = calculated_refresh_rate > 0 ? 1000000UL / calculated_refresh_rate + 200 : 20000;
      uint32_t n = 0, waited = 0;
      // Scan rows since the copy began: the pass count times 32 plus the row.
      // Read often enough (well under a pass apart) the unwrap is exact.
      int pass = 0, last = probe ? scanRow() : 0;
      if (last < 0) last = 0;
      // The longest gap between two reads: over a pass, the unwrap may have
      // missed one, and the frame's verdict is not to be trusted.
      uint32_t readAt = micros(), gapMax = 0;
      auto scanAbs = [&]() -> int {
        const uint32_t now = micros();
        if (now - readAt > gapMax) gapMax = now - readAt;
        readAt = now;
        int sc = scanRow();
        if (sc < 0) sc = last;
        if (sc < last) pass++;
        last = sc;
        return pass * kRowPairs + sc;
      };
      const int startRow = last;
      if (mode == kSyncAhead) {
        const uint32_t w0 = micros();
        while (scanAbs() % kRowPairs != kRowPairs - 1 && micros() - w0 < limit) {}
        waited = micros() - w0;
      }
      // Follow: rows are written behind the scan in the base pass. A row the
      // scan has passed is only safe while the scan is far from coming round
      // to it again; on the last few row pairs it is about to reach row 0, so
      // the copy starts from the next pass instead (at most kFollowGuard rows,
      // about a millisecond). The probe found every remaining mixed frame to
      // be exactly that: row 0 written with the scan on row 31.
      int base = 0;
      if (mode == kSyncFollow && startRow >= kRowPairs - kFollowGuard) {
        const uint32_t w0 = micros();
        while (scanAbs() < kRowPairs && micros() - w0 < limit) {}
        waited = micros() - w0;
        base = kRowPairs;
      }
      int16_t a0[kRowPairs], a1[kRowPairs];
      bool touched[kRowPairs];
      int end = 0;
      for (int r = 0; r < kRowPairs; r++) {
        touched[r] = false;
        if (!full_ && !rowPairChanged(r)) continue;
        if (mode == kSyncFollow) {
          const uint32_t w0 = micros();
          while (scanAbs() <= base + r && micros() - t0 < 2 * limit) {}   // guard + one pass
          waited += micros() - w0;
        }
        const int s0 = probe ? scanAbs() : 0;
        const uint32_t changed = copyRow(r) + copyRow(r + kRowPairs);
        n += changed;
        if (!probe || !changed) continue;
        touched[r] = true;
        a0[r] = (int16_t)s0;
        a1[r] = (int16_t)(end = scanAbs());
      }
      if (probe && n) {
        bool mixed = false;
        uint32_t inflight = 0;
        int why = -1;
        for (int k = 0; k * kRowPairs <= end + kRowPairs; k++) {
          bool shownNew = false, shownOld = false;
          for (int r = 0; r < kRowPairs; r++) {
            if (!touched[r]) continue;
            const int at = k * kRowPairs + r;
            if (at > a1[r]) shownNew = true;
            else if (at < a0[r]) shownOld = true;
            else { mixed = true; inflight++; if (why < 0) why = r; }
          }
          if (shownNew && shownOld) mixed = true;
        }
        changedFrames++;
        if (mixed) {
          mixedFrames++;
          // What the last few mixed frames looked like, for /api/frame?detail.
          MixedNote &m = mixedLog[mixedLogAt++ % kMixedLog];
          int lo = -1, hi = -1;
          for (int r = 0; r < kRowPairs; r++)
            if (touched[r]) { if (lo < 0) lo = r; hi = r; }
          m.first = (int8_t)lo; m.last = (int8_t)hi; m.row = (int8_t)why;
          m.a0 = why >= 0 ? a0[why] : -1; m.a1 = why >= 0 ? a1[why] : -1;
          m.start = (int16_t)startRow; m.end = (int16_t)end;
          m.gapUs = gapMax; m.mode = mode;
        }
        if (gapMax * (uint32_t)(calculated_refresh_rate > 0 ? calculated_refresh_rate : 60) > 1000000UL) unsureFrames++;
        inflightRows += inflight;
      }
      blitUs = micros() - t0 - waited;
      if (probe && !full_) {
        syncWaitUs = waited;
        if (waited > syncWaitMaxUs) syncWaitMaxUs = waited;
        syncWaitSumUs += waited;
        blitSumUs += blitUs;
        flips++;
      }
      // The first copy after boot writes every pixel; it is not a frame in
      // service, so it is kept out of the worst case (gate audit).
      if (!full_ && blitUs > blitMaxUs) blitMaxUs = blitUs;
      full_ = false;
      blitPixels = n;
    }
    lastFlipUs = micros();
    hasFlipped = true;
  }
  // The scan probe's counts, since boot or the last reset: frames that changed
  // anything, and of those the ones that showed a mixed pass; rows rewritten
  // while output; how long the copy waited for the scan and took, summed over
  // all flips so the averages and the flip rate can be had.
  uint32_t changedFrames = 0, mixedFrames = 0, inflightRows = 0;
  uint32_t syncWaitUs = 0, syncWaitMaxUs = 0;
  uint32_t flips = 0, statsSinceMs = 0, unsureFrames = 0;
  struct MixedNote { int8_t first, last, row; uint8_t mode; int16_t a0, a1, start, end; uint32_t gapUs; };
  static const int kMixedLog = 8;
  MixedNote mixedLog[kMixedLog] = {};
  uint32_t mixedLogAt = 0;
  uint64_t syncWaitSumUs = 0, blitSumUs = 0;
  // How the copy is timed to the scan. kSyncOff only to prove the probe can
  // see a mixed pass (the negative control): GET /api/frame?sync=0, back with
  // sync=1 (follow) or sync=2 (ahead). Counts reset on a change.
  enum : uint8_t { kSyncOff = 0, kSyncFollow = 1, kSyncAhead = 2 };
  static const int kFollowGuard = 3;
  uint8_t syncMode = kSyncFollow;
  void resetFrameStats() {
    changedFrames = mixedFrames = inflightRows = flips = unsureFrames = mixedLogAt = 0;
    syncWaitMaxUs = blitMaxUs = 0;
    syncWaitSumUs = blitSumUs = 0;
    statsSinceMs = millis();
  }
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
  // Whether row pair r (rows r and r + 32) differs from what is on the panel.
  bool rowPairChanged(int r) const {
    const size_t row = (size_t)3 * kW;
    return memcmp(frame_ + r * row, shown_ + r * row, row) ||
           memcmp(frame_ + (r + kRowPairs) * row, shown_ + (r + kRowPairs) * row, row);
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

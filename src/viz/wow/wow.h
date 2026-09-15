/*
 * Visualizer styles 7-14, the eight effects the owner picked on 2026-09-15:
 * Prism EQ, Neon Mirror+, Spectrogram, Radial Bloom, Beat Particles,
 * Scope Afterglow, Twin VU, Synthwave Grid.
 *
 * Each is a line-by-line port of tools/audiofx/effects_wow.py and is held to it
 * pixel for pixel on the showreel (tools/audiofx/host/compare_wow.py). Keep
 * the two in step: same expressions, same order, same constants.
 *
 * Portable C++: no Arduino. The panel draws through a Canvas that forwards to
 * the HUB75 display; the host test draws into an array. real is float on the
 * panel; the host comparison builds it as double, as Python computes.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "viz_frame.h"

#ifndef WOW_REAL
#define WOW_REAL float
#endif

namespace wow {

using real = WOW_REAL;

constexpr int kW = 128, kH = 64;
constexpr int kEffects = 8;
constexpr int kFirstStyle = 7;          // settings.vizStyle of the first one
extern const char *const kNames[kEffects];

// gfx.py's primitives, clipped here. A backend supplies the in-bounds writes.
class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual void setPixel(int x, int y, uint16_t c) = 0;              // 0 <= x < kW, 0 <= y < kH
  virtual void spanH(int x, int y, int w, uint16_t c);              // in bounds, w > 0
  virtual void spanV(int x, int y, int h, uint16_t c);              // in bounds, h > 0
  virtual void glyph(int x, int y, unsigned char ch, uint16_t c) = 0;  // Adafruit GFX classic 5x7, no background

  void pixel(int x, int y, uint16_t c);
  void hline(int x, int y, int w, uint16_t c);
  void vline(int x, int y, int h, uint16_t c);
  void line(int x0, int y0, int x1, int y1, uint16_t c);       // Adafruit_GFX::writeLine
  void circle(int x0, int y0, int r, uint16_t c);              // Adafruit_GFX::drawCircle
  void fillCircle(int x0, int y0, int r, uint16_t c);          // spans of int(sqrt(r*r - dy*dy))
  void text(int x, int y, const char *s, uint16_t c);          // 6 px a character
};

struct State;

class Engine {
 public:
  using AllocFn = void *(*)(size_t bytes);

  // The state and every effect's buffers, about 90 KB, from alloc (PSRAM on
  // the panel). Nothing is allocated after this.
  bool begin(AllocFn alloc);
  bool ready() const { return s_ != nullptr; }

  // Fresh state for effect 0..7, as constructing the Python class. react is
  // settings.vizBeatFx / 100: how hard beats hit (1 = the previews).
  void reset(int effect, real react);
  void setReact(real react);   // a settings change, without resetting the effect
  int effect() const;

  // Every frame, in order: a missed one is a missed beat.
  void update(const VizFrame &f);

  // One display frame, dt seconds after the previous. Draws nothing before the first update.
  void render(Canvas &cv, real dt);

 private:
  State *s_ = nullptr;
};

}  // namespace wow

/*
 * Canvas primitives, exactly as tools/audiofx/gfx.py draws them (which follows
 * Adafruit GFX's writeLine and drawCircle). Drawn here rather than by the
 * library so the panel and the host test cannot disagree.
 */
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include <algorithm>
#include <cmath>

#include "wow.h"

namespace wow {

void Canvas::spanH(int x, int y, int w, uint16_t c) {
  for (int i = 0; i < w; i++) setPixel(x + i, y, c);
}

void Canvas::spanV(int x, int y, int h, uint16_t c) {
  for (int i = 0; i < h; i++) setPixel(x, y + i, c);
}

void Canvas::pixel(int x, int y, uint16_t c) {
  if (x >= 0 && x < kW && y >= 0 && y < kH) setPixel(x, y, c);
}

void Canvas::hline(int x, int y, int w, uint16_t c) {
  if (w <= 0 || y < 0 || y >= kH) return;
  const int x0 = std::max(0, x), x1 = std::min(kW, x + w);
  if (x1 > x0) spanH(x0, y, x1 - x0, c);
}

void Canvas::vline(int x, int y, int h, uint16_t c) {
  if (h <= 0 || x < 0 || x >= kW) return;
  const int y0 = std::max(0, y), y1 = std::min(kH, y + h);
  if (y1 > y0) spanV(x, y0, y1 - y0, c);
}

void Canvas::line(int x0, int y0, int x1, int y1, uint16_t c) {
  const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int dx = x1 - x0, dy = std::abs(y1 - y0);
  int err = dx / 2;   // dx >= 0: Python's // and C's / agree
  const int ystep = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++) {
    if (steep) pixel(y0, x0, c);
    else pixel(x0, y0, c);
    err -= dy;
    if (err < 0) {
      y0 += ystep;
      err += dx;
    }
  }
}

void Canvas::circle(int x0, int y0, int r, uint16_t c) {
  if (r <= 0) {
    pixel(x0, y0, c);
    return;
  }
  int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  pixel(x0, y0 + r, c);
  pixel(x0, y0 - r, c);
  pixel(x0 + r, y0, c);
  pixel(x0 - r, y0, c);
  while (x < y) {
    if (f >= 0) {
      y--;
      ddy += 2;
      f += ddy;
    }
    x++;
    ddx += 2;
    f += ddx;
    pixel(x0 + x, y0 + y, c);
    pixel(x0 - x, y0 + y, c);
    pixel(x0 + x, y0 - y, c);
    pixel(x0 - x, y0 - y, c);
    pixel(x0 + y, y0 + x, c);
    pixel(x0 - y, y0 + x, c);
    pixel(x0 + y, y0 - x, c);
    pixel(x0 - y, y0 - x, c);
  }
}

void Canvas::fillCircle(int x0, int y0, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    const int half = (int)std::sqrt((double)(r * r - dy * dy));
    hline(x0 - half, y0 + dy, 2 * half + 1, c);
  }
}

void Canvas::text(int x, int y, const char *s, uint16_t c) {
  for (; *s; s++, x += 6) glyph(x, y, (unsigned char)*s, c);
}

}  // namespace wow

#endif

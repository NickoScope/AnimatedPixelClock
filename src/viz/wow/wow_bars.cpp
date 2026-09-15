/*
 * Styles 7-10: Prism EQ, Neon Mirror+, Spectrogram, Radial Bloom.
 * PrismEQ, NeonMirrorPlus, Spectrogram and RadialBloom in tools/audiofx/effects_wow.py.
 */
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include <cstring>

#include "wow_internal.h"

namespace wow {

void renderPrismEq(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const int base = 59, top = 54;
  const real gain = R(0.80) + R(0.20) * s.beatEnv;
  if (s.beatEnv > R(0.05)) cv.hline(0, base + 1, kW, col(R(150), R(70), R(235), s.beatEnv));
  for (int i = 0; i < 32; i++) {
    const int h = (int)(R(f.level[i]) * top);
    const int x = i * 4;
    const real hue = R(0.83) - i / R(32.0) * R(0.62) + s.hue;
    for (int y = 0; y < h; y++) {
      const real t = y / R(top);                          // colour follows the absolute height,
      const real lit = R(0.45) + R(0.55) * (y + 1) / R(h);  // light the bar's own top
      const Rgb c = hsv(hue + t * R(0.10), R(0.95) - R(0.45) * t, lit * gain);
      cv.hline(x, base - y, 3, col(c));
      if (y < 4) cv.hline(x, base + 1 + y, 3, col(c, R(0.30) - R(0.07) * y));   // floor reflection
    }
    if (h > 0) cv.hline(x, base - h, 3, col(hsv(hue + R(0.05), R(0.30), gain)));
    const int pk = (int)(R(f.peak[i]) * top);
    if (pk > 1) cv.hline(x, base - pk - 1, 3, col(hsv(hue, R(0.20), R(0.95))));
  }
}

void renderNeonMirrorPlus(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const int horizon = 36, reach = 26;
  const real warm = std::min(R(1.0), s.beatEnv * R(0.8));
  const Rgb topA = mix({R(30), R(210), R(235)}, {R(235), R(190), R(60)}, warm);
  const Rgb topB = mix({R(235), R(40), R(200)}, {R(150), R(60), R(235)}, warm);
  for (int i = 0; i < 32; i++) {
    const int h = (int)(R(f.level[i]) * reach);
    const int pk = (int)(R(f.peak[i]) * reach);
    for (int side : {1, -1}) {
      const int x = side > 0 ? 64 + 2 * i : 63 - 2 * i;
      const int gx = x + side;
      for (int y = 1; y <= h; y++) {
        const Rgb c = mix(topA, topB, y / R(reach));
        cv.pixel(x, horizon - y, col(c));
        cv.pixel(gx, horizon - y, col(c, R(0.18)));
        cv.pixel(x, horizon + y, col(c, R(0.45)));
      }
      if (pk > 1) {
        cv.pixel(x, horizon - pk - 1, col(R(225), R(235), R(235)));
        cv.pixel(x, horizon + pk + 1, col(R(235), R(120), R(210), R(0.6)));
      }
    }
  }
  cv.hline(0, horizon, kW, col(R(90), R(30), R(140), R(0.45) + R(0.55) * s.beatEnv));
}

void updateSpectrogram(State &s) {
  const VizFrame &f = s.f;
  real fp[kBands];
  for (int b = 0; b < kBands; b++) fp[b] = f.bands[b] / R(255.0);   // raw bytes: crisp in time
  uint8_t column[kH];
  for (int r = 0; r < kH; r++) {   // linear between band centres 1, 3, ..., 63, flat beyond
    const real x = r + R(0.5);
    real v;
    if (x <= R(1.0)) {
      v = fp[0];
    } else if (x >= R(63.0)) {
      v = fp[31];
    } else {
      const int j = (int)((x - R(1.0)) / R(2.0));
      const real t = (x - (2 * j + 1)) / R(2.0);
      v = fp[j] + (fp[j + 1] - fp[j]) * t;
    }
    column[kH - 1 - r] = (uint8_t)(int)std::min(R(63.0), std::max(R(0.0), v * R(63.0)));   // bass at the bottom
  }
  const int steps = f.steps ? f.steps : 1;   // a PC packet stands for two DSP frames
  for (int k = 0; k < steps; k++) {
    std::memcpy(s.cols + s.head * kH, column, kH);   // the oldest column becomes the newest
    s.ticks[s.head] = (f.beat && k == steps - 1) ? 1 : 0;
    s.head = (s.head + 1) % kW;
  }
}

void renderSpectrogram(State &s, Canvas &cv, real dt) {
  step(s, dt);
  for (int x = 0; x < kW; x++) {
    const uint8_t *c = s.cols + ((s.head + x) % kW) * kH;
    for (int y = 0; y < kH; y++)
      if (c[y]) cv.setPixel(x, y, s.lut[c[y]]);
  }
  const uint16_t tick = col(R(235), R(200), R(90));
  for (int x = 0; x < kW; x++)
    if (s.ticks[(s.head + x) % kW]) cv.vline(x, 0, 3, tick);
}

void beatRadialBloom(State &s) {
  const VizFrame &f = s.f;
  if (s.nRings == kMaxRings) {   // keep the newest MAX_RINGS
    std::memmove(s.rings, s.rings + 1, (kMaxRings - 1) * sizeof(Ring));
    s.nRings--;
  }
  s.rings[s.nRings++] = {R(6.0) + R(f.bass) * R(6.0), R(0.5) + R(0.5) * R(f.strength)};
}

void renderRadialBloom(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  s.rot = pymod(s.rot + dt * (R(0.12) + R(f.treble) * R(0.5)), R(2) * kPi);
  const int cx = 64, cy = 33;
  const real r0 = R(5.0) + R(f.bass) * R(7.0);
  for (int i = 0; i < s.nRings; i++) {
    Ring &g = s.rings[i];
    g.radius += R(55.0) * dt;
    g.strength *= std::exp(-dt / R(0.35));
    cv.circle(cx, cy, (int)g.radius, col(R(200), R(90), R(235), g.strength));
  }
  int keep = 0;
  for (int i = 0; i < s.nRings; i++)
    if (s.rings[i].strength > R(0.04) && s.rings[i].radius < 80) s.rings[keep++] = s.rings[i];
  s.nRings = keep;
  cv.fillCircle(cx, cy, std::max(1, (int)r0 - 2), col(R(60), R(20), R(90), R(0.5) + R(0.5) * s.beatEnv));
  for (int j = 0; j < 64; j++) {
    const real lobe = pymod((j + R(0.5)) / R(16.0), R(2.0));   // four mirrored lobes: bass top and bottom
    const int b = std::min(31, (int)((lobe < R(1.0) ? lobe : R(2.0) - lobe) * 32));
    const real a = s.rot + R(2) * kPi * j / R(64.0) - kPi / 2;
    const real length = R(f.level[b]) * R(21.0);
    const real ca = std::cos(a), sa = std::sin(a);
    const Rgb c = b < 20 ? mix({R(150), R(60), R(235)}, {R(235), R(70), R(160)}, b / R(31.0))
                         : mix({R(235), R(70), R(160)}, {R(235), R(180), R(60)}, (b - 20) / R(11.0));
    const real k = R(0.55) + R(0.45) * R(f.level[b]);
    const real x0 = cx + ca * r0, y0 = cy + sa * r0;
    const real x1 = cx + ca * (r0 + length), y1 = cy + sa * (r0 + length);
    cv.line(pyround(x0), pyround(y0), pyround(x1), pyround(y1), col(c, k));
    const real pk = r0 + R(f.peak[b]) * R(21.0) + 1;
    cv.pixel(pyround(cx + ca * pk), pyround(cy + sa * pk), col(R(235), R(225), R(210), R(0.9)));
  }
}

}  // namespace wow

#endif

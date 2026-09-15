/*
 * Shared by the wow effect files: effects_wow.py's helpers, with Python's
 * semantics where C's differ (round, %, //), and the engine state.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "wow.h"

namespace wow {

#define R(x) (static_cast<real>(x))

constexpr int CAP = 235;
constexpr real kPi = R(3.141592653589793);   // math.pi
constexpr int kKeepParts = 180;              // BeatParticles.KEEP
constexpr int kMaxParts = kKeepParts + 58;   // plus one beat's worth before trimming
constexpr int kMaxRings = 16;                // RadialBloom.MAX_RINGS
constexpr int kStars = 50;

enum Effect { PRISM_EQ, NEON_MIRROR_PLUS, SPECTROGRAM, RADIAL_BLOOM, BEAT_PARTICLES, SCOPE_AFTERGLOW, TWIN_VU, SYNTHWAVE };

struct Rgb { real r, g, b; };

inline uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)((((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)) & 0xFFFF);
}
inline int capi(int v) { return v > CAP ? CAP : (v < 0 ? 0 : v); }   // min(CAP, max(0, v))
inline uint16_t col(real r, real g, real b, real k = R(1.0)) {
  return rgb565(capi((int)(r * k)), capi((int)(g * k)), capi((int)(b * k)));
}
inline uint16_t col(const Rgb &c, real k = R(1.0)) { return col(c.r, c.g, c.b, k); }

// Python's float %: the result takes the divisor's sign.
inline real pymod(real a, real b) {
  real m = std::fmod(a, b);
  if (m != 0) {
    if ((b < 0) != (m < 0)) m += b;
  } else {
    m = std::copysign(R(0.0), b);
  }
  return m;
}
// Python's round(): half to even, which nearbyint() does in the default rounding mode.
inline int pyround(real v) { return (int)std::nearbyint(v); }
// Python's int // int: toward minus infinity.
inline int pyfloordiv(int a, int b) {
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

inline Rgb hsv(real h, real s, real v) {
  h = pymod(h, R(1.0)) * R(6.0);
  const int i = (int)h;
  const real f = h - i;
  const real p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  Rgb o;
  switch (i % 6) {
    case 0: o = {v, t, p}; break;
    case 1: o = {q, v, p}; break;
    case 2: o = {p, v, t}; break;
    case 3: o = {p, q, v}; break;
    case 4: o = {t, p, v}; break;
    default: o = {v, p, q}; break;
  }
  return {o.r * CAP, o.g * CAP, o.b * CAP};
}

inline Rgb mix(const Rgb &a, const Rgb &b, real t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

inline Rgb inferno(real t) {
  static const real T[6] = {R(0.0), R(0.18), R(0.40), R(0.62), R(0.82), R(1.0)};
  static const Rgb C[6] = {{R(0), R(0), R(0)},       {R(40), R(8), R(70)},   {R(140), R(20), R(90)},
                           {R(220), R(60), R(30)},  {R(235), R(150), R(20)}, {R(235), R(225), R(150)}};
  t = std::min(R(1.0), std::max(R(0.0), t));
  for (int k = 0; k < 5; k++)
    if (t <= T[k + 1]) return mix(C[k], C[k + 1], (t - T[k]) / (T[k + 1] - T[k]));
  return C[5];
}

struct XorShift32 {
  uint32_t s = 2463534242u;   // effects_wow.SEED
  uint32_t next() {
    uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
  }
  real uniform(real a, real b) { return a + (b - a) * R(next() / 4294967296.0); }
};

struct Ring { real radius, strength; };
struct Particle { real x, y, vx, vy, life; uint8_t palette; };
struct Star { real x, y, speed; };

struct State {
  int effect = 0;
  real react = R(1.0);
  bool have = false;
  VizFrame f = {};
  real beatEnv = R(0.0), hue = R(0.0), hueTarget = R(0.0);
  XorShift32 rng;

  // Spectrogram: kW columns of kH colour indices in a ring, head = the oldest column.
  uint8_t *cols = nullptr;
  int head = 0;
  uint8_t ticks[kW] = {};
  uint16_t lut[64] = {};

  // Radial Bloom
  real rot = R(0.0);
  Ring rings[kMaxRings] = {};
  int nRings = 0;

  // Beat Particles
  float *rgb = nullptr;           // kH x kW x 3, float32 as RgbCanvas
  Particle *parts = nullptr;
  int nParts = 0;
  Star stars[kStars] = {};

  // Scope Afterglow
  float *glow = nullptr;          // kH x kW, float32

  // Twin VU
  real pos[2] = {}, vel[2] = {};
  real ghosts[2][6] = {};
  int nGhosts[2] = {};
  real norm[2] = {R(0.3), R(0.3)};

  // Synthwave Grid
  real offset = R(0.0);
};

void step(State &s, real dt);

void updateSpectrogram(State &s);
void beatRadialBloom(State &s);
void beatParticles(State &s);

void renderPrismEq(State &s, Canvas &cv, real dt);
void renderNeonMirrorPlus(State &s, Canvas &cv, real dt);
void renderSpectrogram(State &s, Canvas &cv, real dt);
void renderRadialBloom(State &s, Canvas &cv, real dt);
void renderBeatParticles(State &s, Canvas &cv, real dt);
void renderScopeAfterglow(State &s, Canvas &cv, real dt);
void renderTwinVu(State &s, Canvas &cv, real dt);
void renderSynthwave(State &s, Canvas &cv, real dt);

}  // namespace wow

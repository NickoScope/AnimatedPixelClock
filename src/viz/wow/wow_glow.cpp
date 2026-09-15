/*
 * Styles 11-14: Beat Particles, Scope Afterglow, Twin VU, Synthwave Grid.
 * BeatParticles, ScopeAfterglow, TwinVU and Synthwave in tools/audiofx/effects_wow.py.
 * The particle canvas and the afterglow are bytes with integer maths, as there.
 */
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include <cstring>

#include "wow_internal.h"

namespace wow {
namespace {

constexpr double kDegToRad = 3.141592653589793 / 180.0;   // math.radians()
const Rgb kPalettes[3] = {{R(235), R(50), R(130)}, {R(40), R(210), R(235)}, {R(235), R(190), R(70)}};

// RgbCanvas.add()
inline void addLight(uint8_t *rgb, real x, real y, real r, real g, real b) {
  const int ix = (int)x, iy = (int)y;
  if (ix < 0 || ix >= kW || iy < 0 || iy >= kH) return;
  uint8_t *p = rgb + (iy * kW + ix) * 3;
  p[0] = (uint8_t)std::min(235, p[0] + (int)r);
  p[1] = (uint8_t)std::min(235, p[1] + (int)g);
  p[2] = (uint8_t)std::min(235, p[2] + (int)b);
}

}  // namespace

void beatParticles(State &s) {
  const VizFrame &f = s.f;
  const int n = 18 + (int)(40 * R(f.strength));
  const real groups[3] = {R(f.bass), R(f.mid), R(f.treble)};
  int palette = 0;   // groups.index(max(groups)): the first of the largest
  for (int i = 1; i < 3; i++)
    if (groups[i] > groups[palette]) palette = i;
  for (int i = 0; i < n && s.nParts < kMaxParts; i++) {
    const real a = -kPi / 2 + s.rng.uniform(R(-1.05), R(1.05));
    const real v = s.rng.uniform(R(45), R(95)) * (R(0.6) + R(f.strength));
    s.parts[s.nParts++] = {R(64.0), R(62.0), std::cos(a) * v, std::sin(a) * v, R(1.0), (uint8_t)palette};
  }
  if (s.nParts > kKeepParts) {   // parts[-KEEP:]
    const int drop = s.nParts - kKeepParts;
    std::memmove(s.parts, s.parts + drop, kKeepParts * sizeof(Particle));
    s.nParts = kKeepParts;
  }
}

void renderBeatParticles(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const size_t cells = (size_t)kW * kH * 3;
  for (size_t i = 0; i < cells; i++) s.rgb[i] = (uint8_t)(s.rgb[i] * 4 / 5);   // fade(4, 5)
  for (Star &st : s.stars) {
    st.y += dt * (4 + 20 * R(f.bass)) * st.speed;
    if (st.y >= kH) {
      st.x = s.rng.uniform(R(0), R(kW));
      st.y = R(0.0);
    }
    addLight(s.rgb, st.x, st.y, 30 * st.speed, 30 * st.speed, 50 * st.speed);
  }
  for (int i = 0; i < s.nParts; i++) {
    Particle &p = s.parts[i];
    p.vy += R(60.0) * dt;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.life *= std::exp(-dt / R(0.9));
    const Rgb &c = kPalettes[p.palette];
    addLight(s.rgb, p.x, p.y, c.r * p.life, c.g * p.life, c.b * p.life);
  }
  int keep = 0;
  for (int i = 0; i < s.nParts; i++) {
    const Particle &p = s.parts[i];
    if (p.life > R(0.05) && -4 < p.x && p.x < kW + 4 && p.y < kH + 2) s.parts[keep++] = p;
  }
  s.nParts = keep;
  const int width = (int)(R(f.bass) * 60);
  for (int x = 64 - width; x < 64 + width; x++) addLight(s.rgb, R(x), R(63), R(120), R(30), R(90));
  for (int y = 0; y < kH; y++) {   // blit()
    for (int x = 0; x < kW; x++) {
      const uint8_t *p = s.rgb + (y * kW + x) * 3;
      cv.setPixel(x, y, (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3)));
    }
  }
}

void renderScopeAfterglow(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const int keep = (int)(std::exp(-dt / R(0.09)) * R(256.0));   // fixed point: glow * keep >> 8
  for (size_t i = 0; i < (size_t)kW * kH; i++) s.glow[i] = (uint8_t)((s.glow[i] * keep) >> 8);
  const int cy = 36, half = 26;
  bool havePrev = false;
  int y0 = 0;
  for (int i = 0; i < 128; i++) {
    const real d = ((int)f.wave[i] - 128) / R(128.0);
    const int y = pyround(cy - std::max(R(-1.0), std::min(R(1.0), d)) * half);
    if (havePrev) {
      const int steps = std::max(1, std::abs(y - y0));
      for (int k = 0; k <= steps; k++) {
        const int yy = y0 + pyfloordiv((y - y0) * k, steps);
        if (yy < 0 || yy >= kH) continue;
        s.glow[yy * kW + i] = 255;
        if (s.beatEnv > R(0.2)) {
          const int edge = (int)(R(0.5) * s.beatEnv * R(255.0));
          for (int dy : {-1, 1}) {
            if (yy + dy < 0 || yy + dy >= kH) continue;
            uint8_t &g = s.glow[(yy + dy) * kW + i];
            if (edge > g) g = (uint8_t)edge;
          }
        }
      }
    }
    havePrev = true;
    y0 = y;
  }
  const uint16_t grid = col(R(20), R(70), R(40), R(0.5) + R(0.5) * s.beatEnv);
  for (int x = 0; x < kW; x += 16)
    for (int y = 12; y < kH; y += 3) cv.pixel(x, y, grid);
  for (int x = 0; x < kW; x += 3) cv.pixel(x, cy, grid);
  const Rgb lo = {R(40), R(235), R(120)}, hi = {R(235), R(190), R(60)}, core = {R(190), R(235), R(200)};
  for (int y = 0; y < kH; y++) {
    for (int x = 0; x < kW; x++) {
      const uint8_t gb = s.glow[y * kW + x];
      if (gb <= 10) continue;   // 0.04 of full
      const real g = gb / R(255.0);
      const real dev = std::abs(y - cy) / R(half);
      const Rgb base = mix(lo, hi, dev);
      const Rgb c = g > R(0.7) ? mix(base, core, std::max(R(0.0), g - R(0.7)) / R(0.3)) : base;
      cv.pixel(x, y, col(c, g));
    }
  }
}

void renderTwinVu(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const real raw[2] = {R(f.bass), R(f.mid) * R(0.6) + R(f.treble) * R(0.4)};
  real targets[2];
  for (int m = 0; m < 2; m++) {   // each meter ranges itself over the last few seconds
    real v = s.norm[m] * std::exp(-dt / R(4.0));   // max(a, b, c): the first of the largest
    if (raw[m] > v) v = raw[m];
    if (R(0.15) > v) v = R(0.15);
    s.norm[m] = v;
    targets[m] = raw[m] / s.norm[m] * R(0.92);
  }
  for (int m = 0; m < 2; m++) {
    const real acc = R(140.0) * (std::min(R(1.05), targets[m]) - s.pos[m]) - R(16.0) * s.vel[m];
    s.vel[m] += acc * dt;
    s.pos[m] = std::min(R(1.08), std::max(R(-0.02), s.pos[m] + s.vel[m] * dt));
    const int n = std::min(6, s.nGhosts[m] + 1);   // ([pos] + ghosts)[:6]
    for (int g = n - 1; g > 0; g--) s.ghosts[m][g] = s.ghosts[m][g - 1];
    s.ghosts[m][0] = s.pos[m];
    s.nGhosts[m] = n;
  }
  const uint16_t amber = col(R(150), R(100), R(30));
  for (int m = 0; m < 2; m++) {
    const int ox = m == 0 ? 0 : 64;
    const int px = ox + 32, py = 61, rad = 44;
    for (int k = 0; k < 21; k++) {
      const real a = R(-50 + k * 5) * R(kDegToRad);
      const bool red = k >= 16;
      const real x = px + std::sin(a) * rad, y = py - std::cos(a) * rad;
      cv.pixel(pyround(x), pyround(y), red ? col(R(200), R(40), R(30)) : amber);
      if (k % 5 == 0) {
        const real x2 = px + std::sin(a) * (rad - 3), y2 = py - std::cos(a) * (rad - 3);
        cv.line(pyround(x), pyround(y), pyround(x2), pyround(y2), amber);
      }
    }
    for (int g = s.nGhosts[m] - 1; g >= 0; g--) {
      const real a = (-50 + 100 * s.ghosts[m][g]) * R(kDegToRad);
      const real k = g == 0 ? R(1.0) : R(0.35) * (1 - g / R(6.0));
      cv.line(px, py, pyround(px + std::sin(a) * (rad - 4)), pyround(py - std::cos(a) * (rad - 4)),
              col(R(235), R(225), R(200), k));
    }
    cv.fillCircle(px, py, 3, col(R(90), R(60), R(30), R(0.6) + R(0.4) * s.beatEnv));
    cv.text(ox + 3, 54, m == 0 ? "LO" : "HI", amber);
    const bool hot = s.pos[m] > R(0.95) || f.clipping;
    cv.fillCircle(ox + 58, 58, 2, hot ? col(R(235), R(40), R(30)) : col(R(50), R(12), R(8)));
  }
}

void renderSynthwave(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  step(s, dt);
  const int horizon = 30;
  const real energy = (R(f.bass) + R(f.mid) + R(f.treble)) / R(3.0);
  s.offset = pymod(s.offset + dt * (R(0.7) + energy * R(2.0) + s.beatEnv * R(2.5)), R(1.0));
  for (int y = 0; y < horizon; y++) cv.hline(0, y, kW, col(R(18), R(4), R(30), y / R(horizon)));
  const int sr = 13 + (int)(R(f.bass) * 5);
  for (int dy = -sr; dy <= 0; dy++) {
    const int half = (int)std::sqrt((double)std::max(0, sr * sr - dy * dy));
    const int y = horizon + dy;
    if (dy > -sr * R(0.55) && (dy % 3 == 0)) continue;   // the sun's stripes
    const Rgb c = mix({R(235), R(200), R(60)}, {R(235), R(50), R(140)}, (dy + sr) / R(sr));
    cv.hline(64 - half, y, 2 * half + 1, col(c, R(0.85) + R(0.15) * s.beatEnv));
  }
  int band[kW], ridge[kW];
  for (int x = 0; x < kW; x++) {
    band[x] = 31 - std::min(31, (int)(std::abs(x - R(63.5)) * 32 / R(64.0)));
    ridge[x] = horizon - (int)(R(f.level[band[x]]) * 20);
  }
  const uint16_t mountain = col(R(22), R(6), R(38));
  for (int x = 0; x < kW; x++) {
    cv.vline(x, ridge[x] + 1, horizon - ridge[x], mountain);   // the mountains stand in front of the sun
    cv.pixel(x, ridge[x], col(R(40), R(220), R(235), R(0.6) + R(0.4) * R(f.level[band[x]])));
  }
  const real gridK = R(0.45) + R(0.55) * s.beatEnv;
  const real frac = pymod(s.offset, R(1.0));
  for (int k = 1; k < 14; k++) {
    const real z = k - frac;
    if (z <= R(0.15)) continue;
    const int y = horizon + pyround(R(34.0) / z);
    if (horizon < y && y < kH) cv.hline(0, y, kW, col(R(200), R(40), R(180), gridK * std::min(R(1.0), R(3.0) / z)));
  }
  const uint16_t rays = col(R(160), R(30), R(150), gridK);
  for (int j = -8; j <= 8; j++) cv.line(64 + j * 3, horizon + 1, 64 + j * 26, kH - 1, rays);
  cv.hline(0, horizon, kW, col(R(235), R(60), R(180), R(0.7)));
}

}  // namespace wow

#endif

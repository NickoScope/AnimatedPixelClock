#pragma once
// 3D as a way of showing ANY screen, the owner's word of 2026-09-18 15:22:
// the page draws its ordinary 2D frame, nothing about it changes, and a "look"
// chosen for the display shows that frame in 3D.
//
// Two families. With the glasses, each pixel of the picture is given a
// disparity from the picture itself and the two eyes' views are made by moving
// pixels sideways (depth-image-based rendering): POP, LAYERS, FLOAT, DOME.
// Without them, the picture becomes a thing in space: a card, raised blocks,
// a drum, or the POP depth seen from a gently swaying eye (WIGGLE). The things
// are real geometry, so they work with the glasses too.
//
// The input is what the page drew: codes, as drawPixelRGB888() would take them.
// Plain C++11, float only, no allocation.

#include "fx3d_scenes_raster.h"

namespace fx3d {

enum Look : uint8_t {
  LOOK_FLAT = 0,   // as it is
  LOOK_POP,        // glasses: brighter is nearer; black stays on the panel's plane
  LOOK_LAYERS,     // glasses: warm colours in front, white in the middle, cold behind
  LOOK_FLOAT,      // glasses: the whole picture one plane in front of the panel
  LOOK_DOME,       // glasses: the middle bulges out towards the viewer
  LOOK_WIGGLE,     // no glasses: the POP depth, seen from an eye swaying sideways
  LOOK_CARD,       // no glasses: the picture on a card swaying in space
  LOOK_RELIEF,     // no glasses: lit pixels raised as blocks, their sides in shade
  LOOK_DRUM,       // no glasses: wrapped round a drum that turns to and fro
  LOOK_COUNT
};
inline const char *lookName(uint8_t l) {
  static const char *const n[LOOK_COUNT] = {"flat", "pop", "layers", "float", "dome", "wiggle", "card", "relief", "drum"};
  return l < LOOK_COUNT ? n[l] : "?";
}

// The page's frame shown with a look. A Scene, so a frame is made exactly as
// for the scenes: mono, or once per eye and composed.
class PictureScene : public Scene {
 public:
  const uint8_t *codes;   // kPixels * 3, what the page drew; set before each frame
  uint8_t look;
  PictureScene()
      : codes(nullptr), look(LOOK_FLAT), yawPh_(0.0f), pitchPh_(0.0f), wigglePh_(0.0f), drumPh_(0.0f), reliefPh_(0.0f) {
    for (int k = 0; k < 3; k++) drum_[k].valid = false;
  }
  const char *id() const { return "picture"; }
  void setup(View &v) const {
    v.f = 120.0f;
    v.z0 = kZ0;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    // Geometry looks declare where they are; the pixel-moving ones spend
    // Stereo::depthPx directly and need no baseline.
    if (look == LOOK_CARD) {
      // A corner goes furthest: both swings at once.
      const float reach = kCardHalfW * sinf(kCardYaw) + kCardHalfH * sinf(kCardPitch) + 0.05f;
      zn = kZ0 - reach;
      zf = kZ0 + reach;
    } else if (look == LOOK_DRUM) {
      zn = kZ0 - kDrumR;
      zf = kZ0;
    } else {
      zn = zf = kZ0;
    }
  }
  bool fills() const { return true; }
  void reset(uint32_t) {
    yawPh_ = pitchPh_ = wigglePh_ = drumPh_ = reliefPh_ = 0.0f;
    for (int k = 0; k < 3; k++) drum_[k].valid = false;
  }
  // Each swing keeps its own phase, wrapped as it goes: no jump, however long it runs.
  void step(float dt, const Env &) {
    yawPh_ = wrapAngle(yawPh_ + kTwoPi * dt / 7.0f);
    pitchPh_ = wrapAngle(pitchPh_ + kTwoPi * dt / 9.3f);
    wigglePh_ = wrapAngle(wigglePh_ + kTwoPi * dt * 2.0f);
    drumPh_ = wrapAngle(drumPh_ + kTwoPi * dt / 10.0f);
    reliefPh_ = wrapAngle(reliefPh_ + kTwoPi * dt / 8.0f);
  }
  void draw(Ctx &c) {
    if (!codes) return;
    switch (look) {
      case LOOK_POP:
      case LOOK_LAYERS:
      case LOOK_FLOAT:
      case LOOK_DOME:
        if (c.stereo()) reproject(c, 0.5f * (float)c.eye * c.st.depthPx);
        else flat(c);
        break;
      case LOOK_WIGGLE:
        // Wiggle stereoscopy: one eye, moving. About twice a second, sideways
        // by up to the depth budget.
        reproject(c, 0.5f * c.st.depthPx * (c.stereo() ? (float)c.eye : sinf(wigglePh_)));
        break;
      case LOOK_CARD: card(c); break;
      case LOOK_RELIEF: relief(c); break;
      case LOOK_DRUM: drum(c); break;
      default: flat(c); break;
    }
  }
  // The depth a pixel gets in the pixel-moving looks, 0..1 of the budget.
  // px: the page's three codes for the pixel.
  float depthOf(int x, int y, const uint8_t *px) const {
    const float r = (float)kCie8[px[0]], g = (float)kCie8[px[1]], b = (float)kCie8[px[2]];
    const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m < 2.0f) return 0.0f;   // black is the panel's own plane
    switch (look == LOOK_WIGGLE ? (uint8_t)LOOK_POP : look) {
      case LOOK_POP:
        return smoothstepf(0.0f, 160.0f, m);
      case LOOK_LAYERS: {
        const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const float sat = (m - mn) / m;
        if (sat < 0.25f) return 0.55f;                 // white and grey: the middle
        const float warm = (r - b) / m;                // +1 red, -1 blue
        return clampf(0.5f + 0.5f * warm, 0.0f, 1.0f); // warm in front, cold behind
      }
      case LOOK_FLOAT:
        return 1.0f;
      case LOOK_DOME: {
        const float u = ((float)x - 63.5f) / 63.5f, v = ((float)y - 31.5f) / 31.5f;
        return clampf(1.0f - 0.7f * u * u - 0.5f * v * v, 0.0f, 1.0f);
      }
      default:
        return 0.0f;
    }
  }

 private:
  static constexpr float kZ0 = 6.0f, kCardHalfW = 2.75f, kCardHalfH = 1.375f, kDrumR = 2.4f;
  static constexpr float kArc = 1.25f;    // radians of drum the picture covers
  static constexpr float kCardYaw = 0.36f, kCardPitch = 0.16f;   // the card's swings, radians
  static constexpr float kMiss = 100.0f;  // an angle no hit can have: the ray missed the drum
  float yawPh_, pitchPh_, wigglePh_, drumPh_, reliefPh_;

  // A code as linear light, 0..1, from the library's own 256-byte table. Read
  // on the fly: a per-frame linear copy of the page (24.6 KB of PSRAM written
  // and read every frame) made drum and wiggle slower on the panel, not faster
  // (the integration session's measurement of 3d2a1c5, 2026-09-18).
  static float lin(uint8_t code) { return (float)kCie8[code] * (1.0f / 255.0f); }

  void flat(Ctx &c) {
    for (int i = 0; i < kPixels; i++) c.pixel(i, lin(codes[3 * i]), lin(codes[3 * i + 1]), lin(codes[3 * i + 2]));
  }

  // Depth-image-based rendering, row by row. Each source pixel is a box one
  // pixel wide moved by -shift*depth; boxes of the same depth band tile and
  // add, and the bands are laid far to near, each over the ones behind it by
  // its own coverage. Uncovered pixels stay black.
  //
  // A row's pixels are sorted into their bands once (a stable counting sort,
  // so each band still adds its pixels left to right) and each band then
  // touches only its own pixels and the span they land on: the same sums in
  // the same order as walking every band over the whole row, which the host
  // test holds it to, byte for byte. That walk cost 30-44 ms a frame on the
  // panel (the integration session's measurement, 2026-09-18).
  // Rows in the object, not on the stack: loopTask has 8 KB for all of loop().
  static const int kLevels = 8;
  float band_[kW][4];   // r, g, b, coverage of the depth band being laid
  float out_[kW][3];
  float d_[kW];
  uint8_t order_[kW];
  int8_t level_[kW];

  void reproject(Ctx &c, float shift) {
    for (int y = 0; y < kH; y++) {
      const uint8_t *crow = codes + 3 * y * kW;
      int count[kLevels + 1] = {0};
      for (int x = 0; x < kW; x++) {
        const uint8_t *q = crow + 3 * x;
        level_[x] = -1;
        if (!q[0] && !q[1] && !q[2]) continue;   // black: nothing to move
        const float dd = depthOf(x, y, q);
        d_[x] = dd;
        int k = 0;   // the band, by the same comparisons as the walk over every band
        while (k < kLevels - 1 && !(k == 0 ? dd <= 1.0f / kLevels : (dd > (float)k / kLevels && dd <= (float)(k + 1) / kLevels))) k++;
        if (!(k == 0 ? dd <= 1.0f / kLevels : (dd > (float)k / kLevels && dd <= (float)(k + 1) / kLevels))) continue;
        level_[x] = (int8_t)k;
        count[k + 1]++;
      }
      for (int k = 0; k < kLevels; k++) count[k + 1] += count[k];
      int fill[kLevels];
      for (int k = 0; k < kLevels; k++) fill[k] = count[k];
      for (int x = 0; x < kW; x++)
        if (level_[x] >= 0) order_[fill[level_[x]]++] = (uint8_t)x;
      for (int x = 0; x < kW; x++) out_[x][0] = out_[x][1] = out_[x][2] = 0.0f;
      for (int k = 0; k < kLevels; k++) {
        if (count[k] == count[k + 1]) continue;
        int lo = kW, hi = -1;
        for (int n = count[k]; n < count[k + 1]; n++) {
          const int x = order_[n];
          const float xs = (float)x - shift * d_[x];
          const int i0 = (int)floorf(xs);
          if (i0 < lo) lo = i0;
          if (i0 + 1 > hi) hi = i0 + 1;
        }
        if (lo < 0) lo = 0;
        if (hi > kW - 1) hi = kW - 1;
        for (int x = lo; x <= hi; x++) band_[x][0] = band_[x][1] = band_[x][2] = band_[x][3] = 0.0f;
        for (int n = count[k]; n < count[k + 1]; n++) {
          const int x = order_[n];
          const uint8_t *p = crow + 3 * x;
          const float xs = (float)x - shift * d_[x];
          const float fl = floorf(xs);
          const int i0 = (int)fl;
          const float fr = xs - fl;
          const float col[3] = {lin(p[0]), lin(p[1]), lin(p[2])};
          for (int j = 0; j < 2; j++) {
            const int xi = i0 + j;
            if (xi < 0 || xi >= kW) continue;
            const float w = j ? fr : 1.0f - fr;
            band_[xi][0] += col[0] * w;
            band_[xi][1] += col[1] * w;
            band_[xi][2] += col[2] * w;
            band_[xi][3] += w;
          }
        }
        for (int x = lo; x <= hi; x++) {
          const float a = band_[x][3] > 1.0f ? 1.0f : band_[x][3];
          if (a <= 0.0f) continue;
          const float norm = band_[x][3] > 1.0f ? 1.0f / band_[x][3] : 1.0f;
          for (int ch = 0; ch < 3; ch++) out_[x][ch] = out_[x][ch] * (1.0f - a) + band_[x][ch] * norm;
        }
      }
      for (int x = 0; x < kW; x++) c.pixel(y * kW + x, out_[x][0], out_[x][1], out_[x][2]);
    }
  }

  // The picture sampled bilinearly at (tx, ty), in 1/256 of a pixel with the
  // pixel centres on whole pixels, as linear light 0..255 a channel; a tap off
  // the picture counts as black. Integers only, in place of a float version
  // with a lin() for every tap and channel. At 80eb788 card and drum took
  // 46.0 and 31.5 ms a frame on the panel, about 38 and 25 ms of it their own
  // work beyond the blit (the integration session's measurement); how much of
  // that was the sampler was not measured. tx and ty must be above -kBias
  // pixels.
  static const int kBias = 64;
  void sample8(int32_t tx, int32_t ty, int32_t *rgb) const {
    // Biased, so no shift sees a negative number (implementation-defined in C++11).
    const int32_t bx = tx + kBias * 256, by = ty + kBias * 256;
    const int x0 = (int)(bx >> 8) - kBias, y0 = (int)(by >> 8) - kBias;
    const int32_t fx = bx & 255, fy = by & 255;
    const int32_t w[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
    int32_t r = 0, g = 0, b = 0;
    for (int k = 0; k < 4; k++) {
      const int x = x0 + (k & 1), y = y0 + (k >> 1);
      if ((unsigned)x >= (unsigned)kW || (unsigned)y >= (unsigned)kH) continue;
      const uint8_t *p = codes + 3 * (y * kW + x);
      r += w[k] * kCie8[p[0]];
      g += w[k] * kCie8[p[1]];
      b += w[k] * kCie8[p[2]];
    }
    rgb[0] = (r + 32768) >> 16;
    rgb[1] = (g + 32768) >> 16;
    rgb[2] = (b + 32768) >> 16;
  }
  // A position in pixels as 1/256 pixel, rounded; above -kBias pixels.
  static int32_t fix8(float v) { return (int32_t)(v * 256.0f + (float)(kBias * 256) + 0.5f) - kBias * 256; }
  // A lit pixel of a look: linear bytes times shade/256 plus a lift, at most
  // full light. In stereo the eye sees the brightest channel, as Ctx::pixel.
  static void put8(uint8_t *d, bool stereo, const int32_t *rgb, int32_t shade, int32_t lift) {
    if (stereo) {
      const int32_t m = rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
      const int32_t v = ((m * shade + 128) >> 8) + lift;
      *d = (uint8_t)(v > 255 ? 255 : v);
      return;
    }
    for (int ch = 0; ch < 3; ch++) {
      const int32_t v = ((rgb[ch] * shade + 128) >> 8) + lift;
      d[ch] = (uint8_t)(v > 255 ? 255 : v);
    }
  }
  static void dark(uint8_t *d, bool stereo) {
    d[0] = 0;
    if (!stereo) d[1] = d[2] = 0;
  }

  // A card the size of most of the panel, swaying about its middle. Along a
  // row the ray is linear in x, so where it meets the card is a ratio of two
  // linear functions of x: two additions and one division a pixel instead of
  // a ray-plane intersection. The ray-plane walk cost 67.7 ms a frame on the
  // panel (the integration session's measurement, 2026-09-18). The picture is
  // sampled and written in integers, straight into the target.
  //
  // And no division a pixel: `/` on floats compiles to a call of the ROM's
  // soft-float __divsf3 on the S3 (fx3d.cpp.o called it at 222 sites at
  // 6200dc7 and 216 at 42a69d7; the symbol sits at 0x40002274, in ROM). The reciprocal of den is carried along the
  // row by one Newton step from the last pixel's, r' = r (2 - den r), whose
  // relative error is the square of the step's: den moves by at most
  // sin(kCardYaw) / f = 0.3 % of 1 a pixel, and wherever |den| < 0.25 the
  // division is done outright.
  void card(Ctx &c) {
    const M3 r = mul(rotY(kCardYaw * sinf(yawPh_)), rotX(kCardPitch * sinf(pitchPh_)));
    const V3 ctr = v3(0.0f, 0.0f, kZ0), n = apply(r, v3(0, 0, -1)), ux = apply(r, v3(1, 0, 0)), uy = apply(r, v3(0, 1, 0));
    const Light light;
    // Every test below is written so a NaN (from a NaN phase) fails it and
    // never reaches a cast to int.
    const float lit = dot(n, light.dir);
    const int32_t shade = (int32_t)((0.55f + 0.45f * (lit > 0.0f ? (lit < 1.0f ? lit : 1.0f) : 0.0f)) * 256.0f + 0.5f);
#if defined(FX3D_STATS)
    float zMin = 1e9f, zMax = 0.0f;
#endif
    const Ray o = eyeRay(c.view, c.eye, 0.0f, 0.0f);   // the ray of pixel (0, 0); x adds dx, y adds dy
    const V3 dx = v3(1.0f / c.view.f, 0.0f, 0.0f), dy = v3(0.0f, -1.0f / c.view.f, 0.0f);
    const float K = dot(ctr - o.o, n);                  // t = K / (d . n)
    const float a0 = dot(o.o - ctr, ux), b0 = dot(o.o - ctr, uy);
#if defined(FX3D_STATS)
    const float z0 = o.o.z - ctr.z;
#endif
    // The card's (a, b) to the picture's pixels: (a / kCardHalfW * 0.5 + 0.5) * kW - 0.5, b upside down.
    const float sa = 0.5f * (float)kW / kCardHalfW, oa = 0.5f * (float)kW - 0.5f;
    const float sb = -0.5f * (float)kH / kCardHalfH, ob = 0.5f * (float)kH - 0.5f;
    const bool stereo = c.stereo();
    const int step = stereo ? 1 : 3;
    uint8_t *dst = stereo ? c.plane() : c.fb.rgb;
    for (int y = 0; y < kH; y++) {
      const V3 d0 = o.d + dy * (float)y;
      float den = dot(d0, n), U = dot(d0, ux), V = dot(d0, uy);
      const float dDen = dot(dx, n), dU = dot(dx, ux), dV = dot(dx, uy);
#if defined(FX3D_STATS)
      const float Z = d0.z;
#endif
      uint8_t *d = dst + step * y * kW;
      float inv = 0.0f;
      bool haveInv = false;
      for (int x = 0; x < kW; x++, den += dDen, U += dU, V += dV, d += step) {
        if (den > -1e-4f) {   // edge-on or its back: nothing
          haveInv = false;
          dark(d, stereo);
          continue;
        }
        if (haveInv && den < -0.25f) {
          inv = inv * (2.0f - den * inv);
        } else {
          inv = 1.0f / den;
          haveInv = true;
        }
        const float t = K * inv;
        const float a = a0 + t * U, b = b0 + t * V;
        if (!(fabsf(a) <= kCardHalfW && fabsf(b) <= kCardHalfH)) {
          dark(d, stereo);
          continue;
        }
        int32_t rgb[3];
        sample8(fix8(a * sa + oa), fix8(b * sb + ob), rgb);
        // A faint edge so the card reads as a card on a dark page: 0.08 of full light.
        const int32_t edge = (fabsf(a) > kCardHalfW - 0.05f || fabsf(b) > kCardHalfH - 0.05f) ? 20 : 0;
        put8(d, stereo, rgb, shade, edge);
#if defined(FX3D_STATS)
        const float pz = ctr.z + z0 + t * Z;   // the hit's depth, for the host test's stats
        if (pz < zMin) zMin = pz;
        if (pz > zMax) zMax = pz;
#endif
      }
    }
#if defined(FX3D_STATS)
    if (zMin <= zMax) {
      c.note(zMin);
      c.note(zMax);
    }
#endif
  }

  // Raised blocks in an oblique projection: every lit pixel stands up to
  // kRise pixels tall towards the direction the "sun" throws, its side in
  // shade. Laid far to near, so a nearer block hides what is behind it. The
  // direction sways; each eye sees it a little differently, so in stereo the
  // tall blocks come forward.
  //
  // Bytes straight into the target, positions in 1/256 pixel: a float path
  // through Ctx::pixel for every side pixel cost 79-81 ms a frame on the panel
  // (the integration session's measurement, 2026-09-18).
  void relief(Ctx &c) {
    const float kRise = 3.5f;
    const float sway = sinf(reliefPh_);
    const float ex = 0.55f * sway + (c.stereo() ? 0.12f * (float)c.eye * c.st.depthPx : 0.0f);
    const float ey = -0.85f;   // up the panel, away from the viewer
    const bool stereo = c.stereo();
    uint8_t *dst = stereo ? c.plane() : c.fb.rgb;
    c.clear();
    // Far to near: top row first; along a row, from the side the blocks lean
    // towards.
    for (int y = 0; y < kH; y++)
      for (int k = 0; k < kW; k++) {
        const int x = ex >= 0.0f ? kW - 1 - k : k;
        const uint8_t *p = codes + 3 * (y * kW + x);
        const uint8_t r = kCie8[p[0]], g = kCie8[p[1]], b = kCie8[p[2]];
        const uint8_t m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (m <= 1) continue;   // 0.004 of full light and below: nothing stands up
        const float h = kRise * smoothstepf(0.0f, 0.6f, (float)m * (1.0f / 255.0f));
        const int steps = (int)ceilf(h * 1.5f);
        // The side's end in 1/256 pixel, then each step's position from integers.
        const int32_t fx = (int32_t)lrintf(ex * h * 256.0f), fy = (int32_t)lrintf(ey * h * 256.0f);
        for (int s = 0; s <= steps; s++) {   // the side, darker the lower it is
          const int32_t ox = steps ? fx * s / steps : fx, oy = steps ? fy * s / steps : fy;
          const int px = x + (int)((ox + (ox >= 0 ? 128 : -128)) / 256), py = y + (int)((oy + (oy >= 0 ? 128 : -128)) / 256);
          if ((unsigned)px >= (unsigned)kW || (unsigned)py >= (unsigned)kH) continue;
          // 1 for the top, else 0.25 + 0.25 of the way up, in 1/256.
          const int k2 = s == steps ? 256 : 64 + (steps ? 64 * s / steps : 64);
          const int i = py * kW + px;
          if (stereo) {
            dst[i] = (uint8_t)((m * k2 + 128) >> 8);
          } else {
            dst[3 * i] = (uint8_t)((r * k2 + 128) >> 8);
            dst[3 * i + 1] = (uint8_t)((g * k2 + 128) >> 8);
            dst[3 * i + 2] = (uint8_t)((b * k2 + 128) >> 8);
          }
        }
      }
  }

  // A drum with a vertical axis behind the panel's plane, the picture on its
  // front half, turning to and fro; the turn only slides the picture. The
  // rays of one column differ only in height (eyeRay: the origin at y = 0,
  // x and z of the direction the same all down the column), and the axis is
  // vertical, so they all meet the drum at one angle and one distance: the
  // table is per column, and the height on the drum runs linearly down the
  // column. With a table per pixel (72 KB an eye, read from PSRAM every
  // frame) the drum took 31.5 ms a frame on the panel at 80eb788, about 25 ms
  // of it beyond the blit (the integration session's measurement); the
  // table's own share was not measured.
  struct DrumTab {
    bool valid;
    float f, b, z0, cx, cy;
    float ang[kW];       // angle round the drum, radians; kMiss where the column misses
    float hk[kW];        // the height on the drum, -1..1 of the picture, per pixel below the view's centre
    uint8_t shade[kW];
  };
  DrumTab drum_[3];
  static const int32_t kOff = -1 - 0x7FFFFFFF;   // a column showing nothing this frame
  int32_t drumTx_[kW];   // this frame: where across the picture each column samples, 1/256 px

  void drum(Ctx &c) {
    DrumTab &t = drum_[c.eye + 1];
    if (!t.valid || t.f != c.view.f || t.b != c.view.b || t.z0 != c.view.z0 || t.cx != c.view.cx ||
        t.cy != c.view.cy)
      buildDrum(t, c.view, c.eye);
    const float turn = 0.25f * sinf(drumPh_);   // to and fro, never past a third of the picture
    for (int x = 0; x < kW; x++) {
      const float u = (t.ang[x] - turn) / kArc;   // -0.5..0.5 across the picture
      drumTx_[x] = (t.ang[x] < kMiss && fabsf(u) <= 0.5f) ? fix8((u + 0.5f) * kW - 0.5f) : kOff;   // NaN: off
    }
    const bool stereo = c.stereo();
    const int step = stereo ? 1 : 3;
    uint8_t *d = stereo ? c.plane() : c.fb.rgb;
    for (int y = 0; y < kH; y++) {
      const float below = (float)y - c.view.cy;
      for (int x = 0; x < kW; x++, d += step) {
        const float ht = t.hk[x] * below;
        if (drumTx_[x] == kOff || !(fabsf(ht) <= 1.0f)) {
          dark(d, stereo);
          continue;
        }
        int32_t rgb[3];
        sample8(drumTx_[x], fix8((0.5f - 0.5f * ht) * kH - 0.5f), rgb);
        put8(d, stereo, rgb, (t.shade[x] * 257 + 128) >> 8, 0);   // 0..255 to 0..256
      }
    }
    c.note(kZ0 - kDrumR);
    c.note(kZ0);
  }
  static void buildDrum(DrumTab &t, const View &view, int eye) {
    const float zc = kZ0;   // the axis; the drum's front is at kZ0 - R
    const float halfH = kDrumR * kArc / 4.0f;   // the picture keeps its 2:1 on the drum
    for (int x = 0; x < kW; x++) {
      const Ray r = eyeRay(view, eye, (float)x, view.cy);   // any row: only the height differs
      // (o.x + t d.x)^2 + (o.z + t d.z - zc)^2 = R^2, nearest root.
      const float ox = r.o.x, oz = r.o.z - zc;
      const float a = r.d.x * r.d.x + r.d.z * r.d.z, b = 2.0f * (ox * r.d.x + oz * r.d.z),
                  cc = ox * ox + oz * oz - kDrumR * kDrumR;
      const float disc = b * b - 4.0f * a * cc;
      if (disc < 0.0f) {
        t.ang[x] = kMiss;
        t.hk[x] = 0.0f;
        t.shade[x] = 0;
        continue;
      }
      const float tt = (-b - sqrtf(disc)) / (2.0f * a);
      const float px = r.o.x + r.d.x * tt, pz = r.o.z + r.d.z * tt;
      t.ang[x] = atan2f(px, zc - pz);   // 0 facing the viewer
      // Down the column the direction's y is -(y - cy) / f, and the hit is tt along it.
      t.hk[x] = -tt / (view.f * halfH);
      const float facing = (zc - pz) / kDrumR;   // cos of the angle: 1 facing us
      t.shade[x] = (uint8_t)(clampf(0.25f + 0.75f * facing, 0.0f, 1.0f) * 255.0f);
    }
    t.f = view.f;
    t.b = view.b;
    t.z0 = view.z0;
    t.cx = view.cx;
    t.cy = view.cy;
    t.valid = true;
  }
};

}  // namespace fx3d

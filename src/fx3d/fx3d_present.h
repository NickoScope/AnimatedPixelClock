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
  PictureScene() : codes(nullptr), look(LOOK_FLAT), t_(0.0f) {
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
    t_ = 0.0f;
    for (int k = 0; k < 3; k++) drum_[k].valid = false;
  }
  void step(float dt, const Env &) { t_ = fmodf(t_ + dt, 3600.0f); }
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
        reproject(c, 0.5f * c.st.depthPx * (c.stereo() ? (float)c.eye : sinf(kTwoPi * 2.0f * t_)));
        break;
      case LOOK_CARD: card(c); break;
      case LOOK_RELIEF: relief(c); break;
      case LOOK_DRUM: drum(c); break;
      default: flat(c); break;
    }
  }
  // The depth a pixel gets in the pixel-moving looks, 0..1 of the budget.
  float depthOf(int x, int y, const uint8_t *px) const {
    const float r = (float)kCie8[px[0]], g = (float)kCie8[px[1]], b = (float)kCie8[px[2]];
    const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m < 2.0f) return 0.0f;   // black is the panel's own plane
    switch (look == LOOK_WIGGLE ? LOOK_POP : look) {
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
  float t_;

  static float lin(uint8_t code) { return (float)kCie8[code] * (1.0f / 255.0f); }

  void flat(Ctx &c) {
    for (int i = 0; i < kPixels; i++) c.pixel(i, lin(codes[3 * i]), lin(codes[3 * i + 1]), lin(codes[3 * i + 2]));
  }

  // Depth-image-based rendering, row by row. Each source pixel is a box one
  // pixel wide moved by -shift*depth; boxes of the same depth band tile and
  // add, and the bands are laid far to near, each over the ones behind it by
  // its own coverage. Uncovered pixels stay black.
  // Rows in the object, not on the stack: loopTask has 8 KB for all of loop().
  float band_[kW][4];   // r, g, b, coverage of the depth band being laid
  float out_[kW][3];
  float d_[kW];

  void reproject(Ctx &c, float shift) {
    const int kLevels = 8;
    float(&band)[kW][4] = band_;
    float(&out)[kW][3] = out_;
    float(&d)[kW] = d_;
    for (int y = 0; y < kH; y++) {
      memset(out, 0, sizeof out);
      for (int x = 0; x < kW; x++) d[x] = depthOf(x, y, codes + 3 * (y * kW + x));
      for (int k = 0; k < kLevels; k++) {
        const float lo = (float)k / kLevels, hi = (float)(k + 1) / kLevels;
        bool any = false;
        memset(band, 0, sizeof band);
        for (int x = 0; x < kW; x++) {
          const float dd = d[x];
          if (!(k == 0 ? dd <= hi : (dd > lo && dd <= hi))) continue;
          const uint8_t *p = codes + 3 * (y * kW + x);
          if (!p[0] && !p[1] && !p[2]) continue;
          const float xs = (float)x - shift * dd;
          const float fl = floorf(xs);
          const int i0 = (int)fl;
          const float fr = xs - fl;
          const float col[3] = {lin(p[0]), lin(p[1]), lin(p[2])};
          for (int j = 0; j < 2; j++) {
            const int xi = i0 + j;
            if (xi < 0 || xi >= kW) continue;
            const float w = j ? fr : 1.0f - fr;
            band[xi][0] += col[0] * w;
            band[xi][1] += col[1] * w;
            band[xi][2] += col[2] * w;
            band[xi][3] += w;
          }
          any = true;
        }
        if (!any) continue;
        for (int x = 0; x < kW; x++) {
          const float a = band[x][3] > 1.0f ? 1.0f : band[x][3];
          if (a <= 0.0f) continue;
          const float norm = band[x][3] > 1.0f ? 1.0f / band[x][3] : 1.0f;
          for (int ch = 0; ch < 3; ch++) out[x][ch] = out[x][ch] * (1.0f - a) + band[x][ch] * norm;
        }
      }
      for (int x = 0; x < kW; x++) c.pixel(y * kW + x, out[x][0], out[x][1], out[x][2]);
    }
  }

  // The picture sampled bilinearly at (tx, ty), pixel centres at integers.
  void sample(float tx, float ty, float *rgb) const {
    const float fx = floorf(tx), fy = floorf(ty);
    const int x0 = (int)fx, y0 = (int)fy;
    const float ax = tx - fx, ay = ty - fy;
    for (int ch = 0; ch < 3; ch++) rgb[ch] = 0.0f;
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) {
        const int x = x0 + i, y = y0 + j;
        if ((unsigned)x >= (unsigned)kW || (unsigned)y >= (unsigned)kH) continue;
        const float w = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
        const uint8_t *p = codes + 3 * (y * kW + x);
        rgb[0] += w * lin(p[0]);
        rgb[1] += w * lin(p[1]);
        rgb[2] += w * lin(p[2]);
      }
  }

  // A card the size of most of the panel, swaying about its middle.
  void card(Ctx &c) {
    const M3 r = mul(rotY(kCardYaw * sinf(kTwoPi * t_ / 7.0f)), rotX(kCardPitch * sinf(kTwoPi * t_ / 9.3f)));
    const V3 ctr = v3(0.0f, 0.0f, kZ0), n = apply(r, v3(0, 0, -1)), ux = apply(r, v3(1, 0, 0)), uy = apply(r, v3(0, 1, 0));
    const Light light;
    const float shade = 0.55f + 0.45f * clampf(dot(n, light.dir), 0.0f, 1.0f);
    float zMin = 1e9f, zMax = 0.0f;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const Ray ray = eyeRay(c.view, c.eye, (float)x, (float)y);
        const float den = dot(ray.d, n);
        const int i = y * kW + x;
        if (den > -1e-4f) {   // edge-on or its back: nothing
          c.pixel(i, 0.0f, 0.0f, 0.0f);
          continue;
        }
        const float t = dot(ctr - ray.o, n) / den;
        const V3 p = ray.o + ray.d * t;
        const float a = dot(p - ctr, ux), b = dot(p - ctr, uy);
        if (fabsf(a) > kCardHalfW || fabsf(b) > kCardHalfH) {
          c.pixel(i, 0.0f, 0.0f, 0.0f);
          continue;
        }
        float rgb[3];
        sample((a / kCardHalfW * 0.5f + 0.5f) * kW - 0.5f, (0.5f - b / kCardHalfH * 0.5f) * kH - 0.5f, rgb);
        // A faint edge so the card reads as a card on a dark page.
        const float edge = (fabsf(a) > kCardHalfW - 0.05f || fabsf(b) > kCardHalfH - 0.05f) ? 0.08f : 0.0f;
        c.pixel(i, rgb[0] * shade + edge, rgb[1] * shade + edge, rgb[2] * shade + edge);
        if (p.z < zMin) zMin = p.z;
        if (p.z > zMax) zMax = p.z;
      }
    if (zMin <= zMax) {
      c.note(zMin);
      c.note(zMax);
    }
  }

  // Raised blocks in an oblique projection: every lit pixel stands up to
  // kRise pixels tall towards the direction the "sun" throws, its side in
  // shade. Laid far to near, so a nearer block hides what is behind it. The
  // direction sways; each eye sees it a little differently, so in stereo the
  // tall blocks come forward.
  void relief(Ctx &c) {
    const float kRise = 3.5f;
    const float sway = sinf(kTwoPi * t_ / 8.0f);
    const float ex = 0.55f * sway + (c.stereo() ? 0.12f * (float)c.eye * c.st.depthPx : 0.0f);
    const float ey = -0.85f;   // up the panel, away from the viewer
    for (int i = 0; i < kPixels; i++) c.pixel(i, 0.0f, 0.0f, 0.0f);
    // Far to near: top row first; along a row, from the side the blocks lean
    // towards.
    for (int y = 0; y < kH; y++)
      for (int k = 0; k < kW; k++) {
        const int x = ex >= 0.0f ? kW - 1 - k : k;
        const uint8_t *p = codes + 3 * (y * kW + x);
        const float r = lin(p[0]), g = lin(p[1]), b = lin(p[2]);
        const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (m <= 0.004f) continue;
        const float h = kRise * smoothstepf(0.0f, 0.6f, m);
        const int steps = (int)ceilf(h * 1.5f);
        for (int s = 0; s <= steps; s++) {   // the side, darker the lower it is
          const float f = steps ? (float)s / steps : 1.0f;
          const int px = (int)lrintf((float)x + ex * h * f), py = (int)lrintf((float)y + ey * h * f);
          if ((unsigned)px >= (unsigned)kW || (unsigned)py >= (unsigned)kH) continue;
          const float k2 = s == steps ? 1.0f : 0.25f + 0.25f * f;
          c.pixel(py * kW + px, r * k2, g * k2, b * k2);
        }
      }
  }

  // A drum with a vertical axis behind the panel's plane, the picture on its
  // front half, turning to and fro. Where each pixel's ray meets the drum is
  // fixed geometry, worked out once per eye; the turn only slides the picture.
  struct DrumTab {
    bool valid;
    float f, b, z0;
    float ang[kPixels];     // angle round the drum, radians; kMiss where the ray misses
    float ht[kPixels];      // height on the drum, -1..1 of the picture
    uint8_t shade[kPixels];
  };
  DrumTab drum_[3];

  void drum(Ctx &c) {
    DrumTab &t = drum_[c.eye + 1];
    if (!t.valid || t.f != c.view.f || t.b != c.view.b || t.z0 != c.view.z0) buildDrum(t, c.view, c.eye);
    const float turn = 0.25f * sinf(kTwoPi * t_ / 10.0f);   // to and fro, never past a third of the picture
    for (int i = 0; i < kPixels; i++) {
      const float a = t.ang[i];
      if (a >= kMiss) {
        c.pixel(i, 0.0f, 0.0f, 0.0f);
        continue;
      }
      const float u = (a - turn) / kArc;   // -0.5..0.5 across the picture
      if (fabsf(u) > 0.5f || fabsf(t.ht[i]) > 1.0f) {
        c.pixel(i, 0.0f, 0.0f, 0.0f);
        continue;
      }
      float rgb[3];
      sample((u + 0.5f) * kW - 0.5f, (0.5f - 0.5f * t.ht[i]) * kH - 0.5f, rgb);
      const float s = (float)t.shade[i] / 255.0f;
      c.pixel(i, rgb[0] * s, rgb[1] * s, rgb[2] * s);
    }
    c.note(kZ0 - kDrumR);
    c.note(kZ0);
  }
  static void buildDrum(DrumTab &t, const View &view, int eye) {
    const float zc = kZ0;   // the axis; the drum's front is at kZ0 - R
    const float halfH = kDrumR * kArc / 4.0f;   // the picture keeps its 2:1 on the drum
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const int i = y * kW + x;
        const Ray r = eyeRay(view, eye, (float)x, (float)y);
        // (o.x + t d.x)^2 + (o.z + t d.z - zc)^2 = R^2, nearest root.
        const float ox = r.o.x, oz = r.o.z - zc;
        const float a = r.d.x * r.d.x + r.d.z * r.d.z, b = 2.0f * (ox * r.d.x + oz * r.d.z),
                    cc = ox * ox + oz * oz - kDrumR * kDrumR;
        const float disc = b * b - 4.0f * a * cc;
        if (disc < 0.0f) {
          t.ang[i] = kMiss;
          continue;
        }
        const float tt = (-b - sqrtf(disc)) / (2.0f * a);
        const V3 p = r.o + r.d * tt;
        t.ang[i] = atan2f(p.x, zc - p.z);   // 0 facing the viewer
        t.ht[i] = p.y / halfH;
        const float facing = (zc - p.z) / kDrumR;   // cos of the angle: 1 facing us
        t.shade[i] = (uint8_t)(clampf(0.25f + 0.75f * facing, 0.0f, 1.0f) * 255.0f);
      }
    t.f = view.f;
    t.b = view.b;
    t.z0 = view.z0;
    t.valid = true;
  }
};

}  // namespace fx3d

#pragma once
// Scenes that shade every pixel themselves: a voxel landscape to fly over, a
// tunnel, metaballs by sphere tracing, and the Earth with today's day and
// night. In stereo each is rendered once per eye from that eye's position,
// through the same off-axis frustum as View::project, so its depth is real
// rather than a shifted copy. Plain C++11.

#include "fx3d_scenes_solid.h"
#include "../worldclock/worldmap.h"

namespace fx3d {

// The ray through pixel centre (sx, sy) for an eye: the point at depth z on it
// is exactly what View::project sends back to (sx, sy). d.z is 1, so the ray
// parameter is the depth.
struct Ray {
  V3 o, d;
};
inline Ray eyeRay(const View &v, int eye, float sx, float sy) {
  const float e = (float)eye;
  const float k = e * v.f * v.b / (2.0f * v.z0);
  Ray r;
  r.o = v3(e * 0.5f * v.b, 0.0f, 0.0f);
  r.d = v3((sx - v.cx - k) / v.f, -(sy - v.cy) / v.f, 1.0f);
  return r;
}

// Integer hash (the "lowbias32" constants of Chris Wellons' hash-prospector),
// for noise lattices: the same seed gives the same landscape everywhere.
inline uint32_t hash3(uint32_t x, uint32_t y, uint32_t s) {
  uint32_t h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u ^ s * 0xC2B2AE3Du;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

// ----------------------------------------------------------------- voxel ----
// A landscape flown over low, drawn the way NovaLogic's Comanche (1992) did
// it and Sebastian Macke's "VoxelSpace" write-up explains: per screen column,
// march out over a height map front to back and draw each slice only where it
// rises above everything nearer. The map is 256 x 256, generated from the seed
// and tiled, with its light baked in.
class VoxelScene : public Scene {
 public:
  static const int kN = 256;
  VoxelScene() : x_(0.0f), y_(0.0f), heading_(0.0f), alt_(60.0f), t_(0.0f) {}
  const char *id() const { return "voxel"; }
  void setup(View &v) const {
    v.f = 72.0f;
    v.z0 = kZ0;
    v.nearZ = 1.0f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZNear;
    zf = kZFar;
  }
  bool fills() const { return true; }
  void reset(uint32_t seed) {
    generate(seed);
    x_ = 40.0f;
    y_ = 40.0f;
    heading_ = 0.3f;
    t_ = 0.0f;
    alt_ = heightAt(x_, y_) + kClearance;
  }
  void step(float dt, const Env &) {
    t_ = fmodf(t_ + dt, 3600.0f);
    heading_ = wrapAngle(heading_ + dt * 0.16f * sinf(kTwoPi * t_ / 40.0f));
    x_ = fmodf(x_ + sinf(heading_) * kSpeed * dt + kN, (float)kN);
    y_ = fmodf(y_ + cosf(heading_) * kSpeed * dt + kN, (float)kN);
    // Hold a clearance over the highest ground in the next stretch ahead.
    float top = 0.0f;
    for (int k = 0; k < 8; k++) {
      float d = 6.0f * k;
      float h = heightAt(x_ + sinf(heading_) * d, y_ + cosf(heading_) * d);
      if (h > top) top = h;
    }
    float want = top + kClearance;
    alt_ += (want - alt_) * clampf(dt * 1.5f, 0.0f, 1.0f);
  }
  void draw(Ctx &c) {
    const View &v = c.view;
    const float e = (float)c.eye;
    const float shift = e * v.f * v.b / (2.0f * v.z0);
    const float fx = sinf(heading_), fy = cosf(heading_);    // forward on the map
    const float rx = cosf(heading_), ry = -sinf(heading_);   // right on the map
    const float ox = x_ + rx * e * 0.5f * v.b, oy = y_ + ry * e * 0.5f * v.b;
    // For the glasses a lit sky or a coloured plain is one flat field in both
    // eyes: stereo draws the relief as light on black instead.
    const bool stereo = c.stereo();
    const float sky = stereo ? 0.0f : 1.0f;
    const Col lo = skyLow(), hi = skyHigh();
    float zMin = kZFar;
    for (int x = 0; x < kW; x++) {
      const float lat = ((float)x - v.cx - shift) / v.f;
      const float dx = fx + rx * lat, dy = fy + ry * lat;
      int ybuf = kH;
      float z = kZNear, dz = 0.2f;
      while (z < kZFar && ybuf > 0) {
        const int mx = (int)floorf(ox + dx * z) & (kN - 1), my = (int)floorf(oy + dy * z) & (kN - 1);
        const int m = my * kN + mx;
        const float y = kHorizon + (alt_ - (float)h_[m]) * v.f / z;
        if (y < (float)ybuf) {
          int top = (int)ceilf(y);
          if (top < 0) top = 0;
          const float fog = smoothstepf(kZFar * 0.35f, kZFar, z);
          float r, g, b;
          if (stereo) {   // the shape, not the colours: relief light and height, fading into black
            const float lum = (float)l_[m] / 255.0f * (0.35f + 0.65f * (float)h_[m] / 170.0f) * (1.0f - fog);
            r = g = b = lum;
          } else {
            r = mixf(c_[3 * m] / 255.0f, lo.r, fog);
            g = mixf(c_[3 * m + 1] / 255.0f, lo.g, fog);
            b = mixf(c_[3 * m + 2] / 255.0f, lo.b, fog);
          }
          for (int yy = top; yy < ybuf; yy++) c.pixel(yy * kW + x, r, g, b);
          if (z < zMin) zMin = z;
          ybuf = top;
        }
        z += dz;
        dz *= 1.02f;
      }
      for (int yy = 0; yy < ybuf; yy++) {
        const float t = clampf((float)yy / kHorizon, 0.0f, 1.0f);
        c.pixel(yy * kW + x, sky * mixf(hi.r, lo.r, t), sky * mixf(hi.g, lo.g, t), sky * mixf(hi.b, lo.b, t));
      }
    }
    c.note(zMin);
    c.note(kZFar);
  }

 private:
  static constexpr float kZ0 = 24.0f, kZNear = 2.0f, kZFar = 180.0f;
  static constexpr float kHorizon = 22.0f, kSpeed = 16.0f, kClearance = 16.0f;
  static Col skyHigh() {
    Col k = {0.05f, 0.12f, 0.45f};
    return k;
  }
  static Col skyLow() {
    Col k = {0.55f, 0.45f, 0.55f};
    return k;
  }
  uint8_t h_[kN * kN];
  uint8_t c_[kN * kN * 3];   // linear RGB, light baked in
  uint8_t l_[kN * kN];       // the baked light alone, for stereo
  float x_, y_, heading_, alt_, t_;

  float heightAt(float x, float y) const {
    return (float)h_[((int)floorf(y) & (kN - 1)) * kN + ((int)floorf(x) & (kN - 1))];
  }
  // Value noise on a tiling lattice, six octaves.
  static float noise(int x, int y, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < 6; o++) {
      const int cell = 64 >> o, per = kN / cell;
      const int gx = x / cell, gy = y / cell;
      const float fx = (float)(x % cell) / cell, fy = (float)(y % cell) / cell;
      const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
      const float a = (float)(hash3(gx % per, gy % per, seed + o) & 0xFFFF) / 65535.0f;
      const float b = (float)(hash3((gx + 1) % per, gy % per, seed + o) & 0xFFFF) / 65535.0f;
      const float c = (float)(hash3(gx % per, (gy + 1) % per, seed + o) & 0xFFFF) / 65535.0f;
      const float d = (float)(hash3((gx + 1) % per, (gy + 1) % per, seed + o) & 0xFFFF) / 65535.0f;
      sum += amp * mixf(mixf(a, b, sx), mixf(c, d, sx), sy);
      norm += amp;
      amp *= 0.5f;
    }
    return sum / norm;
  }
  void generate(uint32_t seed) {
    const float water = 34.0f;
    for (int y = 0; y < kN; y++)
      for (int x = 0; x < kN; x++) {
        float n = noise(x, y, seed);
        n = clampf((n - 0.28f) / 0.5f, 0.0f, 1.0f);
        float h = n * n * 150.0f + 12.0f;
        h_[y * kN + x] = (uint8_t)(h < water ? water : clampf(h, 0.0f, 255.0f));
      }
    for (int y = 0; y < kN; y++)
      for (int x = 0; x < kN; x++) {
        const int m = y * kN + x;
        const float h = (float)h_[m];
        const float gx = (float)h_[y * kN + ((x + kN - 1) & (kN - 1))] - (float)h_[y * kN + ((x + 1) & (kN - 1))];
        const float gy = (float)h_[((y + kN - 1) & (kN - 1)) * kN + x] - (float)h_[((y + 1) & (kN - 1)) * kN + x];
        const float light = clampf(0.72f + 0.035f * (gx + 0.6f * gy), 0.3f, 1.25f);
        Col k;
        if (h <= water + 0.5f) {
          k.r = 0.02f; k.g = 0.10f; k.b = 0.30f;
        } else if (h < water + 5.0f) {
          k.r = 0.55f; k.g = 0.45f; k.b = 0.22f;
        } else if (h < 80.0f) {
          const float t = (h - water) / (80.0f - water);
          k.r = mixf(0.10f, 0.06f, t); k.g = mixf(0.40f, 0.22f, t); k.b = mixf(0.06f, 0.04f, t);
        } else if (h < 120.0f) {
          k.r = 0.30f; k.g = 0.26f; k.b = 0.22f;
        } else {
          k.r = 0.85f; k.g = 0.88f; k.b = 0.95f;
        }
        const float l = h <= water + 0.5f ? 1.0f : light;
        l_[m] = (uint8_t)(clampf(h <= water + 0.5f ? 0.12f : 0.8f * light, 0.0f, 1.0f) * 255.0f);
        c_[3 * m] = (uint8_t)(clampf(k.r * l, 0.0f, 1.0f) * 255.0f);
        c_[3 * m + 1] = (uint8_t)(clampf(k.g * l, 0.0f, 1.0f) * 255.0f);
        c_[3 * m + 2] = (uint8_t)(clampf(k.b * l, 0.0f, 1.0f) * 255.0f);
      }
  }
};

// ---------------------------------------------------------------- tunnel ----
// Flying down a tube of neon rings and stripes. Where each pixel's ray meets
// the wall is fixed geometry, so it is worked out once per eye (and again only
// if the view changes) and each frame only slides the pattern along it.
class TunnelScene : public Scene {
 public:
  TunnelScene() : travel_(0.0f), twist_(0.0f), hue_(0.0f) {
    for (int k = 0; k < 3; k++) tab_[k].valid = false;
  }
  const char *id() const { return "tunnel"; }
  void setup(View &v) const {
    v.f = 64.0f;
    v.z0 = kZ0;
    v.nearZ = 0.3f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZNear;
    zf = kZFar;
  }
  bool fills() const { return true; }
  void reset(uint32_t) {
    travel_ = twist_ = hue_ = 0.0f;
    for (int k = 0; k < 3; k++) tab_[k].valid = false;
  }
  void step(float dt, const Env &) {
    travel_ = fmodf(travel_ + 7.0f * dt, 1024.0f);
    twist_ = fmodf(twist_ + 0.05f * dt, 1.0f);
    hue_ = fmodf(hue_ + 0.03f * dt, 1.0f);
  }
  void draw(Ctx &c) {
    Tab &t = tab_[c.eye + 1];
    if (!t.valid || t.f != c.view.f || t.b != c.view.b || t.z0 != c.view.z0) build(t, c.view, c.eye);
    const int du = (int)(twist_ * 256.0f), dv = (int)(travel_ * 16.0f);
    const float base = c.stereo() ? 0.0f : 0.06f;   // the glasses want black between the lines
    for (int i = 0; i < kPixels; i++) {
      const int u = (t.u[i] + du) & 255, v = (t.v[i] + dv) & 1023;
      // Rings every 64 texels along, eight stripes around; a glow falls off
      // from both.
      const int ru = u & 31, rv = v & 63;
      const int eu = ru < 16 ? ru : 32 - ru, ev = rv < 32 ? rv : 64 - rv;
      const float ring = ev < 3 ? 1.0f - ev / 3.0f : 0.0f;
      const float stripe = eu < 2 ? 0.55f * (1.0f - eu / 2.0f) : 0.0f;
      const float glow = base + ring + stripe;
      const Col k = palette(hue_ + (float)((v >> 6) & 7) / 8.0f);
      const float s = glow * (float)t.shade[i] / 255.0f;
      c.pixel(i, k.r * s, k.g * s, k.b * s);
    }
    c.note(t.zMin);
    c.note(t.zMax);
  }

 private:
  static constexpr float kZ0 = 3.0f, kZNear = 0.3f, kZFar = 48.0f;
  struct Tab {
    bool valid;
    float f, b, z0, zMin, zMax;
    uint8_t u[kPixels];
    uint16_t v[kPixels];
    uint8_t shade[kPixels];
  };
  Tab tab_[3];   // left, mono, right
  float travel_, twist_, hue_;

  // Ray against the cylinder x^2 + y^2 = 1 from inside it: the far root.
  static void build(Tab &t, const View &view, int eye) {
    t.zMin = kZFar;
    t.zMax = 0.0f;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const Ray r = eyeRay(view, eye, (float)x, (float)y);
        const float a = r.d.x * r.d.x + r.d.y * r.d.y;
        const float b = 2.0f * (r.o.x * r.d.x + r.o.y * r.d.y);
        const float cc = r.o.x * r.o.x + r.o.y * r.o.y - 1.0f;
        float z = kZFar;
        if (a > 1.0e-6f) {
          const float disc = b * b - 4.0f * a * cc;
          z = (-b + sqrtf(disc > 0.0f ? disc : 0.0f)) / (2.0f * a);
          if (z > kZFar) z = kZFar;
          if (z < kZNear) z = kZNear;
        }
        const float px = r.o.x + r.d.x * z, py = r.o.y + r.d.y * z;
        const int i = y * kW + x;
        t.u[i] = (uint8_t)((int)((atan2f(py, px) / kTwoPi + 0.5f) * 256.0f) & 255);
        t.v[i] = (uint16_t)((int)(z * 16.0f) & 1023);
        t.shade[i] = (uint8_t)(clampf(2.4f / (z + 0.6f), 0.0f, 1.0f) * 255.0f);
        if (z < t.zMin) t.zMin = z;
        if (z > t.zMax) t.zMax = z;
      }
    t.f = view.f;
    t.b = view.b;
    t.z0 = view.z0;
    t.valid = true;
  }
};

// ----------------------------------------------------------------- blobs ----
// Metaballs: four spheres melted together with a smooth minimum, found by
// sphere tracing. Formulas from Inigo Quilez's articles "smooth minimum"
// (the polynomial smin) and "normals for an SDF" (the tetrahedron). A ray
// that misses the bounding sphere costs one test.
class BlobsScene : public Scene {
 public:
  static const int kBalls = 4;
  BlobsScene() : t_(0.0f) {}
  const char *id() const { return "blobs"; }
  void setup(View &v) const {
    v.f = 96.0f;
    v.z0 = kZ;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - kBound;
    zf = kZ + kBound;
  }
  bool fills() const { return true; }
  void reset(uint32_t) { t_ = 0.0f; }
  void step(float dt, const Env &) {
    t_ = fmodf(t_ + dt, 600.0f);
    for (int i = 0; i < kBalls; i++) {
      const float a = 0.5f + 0.13f * i, b = 0.37f + 0.11f * i, g = 0.29f + 0.07f * i;
      pos_[i] = v3(1.05f * sinf(a * t_ + 1.7f * i), 0.5f * sinf(b * t_ + 0.9f * i), 0.6f * sinf(g * t_ + 2.3f * i)) +
                v3(0.0f, 0.0f, kZ);
    }
  }
  void draw(Ctx &c) {
    const V3 centre = v3(0.0f, 0.0f, kZ);
    const Light light;
    const Col tint[kBalls] = {{1.0f, 0.25f, 0.2f}, {0.2f, 0.55f, 1.0f}, {0.25f, 1.0f, 0.45f}, {1.0f, 0.8f, 0.2f}};
    float zMin = 1.0e9f, zMax = 0.0f;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const int i = y * kW + x;
        Ray r = eyeRay(c.view, c.eye, (float)x, (float)y);
        const float len = length(r.d);
        const V3 d = r.d * (1.0f / len);
        // The bounding sphere: enter and leave.
        const V3 oc = r.o - centre;
        const float bb = dot(oc, d), cc = dot(oc, oc) - kBound * kBound, disc = bb * bb - cc;
        const float bg = c.stereo() ? 0.0f : 0.004f + 0.012f * (float)y / kH;
        if (disc <= 0.0f) {
          c.pixel(i, bg * 0.4f, bg * 0.5f, bg);
          continue;
        }
        const float sq = sqrtf(disc);
        float t = -bb - sq;
        const float tEnd = -bb + sq;
        bool hit = false;
        V3 p = r.o;
        for (int s = 0; s < 40 && t < tEnd; s++) {
          p = r.o + d * t;
          const float dist = sdf(p);
          if (dist < 0.004f) {
            hit = true;
            break;
          }
          t += dist;
        }
        if (!hit) {
          c.pixel(i, bg * 0.4f, bg * 0.5f, bg);
          continue;
        }
        const V3 n = normal(p);
        if (p.z < zMin) zMin = p.z;
        if (p.z > zMax) zMax = p.z;
        // Colour: each ball's tint weighted by how close it is.
        float wr = 0.0f, wg = 0.0f, wb = 0.0f, wsum = 0.0f;
        for (int k = 0; k < kBalls; k++) {
          float dd = length(p - pos_[k]) - radius(k);
          float w = 1.0f / (0.02f + dd * dd);
          wr += w * tint[k].r;
          wg += w * tint[k].g;
          wb += w * tint[k].b;
          wsum += w;
        }
        const float s = light.shade(n, 0.1f, 0.0f);
        const float h = dot(n, light.half);
        float spec = 0.0f;
        if (h > 0.0f) {
          float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
          spec = 0.8f * h16 * h16;
        }
        const float rim = 1.0f + dot(n, d);   // grazing: 1, facing: 0
        const float rim3 = 0.35f * rim * rim * rim;
        c.pixel(i, s * wr / wsum + spec + rim3 * 0.3f, s * wg / wsum + spec + rim3 * 0.5f,
                s * wb / wsum + spec + rim3);
      }
    if (zMin <= zMax) {
      c.note(zMin);
      c.note(zMax);
    }
  }
 private:
  static constexpr float kZ = 6.0f, kBound = 2.2f, kK = 0.45f;
  static float radius(int k) { return 0.62f - 0.057f * (float)k; }
  float t_;
  V3 pos_[kBalls];
  static float smin(float a, float b, float k) {
    const float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return mixf(b, a, h) - k * h * (1.0f - h);
  }
  float sdf(V3 p) const {
    float d = length(p - pos_[0]) - radius(0);
    for (int k = 1; k < kBalls; k++) d = smin(d, length(p - pos_[k]) - radius(k), kK);
    return d;
  }
  V3 normal(V3 p) const {
    const float h = 0.002f;
    const V3 k1 = v3(1, -1, -1), k2 = v3(-1, -1, 1), k3 = v3(-1, 1, -1), k4 = v3(1, 1, 1);
    return normalize(k1 * sdf(p + k1 * h) + k2 * sdf(p + k2 * h) + k3 * sdf(p + k3 * h) + k4 * sdf(p + k4 * h));
  }
};

// ----------------------------------------------------------------- globe ----
// The Earth as it is lit at this moment, seen from a camera orbiting it. The
// land is the world clock's own mask (Natural Earth 1:110m, 64 x 32), read
// with a soft edge; the sun is the world clock's own model (declination by the
// cosine approximation, subsolar longitude from UTC, twilight to -6 degrees),
// so this page and the world clock always agree about where it is night. The
// world clock's cities are dots.
class GlobeScene : public Scene {
 public:
  GlobeScene() : spin_(0.0f), decl_(0.0f), subLon_(0.0f), home_(-1), t_(0.0f) {}
  const char *id() const { return "globe"; }
  void setup(View &v) const {
    v.f = 110.0f;
    v.z0 = kZ0;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - kR;
    zf = kZStars;
  }
  bool fills() const { return true; }
  void reset(uint32_t seed) {
    spin_ = kPi;   // the side facing the viewer starts at longitude 0
    started_ = false;
    t_ = 0.0f;
    Rng rng(seed);
    for (int k = 0; k < kStars; k++) {
      star_[k].x = rng.range(0.0f, (float)kW);
      star_[k].y = rng.range(0.0f, (float)kH);
      star_[k].z = rng.range(0.15f, 0.6f);   // brightness
    }
  }
  // Which of the world clock's cities is home, or -1: set by the panel.
  void setHome(int i) { home_ = i; }
  void step(float dt, const Env &w) {
    spin_ = wrapAngle(spin_ + dt * kTwoPi / 48.0f);
    t_ = fmodf(t_ + dt, 60.0f);
    if (w.valid) {
      decl_ = -23.44f * (kPi / 180.0f) * cosf(kTwoPi * (float)(w.yday + 10) / 365.0f);
      subLon_ = -15.0f * (w.utcHours - 12.0f) * (kPi / 180.0f);
      // The longitude facing the viewer is pi - spin. Open a radian east of
      // the subsolar point, on the afternoon side with the dusk line in view;
      // the spin then carries the view westward, as the real Earth turns.
      if (!started_) spin_ = wrapAngle(kPi - subLon_ - 1.0f);
      started_ = true;
    }
  }
  void draw(Ctx &c) {
    const View &v = c.view;
    const float e = (float)c.eye;
    const float shift = e * v.f * v.b / (2.0f * v.z0);
    // Stars, far behind the globe: at depth kZStars for this eye.
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) c.pixel(y * kW + x, 0.0f, 0.0f, 0.0f);
    const float starShift = e * 0.5f * v.b * v.f / kZStars - shift;   // where depth kZStars lands for this eye
    for (int k = 0; k < kStars; k++) starPoint(c, star_[k].x - starShift, star_[k].y, star_[k].z);
    c.note(kZStars);
    // The globe: tilted 23.4 degrees for the look, spun about its axis.
    const M3 m = mul(rotZ(0.41f), rotY(spin_));   // Earth frame -> camera
    const V3 sun = v3(cosf(decl_) * sinf(subLon_), sinf(decl_), cosf(decl_) * cosf(subLon_));
    const V3 centre = v3(0.0f, 0.0f, kZ);
    const V3 view = v3(-0.35f, 0.25f, -1.0f);   // for the glint
    float zMin = kZ;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        const Ray r = eyeRay(v, c.eye, (float)x, (float)y);
        const V3 d = normalize(r.d);
        const V3 oc = r.o - centre;
        const float bb = dot(oc, d), cc = dot(oc, oc) - kR * kR, disc = bb * bb - cc;
        const int i = y * kW + x;
        if (disc < 0.0f) {
          // A thin atmosphere just outside the limb.
          const float miss = sqrtf(-disc);
          const float glow = clampf(1.0f - miss / 0.12f, 0.0f, 1.0f);
          if (glow > 0.0f) c.pixel(i, 0.05f * glow, 0.18f * glow, 0.55f * glow * glow);
          continue;
        }
        const float t = -bb - sqrtf(disc);
        const V3 p = r.o + d * t;
        const V3 n = (p - centre) * (1.0f / kR);
        if (p.z < zMin) zMin = p.z;
        // Into the Earth's frame: the transpose of m.
        const V3 ne = v3(m.m[0][0] * n.x + m.m[1][0] * n.y + m.m[2][0] * n.z,
                         m.m[0][1] * n.x + m.m[1][1] * n.y + m.m[2][1] * n.z,
                         m.m[0][2] * n.x + m.m[1][2] * n.y + m.m[2][2] * n.z);
        const float lat = asinf(clampf(ne.y, -1.0f, 1.0f)) * (180.0f / kPi);
        const float lon = atan2f(ne.x, ne.z) * (180.0f / kPi);
        const float land = landAt(lat, lon);
        const float ice = lat > 72.0f || lat < -64.0f ? 1.0f : 0.0f;
        // Sun elevation here, and the world clock's twilight ramp.
        const float el = asinf(clampf(dot(ne, sun), -1.0f, 1.0f)) * (180.0f / kPi);
        const float day = clampf((el + 6.0f) / 6.0f, 0.0f, 1.0f);
        const float lit = 0.18f + 0.82f * clampf(sinf(el * kPi / 180.0f), 0.0f, 1.0f);
        Col k;
        k.r = mixf(0.015f, mixf(0.14f, 0.36f, clampf((30.0f - fabsf(lat)) / 20.0f, 0.0f, 1.0f)), land);
        k.g = mixf(0.05f, mixf(0.32f, 0.30f, clampf((30.0f - fabsf(lat)) / 20.0f, 0.0f, 1.0f)), land);
        k.b = mixf(0.20f, 0.08f, land);
        k.r = mixf(k.r, 0.85f, ice);
        k.g = mixf(k.g, 0.9f, ice);
        k.b = mixf(k.b, 0.97f, ice);
        // Night: the land a faint outline, the sea black.
        const float nr = 0.05f * land, ng = 0.055f * land, nb = 0.08f * land + 0.012f;
        float gl = 0.0f;   // the sun's glint on the sea
        if (land < 0.5f && day > 0.0f) {
          const V3 sc = apply(m, sun);
          const V3 hv = normalize(sc + normalize(view) * -1.0f);
          const float h = dot(n, hv);
          if (h > 0.0f) {
            float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
            gl = 0.5f * h16 * h8 * day;
          }
        }
        const float limb = 0.55f + 0.45f * clampf(-dot(n, d), 0.0f, 1.0f);
        c.pixel(i, limb * mixf(nr, k.r * lit, day) + gl, limb * mixf(ng, k.g * lit, day) + gl,
                limb * mixf(nb, k.b * lit, day) + gl);
      }
    c.note(zMin);
    // The cities, on the side facing us.
    for (int k = 0; k < (int)WORLD_CITY_COUNT; k++) {
      const float la = kWorldCities[k].lat * (kPi / 180.0f), lo = kWorldCities[k].lon * (kPi / 180.0f);
      const V3 ce = v3(cosf(la) * sinf(lo), sinf(la), cosf(la) * cosf(lo));
      const V3 cn = apply(m, ce);
      const V3 p = centre + cn * (kR * 1.01f);
      if (dot(cn, normalize(p - v3(e * 0.5f * v.b, 0.0f, 0.0f))) > -0.15f) continue;   // on the far side
      const bool night = dot(ce, sun) < 0.0f;
      const bool isHome = k == home_;
      const float pulse = isHome ? 0.6f + 0.4f * sinf(kTwoPi * t_ / 1.5f) : 1.0f;
      if (night) c.color(1.0f, 0.75f, 0.25f);
      else c.color(1.0f, 1.0f, 1.0f);
      c.dot(p, isHome ? 1.3f : 0.7f, pulse);
    }
  }

 private:
  static constexpr float kZ = 4.5f, kR = 1.0f, kZ0 = 4.5f, kZStars = 7.0f;
  static const int kStars = 60;
  float spin_, decl_, subLon_;
  int home_;
  bool started_ = false;
  float t_;
  V3 star_[kStars];

  static float cell(int row, int col) {
    if (row < 0 || row >= WORLD_ROWS) return 0.0f;
    col = ((col % WORLD_COLS) + WORLD_COLS) % WORLD_COLS;
    return (kWorldMask[row] >> col) & 1ULL ? 1.0f : 0.0f;
  }
  // The mask read bilinearly, then a soft threshold: coastlines without steps.
  static float landAt(float lat, float lon) {
    const float fr = (WORLD_TOP - lat) / ((WORLD_TOP - WORLD_BOTTOM) / WORLD_ROWS) - 0.5f;
    const float fc = (lon + 180.0f) / (360.0f / WORLD_COLS) - 0.5f;
    const int r0 = (int)floorf(fr), c0 = (int)floorf(fc);
    const float ar = fr - (float)r0, ac = fc - (float)c0;
    const float v = mixf(mixf(cell(r0, c0), cell(r0, c0 + 1), ac), mixf(cell(r0 + 1, c0), cell(r0 + 1, c0 + 1), ac), ar);
    return smoothstepf(0.3f, 0.7f, v);
  }
  static void starPoint(Ctx &c, float x, float y, float v) {
    const int ix = (int)floorf(x), iy = (int)floorf(y);
    if ((unsigned)ix >= (unsigned)kW || (unsigned)iy >= (unsigned)kH) return;
    c.pixel(iy * kW + ix, v, v, v);
  }
};

}  // namespace fx3d

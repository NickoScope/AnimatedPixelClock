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
  uint8_t mapHeight(int x, int y) const { return h_[(y & (kN - 1)) * kN + (x & (kN - 1))]; }   // for the host test
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
  // The march's steps, and the only place that knows them: where each one
  // is, f / z there, and the fog. With no arrays it only counts them, which
  // is how the host test holds the count inside kSteps; a march longer than
  // that would stop at the table's end and cut the horizon short.
  static int march(float f, float *z, float *fz, float *fog) {
    int n = 0;
    for (float t = kZNear, dz = 0.2f; t < kZFar; t += dz, dz *= 1.02f, n++)
      if (z && n < kSteps) {
        z[n] = t;
        fz[n] = f / t;
        fog[n] = smoothstepf(kZFar * 0.35f, kZFar, t);
      }
    return n < kSteps ? n : kSteps;
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
    // Every column marches the same steps, so the steps are a table: where
    // each one is, f / z there, and the fog. A division, floorf and ceilf are
    // calls on the S3 (README, "What floats cost on the S3"), and the march
    // made two floorf and a division at every step of every column, about 150
    // steps of 128 columns an eye. f / z instead of dividing (alt - h) * f by
    // z moves a slice's edge by a millionth of a pixel, so a slice can end a
    // row sooner or later where its edge sits on a pixel's.
    const int steps = march(v.f, stepZ_, stepFz_, stepFog_);
    // The sky by rows, once a frame; each slice's colour once, as bytes.
    uint8_t *rgb = c.fb.rgb, *plane = stereo ? c.plane() : nullptr;
    for (int yy = 0; yy < kH; yy++) {
      const float t = clampf((float)yy / kHorizon, 0.0f, 1.0f);
      pixelBytes(stereo, sky * mixf(hi.r, lo.r, t), sky * mixf(hi.g, lo.g, t), sky * mixf(hi.b, lo.b, t), skyRow_[yy]);
    }
    for (int x = 0; x < kW; x++) {
      const float lat = ((float)x - v.cx - shift) / v.f;
      const float dx = fx + rx * lat, dy = fy + ry * lat;
      int ybuf = kH;
      for (int s = 0; s < steps && ybuf > 0; s++) {
        const float z = stepZ_[s];
        const int mx = floorInt(ox + dx * z) & (kN - 1), my = floorInt(oy + dy * z) & (kN - 1);
        const int m = my * kN + mx;
        const float y = kHorizon + (alt_ - (float)h_[m]) * stepFz_[s];
        if (y < (float)ybuf) {
          int top = y > 0.0f ? ceilInt(y) : 0;
          const float fog = stepFog_[s];
          float r, g, b;
          if (stereo) {   // the shape, not the colours: relief light and height, fading into black
            const float lum = (float)l_[m] * (1.0f / 255.0f) * (0.35f + 0.65f * (float)h_[m] * (1.0f / 170.0f)) * (1.0f - fog);
            r = g = b = lum;
          } else {
            r = mixf(c_[3 * m] * (1.0f / 255.0f), lo.r, fog);
            g = mixf(c_[3 * m + 1] * (1.0f / 255.0f), lo.g, fog);
            b = mixf(c_[3 * m + 2] * (1.0f / 255.0f), lo.b, fog);
          }
          uint8_t px[3];
          pixelBytes(stereo, r, g, b, px);
          for (int yy = top; yy < ybuf; yy++) putBytes(stereo, rgb, plane, yy * kW + x, px);
          if (z < zMin) zMin = z;
          ybuf = top;
        }
      }
      for (int yy = 0; yy < ybuf; yy++) putBytes(stereo, rgb, plane, yy * kW + x, skyRow_[yy]);
    }
    c.note(zMin);
    c.note(kZFar);
  }

 private:
  static constexpr float kZ0 = 24.0f, kZNear = 2.0f, kZFar = 180.0f;
  static constexpr float kHorizon = 22.0f, kSpeed = 16.0f, kClearance = 16.0f;
 public:
  // The march's room: about 150 steps from kZNear to kZFar with dz growing
  // 2 % a step, and a margin. march(f, 0, 0, 0) counts what it really takes.
  static const int kSteps = 200;
 private:
  float stepZ_[kSteps], stepFz_[kSteps], stepFog_[kSteps];
  uint8_t skyRow_[kH][3];
  // Ctx::pixel's bytes, worked out once for a run of pixels of one colour.
  static void pixelBytes(bool stereo, float r, float g, float b, uint8_t *px) {
    if (stereo) {
      const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
      px[0] = (uint8_t)(clampf(m, 0.0f, 1.0f) * 255.0f + 0.5f);
      px[1] = px[2] = px[0];   // one byte is read in stereo; none is left unset
    } else {
      px[0] = (uint8_t)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
      px[1] = (uint8_t)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
      px[2] = (uint8_t)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  static void putBytes(bool stereo, uint8_t *rgb, uint8_t *plane, int i, const uint8_t *px) {
    if (stereo) {
      plane[i] = px[0];
    } else {
      rgb[3 * i] = px[0];
      rgb[3 * i + 1] = px[1];
      rgb[3 * i + 2] = px[2];
    }
  }
private:
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
  // Value noise on a tiling lattice, six octaves. Every octave's lattice is
  // hashed once (4 x 4 up to 128 x 128 values, 21,840 in all) and each texel
  // reads it: hashing all four corners per texel per octave, 1.57 million
  // hashes, made the map take 1.08 s to build on the panel (the integration
  // session's measurement, 2026-09-18). Same lattice values, same landscape.
  static const int kLattice = 4 * 4 + 8 * 8 + 16 * 16 + 32 * 32 + 64 * 64 + 128 * 128;
  float lat_[kLattice];
  void buildLattices(uint32_t seed) {
    int off = 0;
    for (int o = 0; o < 6; o++) {
      const int per = kN / (64 >> o);
      for (int gy = 0; gy < per; gy++)
        for (int gx = 0; gx < per; gx++)
          lat_[off + gy * per + gx] = (float)(hash3((uint32_t)gx, (uint32_t)gy, seed + (uint32_t)o) & 0xFFFF) / 65535.0f;
      off += per * per;
    }
  }
  // The map is built cell by cell, 65,536 of them, so what is a division here
  // is 65,536 calls to __divsf3 (69 cycles each on an S3; README, "What floats
  // cost on the S3"). Dividing by the cell's width is the same as multiplying
  // by its reciprocal to the last bit - the width is a power of two - so the
  // map is the one it was, which the host test checks cell by cell.
  static float invCell(int o) {
    static const float k[6] = {1.0f / 64.0f, 1.0f / 32.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f};
    return k[o];
  }
  float noise(int x, int y) const {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int off = 0;
    for (int o = 0; o < 6; o++) {
      const int cell = 64 >> o, per = kN / cell;
      const int gx = x / cell, gy = y / cell, gx1 = (gx + 1) % per, gy1 = (gy + 1) % per;
      const float ic = invCell(o);
      const float fx = (float)(x % cell) * ic, fy = (float)(y % cell) * ic;
      const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
      const float *l = lat_ + off;
      const float a = l[gy * per + gx], b = l[gy * per + gx1], c = l[gy1 * per + gx], d = l[gy1 * per + gx1];
      sum += amp * mixf(mixf(a, b, sx), mixf(c, d, sx), sy);
      norm += amp;
      amp *= 0.5f;
      off += per * per;
    }
    return sum / norm;
  }
  void generate(uint32_t seed) {
    const float water = 34.0f;
    buildLattices(seed);
    for (int y = 0; y < kN; y++)
      for (int x = 0; x < kN; x++) {
        float n = noise(x, y);
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
          const float t = (h - water) * (1.0f / (80.0f - 34.0f));   // water is 34
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
    // What a pixel can be is known before the loop: eight bands' colours and
    // five glows. palette() is three cosf, newlib's software cosine, and a
    // division is a call into ROM on the S3 (README, "What floats cost on the
    // S3"), so they are worked out here once a frame instead of per pixel.
    Col band[8];
    for (int b = 0; b < 8; b++) band[b] = palette(hue_ + (float)b / 8.0f);
    static const float kRing[3] = {1.0f - 0.0f / 3.0f, 1.0f - 1.0f / 3.0f, 1.0f - 2.0f / 3.0f};
    static const float kStripe[2] = {0.55f * (1.0f - 0.0f / 2.0f), 0.55f * (1.0f - 1.0f / 2.0f)};
    for (int i = 0; i < kPixels; i++) {
      const int u = (t.u[i] + du) & 255, v = (t.v[i] + dv) & 1023;
      // Rings every 64 texels along, eight stripes around; a glow falls off
      // from both.
      const int ru = u & 31, rv = v & 63;
      const int eu = ru < 16 ? ru : 32 - ru, ev = rv < 32 ? rv : 64 - rv;
      const float glow = base + (ev < 3 ? kRing[ev] : 0.0f) + (eu < 2 ? kStripe[eu] : 0.0f);
      const Col &k = band[(v >> 6) & 7];
      const float s = glow * (float)t.shade[i] * (1.0f / 255.0f);
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
  BlobsScene() {
    for (int i = 0; i < kBalls; i++) ph_[i][0] = ph_[i][1] = ph_[i][2] = 0.0f;
    tables();
  }
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
  void reset(uint32_t) {
    for (int i = 0; i < kBalls; i++) {
      ph_[i][0] = 1.7f * i;
      ph_[i][1] = 0.9f * i;
      ph_[i][2] = 2.3f * i;
    }
    step(0.0f, Env());
  }
  // Each orbit keeps its own wrapped phase: the frequencies share no period,
  // so a wrapped time would jump.
  void step(float dt, const Env &) {
    for (int i = 0; i < kBalls; i++) {
      ph_[i][0] = wrapAngle(ph_[i][0] + (0.5f + 0.13f * i) * dt);
      ph_[i][1] = wrapAngle(ph_[i][1] + (0.37f + 0.11f * i) * dt);
      ph_[i][2] = wrapAngle(ph_[i][2] + (0.29f + 0.07f * i) * dt);
      pos_[i] = v3(1.05f * sinf(ph_[i][0]), 0.5f * sinf(ph_[i][1]), 0.6f * sinf(ph_[i][2])) + v3(0.0f, 0.0f, kZ);
    }
  }
  // One ray per 2 x 2 block, 64 x 32 of them, and the panel filled by
  // bilinear interpolation between them. At a ray per pixel the panel took
  // 111.8 ms a frame in mono and 217.8 ms in red-blue (the integration
  // session's measurement, 2026-09-18); the blobs are smooth enough for this.
  // Where each pixel falls between the rays is the same every frame, so it
  // is a table (floorf is newlib's, 53 instructions of bit work, on the S3).
  void draw(Ctx &c) {
    float zMin = 1.0e9f, zMax = 0.0f;
    for (int j = 0; j < kHalfH; j++)
      for (int i = 0; i < kHalfW; i++) shadeAt(c, 2.0f * (float)i + 0.5f, 2.0f * (float)j + 0.5f, half_[j][i], zMin, zMax);
    for (int y = 0; y < kH; y++) {
      const int j0 = row0_[y];
      const float b = rowT_[y];
      for (int x = 0; x < kW; x++) {
        const int i0 = col0_[x];
        const float a = colT_[x];
        float rgb[3];
        for (int ch = 0; ch < 3; ch++)
          rgb[ch] = mixf(mixf(half_[j0][i0][ch], half_[j0][i0 + 1][ch], a), mixf(half_[j0 + 1][i0][ch], half_[j0 + 1][i0 + 1][ch], a), b);
        c.pixel(y * kW + x, rgb[0], rgb[1], rgb[2]);
      }
    }
    if (zMin <= zMax) {
      c.note(zMin);
      c.note(zMax);
    }
  }

 private:
  static constexpr float kZ = 6.0f, kBound = 2.2f, kK = 0.45f, kInvK = 1.0f / kK;
  static const int kHalfW = kW / 2, kHalfH = kH / 2;
  float half_[kHalfH][kHalfW][3];   // the marched samples, one per 2 x 2 block
  // For each panel column and row: the ray before it and how far past it.
  uint8_t col0_[kW], row0_[kH];
  float colT_[kW], rowT_[kH];
  void tables() {
    for (int x = 0; x < kW; x++) {
      const float u = ((float)x - 0.5f) * 0.5f;
      int i0 = (int)floorf(u);
      float a = u - (float)i0;
      if (i0 < 0) { i0 = 0; a = 0.0f; }
      if (i0 > kHalfW - 2) { i0 = kHalfW - 2; a = 1.0f; }
      col0_[x] = (uint8_t)i0;
      colT_[x] = a;
    }
    for (int y = 0; y < kH; y++) {
      const float v = ((float)y - 0.5f) * 0.5f;
      int j0 = (int)floorf(v);
      float b = v - (float)j0;
      if (j0 < 0) { j0 = 0; b = 0.0f; }
      if (j0 > kHalfH - 2) { j0 = kHalfH - 2; b = 1.0f; }
      row0_[y] = (uint8_t)j0;
      rowT_[y] = b;
    }
  }
  static float radius(int k) { return 0.62f - 0.057f * (float)k; }

  // One ray: the bounding sphere, then sphere tracing (at most 28 steps), then
  // the light. Writes linear rgb.
  void shadeAt(const Ctx &c, float sx, float sy, float *out, float &zMin, float &zMax) const {
    const V3 centre = v3(0.0f, 0.0f, kZ);
    const Light light;
    const Col tint[kBalls] = {{1.0f, 0.25f, 0.2f}, {0.2f, 0.55f, 1.0f}, {0.25f, 1.0f, 0.45f}, {1.0f, 0.8f, 0.2f}};
    const Ray r = eyeRay(c.view, c.eye, sx, sy);
    const V3 d = normalizeFast(r.d);
    const float bg = c.stereo() ? 0.0f : 0.004f + 0.012f * sy / kH;
    out[0] = bg * 0.4f;
    out[1] = bg * 0.5f;
    out[2] = bg;
    const V3 oc = r.o - centre;
    const float bb = dot(oc, d), cc = dot(oc, oc) - kBound * kBound, disc = bb * bb - cc;
    if (disc <= 0.0f) return;
    const float sq = sqrtf(disc);
    float t = -bb - sq;
    const float tEnd = -bb + sq;
    bool hit = false;
    V3 p = r.o;
    for (int s = 0; s < 28 && t < tEnd; s++) {
      p = r.o + d * t;
      const float dist = sdf(p);
      if (dist < 0.006f) {
        hit = true;
        break;
      }
      t += dist;
    }
    if (!hit) return;
    const V3 n = normal(p);
    if (p.z < zMin) zMin = p.z;
    if (p.z > zMax) zMax = p.z;
    float wr = 0.0f, wg = 0.0f, wb = 0.0f, wsum = 0.0f;
    for (int k = 0; k < kBalls; k++) {
      const float dd = lengthFast(p - pos_[k]) - radius(k);
      const float w = 1.0f / (0.02f + dd * dd);
      wr += w * tint[k].r;
      wg += w * tint[k].g;
      wb += w * tint[k].b;
      wsum += w;
    }
    const float sh = light.shade(n, 0.1f, 0.0f);
    const float h = dot(n, light.half);
    float spec = 0.0f;
    if (h > 0.0f) {
      const float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
      spec = 0.8f * h16 * h16;
    }
    const float rim = 1.0f + dot(n, d);   // grazing: 1, facing: 0
    const float rim3 = 0.35f * rim * rim * rim;
    const float iw = sh / wsum;
    out[0] = wr * iw + spec + rim3 * 0.3f;
    out[1] = wg * iw + spec + rim3 * 0.5f;
    out[2] = wb * iw + spec + rim3;
  }
  float ph_[kBalls][3];
  V3 pos_[kBalls];
  // Up to 32 of these a ray (the march and the normal): each a square root
  // for every ball and a blend between them, so no sqrtf and no division here.
  static float smin(float a, float b) {
    const float h = clampf(0.5f + 0.5f * (b - a) * kInvK, 0.0f, 1.0f);
    return mixf(b, a, h) - kK * h * (1.0f - h);
  }
  // The blend reaches kK, and smin(a, b) is exactly a once b - a is that far,
  // so a ball further than that cannot change the answer. Squared distances
  // cost no square root, so they are worked out first and only the balls that
  // can take part are rooted: the same number to the last bit, the margin
  // over kK only to stay clear of the reciprocal's own rounding.
  float sdf(V3 p) const {
    float s[kBalls];
    for (int k = 0; k < kBalls; k++) {
      const V3 q = p - pos_[k];
      s[k] = dot(q, q);
    }
    float d = sqrtFast(s[0]) - radius(0);
    for (int k = 1; k < kBalls; k++) {
      const float reach = d + kK * 1.0001f + radius(k);
      if (reach <= 0.0f || s[k] >= reach * reach) continue;
      d = smin(d, sqrtFast(s[k]) - radius(k));
    }
    return d;
  }
  V3 normal(V3 p) const {
    const float h = 0.002f;
    const V3 k1 = v3(1, -1, -1), k2 = v3(-1, -1, 1), k3 = v3(-1, 1, -1), k4 = v3(1, 1, 1);
    return normalizeFast(k1 * sdf(p + k1 * h) + k2 * sdf(p + k2 * h) + k3 * sdf(p + k3 * h) + k4 * sdf(p + k4 * h));
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
  GlobeScene() : spin_(0.0f), decl_(0.0f), subLon_(0.0f), home_(-1), t_(0.0f) {
    for (int k = 0; k < 3; k++) tab_[k].valid = false;
  }
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
    for (int k = 0; k < 3; k++) tab_[k].valid = false;
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
  // Only two things change from frame to frame: the Earth's spin and where
  // the sun is. Everything a pixel needs besides those - where its ray meets
  // the globe, the latitude there, the longitude before the spin, how the
  // limb dims it, the atmosphere just outside - is the same until the view
  // changes, so it is a table per eye. What is left in the frame is
  // multiplications: the per-pixel asinf, atan2f, sinf, the three square
  // roots and the six divisions are all calls on the S3 (README, "What
  // floats cost on the S3").
  void draw(Ctx &c) {
    const View &v = c.view;
    Tab &t = tab_[c.eye + 1];
    if (!t.valid || t.f != v.f || t.b != v.b || t.z0 != v.z0 || t.cx != v.cx || t.cy != v.cy) build(t, v, c.eye);
    const float e = (float)c.eye;
    const float shift = e * v.f * v.b / (2.0f * v.z0);
    // Stars, far behind the globe: at depth kZStars for this eye.
    c.clear();
    const float starShift = e * 0.5f * v.b * v.f / kZStars - shift;   // where depth kZStars lands for this eye
    for (int k = 0; k < kStars; k++) starPoint(c, star_[k].x - starShift, star_[k].y, star_[k].z);
    c.note(kZStars);
    // The globe: tilted 23.4 degrees for the look, spun about its axis.
    const M3 m = mul(rotZ(0.41f), rotY(spin_));   // Earth frame -> camera
    const V3 sun = v3(cosf(decl_) * sinf(subLon_), sinf(decl_), cosf(decl_) * cosf(subLon_));
    const V3 centre = v3(0.0f, 0.0f, kZ);
    const V3 view = v3(-0.35f, 0.25f, -1.0f);   // for the glint
    // The table holds q, the normal in the Earth's frame before the spin.
    // dot(ne, sun) is then dot(q, rotY(spin) sun), and the glint's dot(n, hv)
    // is dot(q, rotZ(-0.41) hv): both vectors are the frame's, not a pixel's.
    const V3 sunQ = apply(rotY(spin_), sun);
    const V3 hvQ = apply(rotZ(-0.41f), normalize(apply(m, sun) + normalize(view) * -1.0f));
    const float spinDeg = spin_ * (180.0f / kPi);
    for (int i = 0; i < kPixels; i++) {
      const Px &px = t.px[i];
      if (!px.limb) {   // the ray missed: a thin atmosphere just outside the limb
        if (px.glow) {
          const float glow = (float)px.glow * (1.0f / 255.0f);
          c.pixel(i, 0.05f * glow, 0.18f * glow, 0.55f * glow * glow);
        }
        continue;
      }
      const V3 q = v3((float)px.q[0] * kQ, (float)px.q[1] * kQ, (float)px.q[2] * kQ);
      const float lat = (float)px.lat * 0.01f;
      float lon = (float)px.lon * 0.01f - spinDeg;
      while (lon < -180.0f) lon += 360.0f;
      while (lon > 180.0f) lon -= 360.0f;
      const float land = landAt(lat, lon);
      const float ice = lat > 72.0f || lat < -64.0f ? 1.0f : 0.0f;
      // The sine of the sun's elevation here, and the world clock's twilight
      // ramp. sin(asin(x)) is x, so the elevation itself is only needed
      // between -6 degrees and 0, where a cubic and a fifth term stand in for
      // asinf (their error below |x| = 0.11 is under a hundred millionth).
      const float ds = dot(q, sunQ);
      float day;
      if (ds >= 0.0f) {
        day = 1.0f;
      } else if (ds <= kSinDusk) {
        day = 0.0f;
      } else {
        const float x2 = ds * ds;
        const float el = (ds + ds * x2 * (1.0f / 6.0f + x2 * (3.0f / 40.0f))) * (180.0f / kPi);
        day = clampf((el + 6.0f) * (1.0f / 6.0f), 0.0f, 1.0f);
      }
      const float lit = 0.18f + 0.82f * clampf(ds, 0.0f, 1.0f);
      const float warm = clampf((30.0f - fabsf(lat)) * (1.0f / 20.0f), 0.0f, 1.0f);
      Col k;
      k.r = mixf(0.015f, mixf(0.14f, 0.36f, warm), land);
      k.g = mixf(0.05f, mixf(0.32f, 0.30f, warm), land);
      k.b = mixf(0.20f, 0.08f, land);
      k.r = mixf(k.r, 0.85f, ice);
      k.g = mixf(k.g, 0.9f, ice);
      k.b = mixf(k.b, 0.97f, ice);
      // Night: the land a faint outline, the sea black.
      const float nr = 0.05f * land, ng = 0.055f * land, nb = 0.08f * land + 0.012f;
      float gl = 0.0f;   // the sun's glint on the sea
      if (land < 0.5f && day > 0.0f) {
        const float h = dot(q, hvQ);
        if (h > 0.0f) {
          const float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
          gl = 0.5f * h16 * h8 * day;
        }
      }
      const float limb = (float)px.limb * (1.0f / 255.0f);
      c.pixel(i, limb * mixf(nr, k.r * lit, day) + gl, limb * mixf(ng, k.g * lit, day) + gl,
              limb * mixf(nb, k.b * lit, day) + gl);
    }
    c.note(t.zMin);
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
  static constexpr float kQ = 1.0f / 32767.0f;
  static constexpr float kSinDusk = -0.104528463f;   // sin(-6 degrees): the world clock's dusk
  static const int kStars = 60;
  // What a pixel's ray finds, until the view changes: the normal in the
  // Earth's frame before the spin, where that is on the map, and the light
  // the limb leaves. limb is never below 0.55 of full, so 0 marks a miss.
  struct Px {
    int16_t q[3];       // the normal before the spin, x 32767
    int16_t lat, lon;   // hundredths of a degree; lon before the spin
    uint8_t limb;       // 0.55 + 0.45 |cos| as a code; 0: the ray missed
    uint8_t glow;       // the atmosphere just outside the limb, as a code
  };
  struct Tab {
    bool valid;
    float f, b, z0, cx, cy, zMin;
    Px px[kPixels];
  };
  Tab tab_[3];   // left, mono, right

  static void build(Tab &t, const View &v, int eye) {
    const V3 centre = v3(0.0f, 0.0f, kZ);
    const M3 back = rotZ(-0.41f);   // camera frame -> the Earth's, before the spin
    t.zMin = kZ;
    for (int y = 0; y < kH; y++)
      for (int x = 0; x < kW; x++) {
        Px &px = t.px[y * kW + x];
        px.limb = 0;
        px.glow = 0;
        px.q[0] = px.q[1] = px.q[2] = 0;
        px.lat = px.lon = 0;
        const Ray r = eyeRay(v, eye, (float)x, (float)y);
        const V3 d = normalize(r.d);
        const V3 oc = r.o - centre;
        const float bb = dot(oc, d), cc = dot(oc, oc) - kR * kR, disc = bb * bb - cc;
        if (disc < 0.0f) {
          // The old globe painted this pixel for any glow above zero, black
          // included, which puts out a star behind the limb; a glow that
          // rounds to nothing keeps the 1 so that still happens.
          const float miss = sqrtf(-disc);
          const float glow = clampf(1.0f - miss / 0.12f, 0.0f, 1.0f);
          px.glow = (uint8_t)(glow * 255.0f + 0.5f);
          if (!px.glow && glow > 0.0f) px.glow = 1;
          continue;
        }
        const V3 p = r.o + d * (-bb - sqrtf(disc));
        const V3 n = (p - centre) * (1.0f / kR);
        if (p.z < t.zMin) t.zMin = p.z;
        const V3 q = apply(back, n);
        px.q[0] = (int16_t)(clampf(q.x, -1.0f, 1.0f) * 32767.0f + (q.x < 0.0f ? -0.5f : 0.5f));
        px.q[1] = (int16_t)(clampf(q.y, -1.0f, 1.0f) * 32767.0f + (q.y < 0.0f ? -0.5f : 0.5f));
        px.q[2] = (int16_t)(clampf(q.z, -1.0f, 1.0f) * 32767.0f + (q.z < 0.0f ? -0.5f : 0.5f));
        const float lat = asinf(clampf(q.y, -1.0f, 1.0f)) * (180.0f / kPi);
        const float lon = atan2f(q.x, q.z) * (180.0f / kPi);
        px.lat = (int16_t)(lat * 100.0f + (lat < 0.0f ? -0.5f : 0.5f));
        px.lon = (int16_t)(lon * 100.0f + (lon < 0.0f ? -0.5f : 0.5f));
        px.limb = (uint8_t)((0.55f + 0.45f * clampf(-dot(n, d), 0.0f, 1.0f)) * 255.0f + 0.5f);
      }
    t.f = v.f;
    t.b = v.b;
    t.z0 = v.z0;
    t.cx = v.cx;
    t.cy = v.cy;
    t.valid = true;
  }
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
  // The mask read bilinearly, then a soft threshold: coastlines without
  // steps. Once per pixel of the globe, so the two divisions, the two floorf
  // and smoothstepf's own division are all multiplications and casts here.
  static float landAt(float lat, float lon) {
    const float fr = (WORLD_TOP - lat) * ((float)WORLD_ROWS / (WORLD_TOP - WORLD_BOTTOM)) - 0.5f;
    const float fc = (lon + 180.0f) * ((float)WORLD_COLS / 360.0f) - 0.5f;
    const int r0 = floorInt(fr), c0 = floorInt(fc);
    const float ar = fr - (float)r0, ac = fc - (float)c0;
    const float v = mixf(mixf(cell(r0, c0), cell(r0, c0 + 1), ac), mixf(cell(r0 + 1, c0), cell(r0 + 1, c0 + 1), ac), ar);
    const float t = clampf((v - 0.3f) * 2.5f, 0.0f, 1.0f);   // smoothstepf(0.3, 0.7, v), without its divide
    return t * t * (3.0f - 2.0f * t);
  }
  static void starPoint(Ctx &c, float x, float y, float v) {
    const int ix = floorInt(x), iy = floorInt(y);
    if ((unsigned)ix >= (unsigned)kW || (unsigned)iy >= (unsigned)kH) return;
    c.pixel(iy * kW + ix, v, v, v);
  }
};

}  // namespace fx3d

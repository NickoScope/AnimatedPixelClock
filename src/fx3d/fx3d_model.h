#pragma once
// The 3D effects' arithmetic: vectors, the mono and stereo projections, a
// rasteriser into linear-light buffers, the anaglyph compositor, and the encode
// into the codes the HUB75 library expects.
//
// Plain C++11, float only, no Arduino and no allocation: every buffer is handed
// in. tools/fx3d/check_fx3d.py tests it on the Mac, and the previews are
// rendered by this same code. Why each rule is what it is: docs/27-fx3d.md in
// the knowledge base.
//
// Conventions
// - Camera space: x right, y up, z forward (into the panel). A point is drawn
//   only at z >= View::nearZ; segments and triangles are clipped there before
//   the perspective divide.
// - Screen: 128 x 64, pixel centres at integer coordinates, y down. The
//   principal point is (63.5, 31.5), the seam between the two panels.
// - Light is linear inside: buffers hold 0..255 of the panel's full output.
//   Encoder turns that into the CIE 1931 lightness code that
//   drawPixelRGB888() expects, once, at the very end.

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace fx3d {

const int kW = 128, kH = 64, kPixels = kW * kH;
const float kPi = 3.14159265f;
const float kTwoPi = 6.28318531f;

// ---------------------------------------------------------------- vectors ----

struct V3 {
  float x, y, z;
};
inline V3 v3(float x, float y, float z) {
  V3 r;
  r.x = x;
  r.y = y;
  r.z = z;
  return r;
}
inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float length(V3 a) { return sqrtf(dot(a, a)); }
inline V3 normalize(V3 a) {
  float l = length(a);
  return l > 0.0f ? a * (1.0f / l) : a;
}

// Square roots for the hot loops, without sqrtf or a division. On the S3
// neither is inline: sqrtf is newlib's errno wrapper calling __ieee754_sqrtf,
// about 30 FPU instructions around the sqrt0.s estimate, and a division calls
// __divsf3 in ROM, about 28 around div0.s (the firmware's and the ROM's
// disassembly; src/fx3d/README.md, "What floats cost on the S3"). This is a
// dozen inline adds and multiplies: the bit-level first guess with Lomont's
// constant 0x5f375a86 (Chris Lomont, "Fast Inverse Square Root", 2003) and
// two Newton steps. Its worst
// relative error, measured by the host test over the floats from 1e-6 to
// 1e6, is printed there and held under 5e-6. The shared length() and
// normalize() keep sqrtf, so nothing that was exact moves.
inline float rsqrtFast(float x) {   // x > 0
  uint32_t i;
  memcpy(&i, &x, sizeof i);
  i = 0x5f375a86u - (i >> 1);
  float y;
  memcpy(&y, &i, sizeof y);
  const float hx = 0.5f * x;
  y = y * (1.5f - hx * y * y);
  y = y * (1.5f - hx * y * y);
  return y;
}
inline float sqrtFast(float x) { return x > 0.0f ? x * rsqrtFast(x) : 0.0f; }
// floorf and ceilf as ints, inline: on the S3 each is a call to 53
// instructions of bit work (README). Exact for |v| < 2^31, not NaN; the
// callers keep to that.
inline int floorInt(float v) {
  const int i = (int)v;
  return (float)i > v ? i - 1 : i;
}
inline int ceilInt(float v) {
  const int i = (int)v;
  return (float)i < v ? i + 1 : i;
}
inline float lengthFast(V3 a) { return sqrtFast(dot(a, a)); }
inline V3 normalizeFast(V3 a) {
  const float d = dot(a, a);
  return d > 0.0f ? a * rsqrtFast(d) : a;
}
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstepf(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
// An angle kept in [0, 2pi): phases are wrapped as they advance, so a scene that
// runs for weeks keeps its float precision.
inline float wrapAngle(float a) {
  a = fmodf(a, kTwoPi);
  return a < 0.0f ? a + kTwoPi : a;
}
inline void swapf(float &a, float &b) {
  float t = a;
  a = b;
  b = t;
}

// Row-major 3x3: apply(m, v) = m * v.
struct M3 {
  float m[3][3];
};
inline M3 identity3() {
  M3 r;
  memset(&r, 0, sizeof r);
  r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
  return r;
}
inline V3 apply(const M3 &a, V3 v) {
  return v3(a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z);
}
inline M3 mul(const M3 &a, const M3 &b) {
  M3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
  return r;
}
inline M3 rotX(float a) {
  M3 r = identity3();
  float c = cosf(a), s = sinf(a);
  r.m[1][1] = c; r.m[1][2] = -s;
  r.m[2][1] = s; r.m[2][2] = c;
  return r;
}
inline M3 rotY(float a) {
  M3 r = identity3();
  float c = cosf(a), s = sinf(a);
  r.m[0][0] = c; r.m[0][2] = s;
  r.m[2][0] = -s; r.m[2][2] = c;
  return r;
}
inline M3 rotZ(float a) {
  M3 r = identity3();
  float c = cosf(a), s = sinf(a);
  r.m[0][0] = c; r.m[0][1] = -s;
  r.m[1][0] = s; r.m[1][1] = c;
  return r;
}

// xorshift32: the same seed gives the same scene on the panel and on the Mac.
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float uniform() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
  float range(float a, float b) { return a + (b - a) * uniform(); }
};

// ------------------------------------------------------------- projection ----

// Mono (eye 0) or off-axis stereo (eye -1 left, +1 right): parallel cameras at
// x = eye * b / 2, the image shifted so that depth z0 has zero parallax
// (docs/27, the brief's section 4):
//   x_eye = cx + f * (X - eye*b/2) / Z + eye * f*b / (2*z0),   y = cy - f*Y/Z.
// There is no vertical parallax by construction: y does not depend on the eye.
struct View {
  float f;      // focal length, pixels
  float cx, cy; // principal point
  float nearZ;  // nothing nearer is drawn
  float z0;     // zero-parallax depth
  float b;      // baseline, scene units; 0 = no parallax at all
  View() : f(64.0f), cx(63.5f), cy(31.5f), nearZ(0.1f), z0(4.0f), b(0.0f) {}

  // Only for z >= nearZ: callers clip first.
  void project(V3 p, int eye, float &sx, float &sy) const {
    const float inv = 1.0f / p.z;
    const float e = (float)eye;
    sx = cx + f * (p.x - e * 0.5f * b) * inv + e * f * b / (2.0f * z0);
    sy = cy - f * p.y * inv;
  }
  // x_left - x_right for a point at depth z: positive in front of z0.
  float disparity(float z) const { return f * b * (1.0f / z - 1.0f / z0); }
};

// The baseline that keeps |x_left - x_right| <= dMax pixels for every depth in
// [zNear, zFar]. 1/z is monotonic, so the extremes are at the ends of the range.
// The brief: limit parallax through the baseline, never by clamping vertices.
inline float baselineFor(float f, float z0, float zNear, float zFar, float dMax) {
  if (!(zNear > 0.0f) || !(zFar >= zNear) || !(f > 0.0f) || !(dMax > 0.0f)) return 0.0f;
  float a = fabsf(1.0f / zNear - 1.0f / z0), c = fabsf(1.0f / zFar - 1.0f / z0);
  float m = a > c ? a : c;
  return m > 0.0f ? dMax / (f * m) : 0.0f;
}

// Clips segment a-b (with a value carried along) to z >= zn. False when all of
// it is behind the plane. Done before the divide, so no infinity is made.
inline bool clipNear(V3 &a, V3 &b, float &va, float &vb, float zn) {
  if (a.z < zn && b.z < zn) return false;
  if (a.z < zn) {
    float t = (zn - a.z) / (b.z - a.z);
    a = lerp(a, b, t);
    va = mixf(va, vb, t);
    a.z = zn;
  } else if (b.z < zn) {
    float t = (zn - b.z) / (a.z - b.z);
    b = lerp(b, a, t);
    vb = mixf(vb, va, t);
    b.z = zn;
  }
  return true;
}

// ----------------------------------------------------------------- raster ----

// Additive writers into linear buffers, saturating at 255. Out-of-range
// coordinates and non-positive or NaN amounts are dropped here, so no primitive
// can write outside a buffer whatever it is given.
struct PlotGray {
  uint8_t *px;
  float gain;   // 255 * intensity scale
  void operator()(int x, int y, float v) const {
    if ((unsigned)x >= (unsigned)kW || (unsigned)y >= (unsigned)kH || !(v > 0.0f)) return;
    uint8_t &d = px[y * kW + x];
    float s = (float)d + v * gain + 0.5f;
    d = s >= 255.0f ? 255 : (uint8_t)s;
  }
};
struct PlotRgb {
  uint8_t *px;
  float r, g, b;   // 255 * colour
  void operator()(int x, int y, float v) const {
    if ((unsigned)x >= (unsigned)kW || (unsigned)y >= (unsigned)kH || !(v > 0.0f)) return;
    uint8_t *d = px + 3 * (y * kW + x);
    float c[3] = {r, g, b};
    for (int k = 0; k < 3; k++) {
      float s = (float)d[k] + v * c[k] + 0.5f;
      d[k] = s >= 255.0f ? 255 : (uint8_t)s;
    }
  }
};

// A point at a sub-pixel position, split over its four neighbours. Sub-pixel
// positions are what let a disparity smaller than a pixel still read as depth.
template <class P>
void splat(const P &plot, float x, float y, float v) {
  if (!(x > -1.0f && x < (float)kW && y > -1.0f && y < (float)kH)) return;
  float fx = floorf(x), fy = floorf(y);
  int ix = (int)fx, iy = (int)fy;
  float ax = x - fx, ay = y - fy;
  plot(ix, iy, v * (1.0f - ax) * (1.0f - ay));
  plot(ix + 1, iy, v * ax * (1.0f - ay));
  plot(ix, iy + 1, v * (1.0f - ax) * ay);
  plot(ix + 1, iy + 1, v * ax * ay);
}

// A round dot of radius r, its edge anti-aliased over one pixel.
template <class P>
void disc(const P &plot, float x, float y, float r, float v) {
  if (!(r > 0.0f)) return;
  if (r > (float)kW) r = (float)kW;   // bigger than the panel is the panel
  if (!(x > -r - 1.0f && x < kW + r && y > -r - 1.0f && y < kH + r)) return;
  // The box is clipped to the screen while still a float: a huge radius must
  // never reach an int conversion.
  int x0 = (int)clampf(floorf(x - r - 0.5f), -1.0f, (float)kW), x1 = (int)clampf(ceilf(x + r + 0.5f), -1.0f, (float)kW);
  int y0 = (int)clampf(floorf(y - r - 0.5f), -1.0f, (float)kH), y1 = (int)clampf(ceilf(y + r + 0.5f), -1.0f, (float)kH);
  for (int j = y0; j <= y1; j++)
    for (int i = x0; i <= x1; i++) {
      float dx = i - x, dy = j - y;
      float c = clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
      if (c > 0.0f) plot(i, j, v * c);
    }
}

// Liang-Barsky against [xmin, xmax] x [ymin, ymax], carrying a value along.
// Rejects non-finite input, so nothing after it sees a NaN.
inline bool clip2(float &x0, float &y0, float &x1, float &y1, float &v0, float &v1,
                  float xmin, float ymin, float xmax, float ymax) {
  const float big = 1.0e6f;
  if (!(fabsf(x0) < big && fabsf(y0) < big && fabsf(x1) < big && fabsf(y1) < big)) return false;
  float dx = x1 - x0, dy = y1 - y0, t0 = 0.0f, t1 = 1.0f;
  float p[4] = {-dx, dx, -dy, dy};
  float q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
  for (int k = 0; k < 4; k++) {
    if (p[k] == 0.0f) {
      if (q[k] < 0.0f) return false;
    } else {
      float t = q[k] / p[k];
      if (p[k] < 0.0f) {
        if (t > t1) return false;
        if (t > t0) t0 = t;
      } else {
        if (t < t0) return false;
        if (t < t1) t1 = t;
      }
    }
  }
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  float nv0 = mixf(v0, v1, t0), nv1 = mixf(v0, v1, t1);
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1; v0 = nv0; v1 = nv1;
  return true;
}

// Xiaolin Wu's anti-aliased line with fractional end points: each column along
// the major axis gets the length of line over it, split between the two pixels
// astride the line. The sqrt(1 + slope^2) factor gives every angle the same
// light per unit length. The value is interpolated from v0 to v1 (depth cue).
template <class P>
void line(const P &plot, float x0, float y0, float x1, float y1, float v0, float v1) {
  if (!clip2(x0, y0, x1, y1, v0, v1, -1.0f, -1.0f, (float)kW, (float)kH)) return;
  const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
  if (steep) {
    swapf(x0, y0);
    swapf(x1, y1);
  }
  if (x0 > x1) {
    swapf(x0, x1);
    swapf(y0, y1);
    swapf(v0, v1);
  }
  const float dx = x1 - x0, dy = y1 - y0;
  const float grad = dx > 1.0e-6f ? dy / dx : 0.0f;
  const float dv = dx > 1.0e-6f ? (v1 - v0) / dx : 0.0f;
  const float comp = sqrtf(1.0f + grad * grad);
  const int xa = (int)floorf(x0 + 0.5f), xb = (int)floorf(x1 + 0.5f);
  for (int x = xa; x <= xb; x++) {
    float lo = (float)x - 0.5f > x0 ? (float)x - 0.5f : x0;
    float hi = (float)x + 0.5f < x1 ? (float)x + 0.5f : x1;
    float cover = hi - lo;
    if (cover <= 0.0f) {
      if (xa != xb) continue;
      cover = 1.0e-3f;   // a zero-length line still marks its pixel faintly
    }
    float xm = 0.5f * (lo + hi);
    float y = y0 + grad * (xm - x0);
    float v = (v0 + dv * (xm - x0)) * cover * comp;
    float fy = floorf(y);
    int iy = (int)fy;
    float a = y - fy;
    if (steep) {
      plot(iy, x, v * (1.0f - a));
      plot(iy + 1, x, v * a);
    } else {
      plot(x, iy, v * (1.0f - a));
      plot(x, iy + 1, v * a);
    }
  }
}

// A projected vertex for the triangle rasteriser: screen position, w = 1/z
// (interpolates linearly on screen, larger is nearer) and a shade.
struct SV {
  float x, y, w, s;
};

// Fills the pixels whose centres lie inside a-b-c, nearer than zbuf, and calls
// frag(index, shade) for each. Vertices are snapped to 1/16 pixel and the edge
// functions are integers with the top-left rule, so two triangles sharing an
// edge cover every pixel along it exactly once: no holes, no seams. A triangle
// is front-facing when it runs counter-clockwise on screen with y up; with
// cullBack the others are skipped. Triangles reaching past +-1024 pixels are
// dropped: keep nearZ large enough that the scene never needs them.
template <class F>
void triangle(SV a, SV b, SV c, float *zbuf, F &frag, bool cullBack) {
  const float guard = 1024.0f;
  if (!(fabsf(a.x) < guard && fabsf(a.y) < guard && fabsf(b.x) < guard && fabsf(b.y) < guard &&
        fabsf(c.x) < guard && fabsf(c.y) < guard))
    return;
  int32_t ax = (int32_t)lrintf(a.x * 16.0f), ay = (int32_t)lrintf(a.y * 16.0f);
  int32_t bx = (int32_t)lrintf(b.x * 16.0f), by = (int32_t)lrintf(b.y * 16.0f);
  int32_t cx = (int32_t)lrintf(c.x * 16.0f), cy = (int32_t)lrintf(c.y * 16.0f);
  int32_t area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (area == 0) return;
  if (area > 0) {                 // clockwise with y up: the back face
    if (cullBack) return;
  } else {
    int32_t t;
    t = bx; bx = cx; cx = t;
    t = by; by = cy; cy = t;
    SV s = b; b = c; c = s;
    area = -area;
  }
  // Bounding box of pixel centres, clipped to the screen.
  int32_t minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
  int32_t maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
  int32_t miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
  int32_t maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
  int x0 = (int)((minx + 15) >> 4), x1 = (int)(maxx >> 4);
  int y0 = (int)((miny + 15) >> 4), y1 = (int)(maxy >> 4);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > kW - 1) x1 = kW - 1;
  if (y1 > kH - 1) y1 = kH - 1;
  if (x0 > x1 || y0 > y1) return;
  // Edge functions E(p, q, r) = (q.x - p.x)(r.y - p.y) - (q.y - p.y)(r.x - p.x),
  // positive inside now that the area is positive. Top-left edges keep the
  // centre that lies exactly on them, the others do not (a bias of one).
  struct Edge {
    int32_t dx, dy, bias;
  };
  Edge e[3] = {{cx - bx, cy - by, 0}, {ax - cx, ay - cy, 0}, {bx - ax, by - ay, 0}};
  const int32_t ox[3] = {bx, cx, ax}, oy[3] = {by, cy, ay};
  int32_t row[3];
  const int32_t px0 = x0 << 4, py0 = y0 << 4;
  for (int k = 0; k < 3; k++) {
    bool topLeft = e[k].dy < 0 || (e[k].dy == 0 && e[k].dx > 0);
    e[k].bias = topLeft ? 0 : 1;
    row[k] = e[k].dx * (py0 - oy[k]) - e[k].dy * (px0 - ox[k]);
  }
  // w and s are affine on screen: their plane equations from the three
  // edge weights, stepped per pixel.
  const float inv = 1.0f / (float)area;
  const float wa = a.w, wb = b.w, wc = c.w, sa = a.s, sb = b.s, sc = c.s;
  // Weight of a vertex = the edge opposite it: e[0] is a's, e[1] b's, e[2] c's.
  const float stepX[3] = {(float)(-e[0].dy * 16), (float)(-e[1].dy * 16), (float)(-e[2].dy * 16)};
  const float stepY[3] = {(float)(e[0].dx * 16), (float)(e[1].dx * 16), (float)(e[2].dx * 16)};
  const float dwdx = (stepX[0] * wa + stepX[1] * wb + stepX[2] * wc) * inv;
  const float dsdx = (stepX[0] * sa + stepX[1] * sb + stepX[2] * sc) * inv;
  for (int y = y0; y <= y1; y++) {
    int32_t w0 = row[0], w1 = row[1], w2 = row[2];
    float w = ((float)w0 * wa + (float)w1 * wb + (float)w2 * wc) * inv;
    float s = ((float)w0 * sa + (float)w1 * sb + (float)w2 * sc) * inv;
    for (int x = x0; x <= x1; x++) {
      if (w0 - e[0].bias >= 0 && w1 - e[1].bias >= 0 && w2 - e[2].bias >= 0) {
        const int i = y * kW + x;
        if (w > zbuf[i]) {
          zbuf[i] = w;
          frag(i, s);
        }
      }
      w0 -= e[0].dy * 16;
      w1 -= e[1].dy * 16;
      w2 -= e[2].dy * 16;
      w += dwdx;
      s += dsdx;
    }
    for (int k = 0; k < 3; k++) row[k] += e[k].dx * 16;
  }
  (void)stepY;
}

// ----------------------------------------------------------------- output ----

enum Mode : uint8_t {
  MODE_MONO = 0,       // full colour, one camera
  MODE_RED_BLUE = 1,   // R left, B right: the brief's first mode
  MODE_RED_CYAN = 2,   // R left, G and B right
  MODE_RED_GREEN = 3,  // R left, G right
  MODE_COUNT = 4
};

// The owner's stereo profile. Gains are linear, so balancing the two eyes'
// brightness is arithmetic on light, not on codes.
struct Stereo {
  uint8_t mode;
  bool swapEyes;       // the glasses decide which eye is red, not a photo
  float gainL, gainR;  // 0..1
  float depthPx;       // the largest |x_left - x_right| a scene may use
  Stereo() : mode(MODE_RED_BLUE), swapEyes(false), gainL(1.0f), gainR(1.0f), depthPx(2.0f) {}
};

// Left and right intensity planes into one linear RGB frame, per channel and
// additive: where both eyes see light, red-blue gives magenta and red-cyan
// white, as the brief expects.
inline void composeAnaglyph(const uint8_t *left, const uint8_t *right, uint8_t *rgb, const Stereo &st) {
  const uint8_t *L = st.swapEyes ? right : left;
  const uint8_t *R = st.swapEyes ? left : right;
  const int gl = (int)(clampf(st.gainL, 0.0f, 1.0f) * 256.0f + 0.5f);
  const int gr = (int)(clampf(st.gainR, 0.0f, 1.0f) * 256.0f + 0.5f);
  const bool g = st.mode == MODE_RED_CYAN || st.mode == MODE_RED_GREEN;
  const bool b = st.mode == MODE_RED_BLUE || st.mode == MODE_RED_CYAN;
  for (int i = 0; i < kPixels; i++) {
    const int r = (L[i] * gl) >> 8, o = (R[i] * gr) >> 8;
    rgb[3 * i + 0] = (uint8_t)r;
    rgb[3 * i + 1] = g ? (uint8_t)o : 0;
    rgb[3 * i + 2] = b ? (uint8_t)o : 0;
  }
}

// ESP32-HUB75-MatrixPanel-DMA 3.0.14, src/cie_luts.h, lumConvTab_8bit: what the
// library does to every drawPixelRGB888() value at 8-bit colour depth (CIE 1931
// lightness to PWM duty). The host test compares it byte for byte with the
// library's file whenever a build has fetched it.
static const uint8_t kCie8[256] = {
    0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
    2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   3,   3,   4,
    4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   6,   6,   6,   6,   6,   7,
    7,   7,   7,   8,   8,   8,   8,   9,   9,   9,   10,  10,  10,  10,  11,  11,
    11,  12,  12,  12,  13,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
    17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  23,  24,  24,  25,
    25,  26,  26,  27,  28,  28,  29,  29,  30,  31,  31,  32,  32,  33,  34,  34,
    35,  36,  37,  37,  38,  39,  39,  40,  41,  42,  43,  43,  44,  45,  46,  47,
    47,  48,  49,  50,  51,  52,  53,  54,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  74,  75,  76,  77,  79,
    80,  81,  82,  83,  85,  86,  87,  88,  90,  91,  92,  94,  95,  96,  98,  99,
    100, 102, 103, 105, 106, 108, 109, 110, 112, 113, 115, 116, 118, 120, 121, 123,
    124, 126, 128, 129, 131, 132, 134, 136, 138, 139, 141, 143, 145, 146, 148, 150,
    152, 154, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179, 181,
    183, 185, 187, 189, 191, 193, 196, 198, 200, 202, 204, 207, 209, 211, 214, 216,
    218, 220, 223, 225, 228, 230, 232, 235, 237, 240, 242, 245, 247, 250, 252, 255,
};

// Linear light (0..255 of full output) -> the code whose CIE output is
// nearest. Inverting the library's own table is what keeps anti-aliasing and
// additive light honest: no second gamma on top of the library's.
struct Encoder {
  uint8_t code[256];
  Encoder() {
    for (int d = 0; d < 256; d++) {
      int best = 0, err = 256;
      for (int v = 0; v < 256; v++) {
        int e = (int)kCie8[v] - d;
        if (e < 0) e = -e;
        if (e < err) {
          err = e;
          best = v;
        }
      }
      code[d] = (uint8_t)best;
    }
  }
};
// A row of codes cut into runs of one colour: emit(x, length, code) for each,
// maximal, left to right. The panel writes a run with one line of the
// library's hlineDMA, which looks its colour up once and then only touches
// each pixel's words, where drawPixelRGB888 pays the whole way for every
// pixel: measured 14.2-14.9 ms for 8192 single pixels on the panel
// (the integration session, 2026-09-18).
template <class F>
void forEachRun(const uint8_t *row, const F &emit) {
  int x = 0;
  while (x < kW) {
    const uint8_t *c = row + 3 * x;
    int e = x + 1;
    while (e < kW && row[3 * e] == c[0] && row[3 * e + 1] == c[1] && row[3 * e + 2] == c[2]) e++;
    emit(x, e - x, c);
    x = e;
  }
}

// A code (what the rest of the firmware uses for a colour) as linear light.
inline float linearOf(uint8_t code) { return (float)kCie8[code] * (1.0f / 255.0f); }

}  // namespace fx3d

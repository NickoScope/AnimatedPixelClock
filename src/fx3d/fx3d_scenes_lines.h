#pragma once
// The line and point scenes: the brief's calibration and MVP scenes
// (calibration, stereo cube, depth layers, starfield), then its "after the MVP"
// ones (a spiral, rings, a clock with its dial, hands and digits in different
// planes). Thin lines on black, as the brief asks for the glasses. Each works
// in mono too, in colour.

#include "fx3d_scene.h"

namespace fx3d {

// Linear-light colours the line scenes use in mono.
struct Col {
  float r, g, b;
};
inline void use(Ctx &c, const Col &k) { c.color(k.r, k.g, k.b); }
const Col kWhite = {1.0f, 1.0f, 1.0f};
const Col kCyan = {0.15f, 0.85f, 1.0f};
const Col kMagenta = {1.0f, 0.2f, 0.85f};
const Col kAmber = {1.0f, 0.55f, 0.08f};
const Col kGreen = {0.25f, 1.0f, 0.35f};
const Col kRed = {1.0f, 0.12f, 0.1f};
const Col kBlueGrey = {0.35f, 0.45f, 0.75f};

// ------------------------------------------------------------ calibration ----
// The brief's section 7, one page at a time:
//   0 red, 1 green, 2 blue: each channel alone with a four-step scale, raw
//     (no eye mapping), so each eye can be checked against each channel;
//   3 eyes: a vertical bar and an L for the left eye only, a horizontal bar and
//     an R for the right eye only. Seeing the other eye's bar is crosstalk;
//     seeing L on the wrong side means the eyes must be swapped;
//   4 the screen plane: a frame, a cross and the seam at x = 63.5, zero parallax;
//   5 depth: three squares at +d, 0 and -d pixels of disparity (front, screen,
//     behind), d = Stereo::depthPx.
class CalibScene : public Scene {
 public:
  static const int kPages = 6;
  int page;
  bool autoAdvance;   // the previews walk the pages; on the panel /fx3d picks them
  CalibScene() : page(0), autoAdvance(false), t_(0.0f) {}
  const char *id() const { return "calib"; }
  void setup(View &v) const {
    v.f = 80.0f;
    v.z0 = 5.0f;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = 5.0f;   // screen-space pages carry their own disparity
    zf = 5.0f;
  }
  void reset(uint32_t) { t_ = 0.0f; }
  void step(float dt, const Env &) {
    t_ += dt;
    if (autoAdvance && t_ >= 3.0f) {
      t_ = 0.0f;
      page = (page + 1) % kPages;
    }
  }
  bool raw() const { return page <= 2; }
  void draw(Ctx &c) {
    if (page <= 2) {
      drawChannel(c, page);
      return;
    }
    if (page == 3) {
      drawEyes(c);
      return;
    }
    const float d = c.stereo() ? c.st.depthPx : 0.0f;
    use(c, kWhite);
    if (page == 4) {
      box(c, 3.0f, 3.0f, 124.0f, 60.0f, 0.8f, 0.0f);
      c.line2(53.5f, 31.5f, 73.5f, 31.5f, 0.8f, 0.0f);
      c.line2(63.5f, 21.5f, 63.5f, 41.5f, 0.8f, 0.0f);
      for (int y = 0; y < kH; y += 4) c.line2(63.5f, (float)y, 63.5f, (float)y + 1.5f, 0.35f, 0.0f);  // the seam
      return;
    }
    box(c, 12.0f, 20.0f, 36.0f, 44.0f, 1.0f, d);     // in front
    box(c, 52.0f, 20.0f, 76.0f, 44.0f, 1.0f, 0.0f);  // on the panel
    box(c, 92.0f, 20.0f, 116.0f, 44.0f, 1.0f, -d);   // behind
    c.line2(22.0f, 10.0f, 26.0f, 10.0f, 0.8f, d);     // "+" over the front one
    c.line2(24.0f, 8.0f, 24.0f, 12.0f, 0.8f, d);
    c.line2(62.0f, 8.0f, 66.0f, 8.0f, 0.8f, 0.0f);    // "0" over the middle one
    c.line2(62.0f, 12.0f, 66.0f, 12.0f, 0.8f, 0.0f);
    c.line2(62.0f, 8.0f, 62.0f, 12.0f, 0.8f, 0.0f);
    c.line2(66.0f, 8.0f, 66.0f, 12.0f, 0.8f, 0.0f);
    c.line2(102.0f, 10.0f, 106.0f, 10.0f, 0.8f, -d);  // "-" over the one behind
  }

 private:
  float t_;
  static void box(Ctx &c, float x0, float y0, float x1, float y1, float v, float d) {
    c.line2(x0, y0, x1, y0, v, d);
    c.line2(x1, y0, x1, y1, v, d);
    c.line2(x1, y1, x0, y1, v, d);
    c.line2(x0, y1, x0, y0, v, d);
  }
  static void drawChannel(Ctx &c, int ch) {
    const float k[3] = {ch == 0 ? 1.0f : 0.0f, ch == 1 ? 1.0f : 0.0f, ch == 2 ? 1.0f : 0.0f};
    // A big bar on the left, then four steps: 1/8, 1/4, 1/2 and all of the
    // panel's output (linear light).
    for (int y = 8; y < 56; y++)
      for (int x = 8; x < 40; x++) c.pixel(y * kW + x, k[0], k[1], k[2]);
    const float step[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    for (int s = 0; s < 4; s++)
      for (int y = 16; y < 48; y++)
        for (int x = 52 + 18 * s; x < 66 + 18 * s; x++)
          c.pixel(y * kW + x, k[0] * step[s], k[1] * step[s], k[2] * step[s]);
  }
  static void drawEyes(Ctx &c) {
    use(c, kWhite);
    if (c.eye <= 0) {   // left eye (and mono: both)
      c.line2(40.0f, 8.0f, 40.0f, 56.0f, 1.0f, 0.0f);
      c.glyph('L', 26.0f, 26.0f, 2.0f, c.view.z0, 1.0f);
    }
    if (c.eye >= 0) {   // right eye
      c.line2(72.0f, 32.0f, 120.0f, 32.0f, 1.0f, 0.0f);
      c.glyph('R', 92.0f, 12.0f, 2.0f, c.view.z0, 1.0f);
    }
  }
};

// ------------------------------------------------------------------ cube ----
// A slowly turning wire cube around the zero-parallax plane: half of it in
// front of the panel, half behind. Nearer edges brighter.
class CubeScene : public Scene {
 public:
  CubeScene() : yaw_(0.0f), pitch_(0.0f) {}
  const char *id() const { return "cube"; }
  void setup(View &v) const {
    v.f = 136.0f;
    v.z0 = kZ;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - kHalfDiag;
    zf = kZ + kHalfDiag;
  }
  void reset(uint32_t) {
    yaw_ = 0.6f;
    pitch_ = 0.45f;
  }
  void step(float dt, const Env &) {
    yaw_ = wrapAngle(yaw_ + 0.42f * dt);
    pitch_ = wrapAngle(pitch_ + 0.27f * dt);
  }
  void draw(Ctx &c) {
    const M3 r = mul(rotY(yaw_), rotX(pitch_));
    V3 p[8];
    for (int i = 0; i < 8; i++) {
      V3 q = v3((i & 1) ? kHalf : -kHalf, (i & 2) ? kHalf : -kHalf, (i & 4) ? kHalf : -kHalf);
      p[i] = apply(r, q) + v3(0.0f, 0.0f, kZ);
    }
    use(c, kCyan);
    for (int i = 0; i < 8; i++)
      for (int bit = 1; bit < 8; bit <<= 1)
        if (!(i & bit)) {
          const int j = i | bit;
          c.line(p[i], p[j], cue(p[i].z), cue(p[j].z));
        }
    use(c, kWhite);
    for (int i = 0; i < 8; i++) c.dot(p[i], 1.0f, cue(p[i].z));
  }

 private:
  // f / z0 = 16 pixels per unit on the panel's plane; the longer lens keeps
  // the far face at 0.7 of the near one's size instead of 0.55.
  static constexpr float kZ = 8.5f;
  static constexpr float kHalf = 0.85f;
  static constexpr float kHalfDiag = 0.85f * 1.7320508f;
  float yaw_, pitch_;
  static float cue(float z) { return mixf(1.0f, 0.35f, clampf((z - (kZ - kHalfDiag)) / (2.0f * kHalfDiag), 0.0f, 1.0f)); }
};

// ---------------------------------------------------------------- layers ----
// Three shapes at three depths and a frame in the panel's own plane: a ring in
// front, a square on the panel, a triangle behind. Each bobs a little, out of
// step with the others, because motion helps the eye read depth.
class LayersScene : public Scene {
 public:
  LayersScene() : t_(0.0f) {}
  const char *id() const { return "layers"; }
  void setup(View &v) const {
    v.f = 80.0f;
    v.z0 = 5.0f;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kNear;
    zf = kFar;
  }
  void reset(uint32_t) { t_ = 0.0f; }
  void step(float dt, const Env &) { t_ = fmodf(t_ + dt, 60.0f); }
  void draw(Ctx &c) {
    const float z0 = c.view.z0;
    use(c, kBlueGrey);   // the panel's plane: a frame just inside the edges
    rect(c, 2.0f, 2.0f, 125.0f, 61.0f, z0, 0.45f);
    const float b1 = 5.0f * sinf(kTwoPi * t_ / 6.0f);
    const float b2 = 5.0f * sinf(kTwoPi * t_ / 6.0f + 2.1f);
    const float b3 = 5.0f * sinf(kTwoPi * t_ / 6.0f + 4.2f);
    use(c, kAmber);      // in front
    ring(c, 30.0f, 32.0f + b1, 13.0f, kNear, 1.0f);
    use(c, kWhite);      // on the panel
    rect(c, 52.0f, 20.0f + b2, 76.0f, 44.0f + b2, z0, 0.9f);
    use(c, kCyan);       // behind
    V3 a = c.atScreen(98.0f, 17.0f + b3, kFar), b = c.atScreen(114.0f, 45.0f + b3, kFar),
       d = c.atScreen(82.0f, 45.0f + b3, kFar);
    c.line(a, b, 0.8f, 0.8f);
    c.line(b, d, 0.8f, 0.8f);
    c.line(d, a, 0.8f, 0.8f);
  }

 private:
  static constexpr float kNear = 4.0f;
  static constexpr float kFar = 6.6f;
  float t_;
  static void rect(Ctx &c, float x0, float y0, float x1, float y1, float z, float v) {
    V3 a = c.atScreen(x0, y0, z), b = c.atScreen(x1, y0, z), d = c.atScreen(x1, y1, z), e = c.atScreen(x0, y1, z);
    c.line(a, b, v, v);
    c.line(b, d, v, v);
    c.line(d, e, v, v);
    c.line(e, a, v, v);
  }
  static void ring(Ctx &c, float sx, float sy, float r, float z, float v) {
    V3 prev = c.atScreen(sx + r, sy, z);
    for (int k = 1; k <= 40; k++) {
      float a = kTwoPi * k / 40.0f;
      V3 p = c.atScreen(sx + r * cosf(a), sy + r * sinf(a), z);
      c.line(prev, p, v, v);
      prev = p;
    }
  }
};

// ----------------------------------------------------------------- stars ----
// Stars flying at the viewer out of a seeded field. They pass through the
// panel's plane; in front of it they fade out near the edges, so the frame of
// the panel never cuts through something standing in front of it.
class StarsScene : public Scene {
 public:
  static const int kCount = 140;
  StarsScene() : rng_(1) {}
  const char *id() const { return "stars"; }
  void setup(View &v) const {
    v.f = 80.0f;
    v.z0 = 5.0f;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZMin;
    zf = kZMax;
  }
  void reset(uint32_t seed) {
    rng_ = Rng(seed);
    for (int i = 0; i < kCount; i++) {
      spawn(s_[i]);
      s_[i].z = rng_.range(kZMin, kZMax);
    }
  }
  void step(float dt, const Env &) {
    for (int i = 0; i < kCount; i++) {
      s_[i].z -= kSpeed * dt;
      if (s_[i].z < kZMin) spawn(s_[i]);
    }
  }
  void draw(Ctx &c) {
    const float z0 = c.view.z0;
    for (int i = 0; i < kCount; i++) {
      const V3 &p = s_[i];
      float near = clampf((kZMax - p.z) / (kZMax - kZMin), 0.0f, 1.0f);
      float v = 0.08f + 0.92f * near * near;
      if (p.z < z0) {   // in front of the panel: fade out towards its edges
        float sx, sy;
        c.view.project(p, 0, sx, sy);
        float edge = sx < kW - 1 - sx ? sx : kW - 1 - sx;
        float ey = sy < kH - 1 - sy ? sy : kH - 1 - sy;
        if (ey < edge) edge = ey;
        v *= smoothstepf(0.0f, 10.0f, edge);
      }
      c.color(0.85f + 0.15f * near, 0.9f, 1.0f);
      V3 tail = p + v3(0.0f, 0.0f, 0.35f * near);
      c.line(p, tail, v, 0.0f);
      c.point(p, v);
    }
  }

 private:
  static constexpr float kZMin = 2.5f, kZMax = 14.0f, kSpeed = 2.4f;
  V3 s_[kCount];
  Rng rng_;
  void spawn(V3 &p) {
    p.x = rng_.range(-4.5f, 4.5f);
    p.y = rng_.range(-2.4f, 2.4f);
    if (fabsf(p.x) < 0.3f && fabsf(p.y) < 0.3f) p.x = 0.6f;   // nothing flies into the eye
    p.z = kZMax;
  }
};

// ----------------------------------------------------------------- helix ----
// Two strands and their rungs, lying across the panel and turning about their
// own axis, so every point swings between in front and behind.
class HelixScene : public Scene {
 public:
  HelixScene() : phase_(0.0f) {}
  const char *id() const { return "helix"; }
  void setup(View &v) const {
    v.f = 192.0f;
    v.z0 = kZ;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - kR;
    zf = kZ + kR;
  }
  void reset(uint32_t) { phase_ = 0.0f; }
  void step(float dt, const Env &) { phase_ = wrapAngle(phase_ + 0.9f * dt); }
  void draw(Ctx &c) {
    const int n = 72;
    V3 pa = at(0, 0.0f), pb = at(0, kPi);
    for (int k = 1; k <= n; k++) {
      V3 a = at(k * kLen / n, 0.0f), b = at(k * kLen / n, kPi);
      use(c, kCyan);
      c.line(pa, a, cue(pa.z), cue(a.z));
      use(c, kMagenta);
      c.line(pb, b, cue(pb.z), cue(b.z));
      if (k % 4 == 0) {
        use(c, kWhite);
        c.line(a, b, 0.45f * cue(a.z), 0.45f * cue(b.z));
      }
      pa = a;
      pb = b;
    }
  }

 private:
  // 16 pixels per unit on the panel's plane, as the cube; the long lens keeps
  // the ends of the strands from splaying out.
  static constexpr float kZ = 12.0f;
  static constexpr float kR = 0.85f, kLen = 7.2f, kTurns = 2.5f;
  float phase_;
  V3 at(float s, float off) const {   // s along the axis, 0..kLen
    float a = phase_ + off + kTwoPi * kTurns * s / kLen;
    return v3(s - 0.5f * kLen, kR * cosf(a), kZ + kR * sinf(a));
  }
  static float cue(float z) { return mixf(1.0f, 0.3f, clampf((z - (kZ - kR)) / (2.0f * kR), 0.0f, 1.0f)); }
};

// ----------------------------------------------------------------- rings ----
// Rings coming at the viewer down a gently winding tube. The panel's plane is
// the nearest ring's, so everything is behind the panel and nothing in front
// of it is ever cut by its frame.
class RingsScene : public Scene {
 public:
  static const int kRings = 8;
  RingsScene() : travel_(0.0f), wobX_(0.0f), wobY_(0.0f) {}
  const char *id() const { return "rings"; }
  void setup(View &v) const {
    v.f = 80.0f;
    v.z0 = kZMin;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZMin;
    zf = kZMin + kRings * kGap;
  }
  void reset(uint32_t) { travel_ = wobX_ = wobY_ = 0.0f; }
  // Travel wraps after 999 rings, a multiple of the three colours; the tube's
  // wobble keeps phases of its own. Nothing jumps, however long it runs.
  void step(float dt, const Env &) {
    travel_ = fmodf(travel_ + kSpeed * dt, 999.0f * kGap);
    wobX_ = wrapAngle(wobX_ + 0.23f * kSpeed * dt);
    wobY_ = wrapAngle(wobY_ + 0.31f * kSpeed * dt);
  }
  void draw(Ctx &c) {
    const float off = fmodf(travel_, kGap);
    const int first = (int)(travel_ / kGap);
    for (int i = kRings - 1; i >= 0; i--) {
      float z = kZMin + i * kGap + (kGap - off);
      if (z > kZMin + kRings * kGap) z -= kRings * kGap;
      const int id = first + i;
      V3 ctr = v3(0.45f * sinf(wobX_ + 0.23f * z), 0.3f * sinf(wobY_ + 0.31f * z + 1.0f), z);
      float fade = clampf((kZMin + kRings * kGap - z) / (1.5f * kGap), 0.0f, 1.0f);
      float v = fade * mixf(1.0f, 0.35f, (z - kZMin) / (kRings * kGap));
      const Col &k = (id % 3 == 0) ? kMagenta : ((id % 3 == 1) ? kCyan : kAmber);
      use(c, k);
      V3 prev = ctr + v3(kR, 0.0f, 0.0f);
      for (int s = 1; s <= 36; s++) {
        float a = kTwoPi * s / 36.0f;
        V3 p = ctr + v3(kR * cosf(a), kR * sinf(a), 0.0f);
        c.line(prev, p, v, v);
        prev = p;
      }
    }
  }

 private:
  static constexpr float kZMin = 3.2f, kGap = 1.4f, kR = 1.05f, kSpeed = 1.6f;
  float travel_, wobX_, wobY_;
};

// ------------------------------------------------------------------ dial ----
// A clock in layers: the dial's ring and ticks on the panel's plane, the
// numerals just in front, the hands further out, the second hand nearest; the
// hours and minutes as digits behind the panel on either side of the dial.
class DialScene : public Scene {
 public:
  DialScene() : sec_(30.5f), min_(8.0f), hour_(10.0f) {}
  const char *id() const { return "dial"; }
  void setup(View &v) const {
    v.f = 80.0f;
    v.z0 = 5.0f;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZHub;
    zf = kZDigits;
  }
  void reset(uint32_t) {}
  void step(float, const Env &w) {
    if (!w.valid) return;
    sec_ = (float)w.second + w.sub;
    min_ = (float)w.minute + sec_ / 60.0f;
    hour_ = (float)(w.hour % 12) + min_ / 60.0f;
    hh_ = w.hour;
    mm_ = w.minute;
  }
  void draw(Ctx &c) {
    const float cx = 63.5f, cy = 31.5f, z0 = c.view.z0;
    use(c, kBlueGrey);
    V3 prev = c.atScreen(cx + kRing, cy, z0);
    for (int k = 1; k <= 64; k++) {
      float a = kTwoPi * k / 64.0f;
      V3 p = c.atScreen(cx + kRing * cosf(a), cy + kRing * sinf(a), z0);
      c.line(prev, p, 0.6f, 0.6f);
      prev = p;
    }
    use(c, kWhite);
    for (int k = 0; k < 12; k++) {
      float a = kTwoPi * k / 12.0f, s = sinf(a), co = -cosf(a);
      float r0 = (k % 3 == 0) ? kRing - 5.0f : kRing - 3.0f;
      c.line(c.atScreen(cx + r0 * s, cy + r0 * co, z0), c.atScreen(cx + (kRing - 1.0f) * s, cy + (kRing - 1.0f) * co, z0),
             0.9f, 0.9f);
    }
    use(c, kAmber);   // numerals, a little in front
    c.text("12", cx - 5.0f, cy - kRing + 7.0f, 1.0f, kZNum, 0.9f);
    c.text("3", cx + kRing - 12.0f, cy - 3.0f, 1.0f, kZNum, 0.9f);
    c.text("6", cx - 2.0f, cy + kRing - 13.0f, 1.0f, kZNum, 0.9f);
    c.text("9", cx - kRing + 8.0f, cy - 3.0f, 1.0f, kZNum, 0.9f);
    hand(c, kAmber, hour_ / 12.0f, 13.0f, kZHour, 1.0f, true);
    hand(c, kCyan, min_ / 60.0f, 21.0f, kZMin, 1.0f, true);
    hand(c, kRed, sec_ / 60.0f, 24.0f, kZSec, 0.9f, false);
    use(c, kWhite);
    c.dot(c.atScreen(cx, cy, kZHub), 1.2f, 1.0f);
    char h[3] = {(char)('0' + hh_ / 10), (char)('0' + hh_ % 10), 0};
    char m[3] = {(char)('0' + mm_ / 10), (char)('0' + mm_ % 10), 0};
    use(c, kGreen);   // behind the panel, either side of the dial
    c.text(h, 6.0f, 20.0f, 2.0f, kZDigits, 0.85f);
    c.text(m, 102.0f, 20.0f, 2.0f, kZDigits, 0.85f);
  }

 private:
  static constexpr float kRing = 29.0f;
  static constexpr float kZNum = 4.6f, kZHour = 4.4f, kZMin = 4.1f, kZSec = 3.85f, kZHub = 3.8f, kZDigits = 7.0f;
  float sec_, min_, hour_;
  int hh_ = 10, mm_ = 8;
  void hand(Ctx &c, const Col &k, float turn, float len, float z, float v, bool wide) {
    const float cx = 63.5f, cy = 31.5f;
    float a = kTwoPi * turn, s = sinf(a), co = -cosf(a);
    use(c, k);
    c.line(c.atScreen(cx - 3.0f * s, cy - 3.0f * co, z), c.atScreen(cx + len * s, cy + len * co, z), v, v);
    if (wide) {   // a second stroke, half a pixel aside, gives the hand body
      float nx = co * 0.6f, ny = -s * 0.6f;
      c.line(c.atScreen(cx + nx, cy + ny, z), c.atScreen(cx + (len - 3.0f) * s + nx, cy + (len - 3.0f) * co + ny, z),
             0.7f * v, 0.7f * v);
    }
  }
};

}  // namespace fx3d

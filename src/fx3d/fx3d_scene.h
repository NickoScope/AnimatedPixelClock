#pragma once
// What a scene draws with, and how a frame is made from it.
//
// A scene holds its own state and moves it only in step(). draw() is then
// called once in mono, or once per eye in stereo, and must not change the
// state: both eyes see the same moment (the brief, section 4). Plain C++11.

#include "fx3d_model.h"

namespace fx3d {

// The buffers a frame is drawn into. All of them come from the caller: PSRAM on
// the panel, the heap on the Mac. Linear light throughout.
struct Frame {
  uint8_t *rgb;     // kPixels * 3, mono colour, then the composed anaglyph
  uint8_t *eye[2];  // kPixels each: [0] left, [1] right
  float *z;         // kPixels: 1/z, 0 = nothing drawn
};
const size_t kFrameBytes = (size_t)kPixels * 3 + 2 * (size_t)kPixels + sizeof(float) * (size_t)kPixels;

// What the panel tells a scene each frame: the wall clock for the scenes that
// show it (valid is false before NTP), and the audio visualizer's latest
// frame for the ones that move to music (sound is false when none is coming).
const int kBands = 32;   // VIZ_BANDS, src/viz/wow/viz_frame.h
struct Env {
  bool valid;
  int hour, minute, second;  // local
  float sub;                 // fraction of the current second
  int yday;                  // 0-based day of the year, UTC
  float utcHours;            // UTC time of day, 0..24
  bool sound;
  float band[kBands];        // 0..1, bass first
  float beat;                // 0..1 on a beat frame, else 0
  Env() : valid(false), hour(0), minute(0), second(0), sub(0.0f), yday(0), utcHours(0.0f), sound(false), beat(0.0f) {
    for (int i = 0; i < kBands; i++) band[i] = 0.0f;
  }
};

// Seven segments on a 4 x 6 grid (y down), plus the few letters the
// calibration scene needs. Each glyph is up to seven segments.
struct Seg {
  int8_t x0, y0, x1, y1;
};
inline int glyphSegments(char ch, Seg *out) {
  static const Seg s[] = {
      {0, 0, 4, 0}, {4, 0, 4, 3}, {4, 3, 4, 6}, {0, 6, 4, 6}, {0, 3, 0, 6}, {0, 0, 0, 3}, {0, 3, 4, 3},
  };  // a b c d e f g
  static const uint8_t digit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  int n = 0;
  uint8_t mask = 0;
  if (ch >= '0' && ch <= '9') mask = digit[ch - '0'];
  else if (ch == 'L') mask = 0x38;             // d e f
  else if (ch == 'R') {                        // a b f e g and a leg
    mask = 0x73;
    Seg leg = {2, 3, 4, 6};
    out[n++] = leg;
  } else if (ch == '-') mask = 0x40;
  else if (ch == '+') {
    Seg h = {0, 3, 4, 3}, v = {2, 1, 2, 5};
    out[n++] = h;
    out[n++] = v;
  }
  for (int k = 0; k < 7; k++)
    if (mask & (1 << k)) out[n++] = s[k];
  return n;
}

// What a frame drew, for the host test and the previews' cost notes. Compiled
// in only with FX3D_STATS, so the panel pays nothing for it.
struct Stats {
  uint32_t lines, points, tris;
  float zMin, zMax;   // camera-space depth of everything drawn, after clipping
  void clear() {
    lines = points = tris = 0;
    zMin = 1.0e30f;
    zMax = -1.0e30f;
  }
};

class Ctx {
 public:
  Frame fb;
  View view;
  Stereo st;
  int eye;  // 0 mono, -1 left, +1 right
#if defined(FX3D_STATS)
  Stats stats;
#endif
  Ctx() : eye(0), cr_(1.0f), cg_(1.0f), cb_(1.0f) {
    memset(&fb, 0, sizeof fb);
#if defined(FX3D_STATS)
    stats.clear();
#endif
  }

  bool stereo() const { return eye != 0; }
  uint8_t *plane() const { return fb.eye[eye > 0 ? 1 : 0]; }

  // The colour of what follows, linear 0..1. In stereo only its brightest
  // channel counts: each eye sees intensity, not colour.
  void color(float r, float g, float b) {
    cr_ = r;
    cg_ = g;
    cb_ = b;
  }

  // Camera-space point that the centre eye sees at screen (sx, sy), depth z.
  // Lets a scene lay things out in pixels and still give them a real depth.
  V3 atScreen(float sx, float sy, float z) const {
    return v3((sx - view.cx) * z / view.f, -(sy - view.cy) * z / view.f, z);
  }

  // Additive, camera space: clipped at nearZ, projected for this eye.
  void line(V3 a, V3 b, float va, float vb) {
    if (!clipNear(a, b, va, vb, view.nearZ)) return;
    note(a.z);
    note(b.z);
    count(1, 0, 0);
    float x0, y0, x1, y1;
    view.project(a, eye, x0, y0);
    view.project(b, eye, x1, y1);
    if (stereo()) fx3d::line(grayPlot(), x0, y0, x1, y1, va, vb);
    else fx3d::line(rgbPlot(), x0, y0, x1, y1, va, vb);
  }
  void point(V3 p, float v) {
    if (p.z < view.nearZ) return;
    note(p.z);
    count(0, 1, 0);
    float x, y;
    view.project(p, eye, x, y);
    if (stereo()) splat(grayPlot(), x, y, v);
    else splat(rgbPlot(), x, y, v);
  }
  // A dot whose radius is r at depth z0 and scales with 1/z.
  void dot(V3 p, float r, float v) {
    if (p.z < view.nearZ) return;
    note(p.z);
    count(0, 1, 0);
    float x, y;
    view.project(p, eye, x, y);
    float rr = r * view.z0 / p.z;
    if (rr < 0.6f) {
      if (stereo()) splat(grayPlot(), x, y, v);
      else splat(rgbPlot(), x, y, v);
      return;
    }
    if (stereo()) disc(grayPlot(), x, y, rr, v);
    else disc(rgbPlot(), x, y, rr, v);
  }
  // A glyph standing in the plane z, top-left at screen (sx, sy) for the centre
  // eye, `px` pixels per grid unit (4 x 6 grid).
  void glyph(char ch, float sx, float sy, float px, float z, float v) {
    Seg s[8];
    int n = glyphSegments(ch, s);
    for (int k = 0; k < n; k++)
      line(atScreen(sx + s[k].x0 * px, sy + s[k].y0 * px, z), atScreen(sx + s[k].x1 * px, sy + s[k].y1 * px, z), v,
           v);
  }
  void text(const char *t, float sx, float sy, float px, float z, float v) {
    for (; *t; t++, sx += 6.0f * px)
      if (*t == ':') {
        dot(atScreen(sx + 1.0f * px, sy + 1.5f * px, z), 0.45f * px, v);
        dot(atScreen(sx + 1.0f * px, sy + 4.5f * px, z), 0.45f * px, v);
        sx -= 3.0f * px;
      } else if (*t != ' ') {
        glyph(*t, sx, sy, px, z, v);
      }
  }

  // Screen-space line shown with disparity d = x_left - x_right pixels
  // (positive: in front of the panel). For the calibration pages.
  void line2(float x0, float y0, float x1, float y1, float v, float d) {
    const float shift = -0.5f * (float)eye * d;
    if (stereo()) fx3d::line(grayPlot(), x0 + shift, y0, x1 + shift, y1, v, v);
    else fx3d::line(rgbPlot(), x0, y0, x1, y1, v, v);
  }

  // Opaque triangle with a depth test, camera space, clipped at nearZ.
  // Shades 0..1 scale the current colour; 1..2 lead it on to white.
  void tri(V3 a, V3 b, V3 c, float sa, float sb, float sc, bool cullBack) {
    V3 in[3] = {a, b, c};
    float sh[3] = {sa, sb, sc};
    V3 out[4];
    float so[4];
    int n = 0;
    for (int k = 0; k < 3; k++) {   // Sutherland-Hodgman against z = nearZ
      const V3 &p = in[k], &q = in[(k + 1) % 3];
      const float sp = sh[k], sq = sh[(k + 1) % 3];
      const bool pin = p.z >= view.nearZ, qin = q.z >= view.nearZ;
      if (pin) {
        out[n] = p;
        so[n++] = sp;
      }
      if (pin != qin) {
        float t = (view.nearZ - p.z) / (q.z - p.z);
        out[n] = lerp(p, q, t);
        out[n].z = view.nearZ;
        so[n++] = mixf(sp, sq, t);
      }
    }
    if (n < 3) return;
    count(0, 0, 1);
    SV sv[4];
    for (int k = 0; k < n; k++) {
      note(out[k].z);
      view.project(out[k], eye, sv[k].x, sv[k].y);
      sv[k].w = 1.0f / out[k].z;
      sv[k].s = so[k];
    }
    if (stereo()) {
      FragGray f = {plane(), 255.0f * maxc()};
      triangle(sv[0], sv[1], sv[2], fb.z, f, cullBack);
      if (n == 4) triangle(sv[0], sv[2], sv[3], fb.z, f, cullBack);
    } else {
      FragRgb f = {fb.rgb, 255.0f * cr_, 255.0f * cg_, 255.0f * cb_};
      triangle(sv[0], sv[1], sv[2], fb.z, f, cullBack);
      if (n == 4) triangle(sv[0], sv[2], sv[3], fb.z, f, cullBack);
    }
  }

  // The current target (the colour frame, or this eye's plane) to black.
  void clear() {
    if (stereo()) memset(plane(), 0, (size_t)kPixels);
    else memset(fb.rgb, 0, (size_t)kPixels * 3);
  }

  // Screen-space pixel write for the scenes that shade every pixel
  // themselves (fly-throughs, raymarching). Overwrites, linear 0..1.
  void pixel(int i, float r, float g, float b) {
    if (stereo()) {
      float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
      plane()[i] = (uint8_t)(clampf(m, 0.0f, 1.0f) * 255.0f + 0.5f);
    } else {
      uint8_t *d = fb.rgb + 3 * i;
      d[0] = (uint8_t)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
      d[1] = (uint8_t)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
      d[2] = (uint8_t)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }

  // For scenes that shade pixels themselves: the depth they reached.
  void note(float z) {
#if defined(FX3D_STATS)
    if (z < stats.zMin) stats.zMin = z;
    if (z > stats.zMax) stats.zMax = z;
#else
    (void)z;
#endif
  }

 private:
  float cr_, cg_, cb_;
  void count(uint32_t l, uint32_t p, uint32_t t) {
#if defined(FX3D_STATS)
    stats.lines += l;
    stats.points += p;
    stats.tris += t;
#else
    (void)l;
    (void)p;
    (void)t;
#endif
  }
  float maxc() const { return cr_ > cg_ ? (cr_ > cb_ ? cr_ : cb_) : (cg_ > cb_ ? cg_ : cb_); }
  PlotGray grayPlot() const {
    PlotGray p = {plane(), 255.0f * maxc()};
    return p;
  }
  PlotRgb rgbPlot() const {
    PlotRgb p = {fb.rgb, 255.0f * cr_, 255.0f * cg_, 255.0f * cb_};
    return p;
  }
  // A shade 0..1 scales the colour; 1..2 carries it on towards white, which
  // is how a specular highlight stays white on a coloured surface.
  static float toward(float c, float s) { return s <= 1.0f ? c * s : c + (255.0f - c) * (s - 1.0f); }
  struct FragGray {
    uint8_t *px;
    float k;
    void operator()(int i, float s) const { px[i] = (uint8_t)(toward(k, clampf(s, 0.0f, 2.0f)) + 0.5f); }
  };
  struct FragRgb {
    uint8_t *px;
    float r, g, b;
    void operator()(int i, float s) const {
      float t = clampf(s, 0.0f, 2.0f);
      px[3 * i + 0] = (uint8_t)(toward(r, t) + 0.5f);
      px[3 * i + 1] = (uint8_t)(toward(g, t) + 0.5f);
      px[3 * i + 2] = (uint8_t)(toward(b, t) + 0.5f);
    }
  };
};

class Scene {
 public:
  virtual ~Scene() {}
  virtual const char *id() const = 0;
  // f, z0 and nearZ for this scene; the baseline is the frame's business.
  virtual void setup(View &v) const = 0;
  // Nearest and farthest depth anything is drawn at: the baseline keeps the
  // disparity of both inside Stereo::depthPx.
  virtual void depthRange(float &zNear, float &zFar) const = 0;
  virtual void reset(uint32_t seed) = 0;
  virtual void step(float dt, const Env &w) = 0;
  virtual void draw(Ctx &c) = 0;
  // True when draw() writes every pixel, so the frame need not clear first.
  virtual bool fills() const { return false; }
  // True while the scene writes raw colour even in a stereo mode: the
  // calibration's pure R, G and B pages, which no eye mapping may touch.
  virtual bool raw() const { return false; }
};

// One frame: the scene is stepped once, drawn once in mono or once per eye in
// stereo, and the result left in c.fb.rgb as linear light.
inline void renderFrame(Scene &s, Ctx &c, float dt, const Env &w) {
  s.step(dt, w);
  s.setup(c.view);
  float zn = 1.0f, zf = 1.0f;
  s.depthRange(zn, zf);
  if (c.st.mode == MODE_MONO || s.raw()) {
    c.view.b = 0.0f;
    c.eye = 0;
    if (!s.fills()) memset(c.fb.rgb, 0, (size_t)kPixels * 3);
    memset(c.fb.z, 0, sizeof(float) * (size_t)kPixels);
    s.draw(c);
    return;
  }
  c.view.b = baselineFor(c.view.f, c.view.z0, zn, zf, c.st.depthPx);
  for (int e = -1; e <= 1; e += 2) {
    c.eye = e;
    if (!s.fills()) memset(c.plane(), 0, (size_t)kPixels);
    memset(c.fb.z, 0, sizeof(float) * (size_t)kPixels);
    s.draw(c);
  }
  composeAnaglyph(c.fb.eye[0], c.fb.eye[1], c.fb.rgb, c.st);
}

}  // namespace fx3d

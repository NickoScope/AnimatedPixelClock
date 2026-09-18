#pragma once
// Solid lit objects: triangles with a depth test and a light. A torus for the
// look, and the time itself in voxels. Plain C++11.

#include "fx3d_scenes_lines.h"

namespace fx3d {

// The light every solid scene uses: from above, to the left, in front. With a
// Blinn-Phong highlight folded into the shade above 1 (white).
struct Light {
  V3 dir;    // towards the light, camera space
  V3 half;   // halfway between the light and the eye
  Light() {
    dir = normalize(v3(-0.55f, 0.65f, -0.55f));
    half = normalize(dir + v3(0.0f, 0.0f, -1.0f));
  }
  // Ambient, diffuse, and a highlight above 1. n is unit, camera space.
  float shade(V3 n, float ambient, float spec) const {
    float d = dot(n, dir);
    float s = ambient + (1.0f - ambient) * (d > 0.0f ? d : 0.0f);
    float h = dot(n, half);
    if (spec > 0.0f && h > 0.0f && d > 0.0f) {
      float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;   // exponent 32, no powf
      s += spec * h16 * h16;
    }
    return s;
  }
};

// A cosine palette: a + b * cos(2 pi (c t + d)), per channel. Inigo Quilez,
// "palettes" (iquilezles.org/articles/palettes). Linear light out.
inline Col palette(float t) {
  Col k;
  k.r = clampf(0.5f + 0.5f * cosf(kTwoPi * (t + 0.00f)), 0.0f, 1.0f);
  k.g = clampf(0.5f + 0.5f * cosf(kTwoPi * (t + 0.33f)), 0.0f, 1.0f);
  k.b = clampf(0.5f + 0.5f * cosf(kTwoPi * (t + 0.67f)), 0.0f, 1.0f);
  return k;
}

// ----------------------------------------------------------------- torus ----
// A torus turning on three axes, Gouraud-lit, its colour running round the
// ring. Half of it in front of the panel in stereo.
class TorusScene : public Scene {
 public:
  static const int kU = 32, kV = 14;
  TorusScene() : yaw_(0.0f), pitch_(0.0f), roll_(0.0f) {}
  const char *id() const { return "torus"; }
  void setup(View &v) const {
    v.f = 110.0f;
    v.z0 = kZ;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - (kR + kr);
    zf = kZ + (kR + kr);
  }
  void reset(uint32_t) {
    yaw_ = 0.3f;
    pitch_ = 1.0f;
    roll_ = 0.0f;
    for (int i = 0; i < kU; i++) {
      cu_[i] = cosf(kTwoPi * (float)i / kU);
      su_[i] = sinf(kTwoPi * (float)i / kU);
    }
    for (int j = 0; j < kV; j++) {
      cv_[j] = cosf(kTwoPi * (float)j / kV);
      sv_[j] = sinf(kTwoPi * (float)j / kV);
    }
  }
  void step(float dt, const Env &) {
    yaw_ = wrapAngle(yaw_ + 0.55f * dt);
    pitch_ = wrapAngle(pitch_ + 0.37f * dt);
    roll_ = wrapAngle(roll_ + 0.21f * dt);
  }
  void draw(Ctx &c) {
    const M3 r = mul(rotZ(roll_), mul(rotY(yaw_), rotX(pitch_)));
    const Light light;
    // Each vertex once: position and shade.
    for (int i = 0; i < kU; i++)
      for (int j = 0; j < kV; j++) {
        V3 n = v3(cv_[j] * cu_[i], sv_[j], cv_[j] * su_[i]);
        V3 p = v3((kR + kr * cv_[j]) * cu_[i], kr * sv_[j], (kR + kr * cv_[j]) * su_[i]);
        n = apply(r, n);
        pos_[i][j] = apply(r, p) + v3(0.0f, 0.0f, kZ);
        shade_[i][j] = light.shade(n, 0.12f, 0.9f);
      }
    for (int i = 0; i < kU; i++) {
      const int i1 = (i + 1) % kU;
      const Col k = palette((float)i / kU + 0.05f);
      c.color(k.r, k.g, k.b);
      for (int j = 0; j < kV; j++) {
        const int j1 = (j + 1) % kV;
        c.tri(pos_[i][j], pos_[i1][j], pos_[i1][j1], shade_[i][j], shade_[i1][j], shade_[i1][j1], true);
        c.tri(pos_[i][j], pos_[i1][j1], pos_[i][j1], shade_[i][j], shade_[i1][j1], shade_[i][j1], true);
      }
    }
  }

 private:
  static constexpr float kZ = 6.0f, kR = 1.0f, kr = 0.42f;
  float yaw_, pitch_, roll_;
  float cu_[kU], su_[kU], cv_[kV], sv_[kV];
  V3 pos_[kU][kV];
  float shade_[kU][kV];
};

// ---------------------------------------------------------------- vclock ----
// The time in voxels: HH:MM from a 5 x 7 font, each lit cell a cube, only the
// faces that show drawn. The block sways so the depth reads; a digit that
// changes flips over on its own axis, and the colon beats the seconds.
inline uint8_t digitRow(int d, int row) {
  // 5 x 7, bit 4 is the left column. Drawn for this panel.
  static const uint8_t f[10][7] = {
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},   // 0
      {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},   // 1
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},   // 2
      {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},   // 3
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},   // 4
      {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},   // 5
      {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},   // 6
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},   // 7
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},   // 8
      {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},   // 9
  };
  return (d >= 0 && d <= 9 && row >= 0 && row < 7) ? f[d][row] : 0;
}

class VoxelClockScene : public Scene {
 public:
  VoxelClockScene() : yawPh_(0.0f), pitchPh_(0.0f), sub_(0.0f) {
    for (int i = 0; i < 4; i++) {
      shown_[i] = 0;
      next_[i] = 0;
      flip_[i] = -1.0f;
    }
  }
  const char *id() const { return "vclock"; }
  void setup(View &v) const {
    v.f = 128.0f;
    v.z0 = kZ;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZ - 1.6f;
    zf = kZ + 1.6f;
  }
  void reset(uint32_t) {
    yawPh_ = pitchPh_ = 0.0f;
    for (int i = 0; i < 4; i++) flip_[i] = -1.0f;
    started_ = false;
  }
  void step(float dt, const Env &w) {
    yawPh_ = wrapAngle(yawPh_ + kTwoPi * dt / 9.0f);
    pitchPh_ = wrapAngle(pitchPh_ + kTwoPi * dt / 7.0f);
    int want[4] = {1, 0, 0, 8};
    if (w.valid) {
      want[0] = w.hour / 10;
      want[1] = w.hour % 10;
      want[2] = w.minute / 10;
      want[3] = w.minute % 10;
      sub_ = w.sub;
    }
    for (int i = 0; i < 4; i++) {
      if (!started_) {
        shown_[i] = next_[i] = want[i];
        continue;
      }
      if (flip_[i] < 0.0f && want[i] != shown_[i]) {
        next_[i] = want[i];
        flip_[i] = 0.0f;
      }
      if (flip_[i] >= 0.0f) {
        flip_[i] += dt / kFlipSeconds;
        if (flip_[i] >= 1.0f) {
          shown_[i] = next_[i];
          flip_[i] = -1.0f;
        }
      }
    }
    started_ = true;
  }
  void draw(Ctx &c) {
    const M3 sway = mul(rotY(0.38f * sinf(yawPh_)), rotX(0.16f * sinf(pitchPh_)));
    const Light light;
    const float col0[4] = {0.0f, 6.0f, 14.0f, 20.0f};   // first column of each digit
    const Col hours = {0.2f, 0.72f, 1.0f}, minutes = {1.0f, 0.5f, 0.1f};
    for (int i = 0; i < 4; i++) {
      float ang = 0.0f;
      int d = shown_[i];
      if (flip_[i] >= 0.0f) {
        ang = kPi * flip_[i];
        if (flip_[i] >= 0.5f) {
          d = next_[i];
          ang -= kPi;
        }
      }
      const M3 flip = rotX(ang);
      const Col &k = i < 2 ? hours : minutes;
      c.color(k.r, k.g, k.b);
      for (int row = 0; row < 7; row++) {
        const uint8_t bits = digitRow(d, row);
        for (int col = 0; col < 5; col++)
          if (bits & (0x10 >> col)) {
            const bool l = col > 0 && (bits & (0x10 >> (col - 1)));
            const bool r = col < 4 && (bits & (0x10 >> (col + 1)));
            const bool up = row > 0 && (digitRow(d, row - 1) & (0x10 >> col));
            const bool dn = row < 6 && (digitRow(d, row + 1) & (0x10 >> col));
            // Cell centre in the digit's own frame, the flip about its middle row.
            V3 local = v3((col - 2.0f) * kU, (3.0f - row) * kU, 0.0f);
            V3 centre = v3((col0[i] + 2.0f - 12.0f) * kU, 0.0f, 0.0f);
            cube(c, light, sway, flip, centre, local, l, r, up, dn);
          }
      }
    }
    // The colon: two cubes that dim through each second.
    const float beat = 0.35f + 0.65f * (1.0f - smoothstepf(0.0f, 0.6f, sub_));
    c.color(beat, beat, beat);
    const M3 none = identity3();
    for (int k = 0; k < 2; k++)
      cube(c, light, sway, none, v3((12.0f - 12.0f) * kU, 0.0f, 0.0f), v3(0.0f, (k ? -1.0f : 1.0f) * kU, 0.0f), false,
           false, false, false);
  }

 private:
  static constexpr float kZ = 7.0f, kU = 0.25f, kFlipSeconds = 0.7f;
  float yawPh_, pitchPh_, sub_;
  int shown_[4], next_[4];
  float flip_[4];
  bool started_ = false;

  // One voxel: the front face always, a side only when no neighbour covers it.
  void cube(Ctx &c, const Light &light, const M3 &sway, const M3 &flip, V3 centre, V3 local, bool l, bool r, bool up,
            bool dn) {
    const float h = 0.5f * kU;
    V3 corner[8];
    for (int k = 0; k < 8; k++) {
      V3 q = local + v3((k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h);
      corner[k] = apply(sway, apply(flip, q) + centre) + v3(0.0f, 0.0f, kZ);
    }
    const M3 rot = mul(sway, flip);
    // Faces as corner indices and outward normals. Camera space is x right,
    // y up, z away from the viewer, so a face turned to the viewer runs
    // counter-clockwise on screen when cross(b - a, c - a) points away from
    // its outward normal; each order below is chosen that way.
    face(c, light, rot, corner, 0, 1, 3, 2, v3(0, 0, -1));            // front (towards the viewer)
    face(c, light, rot, corner, 4, 6, 7, 5, v3(0, 0, 1));             // back
    if (!l) face(c, light, rot, corner, 0, 2, 6, 4, v3(-1, 0, 0));
    if (!r) face(c, light, rot, corner, 1, 5, 7, 3, v3(1, 0, 0));
    if (!up) face(c, light, rot, corner, 2, 3, 7, 6, v3(0, 1, 0));
    if (!dn) face(c, light, rot, corner, 0, 4, 5, 1, v3(0, -1, 0));
  }
  static void face(Ctx &c, const Light &light, const M3 &rot, const V3 *p, int a, int b, int d, int e, V3 n) {
    const float s = light.shade(apply(rot, n), 0.18f, 0.5f);
    c.tri(p[a], p[b], p[d], s, s, s, true);
    c.tri(p[a], p[d], p[e], s, s, s, true);
  }
};

}  // namespace fx3d

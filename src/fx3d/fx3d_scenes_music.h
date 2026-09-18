#pragma once
// To music: the audio visualizer's spectrum as a range of hills walking away
// from the viewer, the live spectrum in front and each past one a row further
// back. Every row is a curve over the 32 bands with a black curtain under it,
// drawn far to near, so a nearer ridge hides what is behind it. A beat
// brightens the front ridge. Plain C++11.

#include "fx3d_scenes_raster.h"

namespace fx3d {

class TerrainScene : public Scene {
 public:
  static const int kRows = 18;
  TerrainScene() : head_(0), acc_(0.0f), flash_(0.0f) {
    memset(rows_, 0, sizeof rows_);
    memset(cur_, 0, sizeof cur_);
  }
  const char *id() const { return "terrain"; }
  void setup(View &v) const {
    v.f = 96.0f;
    v.z0 = kZ0;
    v.nearZ = 0.5f;
  }
  void depthRange(float &zn, float &zf) const {
    zn = kZFront - 0.6f;
    zf = kZFront + (kRows + 1) * kGap + 0.6f;
  }
  void reset(uint32_t) {
    head_ = 0;
    acc_ = flash_ = 0.0f;
    memset(rows_, 0, sizeof rows_);
    memset(cur_, 0, sizeof cur_);
  }
  void step(float dt, const Env &w) {
    const float follow = clampf(dt * 18.0f, 0.0f, 1.0f), fall = clampf(dt * 1.5f, 0.0f, 1.0f);
    for (int b = 0; b < kBands; b++) cur_[b] = w.sound ? mixf(cur_[b], clampf(w.band[b], 0.0f, 1.0f), follow) : cur_[b] * (1.0f - fall);
    if (w.sound && w.beat > flash_) flash_ = clampf(w.beat, 0.0f, 1.0f);
    flash_ *= 1.0f - clampf(dt * 5.0f, 0.0f, 1.0f);
    acc_ += dt;
    if (acc_ >= kRowSeconds * 8.0f) acc_ = kRowSeconds;   // after a stall, one row, not a burst
    while (acc_ >= kRowSeconds) {
      acc_ -= kRowSeconds;
      head_ = (head_ + kRows - 1) % kRows;   // the newest row is head_
      memcpy(rows_[head_], cur_, sizeof cur_);
    }
  }
  void draw(Ctx &c) {
    const float frac = acc_ / kRowSeconds;
    for (int k = kRows - 1; k >= 0; k--) {
      const float z = kZFront + ((float)k + 1.0f + frac) * kGap;
      const float fade = 1.0f - (float)k / kRows;
      ridge(c, rows_[(head_ + k) % kRows], z, 0.25f + 0.6f * fade * fade, 0.0f);
    }
    ridge(c, cur_, kZFront, 1.0f, flash_);
  }

 private:
  static constexpr float kZ0 = 6.5f, kZFront = 4.2f, kGap = 0.55f, kRowSeconds = 0.12f;
  static constexpr float kHalfWidth = 2.6f, kBase = -0.75f, kAmp = 1.3f, kPitch = 0.42f;
  float rows_[kRows][kBands];
  float cur_[kBands];
  int head_;
  float acc_, flash_;

  // Camera-space point for band position u (0..1) at height h on the row at z,
  // the whole field tipped towards the viewer about the zero-parallax depth.
  static V3 at(float u, float h, float z) {
    const float y = kBase + h, dz = z - kZ0;
    const float cp = cosf(kPitch), sp = sinf(kPitch);
    return v3((u * 2.0f - 1.0f) * kHalfWidth, y * cp + dz * sp, kZ0 - y * sp + dz * cp);
  }
  static float height(float level) { return kAmp * level * (2.0f - level) * 0.5f; }   // eases the tops
  V3 top_[kBands], foot_[kBands];   // one ridge's points: here, not on loopTask's 8 KB stack

  void ridge(Ctx &c, const float *band, float z, float bright, float flash) {
    V3 *top = top_, *foot = foot_;
    for (int b = 0; b < kBands; b++) {
      const float u = (float)b / (kBands - 1);
      top[b] = at(u, height(band[b]), z);
      foot[b] = at(u, -0.15f, z);
    }
    c.color(0.0f, 0.0f, 0.0f);   // the curtain: black, and it hides what is behind it
    for (int b = 0; b + 1 < kBands; b++) {
      c.tri(top[b], top[b + 1], foot[b + 1], 0.0f, 0.0f, 0.0f, false);
      c.tri(top[b], foot[b + 1], foot[b], 0.0f, 0.0f, 0.0f, false);
    }
    for (int b = 0; b + 1 < kBands; b++) {
      const float lvl = 0.5f * (band[b] + band[b + 1]);
      const Col k = palette(0.62f - 0.45f * lvl);   // quiet: violet-blue, loud: orange
      c.color(mixf(k.r, 1.0f, flash), mixf(k.g, 1.0f, flash), mixf(k.b, 1.0f, flash));
      c.line(top[b], top[b + 1], bright, bright);
    }
  }
};

}  // namespace fx3d

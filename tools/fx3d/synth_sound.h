#pragma once
// A stand-in for the audio visualizer's frames, for the previews and the host
// test: 120 BPM, a kick in the bass bands on every beat, a snare in the mids on
// two and four, hi-hats on the off-beats, a melody wandering through the
// middle, and a floor of noise. Deterministic: the same time gives the same
// frame. Not music, and not the DSP: only enough to move the scenes.
#include <cmath>

#include "fx3d_scene.h"

inline void synthSound(float t, fx3d::Env &w) {
  const float beat = 0.5f;   // seconds, 120 BPM
  const float inBeat = std::fmod(t, beat) / beat;
  const int n = (int)(t / beat);
  const float kick = std::exp(-inBeat * 7.0f);
  const float snare = (n % 2 == 1) ? std::exp(-inBeat * 9.0f) : 0.0f;
  const float hat = std::exp(-std::fmod(t + 0.25f, 0.5f) * 30.0f);
  const float melody = 12.0f + 6.0f * std::sin(t * 0.9f) + 3.0f * std::sin(t * 2.3f);
  fx3d::Rng rng((uint32_t)(t * 60.0f) + 1u);
  w.sound = true;
  for (int b = 0; b < fx3d::kBands; b++) {
    float v = 0.06f + 0.06f * rng.uniform();
    if (b < 6) v += 0.9f * kick * (1.0f - b / 7.0f);
    if (b >= 8 && b < 22) v += 0.55f * snare * rng.uniform();
    if (b >= 24) v += 0.6f * hat * (0.5f + 0.5f * rng.uniform());
    v += 0.65f * std::exp(-(b - melody) * (b - melody) / 6.0f);
    w.band[b] = v > 1.0f ? 1.0f : v;
  }
  w.beat = inBeat < 0.07f ? 1.0f : 0.0f;
}

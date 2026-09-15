/*
 * PcFrameDeriver: the richer frame rebuilt from a PC companion packet.
 * Constants are the DSP's (src/audio/audio_dsp.cpp); the band bytes stand in
 * for dB, 38 dB over 0..255 as the companion scales them.
 */
// Firmware builds without the new styles skip it; host builds (no ARDUINO) always compile it.
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include "viz_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wow {
namespace {

constexpr float kDbPerStep = 38.0f / 255.0f;    // audio_spectrum.py AGC_RANGE_DB over a byte
constexpr float kAttackS = 0.012f, kReleaseS = 0.200f;
constexpr float kPeakHoldS = 0.35f, kPeakGravity = 2.5f;
constexpr float kFluxK = 2.5f, kFluxDeltaDb = 3.0f, kBeatRiseDb = 3.0f;
constexpr float kBeatWaitS = 0.16f;

}  // namespace

void PcFrameDeriver::feed(const uint8_t *bands, const uint8_t *wave, float dt, VizFrame &out) {
  if (!(dt >= 0.01f)) dt = 0.01f;   // also catches NaN
  if (dt > 0.1f) dt = 0.1f;
  std::memcpy(out.bands, bands, kBands);
  std::memcpy(out.wave, wave, kWavePoints);

  // A packet of all zeros is the companion's silence (below its AGC window).
  bool gated = true;
  for (int b = 0; b < kBands; b++)
    if (bands[b]) gated = false;

  // Beats: the DSP's rule on bass flux, with bytes as dB above the AGC floor.
  float x[kFluxBands];
  float bassDb = 0.0f;
  for (int i = 0; i < kFluxBands; i++) {
    x[i] = bands[i] * kDbPerStep;
    bassDb += x[i];
  }
  bassDb /= kFluxBands;
  float flux = 0.0f;
  if (havePrev_ && !gated) {
    for (int i = 0; i < kFluxBands; i++) flux += std::max(0.0f, x[i] - prev_[i]);
    flux /= kFluxBands;
  }
  std::memcpy(prev_, x, sizeof(x));
  havePrev_ = true;
  float mean = 0.0f, sd = 0.0f;
  bool riseOk = true;
  if (histLen_ > 0) {
    float bassAvg = 0.0f;
    for (int i = 0; i < histLen_; i++) {
      mean += fluxHist_[i];
      bassAvg += bassHist_[i];
    }
    mean /= histLen_;
    bassAvg /= histLen_;
    for (int i = 0; i < histLen_; i++) sd += (fluxHist_[i] - mean) * (fluxHist_[i] - mean);
    sd = std::sqrt(sd / histLen_);
    riseOk = bassDb - bassAvg >= kBeatRiseDb;
  }
  const float thr = std::max(kFluxDeltaDb, mean + kFluxK * sd);
  sinceBeat_ += dt;
  const bool beat = !gated && flux >= thr && flux > prevFlux_ && sinceBeat_ >= kBeatWaitS && riseOk;
  fluxHist_[histPos_] = flux;
  bassHist_[histPos_] = bassDb;
  histPos_ = (histPos_ + 1) % kHistory;
  if (histLen_ < kHistory) histLen_++;
  prevFlux_ = flux;
  out.strength = 0.0f;
  if (beat) {
    sinceBeat_ = 0.0f;
    out.strength = std::min(1.0f, (flux - thr) / std::max(thr, 1e-3f));
  }

  // Levels and peak holds, as the DSP makes them, at the packet's own interval.
  const float ca = 1.0f - std::exp(-dt / kAttackS), cr = 1.0f - std::exp(-dt / kReleaseS);
  float bass = 0.0f, mid = 0.0f, treble = 0.0f;
  for (int b = 0; b < kBands; b++) {
    const float target = bands[b] / 255.0f;
    level_[b] += (target - level_[b]) * (target > level_[b] ? ca : cr);
    if (level_[b] >= peak_[b]) {
      peak_[b] = level_[b];
      peakHold_[b] = kPeakHoldS;
      peakVel_[b] = 0.0f;
    } else if (peakHold_[b] > 0.0f) {
      peakHold_[b] -= dt;
    } else {
      peakVel_[b] += kPeakGravity * dt;
      peak_[b] = std::max(level_[b], peak_[b] - peakVel_[b] * dt);
    }
    if (b < 8) bass += level_[b];
    else if (b < 24) mid += level_[b];
    else treble += level_[b];
  }
  std::memcpy(out.level, level_, sizeof(level_));
  std::memcpy(out.peak, peak_, sizeof(peak_));
  out.bass = bass / 8.0f;
  out.mid = mid / 16.0f;
  out.treble = treble / 8.0f;
  out.beat = beat;
  out.clipping = 0;
  out.steps = 2;
  out.reserved = 0;
}

}  // namespace wow

#endif

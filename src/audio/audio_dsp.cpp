/*
 * See audio_dsp.h. Every step here has its twin, in the same order, in
 * tools/audiofx/dsp.py; change one and the host test fails until the other
 * follows.
 */
// Firmware builds without the microphones skip it; host builds (no ARDUINO) always compile it.
#if defined(AUDIO_MIC_ENABLED) || !defined(ARDUINO)

#include "audio_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace audiodsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFLo = 50.0, kFHi = 16000.0;      // audio_spectrum.py F_LO, F_HI
constexpr double kAgcRangeDb = 38.0;                // audio_spectrum.py AGC_RANGE_DB
constexpr double kAgcDecayDbPerS = 1.5;             // audio_spectrum.py AGC_DECAY_DB_PER_S
constexpr double kAgcFloorDb = -70.0;               // starting value
constexpr double kFixedRefDb = -20.0;               // starting value, AGC off
constexpr int kGateHoldFrames = 12;                 // 240 ms
constexpr double kAttackS = 0.012, kReleaseS = 0.200;
constexpr double kPeakHoldS = 0.35, kPeakGravity = 2.5;
constexpr double kFluxFloorDb = -90.0;              // beat numbers: reference values, docs/22 §4
constexpr double kFluxK = 2.5;
constexpr double kFluxDeltaDb = 3.0;
constexpr double kBeatRiseDb = 3.0;
constexpr int kBeatWaitFrames = 8;                  // 160 ms, as starfield.cpp's cooldown
constexpr int kClipLevel = 32700;
constexpr int kClipHoldFrames = 25;                 // 500 ms
constexpr int kSilentFrames = 100;                  // 2 s
constexpr int kWaveDecim = 8, kWaveSpan = 1920;     // audio_spectrum.py
constexpr double kWaveAgcDecay = 0.55, kWaveAgcFloor = 0.02, kWaveGain = 118.0;
constexpr double kBpmMinS = 0.30, kBpmMaxS = 1.00;

// numpy's clip(v, 0, 255).astype(uint8): truncation after the clip.
uint8_t toByte(double v) {
  if (!(v > 0.0)) return 0;
  if (v >= 255.0) return 255;
  return (uint8_t)v;
}

double medianOf(double *v, int n) {
  std::sort(v, v + n);
  return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

bool Dsp::begin(AllocFn alloc) {
  ring_ = static_cast<float *>(alloc(kN * sizeof(float)));
  win_ = static_cast<float *>(alloc(kN * sizeof(float)));
  re_ = static_cast<float *>(alloc(kN * sizeof(float)));
  im_ = static_cast<float *>(alloc(kN * sizeof(float)));
  cos_ = static_cast<float *>(alloc((kN / 2) * sizeof(float)));
  sin_ = static_cast<float *>(alloc((kN / 2) * sizeof(float)));
  rev_ = static_cast<uint16_t *>(alloc(kN * sizeof(uint16_t)));
  if (!ring_ || !win_ || !re_ || !im_ || !cos_ || !sin_ || !rev_) return false;

  double sum = 0.0;
  for (int i = 0; i < kN; i++) {
    win_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kN - 1)));   // numpy.hanning
    sum += (double)win_[i];
    ring_[i] = 0.0f;
  }
  norm_ = 2.0 / sum;                                                     // full-scale sine = 0 dBFS
  for (int i = 0; i < kN / 2; i++) {
    cos_[i] = (float)std::cos(2.0 * kPi * i / kN);
    sin_[i] = (float)std::sin(2.0 * kPi * i / kN);
  }
  int bits = 0;
  while ((1 << bits) < kN) bits++;
  for (int i = 0; i < kN; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++)
      if ((i >> b) & 1) r |= 1 << (bits - 1 - b);
    rev_[i] = (uint16_t)r;
  }
  // audio_spectrum.py: lo = int(edge / bin_hz), hi = max(lo + 1, int(next_edge / bin_hz)).
  const double binHz = (double)kFs / kN;
  for (int b = 0; b < kBands; b++) {
    const double e0 = kFLo * std::pow(kFHi / kFLo, (double)b / kBands);
    const double e1 = kFLo * std::pow(kFHi / kFLo, (double)(b + 1) / kBands);
    const int lo = (int)(e0 / binHz);
    binLo_[b] = (uint16_t)lo;
    binHi_[b] = (uint16_t)std::max(lo + 1, (int)(e1 / binHz));
  }
  ref_ = kAgcFloorDb;
  waveRef_ = kWaveAgcFloor;
  sinceBeat_ = kBeatWaitFrames;
  std::memset(wave_, 128, sizeof(wave_));
  return true;
}

void Dsp::fft() {
  for (int i = 0; i < kN; i++) {
    const int j = rev_[i];
    if (j > i) {
      std::swap(re_[i], re_[j]);
      std::swap(im_[i], im_[j]);
    }
  }
  for (int size = 2; size <= kN; size <<= 1) {
    const int half = size >> 1, step = kN / size;
    for (int start = 0; start < kN; start += size) {
      for (int j = 0; j < half; j++) {
        const float c = cos_[j * step], s = sin_[j * step];
        const int a = start + j, b = a + half;
        const float tr = re_[b] * c + im_[b] * s;
        const float ti = im_[b] * c - re_[b] * s;
        re_[b] = re_[a] - tr;
        im_[b] = im_[a] - ti;
        re_[a] += tr;
        im_[a] += ti;
      }
    }
  }
}

bool Dsp::processHop(const int16_t *stereo, Frame &out) {
  // Mono into the ring, level and clipping from this hop.
  std::memmove(ring_, ring_ + kHop, (kN - kHop) * sizeof(float));
  float *tail = ring_ + (kN - kHop);
  int peakAbs = 0;
  double sumSq = 0.0;
  for (int i = 0; i < kHop; i++) {
    const int l = stereo[2 * i], r = stereo[2 * i + 1];
    const float m = ((float)l + (float)r) * (0.5f / 32768.0f);
    tail[i] = m;
    sumSq += (double)m * (double)m;
    peakAbs = std::max(peakAbs, std::max(std::abs(l), std::abs(r)));
  }
  filled_ += kHop;
  clipHold_ = peakAbs >= kClipLevel ? kClipHoldFrames : std::max(0, clipHold_ - 1);
  if (filled_ < (uint64_t)kN) return false;
  const double dt = (double)kHop / kFs;

  // Spectrum and bands.
  for (int i = 0; i < kN; i++) {
    re_[i] = ring_[i] * win_[i];
    im_[i] = 0.0f;
  }
  fft();
  double maxDb = -1e300;
  for (int b = 0; b < kBands; b++) {
    double acc = 0.0;
    for (int i = binLo_[b]; i < binHi_[b]; i++) {
      const double mag = std::sqrt((double)re_[i] * re_[i] + (double)im_[i] * im_[i]) * norm_;
      acc += mag * mag;
    }
    bandDb_[b] = 20.0 * std::log10(std::sqrt(acc / (binHi_[b] - binLo_[b])) + 1e-7);
    maxDb = std::max(maxDb, bandDb_[b]);
  }

  // Gate on the hop's RMS, then the AGC reference.
  const double levelDb = std::max(-120.0, 20.0 * std::log10(std::max(std::sqrt(sumSq / kHop), 1e-7)));
  gateHold_ = levelDb >= (double)cfg_.gateDb ? kGateHoldFrames : std::max(0, gateHold_ - 1);
  const bool gated = gateHold_ == 0;
  gatedRun_ = gated ? gatedRun_ + 1 : 0;
  double ref;
  if (cfg_.agc) {
    const double decayed = ref_ - kAgcDecayDbPerS * dt;
    ref_ = gated ? std::max(decayed, kAgcFloorDb) : std::max(std::max(decayed, maxDb), kAgcFloorDb);
    ref = ref_;
  } else {
    ref = kFixedRefDb;
  }
  const double loDb = ref - kAgcRangeDb;
  for (int b = 0; b < kBands; b++)
    out.bands[b] = gated ? 0 : toByte((bandDb_[b] - loDb) / kAgcRangeDb * 255.0);

  // Beats: bass flux against the last second.
  double x[kFluxBands];
  double bassDb = 0.0;
  for (int i = 0; i < kFluxBands; i++) {
    x[i] = std::max(bandDb_[i], kFluxFloorDb);
    bassDb += x[i];
  }
  bassDb /= kFluxBands;
  double flux = 0.0;
  if (havePrev_ && !gated) {
    for (int i = 0; i < kFluxBands; i++) flux += std::max(0.0, x[i] - prevX_[i]);
    flux /= kFluxBands;
  }
  std::memcpy(prevX_, x, sizeof(x));
  havePrev_ = true;
  double mean = 0.0, sd = 0.0;
  bool riseOk = true;
  if (histLen_ > 0) {
    double bassAvg = 0.0;
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
  const double thr = std::max(kFluxDeltaDb, mean + kFluxK * sd);
  sinceBeat_++;
  const bool beat = !gated && flux >= thr && flux > prevFlux_ && sinceBeat_ >= kBeatWaitFrames && riseOk;
  fluxHist_[histPos_] = flux;
  bassHist_[histPos_] = bassDb;
  histPos_ = (histPos_ + 1) % kHistory;
  if (histLen_ < kHistory) histLen_++;
  prevFlux_ = flux;

  const double t = (double)filled_ / kFs;
  float strength = 0.0f;
  if (beat) {
    sinceBeat_ = 0;
    beats_++;
    strength = (float)std::min(1.0, (flux - thr) / std::max(thr, 1e-3));
    if (beatCount_ == 9) {
      std::memmove(beatTimes_, beatTimes_ + 1, 8 * sizeof(double));
      beatCount_ = 8;
    }
    beatTimes_[beatCount_++] = t;
    double iv[8];
    int n = 0;
    for (int i = 1; i < beatCount_; i++) {
      const double d = beatTimes_[i] - beatTimes_[i - 1];
      if (d >= kBpmMinS && d <= kBpmMaxS) iv[n++] = d;
    }
    if (n >= 3) bpm_ = 60.0 / medianOf(iv, n);
  }
  if (beatCount_ > 0 && t - beatTimes_[beatCount_ - 1] > 3.0) bpm_ = 0.0;

  // Levels and peak holds for the effects that want more than bytes.
  const double ca = 1.0 - std::exp(-dt / kAttackS), cr = 1.0 - std::exp(-dt / kReleaseS);
  for (int b = 0; b < kBands; b++) {
    const float target = out.bands[b] / 255.0f;
    level_[b] += (target - level_[b]) * (float)(target > level_[b] ? ca : cr);
    if (level_[b] >= peak_[b]) {
      peak_[b] = level_[b];
      peakHold_[b] = (float)kPeakHoldS;
      peakVel_[b] = 0.0f;
    } else if (peakHold_[b] > 0.0f) {
      peakHold_[b] -= (float)dt;
    } else {
      peakVel_[b] += (float)(kPeakGravity * dt);
      peak_[b] = std::max(level_[b], peak_[b] - peakVel_[b] * (float)dt);
    }
  }

  // Every other frame: the trigger-aligned waveform and a fresh packet.
  const bool published = (k_ % kPublishEvery) == 0;
  if (published) {
    constexpr int kLow = kWaveSpan / kWaveDecim;
    const float *src = ring_ + (kN - kWaveSpan);
    for (int p = 0; p < kLow; p++) {
      double s = 0.0;
      for (int q = 0; q < kWaveDecim; q++) s += (double)src[p * kWaveDecim + q];
      waveLow_[p] = s / kWaveDecim;
    }
    int start = 0;
    for (int i = 0; i < kLow - kWavePoints; i++) {
      if (waveLow_[i] <= 0.0 && waveLow_[i + 1] > 0.0) {
        start = i + 1;
        break;
      }
    }
    double pk = 0.0;
    for (int p = 0; p < kWavePoints; p++) pk = std::max(pk, std::fabs(waveLow_[start + p]));
    const double dtw = (double)kHop * kPublishEvery / kFs;
    waveRef_ = std::max(std::max(waveRef_ * std::pow(kWaveAgcDecay, dtw), pk), kWaveAgcFloor);
    for (int p = 0; p < kWavePoints; p++) wave_[p] = toByte(waveLow_[start + p] / waveRef_ * kWaveGain + 128.0);
    packets_++;
    std::memcpy(out.packet, "FFT1", 4);
    std::memcpy(out.packet + 4, out.bands, kBands);
    std::memcpy(out.packet + 4 + kBands, wave_, kWavePoints);
  }

  std::memcpy(out.wave, wave_, kWavePoints);
  std::memcpy(out.level, level_, sizeof(level_));
  std::memcpy(out.peak, peak_, sizeof(peak_));
  float bass = 0.0f, mid = 0.0f, treble = 0.0f;
  for (int b = 0; b < 8; b++) bass += level_[b];
  for (int b = 8; b < 24; b++) mid += level_[b];
  for (int b = 24; b < kBands; b++) treble += level_[b];
  out.bass = bass / 8.0f;
  out.mid = mid / 16.0f;
  out.treble = treble / 8.0f;
  out.k = k_;
  out.samples = (uint32_t)filled_;
  out.packets = packets_;
  out.beats = beats_;
  out.levelDb = (float)levelDb;
  out.refDb = (float)ref;
  out.flux = (float)flux;
  out.thr = (float)thr;
  out.strength = strength;
  out.bpm = (float)bpm_;
  out.published = published;
  out.gated = gated;
  out.silent = gatedRun_ >= kSilentFrames;
  out.clipping = clipHold_ > 0;
  out.beat = beat;
  k_++;
  return true;
}

}  // namespace audiodsp

#endif

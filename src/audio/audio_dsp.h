/*
 * The onboard-microphone DSP chain: 32 bands, noise gate, AGC, smoothed levels,
 * peak holds, beats and the scope waveform, from 48 kHz stereo int16 hops.
 *
 * Portable C++: no Arduino, no FreeRTOS. tools/audiofx/host builds this same
 * file on the host and holds it frame by frame to tools/audiofx/dsp.py, the
 * reference the previews are rendered with.
 *
 * The packet it fills is byte for byte the PC companion's "FFT1" packet
 * (src/viz/visualizer.h, VIZ_WAVE_PACKET_LEN), so every existing effect draws
 * the microphone without a change. docs/22 in the KB has the design and the
 * source of every number.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace audiodsp {

constexpr int kFs = 48000;          // 256 fs = 12.288 MHz MCLK, a pair es7210.c lists for 48 kHz
constexpr int kN = 2048;            // Hann window and FFT, 23.4 Hz bins: the companion's FFT_N
constexpr int kHop = 960;           // 20 ms, 50 frames a second
constexpr int kBands = 32;          // VIZ_BANDS
constexpr int kWavePoints = 128;    // VIZ_WAVE_POINTS
constexpr int kPacketLen = 4 + kBands + kWavePoints;
constexpr int kPublishEvery = 2;    // a packet every 40 ms: the companion's cadence

struct Config {
  bool agc = true;
  float gateDb = -60.0f;            // frame RMS, dBFS
};

struct Frame {
  uint32_t k = 0;                   // analysis frame number
  uint32_t samples = 0;             // input frames consumed when this one was made (wraps after 24 h)
  uint32_t packets = 0;             // packets built so far; changes when packet[] does
  uint32_t beats = 0;
  uint8_t bands[kBands] = {};       // this frame's band bytes, 0..255
  uint8_t wave[kWavePoints] = {};   // the latest waveform, centred on 128
  uint8_t packet[kPacketLen] = {};  // "FFT1" + bands + wave, as of the last published frame
  float level[kBands] = {};         // 0..1, attack 12 ms / release 200 ms
  float peak[kBands] = {};          // 0..1, held 350 ms then falling
  float bass = 0.0f, mid = 0.0f, treble = 0.0f;   // mean level of bands 0-7, 8-23, 24-31
  float levelDb = -120.0f;          // hop RMS, dBFS
  float refDb = 0.0f;               // AGC reference: a band here draws full height
  float flux = 0.0f, thr = 0.0f;    // bass spectral flux and its threshold, dB
  float strength = 0.0f;            // 0..1 on a beat frame
  float bpm = 0.0f;                 // 0 when unknown
  bool published = false;           // packet[] was rebuilt by this frame
  bool gated = true;                // below the noise gate: bands are zero
  bool silent = false;              // gated for 2 s
  bool clipping = false;            // a sample at full scale in the last 500 ms
  bool beat = false;
};

using AllocFn = void *(*)(size_t bytes);

class Dsp {
 public:
  static constexpr int kFluxBands = 10;   // bands 0..9, 50..~300 Hz
  static constexpr int kHistory = 50;     // one second

  // About 45 KB of tables and work arrays, from alloc (PSRAM on the panel).
  bool begin(AllocFn alloc);
  void setConfig(const Config &c) { cfg_ = c; }
  const Config &config() const { return cfg_; }

  // kHop frames of interleaved L, R. True when out holds a new frame: from the
  // third hop on, once the window has kN samples.
  bool processHop(const int16_t *stereo, Frame &out);

  // A band's FFT bin span [lo, hi), for the host test.
  void bandBins(int band, int &lo, int &hi) const { lo = binLo_[band]; hi = binHi_[band]; }

 private:
  void fft();

  Config cfg_;
  float *ring_ = nullptr, *win_ = nullptr, *re_ = nullptr, *im_ = nullptr;
  float *cos_ = nullptr, *sin_ = nullptr;
  uint16_t *rev_ = nullptr;
  uint16_t binLo_[kBands] = {}, binHi_[kBands] = {};
  double norm_ = 0.0;

  uint64_t filled_ = 0;
  uint32_t k_ = 0, packets_ = 0, beats_ = 0;
  double ref_ = 0.0;
  int gateHold_ = 0, gatedRun_ = 0, clipHold_ = 0, sinceBeat_ = 0;
  double bandDb_[kBands] = {};
  double prevX_[kFluxBands] = {};
  bool havePrev_ = false;
  double fluxHist_[kHistory] = {}, bassHist_[kHistory] = {};
  int histLen_ = 0, histPos_ = 0;
  double prevFlux_ = 0.0;
  double beatTimes_[9] = {};
  int beatCount_ = 0;
  double bpm_ = 0.0;
  float level_[kBands] = {}, peak_[kBands] = {}, peakHold_[kBands] = {}, peakVel_[kBands] = {};
  double waveRef_ = 0.0;
  double waveLow_[240] = {};              // kept off the task stack
  uint8_t wave_[kWavePoints] = {};
};

}  // namespace audiodsp

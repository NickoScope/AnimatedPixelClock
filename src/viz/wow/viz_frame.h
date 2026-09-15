/*
 * The frame visualizer styles 7-14 draw from, and how a PC packet becomes one.
 *
 * From the microphones every DSP frame (20 ms) is one of these, copied from
 * audiodsp::Frame. The PC companion's packet carries only band bytes and the
 * waveform, so PcFrameDeriver rebuilds the rest from them every 40 ms with the
 * DSP's own constants. What that costs is in docs/22 §9 of the KB.
 *
 * Portable C++, compiled on the host by tools/audiofx/host.
 */
#pragma once

#include <cstdint>

namespace wow {

constexpr int kBands = 32;          // VIZ_BANDS
constexpr int kWavePoints = 128;    // VIZ_WAVE_POINTS

struct VizFrame {
  uint8_t bands[kBands];      // 0..255, as in the FFT1 packet
  uint8_t wave[kWavePoints];  // centred on 128
  float level[kBands];        // 0..1, attack 12 ms / release 200 ms
  float peak[kBands];         // 0..1, held 350 ms then falling
  float bass, mid, treble;    // mean level of bands 0-7, 8-23, 24-31
  float strength;             // 0..1 on a beat frame
  uint8_t beat;
  uint8_t clipping;           // always 0 from a PC packet
  uint8_t steps;              // DSP frames this one stands for: 1 from the mic, 2 from a PC packet
  uint8_t reserved;
};

class PcFrameDeriver {
 public:
  void reset() { *this = PcFrameDeriver(); }

  // One packet's 32 band bytes and 128 waveform bytes; dt is the time since the
  // previous packet, clamped to 10..100 ms.
  void feed(const uint8_t *bands, const uint8_t *wave, float dt, VizFrame &out);

 private:
  static constexpr int kFluxBands = 10;
  static constexpr int kHistory = 25;   // one second of packets
  float prev_[kFluxBands] = {};
  bool havePrev_ = false;
  float fluxHist_[kHistory] = {}, bassHist_[kHistory] = {};
  int histLen_ = 0, histPos_ = 0;
  float prevFlux_ = 0.0f;
  float sinceBeat_ = 1.0f;
  float level_[kBands] = {}, peak_[kBands] = {}, peakHold_[kBands] = {}, peakVel_[kBands] = {};
};

}  // namespace wow

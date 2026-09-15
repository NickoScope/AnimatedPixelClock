/*
 * Renders visualizer styles 7-14 (src/viz/wow/) from a WAV, the way
 * tools/audiofx/render.py drives effects_wow.py, so compare_wow.py can hold
 * the two to each other pixel for pixel.
 *
 *   build/test_wow_d <wav> <font.json> <outdir> [--pc]
 *
 * Runs the firmware's DSP (src/audio/audio_dsp.cpp) over the WAV and writes
 *   outdir/frames.bin     every DSP frame as a VizFrame, with its time: the Python side reads these
 *   outdir/wow_<k>.raw    effect k (0..7): 60 Hz frames, 128x64 uint16 RGB565, little-endian
 * With --pc the effects are fed instead from the DSP's 40 ms packets through
 * PcFrameDeriver, as a PC companion's would be (outdir/pc_wow_<k>.raw), and the
 * beats it finds are printed.
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio_dsp.h"
#include "viz_frame.h"
#include "wav_io.h"
#include "wow.h"
#include "wow_internal.h"

namespace {

constexpr int kFps = 60;

void *hostAlloc(size_t n) { return calloc(1, n); }

std::vector<uint8_t> loadFont(const char *path) {
  std::vector<uint8_t> font;
  FILE *f = fopen(path, "rb");
  if (!f) return font;
  int ch, v = 0;
  bool in = false;
  while ((ch = fgetc(f)) != EOF) {
    if (ch >= '0' && ch <= '9') {
      v = v * 10 + (ch - '0');
      in = true;
    } else if (in) {
      font.push_back((uint8_t)v);
      v = 0;
      in = false;
    }
  }
  fclose(f);
  return font;
}

// gfx.Canvas: an array, and the classic font drawn as gfx.py draws it.
class HostCanvas : public wow::Canvas {
 public:
  explicit HostCanvas(const std::vector<uint8_t> &font) : font_(font) { clear(); }
  void clear() { memset(px, 0, sizeof(px)); }
  void setPixel(int x, int y, uint16_t c) override { px[y * wow::kW + x] = c; }
  void glyph(int x, int y, unsigned char ch, uint16_t c) override {
    int code = ch;
    if (code >= 176) code++;
    for (int i = 0; i < 5; i++) {
      const size_t at = (size_t)code * 5 + i;
      uint8_t bits = at < font_.size() ? font_[at] : 0;
      for (int j = 0; j < 8; j++, bits >>= 1)
        if (bits & 1) pixel(x + i, y + j, c);
    }
  }
  uint16_t px[wow::kW * wow::kH];

 private:
  const std::vector<uint8_t> &font_;
};

wow::VizFrame fromDsp(const audiodsp::Frame &fr) {
  wow::VizFrame v;
  memcpy(v.bands, fr.bands, sizeof(v.bands));
  memcpy(v.wave, fr.wave, sizeof(v.wave));
  memcpy(v.level, fr.level, sizeof(v.level));
  memcpy(v.peak, fr.peak, sizeof(v.peak));
  v.bass = fr.bass;
  v.mid = fr.mid;
  v.treble = fr.treble;
  v.strength = fr.strength;
  v.beat = fr.beat;
  v.clipping = fr.clipping;
  v.steps = 1;
  v.reserved = 0;
  return v;
}

struct Timed {
  double t;
  wow::VizFrame f;
};

void writeFrames(const std::string &path, const std::vector<Timed> &frames) {
  FILE *o = fopen(path.c_str(), "wb");
  const uint32_t n = (uint32_t)frames.size();
  fwrite("WOWF", 1, 4, o);
  fwrite(&n, 4, 1, o);
  for (const Timed &t : frames) {
    fwrite(&t.t, 8, 1, o);
    fwrite(t.f.bands, 1, wow::kBands, o);
    fwrite(t.f.wave, 1, wow::kWavePoints, o);
    fwrite(t.f.level, 4, wow::kBands, o);
    fwrite(t.f.peak, 4, wow::kBands, o);
    const float tail[4] = {t.f.bass, t.f.mid, t.f.treble, t.f.strength};
    fwrite(tail, 4, 4, o);
    const uint8_t flags[4] = {t.f.beat, t.f.clipping, t.f.steps, 0};
    fwrite(flags, 1, 4, o);
  }
  fclose(o);
}

// render.py's loop for a "wow" effect, without the clock.
void renderEffect(wow::Engine &eng, int k, const std::vector<Timed> &frames, double seconds,
                  HostCanvas &cv, const std::string &path) {
  FILE *o = fopen(path.c_str(), "wb");
  eng.reset(k, (wow::real)1.0);
  size_t fi = 0;
  const int total = (int)(seconds * kFps);
  double sumUs = 0.0, maxUs = 0.0;
  for (int n = 0; n < total; n++) {
    const double now = n * 1000.0 / kFps;
    while (fi < frames.size() && frames[fi].t * 1000.0 <= now) eng.update(frames[fi++].f);
    cv.clear();
    const auto t0 = std::chrono::steady_clock::now();
    eng.render(cv, (wow::real)(1.0 / kFps));
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    sumUs += us;
    if (us > maxUs) maxUs = us;
    fwrite(cv.px, 2, wow::kW * wow::kH, o);
  }
  fclose(o);
  printf("  render effect %d (%s): mean %.1f us, max %.1f us per frame on this host\n", k, wow::kNames[k],
         sumUs / total, maxUs);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s file.wav glcdfont.json outdir [--pc]\n", argv[0]);
    return 2;
  }
  const bool pc = argc > 4 && !strcmp(argv[4], "--pc");
  std::vector<int16_t> st;
  if (!readWav(argv[1], audiodsp::kFs, st)) {
    fprintf(stderr, "%s: needs 48 kHz 16-bit PCM\n", argv[1]);
    return 1;
  }
  const std::vector<uint8_t> font = loadFont(argv[2]);
  if (font.size() < 1275) {
    fprintf(stderr, "%s: font not read\n", argv[2]);
    return 1;
  }
  const std::string out = argv[3];

  static audiodsp::Dsp dsp;
  static audiodsp::Frame fr;
  if (!dsp.begin(hostAlloc)) return 1;
  std::vector<Timed> frames;
  wow::PcFrameDeriver deriver;
  const size_t count = st.size() / 2;
  int pcBeats = 0;
  for (size_t i = 0; i + audiodsp::kHop <= count; i += audiodsp::kHop) {
    if (!dsp.processHop(&st[2 * i], fr)) continue;
    const double t = fr.samples / (double)audiodsp::kFs;
    if (!pc) {
      frames.push_back({t, fromDsp(fr)});
    } else if (fr.published) {
      Timed tf;
      tf.t = t;
      deriver.feed(fr.packet + 4, fr.packet + 4 + audiodsp::kBands, 0.04f, tf.f);
      if (tf.f.beat) {
        printf("%s%.3f", pcBeats ? ", " : "pc beats at ", t);
        pcBeats++;
      }
      frames.push_back(tf);
    }
  }
  if (pc) printf("%s(%d)\n", pcBeats ? " " : "pc beats: none ", pcBeats);
  writeFrames(out + "/frames.bin", frames);   // with --pc, the frames PcFrameDeriver made

  static wow::Engine eng;
  if (!eng.begin(hostAlloc)) return 1;
  static HostCanvas cv(font);
  const double seconds = count / (double)audiodsp::kFs;
  for (int k = 0; k < wow::kEffects; k++)
    renderEffect(eng, k, frames, seconds, cv, out + (pc ? "/pc_wow_" : "/wow_") + std::to_string(k) + ".raw");
  printf("%zu frames in, %d effects x %d display frames out, real = %s\n", frames.size(), wow::kEffects,
         (int)(seconds * kFps), sizeof(wow::real) == 8 ? "double" : "float");
  const size_t buffers = (size_t)wow::kW * wow::kH * 5 + wow::kMaxParts * sizeof(wow::Particle);
  printf("engine memory: state %zu B + buffers %zu B (spectrogram, particle canvas, particle pool, afterglow); "
         "VizFrame %zu B, PcFrameDeriver %zu B (host, 64-bit pointers)\n",
         sizeof(wow::State), buffers, sizeof(wow::VizFrame), sizeof(wow::PcFrameDeriver));
  return 0;
}

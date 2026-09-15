/*
 * Runs the firmware's DSP (src/audio/audio_dsp.cpp, compiled for this machine)
 * over a WAV and prints the CSV tools/audiofx/dsp.py writes, so compare.py can
 * hold the two to each other frame by frame.
 *
 *   build/test_dsp ../wav/kick120.wav > out.csv
 *   build/test_dsp --bins                          the band -> FFT bin table
 */
#include "audio_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void *hostAlloc(size_t n) { return calloc(1, n); }

// PCM 16-bit, 48 kHz, one or two channels; mono is duplicated as dsp.py does.
static bool readWav(const char *path, std::vector<int16_t> &stereo) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char id[4], kind[4];
  uint32_t size = 0;
  if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) || fread(&size, 4, 1, f) != 1 ||
      fread(kind, 1, 4, f) != 4 || memcmp(kind, "WAVE", 4)) {
    fclose(f);
    return false;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  bool haveFmt = false;
  while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
    if (!memcmp(id, "fmt ", 4)) {
      uint8_t buf[40] = {};
      const size_t n = size < sizeof(buf) ? size : sizeof(buf);
      if (fread(buf, 1, n, f) != n) break;
      if (size > n) fseek(f, (long)(size - n), SEEK_CUR);
      memcpy(&format, buf, 2);
      memcpy(&channels, buf + 2, 2);
      memcpy(&rate, buf + 4, 4);
      memcpy(&bits, buf + 14, 2);
      haveFmt = true;
    } else if (!memcmp(id, "data", 4)) {
      if (!haveFmt || format != 1 || bits != 16 || rate != (uint32_t)audiodsp::kFs || (channels != 1 && channels != 2)) break;
      std::vector<int16_t> raw(size / 2);
      raw.resize(fread(raw.data(), 2, raw.size(), f));
      const size_t frames = raw.size() / channels;
      stereo.resize(frames * 2);
      for (size_t i = 0; i < frames; i++) {
        stereo[2 * i] = raw[i * channels];
        stereo[2 * i + 1] = raw[i * channels + (channels - 1)];
      }
      fclose(f);
      return true;
    } else {
      fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
  }
  fclose(f);
  return false;
}

int main(int argc, char **argv) {
  static audiodsp::Dsp dsp;
  if (!dsp.begin(hostAlloc)) {
    fprintf(stderr, "allocation failed\n");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "--bins")) {
    for (int b = 0; b < audiodsp::kBands; b++) {
      int lo, hi;
      dsp.bandBins(b, lo, hi);
      printf("%d,%d\n", lo, hi);
    }
    return 0;
  }
  if (argc != 2) {
    fprintf(stderr, "usage: %s file.wav | --bins\n", argv[0]);
    return 2;
  }
  std::vector<int16_t> st;
  if (!readWav(argv[1], st)) {
    fprintf(stderr, "%s: needs 48 kHz 16-bit PCM\n", argv[1]);
    return 1;
  }
  printf("k,t_ms,gated,beat,clip,silent,published,level_db,ref_db,flux,thr,bpm");
  for (int b = 0; b < audiodsp::kBands; b++) printf(",b%d", b);
  for (int w = 0; w < audiodsp::kWavePoints; w++) printf(",w%d", w);
  printf("\n");
  static audiodsp::Frame fr;
  const size_t frames = st.size() / 2;
  for (size_t i = 0; i + audiodsp::kHop <= frames; i += audiodsp::kHop) {
    if (!dsp.processHop(&st[2 * i], fr)) continue;
    printf("%u,%ld,%d,%d,%d,%d,%d,%.2f,%.2f,%.3f,%.3f,%.1f", fr.k, lround(fr.samples * 1000.0 / audiodsp::kFs),
           fr.gated, fr.beat, fr.clipping, fr.silent, fr.published, fr.levelDb, fr.refDb, fr.flux, fr.thr, fr.bpm);
    for (int b = 0; b < audiodsp::kBands; b++) printf(",%u", fr.bands[b]);
    for (int w = 0; w < audiodsp::kWavePoints; w++) printf(",%u", fr.wave[w]);
    printf("\n");
  }
  return 0;
}

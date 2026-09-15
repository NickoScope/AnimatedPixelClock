// PCM 16-bit WAV reader for the host tests. Mono is duplicated to stereo, as dsp.py does.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

inline bool readWav(const char *path, int wantRate, std::vector<int16_t> &stereo) {
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
      if (!haveFmt || format != 1 || bits != 16 || rate != (uint32_t)wantRate || (channels != 1 && channels != 2)) break;
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

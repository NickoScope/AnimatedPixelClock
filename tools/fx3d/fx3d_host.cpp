// Renders one fx3d scene on the Mac with the firmware's own headers, for the
// previews: tools/fx3d/render.py builds and drives this.
//
//   fx3d_host <scene> <mode 0-3> <frames> <fps> <seed> <out.bin> [calib page, -1 walks them]
//
// out.bin: "FX3D", u16 width, u16 height, u32 frames, u8 mode, u8 eyes, u16 0,
// then per frame the 128x64x3 codes drawPixelRGB888() would get and, when eyes
// is 1, the left and right linear planes. One JSON line of timings on stdout.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fx3d_catalog.h"

using namespace fx3d;

int main(int argc, char **argv) {
  if (argc < 7) {
    std::fprintf(stderr, "usage: %s scene mode frames fps seed out.bin [page]\n", argv[0]);
    return 2;
  }
  const int idx = catalogFind(argv[1]);
  if (idx < 0) {
    std::fprintf(stderr, "no scene %s\n", argv[1]);
    return 2;
  }
  const int mode = std::atoi(argv[2]), frames = std::atoi(argv[3]);
  const float fps = (float)std::atof(argv[4]);
  const uint32_t seed = (uint32_t)std::strtoul(argv[5], nullptr, 10);
  const int page = argc > 7 ? std::atoi(argv[7]) : -1;
  if (mode < 0 || mode >= MODE_COUNT || frames <= 0 || !(fps > 0.0f)) return 2;

  std::vector<uint8_t> rgb(kPixels * 3), left(kPixels), right(kPixels), codes(kPixels * 3);
  std::vector<float> z(kPixels);
  Ctx c;
  c.fb.rgb = rgb.data();
  c.fb.eye[0] = left.data();
  c.fb.eye[1] = right.data();
  c.fb.z = z.data();
  c.st.mode = (uint8_t)mode;

  std::vector<unsigned char> mem(kCatalog[idx].bytes + 16);
  Scene *s = kCatalog[idx].make(mem.data());
  if (!std::strcmp(argv[1], "calib")) {
    CalibScene *cs = static_cast<CalibScene *>(s);
    if (page < 0) cs->autoAdvance = true;
    else cs->page = page % CalibScene::kPages;
  }
  s->reset(seed);

  FILE *f = std::fopen(argv[6], "wb");
  if (!f) return 1;
  const uint8_t eyes = mode != MODE_MONO ? 1 : 0;
  unsigned char hdr[16] = {'F', 'X', '3', 'D'};
  hdr[4] = kW & 0xff; hdr[5] = kW >> 8; hdr[6] = kH & 0xff; hdr[7] = kH >> 8;
  hdr[8] = frames & 0xff; hdr[9] = (frames >> 8) & 0xff; hdr[10] = (frames >> 16) & 0xff; hdr[11] = (frames >> 24) & 0xff;
  hdr[12] = (uint8_t)mode; hdr[13] = eyes;
  std::fwrite(hdr, 1, sizeof hdr, f);

  // The previews' clock: an invented time, running in step with the frames.
  Wall w;
  w.valid = true;
  float clock = 10 * 3600.0f + 8 * 60.0f + 30.0f;
  const Encoder enc;
  double sum = 0.0, worst = 0.0;
  for (int n = 0; n < frames; n++) {
    const float dt = 1.0f / fps;
    clock += dt;
    int secs = (int)clock;
    w.hour = (secs / 3600) % 24;
    w.minute = (secs / 60) % 60;
    w.second = secs % 60;
    w.sub = clock - (float)secs;
    w.yday = 260;
    w.utcHours = std::fmod(clock / 3600.0f, 24.0f);
    auto t0 = std::chrono::steady_clock::now();
    renderFrame(*s, c, n == 0 ? 0.0f : dt, w);
    for (int i = 0; i < kPixels * 3; i++) codes[i] = enc.code[rgb[i]];
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    sum += us;
    if (us > worst) worst = us;
    std::fwrite(codes.data(), 1, codes.size(), f);
    if (eyes) {
      std::fwrite(left.data(), 1, left.size(), f);
      std::fwrite(right.data(), 1, right.size(), f);
    }
  }
  std::fclose(f);
  s->~Scene();
  std::printf("{\"scene\":\"%s\",\"mode\":%d,\"frames\":%d,\"host_us_avg\":%.1f,\"host_us_max\":%.1f,\"scene_bytes\":%u}\n",
              argv[1], mode, frames, sum / frames, worst, (unsigned)kCatalog[idx].bytes);
  return 0;
}

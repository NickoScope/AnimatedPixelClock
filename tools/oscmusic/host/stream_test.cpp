// The card's read-ahead stream on the host (tools/oscmusic/test_clip_maker.py):
// src/clips/clip_stream.cpp with a reader thread and a render thread, the way
// clip_sd.cpp runs them on the panel. One line per check: "ok ..." or "no ...".
//   stream SCRATCH.pca
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <LittleFS.h>

#include "anim_store.h"
#include "clip_stream.h"

SerialStub Serial;
LittleFSStub LittleFS;

static int failures = 0;
static void say(bool ok, const char* what) {
  printf("%s %s\n", ok ? "ok" : "no", what);
  failures += !ok;
}

static const int N = 60;  // frames in the test clip

// Frame i carries i in its first two bytes and (i * 7) in the rest; delays
// alternate 40 and 60 ms, with one out of range to see the clamp.
static bool writeClip(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  uint8_t head[12] = {'P', 'C', 'A', '1', N, 0, 40, 0, 16, 1, 0, 0};
  fwrite(head, 1, 12, f);
  for (int i = 0; i < 16; i++) { uint16_t c = (uint16_t)(i * 4000); fwrite(&c, 2, 1, f); }
  for (int i = 0; i < N; i++) { uint16_t d = i == 3 ? 10 : (i % 2 ? 60 : 40); fwrite(&d, 2, 1, f); }
  std::vector<uint8_t> fr(PCA_FRAME_BYTES);
  for (int i = 0; i < N; i++) {
    std::fill(fr.begin(), fr.end(), (uint8_t)(i * 7));
    fr[0] = (uint8_t)i;
    fr[1] = (uint8_t)(i >> 8);
    fwrite(fr.data(), 1, fr.size(), f);
  }
  fclose(f);
  return true;
}

struct Run {
  int taken = 0, orderErrors = 0, delayErrors = 0, lastIndex = -1;
  unsigned underrunsSeen = 0;
};

// The render side at `frameMs` a frame for `frames` frames, the reader on its
// own thread filling 4 frames a pass and sleeping 1 ms when the ring is full.
static Run play(const char* path, int frames, int frameMs, long readUs, int stallAt, int stallMs, uint32_t* loops,
                unsigned* underruns, uint32_t* readMaxUs) {
  static uint8_t ring[(CLIP_RING_FRAMES + 1) * PCA_FRAME_BYTES];
  static uint16_t delays[1024];
  ClipStream s;
  s.init(ring, delays, 1024);
  File f(path);
  PcaHeader hdr{};
  Run run;
  if (!animValidatePcaMax(f, &hdr, 1024) || !s.begin(f, hdr)) { run.orderErrors = -1; return run; }
  std::atomic<bool> stop{false};
  readDelayUs() = readUs;
  std::thread reader([&] {
    int passes = 0;
    while (!stop) {
      if (stallAt >= 0 && ++passes == stallAt) std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
      if (!s.fill(f, 4)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  uint8_t frame[PCA_FRAME_BYTES];
  uint16_t ms = 0;
  bool late = false;
  auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);  // the ring fills first
  while (run.taken < frames) {
    std::this_thread::sleep_until(due);
    if (s.take(frame, &ms)) {
      const int idx = frame[0] | (frame[1] << 8);
      const int want = run.lastIndex < 0 ? 0 : (run.lastIndex + 1) % N;
      run.orderErrors += idx != want || frame[100] != (uint8_t)(idx * 7);
      const int wantMs = idx == 3 ? 34 : (idx % 2 ? 60 : 40);
      run.delayErrors += ms != wantMs;
      run.lastIndex = idx;
      run.taken++;
      late = false;
      due += std::chrono::milliseconds(frameMs);
    } else {
      if (!late) s.missed();  // counted once per frame that came late, as the player does
      late = true;
      due += std::chrono::milliseconds(1);
    }
  }
  stop = true;
  reader.join();
  readDelayUs() = 0;
  *loops = s.loops;
  *underruns = s.underruns;
  *readMaxUs = s.readUsMax;
  return run;
}

int main(int argc, char** argv) {
  if (argc < 2 || !writeClip(argv[1])) return 2;
  uint32_t loops = 0, maxUs = 0;
  unsigned under = 0;
  char line[160];

  // 1: the panel's measured worst read (6.44 ms) on every read, 25 fps, 2.5 laps.
  Run a = play(argv[1], 150, 40, 6440, -1, 0, &loops, &under, &maxUs);
  snprintf(line, sizeof line, "150 frames at 25 fps with every read 6.44 ms: order errors %d, delay errors %d, underruns %u, laps back to 0: %u",
           a.orderErrors, a.delayErrors, under, (unsigned)loops);
  say(a.orderErrors == 0 && a.delayErrors == 0 && under == 0 && loops >= 2, line);
  snprintf(line, sizeof line, "worst read timed at %.2f ms", maxUs / 1000.0);
  say(maxUs >= 6440, line);

  // 2: a 700 ms stall is inside the one-second ring: no frame late.
  Run b = play(argv[1], 100, 40, 2590, 12, 700, &loops, &under, &maxUs);
  snprintf(line, sizeof line, "a 700 ms reader stall at 25 fps: underruns %u, order errors %d", under, b.orderErrors);
  say(b.orderErrors == 0 && under == 0, line);

  // 3: a 2.5 s stall is not: frames come late, are counted, and none is skipped.
  Run c = play(argv[1], 100, 40, 2590, 12, 2500, &loops, &under, &maxUs);
  snprintf(line, sizeof line, "a 2.5 s reader stall: underruns %u counted, order errors %d (held, never skipped)", under, c.orderErrors);
  say(c.orderErrors == 0 && under > 0, line);
  return failures ? 1 : 0;
}

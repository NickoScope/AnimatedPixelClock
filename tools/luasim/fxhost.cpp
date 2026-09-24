// ============================================================
// fxhost.cpp - the firmware's Lua effect runtime, run on the host
// ============================================================
// Compiles src/lua/lua_fx.cpp, lua_px.cpp and nslua_sandbox.cpp - the files
// the panel runs - against a stand-in display, and drives them with exactly
// the clock luasim uses, so the two outputs can be compared byte for byte
// (fx_parity.py does). Same command line as luasim:
//
//   fxhost script.lua frames out.raw [--start HH:MM] [--yday N] [--utc H] [--year Y] [--sweep]
//          [--exact] [--incremental] [--panel-limits]
//
//   --exact         count instructions one by one (slow; for the numbers only)
//   --incremental   Lua's incremental GC instead of the panel's generational one
//   --panel-limits  run under the budgets the firmware uses, not generous ones
//
// One line of measurements goes to stderr, starting "fxhost:".
// ============================================================
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lua_fx.h"
#include "lua_px.h"

namespace {

struct FakeDisplay {
  uint8_t px[LUA_PX_BYTES];
  void drawPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= LUA_PX_W || y < 0 || y >= LUA_PX_H) return;
    uint8_t *p = &px[(y * LUA_PX_W + x) * 3];
    p[0] = r; p[1] = g; p[2] = b;
  }
};

double nowMsF() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Job {
  std::string script, out;
  int frames = 0, startMin = 12 * 60 + 34, yday = 255, utcH = 2, year = 2026;
  bool sweep = false, exact = false, gen = true, panel = false;
  int rc = 0;
};

void *run(void *arg) {
  Job &j = *static_cast<Job *>(arg);
  FILE *f = fopen(j.script.c_str(), "rb");
  if (!f) { perror("script"); j.rc = 1; return nullptr; }
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
  fclose(f);

  std::string name = j.script.substr(j.script.find_last_of('/') + 1);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) name.resize(name.size() - 4);

  static uint8_t canvasBytes[LUA_PX_BYTES];   // zeroed, like luasim's static fb
  LuaPxCanvas canvas = {canvasBytes, {0.0, j.startMin / 60 % 24, j.startMin % 60, 56, j.yday, j.utcH * 60, j.year}};
  const LuaFxLimits generous = {400000000u, 400000000u, 600000u, 600000u, 256u * 1024u * 1024u};
  LuaFx fx;
  if (j.exact) fx.setHookStep(1);
  fx.setGenerationalGc(j.gen);

  double t0 = nowMsF();
  if (!fx.open(name.c_str(), src.data(), src.size(), &canvas, j.panel ? kLuaFxPanelLimits : generous)) {
    fprintf(stderr, "script error: %s\n", fx.error());
    j.rc = 1;
    return nullptr;
  }
  const double loadMs = nowMsF() - t0;
  const uint32_t loadInstr = fx.lastInstructions();

  FILE *out = fopen(j.out.c_str(), "wb");
  if (!out) { perror("open"); j.rc = 1; return nullptr; }
  static FakeDisplay display;
  double drawSum = 0, drawMax = 0;
  uint64_t instrSum = 0;
  uint32_t instrMax = 0;
  for (int fr = 0; fr < j.frames; fr++) {
    LuaPxClock &c = canvas.clock;
    c.phase = j.frames > 1 ? (double)fr / (double)j.frames : 0.0;
    if (j.sweep) {
      const int m = (j.startMin + fr * 1440 / (j.frames > 0 ? j.frames : 1)) % 1440;
      c.hour = m / 60; c.min = m % 60; c.sec = 0;
    } else {
      c.sec = (56 + fr / 30) % 60;
    }
    t0 = nowMsF();
    if (!fx.draw()) {
      fprintf(stderr, "frame %d: %s\n", fr, fx.error());
      j.rc = 1;
      fclose(out);
      return nullptr;
    }
    const double ms = nowMsF() - t0;
    drawSum += ms;
    if (ms > drawMax) drawMax = ms;
    instrSum += fx.lastInstructions();
    if (fx.lastInstructions() > instrMax) instrMax = fx.lastInstructions();
    // FXHOST_FRAMES=1: every frame's cost, for finding the heavy scenes
    if (getenv("FXHOST_FRAMES")) fprintf(stderr, "fxframe %d %u %.3f\n", fr, fx.lastInstructions(), ms);
    luaPxBlit(canvasBytes, display);           // through the firmware's blit
    fwrite(display.px, 1, sizeof(display.px), out);
  }
  fclose(out);
  const size_t heapPeak = fx.heapPeak(), heapEnd = fx.heapBytes();
  fx.close();
  fprintf(stderr,
          "fxhost: %s frames %d load_ms %.2f load_instr %u draw_ms_avg %.3f draw_ms_max %.3f "
          "instr_avg %llu instr_max %u heap_peak %zu heap_end %zu heap_after_close %zu fps %u period %.1f\n",
          name.c_str(), j.frames, loadMs, loadInstr, j.frames ? drawSum / j.frames : 0.0, drawMax,
          (unsigned long long)(j.frames ? instrSum / j.frames : 0), instrMax, heapPeak, heapEnd,
          fx.heapBytes(), fx.fps(), fx.periodSeconds());
  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: fxhost script.lua frames out.raw [--start HH:MM] [--yday N] "
                    "[--utc H] [--year Y] [--sweep] [--exact] [--incremental] [--panel-limits]\n");
    return 2;
  }
  Job j;
  j.script = argv[1];
  j.frames = atoi(argv[2]);
  j.out = argv[3];
  for (int a = 4; a < argc; a++) {
    if (!strcmp(argv[a], "--start") && a + 1 < argc) {
      int hh = 0, mm = 0; sscanf(argv[++a], "%d:%d", &hh, &mm); j.startMin = hh * 60 + mm;
    } else if (!strcmp(argv[a], "--yday") && a + 1 < argc) j.yday = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--utc") && a + 1 < argc)    j.utcH = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--year") && a + 1 < argc)   j.year = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--sweep"))                  j.sweep = true;
    else if (!strcmp(argv[a], "--exact"))                  j.exact = true;
    else if (!strcmp(argv[a], "--incremental"))            j.gen = false;
    else if (!strcmp(argv[a], "--panel-limits"))           j.panel = true;
  }

  // Run on a thread whose stack we own and have painted, so the deepest C
  // stack the load and the frames reached can be read back afterwards. An
  // arm64 or x86 frame is not an Xtensa frame: this is a rough guide to the
  // effect task's stack, not a substitute for its high-water mark on the panel.
  const size_t kStack = 1024 * 1024;
  void *stack = mmap(nullptr, kStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (stack == MAP_FAILED) { perror("mmap"); return 1; }
  memset(stack, 0xA5, kStack);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_t th;
  if (pthread_attr_setstack(&attr, stack, kStack) != 0 || pthread_create(&th, &attr, run, &j) != 0) {
    fprintf(stderr, "thread with painted stack failed; running inline, no stack figure\n");
    run(&j);
    return j.rc;
  }
  pthread_join(th, nullptr);
  size_t untouched = 0;
  const uint8_t *s = static_cast<const uint8_t *>(stack);
  while (untouched < kStack && s[untouched] == 0xA5) untouched++;
  fprintf(stderr, "fxhost: stack_used %zu\n", kStack - untouched);
  return j.rc;
}

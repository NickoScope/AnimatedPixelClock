#include "nslua_bench.h"

#if defined(NSLUA_BENCH)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "../clocks/clocks.h"
#include "../config/config.h"
#include "../config/settings.h"
#include "../display/display.h"
#include "nslua.h"

// Three phases of PHASE_MS each, repeating, one serial report at the end of
// each:
//   A idle    the clock alone - the baseline
//   B inline  one allocation-heavy script at the start of every frame, in the
//             render task: what a Lua effect in the render path would cost
//   C task    the same script back to back in a task on core 0 while the clock
//             renders on core 1: the worst case for the shared flash/PSRAM bus
// The clock is held on Snake, which moves every frame, so the phases compare.
//
// nslua_run is single-flight (its instruction budget is file-static). B runs
// only in loop(), C only in the task, and the task is stopped and seen idle
// before any phase change, so the two never overlap.

static const uint32_t PHASE_MS = 20000;
// No collectgarbage(): the sandbox removes it, and the first bench run on the
// board failed every script on that last line after doing all the work. Every
// run gets a fresh lua_State that is closed afterwards, which frees it all.
static const char *const kScript =
    "local t = {} "
    "for i = 1, 3000 do t[i] = string.rep('x', 32) .. i end "
    "local s = table.concat(t, ',', 1, 500) "
    "return #s";
// Pure VM work, no allocation beyond the state itself.
static const char *const kCompute =
    "local x = 0 for i = 1, 50000 do x = x + i % 7 end return x";

struct Stats {
  uint32_t frames = 0;
  uint64_t renderUsSum = 0;
  uint32_t renderUsMax = 0;
  uint32_t gapUsMax = 0;
  uint32_t runs = 0, failures = 0, runUsMax = 0;
  uint64_t runUsSum = 0;
  size_t psramStart = 0, psramMin = SIZE_MAX, internalMin = SIZE_MAX;
};

static Stats    s;
static uint8_t  s_phase = 0;
static uint32_t s_phaseStart = 0;
static uint32_t s_lastLoopUs = 0;
static uint32_t s_frameUs = 0;

static volatile bool     s_taskRun = false, s_taskBusy = false;
static volatile uint32_t s_taskRuns = 0, s_taskFailures = 0, s_taskUsMax = 0;
static volatile uint64_t s_taskUsSum = 0;

static const char *const kPhaseName[3] = {"A idle  ", "B inline", "C task  "};
static char s_errLoop[96] = {0};   // first failure in B, written by loop()
static char s_errTask[96] = {0};   // first failure in C, written by the task only

static bool runOnce(uint32_t &us, char *firstErr, size_t len) {
  char err[96];
  const uint32_t t0 = micros();
  const bool ok = nslua_run(kScript, err, sizeof(err));
  us = micros() - t0;
  if (!ok && !firstErr[0]) strncpy(firstErr, err, len - 1);
  return ok;
}

static void benchTask(void *) {
  for (;;) {
    if (s_taskRun) {
      s_taskBusy = true;
      uint32_t us;
      const bool ok = runOnce(us, s_errTask, sizeof(s_errTask));
      s_taskRuns = s_taskRuns + 1;
      s_taskUsSum = s_taskUsSum + us;
      if (us > s_taskUsMax) s_taskUsMax = us;
      if (!ok) s_taskFailures = s_taskFailures + 1;
      s_taskBusy = false;
    }
    vTaskDelay(1);   // IDLE0 must run: its watchdog, and Wi-Fi shares this core
  }
}

static void stopTask() {
  s_taskRun = false;
  const uint32_t t0 = millis();
  while (s_taskBusy && millis() - t0 < 2000) delay(1);
}

static void beginPhase(uint8_t p) {
  stopTask();
  s = Stats();
  s_phase = p;
  s_phaseStart = millis();
  s.psramStart = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s_taskRuns = s_taskFailures = s_taskUsMax = 0;
  s_taskUsSum = 0;
  s_errLoop[0] = 0;
  s_errTask[0] = 0;
  if (p == 2) s_taskRun = true;
  Serial.printf("[bench] phase %s start\n", kPhaseName[p]);
}

static void report() {
  if (s_phase == 2) {
    stopTask();
    s.runs = s_taskRuns; s.failures = s_taskFailures;
    s.runUsSum = s_taskUsSum; s.runUsMax = s_taskUsMax;
  }
  const float secs = PHASE_MS / 1000.0f;
  Serial.printf("[bench] %s frames %u (%.1f/s) render avg %.2f max %.2f ms | "
                "loop gap max %.2f ms | lua runs %u fail %u avg %.2f max %.2f ms | "
                "psram free start %u min %u end %u | internal min %u\n",
                kPhaseName[s_phase], s.frames, s.frames / secs,
                s.frames ? (s.renderUsSum / (double)s.frames) / 1000.0 : 0.0,
                s.renderUsMax / 1000.0f, s.gapUsMax / 1000.0f,
                s.runs, s.failures,
                s.runs ? (s.runUsSum / (double)s.runs) / 1000.0 : 0.0,
                s.runUsMax / 1000.0f,
                (unsigned)s.psramStart, (unsigned)s.psramMin,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)s.internalMin);
  const char *err = s_phase == 2 ? s_errTask : s_errLoop;
  if (s.failures && err[0]) Serial.printf("[bench] %s first error: %s\n", kPhaseName[s_phase], err);
}

// Once at boot, before the phases: what a run costs when nothing else is going
// on. An empty script is the price of a fresh sandboxed lua_State; the compute
// script is VM speed; the allocation script is the phases' load.
static void once(const char *name, const char *src, int n) {
  uint64_t sum = 0;
  uint32_t mx = 0;
  int fails = 0;
  char err[96] = {0};
  for (int i = 0; i < n; i++) {
    const uint32_t t0 = micros();
    if (!nslua_run(src, err, sizeof(err))) fails++;
    const uint32_t us = micros() - t0;
    sum += us;
    if (us > mx) mx = us;
  }
  Serial.printf("[bench] once %-8s x%-2d avg %.2f max %.2f ms fail %d %s\n", name, n,
                (sum / (double)n) / 1000.0, mx / 1000.0f, fails, fails ? err : "");
}

void nsluaBenchBegin() {
  settings.clockStyle = 7;      // Snake, in RAM only: never saved
  resetClockAnimationState();
  Serial.printf("[bench] refresh %d Hz (driver), framebuffer in %s, psram free %u, internal free %u\n",
                display.refreshRateHz(),
#if defined(SPIRAM_DMA_BUFFER)
                "PSRAM",
#else
                "internal SRAM",
#endif
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  once("empty", "return 1", 20);
  once("compute", kCompute, 5);
  once("allocate", kScript, 5);
  xTaskCreatePinnedToCore(benchTask, "luabench", 32768, nullptr, 1, nullptr, 0);
}

void nsluaBenchLoop() {
  const uint32_t now = micros();
  if (s_lastLoopUs) {
    const uint32_t gap = now - s_lastLoopUs;
    if (gap > s.gapUsMax) s.gapUsMax = gap;
  }
  const size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (ps < s.psramMin) s.psramMin = ps;
  if (in < s.internalMin) s.internalMin = in;

  if (!s_phaseStart) {
    beginPhase(0);
  } else if (millis() - s_phaseStart >= PHASE_MS) {
    report();
    beginPhase((uint8_t)((s_phase + 1) % 3));
  }
  s_lastLoopUs = micros();   // a phase change's own wait is not a render stall
}

void nsluaBenchFrameBegin() {
  s_frameUs = micros();
  if (s_phase == 1) {
    uint32_t us;
    const bool ok = runOnce(us, s_errLoop, sizeof(s_errLoop));
    s.runs++; s.runUsSum += us;
    if (us > s.runUsMax) s.runUsMax = us;
    if (!ok) s.failures++;
  }
}

void nsluaBenchFrameEnd() {
  const uint32_t us = micros() - s_frameUs;
  s.frames++;
  s.renderUsSum += us;
  if (us > s.renderUsMax) s.renderUsMax = us;
}

#endif  // NSLUA_BENCH

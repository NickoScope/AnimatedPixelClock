// ============================================================
// lua_effects.cpp - the Lua effects on the panel: a task, a canvas, a blit
// ============================================================
// Threading (the reasoning is in src/lua/README.md):
//
//   core 0, task "luafx"  owns the LuaFx and the canvas. Opens the selected
//                         effect, calls draw() at up to the effect's FPS,
//                         copies each finished canvas into a spare buffer and
//                         swaps it into "ready" under a spinlock.
//   core 1, loop()        luaEffectsSelect() says which effect is wanted;
//                         luaEffectsRender() swaps "ready" into "front" under
//                         the same spinlock and blits front to the panel.
//
// Four PSRAM buffers of 128x64x3: work (px draws here, and it persists between
// frames as luasim's does), spare and ready (task side), front (render side).
// The spinlock only ever guards pointer swaps and a short error string.
//
// Every selection gets a new number, and frames and errors carry the number
// they were made under. Leaving a page and coming straight back is therefore
// a fresh start - a failed effect is retried - and a frame or an error from an
// earlier visit can never be shown for the current one.
//
// The task's stack is internal RAM, not PSRAM: arduino-esp32 2.0.17 builds
// FreeRTOS without CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY, and its
// xTaskCreateStaticPinnedToCore asserts that a stack passes the same
// internal-memory check as a TCB (checked in the shipped libfreertos.a).
// ============================================================

// The script table is compiled only where this is defined, and the generated
// header is #pragma once, so it has to come before the first include.
#define LUA_EFFECTS_TABLE

#include "lua_effects_page.h"

#if defined(LUA_EFFECTS_ENABLED)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../control/clock_style.h"   // ctrlToast
#include "../display/display.h"
#include "../fonts/picopixel_fb.h"
#include "lua_fx.h"
#include "lua_px.h"
#include "../debug/dbg_log.h"
#if defined(LUA_STORE_ENABLED)
#include "lua_store.h"
#endif

namespace {

// 12 KB: the effects are compiled in, not uploaded, so the parser never sees a
// hostile nesting depth. On the panel (2026-09-14) the six effects left at
// least 11 612 B of the previous 16 KB free across every 30 s report - at most
// 4.8 KB used, football and snooker included - so 12 KB keeps over 7 KB spare
// and gives 4 KB back to the internal heap, which is this board's scarce one.
// The task logs its high-water mark; a new effect that eats into the margin
// shows there. The bench ran nslua on 32 KB; scripts arriving at run time would
// need that back, and the internal heap would have to be measured for it.
const uint32_t    kStackBytes = 12 * 1024;
const BaseType_t  kCore       = 0;      // Wi-Fi's core; loop() and the DMA refresh stay on 1
const UBaseType_t kPriority   = 1;      // as the bench's task and loopTask
const uint32_t    kIdleMs     = 1500;   // no render for this long: nobody is looking, stop drawing
const uint32_t    kReportMs   = 30000;  // serial report period while an effect runs
const uint8_t     kOverrunsAllowed = 3; // draws past the time budget in a row before the effect stops

// A selection as one word, so the task can never read a new number with an
// old index: (sequence << 8) | (index + 1). Index 0 in the low byte is "none".
inline uint32_t selWord(uint32_t seq, int16_t index) { return (seq << 8) | (uint32_t)(index + 1); }
inline int16_t  selIndex(uint32_t word)              { return (int16_t)(word & 0xFF) - 1; }

TaskHandle_t s_task = nullptr;
uint8_t     *s_work = nullptr, *s_spare = nullptr, *s_ready = nullptr, *s_front = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Guarded by s_mux.
bool     s_fresh     = false;  // ready holds a frame render has not taken yet
uint32_t s_readyWord = 0;      // the selection that drew ready
uint32_t s_errorWord = 0;      // the selection that failed; 0 = none
char     s_error[96] = {0};

// Written by one side, read by the other; single words, so no lock.
volatile uint32_t s_wantWord   = 0;     // loop -> task
volatile uint32_t s_demandMs   = 0;     // render -> task
volatile uint8_t  s_fps        = LUA_FX_DEFAULT_FPS;   // task -> render
volatile uint16_t s_frameGapMs = 0;     // task -> render: last frame-to-frame time

// loop() only.
int16_t  s_selected    = -1;
uint32_t s_seq         = 0;
int16_t  s_showRequest = -1;
uint32_t s_frontWord   = 0;

// One reading of the clock for px.t() and px.now() together, so a clock script
// never sees the minute turn in one a frame before the phase wraps in the other.
void fillClock(LuaPxClock &c, double period) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const time_t t = tv.tv_sec;
  struct tm lt, ut;
  localtime_r(&t, &lt);
  gmtime_r(&t, &ut);
  // Aligned to the epoch, so with the default 60 s the phase is the second hand:
  // tetris_clock and snake_clock finish assembling the next time as it arrives.
  double phase = (fmod((double)tv.tv_sec, period) + tv.tv_usec / 1e6) / period;
  if (phase >= 1.0) phase -= 1.0;
  // lua_Number is a float here: anything closer to 1 than this rounds up to 1.0.
  if (phase > 0.99999994) phase = 0.99999994;
  c.phase = phase;
  c.hour = lt.tm_hour;
  c.min  = lt.tm_min;
  c.sec  = lt.tm_sec;
  c.yday = lt.tm_yday;
  c.year = lt.tm_year + 1900;
  int days = lt.tm_yday - ut.tm_yday;
  if (lt.tm_year != ut.tm_year) days = lt.tm_year > ut.tm_year ? 1 : -1;
  c.utcMinutes = days * 1440 + (lt.tm_hour - ut.tm_hour) * 60 + (lt.tm_min - ut.tm_min);
}

bool luaEffectsHasError(uint32_t word) {
  portENTER_CRITICAL(&s_mux);
  const bool has = (s_errorWord == word) && s_error[0];
  portEXIT_CRITICAL(&s_mux);
  return has;
}

void publishError(uint32_t word, const char *msg) {
  // The first line only: the traceback goes to serial, not onto 128 pixels.
  char line[sizeof(s_error)];
  size_t n = 0;
  while (msg[n] && msg[n] != '\n' && n < sizeof(line) - 1) { line[n] = msg[n]; n++; }
  line[n] = 0;
  portENTER_CRITICAL(&s_mux);
  memcpy(s_error, line, n + 1);
  s_errorWord = word;
  portEXIT_CRITICAL(&s_mux);
}

LuaFx       s_fx;       // task only
LuaPxCanvas s_canvas;   // task only

// An effect is either compiled into the image or uploaded to LittleFS, and
// everything here addresses the two as one list: the built-in ones first, in
// the order the generator put them, then whatever was uploaded.
void effectId(int16_t i, char *out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  if (i < 0) { strncpy(out, "?", cap - 1); out[cap - 1] = '\0'; return; }
  if (i < (int16_t)LUA_EFFECT_COUNT) {
    strncpy(out, kLuaEffectScripts[i].id, cap - 1);
    out[cap - 1] = '\0';
    return;
  }
#if defined(LUA_STORE_ENABLED)
  if (!luaStoreStem((uint8_t)(i - LUA_EFFECT_COUNT), out, cap)) {
    strncpy(out, "?", cap - 1);
    out[cap - 1] = '\0';
  }
#else
  strncpy(out, "?", cap - 1);
  out[cap - 1] = '\0';
#endif
}

#if defined(LUA_STORE_ENABLED)
// The source of an uploaded script, held in PSRAM while its effect is open and
// released when it closes. Only the task touches it.
char *s_userSrc = nullptr;

void releaseUser() {
  if (s_userSrc) { luaStoreRelease(s_userSrc); s_userSrc = nullptr; }
}
#endif

// ── the trial run of an upload (luaStoreSetTrial) ─────────────────────────
// loop() asks, the effect task runs it - the only task that may touch s_fx -
// and loop() waits. The task takes its own copy of the text, so a loop() that
// gives up waiting can free its buffer while the task is still compiling.
const uint8_t  kTrialFrames  = 4;
const uint8_t  kTrialOverMax = 1;      // frames over the budget a trial forgives (Wi-Fi shares the core)
const uint32_t kTrialWaitMs  = 15000;  // load deadline 3 s + 4 frames x 0.5 s, with room
struct Trial {
  char    *src = nullptr;
  size_t   len = 0;
  bool     ok = false;
  bool     abandoned = false;
  uint32_t openMs = 0;
  uint16_t ms[kTrialFrames] = {};
  uint8_t  n = 0;
  char     err[160] = "";
};
Trial             s_trial;
volatile uint8_t  s_trialState = 0;   // 0 idle, 1 asked, 2 running, 3 done
char              s_trialReport[96] = "";

void runTrial() {
  s_trialState = 2;
  if (s_fx.isOpen()) s_fx.close();
#if defined(LUA_STORE_ENABLED)
  releaseUser();
#endif
  Trial &t = s_trial;
  memset(s_work, 0, LUA_PX_BYTES);
  fillClock(s_canvas.clock, 60.0);
  int64_t t0 = esp_timer_get_time();
  bool ok = s_fx.open("upload", t.src, t.len, &s_canvas, kLuaFxPanelLimits);
  t.openMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  heap_caps_free(t.src);   // compiled into the state; the text is finished with
  t.src = nullptr;
  if (!ok) {
    snprintf(t.err, sizeof(t.err), "it does not load on the panel: %s", s_fx.error());
  } else {
    uint8_t over = 0;
    for (uint8_t k = 0; k < kTrialFrames && ok; k++) {
      fillClock(s_canvas.clock, s_fx.periodSeconds());
      t0 = esp_timer_get_time();
      const bool drew = s_fx.draw();
      const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
      t.ms[t.n++] = (uint16_t)(ms > 65535 ? 65535 : ms);
      if (!drew) {
        if (strstr(s_fx.error(), "over the time budget")) { over++; }
        else {
          snprintf(t.err, sizeof(t.err), "frame %u fails on the panel: %s", (unsigned)k + 1, s_fx.error());
          ok = false;
        }
      }
      vTaskDelay(1);   // IDLE0 and Wi-Fi get their tick between frames
    }
    if (ok && over > kTrialOverMax) {
      snprintf(t.err, sizeof(t.err),
               "too slow for the panel: %u of %u frames over the %u ms a frame may take "
               "(%u %u %u %u ms). Draw less per frame", (unsigned)over, (unsigned)kTrialFrames,
               (unsigned)kLuaFxPanelLimits.drawMs, t.ms[0], t.ms[1], t.ms[2], t.ms[3]);
      ok = false;
    }
    s_fx.close();
  }
  t.ok = ok;
  dbgLogf("[luafx] trial of an upload: %s, load %u ms, frames %u %u %u %u ms%s%s\n",
          ok ? "passed" : "REFUSED", (unsigned)t.openMs, t.ms[0], t.ms[1], t.ms[2], t.ms[3],
          ok ? "" : ": ", ok ? "" : t.err);
  s_trialState = t.abandoned ? 0 : 3;
}

void effectTask(void *) {
  s_canvas.rgb = s_work;
  char       id[LUA_EFFECT_NAME_CAP] = "?";
  uint32_t   runningWord = 0;
  int16_t    running = -1;
  bool       failed = false;
  TickType_t lastWake = xTaskGetTickCount();
  // Serial report while an effect runs: the numbers to read off the panel.
  uint32_t reportAt = 0, frames = 0, drawMaxUs = 0, instrMax = 0, lastPublishMs = 0;
  uint8_t  overruns = 0;
  uint64_t drawSumUs = 0;

  for (;;) {
    if (s_trialState == 1) {
      runTrial();
      runningWord = 0;          // whatever was on screen opens again, from its start
      lastWake = xTaskGetTickCount();
      continue;
    }
    const uint32_t want = s_wantWord;
    if (want != runningWord) {
      if (s_fx.isOpen()) {
        s_fx.close();
        dbgLogf("[luafx] closed %s, heap back to %u B, psram free %u\n",
                      id, (unsigned)s_fx.heapBytes(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      }
#if defined(LUA_STORE_ENABLED)
      releaseUser();
#endif
      runningWord = want;
      running = selIndex(want);
      failed = false;
      overruns = 0;
      if (running >= 0) {
        effectId(running, id, sizeof(id));
        const char *src = nullptr;
        size_t      srcLen = 0;
        if (running < (int16_t)LUA_EFFECT_COUNT) {
          src = kLuaEffectScripts[running].src;
          srcLen = kLuaEffectScripts[running].len;
        }
#if defined(LUA_STORE_ENABLED)
        else {
          // Read it now rather than at boot: a slot may be empty, the file may
          // have been replaced since, and 24 KB of PSRAM is not worth holding
          // for an effect nobody is looking at.
          s_userSrc = luaStoreRead((uint8_t)(running - LUA_EFFECT_COUNT), &srcLen);
          src = s_userSrc;
          // Checked again on the way out of storage, not only on the way in.
          // luaStoreFinish validates an upload, but a file can reach /lua
          // without passing through it - a filesystem image, a firmware rolled
          // back to a build with a different limit - and the parser must never
          // see one that has not been measured.
          if (src) {
            // static, not a local: this frame is the bottom of the deepest
            // stack in the firmware, and 160 bytes of it is 160 bytes the Lua
            // parser does not get. Only this task runs here.
            static char why[160];
            if (!luaStoreValidate(src, srcLen, why, sizeof(why))) {
              dbgLogf("[luafx] %s refused: %s\n", id, why);
              publishError(runningWord, why);
              releaseUser();
              src = nullptr;
            }
          }
        }
#endif
        memset(s_work, 0, LUA_PX_BYTES);      // luasim starts every run on black
        fillClock(s_canvas.clock, 60.0);      // for a script that reads px.now() at load
        const int64_t t0 = esp_timer_get_time();
        const bool ok = src && s_fx.open(id, src, srcLen, &s_canvas, kLuaFxPanelLimits);
        if (!src) {
          dbgLogf("[luafx] %s: nothing to run in that slot\n", id);
          // Whatever is in s_fx.error() belongs to the previous effect: open()
          // was short-circuited and never cleared it. Say what is actually
          // wrong instead, and leave a refusal message already published alone.
          if (!luaEffectsHasError(runningWord)) publishError(runningWord, "nothing in that slot");
        }
        dbgLogf("[luafx] open %s: %s in %.1f ms, ~%u instr, heap %u B, fps cap %u, period %.0f s, "
                      "stack free %u B\n", id, ok ? "ok" : "FAILED",
                      (esp_timer_get_time() - t0) / 1000.0, (unsigned)s_fx.lastInstructions(),
                      (unsigned)s_fx.heapBytes(), s_fx.fps(), s_fx.periodSeconds(),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        if (!ok) {
          if (src) {                       // open() ran, so the message is its own
            dbgLogf("[luafx] %s\n", s_fx.error());
            publishError(runningWord, s_fx.error());
          }
          failed = true;
#if defined(LUA_STORE_ENABLED)
          releaseUser();   // nothing will run: give the PSRAM back now
#endif
        } else {
          s_fps = s_fx.fps();
#if defined(LUA_STORE_ENABLED)
          // The chunk is compiled and the state owns it; the text is finished
          // with. Up to 24 KB of PSRAM back for as long as the effect runs.
          releaseUser();
#endif
        }
        s_frameGapMs = 0;
        reportAt = millis() + kReportMs;
        frames = drawMaxUs = instrMax = 0;
        drawSumUs = 0;
        lastPublishMs = 0;
      }
      lastWake = xTaskGetTickCount();
    }

    if (running < 0 || failed || (millis() - s_demandMs) > kIdleMs) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));   // luaEffectsSelect() wakes us early
      lastWake = xTaskGetTickCount();
      continue;
    }

    fillClock(s_canvas.clock, s_fx.periodSeconds());
    const int64_t t0 = esp_timer_get_time();
    if (!s_fx.draw()) {
      // One frame past its time budget is dropped, not fatal: WiFi shares this
      // core and can stretch a frame that normally fits (room_radar draws in
      // ~380 ms of its 500). Only kOverrunsAllowed in a row stop the effect.
      // The state survives a failed draw: it runs under lua_pcall.
      if (strstr(s_fx.error(), "over the time budget") && ++overruns < kOverrunsAllowed) {
        dbgLogf("[luafx] %s: frame dropped (%u in a row), %s\n",
                      id, (unsigned)overruns, s_fx.error());
        vTaskDelay(1);
        lastWake = xTaskGetTickCount();
        continue;
      }
      dbgLogf("[luafx] %s stopped: %s\n", id, s_fx.error());
      publishError(runningWord, s_fx.error());
      s_fx.close();
      failed = true;
      continue;
    }
    overruns = 0;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    memcpy(s_spare, s_work, LUA_PX_BYTES);
    portENTER_CRITICAL(&s_mux);
    uint8_t *swap = s_ready;
    s_ready = s_spare;
    s_spare = swap;
    s_readyWord = runningWord;
    s_fresh = true;
    portEXIT_CRITICAL(&s_mux);

    const uint32_t nowMs = millis();
    if (lastPublishMs) {
      const uint32_t gap = nowMs - lastPublishMs;
      s_frameGapMs = (uint16_t)(gap > 65535 ? 65535 : gap);
    }
    lastPublishMs = nowMs;
    frames++;
    drawSumUs += us;
    if (us > drawMaxUs) drawMaxUs = us;
    if (s_fx.lastInstructions() > instrMax) instrMax = s_fx.lastInstructions();
    if (nowMs >= reportAt) {
      dbgLogf("[luafx] %s: %u frames in %u s (%.1f fps), draw avg %.1f max %.1f ms, "
                    "~%u instr max, heap %u B peak %u B, stack free %u B, psram free %u, "
                    "internal free %u min %u\n",
                    id, (unsigned)frames, (unsigned)(kReportMs / 1000),
                    frames * 1000.0 / kReportMs, frames ? drawSumUs / 1000.0 / frames : 0.0,
                    drawMaxUs / 1000.0, (unsigned)instrMax, (unsigned)s_fx.heapBytes(),
                    (unsigned)s_fx.heapPeak(), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
      reportAt = nowMs + kReportMs;
      frames = drawMaxUs = instrMax = 0;
      drawSumUs = 0;
    }

    // One frame per 1/FPS, and never without a tick of sleep: IDLE0 is watched
    // by the task watchdog, and Wi-Fi shares this core.
    const TickType_t period = pdMS_TO_TICKS(1000 / s_fx.fps());
    if (xTaskGetTickCount() - lastWake >= period) {
      vTaskDelay(1);
      lastWake = xTaskGetTickCount();
    } else {
      vTaskDelayUntil(&lastWake, period);
    }
  }
}

void messageLine(int16_t y, const char *text, uint16_t colour) {
  display.setCursor(2, y);
  display.setTextColor(colour);
  display.print(text);
}

// Title, effect name, and the error wrapped over up to four lines, in Picopixel.
void drawMessage(const char *title, const char *name, const char *detail) {
  display.fillScreen(0);
  display.setFont(&PicopixelFB);
  display.setTextWrap(false);
  display.setTextSize(1);   // sticky global state; the clocks leave it at 3 or 4
  messageLine(8, title, display.color565(255, 160, 0));
  messageLine(17, name, display.color565(255, 255, 255));
  char line[31];            // Picopixel averages about 4 px: 30 characters to a line
  int16_t y = 28;
  for (const char *p = detail; *p && y <= 52; y += 8) {
    size_t n = strnlen(p, 30);
    if (n == 30 && p[n]) {  // break at the last space, if there is one
      size_t sp = n;
      while (sp > 0 && p[sp] != ' ') sp--;
      if (sp > 0) n = sp;
    }
    while (n && p[n] && ((unsigned char)p[n] & 0xC0) == 0x80) n--;   // never half a letter
    memcpy(line, p, n);
    line[n] = 0;
    messageLine(y, line, display.color565(160, 170, 180));
    p += n;
    while (*p == ' ') p++;
  }
  display.setFont(NULL);
}

}  // namespace

// ---------------------------------------------------------------- public four
// The count is the compiled-in scripts plus whatever has been uploaded. It
// changes while the panel is running - an upload or a delete moves it - so
// nothing may cache it. The PAGE enum reserves LUA_USER_MAX slots whether or
// not they hold anything; ctrlPageVisitable() hides the empty ones.
uint8_t luaEffectCount() {
#if defined(LUA_STORE_ENABLED)
  return (uint8_t)(LUA_EFFECT_COUNT + luaStoreCount());
#else
  return LUA_EFFECT_COUNT;
#endif
}

uint8_t luaEffectSlots() {
#if defined(LUA_STORE_ENABLED)
  return (uint8_t)(LUA_EFFECT_COUNT + LUA_USER_MAX);
#else
  return LUA_EFFECT_COUNT;
#endif
}

void luaEffectName(uint8_t i, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (i < LUA_EFFECT_COUNT) {
    strncpy(out, kLuaEffectScripts[i].name, cap - 1);
    out[cap - 1] = '\0';
    return;
  }
#if defined(LUA_STORE_ENABLED)
  luaStoreName((uint8_t)(i - LUA_EFFECT_COUNT), out, cap);
#endif
}

bool luaEffectUploaded(uint8_t i) {
#if defined(LUA_STORE_ENABLED)
  return i >= LUA_EFFECT_COUNT && i < luaEffectCount();
#else
  (void)i;
  return false;
#endif
}

void luaEffectShow(uint8_t i) {
  if (i < luaEffectCount()) s_showRequest = i;
}

int16_t luaEffectCurrent() { return s_selected; }

void luaEffectStop() { luaEffectsSelect(-1); }

// Reopen whatever is showing, even though the index has not changed.
//
// luaEffectsSelect returns early when the index is the same, which is right for
// a knob and wrong for an upload: replacing the file behind the effect that is
// on screen left the previously compiled chunk running, so a new script looked
// like the old one until you left the page and came back. The effect task keys
// its reload off the whole word, sequence AND index, so bumping the sequence is
// all it takes - the task sees `want != runningWord`, closes, and reads the
// file again.
//
// This is also the delete case. Removing a script renumbers the slots after it,
// so the index on screen can silently come to mean a different file.
void luaEffectsReload() {
  if (s_selected < 0) return;
  s_seq = (s_seq + 1) & 0x00FFFFFF;
  if (s_seq == 0) s_seq = 1;
  s_wantWord = selWord(s_seq, s_selected);
  if (s_task) xTaskNotifyGive(s_task);
}

uint32_t luaEffectsStackFreeMin() {
  // uxTaskGetStackHighWaterMark is in words on some ports and bytes on Xtensa;
  // ESP-IDF's FreeRTOS returns bytes here, which is what the boot line already
  // prints and what kStackBytes is measured in.
  return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

// ---------------------------------------------------------------- the trial
static bool luaEffectsTrial(const char *src, size_t len, char *err, size_t errlen) {
  if (!s_task) return true;                 // no effect task: nothing to try it on
  if (s_trialState != 0) { snprintf(err, errlen, "another upload is being tried: send it again"); return false; }
  char *copy = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) { snprintf(err, errlen, "no PSRAM to try it"); return false; }
  memcpy(copy, src, len);
  copy[len] = '\0';
  s_trial = Trial();
  s_trial.src = copy;
  s_trial.len = len;
  s_trialState = 1;
  xTaskNotifyGive(s_task);
  const uint32_t t0 = millis();
  while (s_trialState != 3) {
    if (millis() - t0 > kTrialWaitMs) {
      s_trial.abandoned = true;             // the task frees its copy and goes idle
      snprintf(err, errlen, "the trial run did not finish in %u s", (unsigned)(kTrialWaitMs / 1000));
      return false;
    }
    esp_task_wdt_reset();                   // loop() waits here: a few seconds at most
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  s_trialState = 0;
  snprintf(s_trialReport, sizeof(s_trialReport), "loaded in %u ms, 4 frames %u %u %u %u ms (the budget is %u)",
           (unsigned)s_trial.openMs, s_trial.ms[0], s_trial.ms[1], s_trial.ms[2], s_trial.ms[3],
           (unsigned)kLuaFxPanelLimits.drawMs);
  if (s_trial.ok) return true;
  snprintf(err, errlen, "%s", s_trial.err);
  return false;
}

const char *luaEffectsTrialReport() { return s_trialReport; }

// ---------------------------------------------------------------- page hooks
void luaEffectsBegin() {
#if defined(LUA_STORE_ENABLED)
  luaStoreInit();     // before the task, so the first list is already right
  luaStoreSetTrial(luaEffectsTrial);
#endif
  if (s_task) return;
  const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint8_t **bufs[] = {&s_work, &s_spare, &s_ready, &s_front};
  for (uint8_t **b : bufs) {
    *b = static_cast<uint8_t *>(heap_caps_calloc(1, LUA_PX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!*b) {
      dbgLogf("[luafx] no PSRAM for the frame buffers - effects unavailable");
      for (uint8_t **f : bufs) { heap_caps_free(*f); *f = nullptr; }
      return;
    }
  }
  if (xTaskCreatePinnedToCore(effectTask, "luafx", kStackBytes, nullptr, kPriority, &s_task, kCore) != pdPASS) {
    s_task = nullptr;
    dbgLogf("[luafx] task create failed (%u B stack, internal largest block %u) - effects unavailable\n",
                  (unsigned)kStackBytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    for (uint8_t **f : bufs) { heap_caps_free(*f); *f = nullptr; }
    return;
  }
  dbgLogf("[luafx] %u effects, task on core %d with %u B stack (internal free %u -> %u), "
                "4 x %u B frame buffers in PSRAM\n", (unsigned)LUA_EFFECT_COUNT, (int)kCore,
                (unsigned)kStackBytes, (unsigned)internalBefore,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)LUA_PX_BYTES);
}

int16_t luaEffectsTakeShowRequest() {
  const int16_t r = s_showRequest;
  s_showRequest = -1;
  return r;
}

void luaEffectsSelect(int16_t index) {
  if (index >= (int16_t)luaEffectCount()) index = -1;
  if (index == s_selected) return;
  s_selected = index;
  s_seq = (s_seq + 1) & 0x00FFFFFF;
  if (s_seq == 0) s_seq = 1;               // word 0 means "never selected"
  s_wantWord = selWord(s_seq, index);
  if (s_task) xTaskNotifyGive(s_task);
  if (index >= 0) {
    // ctrlToast keeps the pointer until the banner goes (clock_style.h), so the
    // text has to outlive this call. This buffer is written only here, and
    // luaEffectsSelect runs on the loop task alone.
    static char toast[LUA_EFFECT_NAME_CAP];
    luaEffectName((uint8_t)index, toast, sizeof(toast));
    ctrlToast(toast);           // every way in: knob, carousel, web
  }
}

void luaEffectsRender() {
  s_demandMs = millis();
  const int16_t sel = s_selected;
  if (sel < 0) return;
  if (!s_task) {
    // sel reaches LUA_EFFECT_COUNT + luaStoreCount() - 1 once scripts can be
    // uploaded, and kLuaEffectScripts holds only the compiled-in ones. Indexing
    // it here read past the array and printed whatever followed.
    char nm[LUA_EFFECT_NAME_CAP];
    luaEffectName((uint8_t)sel, nm, sizeof(nm));
    drawMessage("LUA EFFECTS UNAVAILABLE", nm, "NO PSRAM OR NO TASK - SEE SERIAL");
    return;
  }
  const uint32_t want = selWord(s_seq, sel);
  char err[sizeof(s_error)];
  bool failed = false;
  portENTER_CRITICAL(&s_mux);
  if (s_fresh) {
    uint8_t *swap = s_front;
    s_front = s_ready;
    s_ready = swap;
    s_frontWord = s_readyWord;
    s_fresh = false;
  }
  if (s_errorWord == want) {
    memcpy(err, s_error, sizeof(err));
    failed = true;
  }
  portEXIT_CRITICAL(&s_mux);

  if (failed) {
    char nm[LUA_EFFECT_NAME_CAP];
    luaEffectName((uint8_t)sel, nm, sizeof(nm));   // same out-of-bounds read as above
    drawMessage("LUA ERROR", nm, err);
  } else if (s_frontWord == want) {
    luaPxBlit(s_front, display);
  } else {
    display.fillScreen(0);                  // still loading: the banner names it meanwhile
  }
}

int luaEffectsRefreshHz() {
  int hz = s_fps ? s_fps : LUA_FX_DEFAULT_FPS;
  const uint16_t gap = s_frameGapMs;
  if (gap) {
    const int achieved = 1000 / gap + 1;
    if (achieved < hz) hz = achieved;
  }
  return hz < 2 ? 2 : hz;
}

#endif  // LUA_EFFECTS_ENABLED

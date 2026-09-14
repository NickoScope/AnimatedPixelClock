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

namespace {

// 16 KB: the effects are compiled in, not uploaded, so the parser never sees a
// hostile nesting depth; on the host the scripts took under 3 KB of stack
// above the harness (fx_parity.py), and the task logs its high-water mark.
// The bench ran nslua on 32 KB; scripts arriving at run time would need that
// back, and the internal heap would have to be measured for it.
const uint32_t    kStackBytes = 16 * 1024;
const BaseType_t  kCore       = 0;      // Wi-Fi's core; loop() and the DMA refresh stay on 1
const UBaseType_t kPriority   = 1;      // as the bench's task and loopTask
const uint32_t    kIdleMs     = 1500;   // no render for this long: nobody is looking, stop drawing
const uint32_t    kReportMs   = 30000;  // serial report period while an effect runs

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
  int days = lt.tm_yday - ut.tm_yday;
  if (lt.tm_year != ut.tm_year) days = lt.tm_year > ut.tm_year ? 1 : -1;
  c.utcMinutes = days * 1440 + (lt.tm_hour - ut.tm_hour) * 60 + (lt.tm_min - ut.tm_min);
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

void effectTask(void *) {
  s_canvas.rgb = s_work;
  uint32_t   runningWord = 0;
  int16_t    running = -1;
  bool       failed = false;
  TickType_t lastWake = xTaskGetTickCount();
  // Serial report while an effect runs: the numbers to read off the panel.
  uint32_t reportAt = 0, frames = 0, drawMaxUs = 0, instrMax = 0, lastPublishMs = 0;
  uint64_t drawSumUs = 0;

  for (;;) {
    const uint32_t want = s_wantWord;
    if (want != runningWord) {
      if (s_fx.isOpen()) {
        s_fx.close();
        Serial.printf("[luafx] closed %s, heap back to %u B, psram free %u\n",
                      kLuaEffectScripts[running].id, (unsigned)s_fx.heapBytes(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      }
      runningWord = want;
      running = selIndex(want);
      failed = false;
      if (running >= 0) {
        const LuaEffectScript &sc = kLuaEffectScripts[running];
        memset(s_work, 0, LUA_PX_BYTES);      // luasim starts every run on black
        fillClock(s_canvas.clock, 60.0);      // for a script that reads px.now() at load
        const int64_t t0 = esp_timer_get_time();
        const bool ok = s_fx.open(sc.id, sc.src, sc.len, &s_canvas, kLuaFxPanelLimits);
        Serial.printf("[luafx] open %s: %s in %.1f ms, ~%u instr, heap %u B, fps cap %u, period %.0f s, "
                      "stack free %u B\n", sc.id, ok ? "ok" : "FAILED",
                      (esp_timer_get_time() - t0) / 1000.0, (unsigned)s_fx.lastInstructions(),
                      (unsigned)s_fx.heapBytes(), s_fx.fps(), s_fx.periodSeconds(),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        if (!ok) {
          Serial.printf("[luafx] %s\n", s_fx.error());
          publishError(runningWord, s_fx.error());
          failed = true;
        } else {
          s_fps = s_fx.fps();
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
      Serial.printf("[luafx] %s stopped: %s\n", kLuaEffectScripts[running].id, s_fx.error());
      publishError(runningWord, s_fx.error());
      s_fx.close();
      failed = true;
      continue;
    }
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
      Serial.printf("[luafx] %s: %u frames in %u s (%.1f fps), draw avg %.1f max %.1f ms, "
                    "~%u instr max, heap %u B peak %u B, stack free %u B, psram free %u, "
                    "internal free %u min %u\n",
                    kLuaEffectScripts[running].id, (unsigned)frames, (unsigned)(kReportMs / 1000),
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
uint8_t luaEffectCount() { return LUA_EFFECT_COUNT; }

const char *luaEffectName(uint8_t i) {
  return i < LUA_EFFECT_COUNT ? kLuaEffectScripts[i].name : "";
}

void luaEffectShow(uint8_t i) {
  if (i < LUA_EFFECT_COUNT) s_showRequest = i;
}

int16_t luaEffectCurrent() { return s_selected; }

// ---------------------------------------------------------------- page hooks
void luaEffectsBegin() {
  if (s_task) return;
  const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint8_t **bufs[] = {&s_work, &s_spare, &s_ready, &s_front};
  for (uint8_t **b : bufs) {
    *b = static_cast<uint8_t *>(heap_caps_calloc(1, LUA_PX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!*b) {
      Serial.println("[luafx] no PSRAM for the frame buffers - effects unavailable");
      for (uint8_t **f : bufs) { heap_caps_free(*f); *f = nullptr; }
      return;
    }
  }
  if (xTaskCreatePinnedToCore(effectTask, "luafx", kStackBytes, nullptr, kPriority, &s_task, kCore) != pdPASS) {
    s_task = nullptr;
    Serial.printf("[luafx] task create failed (%u B stack, internal largest block %u) - effects unavailable\n",
                  (unsigned)kStackBytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    for (uint8_t **f : bufs) { heap_caps_free(*f); *f = nullptr; }
    return;
  }
  Serial.printf("[luafx] %u effects, task on core %d with %u B stack (internal free %u -> %u), "
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
  if (index >= LUA_EFFECT_COUNT) index = -1;
  if (index == s_selected) return;
  s_selected = index;
  s_seq = (s_seq + 1) & 0x00FFFFFF;
  if (s_seq == 0) s_seq = 1;               // word 0 means "never selected"
  s_wantWord = selWord(s_seq, index);
  if (s_task) xTaskNotifyGive(s_task);
  if (index >= 0) ctrlToast(kLuaEffectScripts[index].name);   // every way in: knob, carousel, web
}

void luaEffectsRender() {
  s_demandMs = millis();
  const int16_t sel = s_selected;
  if (sel < 0) return;
  if (!s_task) {
    drawMessage("LUA EFFECTS UNAVAILABLE", kLuaEffectScripts[sel].name, "NO PSRAM OR NO TASK - SEE SERIAL");
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
    drawMessage("LUA ERROR", kLuaEffectScripts[sel].name, err);
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

/*
 * AnimatedPixelClock - Audio Spectrum Visualizer
 *
 * Packets arrive at ~25 Hz; rendering runs at 60 Hz with exponential
 * smoothing toward the latest packet so the bars move fluidly instead of
 * stepping. Peak-hold dots fall with gravity. The bar color is a fixed
 * three-zone vertical gradient (classic EQ look) from three user-editable
 * color slots.
 */

#include "visualizer.h"
#include "../util/psram_state.h"
#include "starfield.h"
#include "oscilloscope.h"

#include "../clocks/clocks.h"
#include "../config/config.h"
#include "../display/display.h"
#include <math.h>

#if defined(VIZ_WOW_ENABLED)
#if !defined(BOARD_HAS_PSRAM)
#error "VIZ_WOW_ENABLED keeps the effects' buffers in PSRAM"
#endif
#include <esp_heap_caps.h>
#include <new>
#include "wow/wow.h"
#endif

#define VIZ_BAR_W 3          // lit pixels per bar (1px gap -> 32 * 4 = 128)
#define VIZ_MAX_H 56.0f      // px, leaves headroom for the corner clock
#define VIZ_SMOOTH 0.35f     // per-frame pull toward the packet value
#define VIZ_PEAK_GRAVITY 60.0f  // px/s^2

static uint8_t vizBands[VIZ_BANDS];
static uint8_t vizWave[VIZ_WAVE_POINTS];
static bool vizWaveEver = false;
static uint32_t vizWaveserial = 0;
static unsigned long vizLastReceived = 0;
static bool vizEverReceived = false;
static uint32_t vizPacketSerial = 0;

static float barH[VIZ_BANDS];
static float peakY[VIZ_BANDS];
static float peakVel[VIZ_BANDS];
static unsigned long lastVizFrame = 0;

// Fixed-size history: no allocations or filesystem work in the render loop.
static const int WATERFALL_ROWS = 26;
static PSRAM_ARRAY(uint8_t, waterfall, [WATERFALL_ROWS][VIZ_BANDS]);
static int waterfallHead = 0;
static unsigned long lastWaterfallRow = 0;
static uint8_t lastStyle = 255;

static uint16_t vizRgb(int r, int g, int b) {
  return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

static void drawPurpleStage(unsigned long now) {
  // A curved concert LED wall: each column follows its own FFT band,
  // while bass opens the light waves and treble adds pale pink highlights.
  // Only 32 x 16 lamps; no framebuffer, allocations or per-pixel trig.
  float bass = 0.0f;
  float treble = 0.0f;
  for (int i = 0; i < 6; i++) bass += barH[i] / (6.0f * VIZ_MAX_H);
  for (int i = 24; i < VIZ_BANDS; i++) treble += barH[i] / (8.0f * VIZ_MAX_H);
  const float phase = (now % 60000UL) * (6.2831853f / 3000.0f);
  for (int i = 0; i < VIZ_BANDS; i++) {
    float level = barH[i] / VIZ_MAX_H;
    float side = (i - 15.5f) / 15.5f;
    float curve = side * side * 3.5f;
    float wave = sinf(i * 0.28f - phase);
    float center = 7.5f + curve + wave * (1.0f + bass * 2.5f);
    float reach = 1.0f + level * 6.0f + bass * 2.0f;
    for (int row = 0; row < SCREEN_HEIGHT / 4; row++) {
      float distance = fabsf(row - center);
      float glow = 1.0f - distance / reach;
      if (glow < 0.0f) glow = 0.0f;
      float rim = 1.0f - fabsf(distance - reach) * 1.4f;
      if (rim < 0.0f) rim = 0.0f;
      float light = glow * (0.2f + level * 0.65f) + rim * bass * 0.4f;
      if (light > 1.0f) light = 1.0f;
      int hot = (int)(glow * glow * treble * 160.0f);
      int r = 12 + (int)(light * 210.0f);
      int g = 2 + (int)(light * 24.0f) + hot;
      int b = 24 + (int)(light * 200.0f);
      int x = i * 4 + 1;
      int y = row * 4 + 1;
      // Dim cross-shaped halo, bright 2x2 core, black separation.
      uint16_t halo = vizRgb(r / 4, g / 4, b / 4);
      display.drawFastHLine(x - 1, y, 4, halo);
      display.drawFastVLine(x, y - 1, 4, halo);
      display.fillRect(x, y, 2, 2, vizRgb(r, g, b));
    }
  }
}

static void drawNeonMirror() {
  const int horizon = settings.vizShowClock ? 36 : 32;
  if (settings.vizShowClock)
    display.drawFastHLine(0, horizon, SCREEN_WIDTH, vizRgb(45, 12, 70));
  for (int i = 0; i < VIZ_BANDS; i++) {
    int h = (int)(barH[i] * (24.0f / VIZ_MAX_H));
    int peak = (int)(peakY[i] * (24.0f / VIZ_MAX_H));
    int x = i * 4;
    for (int y = 1; y <= h; y++) {
      if (y % 3 == 0) continue; // Two lit rows, one dark: discrete LED segments.
      int mix = y * 255 / 24;
      uint16_t upper = vizRgb(40 + mix * 215 / 255, 235 - mix * 185 / 255, 255);
      uint16_t lower = vizRgb(100 + mix * 100 / 255, 20 + mix * 30 / 255, 160);
      display.drawFastHLine(x, horizon - y, VIZ_BAR_W, upper);
      display.drawFastHLine(x, horizon + y, VIZ_BAR_W, lower);
    }
    if (peak > 1) {
      display.drawFastHLine(x, horizon - peak, VIZ_BAR_W, vizRgb(210, 255, 255));
      display.drawFastHLine(x, horizon + peak, VIZ_BAR_W, vizRgb(255, 100, 210));
    }
  }
}

static void drawPhosphorWaterfall(unsigned long now, bool stale) {
  // Wall-clock cadence keeps the trail speed independent of render rate.
  unsigned long steps = (now - lastWaterfallRow) / 40;
  if (steps > 0) {
    lastWaterfallRow = now - (now - lastWaterfallRow) % 40;
    if (steps > WATERFALL_ROWS) steps = WATERFALL_ROWS;
    for (unsigned long s = 0; s < steps; s++) {
      waterfallHead = (waterfallHead + WATERFALL_ROWS - 1) % WATERFALL_ROWS;
      for (int i = 0; i < VIZ_BANDS; i++) {
        waterfall[waterfallHead][i] = stale ? 0 : (uint8_t)(barH[i] * (255.0f / VIZ_MAX_H));
      }
    }
  }
  if (settings.vizShowClock)
    display.drawFastHLine(0, 10, SCREEN_WIDTH, vizRgb(0, 65, 34));
  for (int row = 0; row < WATERFALL_ROWS; row++) {
    int fade = 255 - row * 7;
    for (int i = 0; i < VIZ_BANDS; i++) {
      int level = waterfall[(waterfallHead + row) % WATERFALL_ROWS][i];
      if (level < 5) continue;
      int r, g, b;
      if (level < 128) {
        r = 0; g = level * 2; b = level / 2;
      } else if (level < 208) {
        r = (level - 128) * 2; g = 255; b = 64 + (level - 128);
      } else {
        r = 255; g = 225 - (level - 208); b = 80 - (level - 208);
      }
      uint16_t color = vizRgb(r * fade / 255, g * fade / 255, b * fade / 255);
      int top = settings.vizShowClock ? 12 : 0;
      int y = top + row * (SCREEN_HEIGHT - top) / WATERFALL_ROWS;
      int nextY = top + (row + 1) * (SCREEN_HEIGHT - top) / WATERFALL_ROWS;
      display.fillRect(i * 4, y, VIZ_BAR_W, nextY - y, color);
    }
  }
}

static bool ingestPacket(const uint8_t* buf, int len) {
  if (len < VIZ_PACKET_LEN || memcmp(buf, "FFT1", 4) != 0) return false;
  memcpy(vizBands, buf + 4, VIZ_BANDS);
  if (len >= VIZ_WAVE_PACKET_LEN) {
    memcpy(vizWave, buf + VIZ_PACKET_LEN, VIZ_WAVE_POINTS);
    vizWaveEver = true;
    ++vizWaveserial;
  } else {
    // A legacy companion must not keep a previous sender's waveform alive.
    vizWaveEver = false;
  }
  vizLastReceived = millis();
  ++vizPacketSerial;
  vizEverReceived = true;
  return true;
}

#if defined(VIZ_WOW_ENABLED)
// ---- Styles 7-14: src/viz/wow ----
namespace {

constexpr int kWowQueue = 16;           // 320 ms of DSP frames between two renders
wow::Engine* s_wow = nullptr;           // everything below lives in PSRAM
wow::VizFrame* s_wowQueue = nullptr;
wow::PcFrameDeriver* s_pcDeriver = nullptr;
int s_wowHead = 0, s_wowCount = 0;
unsigned long s_pcWowMs = 0;

void* wowPsram(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

bool wowStyle(uint8_t style) { return wow::effectForStyle(style) >= 0; }   // 2 (Code EQ) and 7-14

// The effects' own primitives, the panel's pixels.
class PanelCanvas : public wow::Canvas {
 public:
  void setPixel(int x, int y, uint16_t c) override { display.drawPixel(x, y, c); }
  void spanH(int x, int y, int w, uint16_t c) override { display.drawFastHLine(x, y, w, c); }
  void spanV(int x, int y, int h, uint16_t c) override { display.drawFastVLine(x, y, h, c); }
  void glyph(int x, int y, unsigned char ch, uint16_t c) override { display.drawChar(x, y, ch, c, c, 1); }
};

// A PC packet carries bands and the waveform only: rebuild the rest (viz_frame.cpp).
void wowFromPc() {
  if (!s_pcDeriver || !wowStyle(settings.vizStyle)) return;
  const unsigned long now = millis();
  if (now - s_pcWowMs > 1000) s_pcDeriver->reset();   // a new stream: no stale beat history
  const float dt = (now - s_pcWowMs) / 1000.0f;       // the deriver clamps it to 10..100 ms
  s_pcWowMs = now;
  uint8_t wave[VIZ_WAVE_POINTS];
  if (vizWaveEver) memcpy(wave, vizWave, sizeof(wave));
  else memset(wave, 128, sizeof(wave));
  wow::VizFrame f;
  s_pcDeriver->feed(vizBands, wave, dt, f);
  vizWowFeed(f);
}

}  // namespace

bool vizWowBegin() {
  if (s_wow) return true;
  void* e = wowPsram(sizeof(wow::Engine));
  void* d = wowPsram(sizeof(wow::PcFrameDeriver));
  s_wowQueue = static_cast<wow::VizFrame*>(wowPsram(kWowQueue * sizeof(wow::VizFrame)));
  wow::Engine* eng = e ? new (e) wow::Engine() : nullptr;
  if (!eng || !d || !s_wowQueue || !eng->begin(wowPsram)) {
    Serial.println("[viz] no PSRAM for styles 7-14");
    return false;
  }
  s_pcDeriver = new (d) wow::PcFrameDeriver();
  s_wow = eng;
  return true;
}

void vizWowFeed(const wow::VizFrame& f) {
  if (!s_wow || !wowStyle(settings.vizStyle)) return;
  if (s_wowCount == kWowQueue) {   // no render for 320 ms: the oldest frame goes
    s_wowHead = (s_wowHead + 1) % kWowQueue;
    s_wowCount--;
  }
  s_wowQueue[(s_wowHead + s_wowCount) % kWowQueue] = f;
  s_wowCount++;
}

bool vizIngestMic(const uint8_t* buf, int len) { return ingestPacket(buf, len); }
#endif

bool vizIngest(const uint8_t* buf, int len) {
  if (!ingestPacket(buf, len)) return false;
#if defined(VIZ_WOW_ENABLED)
  wowFromPc();
#endif
  return true;
}

const uint8_t* vizWaveform() { return vizWaveEver ? vizWave : nullptr; }
uint32_t vizWaveSerial() { return vizWaveserial; }

bool vizRecentEnough(unsigned long maxAgeMs) {
  return vizEverReceived && (millis() - vizLastReceived) <= maxAgeMs;
}

static unsigned long vizForcedAt = 0;

void vizNoteForced() { vizForcedAt = millis(); }

bool vizShouldDisplay() {
  return vizRecentEnough(10000) || (millis() - vizForcedAt) < 10000;
}

// Small HH:MM top-right, same idea as the ambient corner clock.
static void drawVizClock() {
  struct tm timeinfo;
  if (!peekLocalTime(&timeinfo)) return;
  int displayHour, displayMin;
  bool isPM;
  formatTimeForDisplay(timeinfo.tm_hour, timeinfo.tm_min, displayHour,
                       displayMin, isPM);
  char timeStr[6];
  sprintf(timeStr, "%02d%c%02d", displayHour, shouldShowColon() ? ':' : ' ',
          displayMin);
  display.fillRect(SCREEN_WIDTH - 34, 0, 34, 10, DISPLAY_BLACK);
  display.setTextSize(1);
  display.setTextColor(DISPLAY_WHITE);
  display.setCursor(SCREEN_WIDTH - 31, 1);
  display.print(timeStr);
}

void displayVisualizer() {
  unsigned long now = millis();
  bool resetStyle = settings.vizStyle != lastStyle || now - lastVizFrame > 250;
  if (resetStyle) {
    memset(waterfall, 0, sizeof(waterfall));
    waterfallHead = 0;
    lastWaterfallRow = now - 40;
    lastStyle = settings.vizStyle;
  }
  float dt = (now - lastVizFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastVizFrame = now;

  bool stale = !vizRecentEnough(2000);

  uint16_t cLow = SPRITE_COLOR(COL_VIZ_LOW);
  uint16_t cMid = SPRITE_COLOR(COL_VIZ_MID);
  uint16_t cPeak = SPRITE_COLOR(COL_VIZ_PEAK);

  // Fixed zone boundaries (from the bottom), so the gradient stays put and
  // the bars sweep through it - the classic EQ look.
  const int lowZone = 28;
  const int midZone = 45;
  // Preserve the original EQ response; new styles use time-scaled smoothing.
  float smooth = settings.vizStyle == 0 ? VIZ_SMOOTH : 1.0f - expf(-26.0f * dt);

  for (int i = 0; i < VIZ_BANDS; i++) {
    float target = stale ? 0.0f : vizBands[i] * (VIZ_MAX_H / 255.0f);
    barH[i] += (target - barH[i]) * smooth;

    if (barH[i] >= peakY[i]) {
      peakY[i] = barH[i];
      peakVel[i] = 0;
    } else {
      peakVel[i] += VIZ_PEAK_GRAVITY * dt;
      peakY[i] -= peakVel[i] * dt;
      if (peakY[i] < 0) peakY[i] = 0;
    }

    if (settings.vizStyle != 0) continue;

    int h = (int)barH[i];
    int x = i * 4;
    if (h > 0) {
      int hLow = h < lowZone ? h : lowZone;
      display.fillRect(x, SCREEN_HEIGHT - hLow, VIZ_BAR_W, hLow, cLow);
      if (h > lowZone) {
        int hMid = (h < midZone ? h : midZone) - lowZone;
        display.fillRect(x, SCREEN_HEIGHT - lowZone - hMid, VIZ_BAR_W, hMid, cMid);
      }
      if (h > midZone) {
        display.fillRect(x, SCREEN_HEIGHT - h, VIZ_BAR_W, h - midZone, cPeak);
      }
    }
    int py = SCREEN_HEIGHT - 1 - (int)peakY[i];
    if (peakY[i] > 1) {
      display.drawFastHLine(x, py, VIZ_BAR_W, cPeak);
    }
  }

  if (settings.vizStyle == 1) drawNeonMirror();
#if defined(VIZ_WOW_ENABLED)
  constexpr bool waterfallAt2 = false;   // style 2 is Code EQ here (src/viz/wow/wow_matrix.cpp)
#else
  constexpr bool waterfallAt2 = true;
#endif
  if (waterfallAt2 && settings.vizStyle == 2) drawPhosphorWaterfall(now, stale);
  if (settings.vizStyle == 3) drawPurpleStage(now);
  if (settings.vizStyle == 5) {
    float levels[VIZ_BANDS];
    for (int i = 0; i < VIZ_BANDS; i++) levels[i] = barH[i] / VIZ_MAX_H;
    drawStarfieldOverdrive(levels, vizBands, vizPacketSerial, dt, resetStyle);
  }
  if (settings.vizStyle == 6)
    drawOscilloscope(vizWaveform(), vizWaveserial, stale, dt, resetStyle);
#if defined(VIZ_WOW_ENABLED)
  if (wowStyle(settings.vizStyle)) {
    if (s_wow) {
      const int effect = wow::effectForStyle(settings.vizStyle);
      const wow::real react = settings.vizBeatFx / 100.0f;
      if (resetStyle || s_wow->effect() != effect) s_wow->reset(effect, react);
      else s_wow->setReact(react);
      for (; s_wowCount > 0; s_wowCount--) {   // every frame since the last render, in order
        s_wow->update(s_wowQueue[s_wowHead]);
        s_wowHead = (s_wowHead + 1) % kWowQueue;
      }
      PanelCanvas cv;
      s_wow->render(cv, dt);
    } else {
      display.setTextSize(1);
      display.setTextColor(DISPLAY_WHITE);
      display.setCursor(4, 40);
      display.print("Style needs PSRAM");
    }
  }
#endif

  if (stale) {
    display.setTextSize(1);
    display.setTextColor(DISPLAY_WHITE);
    display.setCursor(25, 28);
    display.print("No audio data...");
  }

  if (settings.vizShowClock) {
    drawVizClock();
  }
}

/*
 * Capture task, source switch and the adapter into the visualizer. See audio_mic.h.
 */
#if defined(AUDIO_MIC_ONLY) && !defined(AUDIO_MIC_ENABLED)
#error "AUDIO_MIC_ONLY needs AUDIO_MIC_ENABLED"
#endif

#if defined(AUDIO_MIC_ENABLED)

#if !defined(BOARD_WAVESHARE_RGB_MATRIX)
#error "AUDIO_MIC_ENABLED drives the ES7210 of the Waveshare ESP32-S3-RGB-Matrix; no other board has it"
#endif
#if !defined(BOARD_HAS_PSRAM)
#error "AUDIO_MIC_ENABLED keeps its DSP buffers in PSRAM"
#endif

#include "audio_mic.h"

#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cmath>
#include <cstring>
#include <new>

#include "../config/config.h"
#include "../viz/visualizer.h"
#include "es7210.h"
#if defined(VIZ_WOW_ENABLED)
#include "../viz/wow/wow.h"
#endif

namespace {

// Waveshare BSP: BSP_I2S_PORT, BSP_I2S_MCLK / SCLK / LCLK / DSIN.
constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr int kPinMclk = 12, kPinBclk = 43, kPinWs = 38, kPinDin = 39;
// 4 x 256 frames x 2 channels x 2 bytes = 4 KB of internal DMA memory
// (driver/i2s.c allocates it MALLOC_CAP_DMA), 21 ms of slack before an overrun.
constexpr int kDmaBufs = 4, kDmaFrames = 256;
constexpr uint32_t kStackBytes = 4096;     // panel 2026-09-15: 1,232 of 5,120 B used; audit: I2S-failure path ~2.3-2.6 KB
constexpr UBaseType_t kPriority = 5;       // above the Lua and clip tasks (1), below Wi-Fi
constexpr BaseType_t kCore = 0;            // loop() and the panel refresh stay on core 1
constexpr unsigned long kPcFreshMs = 1500;
constexpr unsigned long kRetryMs = 30000;
constexpr unsigned long kStallMs = 1000;
constexpr unsigned long kActiveHoldMs = 5000;
constexpr unsigned long kBusBackoffMs = 60000;   // a held or stalled bus, as the climate reader backs off
// The task, its stack and the I2S driver hold ~10 KB of internal RAM while they
// run. They start only while the visualizer shows the microphones and stop this
// long after it stops: longer than a style change or the carousel passing through.
constexpr unsigned long kIdleStopMs = 25000;

enum MicState : uint8_t { MIC_IDLE, MIC_STARTING, MIC_OK, MIC_NO_CODEC, MIC_NO_I2S, MIC_NO_MEMORY, MIC_STALLED, MIC_NO_I2C,
                          MIC_I2C_HELD, MIC_I2C_STALLED };
const char *const kStateNames[] = {"idle", "starting", "ok", "no codec", "i2s failed", "no memory", "stalled", "no i2c bus",
                                   "i2c held low", "i2c stalled"};
const char *const kSourceNames[] = {"auto", "pc", "mic"};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
audiodsp::Frame s_shared;                  // internal RAM: copied only inside s_mux
bool s_haveFrame = false;

volatile MicState s_state = MIC_IDLE;
volatile bool s_settingsDirty = true;
volatile unsigned long s_lastActiveMs = 0; // loop() writes, the task reads
volatile uint32_t s_overruns = 0, s_stalls = 0, s_dspUs = 0, s_dspUsMax = 0, s_internalCost = 0;
volatile float s_meterDb = -120.0f;        // hop RMS, kept even while the DSP rests
volatile unsigned long s_meterClipMs = 0;
volatile bool s_meterClipped = false;
size_t s_internalBefore = 0;
// The task sets s_i2sReady once MCLK runs; the loop task then configures the
// codec over I2C and sets s_codecUp. A stall clears it for another bring-up.
volatile bool s_i2sReady = false, s_codecUp = false;
volatile unsigned long s_codecRetryMs = 0;
uint8_t s_gainApplied = 255;   // loop task only
uint32_t s_i2cHeld = 0, s_i2cStalls = 0;   // loop task only

// Start and stop. The loop task creates the task and asks it to stop; the task
// uninstalls I2S, records its stack margin, says it has gone and deletes itself.
// Nobody else ever calls a FreeRTOS function on its handle.
TaskHandle_t s_task = nullptr;             // loop task only; cleared once the task has gone
volatile bool s_stopReq = false;           // loop task -> task
volatile bool s_taskGone = false;          // task -> loop task, set just before vTaskDelete(NULL)
volatile uint32_t s_stackFree = 0;         // the task's own uxTaskGetStackHighWaterMark(NULL)
bool s_codecConfigured = false;            // loop task only: es7210::begin() succeeded since the last end()
uint32_t s_starts = 0;                     // loop task only
unsigned long s_wantedMs = 0, s_nextStartMs = 0;

// PSRAM, allocated once in audioBegin() and kept: ~52 KB the task reuses on every start.
audiodsp::Dsp *s_dsp = nullptr;
audiodsp::Frame *s_work = nullptr;
int16_t *s_hop = nullptr;
constexpr size_t kHopBytes = audiodsp::kHop * 2 * sizeof(int16_t);

#if defined(VIZ_WOW_ENABLED)
// Every DSP frame for visualizer styles 7-14: the task writes, audioPoll() drains, both inside s_mux.
constexpr int kWowRing = 16;
wow::VizFrame *s_wowRing = nullptr;   // PSRAM
int s_wowHead = 0, s_wowCount = 0;
volatile uint32_t s_wowLost = 0;

void toVizFrame(const audiodsp::Frame &a, wow::VizFrame &v) {
  memcpy(v.bands, a.bands, sizeof(v.bands));
  memcpy(v.wave, a.wave, sizeof(v.wave));
  memcpy(v.level, a.level, sizeof(v.level));
  memcpy(v.peak, a.peak, sizeof(v.peak));
  v.bass = a.bass;
  v.mid = a.mid;
  v.treble = a.treble;
  v.strength = a.strength;
  v.beat = a.beat;
  v.clipping = a.clipping;
  v.steps = 1;
  v.reserved = 0;
}
#endif

// loop() only.
unsigned long s_lastPcMs = 0;
bool s_pcEver = false;
uint32_t s_seenPackets = 0;

void *psram(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

uint8_t sourceSetting() {
#if defined(AUDIO_MIC_ONLY)
  return AUDIO_SRC_MIC;
#else
  return settings.audioSource <= AUDIO_SRC_MIC ? settings.audioSource : AUDIO_SRC_AUTO;
#endif
}

bool pcFresh() { return s_pcEver && millis() - s_lastPcMs <= kPcFreshMs; }

bool micFeedsViz() {
  switch (sourceSetting()) {
    case AUDIO_SRC_PC: return false;
    case AUDIO_SRC_MIC: return true;
    default: return !pcFresh();
  }
}

bool installI2s(QueueHandle_t &events) {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);   // RX-only master still drives MCLK
  cfg.sample_rate = audiodsp::kFs;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // MIC1 left, MIC2 right
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;  // the ES7210 is set to normal I2S
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = kDmaBufs;
  cfg.dma_buf_len = kDmaFrames;
  cfg.use_apll = false;                                  // the S3 has no APLL
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;             // 12.288 MHz, the ES7210's 48 kHz pair
  if (i2s_driver_install(kPort, &cfg, 8, &events) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = kPinMclk;
  pins.bck_io_num = kPinBclk;
  pins.ws_io_num = kPinWs;
  pins.data_out_num = -1;   // I2S_PIN_NO_CHANGE: IO21, the ES8311's input, is left alone
  pins.data_in_num = kPinDin;
  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kPort);
    return false;
  }
  return true;
}

void meter(const int16_t *hop) {
  double sum = 0.0;
  int peak = 0;
  for (int i = 0; i < audiodsp::kHop; i++) {
    const int l = hop[2 * i], r = hop[2 * i + 1];
    const double m = (l + r) * (0.5 / 32768.0);
    sum += m * m;
    peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
  }
  const double rms = std::sqrt(sum / audiodsp::kHop);
  s_meterDb = (float)std::max(-120.0, 20.0 * std::log10(std::max(rms, 1e-7)));
  if (peak >= 32700) {
    s_meterClipMs = millis();
    s_meterClipped = true;
  }
}

// Loop task: the ES7210 register sequence, so every I2C transaction on the
// shared bus happens on one core. MCLK is already running.
void codecBringUp() {
  if (!es7210::busReady()) {
    if (s_state != MIC_NO_I2C) Serial.println("[audio] the I2C bus is not started (boardI2cBegin); the ES7210 waits");
    s_state = MIC_NO_I2C;
    s_codecRetryMs = millis() + kRetryMs;
    return;
  }
  if (!es7210::busFree()) {   // a held line: no transaction at all, look again in a minute
    if (s_state != MIC_I2C_HELD) Serial.println("[audio] I2C SDA or SCL held low; the ES7210 waits");
    s_state = MIC_I2C_HELD;
    s_i2cHeld++;
    s_codecRetryMs = millis() + kBusBackoffMs;
    return;
  }
  const uint8_t gain = settings.micGainDb;
  if (!es7210::begin(gain)) {
    if (es7210::lastStalled()) {
      s_state = MIC_I2C_STALLED;
      s_i2cStalls++;
      Serial.println("[audio] an I2C transaction to the ES7210 stalled; nothing more sent for a minute");
      s_codecRetryMs = millis() + kBusBackoffMs;
    } else {
      s_state = MIC_NO_CODEC;
      Serial.printf("[audio] no ES7210 answering at 0x%02X on I2C 47/48\n", es7210::kAddr);
      s_codecRetryMs = millis() + kRetryMs;
    }
    return;
  }
  s_gainApplied = gain;
  s_codecConfigured = true;
  s_state = MIC_OK;
  s_codecUp = true;
  const size_t now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!s_internalCost) s_internalCost = s_internalBefore > now ? (uint32_t)(s_internalBefore - now) : 0;
  Serial.printf("[audio] ES7210 up: 48 kHz, gain %u dB, internal heap cost about %u B\n", (unsigned)gain,
                (unsigned)s_internalCost);
}

// The task's only way out: its own stack margin for /api/info, then gone.
[[noreturn]] void leaveTask() {
  s_stackFree = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  s_taskGone = true;
  vTaskDelete(nullptr);
  for (;;) {}
}

void captureTask(void *) {
  audiodsp::Dsp *dsp = s_dsp;
  audiodsp::Frame *work = s_work;
  int16_t *hop = s_hop;

  QueueHandle_t events = nullptr;
  s_state = MIC_STARTING;
  if (!installI2s(events)) {
    s_state = MIC_NO_I2S;
    Serial.println("[audio] I2S driver install failed");
    leaveTask();
  }
  vTaskDelay(pdMS_TO_TICKS(20));   // MCLK runs before the codec is touched, as the BSP orders it
  s_i2sReady = true;

  size_t got = 0;
  unsigned long lastData = millis();
  uint32_t passes = 0;
  while (!s_stopReq) {
    if ((++passes & 0xFF) == 0) s_stackFree = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (!s_codecUp) {
      // audioPoll() on the loop task configures the codec. Keep the DMA
      // flowing meanwhile and drop what arrives.
      size_t n = 0;
      i2s_read(kPort, reinterpret_cast<char *>(hop), kHopBytes, &n, pdMS_TO_TICKS(100));
      got = 0;
      lastData = millis();
      continue;
    }
    if (s_settingsDirty) {
      s_settingsDirty = false;
      audiodsp::Config c;
      c.agc = settings.micAgc;
      c.gateDb = settings.micGateDb;
      dsp->setConfig(c);
    }

    size_t n = 0;
    i2s_read(kPort, reinterpret_cast<char *>(hop) + got, kHopBytes - got, &n, pdMS_TO_TICKS(100));
    i2s_event_t ev;
    while (events && xQueueReceive(events, &ev, 0) == pdTRUE)
      if (ev.type == I2S_EVENT_RX_Q_OVF) s_overruns++;
    if (n == 0) {
      if (millis() - lastData > kStallMs) {
        // No samples for a second: audioPoll() brings the codec up again, 5 s
        // from now, so a broken driver does not re-init it every second.
        s_state = MIC_STALLED;
        s_stalls++;
        s_codecRetryMs = millis() + 5000;
        s_codecUp = false;
        got = 0;
      }
      continue;
    }
    lastData = millis();
    got += n;
    if (got < kHopBytes) continue;
    got = 0;

    meter(hop);
    if (s_meterClipped && millis() - s_meterClipMs > 500) s_meterClipped = false;
    if (millis() - s_lastActiveMs >= kActiveHoldMs) continue;   // nobody is watching: skip the DSP

    const int64_t t0 = esp_timer_get_time();
    const bool fresh = dsp->processHop(hop, *work);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_dspUs = us;
    if (us > s_dspUsMax) s_dspUsMax = us;
    if (fresh) {
      portENTER_CRITICAL(&s_mux);
      s_shared = *work;
      s_haveFrame = true;
#if defined(VIZ_WOW_ENABLED)
      if (s_wowRing && settings.vizStyle >= wow::kFirstStyle) {
        if (s_wowCount == kWowRing) {   // audioPoll() fell behind: the oldest goes
          s_wowHead = (s_wowHead + 1) % kWowRing;
          s_wowCount--;
          s_wowLost++;
        }
        toVizFrame(*work, s_wowRing[(s_wowHead + s_wowCount) % kWowRing]);
        s_wowCount++;
      }
#endif
      portEXIT_CRITICAL(&s_mux);
    }
  }
  s_i2sReady = false;
  i2s_driver_uninstall(kPort);   // frees the DMA buffers, the queues and the driver object (IDF 4.4.7 i2s.c)
  leaveTask();
}

// Loop task.
void startCapture(unsigned long now) {
  s_stopReq = false;
  s_taskGone = false;
  s_i2sReady = false;
  s_codecUp = false;
  s_codecRetryMs = 0;
  s_internalCost = 0;
  s_internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (xTaskCreatePinnedToCore(captureTask, "audio", kStackBytes, nullptr, kPriority, &s_task, kCore) != pdPASS) {
    s_task = nullptr;
    s_state = MIC_NO_MEMORY;
    s_nextStartMs = now + kRetryMs;
    Serial.printf("[audio] task not created: largest internal block %u B\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return;
  }
  s_starts++;
}

// Loop task, once the task has deleted itself: the codec's analog side off, the state back to idle.
void finishStop(unsigned long now) {
  const bool requested = s_stopReq;
  s_task = nullptr;
  s_stopReq = false;
  s_taskGone = false;
  s_codecUp = false;
  if (s_codecConfigured && es7210::busReady() && es7210::busFree()) es7210::end();
  s_codecConfigured = false;
  if (requested) {
    s_state = MIC_IDLE;
  } else {
    s_nextStartMs = now + kRetryMs;   // the task gave up on I2S: its state stays, retry later
  }
}

}  // namespace

void audioApplySettings() {
  clampAudioSettings();
  s_settingsDirty = true;
  // The web handlers call this on the loop task, so the gain goes over I2C from here.
  if (s_codecUp && settings.micGainDb != s_gainApplied && es7210::busFree() && es7210::setGainDb(settings.micGainDb))
    s_gainApplied = settings.micGainDb;
}

void audioBegin() {
  if (s_dsp) return;
  audioApplySettings();
  void *dspMem = psram(sizeof(audiodsp::Dsp));
  void *workMem = psram(sizeof(audiodsp::Frame));
  s_hop = static_cast<int16_t *>(psram(kHopBytes));
  audiodsp::Dsp *dsp = dspMem ? new (dspMem) audiodsp::Dsp() : nullptr;
  s_work = workMem ? new (workMem) audiodsp::Frame() : nullptr;
  if (!dsp || !s_work || !s_hop || !dsp->begin(psram)) {
    s_state = MIC_NO_MEMORY;   // the microphones never start
    Serial.println("[audio] no PSRAM for the DSP buffers");
    return;
  }
  s_dsp = dsp;
#if defined(VIZ_WOW_ENABLED)
  if (!s_wowRing) s_wowRing = static_cast<wow::VizFrame *>(psram(kWowRing * sizeof(wow::VizFrame)));
#endif
}

void audioPoll(bool vizShown) {
  const unsigned long now = millis();
  const bool wanted = vizShown && micFeedsViz();
  if (wanted) s_wantedMs = now;
  if (s_task && s_taskGone) finishStop(now);
  if (!s_task && wanted && s_dsp && (int32_t)(now - s_nextStartMs) >= 0) startCapture(now);
  if (s_task && !s_stopReq && !wanted && now - s_wantedMs >= kIdleStopMs) s_stopReq = true;

  if (s_task && !s_stopReq && s_i2sReady && !s_codecUp && (int32_t)(now - s_codecRetryMs) >= 0) codecBringUp();
  if (wanted) s_lastActiveMs = now;
  const bool feed = s_state == MIC_OK && micFeedsViz();
  uint8_t packet[audiodsp::kPacketLen];
  bool have;
  uint32_t packets;
  portENTER_CRITICAL(&s_mux);
  have = s_haveFrame;
  packets = s_shared.packets;
  if (have && packets != s_seenPackets) memcpy(packet, s_shared.packet, sizeof(packet));
  portEXIT_CRITICAL(&s_mux);
  if (have && packets != s_seenPackets) {
    s_seenPackets = packets;
#if defined(VIZ_WOW_ENABLED)
    if (feed) vizIngestMic(packet, sizeof(packet));   // styles 7-14 take the DSP's own frames, below
#else
    if (feed) vizIngest(packet, sizeof(packet));
#endif
  }
#if defined(VIZ_WOW_ENABLED)
  while (s_wowRing) {
    wow::VizFrame f;   // 436 B on the loop task's stack
    bool got = false;
    portENTER_CRITICAL(&s_mux);
    if (s_wowCount > 0) {
      f = s_wowRing[s_wowHead];
      s_wowHead = (s_wowHead + 1) % kWowRing;
      s_wowCount--;
      got = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!got) break;
    if (feed) vizWowFeed(f);
  }
#endif
}

bool audioAcceptPcPacket() {
  if (sourceSetting() == AUDIO_SRC_MIC) return false;
  s_lastPcMs = millis();
  s_pcEver = true;
  return true;
}

bool audioSnapshot(audiodsp::Frame &out) {
  portENTER_CRITICAL(&s_mux);
  const bool have = s_haveFrame;
  if (have) out = s_shared;
  portEXIT_CRITICAL(&s_mux);
  return have;
}

void audioInfoJson(JsonObject out) {
  float bpm = 0.0f, refDb = 0.0f, levelDb = 0.0f;
  uint32_t beats = 0, frames = 0;
  bool silent = false, clipping = false;
  portENTER_CRITICAL(&s_mux);
  const bool have = s_haveFrame;
  if (have) {
    bpm = s_shared.bpm;
    refDb = s_shared.refDb;
    levelDb = s_shared.levelDb;
    beats = s_shared.beats;
    frames = s_shared.k;
    silent = s_shared.silent;
    clipping = s_shared.clipping;
  }
  portEXIT_CRITICAL(&s_mux);
  const bool dspLive = have && millis() - s_lastActiveMs < kActiveHoldMs;

  const char *active = "none";
  if (micFeedsViz()) {
    if (s_state == MIC_OK) active = "mic";
  } else if (pcFresh()) {
    active = "pc";
  }
  out["audioMic"] = kStateNames[s_state];
  out["audioMicRunning"] = s_task != nullptr;   // the capture task and the I2S driver exist
  out["audioMicStarts"] = s_starts;
  out["audioSourceSetting"] = kSourceNames[sourceSetting()];
  out["audioSource"] = active;
  out["audioLevelDb"] = std::round((dspLive ? levelDb : (float)s_meterDb) * 10.0f) / 10.0f;
  out["audioClipping"] = dspLive ? clipping : (bool)s_meterClipped;
  out["audioDspLive"] = dspLive;
  out["audioBpm"] = std::round(bpm * 10.0f) / 10.0f;
  out["audioBeats"] = beats;
  out["audioSilent"] = silent;
  out["audioAgcRefDb"] = std::round(refDb * 10.0f) / 10.0f;
  out["audioFrames"] = frames;
  out["audioOverruns"] = (uint32_t)s_overruns;
  out["audioStalls"] = (uint32_t)s_stalls;
  out["audioDspUs"] = (uint32_t)s_dspUs;
  out["audioDspUsMax"] = (uint32_t)s_dspUsMax;
  out["audioStackFreeBytes"] = (uint32_t)s_stackFree;   // the task's own reading; 0 until it has run
  out["audioInternalBytes"] = (uint32_t)s_internalCost;
  out["audioI2cHeld"] = s_i2cHeld;       // bring-ups skipped: SDA or SCL held low
  out["audioI2cStalls"] = s_i2cStalls;   // bring-ups ended by a transaction over 100 ms
#if defined(VIZ_WOW_ENABLED)
  out["audioWowLost"] = (uint32_t)s_wowLost;   // DSP frames styles 7-14 never saw
#endif
}

#endif  // AUDIO_MIC_ENABLED

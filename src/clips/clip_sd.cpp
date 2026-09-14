/*
 * AnimatedPixelClock - Clip gallery on the TF card
 *
 * See clip_sd.h. The reader task owns the open file and the ring's head; the
 * loop task (render and web) owns requests and the ring's tail. A request is a
 * name and a generation count copied under a spinlock; the reader acts on a
 * new generation, publishes its state, then echoes the generation back, and
 * the loop task takes frames only when the echo matches its latest request -
 * so a ring reset can never race a take.
 */

#include "clip_sd.h"

#if defined(CLIPS_SD_ENABLED)

#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include "clip_stream.h"

static const int SD_PIN_CLK = 1;
static const int SD_PIN_CMD = 44;
static const int SD_PIN_D0 = 17;
// What the card stack under the reader needs - VFS, FATFS with its long-name
// buffer on the stack (CONFIG_FATFS_LFN_STACK), the SDMMC driver - is not
// measured. 6 KB is a guess with room; stackFree in /api/clips shows the margin
// on the panel (ESP-IDF's StackType_t is a byte, so the count is in bytes).
static const uint32_t READER_STACK = 6144;

static bool mounted = false;
static const char* reason = "the card has not been looked for yet";
static uint64_t totalBytes = 0;
static uint64_t freeBytes = 0;

static ClipStream stream;
static uint8_t* ringBuf = nullptr;
static uint16_t* delayBuf = nullptr;
static TaskHandle_t reader = nullptr;

static portMUX_TYPE reqMux = portMUX_INITIALIZER_UNLOCKED;
static char reqName[25] = "";            // under reqMux
static uint32_t reqGen = 0;              // under reqMux; written by the loop task only
static volatile uint32_t doneGen = 0;    // reader: the request it has acted on
static volatile uint8_t readerState = CLIP_SD_IDLE;
static char openName[25] = "";           // reader: for the stats

static void readerTask(void*) {
  File f;
  uint32_t gen = 0;
  for (;;) {
    char name[sizeof(reqName)];
    portENTER_CRITICAL(&reqMux);
    const uint32_t want = reqGen;
    memcpy(name, reqName, sizeof(name));
    portEXIT_CRITICAL(&reqMux);
    if (want != gen) {
      // Unpublish first: the loop task stops taking before the ring resets.
      readerState = CLIP_SD_IDLE;
      if (f) f.close();
      openName[0] = '\0';
      gen = want;
      if (name[0]) {
        f = SD_MMC.open(clipSdPath(name), FILE_READ);
        PcaHeader hdr;
        if (animValidatePcaMax(f, &hdr, CLIP_SD_MAX_FRAMES) && stream.begin(f, hdr)) {
          memcpy(openName, name, sizeof(openName));
          readerState = CLIP_SD_PLAYING;
        } else {
          if (f) f.close();
          readerState = CLIP_SD_FAILED;
        }
      }
      doneGen = gen;
    }
    if (readerState == CLIP_SD_PLAYING && !stream.fill(f, 4)) {
      f.close();  // card pulled, file cut short: the loop task retries
      readerState = CLIP_SD_FAILED;
    }
    // A request wakes the task at once; a full ring frees a slot every 40 ms.
    const bool full = readerState != CLIP_SD_PLAYING || stream.queued() >= CLIP_RING_FRAMES;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(full ? 10 : 1));
  }
}

bool clipSdMount() {
  if (mounted) return true;
  // 1-bit SD at 20 MHz (SDMMC_FREQ_DEFAULT), as mounted on the panel; 25 fps
  // of 4 KB frames is ~100 KB/s against the 1542 KB/s read measured there.
  if (!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0) ||
      !SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) {
    reason = "no card in the TF slot, or one that does not mount (FAT only, not exFAT)";
    return false;
  }
  if (!SD_MMC.exists(CLIP_SD_DIR) && !SD_MMC.mkdir(CLIP_SD_DIR)) {
    reason = "the card mounted, but /clips cannot be created on it";
    SD_MMC.end();
    return false;
  }
  if (SD_MMC.exists(CLIP_SD_TMP)) SD_MMC.remove(CLIP_SD_TMP);  // a broken-off upload
  if (!ringBuf) ringBuf = (uint8_t*)heap_caps_malloc((CLIP_RING_FRAMES + 1) * PCA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!delayBuf) delayBuf = (uint16_t*)heap_caps_malloc(CLIP_SD_MAX_FRAMES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ringBuf || !delayBuf) {
    reason = "no PSRAM for the read-ahead ring";
    SD_MMC.end();
    return false;
  }
  stream.init(ringBuf, delayBuf, CLIP_SD_MAX_FRAMES);
  if (!reader && xTaskCreatePinnedToCore(readerTask, "clipsd", READER_STACK, nullptr, 1, &reader, 0) != pdPASS) {
    reader = nullptr;
    reason = "the reader task did not start";
    SD_MMC.end();
    return false;
  }
  mounted = true;
  reason = "";
  clipSdRefresh();
  return true;
}

void clipSdInit() {
  if (clipSdMount()) {
    Serial.printf("ClipSD: %s card, %u of %u MB free\n", clipSdCardType(),
                  (unsigned)(freeBytes >> 20), (unsigned)(totalBytes >> 20));
  } else {
    Serial.printf("ClipSD: %s\n", reason);
  }
}

bool clipSdMounted() { return mounted; }
const char* clipSdReason() { return reason; }
uint64_t clipSdTotalBytes() { return totalBytes; }
uint64_t clipSdFreeBytes() { return freeBytes; }

const char* clipSdCardType() {
  if (!mounted) return "none";
  switch (SD_MMC.cardType()) {
    case CARD_SDHC: return "SDHC/SDXC";
    case CARD_SD: return "SDSC";
    case CARD_MMC: return "MMC";
    default: return "unknown";
  }
}

void clipSdRefresh() {
  if (!mounted) { totalBytes = freeBytes = 0; return; }
  totalBytes = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  freeBytes = totalBytes > used ? totalBytes - used : 0;
}

fs::FS& clipSdFs() { return SD_MMC; }

String clipSdPath(const char* name) { return String(CLIP_SD_DIR "/") + name + ".pca"; }

bool clipSdIsRef(const char* setting) { return strncmp(setting, CLIP_SD_REF, 3) == 0; }

static void request(const char* name) {
  portENTER_CRITICAL(&reqMux);
  strncpy(reqName, name, sizeof(reqName) - 1);
  reqName[sizeof(reqName) - 1] = '\0';
  reqGen++;
  portEXIT_CRITICAL(&reqMux);
  xTaskNotifyGive(reader);
}

void clipSdPlay(const char* name) {
  if (mounted && reader) request(name);
}

void clipSdStop() {
  if (!reader) return;
  request("");
  const unsigned long t0 = millis();
  while (doneGen != reqGen && millis() - t0 < 3000) vTaskDelay(1);
}

ClipSdState clipSdState() {
  return doneGen == reqGen ? (ClipSdState)readerState : CLIP_SD_IDLE;
}

bool clipSdTake(uint8_t* frame, uint16_t* delayMs) {
  return clipSdState() == CLIP_SD_PLAYING && stream.take(frame, delayMs);
}

void clipSdMissed() { stream.missed(); }

const uint16_t* clipSdPalette() { return stream.palette(); }

ClipSdStats clipSdStats() {
  // Fields the reader writes may be a read old; these are numbers to look at.
  ClipSdStats s{};
  s.state = clipSdState();
  memcpy(s.clip, openName, sizeof(s.clip));
  s.clip[sizeof(s.clip) - 1] = '\0';
  s.frames = stream.frames();
  s.reads = stream.reads;
  s.readUsAvg = stream.reads ? (uint32_t)(stream.readUsTotal / stream.reads) : 0;
  s.readUsMax = stream.readUsMax;
  s.underruns = stream.underruns;
  s.loops = stream.loops;
  s.queued = stream.queued();
  s.stackFree = reader ? (uint32_t)uxTaskGetStackHighWaterMark(reader) : 0;
  return s;
}

#endif  // CLIPS_SD_ENABLED

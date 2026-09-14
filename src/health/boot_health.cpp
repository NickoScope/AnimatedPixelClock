#include "boot_health.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <string.h>
#include <time.h>

// ── OTA rollback ────────────────────────────────────────────────────────────
// The SDK's bootloader rolls back an image that booted once and was not
// confirmed before the next reset (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
// arduino-esp32 2.0.17 confirms every image in initArduino(), before setup(),
// unless this weak hook says the application will do it
// (cores/esp32/esp32-hal-misc.c). That left rollback catching only an image
// that dies before setup(). Here the image is confirmed once it has proven
// itself, so one that crashes - or hangs into the task watchdog, which panics -
// before that is rolled back on the reset that follows.
#if CONFIG_APP_ROLLBACK_ENABLE
extern "C" bool verifyRollbackLater() { return true; }
#endif

// "Proven", in the owner's words (2026-09-14): the panel has shown a page and is
// on the network. A minute of running on top is a choice, not a measured
// figure: long enough for the first fetches and the first effect to have run.
// 200 frames is ten seconds of pages at the slowest animated rate, 20 Hz.
static const uint32_t kProveMs     = 60000;
static const uint32_t kProveFrames = 200;

static const esp_partition_t *s_running  = nullptr;
static const esp_partition_t *s_invalid  = nullptr;   // the last image that was rolled back
static esp_ota_img_states_t   s_state    = ESP_OTA_IMG_UNDEFINED;
static bool        s_stateKnown   = false;
static bool        s_pending      = false;
static uint32_t    s_confirmedAtMs = 0;
static uint32_t    s_frames       = 0;
static const char *s_confirmError = nullptr;

static const char *stateName() {
  if (!s_stateKnown) return "unknown";
  switch (s_state) {
  case ESP_OTA_IMG_NEW:            return "new";
  case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
  case ESP_OTA_IMG_VALID:          return "valid";
  case ESP_OTA_IMG_INVALID:        return "invalid";
  case ESP_OTA_IMG_ABORTED:        return "aborted";
  default:                         return "undefined";   // flashed over USB: no OTA state
  }
}

// ── last crash ──────────────────────────────────────────────────────────────
// The SDK writes a core dump to flash on a panic (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
// ELF). Its summary is read at the next boot, kept in NVS and the dump erased,
// so the one after that is not mistaken for this one. Decoding the backtrace
// needs the ELF of the image named by `sha`.
struct CrashRecord {
  uint32_t magic;
  char     task[16];
  uint32_t pc;
  uint32_t cause;          // Xtensa EXCCAUSE
  uint32_t vaddr;          // EXCVADDR
  uint32_t bt[8];
  uint8_t  depth;
  bool     corrupted;
  char     sha[17];        // first 16 hex digits of the crashed image's ELF SHA-256
  int32_t  bootReason;     // esp_reset_reason() of the boot that found it
  int64_t  seenUtc;        // when that boot first had the time; 0 until then
};
static const uint32_t kCrashMagic = 0x31445243;   // "CRD1"
static const char    *kNs = "health";
static CrashRecord s_crash = {};
static bool s_crashThisBoot = false;
static bool s_crashNeedsUtc = false;

// Xtensa exception causes, in the order of ESP-IDF's own table
// (components/esp_system/port/arch/xtensa/panic_arch.c, panic_arch_fill_info).
static const char *causeName(uint32_t c) {
  static const char *const kNames[] = {
    "IllegalInstruction", "Syscall", "InstructionFetchError", "LoadStoreError",
    "Level1Interrupt", "Alloca", "IntegerDivideByZero", "PCValue",
    "Privileged", "LoadStoreAlignment", "res", "res",
    "InstrPDAddrError", "LoadStorePIFDataError", "InstrPIFAddrError", "LoadStorePIFAddrError",
    "InstTLBMiss", "InstTLBMultiHit", "InstFetchPrivilege", "res",
    "InstrFetchProhibited", "res", "res", "res",
    "LoadStoreTLBMiss", "LoadStoreTLBMultihit", "LoadStorePrivilege", "res",
    "LoadProhibited", "StoreProhibited",
  };
  return c < sizeof(kNames) / sizeof(kNames[0]) ? kNames[c] : "other";
}

static void saveCrash() {
  Preferences p;
  if (p.begin(kNs, false)) {
    p.putBytes("crash", &s_crash, sizeof(s_crash));
    p.end();
  }
}

static void readCrashReport() {
  Preferences p;
  if (p.begin(kNs, false)) {   // read-write: a read-only open of a new namespace logs an error
    if (p.getBytesLength("crash") == sizeof(s_crash)) p.getBytes("crash", &s_crash, sizeof(s_crash));
    p.end();
  }
  if (s_crash.magic != kCrashMagic) memset(&s_crash, 0, sizeof(s_crash));

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) return;
  esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)calloc(1, sizeof(*sum));
  if (!sum) return;
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    CrashRecord c = {};
    c.magic = kCrashMagic;
    strncpy(c.task, sum->exc_task, sizeof(c.task) - 1);
    c.pc    = sum->exc_pc;
    c.cause = sum->ex_info.exc_cause;
    c.vaddr = sum->ex_info.exc_vaddr;
    const uint32_t depth = sum->exc_bt_info.depth;
    c.depth = (uint8_t)(depth < 8 ? depth : 8);
    for (uint8_t i = 0; i < c.depth; i++) c.bt[i] = sum->exc_bt_info.bt[i];
    c.corrupted = sum->exc_bt_info.corrupted;
    memcpy(c.sha, sum->app_elf_sha256, sizeof(c.sha) - 1);
    c.sha[sizeof(c.sha) - 1] = '\0';
    c.bootReason = (int32_t)esp_reset_reason();
    s_crash = c;
    s_crashThisBoot = true;
    s_crashNeedsUtc = true;
    saveCrash();
    Serial.printf("[health] crash report from the last run: task %s, %s (%u), pc 0x%08x, addr 0x%08x, image %s\n",
                  c.task, causeName(c.cause), (unsigned)c.cause, (unsigned)c.pc, (unsigned)c.vaddr, c.sha);
  } else {
    Serial.println("[health] a crash report is in flash but could not be read");
  }
  free(sum);
  // Erased either way: a report that cannot be read now will not become readable.
  esp_core_dump_image_erase();
#endif
}

void healthBegin() {
  s_running = esp_ota_get_running_partition();
  if (s_running && esp_ota_get_state_partition(s_running, &s_state) == ESP_OK) {
    s_stateKnown = true;
    s_pending    = (s_state == ESP_OTA_IMG_PENDING_VERIFY);
  }
  s_invalid = esp_ota_get_last_invalid_partition();
  Serial.printf("[health] running %s, OTA state %s%s%s\n", s_running ? s_running->label : "?", stateName(),
                s_invalid ? ", rolled back from " : "", s_invalid ? s_invalid->label : "");
  readCrashReport();
}

void healthNoteFrame() {
  if (s_frames < 0xFFFFFFFFUL) s_frames++;
}

void healthTick(bool displayIdle) {
  if (s_crashNeedsUtc) {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      s_crash.seenUtc = (int64_t)now;
      s_crashNeedsUtc = false;
      saveCrash();
    }
  }
#if CONFIG_APP_ROLLBACK_ENABLE
  if (!s_pending) return;
#if defined(HEALTH_ROLLBACK_TEST)
  // Test image only, built with -DHEALTH_ROLLBACK_TEST: it dies before it can
  // be confirmed, so the bootloader has to put the previous image back.
  if (millis() > 20000UL) {
    Serial.println("[health] HEALTH_ROLLBACK_TEST: aborting before the image is confirmed");
    Serial.flush();
    abort();
  }
  return;
#endif
  if (millis() < kProveMs || WiFi.status() != WL_CONNECTED) return;
  if (!displayIdle && s_frames < kProveFrames) return;
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  s_pending = false;
  if (err == ESP_OK) {
    s_state = ESP_OTA_IMG_VALID;
    s_confirmedAtMs = millis();
    Serial.printf("[health] OTA image confirmed after %u s, %u frames: no rollback from here\n",
                  (unsigned)(s_confirmedAtMs / 1000UL), (unsigned)s_frames);
  } else {
    s_confirmError = esp_err_to_name(err);
    Serial.printf("[health] could not confirm the OTA image: %s\n", s_confirmError);
  }
#else
  (void)displayIdle;
#endif
}

void healthInfoJson(JsonObject out) {
  JsonObject ota = out["ota"].to<JsonObject>();
  ota["partition"] = s_running ? String(s_running->label) : String();
  ota["state"]     = stateName();
  if (s_invalid) ota["rolledBackFrom"] = String(s_invalid->label);
  if (s_confirmedAtMs) ota["confirmedAtS"] = s_confirmedAtMs / 1000UL;
  if (s_pending) {
    ota["confirmsAfterS"] = kProveMs / 1000UL;
    ota["frames"]         = s_frames;
  }
  if (s_confirmError) ota["error"] = s_confirmError;

  if (s_crash.magic != kCrashMagic) return;
  JsonObject c = out["lastCrash"].to<JsonObject>();
  char hex[12];
  c["task"]      = String(s_crash.task);
  c["cause"]     = s_crash.cause;
  c["causeName"] = causeName(s_crash.cause);
  snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s_crash.pc);    c["pc"]   = String(hex);
  snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s_crash.vaddr); c["addr"] = String(hex);
  c["image"]      = String(s_crash.sha);
  c["bootReason"] = s_crash.bootReason;
  c["thisBoot"]   = s_crashThisBoot;
  if (s_crash.seenUtc) c["seenUtc"] = s_crash.seenUtc;
  JsonArray bt = c["backtrace"].to<JsonArray>();
  for (uint8_t i = 0; i < s_crash.depth; i++) {
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s_crash.bt[i]);
    bt.add(String(hex));
  }
  if (s_crash.corrupted) c["backtraceCorrupted"] = true;
}

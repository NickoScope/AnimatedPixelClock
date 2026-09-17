#include "boot_health.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

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

void healthBegin() {
  s_running = esp_ota_get_running_partition();
  if (s_running && esp_ota_get_state_partition(s_running, &s_state) == ESP_OK) {
    s_stateKnown = true;
    s_pending    = (s_state == ESP_OTA_IMG_PENDING_VERIFY);
  }
  s_invalid = esp_ota_get_last_invalid_partition();
  Serial.printf("[health] running %s, OTA state %s%s%s\n", s_running ? s_running->label : "?", stateName(),
                s_invalid ? ", rolled back from " : "", s_invalid ? s_invalid->label : "");
}

void healthNoteFrame() {
  if (s_frames < 0xFFFFFFFFUL) s_frames++;
}

void healthTick(bool displayIdle) {
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
}

#include "net_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_lock = nullptr;
static portMUX_TYPE      s_initMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     s_busy = false;

// Created on first use from whichever task gets there first. The mutex is
// allocated outside the critical section, which must not allocate.
static SemaphoreHandle_t lockHandle() {
  if (s_lock) return s_lock;
  SemaphoreHandle_t m = xSemaphoreCreateMutex();
  portENTER_CRITICAL(&s_initMux);
  if (!s_lock) { s_lock = m; m = nullptr; }
  portEXIT_CRITICAL(&s_initMux);
  if (m) vSemaphoreDelete(m);
  return s_lock;
}

bool netLockBusy() { return s_busy; }

bool netLockTake(uint32_t waitMs) {
  const SemaphoreHandle_t l = lockHandle();
  if (!l || xSemaphoreTake(l, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
  s_busy = true;
  return true;
}

void netLockGive() {
  s_busy = false;
  if (s_lock) xSemaphoreGive(s_lock);
}

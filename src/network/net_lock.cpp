#include "net_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_lock = nullptr;
static portMUX_TYPE      s_initMux = portMUX_INITIALIZER_UNLOCKED;

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

// Asked of the mutex itself, not of a flag beside it. The flag was set after
// the take and cleared before the give, so between one fetch giving and the
// next taking it read "free" while the network was about to be busy again.
// That was tolerable when the only cost was an extra fetch; now the web server
// reads this to decide whether to send kilobytes, and a wrong "free" puts them
// on the wire in the middle of a TLS handshake - the exact collision this is
// here to prevent.
bool netLockBusy() { return s_lock && xSemaphoreGetMutexHolder(s_lock) != nullptr; }

bool netLockTake(uint32_t waitMs) {
  const SemaphoreHandle_t l = lockHandle();
  return l && xSemaphoreTake(l, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void netLockGive() {
  if (s_lock) xSemaphoreGive(s_lock);
}

#include "tls_psram.h"

#if defined(BOARD_HAS_PSRAM)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "mbedtls/platform.h"

// The precompiled arduino-esp32 2.0.17 libraries are configured with
// CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC (tools/sdk/esp32s3/opi_opi/include/sdkconfig.h),
// so every TLS session takes its buffers from internal SRAM - on this board the
// scarcest memory there is, next to 16 MB of PSRAM.
//
// The same libraries' mbedtls/esp_config.h defines MBEDTLS_PLATFORM_MEMORY with
// MBEDTLS_PLATFORM_STD_CALLOC rather than the CALLOC/FREE macros, and that is
// the configuration in which mbedtls/platform.h keeps
// mbedtls_platform_set_calloc_free(). Calling it has the effect of
// CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, chosen at run time instead of at library
// build time. Anything the default allocator handed out before the switch is
// still freed correctly: heap_caps_free() accepts a pointer from any heap.
static void *psramCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

static void psramFree(void *p) { heap_caps_free(p); }

void tlsUsePsram() {
  if (!psramFound()) return;
  mbedtls_platform_set_calloc_free(psramCalloc, psramFree);
}

#endif  // BOARD_HAS_PSRAM

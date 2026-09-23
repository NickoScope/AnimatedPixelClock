#include "psram_state.h"

#if defined(BOARD_HAS_PSRAM)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

// Namespace-scope state is set up by the global constructors. Whether PSRAM is
// in the heap by then depends on the prebuilt config for the board's memory
// type (tools/sdk/esp32s3/<type>/include/sdkconfig.h):
//   opi_opi (Waveshare), qio_opi (WROOM N16R8): CONFIG_SPIRAM_BOOT_INIT is on,
//     IDF adds PSRAM to the heap in do_core_init(), before do_global_ctors(),
//     and psramInit() returns at once.
//   qio_qspi (S3-Zero): it is off, and psramInit() - what initArduino() calls
//     later anyway, and a no-op once done (esp32-hal-psram.c) - does the init
//     here. The steps are the ones BOOT_INIT runs even earlier in
//     call_start_cpu0, and the second core is still parked in IRAM
//     (call_start_cpu1) until do_secondary_init(), after the constructors.
// If there is no PSRAM, the state goes internal as it did before.
//
// The heap aligns to 4 bytes (heap_tlsf_config.h), and some state holds
// int64_t (AeroTracker), so the alignment of the element type is asked for.
static size_t s_bytes = 0;

void *psramStateAlloc(size_t bytes, size_t align) {
  if (align < 4) align = 4;
  void *p = nullptr;
  if (psramInit()) p = heap_caps_aligned_calloc(align, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) s_bytes += bytes;
  else p = heap_caps_aligned_calloc(align, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) abort();   // a reference cannot be null; with no memory at all there is no boot
  return p;
}

size_t psramStateBytes() { return s_bytes; }

#endif

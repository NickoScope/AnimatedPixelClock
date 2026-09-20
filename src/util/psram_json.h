#pragma once
// One JSON allocator that puts the document's blocks in PSRAM.
//
// It has to be said once, not four times. In this build
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 4096, so every block ArduinoJson asks
// for - they are smaller than that - lands in the INTERNAL heap however much
// PSRAM is free. That is the heap the Wi-Fi task takes its receive buffers
// from, and the one this panel runs short of: measured 2026-09-20, internal
// free reached 144 B with a fetch and the portal at once and the link dropped.
//
// There were four byte-identical copies of this class - in web.cpp, weather.cpp,
// wc_home.cpp and yachtradar.cpp - so a change like the one below would have
// had to be made four times.

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
};

inline ArduinoJson::Allocator *psramJson() {
  static PsramJsonAllocator a;
  return &a;
}

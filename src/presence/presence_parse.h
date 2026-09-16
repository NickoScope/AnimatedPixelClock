#pragma once
// The contract's payload, turned into Reports, without touching the heap.
//
// The agreed message (docs/16-presence-radar.md in the knowledge base):
//   {"t":[[x_mm,y_mm,v_mmps]|null, ...3], "p":int, "m":int, "s":int,
//    "lux":int|null, "ts":unix}
// and the retained summary adds "online":bool. Values are raw from the sensor:
// x signed sideways, y forward, v signed mm/s.
//
// Why not a stack StaticJsonDocument, which is what one would reach for: it no
// longer exists. In the ArduinoJson this build pins (7.4.3) it survives only in
// src/ArduinoJson/compatibility.hpp, as a DEPRECATED subclass of JsonDocument
// that inherits its constructors and overrides capacity() to return N. It
// carries no storage of its own and allocates through DefaultAllocator -
// malloc, on the internal heap - so it would cost precisely what this module
// must not cost, and the N would buy nothing.
//
// A fixed block of our own was tried next and does not work either, for the
// reason measured above the cap below: the library wants one 4 KB pool for a
// 100-byte message. Keeping 4 KB of DRAM parked in .bss for the life of the
// board is the same mistake in a different place - internal RAM is the scarce
// resource here (docs/22 §12.3: the audio visualizer exhausted it and took the
// network down). So the memory comes from PSRAM, capped, exactly as the other
// MQTT consumers on this board do it (RbJsonAllocator in
// src/railboard/railboard.cpp, the two allocators in src/market/market_ha.cpp).
// It is the parse in an MQTT callback, never the render path.
//
// Plain C++ apart from ArduinoJson, so tools/presence/check_presence.py builds
// and runs it on this machine, where the same cap is applied over malloc.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include <esp_heap_caps.h>
#endif

#include "presence_model.h"

#ifndef PRESENCE_JSON_CAP
// Measured, not guessed. For the contract's three-target message (100 bytes)
// ArduinoJson 7.4.3 asks for a 46-byte string buffer, then ONE pool of 4096
// bytes, and only then shrinks that pool to 416. So a fixed block sized by the
// message is useless: the transient 4 KB is what has to fit. The cap is that
// peak with headroom; past it deserializeJson() reports NoMemory, the payload
// is refused, and the panel keeps showing what it already had.
#define PRESENCE_JSON_CAP 8192
#endif

namespace presence {

// A bump allocator over one fixed block. deallocate() does nothing; the whole
// block is reclaimed by reset() before each parse, which is the only lifetime
// a parse needs. No malloc, no free, no fragmentation, no heap.
class JsonPool : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t n) override { return reallocate(nullptr, n); }

  void deallocate(void *ptr) override {
    if (!ptr) return;
    Hdr *h = static_cast<Hdr *>(ptr) - 1;
    used_ -= h->size;
    freeBlock(h);
  }

  void *reallocate(void *ptr, size_t n) override {
    Hdr *old = ptr ? static_cast<Hdr *>(ptr) - 1 : nullptr;
    const size_t was = old ? old->size : 0;
    if (used_ - was + n > PRESENCE_JSON_CAP) return nullptr;   // refused, not grown without limit
    Hdr *h = static_cast<Hdr *>(reallocBlock(old, sizeof(Hdr) + n));
    if (!h) return nullptr;
    h->size = n;
    used_ = used_ - was + n;
    if (used_ > peak_) peak_ = used_;
    return h + 1;
  }

  size_t peak() const { return peak_; }
  size_t used() const { return used_; }
  size_t cap() const { return PRESENCE_JSON_CAP; }

 private:
  // The size lives in front of each block, aligned like the heap itself, so
  // what is handed back keeps the alignment ArduinoJson may rely on.
  union Hdr {
    size_t      size;
    long double align;
  };

#if defined(ARDUINO)
  // PSRAM first. The fallback to internal exists so a board without PSRAM still
  // works; the cap above is what keeps that case bounded.
  static void *reallocBlock(void *p, size_t n) {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
  static void freeBlock(void *p) { heap_caps_free(p); }
#else
  static void *reallocBlock(void *p, size_t n) { return realloc(p, n); }
  static void  freeBlock(void *p) { free(p); }
#endif

  size_t used_ = 0, peak_ = 0;
};

// What the message says about the room besides the three targets. Only for
// /api/info and the portal: none of it draws anybody.
struct Summary {
  bool     have = false;
  int32_t  people = 0, moving = 0, still = 0;
  bool     haveLux = false;
  int32_t  lux = 0;
  bool     haveOnline = false;
  bool     online = false;
  uint32_t ts = 0;
};

// Fills r[] for every slot - an absent slot comes back present = false - and
// sum when it is not null. False means the payload was not the contract's, and
// then nothing has been written that the caller should believe.
inline bool parse(JsonPool &pool, const char *json, size_t len, Report r[kSlots], Summary *sum) {
  for (uint8_t i = 0; i < kSlots; i++) r[i] = Report{false, 0, 0, 0};
  if (!json || !len) return false;

  // The document gives every block back when it goes out of scope here, so the
  // pool's used() is zero between messages and only peak() remembers.
  ArduinoJson::JsonDocument doc(&pool);
  if (deserializeJson(doc, json, len) != ArduinoJson::DeserializationError::Ok) return false;
  if (!doc.is<ArduinoJson::JsonObjectConst>()) return false;

  ArduinoJson::JsonArrayConst t = doc["t"];
  if (t.isNull()) return false;            // a message with no targets array is not ours

  uint8_t i = 0;
  for (ArduinoJson::JsonVariantConst e : t) {
    if (i >= kSlots) break;
    ArduinoJson::JsonArrayConst a = e;
    // null is an empty slot, and so is a triple that is not three numbers.
    if (!a.isNull() && a.size() >= 3 &&
        a[0].is<int32_t>() && a[1].is<int32_t>() && a[2].is<int32_t>()) {
      r[i].present = true;
      r[i].x = a[0].as<int32_t>();
      r[i].y = a[1].as<int32_t>();
      r[i].v = a[2].as<int32_t>();
    }
    i++;
  }

  if (sum) {
    sum->have = true;
    sum->people = doc["p"] | 0;
    sum->moving = doc["m"] | 0;
    sum->still = doc["s"] | 0;
    sum->haveLux = doc["lux"].is<int32_t>();
    sum->lux = sum->haveLux ? doc["lux"].as<int32_t>() : 0;
    sum->haveOnline = doc["online"].is<bool>();
    sum->online = sum->haveOnline ? doc["online"].as<bool>() : false;
    sum->ts = doc["ts"] | 0u;
  }
  return true;
}

}  // namespace presence

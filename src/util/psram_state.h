#pragma once
// Page and effect state in PSRAM instead of internal SRAM.
//
// Internal SRAM is the radio's memory: the Wi-Fi driver takes its 1,626 B
// receive buffers from the same DMA-capable internal heap, and on 2.5.5 that
// pool fell to 172 B in ordinary running (self-test 2026-09-23). Our own
// zero-initialised arrays in .bss took 67 KB of it. The prebuilt arduino-esp32
// 2.0.17 config cannot place .bss in PSRAM (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
// is off), so the state that only the pages and effects touch from loop() or
// their own tasks is allocated there instead, once, by the global constructors
// (keep these at file scope, not function-local, so that happens at boot):
//
//   static uint8_t buf[N];            becomes   static PSRAM_ARRAY(uint8_t, buf, [N]);
//   extern Frag frags[N];             becomes   PSRAM_ARRAY_EXTERN(Frag, frags, [N]);
//   static Board b;                   becomes   static PSRAM_OBJECT(Board, b);
//
// The name is a reference to the array, so indexing, decay to a pointer and
// sizeof(buf) mean exactly what they meant before. The memory is zeroed, which
// is what .bss gave, so only trivial types are allowed (checked at compile
// time). Not for anything touched by an interrupt, by DMA, or while the flash
// cache is off, and never for task stacks (the kernel asserts they are internal).
//
// Without PSRAM in the build the macros declare the plain arrays as before.

#include <stddef.h>

#if defined(BOARD_HAS_PSRAM)

#include <type_traits>

void *psramStateAlloc(size_t bytes, size_t align);   // zeroed; PSRAM, or internal if there is none
size_t psramStateBytes();              // how much state was placed in PSRAM

template <typename T>
T &psramState() {
  typedef typename std::remove_all_extents<T>::type Elem;
  static_assert(std::is_trivial<Elem>::value, "PSRAM state must be a trivial type: it is zeroed, not constructed");
  return *static_cast<T *>(psramStateAlloc(sizeof(T), alignof(Elem)));
}

#define PSRAM_ARRAY(elem, name, dims) elem (&name) dims = psramState<elem dims>()
#define PSRAM_ARRAY_EXTERN(elem, name, dims) extern elem (&name) dims
#define PSRAM_OBJECT(type, name) type &name = psramState<type>()

#else

inline size_t psramStateBytes() { return 0; }
#define PSRAM_ARRAY(elem, name, dims) elem name dims
#define PSRAM_ARRAY_EXTERN(elem, name, dims) extern elem name dims
#define PSRAM_OBJECT(type, name) type name

#endif

// ============================================================
// lua_fx.h - one Lua effect in one persistent lua_State
// ============================================================
// Open once: fresh state, the nslua whitelist (nslua_sandbox.h), px bound to
// a canvas, the script run as a text chunk. Then draw() once per frame: the
// global draw() under an instruction budget and a wall-clock deadline, both
// reset per call. Close on leaving the page, which frees the whole heap.
//
// Any failure - syntax, runtime error, budget, deadline, the heap cap - comes
// back as false with a message; the state is left open after a draw error so
// the caller decides. Nothing here raises past lua_pcall.
//
// Not thread-safe: one LuaFx belongs to one task. Several LuaFx may exist at
// once (the budget lives in the object, not in a file-static like nslua.cpp).
// Host-portable, so tools/luasim/fxhost runs exactly this code on a Mac.
// ============================================================
#ifndef LUA_FX_H
#define LUA_FX_H

#include <stddef.h>
#include <stdint.h>

#include "lua_px.h"

struct lua_State;
struct lua_Debug;

struct LuaFxLimits {
  uint32_t loadInstructions;   // running the chunk once: tables, precomputed grids
  uint32_t drawInstructions;   // one draw() call
  uint32_t loadMs;             // wall-clock second line for the load
  uint32_t drawMs;             // ... and for one draw()
  size_t   heapBytes;          // the whole Lua heap of this effect
};

// The budgets the panel runs effects under. Sized from the shipped scripts'
// exact instruction counts and heap peaks (tools/luasim/fx_parity.py prints
// them) with headroom for scripts not yet written; see src/lua/README.md.
static const LuaFxLimits kLuaFxPanelLimits = {
  20000000u,          // load: room_radar precomputes its whole fan here
  2000000u,           // draw: the same figure nslua_run has always used
  3000u,              // load deadline, ms
  500u,               // draw deadline, ms
  4u * 1024u * 1024u, // heap: 4 MB of the 16 MB PSRAM
};

#define LUA_FX_DEFAULT_FPS 20

class LuaFx {
public:
  LuaFx() = default;
  ~LuaFx() { close(); }
  LuaFx(const LuaFx &) = delete;
  LuaFx &operator=(const LuaFx &) = delete;

  // name is the chunk name in error messages ("room_radar:12: ...").
  bool open(const char *name, const char *src, size_t len, LuaPxCanvas *canvas,
            const LuaFxLimits &limits);
  bool draw();
  void close();

  bool        isOpen() const { return L_ != nullptr; }
  const char *error() const  { return err_; }

  // Read once after the load. PERIOD: seconds that px.t() spans, default 60.
  // FPS: the effect's own frame cap, default LUA_FX_DEFAULT_FPS, 1..30.
  double  periodSeconds() const { return period_; }
  uint8_t fps() const           { return fps_; }

  // Instructions the last load or draw() used, rounded down to the hook step.
  uint32_t lastInstructions() const { return used_; }
  size_t   heapBytes() const        { return heap_; }
  size_t   heapPeak() const         { return heapPeak_; }

  // 1000 by default. 1 counts exactly, for measuring on the host.
  void setHookStep(int step) { hookStep_ = step > 0 ? step : 1; }
  // Generational by default: an effect makes the same short-lived tables every
  // frame, and on the shipped scripts it peaked at no more heap than Lua's
  // incremental collector, at the same draw time (fx_parity.py measures both).
  // The Watch runs its persistent state the same way. Takes effect at open().
  void setGenerationalGc(bool on) { generationalGc_ = on; }

  // Allocator and hook are C callbacks; they reach the object through these.
  static void *alloc(void *ud, void *ptr, size_t osize, size_t nsize);
  static void  hook(lua_State *L, lua_Debug *ar);

private:
  void arm(uint32_t instructions, uint32_t ms);
  void takeError(int status);

  lua_State  *L_ = nullptr;
  LuaFxLimits limits_ = {0, 0, 0, 0, 0};
  int         hookStep_ = 1000;
  bool        generationalGc_ = true;
  uint32_t    budget_ = 0, used_ = 0, sinceClock_ = 0;
  uint32_t    startMs_ = 0, deadlineMs_ = 0;
  size_t      heap_ = 0, heapPeak_ = 0;
  double      period_ = 60.0;
  uint8_t     fps_ = LUA_FX_DEFAULT_FPS;
  char        err_[192] = {0};
};

#endif  // LUA_FX_H

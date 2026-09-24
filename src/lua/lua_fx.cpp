// ============================================================
// lua_fx.cpp - one Lua effect in one persistent lua_State. See lua_fx.h.
// ============================================================
#include "lua_fx.h"

#if defined(LUA_EFFECTS_ENABLED) || !defined(ARDUINO)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <time.h>
#endif

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
#include "vendor/lua/lualib.h"
}

#include "nslua_sandbox.h"

#if defined(PRESENCE_ENABLED)
#include "../presence/presence.h"   // guarded: the host build has no ArduinoJson
#endif

static uint32_t nowMs() {
#if defined(ARDUINO)
  return millis();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

// lua_Alloc with a cap. Only growth may be refused: Lua assumes a shrink never
// fails (5.4 manual, lua_Alloc). When ptr is NULL, osize is a type tag, not a
// size, so it does not count.
void *LuaFx::alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  LuaFx *fx = static_cast<LuaFx *>(ud);
  const size_t old = ptr ? osize : 0;
  if (nsize == 0) {
    if (ptr) {
#if defined(ARDUINO)
      heap_caps_free(ptr);
#else
      free(ptr);
#endif
      fx->heap_ -= old;
    }
    return nullptr;
  }
  if (nsize > old && fx->heap_ - old + nsize > fx->limits_.heapBytes) return nullptr;
#if defined(ARDUINO)
  // PSRAM: the internal heap is down to tens of KB with the firmware running.
  void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *p = realloc(ptr, nsize);
#endif
  if (!p) return nullptr;
  fx->heap_ = fx->heap_ - old + nsize;
  if (fx->heap_ > fx->heapPeak_) fx->heapPeak_ = fx->heap_;
  return p;
}

// The count hook: instructions first, then the clock about every thousand
// instructions. The budget lives in the object, found through the state's
// extra space, so two effects (or nslua_run) never share a counter.
// coroutine is not opened in the sandbox, so no thread can start with a fresh
// hook count behind the budget's back.
void LuaFx::hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  LuaFx *fx = *static_cast<LuaFx **>(lua_getextraspace(L));
  fx->used_ += (uint32_t)fx->hookStep_;
  if (fx->used_ >= fx->budget_) {
    luaL_error(L, "over the instruction budget (%d)", (int)fx->budget_);
    return;
  }
  fx->sinceClock_ += (uint32_t)fx->hookStep_;
  if (fx->sinceClock_ >= 1000u) {
    fx->sinceClock_ = 0;
    if (nowMs() - fx->startMs_ >= fx->deadlineMs_)
      luaL_error(L, "over the time budget (%d ms)", (int)fx->deadlineMs_);
  }
}

void LuaFx::charge(lua_State *L, uint32_t instructions) {
  LuaFx *fx = *static_cast<LuaFx **>(lua_getextraspace(L));
  if (!fx) return;
  fx->used_ += instructions;
  if (fx->used_ >= fx->budget_)
    luaL_error(L, "over the instruction budget (%d)", (int)fx->budget_);
  if (nowMs() - fx->startMs_ >= fx->deadlineMs_)
    luaL_error(L, "over the time budget (%d ms)", (int)fx->deadlineMs_);
}

void LuaFx::arm(uint32_t instructions, uint32_t ms) {
  budget_ = instructions;
  used_ = 0;
  sinceClock_ = 0;
  startMs_ = nowMs();
  deadlineMs_ = ms;
  lua_sethook(L_, hook, LUA_MASKCOUNT, hookStep_);   // also restarts the count
}

void LuaFx::takeError(int status) {
  const char *m = lua_tostring(L_, -1);
  if (!m) m = (status == LUA_ERRMEM) ? "not enough memory" : "unknown Lua error";
  strncpy(err_, m, sizeof(err_) - 1);
  err_[sizeof(err_) - 1] = 0;
}

namespace {
struct OpenCtx {
  const char  *src;
  size_t       len;
  const char  *chunk;
  LuaPxCanvas *canvas;
  double       period;
  lua_Number   fps;
};

// Opening the libraries allocates, so it runs under a pcall of its own.
int libsProtected(lua_State *L) {
  OpenCtx *c = static_cast<OpenCtx *>(lua_touserdata(L, 1));
  nslua_sandbox_open(L);
  luaPxOpen(L, c->canvas);
#if defined(PRESENCE_ENABLED)
  // The room radar's real targets, bound before the chunk runs so the script
  // can read the scale at load. Not the P&P registry (nslua_bindings.h): that
  // is opened by nslua.cpp only, and pulling it in here would put
  // nslua_bindings.cpp into tools/luasim/fxhost, which compiles exactly three
  // files of this directory and must keep matching luasim pixel for pixel.
  presenceLuaOpen(L);
#endif
  return 0;
}

// The chunk body, and the globals it left behind. Reading a global can reach an
// __index a script put on _G, so this is protected too - but it is shallow, and
// it is no longer the deepest thing on the stack.
int bodyProtected(lua_State *L) {
  OpenCtx *c = static_cast<OpenCtx *>(lua_touserdata(L, 1));
  lua_pushvalue(L, 2);                 // the loaded chunk
  lua_call(L, 0, 0);
  if (lua_getglobal(L, "draw") != LUA_TFUNCTION) return luaL_error(L, "the script defines no draw()");
  lua_pop(L, 1);
  if (lua_getglobal(L, "PERIOD") == LUA_TNUMBER && lua_tonumber(L, -1) > 0) c->period = lua_tonumber(L, -1);
  lua_pop(L, 1);
  if (lua_getglobal(L, "FPS") == LUA_TNUMBER) c->fps = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return 0;
}

int drawProtected(lua_State *L) {
  if (lua_getglobal(L, "draw") != LUA_TFUNCTION) return luaL_error(L, "draw is no longer a function");
  lua_call(L, 0, 0);
  return 0;
}
}  // namespace

bool LuaFx::open(const char *name, const char *src, size_t len, LuaPxCanvas *canvas,
                 const LuaFxLimits &limits) {
  close();
  err_[0] = 0;
  limits_ = limits;
  heap_ = heapPeak_ = 0;
  used_ = 0;
  period_ = 60.0;
  fps_ = LUA_FX_DEFAULT_FPS;

  lua_State *L = lua_newstate(alloc, this);
  if (!L) {
    strncpy(err_, "no memory for a Lua state", sizeof(err_) - 1);
    return false;
  }
  *static_cast<LuaFx **>(lua_getextraspace(L)) = this;
  L_ = L;

  char chunk[40];
  snprintf(chunk, sizeof(chunk), "=%s", name);
  OpenCtx ctx = {src, len, chunk, canvas, 60.0, (lua_Number)LUA_FX_DEFAULT_FPS};
  arm(limits.loadInstructions, limits.loadMs);

  lua_pushcfunction(L, nslua_message_handler);
  lua_pushcfunction(L, libsProtected);
  lua_pushlightuserdata(L, &ctx);
  int status = lua_pcall(L, 1, 0, 1);
  if (status != LUA_OK) {
    lua_sethook(L, nullptr, 0, 0);
    takeError(status);
    close();
    return false;
  }
  lua_settop(L, 0);

  // The parse is the deepest C recursion this runtime ever performs - Lua's
  // parser descends with the source's nesting - and it is deliberately NOT
  // wrapped in a pcall of ours. Two things follow, and on a 12 KB task stack
  // both of them matter:
  //
  //   * the frames a pcall puts above it (lua_pcallk, luaD_pcall,
  //     luaD_rawrunprotected, callnoyield, ccall, precall, precallC and the
  //     protected function itself) are not there, which is over 400 bytes of
  //     prologue the parser does not have to pay for;
  //   * with no errfunc set, a syntax error throws straight out instead of
  //     running nslua_message_handler, and that handler calls luaL_traceback,
  //     whose own frame is 448 bytes - spent at maximum depth, because Lua
  //     calls the handler before it unwinds (ldebug.c, luaG_errormsg).
  //
  // Nothing is lost by it: lua_load protects itself through luaD_protectedparser,
  // it returns a status rather than raising, and a traceback of a syntax error
  // has no call stack to show anyway.
  //
  // Mode "t": source text only. The bytecode loader is not safe against
  // hostile input (see nslua.cpp).
  status = luaL_loadbufferx(L, ctx.src, ctx.len, ctx.chunk, "t");
  if (status != LUA_OK) {
    lua_sethook(L, nullptr, 0, 0);
    takeError(status);
    close();
    return false;
  }

  lua_pushcfunction(L, nslua_message_handler);   // 2
  lua_pushcfunction(L, bodyProtected);           // 3
  lua_pushlightuserdata(L, &ctx);                // 4
  lua_pushvalue(L, 1);                           // 5: the chunk
  status = lua_pcall(L, 2, 0, 2);
  lua_sethook(L, nullptr, 0, 0);
  if (status != LUA_OK) {
    takeError(status);
    close();
    return false;
  }
  lua_settop(L, 0);
  period_ = ctx.period;
  fps_ = ctx.fps < 1 ? 1 : ctx.fps > 30 ? 30 : (uint8_t)ctx.fps;
  if (generationalGc_) lua_gc(L, LUA_GCGEN, 0, 0);   // two zeros: 5.4 reads both
  return true;
}

bool LuaFx::draw() {
  if (!L_) return false;
  lua_settop(L_, 0);
  arm(limits_.drawInstructions, limits_.drawMs);
  lua_pushcfunction(L_, nslua_message_handler);
  lua_pushcfunction(L_, drawProtected);
  const int status = lua_pcall(L_, 0, 0, 1);
  lua_sethook(L_, nullptr, 0, 0);
  if (status != LUA_OK) {
    takeError(status);
    lua_settop(L_, 0);
    return false;
  }
  lua_settop(L_, 0);
  return true;
}

void LuaFx::close() {
  if (!L_) return;
  lua_close(L_);   // frees every block through alloc(); heap_ returns to 0
  L_ = nullptr;
}

#endif  // LUA_EFFECTS_ENABLED || host

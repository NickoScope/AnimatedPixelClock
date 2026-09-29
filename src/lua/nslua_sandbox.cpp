// ============================================================
// nslua_sandbox.cpp - the whitelist, shared by nslua.cpp and lua_fx.cpp
// ============================================================
// The code is the sandbox nslua.cpp has carried since v33.32.0, moved here
// as it was. See nslua_sandbox.h.
// ============================================================
#include "nslua_sandbox.h"

#include <stdio.h>
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
#include "vendor/lua/lualib.h"
}

static void consoleLine(const char *s) {
#if defined(ARDUINO)
    Serial.printf("[lua] %s\n", s);
#else
    fprintf(stderr, "[lua] %s\n", s);
#endif
}

// print -> console, arguments separated by tabs, one line of at most 159 chars.
static int sandboxPrint(lua_State *L) {
    const int n = lua_gettop(L);
    char line[160];
    int pos = 0;
    for (int i = 1; i <= n && pos < (int)sizeof(line) - 2; i++) {
        size_t l = 0;
        const char *s = luaL_tolstring(L, i, &l);  // honours __tostring
        if (i > 1 && pos < (int)sizeof(line) - 2) line[pos++] = '\t';
        while (l-- && pos < (int)sizeof(line) - 2) line[pos++] = *s++;
        lua_pop(L, 1);
    }
    line[pos] = 0;
    consoleLine(line);
    return 0;
}

// log(value) -> console, any type converted to a string.
static int sandboxLog(lua_State *L) {
    size_t l = 0;
    const char *s = luaL_tolstring(L, 1, &l);
    consoleLine(s ? s : "(nil)");
    lua_pop(L, 1);
    return 0;
}

// require: only host preloads (the LOADED registry table); there are no files.
static int sandboxRequire(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    return luaL_error(L, "module '%s' is not available in the nslua sandbox", name);
}

extern "C" int nslua_message_handler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") != 0 &&
            lua_type(L, -1) == LUA_TSTRING) {
            return 1;
        }
        msg = lua_pushfstring(L, "(error object is a %s value)",
                              luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// A library function that calls back into Lua, run with a nesting count
// shared by all such functions (upvalue 2, a full userdata int).
static const int kCallbackDepth = 3;
static int guardedCall(lua_State *L) {
    int *depth = static_cast<int *>(lua_touserdata(L, lua_upvalueindex(2)));
    if (*depth >= kCallbackDepth)
        return luaL_error(L, "gsub, format, sort, table.concat/unpack/move/insert/remove, tostring, print, log, math.max/min nest at most %d deep",
                          kCallbackDepth);
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    (*depth)++;
    const int st = lua_pcall(L, n, LUA_MULTRET, 0);
    (*depth)--;
    if (st != LUA_OK) return lua_error(L);
    return lua_gettop(L);
}

// setmetatable without __gc. Lua 5.4 runs a finalizer with hooks off
// (GCTM sets allowhook = 0), so the instruction budget and the frame's
// deadline cannot stop one: `__gc = function() while true do end end` would
// hold the effect task until the task watchdog restarts the panel. An object
// is only marked for finalization when __gc is in its metatable at
// setmetatable time (luaC_checkfinalizer), so refusing it there is enough.
static int sandboxSetmetatable(lua_State *L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) return luaL_error(L, "setmetatable: __gc is not allowed in an effect");
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static void guardCallbacks(lua_State *L) {
    int *depth = static_cast<int *>(lua_newuserdatauv(L, sizeof(int), 0));
    *depth = 0;
    const int d = lua_gettop(L);
    // The second gate audit's list (2026-09-29, from -fstack-usage): gsub,
    // format and sort, and table.concat through __index (720 B a level, 13 KB
    // at 17), print through __tostring (12.7 KB with the error path), and
    // table.unpack and table.move through __index (at the edge). The third
    // (same day) added table.insert and table.remove through __newindex/__index
    // (416 B a level), tostring (384) and log (400): with them left out, 4
    // wrapped levels plus 9 of those plus the deepest leaf came to 12.3-12.5 KB.
    // Wrapped, and 3 deep, what is left unwrapped is plain metamethods at
    // ~256 B a level. A pcall around a table.insert is a few microseconds; no
    // shipped script calls one more than a handful of times a frame. The
    // fourth audit added math.max and math.min through __lt (448 B a level).
    static const char *const kWrap[][2] = {{LUA_STRLIBNAME, "gsub"}, {LUA_STRLIBNAME, "format"},
                                           {LUA_TABLIBNAME, "sort"}, {LUA_TABLIBNAME, "concat"},
                                           {LUA_TABLIBNAME, "unpack"}, {LUA_TABLIBNAME, "move"},
                                           {LUA_TABLIBNAME, "insert"}, {LUA_TABLIBNAME, "remove"},
                                           {LUA_GNAME, "tostring"}, {LUA_GNAME, "print"},
                                           {LUA_GNAME, "log"}, {LUA_MATHLIBNAME, "max"},
                                           {LUA_MATHLIBNAME, "min"}};
    for (size_t i = 0; i < sizeof(kWrap) / sizeof(kWrap[0]); i++) {
        if (strcmp(kWrap[i][0], LUA_GNAME) == 0) lua_pushglobaltable(L);
        else lua_getglobal(L, kWrap[i][0]);
        lua_getfield(L, -1, kWrap[i][1]);          // the original
        lua_pushvalue(L, d);                       // the shared count
        lua_pushcclosure(L, guardedCall, 2);
        lua_setfield(L, -2, kWrap[i][1]);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                                 // the count: the closures hold it
}

extern "C" void nslua_sandbox_open(lua_State *L) {
    // Whitelist: base, table, string, math only. io, os, debug and package
    // are not merely left closed - their sources are not in the build.
    // coroutine is compiled in (lcorolib.c) but deliberately not opened: a new
    // thread starts with a fresh hook count, so it would need the per-thread
    // charge the Watch added before it could be exposed.
    static const luaL_Reg libraries[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {NULL, NULL},
    };
    for (const luaL_Reg *lib = libraries; lib->func != NULL; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    // dofile/loadfile reach a filesystem; load accepts unverifiable bytecode;
    // collectgarbage hands a script control of GC pauses. setmetatable, rawset
    // and friends stay: tables need them and they are not an escape.
    //
    // pcall and xpcall go for a different reason, and it is about the stack
    // rather than the sandbox. Each nested pcall costs a whole C cycle -
    // luaD_precall 48 + precallC 32 + luaB_pcall 32 + lua_pcallk 64 +
    // luaD_pcall 48 + luaD_rawrunprotected 128 + f_call 32 +
    // luaD_callnoyield 32 + ccall 32 + luaV_execute 112 = 560 bytes a level,
    // measured from this build - and `local function f() pcall(f) end` reaches
    // LUAI_MAXCCALLS with three lines of source that no amount of scanning the
    // text can recognise as deep. On a 12 KB task stack that peaks around
    // 11.6 KB, inside the margin kept for interrupts.
    //
    // Nothing shipped uses them: an effect is a draw loop, its errors are
    // caught by the C-level lua_pcall around draw(), and a script swallowing
    // its own would only hide them from the budget machinery.
    static const char *const kRemoved[] = {"dofile", "loadfile", "load",
                                           "collectgarbage", "pcall", "xpcall"};
    for (size_t i = 0; i < sizeof(kRemoved) / sizeof(kRemoved[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, kRemoved[i]);
    }

    // string.gsub, string.format and table.sort call back into Lua (a
    // replacement function, a __tostring, a comparator) from C frames that
    // are large: str_gsub is 656 bytes on this build, a luaL_Buffer on the
    // stack, and a gsub whose replacement calls gsub again is ~944 bytes a
    // level. LUAI_MAXCCALLS (20) lets 18 of those through, 17 KB against the
    // task's 12 KB (the gate audit of 2026-09-29, from -fstack-usage). So the
    // these are wrapped with one shared count and may nest 4 deep; the
    // wrapper's own lua_pcall is there only to bring the count back down when
    // an error passes through, and the error goes on as it came.
    lua_pushcfunction(L, sandboxPrint);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, sandboxLog);
    lua_setglobal(L, "log");
    lua_pushcfunction(L, sandboxRequire);
    lua_setglobal(L, "require");

    guardCallbacks(L);          // after print, which it wraps too

    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, sandboxSetmetatable, 1);
    lua_setglobal(L, "setmetatable");
}

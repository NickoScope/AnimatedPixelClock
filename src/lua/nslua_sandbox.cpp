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
    static const char *const kRemoved[] = {"dofile", "loadfile", "load",
                                           "collectgarbage"};
    for (size_t i = 0; i < sizeof(kRemoved) / sizeof(kRemoved[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, kRemoved[i]);
    }

    lua_pushcfunction(L, sandboxPrint);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, sandboxLog);
    lua_setglobal(L, "log");
    lua_pushcfunction(L, sandboxRequire);
    lua_setglobal(L, "require");
}

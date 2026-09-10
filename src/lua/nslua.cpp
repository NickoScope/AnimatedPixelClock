// ============================================================
// nslua.cpp — sandboxed Lua 5.4.8 runtime for ESP32-S3 (NickoScope32)
// ============================================================
// Build: 2026-07-30 (Cannes) · v33.32.0 nslua interpreter pilot
//
// Порт песочницы BeamLua (H743) на S3. Смотри nslua.h.
//
// Аллокатор — PSRAM через ESP-IDF heap_caps API. Сверено по докам
// (arduino-esp32 esp32-hal-psram.c: ps_realloc → heap_caps_realloc
// (ptr, size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); ESP-IDF external-ram
// guide: свободить можно обычным free()/heap_caps_free()). PSRAM
// инициализируется автоматически стартапом Arduino при -DBOARD_HAS_PSRAM.
// ============================================================

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
#include "vendor/lua/lualib.h"
}

#include "nslua.h"
#include "nslua_bindings.h"   // P&P-реестр биндинг-групп (sys/time/echo/...)

// ============================================================
// PSRAM аллокатор (lua_Alloc, семантика realloc — Lua RM §lua_Alloc)
// ============================================================
static const uint32_t NSLUA_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

static void *nslua_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        if (ptr) heap_caps_free(ptr);
        return NULL;
    }
    // heap_caps_realloc(NULL, ...) ведёт себя как malloc.
    return heap_caps_realloc(ptr, nsize, NSLUA_CAPS);
}

// ============================================================
// Instruction budget (порт LuaBinding.cpp / lua_engine.cpp instructionHook)
// ============================================================
// ИНВАРИАНТ: бюджет — file-static (единый на прогон, не per-lua_State).
// Корректно, т.к. nslua_run — SINGLE-FLIGHT: только из loop()/главной
// задачи, НИКОГДА из ISR и без реентранси (свежий state, закрывается тут же).
// Если появятся два одновременных стейта — перенести в lua_State extraspace.
static uint32_t s_instr_remaining = 0;
static int      s_instr_step = 0;

static int nextHookStep(uint32_t remaining) {
    const uint32_t kMax = 500000u;
    return (int)(remaining > kMax ? kMax : remaining);
}

static void instructionHook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    const uint32_t consumed = (uint32_t)s_instr_step;
    s_instr_remaining = (s_instr_remaining > consumed)
                            ? s_instr_remaining - consumed : 0;
    if (s_instr_remaining == 0) {
        luaL_error(L, "script exceeded the nslua instruction budget");
        return;  // не достигается
    }
    s_instr_step = nextHookStep(s_instr_remaining);
    lua_sethook(L, instructionHook, LUA_MASKCOUNT, s_instr_step);
}

static void resetInstructionBudget(lua_State *L) {
    s_instr_remaining = NSLUA_INSTR_BUDGET;
    s_instr_step = nextHookStep(s_instr_remaining);
    lua_sethook(L, instructionHook, LUA_MASKCOUNT, s_instr_step);
}

// ============================================================
// Песочница
// ============================================================

// print → Serial (lua_writestring из vendor пишет в stdout newlib;
// на S3 это USB-CDC, но заворачиваем сами для единообразия с log()).
static int sandboxPrint(lua_State *L) {
    const int n = lua_gettop(L);
    char line[160];
    int pos = 0;
    for (int i = 1; i <= n && pos < (int)sizeof(line) - 2; i++) {
        size_t l = 0;
        const char *s = luaL_tolstring(L, i, &l);  // использует __tostring
        if (i > 1 && pos < (int)sizeof(line) - 2) line[pos++] = '\t';
        while (l-- && pos < (int)sizeof(line) - 2) line[pos++] = *s++;
        lua_pop(L, 1);
    }
    line[pos] = 0;
    Serial.printf("[lua] %s\n", line);
    return 0;
}

// log(str) — явный биндинг в Serial (первое доказательство биндинга).
static int sandboxLog(lua_State *L) {
    size_t l = 0;
    const char *s = luaL_tolstring(L, 1, &l);   // любой тип → строка
    Serial.printf("[lua] %s\n", s ? s : "(nil)");
    lua_pop(L, 1);
    return 0;
}

// require — только хост-прелоады (реестр LOADED), файлов нет by construction.
static int sandboxRequire(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    return luaL_error(L, "module '%s' is not available in the nslua sandbox", name);
}

// message handler: строка + traceback (как в lua_engine.cpp).
static int messageHandler(lua_State *L) {
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

// Setup песочницы: библиотеки, чёрный список, print/log/require, hook.
// Вызывается под pcall — при OOM бросает наверх строкой.
static void setupSandbox(lua_State *L) {
    // Белый список: ТОЛЬКО base, table, string, math. io/os/debug/
    // coroutine/package не только не открыты — их .c нет в сборке.
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

    // dofile/loadfile — файловая система; load — непроверяемый байткод;
    // collectgarbage — контроль GC (паузы/тайминг), policy-скрипту не место.
    // (setmetatable/rawset/… оставлены: нужны для таблиц, escape'ом не являются —
    //  io/os/load физически отсутствуют в сборке. Аудит nslua v33.32.0.)
    static const char *const kRemoved[] = {"dofile", "loadfile", "load",
                                           "collectgarbage"};
    for (size_t i = 0; i < sizeof(kRemoved) / sizeof(kRemoved[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, kRemoved[i]);
    }

    // print → Serial
    lua_pushcfunction(L, sandboxPrint);
    lua_setglobal(L, "print");

    // log(str) → Serial
    lua_pushcfunction(L, sandboxLog);
    lua_setglobal(L, "log");

    // require-заглушка (только хост-прелоады, которых на S3 пока нет)
    lua_pushcfunction(L, sandboxRequire);
    lua_setglobal(L, "require");

    // P&P биндинг-группы (sys/time/echo/...) — саморегистрируются через
    // NSLUA_BIND_REGISTER, открываются здесь в _G. Новая группа = 1 макрос
    // в своём .cpp, без правки этого файла.
    nslua_open_all_bindings(L);

    // Бюджет инструкций
    resetInstructionBudget(L);
}

struct RunCtx {
    const char *src;
};

// Выполняется под lua_pcall: sandbox → load → run.
static int runSandboxed(lua_State *L) {
    RunCtx *ctx = static_cast<RunCtx *>(lua_touserdata(L, 1));
    setupSandbox(L);
    // mode="t" — ТОЛЬКО текст. luaL_loadbuffer() = loadbufferx(mode=NULL),
    // а NULL пропускает и БИНАРНЫЙ чанк (ldo.c checkmode/LUA_SIGNATURE), при
    // том что lundump.c в сборке. Загрузчик байткода Lua не защищён от
    // враждебного ввода — это обход песочницы в обход всего белого списка,
    // как только src придёт из Telegram/HTTP. Аудит nslua v33.32.0.
    if (luaL_loadbufferx(L, ctx->src, strlen(ctx->src), "nslua", "t") != LUA_OK) {
        return lua_error(L);
    }
    lua_call(L, 0, 0);   // результат скрипта игнорируем (чистый рантайм)
    return 0;
}

// ============================================================
// Публичный API
// ============================================================
bool nslua_begin(void) {
    if (!psramFound()) {
        Serial.println("[nslua] PSRAM not found — Lua runtime disabled");
        return false;
    }
    Serial.println("[nslua] ready (Lua 5.4.8, PSRAM allocator)");
    nslua_bindings_dump();   // P&P: перечислить саморегистрированные группы
    return true;
}

bool nslua_run(const char *src, char *err, size_t errlen) {
    if (err && errlen > 0) err[0] = 0;
    if (src == NULL) {
        if (err && errlen > 0) { strncpy(err, "null source", errlen - 1); err[errlen - 1] = 0; }
        return false;
    }

    lua_State *L = lua_newstate(nslua_lua_alloc, NULL);
    if (L == NULL) {
        if (err && errlen > 0) { strncpy(err, "cannot create Lua state (PSRAM?)", errlen - 1); err[errlen - 1] = 0; }
        return false;
    }

    RunCtx ctx = { src };
    lua_pushcfunction(L, messageHandler);       // errfunc @1
    lua_pushcfunction(L, runSandboxed);
    lua_pushlightuserdata(L, &ctx);
    const int status = lua_pcall(L, 1, 0, 1);   // 1 arg (ctx), errfunc @1

    bool ok = (status == LUA_OK);
    if (!ok && err && errlen > 0) {
        const char *m = lua_tostring(L, -1);
        strncpy(err, m ? m : "unknown Lua error", errlen - 1);
        err[errlen - 1] = 0;
    }

    lua_close(L);
    return ok;
}

// ============================================================
// nslua_bind_sys.cpp — первая P&P-группа биндингов "sys" (S3)
// ============================================================
// Build: 2026-07-30 (Cannes) · v33.32.0
//
// Эталон плагина-биндинга: тривиальные host-info функции, доказывающие,
// что реестр NSLUA_BIND_REGISTER срабатывает end-to-end (группа видна
// в песочнице как глобал `sys`). Будущие группы (time/echo/loud) —
// ровно по этому шаблону, БЕЗ правки nslua.cpp/центрального файла.
//
//   sys.millis()    → uptime, мс (uint32; под LUA_32BITS завернётся ~24.8 сут — ок для диагностики)
//   sys.heap_free() → свободно в PSRAM, байт (там живёт Lua-рантайм)
// ============================================================
#include "nslua_bindings.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
}

static int sys_millis(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

static int sys_heap_free(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 1;
}

static const luaL_Reg sys_funcs[] = {
    {"millis",    sys_millis},
    {"heap_free", sys_heap_free},
    {NULL, NULL},
};

// luaopen_-стиль: строит модуль-таблицу, оставляет её на стеке.
static int luaopen_nslua_sys(lua_State *L) {
    luaL_newlib(L, sys_funcs);
    return 1;
}

// P&P: одна строка — и группа `sys` подхватывается песочницей автоматически.
NSLUA_BIND_REGISTER({ "sys", luaopen_nslua_sys });

// ============================================================
// nslua_bindings.cpp — реестр P&P-биндингов (static-init, S3)
// ============================================================
// Build: 2026-07-30 (Cannes) · v33.32.0
//
// Реализация 1:1 с nsp_handlers_registry.cpp: группы регистрируют
// себя на C++ static-init (до setup()) через Registrar из
// NSLUA_BIND_REGISTER; nslua_open_all_bindings() открывает их в каждый
// lua_State.
//
// Память: реестр = NSLUA_BINDINGS_MAX × 4 Б = 64 Б RAM; дескрипторы —
// в .rodata FLASH. Регистрация O(1); открытие O(N), N≤~8.
// ============================================================
#include "nslua_bindings.h"
#include <Arduino.h>

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
}

#define NSLUA_BINDINGS_MAX  16

static const nslua_binding_module_t *s_reg[NSLUA_BINDINGS_MAX];
static unsigned                      s_count = 0;

// Зовётся Registrar'ом на static-init. Порядок между TU не определён,
// но это неважно — группы независимы, открываются по имени.
extern "C" void _nslua_bind_register_internal(const nslua_binding_module_t *m) {
    if (!m || !m->name || !m->openf) return;
    if (s_count >= NSLUA_BINDINGS_MAX) return;  // silent drop (Serial ещё не готов на static-init)
    s_reg[s_count++] = m;
}

extern "C" unsigned nslua_bindings_count(void) { return s_count; }

extern "C" void nslua_open_all_bindings(lua_State *L) {
    for (unsigned i = 0; i < s_count; i++) {
        // luaL_requiref: строит таблицу через openf, кладёт в _G[name] и
        // package.loaded, дедупит; glb=1 → видна как глобал в песочнице.
        luaL_requiref(L, s_reg[i]->name,
                      reinterpret_cast<lua_CFunction>(s_reg[i]->openf), 1);
        lua_pop(L, 1);  // снять таблицу, оставленную requiref
    }
}

// Диагностика boot: доказать, что Registrar сработал для каждого
// NSLUA_BIND_REGISTER во всех TU (как nsp_handlers_dump()).
extern "C" void nslua_bindings_dump(void) {
    Serial.printf("[nslua] %u binding group(s) registered:\n", s_count);
    for (unsigned i = 0; i < s_count; i++) {
        Serial.printf("  [%u] %s\n", i, s_reg[i]->name ? s_reg[i]->name : "(none)");
    }
}

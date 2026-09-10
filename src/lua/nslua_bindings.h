// ============================================================
// nslua_bindings.h — Plug-and-Play binding registry for nslua (ESP32-S3)
// ============================================================
// Build: 2026-07-30 (Cannes) · v33.32.0
//
// Зеркалит проектный P&P-паттерн NSP_HANDLER_REGISTER (см.
// nsp_handler_module.h): каждый набор биндингов саморегистрируется
// на C++ static-init (до setup()), а setupSandbox() открывает ВСЕ
// зарегистрированные группы в песочницу. Новая группа биндингов =
// один NSLUA_BIND_REGISTER в СВОЁМ .cpp — без правки центрального файла.
//
// Почему static-init, а не linker-section: на ESP32-S3/Xtensa ESP-IDF
// linker-фрагменты в PlatformIO не правятся удобно, а секция без KEEP()
// выпадает под --gc-sections. Static-конструкторы универсальны и
// детерминированы (см. nsp_handlers_registry.cpp — та же причина).
//
// Группа биндингов = именованная модуль-таблица. openf следует
// соглашению luaopen_* (luaL_newlib(L, funcs); return 1;). Открывается
// через luaL_requiref(name, openf, glb=1) → таблица видна как глобал
// `name` в песочнице (+ package.loaded), с дедупом.
// ============================================================
#ifndef NSLUA_BINDINGS_H
#define NSLUA_BINDINGS_H

struct lua_State;   // fwd — заголовок не тянет vendor/lua

// openf — сигнатура lua_CFunction (int f(lua_State*)); строит модуль-таблицу.
typedef int (*nslua_open_fn)(struct lua_State *L);

struct nslua_binding_module_s {
    const char    *name;    // имя глобальной таблицы: "sys", "time", "echo"...
    nslua_open_fn  openf;   // luaopen_-стиль: luaL_newlib(L, funcs); return 1;
};
typedef struct nslua_binding_module_s nslua_binding_module_t;

#ifdef __cplusplus
extern "C" {
#endif

// Открыть ВСЕ зарегистрированные группы в L (luaL_requiref name, openf, 1).
// Звать один раз на lua_State, ПОСЛЕ открытия base/table/string/math.
void     nslua_open_all_bindings(struct lua_State *L);
unsigned nslua_bindings_count(void);
void     nslua_bindings_dump(void);   // Serial-список групп (диагностика boot)

// Внутреннее: зовётся Registrar'ом из NSLUA_BIND_REGISTER на static-init.
void     _nslua_bind_register_internal(const nslua_binding_module_t *m);

#ifdef __cplusplus
}
#endif

// ============================================================
// NSLUA_BIND_REGISTER — static-init Registrar (как NSP_HANDLER_REGISTER):
//   - static const дескриптор в .rodata;
//   - wrapper-конструктор пушит себя в реестр до setup();
//   - __attribute__((used)) — не даёт выкинуть под gc-sections;
//   - __LINE__-паста → несколько регистраций в одном .cpp.
// Только для C++ TU (все наши биндинги — .cpp).
// ============================================================
#ifdef __cplusplus

#define NSLUA_BIND_PASTE_RAW(a, b)  a ## b
#define NSLUA_BIND_PASTE_EXP(a, b)  NSLUA_BIND_PASTE_RAW(a, b)
#define NSLUA_BIND_UNIQUE(prefix)   NSLUA_BIND_PASTE_EXP(prefix, __LINE__)

namespace _nslua_bind_internal {
  struct Registrar {
    Registrar(const nslua_binding_module_t *m) {
      _nslua_bind_register_internal(m);
    }
  };
}

#define NSLUA_BIND_REGISTER(...)                                               \
    static const nslua_binding_module_t NSLUA_BIND_UNIQUE(nslua_b_) = __VA_ARGS__; \
    static const _nslua_bind_internal::Registrar                              \
        NSLUA_BIND_UNIQUE(nslua_b_reg_)                                       \
        __attribute__((used)) (& NSLUA_BIND_UNIQUE(nslua_b_))

#endif  // __cplusplus

#endif  // NSLUA_BINDINGS_H

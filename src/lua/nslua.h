// ============================================================
// nslua.h — sandboxed Lua 5.4.8 runtime for ESP32-S3 (NickoScope32)
// ============================================================
// Build: 2026-07-30 (Cannes) · v33.32.0 nslua interpreter pilot
//
// Порт песочницы BeamLua (H743 src/beamlua/lua_engine.cpp) на S3.
// Отличия от H743:
//   * аллокатор — PSRAM (heap_caps_realloc MALLOC_CAP_SPIRAM|8BIT),
//     а не статический boundary-tag пул в .bss;
//   * print / log → Serial.printf (на H743 было slog);
//   * НЕТ beam/prelude/scene-return — это render-путь H743, на S3
//     нужен только чистый рантайм (первое доказательство биндинга);
//   * coroutine НЕ открыт В ЭТОМ (Phase-1, boot self-test) пути — сознательно
//     минимальная поверхность. Сама библиотека в сборке ЕСТЬ с v0.32.0
//     (vendor/lua/lcorolib.c) и открыта в persistent-песочнице
//     nslua_state.cpp вместе с платой за создание потока.
//
// Каждый прогон = свежий lua_State: sandbox → load → run под pcall.
// Ошибки (синтаксис, рантайм, OOM, бюджет инструкций) возвращаются
// строкой, никогда не крэшат прошивку. ВЫЗЫВАТЬ ТОЛЬКО ИЗ loop()/
// главной задачи, НИКОГДА из ISR.
// ============================================================

#ifndef NSLUA_H
#define NSLUA_H

#include <stddef.h>
#include <stdbool.h>

// Бюджет VM-инструкций на один прогон скрипта (защита от вечного цикла).
#define NSLUA_INSTR_BUDGET   2000000u

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация рантайма. Проверяет наличие PSRAM (psramFound()).
// Возврат true, если PSRAM доступна и рантайм готов.
bool nslua_begin(void);

// Прогнать скрипт: свежий lua_State (PSRAM-аллокатор) → sandbox →
// load+execute под lua_pcall. При ошибке — текст в err (errlen байт),
// возврат false. При успехе возврат true, err[0]=0.
bool nslua_run(const char *src, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif // NSLUA_H

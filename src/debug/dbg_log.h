#pragma once
// The panel's log, readable over the network, off by default.
//
// The owner's words, 2026-09-20: "полное управление и логирование по сети не
// только по кабелю, чтобы можно было проводить полный дебагинг, просто сделай в
// вебе включение и выключение, чтобы зря в боевом режиме ресурсы не жрать и не
// нужно будет тыкать кабель каждый раз."
//
// So: **off costs nothing.** No buffer is allocated, no hook is installed, and
// dbgLogf() falls through to Serial exactly as a bare Serial.printf did. Turned
// on, it takes DBG_LOG_BYTES of **PSRAM** - never internal RAM, which is the
// heap this panel actually runs short of - and keeps the newest lines there for
// a reader to fetch with a cursor (dbg_ring.h).
//
// What is captured while it is on:
//   - every dbgLogf() line, which is where the panel's own diagnostics go;
//   - what the IDF logs through ESP_LOGx, such as the ping socket's failures.
//
// **What it does NOT capture, said plainly.** Arduino's log_e/log_w/log_i do
// not go through esp_log_write in this build: with neither CORE_DEBUG_LEVEL nor
// USE_ESP_IDF_LOG defined in platformio.ini, esp32-hal-log.h expands them to
// log_printf() -> ets_printf(), which the vprintf hook never sees. So a line
// like "WebServer.cpp:638 request handler not found" reaches the cable and not
// the network. Capturing those means building the whole firmware with
// -DUSE_ESP_IDF_LOG, which changes every log line's format, and that is a
// decision for its own change, not a side effect of this one.
// Serial keeps getting all of it either way, so a cable still works.
//
// The switch is remembered in NVS, so a panel left with the log on comes back
// with it on - deliberate: an intermittent fault is worth catching across the
// reboot it causes.

#include <stdint.h>
#include <stddef.h>

// 32 KB of PSRAM: about four hundred of the panel's log lines, which covers the
// bursts that matter (2026-09-20: twenty-five allocation failures in eleven
// seconds, plus the loop and effect lines around them).
#define DBG_LOG_BYTES 32768
static_assert((DBG_LOG_BYTES & (DBG_LOG_BYTES - 1)) == 0, "the ring's capacity must be a power of two: dbg_ring.h indexes by the absolute sequence");

// Most a single read returns, so /api/log is never one of the big responses the
// portal holds back when the radio is starving - the log has to stay readable
// exactly then.
// Kept small on purpose. A read is copied inside the writer's spinlock, and
// that is time with interrupts off on one core while the other spins if it
// logs; 1 KB of memcpy out of PSRAM is tens of microseconds, 4 KB was not.
#define DBG_LOG_READ_MAX 1024

void dbgLogBegin();                      // setup(): restores the switch from NVS
bool dbgLogEnabled();
bool dbgLogSetEnabled(bool on);          // allocates or frees; returns the state it reached
void dbgLogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// For callers that have already formatted the line into their own buffer -
// loopMark() does, deliberately, because Print::printf would malloc for a line
// over 64 bytes and it runs exactly when memory is short.
void dbgLogWrite(const char *p, uint32_t n);

// Bytes after `since`, at most `max` and never more than DBG_LOG_READ_MAX.
// `*from` comes back as the sequence number of the first byte actually returned:
// equal to `since` when nothing was missed, larger when the reader fell behind.
uint32_t dbgLogRead(uint32_t since, char *out, uint32_t max, uint32_t *from);

// Where a reader stages its copy: DBG_LOG_READ_MAX bytes of **PSRAM**, carved
// from the same block as the ring, so the web handler needs no internal RAM of
// its own. Null while the log is off. Handlers run on the loop task only, so
// one shared area is enough.
char *dbgLogReadBuf();

// Empties the ring without touching the switch, the buffer or NVS.
void dbgLogClear();

uint32_t dbgLogSeq();        // bytes ever written since the log was turned on
uint32_t dbgLogKept();       // bytes currently held
uint32_t dbgLogDropped();    // bytes thrown away to make room

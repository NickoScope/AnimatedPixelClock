// The remote log. See dbg_log.h for what it is and why it is off by default.
#include "dbg_log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdarg.h>

#include "dbg_ring.h"

namespace {

const char *const kNs = "dbg";
const char *const kKey = "on";

DbgRing g_ring = {nullptr, 0, 0, 0, 0};
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_prevVprintf = nullptr;
bool g_on = false;

// The ring is written from whatever task logged - the Wi-Fi task and the loop
// both do - so every touch of it is inside the spinlock. The sections are one
// memcpy of at most a couple of hundred bytes.
void ringWrite(const char *p, uint32_t n) {
  portENTER_CRITICAL(&g_mux);
  dbgRingWrite(&g_ring, p, n);
  portEXIT_CRITICAL(&g_mux);
}

// Everything the framework logs (log_e, log_w, ...) passes through here while
// the log is on, and is forwarded to whatever printed it before, so the cable
// still shows it.
int dbgVprintf(const char *fmt, va_list ap) {
  // This lands on the stack of whatever task logged - tiT and mdns among them,
  // and their stacks are not generous. 128 B is enough for the lines that
  // matter and half the footprint.
  char line[128];
  va_list copy;
  va_copy(copy, ap);
  const int n = vsnprintf(line, sizeof line, fmt, copy);
  va_end(copy);
  if (n > 0) {
    uint32_t len = (uint32_t)(n < (int)sizeof line ? n : (int)sizeof line - 1);
    // The cable gets the whole line below; the ring gets what fits. Say so,
    // rather than let the two show different text for the same line.
    if (n >= (int)sizeof line) __builtin_memcpy(line + sizeof line - 5, "...\n", 4);
    ringWrite(line, len);
  }
  vprintf_like_t prev = g_prevVprintf;   // one read: the off path clears it
  return prev ? prev(fmt, ap) : 0;
}

}  // namespace

bool dbgLogEnabled() { return g_on; }
uint32_t dbgLogSeq() { return g_ring.seq; }
uint32_t dbgLogKept() { return g_ring.kept; }
uint32_t dbgLogDropped() { return g_ring.dropped; }

bool dbgLogSetEnabled(bool on) {
  if (on == g_on) return g_on;
  if (on) {
    // PSRAM only. Internal RAM is what the radio and the web server run short
    // of (drafts/28-portal-hang-2026-09-20.md); a debug log must not compete
    // for it, or turning the log on would change what it is there to observe.
    // One block: the ring, then the staging area a reader copies into. Both in
    // PSRAM, so turning the log on costs no internal RAM at all.
    char *buf = (char *)heap_caps_malloc(DBG_LOG_BYTES + DBG_LOG_READ_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
      Serial.println("[dbg] no PSRAM for the log: it stays off");
      return false;
    }
    portENTER_CRITICAL(&g_mux);
    dbgRingInit(&g_ring, buf, DBG_LOG_BYTES);
    portEXIT_CRITICAL(&g_mux);
    // The previous hook is in place before ours can run, so no line is lost in
    // the window between installing and remembering it.
    g_prevVprintf = esp_log_set_vprintf(dbgVprintf);
    if (!g_prevVprintf) g_prevVprintf = vprintf;
    g_on = true;
    dbgLogf("[dbg] log on: %u B in PSRAM\n", (unsigned)DBG_LOG_BYTES);
  } else {
    if (g_prevVprintf) esp_log_set_vprintf(g_prevVprintf);
    g_prevVprintf = nullptr;
    g_on = false;
    portENTER_CRITICAL(&g_mux);
    char *buf = g_ring.buf;
    dbgRingInit(&g_ring, nullptr, 0);
    portEXIT_CRITICAL(&g_mux);
    heap_caps_free(buf);
    Serial.println("[dbg] log off, buffer freed");
  }
  Preferences p;
  if (p.begin(kNs, false)) {
    p.putUChar(kKey, g_on ? 1 : 0);
    p.end();
  }
  return g_on;
}

void dbgLogBegin() {
  Preferences p;
  uint8_t want = 0;
  if (p.begin(kNs, true)) {   // false here just means the namespace is new
    want = p.getUChar(kKey, 0);
    p.end();
  }
  if (want == 1) dbgLogSetEnabled(true);
}

void dbgLogf(const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  const uint32_t len = (uint32_t)(n < (int)sizeof line ? n : (int)sizeof line - 1);
  // Serial always, so a cable shows the same thing whether the log is on or not.
  Serial.write((const uint8_t *)line, len);
  if (g_on) ringWrite(line, len);
}

void dbgLogWrite(const char *p, uint32_t n) {
  Serial.write((const uint8_t *)p, n);
  if (g_on) ringWrite(p, n);
}

void dbgLogClear() {
  // In place: freeing and re-allocating would churn 36 KB of PSRAM and write
  // the switch to flash twice, on an unauthenticated GET.
  portENTER_CRITICAL(&g_mux);
  if (g_ring.buf) dbgRingInit(&g_ring, g_ring.buf, DBG_LOG_BYTES);
  portEXIT_CRITICAL(&g_mux);
}

char *dbgLogReadBuf() { return g_ring.buf ? g_ring.buf + DBG_LOG_BYTES : nullptr; }

uint32_t dbgLogRead(uint32_t since, char *out, uint32_t max, uint32_t *from) {
  if (max > DBG_LOG_READ_MAX) max = DBG_LOG_READ_MAX;
  portENTER_CRITICAL(&g_mux);
  const uint32_t n = dbgRingRead(&g_ring, since, out, max, from);
  portEXIT_CRITICAL(&g_mux);
  return n;
}

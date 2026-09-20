// The broker. See net_broker.h for what it is for, nb_queue.h for whose turn
// it is, and docs/32-net-broker.md in the knowledge base for how we got here.
#include "net_broker.h"

#if defined(NET_BROKER_ENABLED)

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#include <string.h>

#include "../debug/dbg_log.h"
#include "../network/net_lock.h"

namespace {

// **12 KB, and why that number for now.** It is the largest of the four stacks
// it replaces (the flight board's; weather 8, world clock 8, rail 9), so no
// caller can be worse off than it is today. It is deliberately NOT tightened
// yet: the rail board's stack was cut from 12 KB to 9 only after three agreeing
// high-water readings, and the broker has no readings at all. It prints its own
// high-water mark on every fetch (`[nb]` lines, and nbGetStats for the
// diagnostics page); the number comes down when there is a distribution to cut
// it from, not before.
const uint32_t kStackBytes = 12 * 1024;

// Core 0, below the Lua effect task, exactly where the four fetch tasks run
// today: the Arduino loop and the HUB75 DMA refresh live on core 1.
const UBaseType_t kPriority = 0;
const BaseType_t  kCore     = 0;

// Long enough for a slow origin on a busy evening, short enough that one dead
// host does not hold the other three for a visible time. The rail board used
// 12 s against its own API and the weather 10 s; this is the larger, so no
// caller gets a shorter deadline than it has today.
const uint32_t kDefaultTimeoutMs = 12000;
// Seconds, and a separate figure: the library's own default is 120 s, which is
// two minutes of one caller holding the wire before anything is even sent.
const int kHandshakeTimeoutS = 12;

// How long the task sleeps when there is nothing to do. It is woken by
// nbSubmitRequest, so this is only a backstop against a lost notification -
// nothing waits on it in normal use.
const uint32_t kIdleWaitMs = 1000;

// The stack and the task block are .bss, not the heap. That is the whole
// point: taken at link time, so they can never fail to be allocated at the
// worst moment, and never leave a hole behind when the fetch ends. FreeRTOS in
// arduino-esp32 2.0.17 is built without CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM,
// and xTaskCreateStaticPinnedToCore asserts the stack is internal memory -
// .bss is internal, so this satisfies it (portVALID_STACK_MEM -> the port's
// xPortcheckValidStackMem, portmacro.h:742; the same note is in src/lua/README.md).
//
// **On the units, because the header contradicts itself.** task.h documents
// ulStackDepth as "the number of bytes. Note that this differs from vanilla
// FreeRTOS", and then documents pxStackBuffer as needing "at least ulStackDepth
// indexes". Both are true at once and neither is a trap: this port defines
// portSTACK_TYPE as uint8_t (portmacro.h:80), so a StackType_t IS a byte and an
// index IS a byte. The expression below is therefore the same number either way
// - it is written with the sizeof so that it stays correct if that ever changes.
StackType_t  s_stack[kStackBytes / sizeof(StackType_t)];
StaticTask_t s_tcb;
TaskHandle_t s_task = nullptr;

// What a caller asked for. The strings are in PSRAM - they are touched only by
// the loop task and the broker task, never by DMA and never as a stack - so
// four generous buffers cost nothing where it is scarce.
struct NbText { char url[NB_URL_MAX]; char auth[NB_AUTH_MAX]; };
NbText *s_text = nullptr;

// The small part stays internal: sixteen bytes a caller.
struct NbJob {
  NbParseFn parse;
  void     *ctx;
  uint32_t  timeoutMs;
  bool      done;      // an outcome is waiting to be collected
  bool      ok;        // what parse() returned, or false
};
NbJob s_job[NB_CALLER_COUNT];

NbQueue      s_q;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t s_served = 0;
uint32_t s_failed = 0;
uint32_t s_stackFreeMin = kStackBytes;

// One TLS client and one HTTPClient for every fetch, constructed once. The
// mbedTLS record buffers they own are already redirected to PSRAM elsewhere in
// the firmware; what is saved here is the churn of building and tearing the
// pair down four different ways.
WiFiClientSecure *s_tls = nullptr;
HTTPClient       *s_http = nullptr;

// Do one fetch, start to finish, on the broker task. Returns what the caller's
// parse function returned.
bool runJob(uint8_t who) {
  NbJob job;
  portENTER_CRITICAL(&s_mux);
  job = s_job[who];
  portEXIT_CRITICAL(&s_mux);

  NbReply reply = {0, nullptr, false, job.ctx};

  // Still interlocked with the three modules that have not moved over yet, and
  // it **waits** rather than gives up: while they still start their own fetch
  // tasks, a broker that refused the moment one of them held the lock would
  // simply never fetch. The wait is the same 30 s each of them uses today. When
  // the last one is migrated the lock has no other holder and goes with it.
  NetLockGuard net(NET_LOCK_WAIT_MS);
  if (!net.held()) {
    reply.code = -1;
    return false;
  }

  // setTimeout takes a uint16_t, so a caller asking for more than 65.5 s would
  // silently wrap to something short. Clamp, and say so here rather than let
  // the truncation be discovered on the wall.
  uint32_t timeout = job.timeoutMs ? job.timeoutMs : kDefaultTimeoutMs;
  if (timeout > 65000) timeout = 65000;
  s_tls->setInsecure();   // as every caller does today; public, non-sensitive data
  s_tls->setHandshakeTimeout(kHandshakeTimeoutS);
  s_http->setTimeout((uint16_t)timeout);
  s_http->setConnectTimeout((int32_t)timeout);
  s_http->useHTTP10(true);   // no chunked framing to unpick while parsing the stream

  if (!s_http->begin(*s_tls, s_text[who].url)) {
    dbgLogf("[nb] %u: begin refused the URL\n", (unsigned)who);
    return false;
  }
  if (s_text[who].auth[0]) s_http->addHeader("Authorization", s_text[who].auth);
  s_http->addHeader("Accept", "application/json");

  reply.code = s_http->GET();
  bool ok = false;
  if (reply.code == HTTP_CODE_OK) {
    reply.body = s_http->getStreamPtr();
    ok = job.parse ? job.parse(reply) : true;
  } else {
    char err[48];
    reply.tls = reply.code < 0 && s_tls->lastError(err, sizeof err) != 0;
    // The URL is not logged: it carries the caller's API key for the boards
    // that use one. The status is what tells us anything anyway.
    dbgLogf("[nb] %u: HTTP %d%s\n", (unsigned)who, reply.code, reply.tls ? " (tls)" : "");
    if (job.parse) job.parse(reply);   // let the caller see the failure code
  }
  s_http->end();
  s_tls->stop();   // the session is not kept: four different hosts take turns
  return ok;
}

void brokerTask(void *) {
  for (;;) {
    uint8_t who = NB_CALLER_COUNT;
    portENTER_CRITICAL(&s_mux);
    who = nbPick(&s_q, millis());
    if (who < NB_CALLER_COUNT) nbStart(&s_q, who);
    portEXIT_CRITICAL(&s_mux);

    if (who >= NB_CALLER_COUNT) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWaitMs));
      continue;
    }

    const uint32_t startMs = millis();
    const bool ok = runJob(who);
    const uint32_t tookMs = millis() - startMs;

    // In ESP-IDF's FreeRTOS port a task's stack depth is given in bytes, and
    // this returns the smallest amount that was ever still free, in bytes too.
    const uint32_t freeMin = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

    portENTER_CRITICAL(&s_mux);
    s_job[who].done = true;
    s_job[who].ok = ok;
    memset(s_text[who].auth, 0, sizeof s_text[who].auth);   // the token does not linger
    if (freeMin < s_stackFreeMin) s_stackFreeMin = freeMin;
    s_served++;
    if (!ok) s_failed++;
    nbFinish(&s_q);
    portEXIT_CRITICAL(&s_mux);

    dbgLogf("[nb] %u: %s in %u ms, stack low-water %u B free\n", (unsigned)who,
            ok ? "ok" : "failed", (unsigned)tookMs, (unsigned)freeMin);
  }
}

}  // namespace

bool nbBegin() {
  if (s_task) return true;

  s_text = (NbText *)heap_caps_calloc(NB_CALLER_COUNT, sizeof(NbText),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_text) {
    Serial.println("[nb] no PSRAM for the request buffers: the broker stays down");
    return false;
  }
  s_tls = new (std::nothrow) WiFiClientSecure();
  s_http = new (std::nothrow) HTTPClient();
  if (!s_tls || !s_http) {
    Serial.println("[nb] no room for the TLS client: the broker stays down");
    delete s_tls; s_tls = nullptr;
    delete s_http; s_http = nullptr;
    heap_caps_free(s_text); s_text = nullptr;
    return false;
  }

  nbInit(&s_q);
  memset(s_job, 0, sizeof s_job);

  s_task = xTaskCreateStaticPinnedToCore(brokerTask, "netbroker",
                                         kStackBytes / sizeof(StackType_t), nullptr,
                                         kPriority, s_stack, &s_tcb, kCore);
  if (!s_task) {
    // Static creation has nothing to allocate, so this means a bad argument,
    // not a shortage - but say so rather than run on with a dead broker.
    Serial.println("[nb] task create refused: the broker stays down");
    delete s_tls; s_tls = nullptr;
    delete s_http; s_http = nullptr;
    heap_caps_free(s_text); s_text = nullptr;
    return false;
  }
  Serial.printf("[nb] up: %u B stack in .bss, %u B of request buffers in PSRAM\n",
                (unsigned)kStackBytes, (unsigned)(NB_CALLER_COUNT * sizeof(NbText)));
  return true;
}

bool nbSubmitRequest(uint8_t who, const NbRequest &req, bool interactive) {
  if (!s_task || who >= NB_CALLER_COUNT || !req.url) return false;
  if (strlen(req.url) >= NB_URL_MAX) return false;
  if (req.auth && strlen(req.auth) >= NB_AUTH_MAX) return false;

  portENTER_CRITICAL(&s_mux);
  // An uncollected outcome is not overwritten: collect it, then ask again.
  // Otherwise a caller that submits twice in a row loses the first answer and
  // waits a whole refresh interval to find out.
  const bool blocked = s_job[who].done || s_q.slot[who].state == NB_ONAIR;
  if (!blocked) {
    s_job[who].parse = req.parse;
    s_job[who].ctx = req.ctx;
    s_job[who].timeoutMs = req.timeoutMs;
    s_job[who].ok = false;
    // Under the lock, so the broker can never read half of a URL that the loop
    // task is still writing.
    strlcpy(s_text[who].url, req.url, NB_URL_MAX);
    if (req.auth) strlcpy(s_text[who].auth, req.auth, NB_AUTH_MAX);
    else s_text[who].auth[0] = '\0';
    nbSubmit(&s_q, who, interactive, millis());
  }
  portEXIT_CRITICAL(&s_mux);

  if (blocked) return false;
  xTaskNotifyGive(s_task);
  return true;
}

bool nbPending(uint8_t who) {
  if (!s_task || who >= NB_CALLER_COUNT) return false;
  portENTER_CRITICAL(&s_mux);
  const bool p = s_q.slot[who].state != NB_EMPTY;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

bool nbTake(uint8_t who, bool *ok) {
  if (!s_task || who >= NB_CALLER_COUNT) return false;
  portENTER_CRITICAL(&s_mux);
  const bool had = s_job[who].done;
  const bool res = s_job[who].ok;
  s_job[who].done = false;
  portEXIT_CRITICAL(&s_mux);
  if (had && ok) *ok = res;
  return had;
}

void nbGetStats(NbStats *out) {
  if (!out) return;
  const uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  out->onAir = s_q.onAir;
  out->waiting = nbWaiting(&s_q);
  out->onAirMs = nbOnAirMs(&s_q, now);
  out->served = s_served;
  out->failed = s_failed;
  out->stackFreeMin = s_stackFreeMin;
  portEXIT_CRITICAL(&s_mux);
}

#else   // !NET_BROKER_ENABLED

// The broker is not built in. Every caller keeps its own path; nbBegin() says
// so once, and a submit is simply refused - callers test the return value and
// fall back, so nothing changes for a build without the flag.
bool nbBegin() { return false; }
bool nbSubmitRequest(uint8_t, const NbRequest &, bool) { return false; }
bool nbPending(uint8_t) { return false; }
bool nbTake(uint8_t, bool *) { return false; }
void nbGetStats(NbStats *out) { if (out) *out = NbStats{NB_CALLER_COUNT, 0, 0, 0, 0, 0}; }

#endif

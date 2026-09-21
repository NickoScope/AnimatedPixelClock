// The broker. See net_broker.h for what it is for, nb_queue.h for whose turn
// it is, and docs/32-net-broker.md in the knowledge base for how we got here.
#include "net_broker.h"

#if defined(NET_BROKER_ENABLED)

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>
#include <string.h>

#include "../debug/dbg_log.h"
#include "../network/net_lock.h"

namespace {

// **8 KB, from a measurement rather than from the largest of four.**
//
// This was 12 KB - the largest of the stacks it replaces - chosen so that no
// caller could be worse off. That reasoning was wrong in a way the panel
// proved on 2026-09-20: taking 12 KB into .bss cut the largest contiguous
// internal block from 16,372 B to 9,716, below what the flight board requires
// before it will even attempt a fetch. A number picked for safety made the
// board unusable.
//
// The figure below comes from the closest analogue there is - the rail board's
// own fetch task, which does the same work this one does: TLS handshake,
// HTTPClient, and the consumer's parse straight off the socket. Four readings
// of its high-water mark, 2026-09-20 and 2026-09-21:
//
//     used 5,984 · 6,136 · 6,152 · 6,160 B     (spread 176 B over four)
//
// The last was taken through /api/railboard with a 9,216 B stack, leaving
// 3,056 B free. 8,192 B therefore carries the measured peak plus about 2 KB -
// a slightly tighter margin than the rail board keeps, on a task that reports
// its own high-water on every fetch so the margin is watched rather than
// assumed. If `stackFreeMin` in /api/info ever approaches zero, this is where
// to look, and `nsc status` is how to see it.
//
// **What 8 KB does NOT fix.** It is not enough on its own: the flight board's
// threshold is 13,312 B against a largest block of 16,372, so barely 3 KB of
// contiguity is available while that board still starts its own task. The
// broker has to take the flight board's fetch over before it can pay for
// itself - see docs/32-net-broker.md.
const uint32_t kStackBytes = 8 * 1024;

// Core 0, below the Lua effect task, exactly where the four fetch tasks run
// today: the Arduino loop and the HUB75 DMA refresh live on core 1.
//
// Priority 1, not 0, and the difference matters now. The four fetch tasks ran
// at 0 - tskIDLE_PRIORITY - which was harmless because each lived for one
// fetch and died. This one never dies, and a permanent task sharing IDLE0's
// priority shares it with the task that feeds the watchdog. Every path in its
// loop blocks, so it does not starve anything; one above idle simply removes
// the question.
const UBaseType_t kPriority = 1;
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
// nbSubmitRequest, so this is only a backstop - nothing waits on it in normal
// use.
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
//
// Aligned to 16 because a uint8_t array has an alignment of one, and the port
// rounds the stack top DOWN to portBYTE_ALIGNMENT (16): an unaligned buffer
// silently loses up to fifteen bytes off the top. Not a fault, but this way the
// 12,288 B the map reports is the 12,288 B the task actually gets.
StackType_t  s_stack[kStackBytes / sizeof(StackType_t)] __attribute__((aligned(16)));
StaticTask_t s_tcb;
TaskHandle_t s_task = nullptr;

// The wake-up is a semaphore of our own rather than the task's notification.
// A task has exactly one notification slot in this build
// (configTASK_NOTIFICATION_ARRAY_ENTRIES is 1, FreeRTOSConfig.h:253), several
// IDF drivers signal completion through direct-to-task notifications, and a
// caller's parse() runs on THIS task - so a driver used inside a parse could
// eat the broker's own wake-up and leave requests waiting for the 1 s backstop.
// Static, so it is .bss like the stack and cannot fail to be created.
StaticSemaphore_t s_wakeBuf;
SemaphoreHandle_t s_wake = nullptr;

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

// Deliver an outcome to the caller, exactly once, whatever happened. Every
// path out of a request goes through here, which is what makes the contract on
// NbParseFn true rather than aspirational.
bool deliver(const NbJob &job, NbReply &reply) {
  return job.parse ? job.parse(reply) : (reply.code == HTTP_CODE_OK);
}

// Do one fetch on the broker task. The network's turn is already held by the
// caller: taking it here would mean the slot sat marked on-air for the whole
// wait, and an interactive request arriving a millisecond later would queue
// behind a wait it has nothing to do with.
bool runJob(uint8_t who) {
  NbJob job;
  portENTER_CRITICAL(&s_mux);
  job = s_job[who];
  portEXIT_CRITICAL(&s_mux);

  NbReply reply = {0, nullptr, false, job.ctx};

  uint32_t timeout = job.timeoutMs ? job.timeoutMs : kDefaultTimeoutMs;
  // setTimeout takes a uint16_t, so a caller asking for more than 65 s would
  // silently wrap to something short. Clamp rather than let the truncation be
  // discovered on the wall.
  if (timeout > 65000) timeout = 65000;
  s_tls->setInsecure();   // as every caller does today; public, non-sensitive data
  s_tls->setHandshakeTimeout(kHandshakeTimeoutS);
  s_http->setTimeout((uint16_t)timeout);
  s_http->setConnectTimeout((int32_t)timeout);
  s_http->useHTTP10(true);   // no chunked framing to unpick while parsing the stream

  // **Every exit from here down calls end(), and that is a security property,
  // not tidiness.** One HTTPClient is reused by four callers in turn, and the
  // headers it sends accumulate in one String that ONLY end() -> clear()
  // empties (HTTPClient.cpp:123-128, :1064). Leave by a path that skips end()
  // and the next caller's request carries the previous caller's Authorization
  // header to a different host. So: a guard, rather than remembering.
  struct HttpEnd {
    HTTPClient *h;
    ~HttpEnd() { h->end(); }
  } httpEnd{s_http};

  if (!s_http->begin(*s_tls, s_text[who].url)) {
    // Not logged with the URL: it carries an API key for two of the boards.
    dbgLogf("[nb] %u: the URL was refused before connecting\n", (unsigned)who);
    reply.code = NB_ERR_BAD_URL;
    return deliver(job, reply);
  }
  if (s_text[who].auth[0]) s_http->addHeader("Authorization", s_text[who].auth);
  s_http->addHeader("Accept", "application/json");

  // Baseline, because WiFiClientSecure::_lastError is sticky and now SHARED.
  // It is written only by connect(IPAddress,...) - WiFiClientSecure.cpp:142 -
  // and never cleared: not by stop(), not at the start of a connect. Two
  // consequences, both of which would make this log lie:
  //   * on SUCCESS it is set to start_ssl_client's return, which is the socket
  //     descriptor (ssl_client.cpp) - a positive number, not zero, so every
  //     later failure would report a handshake error that did not happen;
  //   * connect(const char *host,...) returns 0 on a DNS failure WITHOUT
  //     touching it, so a name that does not resolve would report whatever the
  //     previous fetch left - and with one client serving four consumers, that
  //     is another module's error against this module's host.
  // So: a real handshake failure is negative AND different from what was there
  // before this request.
  char prevErr[64];
  const int errBefore = s_tls->lastError(prevErr, sizeof prevErr);

  reply.code = s_http->GET();
  if (reply.code > 0) {
    // Set for an error status too, so a caller that wants to read the server's
    // explanation can. Never set when code < 0: there is no stream then.
    reply.body = s_http->getStreamPtr();
    if (reply.code != HTTP_CODE_OK) dbgLogf("[nb] %u: HTTP %d\n", (unsigned)who, reply.code);
  } else {
    char err[64] = {0};
    const int errAfter = s_tls->lastError(err, sizeof err);
    reply.tls = errAfter < 0 && errAfter != errBefore;
    dbgLogf("[nb] %u: HTTP %d%s%s\n", (unsigned)who, reply.code,
            reply.tls ? " tls: " : "", reply.tls ? err : "");
  }

  const bool ok = deliver(job, reply);
  // end() is the guard's job; stop() is ours, because the session is not kept:
  // four different hosts take turns on this one client.
  s_tls->stop();
  return ok;
}

void brokerTask(void *) {
  for (;;) {
    // Look without committing. Nothing is marked on-air yet, so the wait for
    // the network's turn below cannot make a later interactive request queue
    // behind it.
    portENTER_CRITICAL(&s_mux);
    uint8_t who = nbPick(&s_q, millis());
    portEXIT_CRITICAL(&s_mux);
    if (who >= NB_CALLER_COUNT) {
      xSemaphoreTake(s_wake, pdMS_TO_TICKS(kIdleWaitMs));
      continue;
    }

    // Still interlocked with the three modules that have not moved over yet,
    // and it waits rather than gives up: while they still start their own
    // fetch tasks, a broker that refused the moment one of them held the lock
    // would simply never fetch. When the last one is migrated this lock has no
    // other holder and goes with it.
    NetLockGuard net(NET_LOCK_WAIT_MS);

    // Pick again: the wait may have been long, and an interactive request that
    // arrived during it deserves to win the turn it would have won anyway.
    portENTER_CRITICAL(&s_mux);
    who = nbPick(&s_q, millis());
    if (who < NB_CALLER_COUNT) nbStart(&s_q, who);
    portEXIT_CRITICAL(&s_mux);
    if (who >= NB_CALLER_COUNT) continue;

    const uint32_t startMs = millis();
    bool ok;
    if (net.held()) {
      ok = runJob(who);
    } else {
      // Report it rather than silently re-queue: a caller waiting on nbTake()
      // must always get an answer, or it sits busy until a reboot.
      NbJob job;
      portENTER_CRITICAL(&s_mux);
      job = s_job[who];
      portEXIT_CRITICAL(&s_mux);
      NbReply reply = {NB_ERR_NO_TURN, nullptr, false, job.ctx};
      dbgLogf("[nb] %u: no network turn within %u ms\n", (unsigned)who,
              (unsigned)NET_LOCK_WAIT_MS);
      ok = deliver(job, reply);
    }
    const uint32_t tookMs = millis() - startMs;

    // In ESP-IDF's FreeRTOS port a task's stack depth is given in bytes, and
    // this returns the smallest amount that was ever still free, in bytes too.
    const uint32_t freeMin = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

    // Outside the critical section: 128 B of PSRAM, and nobody may write to
    // this slot while it is still on air.
    memset(s_text[who].auth, 0, sizeof s_text[who].auth);   // the token does not linger

    portENTER_CRITICAL(&s_mux);
    s_job[who].done = true;
    s_job[who].ok = ok;
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

  s_text = static_cast<NbText *>(heap_caps_calloc(NB_CALLER_COUNT, sizeof(NbText),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

  s_wake = xSemaphoreCreateBinaryStatic(&s_wakeBuf);   // static: cannot fail
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

bool nbUp() { return s_task != nullptr; }

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
  xSemaphoreGive(s_wake);
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
  // The guard belongs here, not only in the caller: if nbBegin() failed before
  // nbInit(), s_q is zeroed .bss where onAir == 0 == NB_WEATHER, so nbBusy()
  // would read as true and onAirMs would climb from boot.
  if (!s_task) { *out = NbStats{NB_CALLER_COUNT, 0, 0, 0, 0, 0}; return; }
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
bool nbUp() { return false; }
bool nbSubmitRequest(uint8_t, const NbRequest &, bool) { return false; }
bool nbPending(uint8_t) { return false; }
bool nbTake(uint8_t, bool *) { return false; }
void nbGetStats(NbStats *out) { if (out) *out = NbStats{NB_CALLER_COUNT, 0, 0, 0, 0, 0}; }

#endif

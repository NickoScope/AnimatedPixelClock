// The broker. See net_broker.h for what it is for, nb_queue.h for whose turn
// it is, and docs/32-net-broker.md in the knowledge base for how we got here
// and which parts are NetGate's.
#include "net_broker.h"

#if defined(NET_BROKER_ENABLED)

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "../debug/dbg_log.h"
#include "../network/net_lock.h"

namespace {

// **8 KB, from a measurement rather than from the largest of four.**
//
// This was 12 KB - the largest of the stacks it replaces - chosen so that no
// caller could be worse off. That reasoning was wrong in a way the panel proved
// on 2026-09-20: taking 12 KB into .bss cut the largest contiguous internal
// block from 16,372 B to 9,716, below what the flight board requires before it
// will even attempt a fetch. A number picked for safety made the board unusable.
//
// 8,192 B comes from the rail board's own fetch task, which did the same work:
// four readings of its high-water mark, 5,984 / 6,136 / 6,152 / 6,160 B, a
// spread of 176 B. Since the parse moved off this task onto loop(), the measured
// use here is lower still - 4,052 B of 8,192 on the weather fetch, read from
// /api/info on 2026-09-21 - and, more to the point, it no longer depends on what
// any consumer's parser does. That is what the mailbox buys: the number below is
// now a property of this file.
//
// NetGate's equivalent task is 16 KB and its own ADD-62 v1.4 records that 8 KB
// proved sufficient over 24 h of uptime; that finding was never applied there.
const uint32_t kStackBytes = 8 * 1024;

// Core 0, below the Lua effect task, exactly where the four fetch tasks ran:
// the Arduino loop and the HUB75 DMA refresh live on core 1.
//
// Priority 1, not 0. The four fetch tasks ran at 0 - tskIDLE_PRIORITY - which
// was harmless because each lived for one fetch and died. This one never dies,
// and a permanent task sharing IDLE0's priority shares it with the task that
// feeds the watchdog. Every path in its loop blocks, so it starves nothing;
// one above idle simply removes the question. (NetGate's netTask is also 1.)
const UBaseType_t kPriority = 1;
const BaseType_t  kCore     = 0;

// Long enough for a slow origin on a busy evening, short enough that one dead
// host does not hold the other three for a visible time. The rail board used
// 12 s against its own API and the weather 10 s; this is the larger, so no
// caller gets a shorter deadline than it had.
const uint32_t kDefaultTimeoutMs = 12000;
// Seconds, and a separate figure: the library's own default is 120 s, which is
// two minutes of one caller holding the wire before anything is even sent.
const int kHandshakeTimeoutS = 12;

// How long the task sleeps when there is nothing to do. It is woken by
// nbSubmitRequest, so this is only a backstop.
const uint32_t kIdleWaitMs = 1000;

// **How big each caller's mailbox is, and what a zero means.**
//
// A zero is "this module has not migrated yet": it has no mailbox, and
// nbSubmitRequest refuses it. So the migration state of the whole thing is this
// one table rather than four modules' worth of half-remembered state.
//
// A number here has to be earned by a measurement of the real bodies that
// module receives - the panel reports `bodyBytes` on both transport boards -
// because a cap too small silently becomes NB_ERR_TRUNC and a cap too large is
// PSRAM held from boot for nothing. Weather's open-meteo reply with the fields
// we ask for runs well under 2 KB; 8 KB is four times that.
const uint32_t kBodyCap[NB_CALLER_COUNT] = {
  // 8 KB against a measured body of 746 B (panel, 2026-09-21: "[nb] 0: code
  // 200, 746 B in 1255 ms"). Deliberately NOT cut to fit that: one sample is
  // not a distribution, this lives in PSRAM where it costs nothing scarce, and
  // a cap that is too small becomes NB_ERR_TRUNC on the day open-meteo adds a
  // field. It comes down when there are several readings to come down to.
  8 * 1024,   // NB_WEATHER     - migrated 2026-09-21
  0,          // NB_WORLDCLOCK  - not yet; size it from a measured body
  0,          // NB_RAIL        - not yet; its own buffer is 1.5 MB today
  0,          // NB_FLIGHT      - not yet; its own buffer is 192 KB today
};

// The stack and the task block are .bss, not the heap. That is the whole point:
// taken at link time, so they can neither fail to be allocated at the worst
// moment nor leave a hole when a fetch ends. FreeRTOS in arduino-esp32 2.0.17 is
// built without CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM, and
// xTaskCreateStaticPinnedToCore asserts the stack is internal memory - .bss is
// internal, so this satisfies it (portVALID_STACK_MEM -> the port's
// xPortcheckValidStackMem, portmacro.h:742).
//
// **On the units, because the header contradicts itself.** task.h documents
// ulStackDepth as "the number of bytes. Note that this differs from vanilla
// FreeRTOS", and then documents pxStackBuffer as needing "at least ulStackDepth
// indexes". Both are true at once: this port defines portSTACK_TYPE as uint8_t
// (portmacro.h:80), so a StackType_t IS a byte and an index IS a byte.
//
// Aligned to 16 because a uint8_t array has an alignment of one and the port
// rounds the stack top DOWN to portBYTE_ALIGNMENT (16): an unaligned buffer
// silently loses up to fifteen bytes off the top.
StackType_t  s_stack[kStackBytes / sizeof(StackType_t)] __attribute__((aligned(16)));
StaticTask_t s_tcb;
TaskHandle_t s_task = nullptr;

// The wake-up is a semaphore of our own rather than the task's notification.
// A task has exactly one notification slot in this build
// (configTASK_NOTIFICATION_ARRAY_ENTRIES is 1, FreeRTOSConfig.h:253) and several
// IDF drivers signal completion through direct-to-task notifications, so
// anything this task calls could eat its wake-up. Static, so it is .bss like the
// stack and cannot fail to be created.
StaticSemaphore_t s_wakeBuf;
SemaphoreHandle_t s_wake = nullptr;

// What a caller asked for. The strings are in PSRAM - touched only by the loop
// task and the broker task, never by DMA and never as a stack - so four generous
// buffers cost nothing where it is scarce.
struct NbText { char url[NB_URL_MAX]; char auth[NB_AUTH_MAX]; };
NbText *s_text = nullptr;

// The small part stays internal: a few bytes a caller.
struct NbJob {
  const char *caCert;     // not copied: a static/PROGMEM string from the caller
  const char *authHeader; // likewise; nullptr means "Authorization"
  const char *collect;    // likewise, a literal header name
  uint32_t    tag;
  uint32_t    timeoutMs;
};
NbJob s_job[NB_CALLER_COUNT];

NbMailbox s_mb[NB_CALLER_COUNT];

NbQueue      s_q;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t s_served = 0;
uint32_t s_failed = 0;
uint32_t s_stackFreeMin = kStackBytes;

// Where the body goes. HTTPClient::writeToStream() decodes chunked framing and
// content-length itself and pushes the decoded bytes here, which is why this
// exists instead of a hand-rolled read loop on available()/connected(). Both of
// our transport boards still have such a loop, and a truncated response is
// exactly the kind of fault they hide. NetGate reached the same conclusion - its
// NgSink is netgate.cpp:81-103 - and this is that, with 32-bit lengths, because
// two of our consumers deal in bodies far past its 64 KB.
class NbSink : public Stream {
 public:
  char    *buf;
  uint32_t cap;
  uint32_t len = 0;
  bool     truncated = false;

  NbSink(char *b, uint32_t c) : buf(b), cap(c) {}

  size_t write(uint8_t b) override {
    if (cap < 2 || len >= cap - 1) { truncated = true; return 0; }
    buf[len++] = (char)b;
    return 1;
  }
  size_t write(const uint8_t *data, size_t size) override {
    if (cap < 2) { truncated = true; return 0; }
    const size_t room = (size_t)(cap - 1) - len;
    const size_t n = size < room ? size : room;
    if (n) { memcpy(buf + len, data, n); len += (uint32_t)n; }
    if (n < size) truncated = true;
    return n;   // a short write stops writeToStream by itself
  }
  // A sink only writes; the read half of Stream is not used.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Do one fetch on the broker task. The network's turn is already held by the
// caller. Returns the code that goes in the mailbox.
int32_t runJob(uint8_t who, NbMailbox &mb) {
  NbJob job;
  portENTER_CRITICAL(&s_mux);
  job = s_job[who];
  portEXIT_CRITICAL(&s_mux);

  mb.bodyLen = 0;
  mb.header[0] = '\0';
  if (mb.body && mb.bodyCap) mb.body[0] = '\0';

  uint32_t timeout = job.timeoutMs ? job.timeoutMs : kDefaultTimeoutMs;
  // setTimeout takes a uint16_t, so a caller asking for more than 65 s would
  // silently wrap to something short.
  if (timeout > 65000) timeout = 65000;

  int32_t code = 0;
  bool truncated = false;
  {
    // Both live in this scope and nowhere else, and that is deliberate: what
    // they own on the heap goes back when they leave it, which is before the
    // answer is published, so the consumer never acts on a heap this request is
    // still holding. NetGate makes the same choice and says why at
    // netgate.cpp:128 and :198.
    WiFiClientSecure tls;
    HTTPClient http;

    // Exactly one of these, on every request, never conditionally. A request
    // that set neither would inherit whatever the library was left in.
    if (job.caCert) tls.setCACert(job.caCert);
    else            tls.setInsecure();
    tls.setHandshakeTimeout(kHandshakeTimeoutS);

    http.setTimeout((uint16_t)timeout);
    http.setConnectTimeout((int32_t)timeout);
    http.setReuse(false);

    if (!http.begin(tls, s_text[who].url)) {
      // Not logged with the URL: it carries an API key for two of the boards.
      dbgLogf("[nb] %u: the URL was refused before connecting\n", (unsigned)who);
      code = NB_ERR_BAD_URL;
    } else {
      if (s_text[who].auth[0])
        http.addHeader(job.authHeader ? job.authHeader : "Authorization", s_text[who].auth);
      http.addHeader("Accept", "application/json");
      const char *collect[1] = {job.collect};
      if (job.collect) http.collectHeaders(collect, 1);

      code = http.GET();
      if (code > 0 && mb.body && mb.bodyCap) {
        NbSink sink(mb.body, mb.bodyCap);
        const int written = http.writeToStream(&sink);
        mb.bodyLen = sink.len;
        truncated = sink.truncated;
        if (written < 0 && sink.len == 0 && code == HTTP_CODE_OK) code = HTTPC_ERROR_READ_TIMEOUT;
      }
      // Read before end(): the header table is cleared with the connection.
      if (job.collect) {
        const String value = http.header(job.collect);
        strncpy(mb.header, value.c_str(), sizeof(mb.header) - 1);
        mb.header[sizeof(mb.header) - 1] = '\0';
      }
      http.end();
    }
  }   // the TLS client's memory is back here, before anything is published

  if (mb.body && mb.bodyCap) mb.body[mb.bodyLen] = '\0';
  // Truncation is reported, never silent. A caller parsing a body cut mid-object
  // gets a parse error and blames the server; this says who really did it.
  if (truncated) {
    dbgLogf("[nb] %u: body did not fit %u B - truncated\n", (unsigned)who, (unsigned)mb.bodyCap);
    code = NB_ERR_TRUNC;
  }
  if (code < 0 && code != NB_ERR_BAD_URL && code != NB_ERR_TRUNC)
    dbgLogf("[nb] %u: transport %d\n", (unsigned)who, (int)code);
  else if (code > 0 && code != HTTP_CODE_OK)
    dbgLogf("[nb] %u: HTTP %d\n", (unsigned)who, (int)code);
  return code;
}

// Fill every field, then bump seq. That order is the contract: a consumer that
// sees a new seq is guaranteed the payload that belongs to it.
void publish(uint8_t who, int32_t code, uint32_t startMs) {
  NbMailbox &mb = s_mb[who];
  mb.code = code;
  mb.tag = s_job[who].tag;
  mb.durationMs = millis() - startMs;
  mb.seq = mb.seq + 1;
}

void brokerTask(void *) {
  for (;;) {
    // Look without committing. Nothing is marked on-air yet, so the wait for the
    // network's turn below cannot make a later interactive request queue behind
    // a wait it has nothing to do with.
    portENTER_CRITICAL(&s_mux);
    uint8_t who = nbPick(&s_q, millis());
    portEXIT_CRITICAL(&s_mux);
    if (who >= NB_CALLER_COUNT) {
      xSemaphoreTake(s_wake, pdMS_TO_TICKS(kIdleWaitMs));
      continue;
    }

    // Still interlocked with the three modules that have not moved over yet, and
    // it waits rather than gives up: while they still start their own fetch
    // tasks, a broker that refused the moment one of them held the lock would
    // simply never fetch. When the last one migrates this lock has no other
    // holder and goes with it.
    NetLockGuard net(NET_LOCK_WAIT_MS);

    // Pick again: the wait may have been long, and an interactive request that
    // arrived during it deserves the turn it would have won anyway.
    portENTER_CRITICAL(&s_mux);
    who = nbPick(&s_q, millis());
    if (who < NB_CALLER_COUNT) nbStart(&s_q, who);
    portEXIT_CRITICAL(&s_mux);
    if (who >= NB_CALLER_COUNT) continue;

    const uint32_t startMs = millis();
    int32_t code;
    if (net.held()) {
      code = runJob(who, s_mb[who]);
    } else {
      // Reported, never silently re-queued: a caller watching its mailbox must
      // always get an answer, or it sits busy until a reboot.
      dbgLogf("[nb] %u: no network turn within %u ms\n", (unsigned)who,
              (unsigned)NET_LOCK_WAIT_MS);
      s_mb[who].bodyLen = 0;
      s_mb[who].header[0] = '\0';
      code = NB_ERR_NO_TURN;
    }

    // In ESP-IDF's FreeRTOS port a task's stack depth is given in bytes, and
    // this returns the smallest amount ever still free, in bytes too.
    const uint32_t freeMin = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);

    // Outside the critical section: PSRAM, and nobody may write to this slot
    // while it is still on air.
    memset(s_text[who].auth, 0, sizeof s_text[who].auth);   // the token does not linger

    publish(who, code, startMs);

    portENTER_CRITICAL(&s_mux);
    if (freeMin < s_stackFreeMin) s_stackFreeMin = freeMin;
    s_served++;
    if (code != HTTP_CODE_OK) s_failed++;
    nbFinish(&s_q);
    portEXIT_CRITICAL(&s_mux);

    dbgLogf("[nb] %u: code %d, %u B in %u ms, stack low-water %u B free\n",
            (unsigned)who, (int)code, (unsigned)s_mb[who].bodyLen,
            (unsigned)s_mb[who].durationMs, (unsigned)freeMin);
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

  memset(s_mb, 0, sizeof s_mb);
  uint32_t mailboxBytes = 0;
  for (uint8_t i = 0; i < NB_CALLER_COUNT; i++) {
    if (!kBodyCap[i]) continue;   // not migrated: no mailbox, submits refused
    s_mb[i].body = static_cast<char *>(heap_caps_malloc(kBodyCap[i],
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_mb[i].body) {
      // One caller losing its mailbox is not fatal to the others: it keeps its
      // own fetch path, which is exactly what nbReady() is for.
      Serial.printf("[nb] no PSRAM for mailbox %u (%u B): that caller keeps its own path\n",
                    (unsigned)i, (unsigned)kBodyCap[i]);
      continue;
    }
    s_mb[i].bodyCap = kBodyCap[i];
    s_mb[i].body[0] = '\0';
    mailboxBytes += kBodyCap[i];
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
    for (uint8_t i = 0; i < NB_CALLER_COUNT; i++) {
      heap_caps_free(s_mb[i].body); s_mb[i].body = nullptr; s_mb[i].bodyCap = 0;
    }
    heap_caps_free(s_text); s_text = nullptr;
    return false;
  }
  Serial.printf("[nb] up: %u B stack in .bss, %u B of mailboxes and %u B of request buffers in PSRAM\n",
                (unsigned)kStackBytes, (unsigned)mailboxBytes,
                (unsigned)(NB_CALLER_COUNT * sizeof(NbText)));
  return true;
}

bool nbUp() { return s_task != nullptr; }

bool nbReady(uint8_t who) {
  return s_task && who < NB_CALLER_COUNT && s_mb[who].body && s_mb[who].bodyCap;
}

NbMailbox *nbMailbox(uint8_t who) {
  if (!s_task || who >= NB_CALLER_COUNT || !s_mb[who].body) return nullptr;
  return &s_mb[who];
}

bool nbSubmitRequest(uint8_t who, const NbRequest &req, bool interactive) {
  if (!nbReady(who) || !req.url) return false;
  if (strlen(req.url) >= NB_URL_MAX) return false;
  if (req.auth && strlen(req.auth) >= NB_AUTH_MAX) return false;

  portENTER_CRITICAL(&s_mux);
  const bool blocked = s_q.slot[who].state != NB_EMPTY;
  if (!blocked) {
    s_job[who].caCert = req.caCert;
    s_job[who].authHeader = req.authHeader;
    s_job[who].collect = req.collect;
    s_job[who].tag = req.tag;
    s_job[who].timeoutMs = req.timeoutMs;
    // Under the lock, so the broker can never read half of a URL the loop task
    // is still writing.
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

// The broker is not built in. Every caller keeps its own path; nbUp() says so
// and a submit is simply refused, so nothing changes for a build without it.
bool nbBegin() { return false; }
bool nbUp() { return false; }
bool nbReady(uint8_t) { return false; }
NbMailbox *nbMailbox(uint8_t) { return nullptr; }
bool nbSubmitRequest(uint8_t, const NbRequest &, bool) { return false; }
bool nbPending(uint8_t) { return false; }
void nbGetStats(NbStats *out) { if (out) *out = NbStats{NB_CALLER_COUNT, 0, 0, 0, 0, 0}; }

#endif

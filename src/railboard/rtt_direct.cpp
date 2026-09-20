#include "rtt_direct.h"

#if defined(RAILBOARD_DIRECT_ENABLED)

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>

#include "rtt_roots.h"
#include "../network/net_lock.h"
#include "../network/net_turns.h"
#include "../network/network.h"

namespace {

// main.yml "servers" (lines 74-76) is the only server; get_access_token has no
// override of its own (lines 1644-1669).
const char *const kLocationUrl = "https://data.rtt.io/gb-nr/location?";
const char *const kTokenUrl    = "https://data.rtt.io/api/get_access_token";
const char *const kNvsNs       = "rb";

// ── timing ──────────────────────────────────────────────────────────────────
// Design choices, not RTT's numbers (README "Choices that are not standards").
// Home Assistant polls the same token every 20 s when its poll is on, so the
// day's quota is shared: this side slows down as it runs out and leaves the
// rest to Home Assistant.
// 120 s, and it was 30. Measured over the cable 2026-09-20: five minutes with
// this page shown and NO other traffic left the panel at 13,644 B free internal
// and a 7,668 B largest block, because each fetch costs 12-16 KB - a 12 KB task
// stack that must be internal in this SDK (xPortcheckValidStackMem: no
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM here) plus the TLS context. With
// the portal now standing aside while a fetch is on the wire, a slower poll
// also means the portal is refused four times less often. Freshness is what it
// costs; departures do not change on a thirty-second scale, and the board still
// fetches at once when the page appears and when the station changes.
const uint32_t kIntervalS  = 120;       // was 30 - docs/drafts/31 has the measurement
// Off screen nothing is fetched: no task, no TLS session, no buffers (the
// owner's brief, 2026-09-14). Coming into view fetches at once unless the last
// good board is younger than kIntervalS.
const uint32_t kSlowS      = 120;       // remaining-day <= 2 x floor
const uint32_t kTrickleS   = 900;       // remaining-day <= floor, or none left this hour
const int32_t  kFloorMin   = 500;       // floor = max(500, limit-day / 10)
const uint32_t kErrorS[]   = {60, 120, 300};
const uint32_t kAuthFirstS = 900;       // a refused token: 15 min, doubling ...
const uint32_t kAuthMaxS   = 6 * 3600;  // ... to 6 h
const uint32_t kRetryMinS  = 60;
const uint32_t kRetryMaxS  = 3600;
const int64_t  kTokenMarginS = 300;     // exchange again 5 min before validUntil
const int64_t  kTokenAssumeS = 600;     // validUntil missing or unreadable

// ── memory ──────────────────────────────────────────────────────────────────
// The stack is internal RAM (no CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY in
// arduino-esp32 2.0.17, see src/lua/lua_effects.cpp) and exists only while a
// fetch runs. Everything else - mbedTLS (tlsUsePsram), the body, the JSON, the
// access token - is PSRAM. The task reports its stack high-water mark.
// 12 KB, and it stays 12 KB. It was cut to 9 KB on 2026-09-20 from a single
// high-water reading (stackFree 6,304 B of 12 KB, so 5,984 B used) and put back
// the same evening: one sample is not a distribution, the deepest path through
// mbedTLS depends on the certificate chain and the cipher suite that day, and
// the project's own rule is that a threshold comes from a distribution or it is
// reference only. It also did not help - the panel still lost the link. The
// real problem is the budget, not this number: src/web (JSON in PSRAM), the
// audio leak, and the per-socket send buffer come first.
const uint32_t    kStackBytes      = 12 * 1024;
// Below the Lua effect task (priority 1, core 0): a TLS handshake is hundreds of
// milliseconds of maths, and at equal priority it took turns with the effect's
// frames - the snooker clock stuttered on the panel, 2026-09-14. At 0 it runs
// in the gaps between frames.
const UBaseType_t kPriority        = 0;
const BaseType_t  kCore            = 0;
const size_t      kMinInternalFree = 28 * 1024;   // do not start below this
// Sized from synthetic answers with every field the schema lists (the spec has
// no real example): ~2.4 KB a service compact, ~3.8 KB indented, and ~1.2 KB of
// filtered document on a 64-bit host (less on this 32-bit one). 1.5 MB and
// 384 KB take ~390 indented services and ~300 parsed; past that the fetch
// reports BIG or NOMEM and Home Assistant's boards stay. Both are PSRAM, and
// only while a fetch runs. The real size is in /api/railboard as "bytes".
const size_t      kBodyMax         = 1536 * 1024;
const size_t      kJsonCap         = 384 * 1024;
const size_t      kTokenBodyMax    = 8 * 1024;
const size_t      kTokenMax        = 2048;

enum State : uint8_t {
  ST_IDLE = 0, ST_OK, ST_NOTOKEN, ST_NOCLOCK, ST_NOWIFI, ST_LOWMEM, ST_TASK,
  ST_AUTH, ST_RATE, ST_NET, ST_TLS, ST_HTTP, ST_BAD, ST_BIG, ST_NOMEM, ST_COUNT
};
const char *const kStateNames[ST_COUNT] = {
  "IDLE", "OK", "NO TOKEN", "NO CLOCK", "NO WIFI", "LOW MEM", "TASK",
  "AUTH", "RATE", "NET", "TLS", "HTTP", "BAD", "BIG", "NOMEM"
};

enum Kind : uint8_t { KIND_UNKNOWN = 0, KIND_ACCESS, KIND_REFRESH, KIND_REFUSED };
const char *const kKindNames[] = {"unknown", "access", "refresh-exchanged", "refused"};

// What one fetch found. Written by the task, read by the loop under s_mux.
struct Outcome {
  uint8_t  state;
  uint8_t  kind;
  int16_t  http;
  int32_t  leftDay, limitDay, leftHour, leftMinute;
  uint32_t retryS;
  uint32_t bodyBytes, jsonPeak, services;
  uint32_t heapBefore, heapMin, stackFree;
  int64_t  validUntil;
  int64_t  fetchedAt;
  char     crs[4];
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Diagnostics for a board that fetches and shows nothing: why services were
// dropped, and the start of the services array exactly as RTT sent it. Only the
// location answer is sampled - train data; the token exchange has its own
// buffer and never passes through here. Both under s_mux.
struct DirectDiag {
  uint16_t seen, disp, pax, noEvent, noSched, call, time;
  int64_t  firstT, firstNow;
};
DirectDiag   s_diag;
const size_t kSampleMax = 900;
char        *s_sample = nullptr;        // PSRAM, allocated on the first 200
bool         s_running = false;     // under s_mux
bool         s_ready   = false;     // under s_mux
Outcome      s_done;                // under s_mux
rtt::Lists  *s_result  = nullptr;   // PSRAM; the task writes it, the loop reads it once s_ready

// Task-only: the exchanged access token and when it stops being good.
char    *s_access      = nullptr;   // PSRAM, kTokenMax
int64_t  s_accessUntil = 0;
uint8_t  s_kind        = KIND_UNKNOWN;
char     s_taskCrs[4]  = "";

// Loop-only.
bool     s_tokenStored = false;
bool     s_begun       = false;
uint32_t s_nextAtMs    = 0;
uint32_t s_authStepS   = 0;
uint8_t  s_errStreak   = 0;
uint32_t s_polls = 0, s_fails = 0;
uint32_t s_lastDoneMs = 0, s_lastOkMs = 0;
uint32_t s_lastDelayS = kIntervalS;
bool     s_onScreen   = false;
bool     s_haveOutcome = false;
Outcome  s_last;                    // the latest finished fetch
uint8_t  s_blocked = ST_IDLE;       // why a due fetch did not start

// ── JSON memory ─────────────────────────────────────────────────────────────
// PSRAM, capped, and - for the token exchange - zeroed before it goes back.
class CappedPsram : public ArduinoJson::Allocator {
 public:
  CappedPsram(size_t cap, bool wipe) : cap_(cap), wipe_(wipe) {}
  void *allocate(size_t size) override { return reallocate(nullptr, size); }
  void deallocate(void *ptr) override {
    if (!ptr) return;
    Hdr *h = static_cast<Hdr *>(ptr) - 1;
    used_ -= h->size;
    if (wipe_) memset(ptr, 0, h->size);
    heap_caps_free(h);
  }
  void *reallocate(void *ptr, size_t size) override {
    Hdr *old = ptr ? static_cast<Hdr *>(ptr) - 1 : nullptr;
    const size_t was = old ? old->size : 0;
    if (used_ - was + size > cap_) return nullptr;
    Hdr *h = static_cast<Hdr *>(heap_caps_malloc(sizeof(Hdr) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!h) return nullptr;
    if (old) {
      memcpy(h + 1, old + 1, was < size ? was : size);
      if (wipe_) memset(old + 1, 0, was);
      heap_caps_free(old);
    }
    h->size = size;
    used_   = used_ - was + size;
    if (used_ > peak_) peak_ = used_;
    return h + 1;
  }
  size_t peak() const { return peak_; }

 private:
  union Hdr { size_t size; max_align_t align; };
  size_t cap_, used_ = 0, peak_ = 0;
  bool   wipe_;
};

void noteHeap(Outcome &o) {
  const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (f < o.heapMin) o.heapMin = f;
}

void wipeAccess() {
  if (s_access) memset(s_access, 0, kTokenMax);
  s_accessUntil = 0;
}

int32_t headerInt(HTTPClient &http, const char *name) {
  const String v = http.header(name);
  if (!v.length()) return -1;
  return (int32_t)v.toInt();
}

// A body delivered without chunking (useHTTP10), read until the server closes,
// Content-Length is reached, or it stalls for 10 s. NUL-terminated.
size_t readBody(HTTPClient &http, uint8_t *buf, size_t cap, bool *big) {
  *big = false;
  WiFiClient *s = http.getStreamPtr();
  const int size = http.getSize();
  if (!s || cap < 2) return 0;
  if (size > 0 && (size_t)size >= cap) { *big = true; return 0; }
  size_t n = 0;
  uint32_t last = millis();
  for (;;) {
    const int avail = s->available();
    if (avail > 0) {
      if (n >= cap - 1) { *big = true; break; }
      size_t want = (size_t)avail;
      if (want > cap - 1 - n) want = cap - 1 - n;
      const int got = s->read(buf + n, want);
      if (got > 0) {
        n += (size_t)got;
        last = millis();
        if (size > 0 && n >= (size_t)size) break;
        continue;
      }
    }
    if (!s->connected() && s->available() <= 0) break;
    if (millis() - last > 10000) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  buf[n] = '\0';
  return n;
}

struct Reply {
  int      code;
  int32_t  leftDay, limitDay, leftHour, leftMinute;
  int32_t  retryS;
  size_t   len;
  bool     big;
  bool     tls;       // the connection failed inside TLS (certificate, handshake)
};

// One GET with the bearer, headers collected, body read into `buf` on 200.
// The client and its TLS session are local: both are gone when this returns.
void get(const char *url, const char *bearer, uint8_t *buf, size_t cap, Reply *r, Outcome &o) {
  memset(r, 0, sizeof(*r));
  r->leftDay = r->limitDay = r->leftHour = r->leftMinute = r->retryS = -1;
  WiFiClientSecure tls;
  tls.setCACert(kRttRootsPem);
  tls.setHandshakeTimeout(12);          // seconds; the library default is 120
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);                 // no chunked transfer: the body is read as it comes
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  if (!http.begin(tls, url)) { r->code = HTTPC_ERROR_CONNECTION_REFUSED; return; }
  static const char *kHeaders[] = {"Retry-After", "X-RateLimit-Remaining-Day", "X-RateLimit-Limit-Day",
                                   "X-RateLimit-Remaining-Hour", "X-RateLimit-Remaining-Minute"};
  http.collectHeaders(kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]));
  {
    // Built with its room reserved, so no reallocation leaves a copy behind,
    // and zeroed as soon as HTTPClient has taken its own.
    String auth;
    auth.reserve(8 + strlen(bearer));
    auth = "Bearer ";
    auth += bearer;
    http.addHeader("Authorization", auth);
    memset((char *)auth.c_str(), 0, auth.length());
  }
  http.addHeader("Accept", "application/json");
  r->code = http.GET();
  noteHeap(o);
  if (r->code < 0) {
    char e[48];
    r->tls = tls.lastError(e, sizeof(e)) != 0;
    http.end();
    return;
  }
  r->retryS     = headerInt(http, "Retry-After");
  r->leftDay    = headerInt(http, "X-RateLimit-Remaining-Day");
  r->limitDay   = headerInt(http, "X-RateLimit-Limit-Day");
  r->leftHour   = headerInt(http, "X-RateLimit-Remaining-Hour");
  r->leftMinute = headerInt(http, "X-RateLimit-Remaining-Minute");
  if (r->code == 200 && buf) {
    r->len = readBody(http, buf, cap, &r->big);
    noteHeap(o);
  }
  http.end();
}

void keepQuota(const Reply &r, Outcome &o) {
  if (r.leftDay >= 0)    o.leftDay = r.leftDay;
  if (r.limitDay >= 0)   o.limitDay = r.limitDay;
  if (r.leftHour >= 0)   o.leftHour = r.leftHour;
  if (r.leftMinute >= 0) o.leftMinute = r.leftMinute;
  if (r.retryS > 0)      o.retryS = (uint32_t)r.retryS;
}

// Anything but 200 or 204, as a state.
uint8_t failure(const Reply &r) {
  if (r.code < 0)                      return r.tls ? ST_TLS : ST_NET;
  if (r.code == 401 || r.code == 403)  return ST_AUTH;
  if (r.code == 429)                   return ST_RATE;
  return ST_HTTP;
}

// GET /api/get_access_token with the stored token. True with s_access filled.
bool exchange(const char *stored, Outcome &o) {
  uint8_t *body = static_cast<uint8_t *>(heap_caps_malloc(kTokenBodyMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!body) { o.state = ST_NOMEM; return false; }
  Reply r;
  get(kTokenUrl, stored, body, kTokenBodyMax, &r, o);
  keepQuota(r, o);
  o.http = (int16_t)r.code;
  bool ok = false;
  if (r.code == 200 && !r.big) {
    CappedPsram alloc(16 * 1024, true);
    {
      JsonDocument filter(&alloc), doc(&alloc);
      rtt::tokenFilter(filter);
      const DeserializationError e = deserializeJson(doc, (const char *)body, r.len, DeserializationOption::Filter(filter));
      int64_t until = 0;
      wipeAccess();
      if (!e && rtt::accessToken(doc.as<JsonVariantConst>(), s_access, kTokenMax, &until)) {
        const int64_t now = time(nullptr);
        s_accessUntil = until > now ? until : now + kTokenAssumeS;
        s_kind = KIND_REFRESH;
        o.validUntil = s_accessUntil;
        ok = true;
      } else {
        o.state = e == DeserializationError::NoMemory ? ST_NOMEM : ST_BAD;
      }
    }
  } else if (r.code == 401 || r.code == 403) {
    wipeAccess();
    s_kind  = KIND_REFUSED;          // the stored token itself is not accepted
    o.state = ST_AUTH;
  } else {
    o.state = r.big ? ST_BIG : failure(r);
  }
  memset(body, 0, kTokenBodyMax);    // it held the access token
  heap_caps_free(body);
  return ok;
}

void runFetch(Outcome &o) {
  char *stored = static_cast<char *>(heap_caps_calloc(1, kTokenMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t *body = static_cast<uint8_t *>(heap_caps_malloc(kBodyMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  char kind[12] = "";
  if (!stored || !body) { o.state = ST_NOMEM; goto done; }
  {
    Preferences p;
    if (p.begin(kNvsNs, true)) {
      if (p.isKey("token")) p.getString("token", stored, kTokenMax);
      if (p.isKey("kind"))  p.getString("kind", kind, sizeof(kind));
      p.end();
    }
  }
  if (!stored[0]) { o.state = ST_NOTOKEN; goto done; }
  {
    const bool cfgRefresh = !strcmp(kind, "refresh");
    const bool cfgAccess  = !strcmp(kind, "access");
    const int64_t now = time(nullptr);
    bool useAccess = s_access && s_access[0] && now < s_accessUntil - kTokenMarginS;
    if (cfgRefresh && !useAccess) {
      if (!exchange(stored, o)) goto done;
      useAccess = true;
    }

    char query[80], url[128];
    rtt::buildQuery(s_taskCrs, now, true, query, sizeof(query));
    snprintf(url, sizeof(url), "%s%s", kLocationUrl, query);

    Reply r;
    get(url, useAccess ? s_access : stored, body, kBodyMax, &r, o);
    keepQuota(r, o);
    if (r.code == 401 && !cfgAccess) {
      // The stored token as an access token, or an access token RTT no longer
      // takes: exchange once and ask again. A refused exchange ends it.
      if (useAccess) wipeAccess();
      if (!exchange(stored, o)) goto done;
      get(url, s_access, body, kBodyMax, &r, o);
      keepQuota(r, o);
      useAccess = true;
    }
    o.http = (int16_t)r.code;

    if (r.code == 204) {
      rtt::emptyLists(s_taskCrs, s_result);
      o.state = ST_OK;
    } else if (r.code == 200) {
      if (r.big) { o.state = ST_BIG; goto done; }
      o.bodyBytes = (uint32_t)r.len;
      if (!s_sample) s_sample = static_cast<char *>(heap_caps_calloc(1, kSampleMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (s_sample) {
        static const char kKey[] = "\"services\"";
        size_t from = 0;
        for (size_t at = 0; at + sizeof(kKey) - 1 <= r.len; at++)
          if (!memcmp(body + at, kKey, sizeof(kKey) - 1)) { from = at; break; }
        size_t n = r.len - from;
        if (n > kSampleMax) n = kSampleMax;
        portENTER_CRITICAL(&s_mux);
        memcpy(s_sample, body + from, n);
        s_sample[n] = '\0';
        portEXIT_CRITICAL(&s_mux);
      }
      CappedPsram alloc(kJsonCap, false);
      {
        JsonDocument filter(&alloc), doc(&alloc);
        rtt::locationFilter(filter);
        const DeserializationError e = deserializeJson(doc, (const char *)body, r.len,
                                                       DeserializationOption::Filter(filter),
                                                       DeserializationOption::NestingLimit(12));
        o.jsonPeak = (uint32_t)alloc.peak();
        noteHeap(o);
        if (e == DeserializationError::NoMemory) {
          o.state = ST_NOMEM;
        } else if (e || !rtt::transform(doc.as<JsonVariantConst>(), time(nullptr), s_taskCrs, s_result)) {
          o.state = ST_BAD;
        } else {
          o.services = s_result->seen;
          o.state = ST_OK;
          const DirectDiag dg = {s_result->seen, s_result->skipDisp, s_result->skipPax, s_result->noEvent,
                                 s_result->noSched, s_result->skipCall, s_result->skipTime,
                                 s_result->firstT, s_result->firstNow};
          portENTER_CRITICAL(&s_mux);
          s_diag = dg;
          portEXIT_CRITICAL(&s_mux);
        }
      }
    } else {
      o.state = failure(r);
    }
    if (o.state == ST_OK && !useAccess && s_kind == KIND_UNKNOWN) s_kind = KIND_ACCESS;
  }

done:
  if (stored) { memset(stored, 0, kTokenMax); heap_caps_free(stored); }
  if (body) heap_caps_free(body);
}

void fetchTask(void *) {
  Outcome o;
  memset(&o, 0, sizeof(o));
  o.leftDay = o.limitDay = o.leftHour = o.leftMinute = -1;
  memcpy(o.crs, s_taskCrs, sizeof(o.crs));
  {
    NetLockGuard net(NET_LOCK_WAIT_MS);   // released before vTaskDelete below
    o.heapBefore = o.heapMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (net.held()) runFetch(o);
    else            o.state = ST_LOWMEM;
  }
  o.kind       = s_kind;
  o.validUntil = s_kind == KIND_REFRESH ? s_accessUntil : 0;
  o.fetchedAt  = time(nullptr);
  o.stackFree  = uxTaskGetStackHighWaterMark(nullptr);
  portENTER_CRITICAL(&s_mux);
  s_done    = o;
  s_ready   = true;
  s_running = false;
  portEXIT_CRITICAL(&s_mux);
  vTaskDelete(nullptr);
}

uint32_t nextDelayS(const Outcome &o) {
  if (o.state != ST_AUTH) s_authStepS = 0;
  switch (o.state) {
  case ST_OK: {
    s_errStreak = 0;
    uint32_t d = kIntervalS;
    if (o.leftDay >= 0) {
      const int32_t floor = o.limitDay > 0 && o.limitDay / 10 > kFloorMin ? o.limitDay / 10 : kFloorMin;
      if (o.leftDay <= floor)            d = kTrickleS;
      else if (o.leftDay <= 2 * floor)   d = kSlowS;
    }
    if (o.leftHour == 0)                       d = kTrickleS;
    if (o.leftMinute == 0 && d < kRetryMinS)   d = kRetryMinS;
    return d;
  }
  case ST_AUTH:
    s_authStepS = s_authStepS ? (s_authStepS * 2 > kAuthMaxS ? kAuthMaxS : s_authStepS * 2) : kAuthFirstS;
    return s_authStepS;
  case ST_RATE:
    if (!o.retryS) return kTrickleS;
    return o.retryS < kRetryMinS ? kRetryMinS : (o.retryS > kRetryMaxS ? kRetryMaxS : o.retryS);
  case ST_NOTOKEN:
    return kTrickleS;
  default: {
    const uint8_t i = s_errStreak < 2 ? s_errStreak : 2;
    if (s_errStreak < 255) s_errStreak++;
    return kErrorS[i];
  }
  }
}

}  // namespace

void rttDirectBegin() {
  if (s_begun) return;
  s_begun = true;
  Preferences p;
  if (p.begin(kNvsNs, true)) {         // read-only; isKey() logs nothing for a missing key
    s_tokenStored = p.isKey("token");
    p.end();
  }
  s_result = static_cast<rtt::Lists *>(heap_caps_calloc(1, sizeof(rtt::Lists), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_access = static_cast<char *>(heap_caps_calloc(1, kTokenMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_result || !s_access) {
    s_tokenStored = false;
    Serial.println("[rtt] no PSRAM for the direct fetch - Home Assistant only");
    return;
  }
  Serial.printf("[rtt] direct fetch %s\n", s_tokenStored ? "armed: a token is stored" : "off: no token in NVS rb/token");
  s_nextAtMs = millis();
}

bool rttDirectHasToken() { return s_tokenStored; }

bool rttDirectAuthRefused() { return s_haveOutcome && s_last.kind == KIND_REFUSED; }

void rttDirectStationChanged() {
  if (!s_authStepS) s_nextAtMs = millis();     // a new station does not mend a refused token
}

void rttDirectOnScreen(bool onScreen) {
  if (onScreen && !s_onScreen && s_tokenStored) {
    // Coming into view: fetch now unless the last good board is fresher than a poll.
    const uint32_t nowMs = millis();
    if (!s_lastOkMs || nowMs - s_lastOkMs >= kIntervalS * 1000UL) s_nextAtMs = nowMs;
  }
  s_onScreen = onScreen;
}

bool rttDirectLoop(const char *crs, rtt::Lists *out, int64_t *fetchedAt) {
  if (!s_begun || !s_tokenStored || !s_result) return false;
  const uint32_t nowMs = millis();
  bool handed = false;

  bool running, ready;
  Outcome done;
  portENTER_CRITICAL(&s_mux);
  running = s_running;
  ready   = s_ready;
  if (ready) { done = s_done; s_ready = false; }
  portEXIT_CRITICAL(&s_mux);

  if (ready) {
    s_last = done;
    s_haveOutcome = true;
    s_polls++;
    s_lastDoneMs = nowMs;
    if (done.state == ST_OK) {
      s_lastOkMs = nowMs;
      if (!strcmp(done.crs, crs)) {      // not for a station left while it ran
        memcpy(out, s_result, sizeof(*out));
        *fetchedAt = done.fetchedAt;
        handed = true;
      } else {
        s_nextAtMs = nowMs;
      }
    } else {
      s_fails++;
    }
    if (strcmp(done.crs, crs) == 0 || done.state != ST_OK) {
      s_lastDelayS = nextDelayS(done);
      s_nextAtMs   = nowMs + s_lastDelayS * 1000UL;
    }
  }
  if (running) return handed;

  if (!s_onScreen) return handed;                                           // off screen nothing is fetched
  if ((int32_t)(nowMs - s_nextAtMs) < 0) return handed;
  if (WiFi.status() != WL_CONNECTED) { s_blocked = ST_NOWIFI; return handed; }
  if (time(nullptr) < 1700000000) { s_blocked = ST_NOCLOCK; return handed; }   // certificates need the date
  // A person at the portal beats a refresh that can wait (net_turns.h). The
  // deadline stops a browser left open from starving this for ever - counted in
  // time, because this runs on every pass of loop() and passes are free.
  { static uint32_t yieldingSince = 0;
    const uint32_t nowTurn = millis();
    if (netTurnYield(netMsSinceHttp(), yieldingSince, nowTurn)) {
      if (!yieldingSince) yieldingSince = nowTurn ? nowTurn : 1;
      return handed;
    }
    yieldingSince = 0; }
  if (netLockBusy()) { s_nextAtMs = nowMs + 2000UL; return handed; }         // another fetch is on the network
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalFree ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kStackBytes + 1024) {
    s_blocked  = ST_LOWMEM;
    s_nextAtMs = nowMs + kErrorS[0] * 1000UL;
    return handed;
  }
  s_blocked = ST_IDLE;
  memcpy(s_taskCrs, crs, sizeof(s_taskCrs));
  portENTER_CRITICAL(&s_mux);
  s_running = true;
  portEXIT_CRITICAL(&s_mux);
  if (xTaskCreatePinnedToCore(fetchTask, "rttfetch", kStackBytes, nullptr, kPriority, nullptr, kCore) != pdPASS) {
    portENTER_CRITICAL(&s_mux);
    s_running = false;
    portEXIT_CRITICAL(&s_mux);
    s_blocked  = ST_TASK;
    s_nextAtMs = nowMs + kErrorS[0] * 1000UL;
  }
  return handed;
}

void rttDirectLine(char *value, size_t valueCap, char *quota, size_t quotaCap) {
  quota[0] = '\0';
  if (!s_tokenStored) { snprintf(value, valueCap, "HA ONLY"); return; }
  if (!s_haveOutcome) {
    snprintf(value, valueCap, "DIRECT %s", s_blocked != ST_IDLE ? kStateNames[s_blocked] : "WAITING");
    return;
  }
  if (s_last.http > 0) snprintf(value, valueCap, "DIRECT %s %d", kStateNames[s_last.state], (int)s_last.http);
  else                 snprintf(value, valueCap, "DIRECT %s", kStateNames[s_last.state]);
  if (s_last.leftDay >= 0) snprintf(quota, quotaCap, "LEFT %ld", (long)s_last.leftDay);
}

void rttDirectStatusJson(JsonObject out) {
  const uint32_t nowMs = millis();
  bool running;
  portENTER_CRITICAL(&s_mux);
  running = s_running;
  portEXIT_CRITICAL(&s_mux);

  out["built"] = true;
  out["token"] = s_tokenStored;                    // stored or not; the value never leaves NVS
  out["kind"]  = kKindNames[s_haveOutcome ? s_last.kind : KIND_UNKNOWN];
  out["validUntil"] = (s_haveOutcome && s_last.validUntil > 0) ? (uint32_t)s_last.validUntil : 0;
  out["fetching"] = running;
  out["state"] = !s_tokenStored ? "NO TOKEN"
               : (s_blocked != ST_IDLE && !running) ? kStateNames[s_blocked]
               : (s_haveOutcome ? kStateNames[s_last.state] : "WAITING");
  out["polls"] = s_polls;
  out["fails"] = s_fails;
  if (!s_haveOutcome) return;
  out["http"] = s_last.http;
  out["fetchedAgo"] = (nowMs - s_lastDoneMs) / 1000UL;
  if (s_lastOkMs) out["okAgo"] = (nowMs - s_lastOkMs) / 1000UL;
  if (!running) {
    const int32_t left = (int32_t)(s_nextAtMs - nowMs);
    out["nextIn"] = left > 0 ? (uint32_t)(left / 1000) : 0;
  }
  out["interval"]   = s_lastDelayS;
  out["left"]       = s_last.leftDay;
  out["limit"]      = s_last.limitDay;
  out["leftHour"]   = s_last.leftHour;
  out["leftMinute"] = s_last.leftMinute;
  out["floor"]      = s_last.limitDay > 0 && s_last.limitDay / 10 > kFloorMin ? s_last.limitDay / 10 : kFloorMin;
  out["retry"]      = s_last.retryS;
  out["bytes"]      = s_last.bodyBytes;
  out["services"]   = s_last.services;
  out["jsonPeak"]   = s_last.jsonPeak;
  out["heapBefore"] = s_last.heapBefore;
  out["heapMin"]    = s_last.heapMin;
  out["stackFree"]  = s_last.stackFree;

  DirectDiag dg;
  char sample[kSampleMax + 1];
  portENTER_CRITICAL(&s_mux);
  dg = s_diag;
  if (s_sample) memcpy(sample, s_sample, sizeof(sample));
  else          sample[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
  JsonObject sk = out["skips"].to<JsonObject>();
  sk["seen"]          = dg.seen;
  sk["notCall"]       = dg.disp;       // displayAs null or PASS
  sk["notPassenger"]  = dg.pax;
  sk["noEvent"]       = dg.noEvent;    // no arrival/departure object
  sk["noAdvertised"]  = dg.noSched;    // no scheduleAdvertised
  sk["callType"]      = dg.call;
  sk["outsideWindow"] = dg.time;
  if (dg.firstT) { sk["firstSched"] = (long long)dg.firstT; sk["now"] = (long long)dg.firstNow; }
  out["sample"] = (const char *)sample;
}

#endif  // RAILBOARD_DIRECT_ENABLED

#include "aero_direct.h"
#include "../util/psram_state.h"

#if defined(FLIGHTBOARD_DIRECT_ENABLED)

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <atomic>

#include "../net/net_broker.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>

#include "aero_roots.h"
#include "../network/net_lock.h"
#include "../network/net_turns.h"
#include "../network/network.h"

namespace {

// The spec's only server: https://{env}.flightaware.com/aeroapi, env "aeroapi";
// the key goes in the x-apikey header (components.securitySchemes.ApiKeyAuth).
const char *const kBase  = "https://aeroapi.flightaware.com/aeroapi";
const char *const kKeyNs = "aero";
const char *const kUseNs = "fbuse";
const char *const kCfgNs = "fbcfg";

// ── timing ──────────────────────────────────────────────────────────────────
// Choices, not FlightAware's numbers (README "Choices"). No failure is ever
// retried sooner than a minute, and none of them is free of the day's cap.
const uint32_t kErrorS[]      = {60, 300, 900, 3600};   // NET, TLS, HTTP, BAD, BIG, NOMEM, in a row
const uint32_t kAuthFirstS    = 3600;                   // a refused key: 1 h, doubling ...
const uint32_t kAuthMaxS      = 24 * 3600;              // ... to a day
const uint32_t kRetryMinS     = 60;                     // 429 with Retry-After, clamped to this ...
const uint32_t kRetryMaxS     = 3600;                   // ... and this
const uint32_t kRateBareS     = 900;                    // 429 without it
const uint32_t kRefusedListS  = 6 * 3600;               // 400 or 404 for an airport's list
const uint32_t kRefusedTrackS = 24 * 3600;              // 400 or 404 for an ident
const uint32_t kLowMemS       = 15;                     // no call was made: look again soon
const uint32_t kNvsFailS      = 60;                     // the counter could not be kept: no call
const uint32_t kSettleMs      = 2500;                   // budget and trackers reach NVS after this

// ── memory ──────────────────────────────────────────────────────────────────
// As the rail board's direct fetch: the stack is internal RAM and exists only
// while a call runs; TLS (tlsUsePsram), the body and the JSON are PSRAM.
// One page is at most 15 records. A synthetic record with every field the
// spec lists is 1.9 KB indented (tools/flightboard/samples), a real one Home
// Assistant held 2.4 KB; 15 of those are 36 KB. 192 KB leaves room for long
// route strings and indentation, and the filtered parse of 16 records peaked
// at 15 KB on a 64-bit host. The real sizes are in /api/flightboard.
const uint32_t    kStackBytes      = 12 * 1024;
// Below the Lua effect task, for the same reason as the rail board's fetch.
const UBaseType_t kPriority        = 0;
const BaseType_t  kCore            = 0;
const size_t      kMinInternalFree = 28 * 1024;
const size_t      kBodyMax         = 192 * 1024;
const size_t      kJsonCap         = 64 * 1024;
const size_t      kKeyMax          = 256;       // the key's length is not documented; this is room
const size_t      kSampleMax       = 900;

enum State : uint8_t {
  ST_IDLE = 0, ST_OK, ST_NOKEY, ST_NOCLOCK, ST_NOWIFI, ST_LOWMEM, ST_TASK, ST_CAP, ST_NVS,
  ST_AUTH, ST_RATE, ST_REFUSED, ST_NET, ST_TLS, ST_HTTP, ST_BAD, ST_BIG, ST_NOMEM, ST_COUNT
};
const char *const kStateNames[ST_COUNT] = {
  "IDLE", "OK", "NO KEY", "NO CLOCK", "NO WIFI", "LOW MEM", "TASK", "CAP", "NVS",
  "AUTH", "RATE", "REFUSED", "NET", "TLS", "HTTP", "BAD", "BIG", "NOMEM"
};

enum JobKind : uint8_t { JOB_LIST = 0, JOB_TRACK };

struct Job {
  uint8_t kind;
  uint8_t list;                 // aero::List for JOB_LIST
  uint8_t slot;                 // tracker slot for JOB_TRACK
  char    icao[5];
  char    ident[FB_IDENT_LEN];
  int64_t now;
  int64_t added;
};

struct Outcome {
  uint8_t  state;
  int16_t  http;
  uint32_t retryS;
  uint32_t bodyBytes, jsonPeak, heapBefore, heapMin, stackFree;
  bool     more;
  Job      job;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool     s_running = false;             // under s_mux
// The broker path's own bookkeeping, loop task only.
bool     s_nbWaiting = false;
uint32_t s_nbSeq = 0;
bool     s_ready   = false;             // under s_mux
Outcome  s_done;                        // under s_mux
Job      s_job;                         // written by the loop before the task starts, read by the task
aero::ListResult *s_tmpList = nullptr;  // PSRAM; the task writes it, the loop reads it once s_ready
aero::Track       s_tmpTrack;           // the same
char    *s_sample = nullptr;            // PSRAM; the start of the last answer's array, under s_mux

struct ListCache {
  aero::ListResult *r;                  // PSRAM
  bool     have;
  uint32_t atMs;
  uint32_t holdUntilMs;                 // 0 = none
};
ListCache s_lists[aero::LIST_COUNT];
char      s_listIcao[5] = "";
uint32_t  s_gen = 0;

PSRAM_ARRAY(AeroTracker, s_trk, [FB_TRACK_MAX]);
uint8_t     s_trkN = 0;

fbs::Budget s_budget = fbs::defaults();
bool        s_budgetStored = false;
uint32_t    s_cfgDirtyAt = 0;

fbs::Usage s_use = {};

bool     s_keyStored = false;
bool     s_begun = false;
uint32_t s_lastStartMs = 0;
bool     s_started = false;
uint32_t s_holdUntilMs = 0;             // 0 = none
uint8_t  s_errStreak = 0;
uint32_t s_authStepS = 0;
uint32_t s_calls = 0, s_fails = 0;
bool     s_haveLast = false;
Outcome  s_last;
uint32_t s_lastDoneMs = 0;
uint8_t  s_blocked = ST_IDLE;
const char *s_capWhy = "";

int64_t nowUtc() { return (int64_t)time(nullptr); }
bool    clockSet() { return nowUtc() >= 1700000000; }

void markCfg() { s_cfgDirtyAt = millis() | 1; }

bool held(uint32_t untilMs, uint32_t nowMs) { return untilMs && (int32_t)(nowMs - untilMs) < 0; }

// ── NVS ─────────────────────────────────────────────────────────────────────
struct __attribute__((packed)) UseRecord {
  uint8_t  v;
  uint32_t day, month;
  uint16_t dayN, monthN;
  uint32_t total;
};
const uint8_t kUseV = 1;

struct __attribute__((packed)) TrkRecord {
  uint8_t  v;
  char     ident[FB_IDENT_LEN];
  uint32_t added;
};
const uint8_t kTrkV = 1;

// Written before the call it counts, and the call is not made if it does not
// go in: a counter that a reboot would forget is a cap that does not hold.
bool saveUse() {
  Preferences p;
  if (!p.begin(kUseNs, false)) return false;
  const UseRecord r = {kUseV, s_use.day, s_use.month, s_use.dayN, s_use.monthN, s_use.total};
  const bool ok = p.putBytes("u", &r, sizeof(r)) == sizeof(r);
  p.end();
  return ok;
}

void loadNvs() {
  Preferences p;
  // Read-write opens: a read-only open of a namespace never written logs an
  // error on every boot (src/panel/panel.cpp, arduino-esp32 2.0.17). isKey()
  // before any getter that logs a missing key.
  if (p.begin(kKeyNs, false)) {
    s_keyStored = p.isKey("key") && p.getString("key", "").length() > 0;
    p.end();
  }
  if (p.begin(kUseNs, false)) {
    UseRecord r;
    if (p.isKey("u") && p.getBytesLength("u") == sizeof(r) && p.getBytes("u", &r, sizeof(r)) == sizeof(r) && r.v == kUseV)
      s_use = {r.day, r.month, r.dayN, r.monthN, r.total};
    p.end();
  }
  if (p.begin(kCfgNs, false)) {
    fbs::Budget b = fbs::defaults();
    if (p.isKey("floor")) {
      b.floorMin = p.getUShort("floor", b.floorMin);
      b.dayCap   = p.getUShort("day", b.dayCap);
      b.monthCap = p.getUShort("mon", b.monthCap);
      if (fbs::valid(b)) { s_budget = b; s_budgetStored = true; }
    }
    for (uint8_t i = 0; i < FB_TRACK_MAX; i++) {
      char key[6];
      snprintf(key, sizeof(key), "trk%u", (unsigned)i);
      TrkRecord r;
      if (!p.isKey(key) || p.getBytesLength(key) != sizeof(r) || p.getBytes(key, &r, sizeof(r)) != sizeof(r) || r.v != kTrkV)
        continue;
      r.ident[FB_IDENT_LEN - 1] = '\0';
      char id[FB_IDENT_LEN];
      if (!aero::normaliseIdent(r.ident, id, sizeof(id)) || s_trkN >= FB_TRACK_MAX) continue;
      AeroTracker &t = s_trk[s_trkN++];
      memset(&t, 0, sizeof(t));
      memcpy(t.ident, id, sizeof(t.ident));
      t.added = r.added;
    }
    p.end();
  }
}

// Budget and trackers, once changes stop arriving. All three tracker slots are
// rewritten so a removed one never lingers under a higher key.
void saveCfg() {
  if (!s_cfgDirtyAt || millis() - s_cfgDirtyAt < kSettleMs) return;
  Preferences p;
  if (!p.begin(kCfgNs, false)) { markCfg(); return; }
  s_cfgDirtyAt = 0;
  bool ok = true;
  ok &= p.putUShort("floor", s_budget.floorMin) && p.putUShort("day", s_budget.dayCap) && p.putUShort("mon", s_budget.monthCap);
  for (uint8_t i = 0; i < FB_TRACK_MAX; i++) {
    char key[6];
    snprintf(key, sizeof(key), "trk%u", (unsigned)i);
    if (i < s_trkN) {
      TrkRecord r = {};
      r.v = kTrkV;
      memcpy(r.ident, s_trk[i].ident, sizeof(r.ident));
      r.added = s_trk[i].added;
      ok &= p.putBytes(key, &r, sizeof(r)) == sizeof(r);
    } else if (p.isKey(key)) {          // remove() logs an error for a key that is not there
      ok &= p.remove(key);
    }
  }
  p.end();
  if (!ok) markCfg();
}

// ── one call ────────────────────────────────────────────────────────────────
class CappedPsram : public ArduinoJson::Allocator {
 public:
  explicit CappedPsram(size_t cap) : cap_(cap) {}
  void *allocate(size_t size) override { return reallocate(nullptr, size); }
  void deallocate(void *ptr) override {
    if (!ptr) return;
    Hdr *h = static_cast<Hdr *>(ptr) - 1;
    used_ -= h->size;
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
};

void noteHeap(Outcome &o) {
  const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (f < o.heapMin) o.heapMin = f;
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

void keepSample(const uint8_t *body, size_t len, const char *arrayKey) {
  if (!s_sample) s_sample = static_cast<char *>(heap_caps_calloc(1, kSampleMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_sample) return;
  char key[32];
  snprintf(key, sizeof(key), "\"%s\"", arrayKey);
  const size_t kl = strlen(key);
  size_t from = 0;
  for (size_t at = 0; at + kl <= len; at++)
    if (!memcmp(body + at, key, kl)) { from = at; break; }
  size_t n = len - from;
  if (n > kSampleMax) n = kSampleMax;
  portENTER_CRITICAL(&s_mux);
  memcpy(s_sample, body + from, n);
  s_sample[n] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

// Everything from "the body is in a buffer" onwards, shared by the broker path
// and the fallback task. It runs on whichever of them is doing the work - on
// the broker path that is the loop task, where a JSON parse is an ordinary
// loop-side cost and the broker's own stack never has to be big enough for it.
void consumeBody(Outcome &o, const Job &j, const char *body, size_t len) {
  const char *arrayKey = j.kind == JOB_LIST ? aero::listName((aero::List)j.list) : "flights";
  keepSample((const uint8_t *)body, len, arrayKey);
  CappedPsram alloc(kJsonCap);
  JsonDocument filter(&alloc), doc(&alloc);
  if (j.kind == JOB_LIST) aero::listFilter(filter, (aero::List)j.list);
  else                    aero::trackFilter(filter);
  const DeserializationError e = deserializeJson(doc, body, len,
                                                 DeserializationOption::Filter(filter),
                                                 DeserializationOption::NestingLimit(10));
  o.jsonPeak = (uint32_t)alloc.peak();
  noteHeap(o);
  if (e == DeserializationError::NoMemory) {
    o.state = ST_NOMEM;
  } else if (e) {
    o.state = ST_BAD;
  } else if (j.kind == JOB_LIST) {
    o.state = aero::parseList(doc.as<JsonVariantConst>(), (aero::List)j.list, s_tmpList) ? ST_OK : ST_BAD;
    o.more = s_tmpList->more;
  } else {
    o.state = aero::pickTrack(doc.as<JsonVariantConst>(), j.now, j.added, &s_tmpTrack) ? ST_OK : ST_BAD;
  }
}

// The key and the URL, built where the request is submitted rather than inside
// the fetch. Returns false with o.state set when there is nothing to ask for.
// `key` is the caller's buffer and the caller zeroes it the moment the broker
// has taken its copy.
bool buildRequest(const Job &j, Outcome &o, char *url, size_t urlCap, char *key, size_t keyCap) {
  key[0] = '\0';
  {
    Preferences p;
    if (p.begin(kKeyNs, true)) {
      if (p.isKey("key")) p.getString("key", key, keyCap);
      p.end();
    }
  }
  if (!key[0]) { o.state = ST_NOKEY; return false; }
  char path[200];
  const bool built = j.kind == JOB_LIST
      ? aero::listQuery(j.icao, (aero::List)j.list, j.now, path, sizeof(path))
      : aero::trackQuery(j.ident, j.now, path, sizeof(path));
  if (!built || snprintf(url, urlCap, "%s%s", kBase, path) >= (int)urlCap) {
    o.state = ST_BAD;
    memset(key, 0, keyCap);
    return false;
  }
  return true;
}

void runCall(Outcome &o) {
  const Job &j = o.job;
  char *key = static_cast<char *>(heap_caps_calloc(1, kKeyMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t *body = static_cast<uint8_t *>(heap_caps_malloc(kBodyMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!key || !body) {
    o.state = ST_NOMEM;
  } else {
    {
      Preferences p;
      if (p.begin(kKeyNs, true)) {
        if (p.isKey("key")) p.getString("key", key, kKeyMax);
        p.end();
      }
    }
    char path[200], url[260];
    const bool built = j.kind == JOB_LIST ? aero::listQuery(j.icao, (aero::List)j.list, j.now, path, sizeof(path))
                                          : aero::trackQuery(j.ident, j.now, path, sizeof(path));
    if (!key[0]) {
      o.state = ST_NOKEY;
    } else if (!built || snprintf(url, sizeof(url), "%s%s", kBase, path) >= (int)sizeof(url)) {
      o.state = ST_BAD;
    } else {
      WiFiClientSecure tls;
      tls.setCACert(kAeroRootsPem);
      tls.setHandshakeTimeout(12);        // seconds; the library default is 120
      HTTPClient http;
      http.setReuse(false);
      http.useHTTP10(true);               // no chunked transfer: the body is read as it comes
      http.setConnectTimeout(8000);
      http.setTimeout(12000);
      if (!http.begin(tls, url)) {
        o.state = ST_NET;
      } else {
        static const char *kHeaders[] = {"Retry-After"};
        http.collectHeaders(kHeaders, 1);
        {
          // Room reserved first, so no reallocation leaves a copy behind, and
          // zeroed once HTTPClient has taken its own.
          String k;
          k.reserve(strlen(key) + 1);
          k = key;
          http.addHeader("x-apikey", k);
          memset((char *)k.c_str(), 0, k.length());
        }
        memset(key, 0, kKeyMax);
        http.addHeader("Accept", "application/json");
        const int code = http.GET();
        noteHeap(o);
        o.http = (int16_t)code;
        if (code < 0) {
          char e[48];
          o.state = tls.lastError(e, sizeof(e)) != 0 ? ST_TLS : ST_NET;
        } else if (code == 200) {
          bool big = false;
          const size_t len = readBody(http, body, kBodyMax, &big);
          noteHeap(o);
          o.bodyBytes = (uint32_t)len;
          if (big) o.state = ST_BIG;
          else     consumeBody(o, j, (const char *)body, len);
        } else {
          const String ra = http.header("Retry-After");
          o.retryS = ra.length() ? (uint32_t)ra.toInt() : 0;
          o.state = (code == 401 || code == 403) ? ST_AUTH
                  : code == 429                  ? ST_RATE
                  : (code == 400 || code == 404) ? ST_REFUSED
                                                 : ST_HTTP;
        }
        http.end();
      }
    }
  }
  if (key) { memset(key, 0, kKeyMax); heap_caps_free(key); }
  if (body) heap_caps_free(body);
}

void callTask(void *) {
  Outcome o;
  memset(&o, 0, sizeof(o));
  o.job = s_job;
  {
    NetLockGuard net(NET_LOCK_WAIT_MS);   // released before vTaskDelete below
    o.heapBefore = o.heapMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (net.held()) runCall(o);
    else            o.state = ST_LOWMEM;
  }
  o.stackFree = uxTaskGetStackHighWaterMark(nullptr);
  portENTER_CRITICAL(&s_mux);
  s_done    = o;
  s_ready   = true;
  s_running = false;
  portEXIT_CRITICAL(&s_mux);
  vTaskDelete(nullptr);
}

// ── the loop's side ─────────────────────────────────────────────────────────
void dropLists() {
  for (ListCache &c : s_lists) { c.have = false; c.holdUntilMs = 0; }
  s_gen++;
}

int findTracker(const char *ident) {
  for (uint8_t i = 0; i < s_trkN; i++)
    if (!strcmp(s_trk[i].ident, ident)) return i;
  return -1;
}

void removeAt(uint8_t i) {
  for (uint8_t k = i; k + 1 < s_trkN; k++) s_trk[k] = s_trk[k + 1];
  s_trkN--;
  markCfg();
}

void take(const Outcome &o, uint32_t nowMs) {
  s_last = o;
  s_haveLast = true;
  s_lastDoneMs = nowMs;
  if (o.state != ST_OK) s_fails++;

  // Where the answer goes.
  if (o.job.kind == JOB_LIST) {
    ListCache &c = s_lists[o.job.list];
    const bool same = !strcmp(o.job.icao, s_listIcao);    // not for an airport left while it ran
    if (o.state == ST_OK && same) {
      memcpy(c.r, s_tmpList, sizeof(*c.r));
      c.have = true;
      c.atMs = nowMs;
      s_gen++;
    } else if (o.state == ST_REFUSED && same) {
      c.holdUntilMs = nowMs + kRefusedListS * 1000UL;
    }
  } else {
    const int i = findTracker(o.job.ident);             // removed while it ran: dropped
    if (i >= 0) {
      AeroTracker &t = s_trk[i];
      t.http = o.http;
      const int64_t now = nowUtc();
      if (o.state == ST_OK) {
        t.t = s_tmpTrack;
        t.fetchedMs = nowMs;
        const uint32_t next = aero::trackNextS(t.t, now);
        t.nextAt = next ? now + next : INT64_MAX;
      } else if (o.state == ST_REFUSED) {
        t.nextAt = now + kRefusedTrackS;
      } else {
        t.nextAt = 0;                                   // the global wait below decides when
      }
    }
  }

  // When anything is asked next.
  if (o.state != ST_AUTH) s_authStepS = 0;
  switch (o.state) {
  case ST_OK:
  case ST_REFUSED:
    s_errStreak = 0;
    s_holdUntilMs = 0;
    break;
  case ST_AUTH:
    s_authStepS = s_authStepS ? (s_authStepS * 2 > kAuthMaxS ? kAuthMaxS : s_authStepS * 2) : kAuthFirstS;
    s_holdUntilMs = nowMs + s_authStepS * 1000UL;
    break;
  case ST_RATE: {
    uint32_t s = o.retryS ? o.retryS : kRateBareS;
    s = s < kRetryMinS ? kRetryMinS : (s > kRetryMaxS ? kRetryMaxS : s);
    s_holdUntilMs = nowMs + s * 1000UL;
    break;
  }
  case ST_NOKEY:
    s_keyStored = false;                                // erased since boot: stop until a reboot
    break;
  default: {
    const uint8_t i = s_errStreak < 3 ? s_errStreak : 3;
    if (s_errStreak < 255) s_errStreak++;
    s_holdUntilMs = nowMs + kErrorS[i] * 1000UL;
    break;
  }
  }
  s_holdUntilMs |= s_holdUntilMs ? 1 : 0;
}

// The next call worth making, or false. Trackers first: a take-off cannot wait.
bool nextJob(const AeroWant &w, int64_t now, uint32_t nowMs, Job *j, bool *board) {
  memset(j, 0, sizeof(*j));
  j->now = now;
  // Nothing, tracked flights included, is called while the page is off the
  // panel (the owner's brief, 2026-09-14).
  if (!w.visible) return false;
  for (uint8_t i = 0; i < s_trkN; i++) {
    if (s_trk[i].nextAt > now) continue;
    j->kind = JOB_TRACK;
    j->slot = i;
    memcpy(j->ident, s_trk[i].ident, sizeof(j->ident));
    j->added = s_trk[i].added ? s_trk[i].added : now;
    *board = false;
    return true;
  }
  if (!w.icao || !w.icao[0]) return false;
  // Coming flights before past ones: most of the rows on screen are ahead of now.
  static const aero::List kOrder[] = {aero::ARR_NEXT, aero::DEP_NEXT, aero::ARR_PAST, aero::DEP_PAST};
  for (aero::List l : kOrder) {
    if (aero::listDeparts(l) ? !w.dep : !w.arr) continue;
    ListCache &c = s_lists[l];
    if (held(c.holdUntilMs, nowMs)) continue;
    c.holdUntilMs = 0;                          // run out: cleared, as the global wait is
    const uint32_t floorMs = (uint32_t)s_budget.floorMin * 60000UL * (aero::listPast(l) ? 2 : 1);
    if (c.have && nowMs - c.atMs < floorMs) continue;
    j->kind = JOB_LIST;
    j->list = l;
    memcpy(j->icao, w.icao, sizeof(j->icao) - 1);
    *board = true;
    return true;
  }
  return false;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
void aeroDirectBegin() {
  if (s_begun) return;
  for (ListCache &c : s_lists) {
    c.r = static_cast<aero::ListResult *>(heap_caps_calloc(1, sizeof(aero::ListResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!c.r) { Serial.println("[aero] no PSRAM for the direct fetch"); return; }
  }
  s_tmpList = static_cast<aero::ListResult *>(heap_caps_calloc(1, sizeof(aero::ListResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_tmpList) { Serial.println("[aero] no PSRAM for the direct fetch"); return; }
  loadNvs();
  s_begun = true;
  Serial.printf("[aero] direct fetch %s; %u tracked; budget %u/day %u/month, floor %u min\n",
                s_keyStored ? "armed: a key is stored" : "off: no key in NVS aero/key", (unsigned)s_trkN,
                (unsigned)s_budget.dayCap, (unsigned)s_budget.monthCap, (unsigned)s_budget.floorMin);
}

bool aeroDirectHasKey() { return s_begun && s_keyStored; }

// When this starter first stood aside for someone at the portal, 0 if it is not
// waiting. File scope so the early returns above can clear it: the deadline
// measures time since the FIRST standing aside, so a pass that left for another
// reason - the page off screen, the link down - would otherwise come back with
// the deadline already spent and skip its first yield.
static uint32_t s_aeroYieldingSince = 0;

void aeroDirectLoop(const AeroWant &w) {
  if (!s_begun) return;
  saveCfg();
  const uint32_t nowMs = millis();
  if (w.icao && strcmp(w.icao, s_listIcao)) {
    strncpy(s_listIcao, w.icao, sizeof(s_listIcao) - 1);
    s_listIcao[sizeof(s_listIcao) - 1] = '\0';
    dropLists();
  }

  // The broker path's answer. It arrives in a PSRAM mailbox and is turned into
  // an Outcome here, on the loop task, so everything below - take(), the
  // budget, the trackers - is reached by exactly the same road as before.
  if (s_nbWaiting) {
    NbMailbox *mb = nbMailbox(NB_FLIGHT);
    if (mb) {
      const uint32_t seq = mb->seq.load(std::memory_order_acquire);
      if (seq != s_nbSeq) {
        s_nbSeq = seq;
        s_nbWaiting = false;
        Outcome o;
        memset(&o, 0, sizeof(o));
        o.job = s_job;
        o.heapBefore = o.heapMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        o.http = (int16_t)mb->code;
        o.bodyBytes = mb->bodyLen;
        o.retryS = mb->header[0][0] ? (uint32_t)atoi(mb->header[0]) : 0;
        if (mb->code == 200 && mb->bodyLen) {
          consumeBody(o, s_job, mb->body, mb->bodyLen);
        } else if (mb->code == NB_ERR_TRUNC) {
          o.state = ST_BIG;
        } else if (mb->code < 0) {
          o.state = (mb->code == NB_ERR_NO_TURN) ? ST_LOWMEM : ST_NET;
        } else {
          o.state = (mb->code == 401 || mb->code == 403) ? ST_AUTH
                  : mb->code == 429                      ? ST_RATE
                  : (mb->code == 400 || mb->code == 404) ? ST_REFUSED
                                                         : ST_HTTP;
        }
        // The loop task's high-water: that is where the parse runs now, so it
        // is the stack that can get tight. Reported under the same name so
        // /api/flightboard keeps answering the question it always answered.
        o.stackFree = uxTaskGetStackHighWaterMark(nullptr);
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
        take(o, nowMs);
      }
    }
  }

  bool running, ready;
  Outcome done;
  portENTER_CRITICAL(&s_mux);
  running = s_running;
  ready   = s_ready;
  if (ready) { done = s_done; s_ready = false; }
  portEXIT_CRITICAL(&s_mux);
  if (ready) take(done, nowMs);

  const int64_t now = nowUtc();
  if (clockSet()) {
    for (uint8_t i = 0; i < s_trkN;) {
      const AeroTracker &t = s_trk[i];
      if (!t.added) { s_trk[i].added = (uint32_t)now; markCfg(); }
      const int64_t exp = t.t.state != FB_TRK_WAIT ? aero::trackExpiresAt(t.t) : 0;
      if (exp && now >= exp) removeAt(i);
      else i++;
    }
  }

  // A wait that has run out is cleared, so that millis() wrapping 49 days on
  // cannot make it look active again.
  if (s_holdUntilMs && !held(s_holdUntilMs, nowMs)) s_holdUntilMs = 0;

  // Each of these is "not the moment to fetch at all", so any standing aside is
  // forgotten with them: the deadline measures time since the FIRST yield, and
  // a pass that left here would otherwise come back with it already spent.
  if (!s_keyStored) { s_blocked = ST_NOKEY; s_aeroYieldingSince = 0; return; }
  if (running) return;
  if (s_started && nowMs - s_lastStartMs < fbs::kCallSpacingS * 1000UL) return;
  if (held(s_holdUntilMs, nowMs)) return;
  if (WiFi.status() != WL_CONNECTED) { s_blocked = ST_NOWIFI; s_aeroYieldingSince = 0; return; }
  if (!clockSet()) { s_blocked = ST_NOCLOCK; s_aeroYieldingSince = 0; return; }   // certificates, windows and caps need the date
  // A person at the portal beats a refresh that can wait (net_turns.h). The
  // deadline stops a browser left open from starving this for ever - counted in
  // time, because this runs on every pass of loop() and passes are free.
  { const uint32_t nowTurn = millis();
    if (netTurnYield(netMsSinceHttp(), s_aeroYieldingSince, nowTurn)) {
      if (!s_aeroYieldingSince) s_aeroYieldingSince = nowTurn ? nowTurn : 1;
      return;
    }
    s_aeroYieldingSince = 0; }
  if (netLockBusy()) { s_holdUntilMs = (nowMs + 2000UL) | 1; return; }   // another fetch is on the network; nothing counted

  Job j;
  bool board = false;
  if (!nextJob(w, now, nowMs, &j, &board)) { s_blocked = ST_IDLE; return; }
  fbs::roll(s_use, now);
  if (const char *why = fbs::refuse(s_use, s_budget, board, s_trkN > 0)) {
    s_blocked = ST_CAP;
    s_capWhy = why;
    // A tracker is looked at again in an hour rather than every pass; the
    // board simply stays as it is until the day or the budget changes.
    if (j.kind == JOB_TRACK) s_trk[j.slot].nextAt = now + 3600;
    return;
  }
  // Only the fallback path needs a contiguous block; the broker has no task to
  // create. This is the 13,312 B threshold that the measurements of 2026-09-21
  // kept running into, and migrating is what removes it rather than easing it.
  if (!nbReady(NB_FLIGHT) &&
      (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalFree ||
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kStackBytes + 1024)) {
    s_blocked = ST_LOWMEM;
    s_holdUntilMs = (nowMs + kLowMemS * 1000UL) | 1;
    return;
  }

  // Counted and kept before the call exists.
  const fbs::Usage before = s_use;
  s_use.dayN++;
  s_use.monthN++;
  s_use.total++;
  if (!saveUse()) {
    s_use = before;
    s_blocked = ST_NVS;
    s_holdUntilMs = (nowMs + kNvsFailS * 1000UL) | 1;
    return;
  }
  s_blocked = ST_IDLE;
  s_calls++;
  s_lastStartMs = nowMs;
  s_started = true;
  s_job = j;
  portENTER_CRITICAL(&s_mux);
  s_running = true;
  portEXIT_CRITICAL(&s_mux);
  if (nbReady(NB_FLIGHT)) {
    // No task, so no contiguous 13 KB to find at a moment nobody chose - which
    // is what this board's own guard above was there to check for, and why it
    // is skipped on this path.
    char url[260];
    char key[kKeyMax];
    Outcome pre;
    memset(&pre, 0, sizeof(pre));
    pre.job = j;
    if (!buildRequest(j, pre, url, sizeof(url), key, sizeof(key))) {
      portENTER_CRITICAL(&s_mux);
      s_running = false;
      portEXIT_CRITICAL(&s_mux);
      take(pre, nowMs);          // the call stays counted: never undercount
      return;
    }
    NbRequest req = {};
    req.url = url;
    req.auth = key;
    req.authHeader = "x-apikey";   // AeroAPI's own scheme, not Authorization
    req.caCert = kAeroRootsPem;    // this board verifies its server; do not drop it
    req.collect[0] = "Retry-After";
    req.collectCount = 1;
    req.timeoutMs = 12000;
    const bool sent = nbSubmitRequest(NB_FLIGHT, req, false);
    memset(key, 0, sizeof key);    // the broker has its own copy now
    if (sent) {
      s_nbWaiting = true;
    } else {
      portENTER_CRITICAL(&s_mux);
      s_running = false;
      portEXIT_CRITICAL(&s_mux);
      s_blocked = ST_TASK;
      s_holdUntilMs = (nowMs + kErrorS[0] * 1000UL) | 1;
    }
  } else if (xTaskCreatePinnedToCore(callTask, "aerocall", kStackBytes, nullptr, kPriority, nullptr, kCore) != pdPASS) {
    portENTER_CRITICAL(&s_mux);
    s_running = false;
    portEXIT_CRITICAL(&s_mux);
    s_blocked = ST_TASK;                                   // the call stays counted: never undercount
    s_holdUntilMs = (nowMs + kErrorS[0] * 1000UL) | 1;
  }
}

const aero::ListResult *aeroDirectList(aero::List l, uint32_t *ageS) {
  if (l >= aero::LIST_COUNT || !s_lists[l].have) return nullptr;
  if (ageS) *ageS = (millis() - s_lists[l].atMs) / 1000UL;
  return s_lists[l].r;
}

uint32_t aeroDirectListGen() { return s_gen; }

void aeroDirectAirportChanged() {
  s_listIcao[0] = '\0';        // the next loop pass takes the new airport and drops the lists
}

const char *aeroDirectState() {
  static char buf[20];
  if (!s_begun || !s_keyStored) return "NO KEY";
  bool running;
  portENTER_CRITICAL(&s_mux);
  running = s_running;
  portEXIT_CRITICAL(&s_mux);
  if (running) return "FETCHING";
  if (s_blocked == ST_CAP) return s_capWhy;
  if (s_blocked != ST_IDLE) return kStateNames[s_blocked];
  if (s_haveLast && s_last.state != ST_OK) {
    if (s_last.http > 0) snprintf(buf, sizeof(buf), "%s %d", kStateNames[s_last.state], (int)s_last.http);
    else                 snprintf(buf, sizeof(buf), "%s", kStateNames[s_last.state]);
    return buf;
  }
  return "WAITING";
}

const fbs::Budget &aeroDirectBudget() { return s_budget; }

void aeroDirectSetBudget(const fbs::Budget &b) {
  if (!fbs::valid(b)) return;
  if (b.floorMin == s_budget.floorMin && b.dayCap == s_budget.dayCap && b.monthCap == s_budget.monthCap && s_budgetStored)
    return;
  s_budget = b;
  s_budgetStored = true;
  markCfg();
  // A tracker parked on a cap is worth looking at again under the new one.
  for (uint8_t i = 0; i < s_trkN; i++)
    if (s_trk[i].nextAt != INT64_MAX && s_trk[i].t.state == FB_TRK_WAIT) s_trk[i].nextAt = 0;
}

uint8_t aeroTrackCount() { return s_trkN; }

const AeroTracker *aeroTrack(uint8_t i) { return i < s_trkN ? &s_trk[i] : nullptr; }

const char *aeroTrackAdd(const char *ident, bool *full) {
  *full = false;
  char id[FB_IDENT_LEN];
  if (!aero::normaliseIdent(ident, id, sizeof(id)))
    return "track.ident must be an airline code and a flight number, such as AF7301 or BAW336";
  if (findTracker(id) >= 0) { *full = true; return "that flight is already tracked"; }
  if (s_trkN >= FB_TRACK_MAX) { *full = true; return "three flights are tracked already: remove one first"; }
  AeroTracker &t = s_trk[s_trkN++];
  memset(&t, 0, sizeof(t));
  memcpy(t.ident, id, sizeof(t.ident));
  t.added = clockSet() ? (uint32_t)nowUtc() : 0;
  t.nextAt = 0;
  markCfg();
  return nullptr;
}

bool aeroTrackRemove(const char *ident) {
  const int i = findTracker(ident);
  if (i < 0) return false;
  removeAt((uint8_t)i);
  return true;
}

void aeroDirectStatusJson(JsonObject out) {
  const uint32_t nowMs = millis();
  const int64_t now = nowUtc();
  bool running;
  portENTER_CRITICAL(&s_mux);
  running = s_running;
  portEXIT_CRITICAL(&s_mux);

  out["built"]    = true;
  out["key"]      = s_keyStored;              // stored or not; the key never leaves NVS
  out["state"]    = aeroDirectState();
  out["fetching"] = running;
  out["calls"]    = s_calls;                  // since boot
  out["fails"]    = s_fails;
  if (held(s_holdUntilMs, nowMs)) out["waitS"] = (s_holdUntilMs - nowMs) / 1000UL;

  fbs::toJson(s_budget, out["budget"].to<JsonObject>());
  fbs::boundsJson(out["bounds"].to<JsonObject>());

  fbs::Usage u = s_use;
  const bool dated = fbs::roll(u, now);
  JsonObject use = out["usage"].to<JsonObject>();
  use["today"] = dated ? u.dayN : 0;
  use["month"] = dated ? u.monthN : 0;
  use["total"] = u.total;
  use["boardDayCap"] = fbs::boardDayCap(s_budget, s_trkN > 0);
  use["usdPerCall"]  = fbs::kMicroUsdPerCall / 1e6;
  use["usdToday"]    = (dated ? u.dayN : 0) * (fbs::kMicroUsdPerCall / 1e6);
  use["usdMonth"]    = (dated ? u.monthN : 0) * (fbs::kMicroUsdPerCall / 1e6);
  use["usdMonthCap"] = s_budget.monthCap * (fbs::kMicroUsdPerCall / 1e6);
  if (dated) use["dayEndsInS"] = (uint32_t)(86400 - now % 86400);
  use["spacingS"] = fbs::kCallSpacingS;

  JsonArray lists = out["lists"].to<JsonArray>();
  for (uint8_t l = 0; l < aero::LIST_COUNT; l++) {
    const ListCache &c = s_lists[l];
    JsonObject o = lists.add<JsonObject>();
    o["name"] = aero::listName((aero::List)l);
    o["have"] = c.have;
    if (held(c.holdUntilMs, nowMs)) o["refusedWaitS"] = (c.holdUntilMs - nowMs) / 1000UL;
    if (!c.have) continue;
    o["age"]      = (nowMs - c.atMs) / 1000UL;
    o["kept"]     = c.r->n;
    o["seen"]     = c.r->seen;
    o["noTime"]   = c.r->noTime;
    o["noIdent"]  = c.r->noIdent;
    o["overflow"] = c.r->overflow;
    o["more"]     = c.r->more;             // AeroAPI had more than the one page asked for
  }

  if (!s_haveLast) return;
  JsonObject last = out["last"].to<JsonObject>();
  last["state"] = kStateNames[s_last.state];
  last["http"]  = s_last.http;
  last["what"]  = s_last.job.kind == JOB_LIST ? aero::listName((aero::List)s_last.job.list) : "flights";
  last["for"]   = s_last.job.kind == JOB_LIST ? s_last.job.icao : s_last.job.ident;
  last["ago"]   = (nowMs - s_lastDoneMs) / 1000UL;
  last["bytes"] = s_last.bodyBytes;
  last["jsonPeak"]   = s_last.jsonPeak;
  last["heapBefore"] = s_last.heapBefore;
  last["heapMin"]    = s_last.heapMin;
  last["stackFree"]  = s_last.stackFree;
  if (s_last.retryS) last["retryAfter"] = s_last.retryS;
  char sample[kSampleMax + 1];
  portENTER_CRITICAL(&s_mux);
  if (s_sample) memcpy(sample, s_sample, sizeof(sample));
  else          sample[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
  out["sample"] = (const char *)sample;
}

void aeroDirectTracksJson(JsonArray out) {
  static const char *const kStates[FB_TRK_COUNT] = {"wait", "notfound", "sched", "delayed", "taxi",
                                                    "enroute", "landed", "cancelled", "diverted"};
  const int64_t now = nowUtc();
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < s_trkN; i++) {
    const AeroTracker &k = s_trk[i];
    const aero::Track &t = k.t;
    JsonObject o = out.add<JsonObject>();
    o["ident"] = (const char *)k.ident;
    o["added"] = k.added;
    o["state"] = kStates[t.state < FB_TRK_COUNT ? t.state : 0];
    if (k.http) o["http"] = k.http;
    if (k.fetchedMs) o["age"] = (nowMs - k.fetchedMs) / 1000UL;
    if (k.nextAt == INT64_MAX) o["nextIn"] = -1;                       // no more calls
    else o["nextIn"] = k.nextAt > now ? (long)(k.nextAt - now) : 0L;
    if (t.state == FB_TRK_WAIT || t.state == FB_TRK_NOTFOUND) continue;
    o["fn"]      = (const char *)t.fn;
    o["from"]    = (const char *)t.from;
    o["to"]      = (const char *)t.to;
    o["gate"]    = (const char *)t.gate;
    o["current"] = t.current;
    o["flights"] = t.flights;
    o["shown"]   = (long long)aero::trackShownTime(t);
    o["delayMin"] = aero::trackDelayMin(t);
    auto times = [&](const char *key, int64_t s, int64_t e, int64_t a) {
      JsonObject x = o[key].to<JsonObject>();
      if (s) x["sched"] = (long long)s;
      if (e) x["est"] = (long long)e;
      if (a) x["act"] = (long long)a;
    };
    times("dep", t.schedOut, t.estOut, t.actOut ? t.actOut : t.actOff);
    times("arr", t.schedIn, t.estIn ? t.estIn : t.estOn, t.actIn ? t.actIn : t.actOn);
    const int64_t exp = aero::trackExpiresAt(t);
    if (exp) o["expiresIn"] = exp > now ? (long long)(exp - now) : 0LL;
  }
}

#endif  // FLIGHTBOARD_DIRECT_ENABLED

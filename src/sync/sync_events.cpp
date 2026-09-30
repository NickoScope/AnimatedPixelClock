// Instant sync events for the virtual twin: sync_events.h says what and why.
//
// Memory, inventoried before writing this (2026-09-30, the panel on 2.7.11:
// internal heap 53.8 KB free, 27 KB at its lowest, DMA-capable 19 KB at its
// lowest). Everything here is loop()-task state: the listeners and the input
// ring go to PSRAM (PSRAM_ARRAY), the rest is a few words. A datagram is built
// on the loop task's stack (under 320 B) and sent from the socket the PC
// monitor already listens on (network.cpp udp, port 4210): no new socket. The
// first send allocates WiFiUDP's 1,460 B transmit buffer, from the internal
// heap (a malloc under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and keeps it;
// each datagram is then one lwIP pbuf for as long as it takes to leave.

#include "sync_events.h"

#if defined(SYNC_EVENTS_ENABLED)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <string.h>
#include <sys/time.h>

#include "../config/config.h"
#include "../config/settings.h"
#include "../display/display.h"
#include "../panel/panel.h"
#include "../util/psram_state.h"
#include "../web/web.h"
#if defined(LUA_EFFECTS_ENABLED)
#include "../lua/lua_effects.h"
#endif

extern WiFiUDP udp;                         // network.cpp
extern WebServer server;
const char *panelPageKeyName(uint8_t page); // web_panel.cpp

namespace {

const uint32_t kTtlMs      = 60000;         // a listener that does not renew is dropped
const uint32_t kCauseMs    = 1500;          // how long a noted cause explains a change
const uint32_t kCoalesceMs = 100;           // at most one screen datagram per listener this often
const uint8_t  kListeners  = 2;
const uint8_t  kRing       = 8;

const char *const kByName[] = {"auto", "knob", "ir", "http", "sync", "carousel", "schedule"};
const char *const kInName[] = {"press", "cw", "ccw"};

struct Listener {
  uint32_t ip;                              // 0 = free
  uint16_t port;
  uint32_t untilMs;
  uint32_t lastScreenMs;
  bool     screenDue;                       // a change it has not been sent yet
};
PSRAM_ARRAY(Listener, s_lis, [kListeners]);

struct InputRec {
  uint32_t seq;
  uint64_t epochMs;                         // wall clock, 0 before NTP
  uint8_t  kind, by, page;
  bool     entered;
};
PSRAM_ARRAY(InputRec, s_ring, [kRing]);
uint8_t  s_ringNext = 0;
uint32_t s_ringCount = 0;

uint32_t s_seq = 0;
uint8_t  s_cause = SYNC_BY_AUTO;
uint32_t s_causeAt = 0;
uint32_t s_lastReq = 0;
uint8_t  s_simBy = SYNC_BY_HTTP;

struct Screen {
  uint8_t  page, style, bright;
  bool     entered, forcedOff, schedOff;
  int16_t  fxId;
  uint32_t fxRun, fxClicks;
  bool     operator==(const Screen &o) const {
    return page == o.page && style == o.style && bright == o.bright && entered == o.entered &&
           forcedOff == o.forcedOff && schedOff == o.schedOff && fxId == o.fxId &&
           fxRun == o.fxRun && fxClicks == o.fxClicks;
  }
};
Screen   s_last = {};
bool     s_haveLast = false;
uint8_t  s_screenBy = SYNC_BY_AUTO;         // why the unsent change happened
// The seq a screen state goes out under, given when it is first sent rather
// than when it changes: changes coalesced into one datagram would otherwise
// use up numbers that never went out, and a listener reads a gap as a loss.
// 0 = the current state has no number yet.
uint32_t s_screenSeq = 0;

uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec < 1700000000 ? 0 : (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

uint8_t causeNow(uint32_t nowMs) {
  // Signed: s_causeAt is millis() | 1 (0 means none), so it can be 1 ms ahead
  // of a nowMs read just after, and unsigned that is 49 days old - every cause
  // read as "auto" on the panel (2026-09-30).
  return (s_causeAt && (int32_t)(nowMs - s_causeAt) <= (int32_t)kCauseMs) ? s_cause : (uint8_t)SYNC_BY_AUTO;
}

Screen snapshot() {
  Screen s = {};
  s.page = panelCurrentPage();
  s.style = settings.clockStyle;
  s.bright = (uint8_t)(((uint16_t)settings.displayBrightness * 100 + 127) / 255);   // as /api/status
  s.entered = panelEnteredPage();
  s.forcedOff = isDisplayForcedOff();
  s.schedOff = isDisplayScheduledOff();
  s.fxId = -1;
#if defined(LUA_EFFECTS_ENABLED)
  bool open;
  luaEffectsFx(&s.fxId, &s.fxRun, &s.fxClicks, &open);
#endif
  return s;
}

// A JSON string, escaped: page names come from uploaded scripts and custom
// airports, and a quote in one must not break the datagram.
size_t putStr(char *out, size_t cap, const char *s) {
  size_t n = 0;
  if (n < cap) out[n++] = '"';
  for (; s && *s && n + 3 < cap; s++) {
    const char c = *s;
    if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
    else if ((unsigned char)c < 0x20) { out[n++] = ' '; }
    else out[n++] = c;
  }
  if (n < cap) out[n++] = '"';
  return n;
}

bool live(const Listener &l, uint32_t nowMs) {
  return l.ip && (int32_t)(l.untilMs - nowMs) > 0;
}

void sendTo(const Listener &l, const char *buf, size_t n) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udp.beginPacket(IPAddress(l.ip), l.port)) return;
  udp.write((const uint8_t *)buf, n);
  udp.endPacket();                          // one try: seq tells the listener what it missed
}

size_t buildScreen(char *buf, size_t cap, const Screen &s, uint8_t by) {
  char name[48];
  snprintf(name, sizeof(name), "%s", panelPageName(s.page));
  int n = snprintf(buf, cap, "{\"seq\":%u,\"t\":\"screen\",\"page\":%u,\"key\":\"%s\",\"name\":",
                   (unsigned)s_screenSeq, (unsigned)s.page, panelPageKeyName(s.page));
  if (n < 0 || (size_t)n >= cap) return 0;
  n += (int)putStr(buf + n, cap - n, name);
  const int m = snprintf(buf + n, cap - n,
      ",\"style\":%u,\"entered\":%s,\"off\":%s,\"bright\":%u,\"fx\":{\"id\":%d,\"run\":%u,\"clicks\":%u},\"by\":\"%s\"}",
      (unsigned)s.style, s.entered ? "true" : "false", (s.forcedOff || s.schedOff) ? "true" : "false",
      (unsigned)s.bright, (int)s.fxId, (unsigned)s.fxRun, (unsigned)s.fxClicks, kByName[by]);
  if (m < 0 || (size_t)(n + m) >= cap) return 0;
  return (size_t)(n + m);
}

}  // namespace

void syncNoteCause(SyncBy by) {
  s_cause = by;
  s_causeAt = millis() | 1;
}

void syncNoteSimulated(SyncBy by) { s_simBy = by; syncNoteCause(by); }
SyncBy syncSimulatedBy() { return (SyncBy)s_simBy; }

SyncBy syncHttpBy() {
  return server.header("X-Twin-Sync") == "1" ? SYNC_BY_SYNC : SYNC_BY_HTTP;
}

void syncAfterHttp(uint32_t reqCount) {
  if (reqCount == s_lastReq) return;
  s_lastReq = reqCount;
  // Only a request that can change the screen is a cause. The twin's sync reads
  // /api/panel every few seconds; were its reads causes, a carousel step in the
  // same second would go out as "sync" and never be mirrored. Every POST
  // counts; of the GETs, the few that act (web.cpp: display, mode, clock style,
  // the remote, a clip).
  if (server.method() == HTTP_GET) {
    extern const char *webLastUri();
    const char *u = webLastUri();
    static const char *const kActs[] = {"/api/display/", "/api/mode/", "/api/clock/", "/api/ir/do",
                                        "/api/ir/press", "/api/anim/play", "/api/fx3d"};
    bool acts = false;
    for (const char *a : kActs) acts = acts || strncmp(u, a, strlen(a)) == 0;
    if (!acts) return;
  }
  // The headers of the request just served are still the server's current ones:
  // WebServer resets them only when it parses the next request.
  syncNoteCause(syncHttpBy());
}

void syncInput(SyncInputKind kind, SyncBy by) {
  const uint32_t nowMs = millis();
  syncNoteCause(by);
  InputRec &r = s_ring[s_ringNext];
  r.seq = ++s_seq;
  r.epochMs = epochMs();
  r.kind = (uint8_t)kind;
  r.by = (uint8_t)by;
  r.page = panelCurrentPage();
  r.entered = panelEnteredPage();
  s_ringNext = (uint8_t)((s_ringNext + 1) % kRing);
  s_ringCount++;
  char buf[160];
  const int n = snprintf(buf, sizeof(buf),
      "{\"seq\":%u,\"t\":\"input\",\"kind\":\"%s\",\"by\":\"%s\",\"page\":%u,\"entered\":%s}",
      (unsigned)r.seq, kInName[kind], kByName[by], (unsigned)r.page, r.entered ? "true" : "false");
  if (n <= 0 || (size_t)n >= sizeof(buf)) return;
  for (uint8_t i = 0; i < kListeners; i++)
    if (live(s_lis[i], nowMs)) sendTo(s_lis[i], buf, (size_t)n);
}

void syncEventsLoop() {
  const uint32_t nowMs = millis();
  const Screen s = snapshot();
  if (!s_haveLast) { s_last = s; s_haveLast = true; }
  if (!(s == s_last)) {
    // Only the night schedule flipped the screen off or on: its own cause.
    const bool onlySched = s.schedOff != s_last.schedOff && s.page == s_last.page && s.style == s_last.style &&
                           s.bright == s_last.bright && s.forcedOff == s_last.forcedOff;
    uint8_t by = causeNow(nowMs);
    if (by == SYNC_BY_AUTO && onlySched) by = SYNC_BY_SCHEDULE;
    s_screenBy = by;
    s_last = s;
    s_screenSeq = 0;
    for (uint8_t i = 0; i < kListeners; i++)
      if (live(s_lis[i], nowMs)) s_lis[i].screenDue = true;
  }
  for (uint8_t i = 0; i < kListeners; i++) {
    Listener &l = s_lis[i];
    if (!l.ip) continue;
    if (!live(l, nowMs)) { l.ip = 0; continue; }
    if (!l.screenDue || nowMs - l.lastScreenMs < kCoalesceMs) continue;
    if (!s_screenSeq) s_screenSeq = ++s_seq;   // one number for this state, whichever listener gets it first
    char buf[320];
    const size_t n = buildScreen(buf, sizeof(buf), s_last, s_screenBy);
    if (n) sendTo(l, buf, n);
    l.screenDue = false;
    l.lastScreenMs = nowMs;
  }
}

bool syncListen(const IPAddress &ip, uint16_t port, bool stop) {
  const uint32_t nowMs = millis();
  const uint32_t a = (uint32_t)ip;
  for (uint8_t i = 0; i < kListeners; i++) {
    Listener &l = s_lis[i];
    if (l.ip == a && l.port == port) {
      if (stop) { l.ip = 0; return true; }
      l.untilMs = nowMs + kTtlMs;
      return true;
    }
  }
  if (stop) return true;
  for (uint8_t i = 0; i < kListeners; i++) {
    Listener &l = s_lis[i];
    if (l.ip && live(l, nowMs)) continue;
    l.ip = a;
    l.port = port;
    l.untilMs = nowMs + kTtlMs;
    l.lastScreenMs = 0;
    l.screenDue = true;                     // it starts from the screen as it is
    return true;
  }
  return false;
}

uint32_t syncSeq() { return s_seq; }

void syncPanelJson(JsonObject now, JsonDocument &doc, int32_t inputAfter) {
#if defined(LUA_EFFECTS_ENABLED)
  {
    int16_t id; uint32_t run, clicks; bool open;
    luaEffectsFx(&id, &run, &clicks, &open);
    JsonObject fx = now["fx"].to<JsonObject>();
    fx["id"] = id;
    fx["open"] = open;
    fx["run"] = run;
    fx["clicks"] = clicks;
  }
#endif
  now["seq"] = s_seq;
  now["inputSeq"] = s_ringCount ? s_ring[(s_ringNext + kRing - 1) % kRing].seq : 0;
  if (inputAfter < 0) return;
  JsonArray in = doc["input"].to<JsonArray>();
  const uint8_t have = s_ringCount < kRing ? (uint8_t)s_ringCount : kRing;
  for (uint8_t k = 0; k < have; k++) {       // oldest first
    const InputRec &r = s_ring[(s_ringNext + kRing - have + k) % kRing];
    if ((int64_t)r.seq <= (int64_t)inputAfter) continue;
    JsonObject o = in.add<JsonObject>();
    o["seq"] = r.seq;
    o["epochMs"] = r.epochMs;
    o["kind"] = kInName[r.kind];
    o["by"] = kByName[r.by];
    o["page"] = r.page;
    o["entered"] = r.entered;
  }
}

#endif  // SYNC_EVENTS_ENABLED

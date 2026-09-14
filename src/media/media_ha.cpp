// Media player: the MQTT side. Topics, ingest, the selection Home Assistant
// follows, commands (volume coalesced), NVS, and what the portal reads.
//
// Everything here runs on the loop task: the bus calls the handler from
// mqttBusLoop(), the page and the web handlers run in loop() too. Nothing is
// shared with another task, so there is no lock and no torn read.

#include "media.h"

#if defined(MEDIAPLAYER_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "../mqtt/mqtt_bus.h"

namespace media {

// ── memory ──────────────────────────────────────────────────────────────────
// The model is about 8 KB with its spares - a parse lands in a spare and is
// copied over the live one only when it is valid - so it goes to PSRAM, where
// internal SRAM is the scarce heap on this board (src/railboard/railboard.cpp,
// "JSON memory"). The parse's own allocations are capped the same way: past
// the cap deserializeJson() reports NoMemory and the payload is refused. The
// cap is a bound, not a measurement; /api/media reports the peak seen.
static const size_t JSON_CAP = 12288;

class MediaJsonAllocator : public ArduinoJson::Allocator {
 public:
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
    if (used_ - was + size > JSON_CAP) return nullptr;
    Hdr *h = static_cast<Hdr *>(heap_caps_realloc(old, sizeof(Hdr) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!h) h = static_cast<Hdr *>(heap_caps_realloc(old, sizeof(Hdr) + size, MALLOC_CAP_8BIT));
    if (!h) return nullptr;
    h->size = size;
    used_   = used_ - was + size;
    if (used_ > peak_) peak_ = used_;
    return h + 1;
  }

  size_t peak() const { return peak_; }

 private:
  union Hdr { size_t size; max_align_t align; };
  size_t used_ = 0;
  size_t peak_ = 0;
};

static MediaJsonAllocator s_alloc;

struct Model {
  NowPlaying np, npIn;
  Players    pl, plIn;
  Favs       fv, fvIn;
};
static Model *s_m = nullptr;

// ── topics ──────────────────────────────────────────────────────────────────
static char s_dev[7]      = "";
static char s_root[64]    = "";   // MQTT_BASE/<dev>/media/ - also the handler's prefix, so it lives forever
static char s_sub[64]     = "";
static char s_tCmd[72]    = "";
static char s_tSelect[72] = "";
static bool s_topics      = false;
static bool s_handler     = false;
static bool s_subscribed  = false;

// ── state ───────────────────────────────────────────────────────────────────
static const char *const NVS_NS     = "media";
static const char *const NVS_PLAYER = "player";
static const uint32_t    SETTLE_MS  = 2500;    // as src/panel: a burst of changes is one write

static char     s_sel[kPlayerIdLen]      = "";
static char     s_selSaved[kPlayerIdLen] = "";
static uint32_t s_selDirtyAt = 0;
static bool     s_selToSend  = false;
static bool     s_wasUp      = false;

static bool     s_bridgeHave   = false;
static bool     s_bridgeOnline = false;
static uint32_t s_rxMs         = 0;
static uint32_t s_trackKey     = 0;
static bool     s_trackKnown   = false;
static uint32_t s_newTrackMs   = 0;
static uint16_t s_refused      = 0;
static char     s_refusal[40]  = "";
static size_t   s_jsonPeak     = 0;

static uint32_t s_cmdSent = 0, s_cmdFailed = 0, s_cmdMs = 0;
static char     s_cmdLast[10] = "";
static bool     s_cmdOk = false;

static int      s_volTarget   = -1;   // absolute, not yet confirmed
static int      s_volSent     = -1;
static int      s_stepPending = 0;    // relative, while the volume is not reported
static uint32_t s_volSentMs   = 0;
static uint32_t s_volTouchMs  = 0;

// ── accessors ───────────────────────────────────────────────────────────────
bool ready() { return s_m != nullptr; }
const NowPlaying &now() { return s_m->np; }
const Players &players() { return s_m->pl; }
const Favs &favs() { return s_m->fv; }
const char *selected() { return s_sel; }
const char *deviceId() { return s_dev; }
bool bridgeOnline() { return s_bridgeOnline; }
bool haveBridge() { return s_bridgeHave; }
uint32_t newTrackMs() { return s_newTrackMs; }

static bool clockAge(long *age) {
  const time_t t = time(nullptr);
  if (!s_m || !s_m->np.have || t < 1700000000) return false;   // before NTP the clock reads 1970
  *age = (long)((int64_t)t - (int64_t)s_m->np.ts);
  return true;
}

long ageS() {
  long a;
  if (!clockAge(&a)) return -1;
  return a < 0 ? 0 : a;
}

// Stale when Home Assistant's own timestamp is older than MEDIA_STALE_S.
// Without a clock, or with one five minutes behind Home Assistant's, judge by
// when the payload arrived instead - the rail board's rule.
bool stale() {
  if (!s_m || !s_m->np.have) return true;
  long a;
  if (!clockAge(&a) || a < -300) return (millis() - s_rxMs) / 1000UL > (uint32_t)MEDIA_STALE_S;
  return a > (long)MEDIA_STALE_S;
}

// The player on screen gets the commands; before Home Assistant has named one,
// the portal's choice.
static const char *target() {
  if (s_m && s_m->np.have && s_m->np.player[0]) return s_m->np.player;
  return s_sel;
}

// ── ingest ──────────────────────────────────────────────────────────────────
static void refuse(const char *leaf, const char *why, uint16_t len) {
  if (s_refused < 65535) s_refused++;
  snprintf(s_refusal, sizeof(s_refusal), "%.12s: %s", leaf, why);
  Serial.printf("[media] refused %.12s (%s, %u B)\n", leaf, why, (unsigned)len);
}

static void acceptState() {
  NowPlaying &in = s_m->npIn;
  const uint32_t key = trackKey(in);
  // A retained payload delivered again, or Home Assistant's keepalive, has the
  // same key: only a different track is a new one.
  if (s_trackKnown && key != s_trackKey) s_newTrackMs = millis() | 1;
  s_trackKey   = key;
  s_trackKnown = true;
  memcpy(&s_m->np, &in, sizeof(in));
  s_rxMs = millis() | 1;
}

static void onMessage(const char *topic, const uint8_t *payload, uint16_t len) {
  if (!s_m) return;
  const char *leaf = topic + strlen(s_root);
  // Ours, echoed back: the subscription is the whole media/+ level.
  if (!strcmp(leaf, "cmd") || !strcmp(leaf, "select")) return;
  if (!strcmp(leaf, "ha")) {
    if (!len) { s_bridgeHave = false; return; }   // a cleared retained topic
    if (len == 6 && !memcmp(payload, "online", 6))       { s_bridgeHave = true; s_bridgeOnline = true; }
    else if (len == 7 && !memcmp(payload, "offline", 7)) { s_bridgeHave = true; s_bridgeOnline = false; }
    else refuse(leaf, "not online or offline", len);
    return;
  }
  const bool isState = !strcmp(leaf, "state"), isPlayers = !strcmp(leaf, "players"), isFavs = !strcmp(leaf, "favs");
  if (!isState && !isPlayers && !isFavs) return;   // a later topic this build does not know
  if (!len) return;                                 // cleared: keep what is shown

  JsonDocument doc(&s_alloc);
  const DeserializationError err = deserializeJson(doc, (const char *)payload, len);
  if (s_alloc.peak() > s_jsonPeak) s_jsonPeak = s_alloc.peak();
  if (err) { refuse(leaf, err.c_str(), len); return; }
  const JsonObjectConst o = doc.as<JsonObjectConst>();
  const char *why;
  if (isState) {
    why = stateFrom(o, &s_m->npIn);
    if (!why) acceptState();
  } else if (isPlayers) {
    why = playersFrom(o, &s_m->plIn);
    if (!why) memcpy(&s_m->pl, &s_m->plIn, sizeof(Players));
  } else {
    why = favsFrom(o, &s_m->fvIn);
    if (!why) memcpy(&s_m->fv, &s_m->fvIn, sizeof(Favs));
  }
  if (why) refuse(leaf, why, len);
}

// The device id needs the MAC, which mqtt_bus reads the same way for its
// client id. Retried from mediaLoop() until it is known.
static void ensureTopics() {
  if (s_topics) return;
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  if (!(mac[3] | mac[4] | mac[5])) return;
  snprintf(s_dev, sizeof(s_dev), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  const int n = snprintf(s_root, sizeof(s_root), MQTT_BASE "/%s/media/", s_dev);
  if (n <= 0 || (size_t)n + 8 >= sizeof(s_root)) { Serial.println("[media] MQTT_BASE too long for the topics"); return; }
  snprintf(s_sub, sizeof(s_sub), "%s+", s_root);
  snprintf(s_tCmd, sizeof(s_tCmd), "%scmd", s_root);
  snprintf(s_tSelect, sizeof(s_tSelect), "%sselect", s_root);
  s_topics     = true;
  s_handler    = mqttBusOnMessage(s_root, onMessage);
  s_subscribed = mqttBusSubscribe(s_sub);
  Serial.printf("[media] topics %s{state,players,favs,ha,select,cmd}%s%s\n", s_root,
                s_handler ? "" : " - MQTT bus is full: handler REFUSED",
                s_subscribed ? "" : " - MQTT bus is full: subscription REFUSED");
}

// ── selection ───────────────────────────────────────────────────────────────
static void loadSelection() {
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error on
  // every boot (src/panel/panel.cpp, arduino-esp32 2.0.17). isKey() first:
  // getString() logs at error level for a key never written.
  if (!p.begin(NVS_NS, false)) return;
  if (p.isKey(NVS_PLAYER)) {
    char buf[kPlayerIdLen];
    if (p.getString(NVS_PLAYER, buf, sizeof(buf)) && validPlayerId(buf)) {
      copyUtf8(s_sel, sizeof(s_sel), buf);
      copyUtf8(s_selSaved, sizeof(s_selSaved), buf);
      s_selToSend = true;
    }
  }
  p.end();
}

static void saveSelection() {
  if (!s_selDirtyAt || millis() - s_selDirtyAt < SETTLE_MS) return;
  s_selDirtyAt = 0;
  if (!strcmp(s_sel, s_selSaved)) return;
  Preferences p;
  if (p.begin(NVS_NS, false) && p.putString(NVS_PLAYER, s_sel)) copyUtf8(s_selSaved, sizeof(s_selSaved), s_sel);
  else s_selDirtyAt = millis() | 1;   // try again after another settle
  p.end();
}

bool setSelected(const char *id) {
  if (!validPlayerId(id)) return false;
  if (strcmp(s_sel, id)) {
    copyUtf8(s_sel, sizeof(s_sel), id);
    s_selDirtyAt = millis() | 1;
  }
  s_selToSend = true;   // the same one again too: Home Assistant may have missed it
  return true;
}

// Retained, so the app learns the choice after its own restart as well as at
// the moment it changes; sent again on every reconnect, since the broker may
// have lost it.
static void selectionTick() {
  const bool up = mqttBusConnected();
  if (up && !s_wasUp && s_sel[0]) s_selToSend = true;
  s_wasUp = up;
  if (!up || !s_selToSend || !s_topics || !s_sel[0]) return;
  char body[96];
  const size_t n = selectPayload(body, sizeof(body), s_sel);
  if (n && mqttBusPublish(s_tSelect, body, true)) s_selToSend = false;
}

// ── commands ────────────────────────────────────────────────────────────────
static bool publishCmd(const char *body, size_t n, const char *name) {
  const bool ok = n && s_topics && mqttBusPublish(s_tCmd, body, false);
  if (ok) s_cmdSent++;
  else    s_cmdFailed++;
  copyUtf8(s_cmdLast, sizeof(s_cmdLast), name);
  s_cmdOk = ok;
  s_cmdMs = millis() | 1;
  return ok;
}

bool sendSimple(const char *cmd) {
  char b[128];
  return publishCmd(b, cmdSimple(b, sizeof(b), cmd, target()), cmd);
}

bool sendMute(bool muted) {
  char b[128];
  return publishCmd(b, cmdMute(b, sizeof(b), target(), muted), "mute");
}

bool sendFav(const char *favId) {
  char b[224];
  return publishCmd(b, cmdFav(b, sizeof(b), target(), favId), "play_fav");
}

bool sendVolStep(int step) {
  char b[128];
  return publishCmd(b, cmdInt(b, sizeof(b), "vol_step", target(), step), "vol_step");
}

void setVolume(int v) {
  s_volTarget   = v < 0 ? 0 : (v > 100 ? 100 : v);
  s_stepPending = 0;
  s_volTouchMs  = millis() | 1;
}

void nudgeVolume(int delta) {
  const int base = volumeShown();
  if (base >= 0) {
    setVolume(base + delta);
    return;
  }
  // No volume reported: an absolute value would be a guess, and a loud one
  // could be. Steps instead, which the app applies to whatever the player has.
  const int s = s_stepPending + delta;
  s_stepPending = s < -20 ? -20 : (s > 20 ? 20 : s);
  s_volTouchMs  = millis() | 1;
}

int volumeShown() {
  if (s_volTarget >= 0) return s_volTarget;
  return (s_m && s_m->np.have) ? s_m->np.vol : -1;
}

// At most one volume command every MEDIA_VOL_SEND_MS, however fast the knob
// turns: the newest value goes, the ones in between never do.
static void volumeTick() {
  const uint32_t ms = millis();
  const bool up  = mqttBusConnected();
  const bool due = !s_volSentMs || ms - s_volSentMs >= (uint32_t)MEDIA_VOL_SEND_MS;
  if (up && due && s_volTarget >= 0 && s_volTarget != s_volSent) {
    char b[128];
    if (publishCmd(b, cmdInt(b, sizeof(b), "vol", target(), s_volTarget), "vol")) s_volSent = s_volTarget;
    s_volSentMs = ms | 1;
  } else if (up && due && s_stepPending) {
    if (sendVolStep(s_stepPending)) s_stepPending = 0;
    s_volSentMs = ms | 1;
  }
  // The value turned to stays on screen for 3 s after the last detent, long
  // enough for Home Assistant to report it back; offline, it is dropped then.
  if (ms - s_volTouchMs > 3000) {
    if (s_volTarget >= 0 && (s_volTarget == s_volSent || !up)) s_volTarget = s_volSent = -1;
    if (s_stepPending && !up) s_stepPending = 0;
  }
  if (s_volTarget < 0 && !s_stepPending && s_volSentMs && ms - s_volSentMs >= (uint32_t)MEDIA_VOL_SEND_MS) s_volSentMs = 0;
}

// ── lifecycle ───────────────────────────────────────────────────────────────
}  // namespace media

void mediaBegin() {
  using namespace media;
  s_m = static_cast<Model *>(heap_caps_calloc(1, sizeof(Model), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_m) s_m = static_cast<Model *>(heap_caps_calloc(1, sizeof(Model), MALLOC_CAP_8BIT));
  if (!s_m) Serial.printf("[media] no memory for the model (%u B): the page says so\n", (unsigned)sizeof(Model));
  loadSelection();
  ensureTopics();
}

void mediaLoop() {
  using namespace media;
  ensureTopics();
  saveSelection();
  selectionTick();
  volumeTick();
  pageTick();
}

// ── the portal ──────────────────────────────────────────────────────────────
void mediaStatusJson(JsonObject out) {
  using namespace media;
  const time_t t = time(nullptr);
  const bool synced = t > 1700000000;
  out["ready"]      = ready();
  out["dev"]        = (const char *)s_dev;
  out["root"]       = (const char *)s_root;
  out["handler"]    = s_handler;
  out["subscribed"] = s_subscribed;
  out["selected"]   = (const char *)s_sel;
  out["selSent"]    = !s_selToSend;
  out["selSaved"]   = s_selDirtyAt == 0;
  out["bridge"]     = !s_bridgeHave ? "unknown" : (s_bridgeOnline ? "online" : "offline");
  out["staleS"]     = MEDIA_STALE_S;
  out["synced"]     = synced;
  out["volPending"] = s_volTarget;
  out["refused"]    = s_refused;
  out["lastRefusal"] = (const char *)s_refusal;
  out["jsonPeak"]   = (uint32_t)s_jsonPeak;
  JsonObject c = out["cmds"].to<JsonObject>();
  c["sent"]   = s_cmdSent;
  c["failed"] = s_cmdFailed;
  c["last"]   = (const char *)s_cmdLast;
  c["lastOk"] = s_cmdOk;
  if (s_cmdMs) c["agoMs"] = millis() - s_cmdMs;
  if (!s_m) return;

  const NowPlaying &n = s_m->np;
  const bool isStale = stale();
  JsonObject np = out["np"].to<JsonObject>();
  np["have"] = n.have;
  if (n.have) {
    np["player"] = (const char *)n.player;
    np["name"]   = (const char *)n.name;
    np["src"]    = (const char *)n.src;
    np["st"]     = kStateKeys[n.st < ST_COUNT ? n.st : ST_UNAVAILABLE];
    np["kind"]   = kKindKeys[n.kind < K_COUNT ? n.kind : K_MUSIC];
    np["title"]  = (const char *)n.title;
    np["artist"] = (const char *)n.artist;
    np["album"]  = (const char *)n.album;
    np["vol"]    = n.vol;
    np["muted"]  = n.muted;
    np["pos"]    = (synced && !isStale) ? positionAt(n, (int64_t)t) : n.pos;
    np["dur"]    = n.dur;
    np["ts"]     = n.ts;
    np["err"]    = (const char *)n.err;
    np["stale"]  = isStale;
    np["rx"]     = (millis() - s_rxMs) / 1000UL;
    if (ageS() >= 0) np["age"] = ageS();
  }

  const Players &pl = s_m->pl;
  JsonObject jp = out["players"].to<JsonObject>();
  jp["have"] = pl.have;
  jp["sel"]  = (const char *)pl.sel;
  jp["dropped"] = pl.dropped;
  if (pl.have && synced) jp["age"] = (long)((int64_t)t - (int64_t)pl.ts);
  JsonArray lp = jp["list"].to<JsonArray>();
  for (uint8_t i = 0; i < pl.n; i++) {
    JsonObject o = lp.add<JsonObject>();
    o["id"]   = (const char *)pl.p[i].id;
    o["name"] = (const char *)pl.p[i].name;
    o["st"]   = kStateKeys[pl.p[i].st < ST_COUNT ? pl.p[i].st : ST_UNAVAILABLE];
  }

  const Favs &fv = s_m->fv;
  JsonObject jf = out["favs"].to<JsonObject>();
  jf["have"] = fv.have;
  jf["dropped"] = fv.dropped;
  if (fv.have && synced) jf["age"] = (long)((int64_t)t - (int64_t)fv.ts);
  JsonArray lf = jf["list"].to<JsonArray>();
  for (uint8_t i = 0; i < fv.n; i++) {
    JsonObject o = lf.add<JsonObject>();
    o["id"]   = (const char *)fv.f[i].id;
    o["name"] = (const char *)fv.f[i].name;
  }
}

int mediaWebPost(JsonObjectConst in, const char **why) {
  using namespace media;
  static const char *const kKeys[] = {"select", "cmd", "vol", "vol_step", "mute", "play_fav"};
  int present = 0;
  for (JsonPairConst kv : in) {
    bool known = false;
    for (const char *k : kKeys) known = known || !strcmp(kv.key().c_str(), k);
    if (!known) { *why = "unknown key: send one of select, cmd, vol, vol_step, mute, play_fav"; return 400; }
    present++;
  }
  if (present != 1) { *why = "send exactly one of select, cmd, vol, vol_step, mute, play_fav"; return 400; }
  if (!s_m) { *why = "the media player has no memory on this panel"; return 503; }

  JsonVariantConst v = in["select"];
  if (!v.isNull()) {
    const char *id = v.is<const char *>() ? v.as<const char *>() : nullptr;
    if (!id || !validPlayerId(id)) { *why = "select must be a media_player entity id of at most 63 characters"; return 400; }
    const Players &pl = s_m->pl;
    bool listed = !pl.have || !pl.n;   // before any list arrives, any valid id: Home Assistant decides
    for (uint8_t i = 0; i < pl.n; i++) listed = listed || !strcmp(pl.p[i].id, id);
    if (!listed) { *why = "that player is not in Home Assistant's list"; return 409; }
    setSelected(id);                   // published from mediaLoop(), now or on connect
    return 200;
  }

  // Everything else is a command: it has to be able to go now.
  if (!mqttBusConnected()) { *why = "MQTT is not connected: nothing was sent"; return 503; }
  if (!target()[0]) { *why = "Home Assistant follows no player yet"; return 409; }

  if (!(v = in["cmd"]).isNull()) {
    static const char *const kCmds[] = {"toggle", "play", "pause", "next", "prev"};
    const char *c = v.is<const char *>() ? v.as<const char *>() : nullptr;
    bool ok = false;
    for (const char *k : kCmds) ok = ok || (c && !strcmp(c, k));
    if (!ok) { *why = "cmd must be toggle, play, pause, next or prev"; return 400; }
    if (!sendSimple(c)) { *why = "not sent"; return 503; }
    return 200;
  }
  if (!(v = in["vol"]).isNull()) {
    if (v.is<bool>() || !v.is<long>() || v.as<long>() < 0 || v.as<long>() > 100) { *why = "vol must be a whole number 0 to 100"; return 400; }
    setVolume((int)v.as<long>());      // coalesced with the knob's
    return 200;
  }
  if (!(v = in["vol_step"]).isNull()) {
    if (v.is<bool>() || !v.is<long>() || v.as<long>() < -20 || v.as<long>() > 20 || !v.as<long>()) {
      *why = "vol_step must be a whole number -20 to 20, not 0";
      return 400;
    }
    if (!sendVolStep((int)v.as<long>())) { *why = "not sent"; return 503; }
    return 200;
  }
  if (!(v = in["mute"]).isNull()) {
    if (!v.is<bool>()) { *why = "mute must be true or false"; return 400; }
    if (!sendMute(v.as<bool>())) { *why = "not sent"; return 503; }
    return 200;
  }
  v = in["play_fav"];
  const char *id = v.is<const char *>() ? v.as<const char *>() : nullptr;
  if (!id || !validFavId(id)) { *why = "play_fav must be a favourite's id"; return 400; }
  bool listed = false;
  for (uint8_t i = 0; i < s_m->fv.n; i++) listed = listed || !strcmp(s_m->fv.f[i].id, id);
  if (!listed) { *why = "not one of the favourites Home Assistant sent"; return 409; }
  if (!sendFav(id)) { *why = "not sent"; return 503; }
  return 200;
}

#endif  // MEDIAPLAYER_ENABLED

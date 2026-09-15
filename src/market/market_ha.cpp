// Stock market dashboard: the MQTT side, the settings and the stores.
//
// Topics and ingest (one wildcard subscription, every payload validated whole
// by market_model.cpp before it replaces the record it names), the settings
// (market_settings.h) in NVS as one blob and out to Home Assistant as the
// retained config, the last records on LittleFS so a reboot with Home
// Assistant down still shows the pages with their as-of, and what the portal
// reads and writes through /api/market.
//
// Everything here runs on the loop task: the bus calls the handler from
// mqttBusLoop(), the pages and the web handlers run in loop() too. Nothing is
// shared with another task, so there is no lock and no torn read.

#include "market.h"

#if defined(MARKET_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "../mqtt/mqtt_bus.h"

namespace market {

// ── memory ──────────────────────────────────────────────────────────────────
// Everything this module keeps is one Store in PSRAM (the model 85 KB, its
// spares, three copies of the settings, the settings' list buffers, the
// topics, the notes, the page's buffers) plus the LittleFS record buffer:
// this board has 16 MB of PSRAM, and the internal heap is the scarce one
// (src/railboard/railboard.cpp, "JSON memory"). JSON documents take their
// memory from PSRAM too, through the two allocators below; a payload's parse
// is capped, and past the cap deserializeJson() reports NoMemory and the
// payload is refused. The caps are bounds, not measurements; /api/market
// reports the payload parse's peak.
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  explicit PsramJsonAllocator(size_t cap) : cap_(cap) {}

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
  size_t cap_;
  size_t used_ = 0;
  size_t peak_ = 0;
};

static PsramJsonAllocator s_alloc(16384);      // payloads from the app
static PsramJsonAllocator s_setAlloc(32768);   // the settings' documents: the config payload, the extra row

struct Store {
  Model         m;
  Spare         sp;
  mks::Settings s, scratch, dflt;
  mks::Scratch  msc;                         // market_settings.cpp's list buffers
  KnobSettle    settle;                      // the knob's changes, waiting to be saved
  ConfigGate    gate;                        // the config last published
  ConnectWatch  connects;                    // the bus's connects already answered with the config
  PageBuf       page;
  char          cfg[mks::kConfigMax];        // the config payload, serialised
  char          blob[mks::kBlobMax];         // the NVS blob
  char          watchSyms[kMaxWatch][kSymLen];
  char          watchNames[kMaxWatch][kNameLen];
  char          dev[7];
  char          root[64];                    // MQTT_BASE/<dev>/market/ - the handler's prefix: the store is never freed
  char          sub[64];
  char          tConfig[72];
  char          cfgTicker[kSymLen];          // the ticker in the last config published
  char          nvsNote[24];
  char          fsNote[48];
  char          refusal[48];
};
static Store   *s_st  = nullptr;
static uint8_t *s_rec = nullptr;        // the LittleFS record, recordSize() bytes

// ── state ───────────────────────────────────────────────────────────────────
static const char *const NVS_NS   = "market";
static const char *const NVS_BLOB = "cfg";
static const char *const NVS_VER  = "v";
static const char *const FS_DIR   = "/market";
static const char *const FS_PATH  = "/market/last.bin";
static const char *const FS_TMP   = "/market/last.tmp";

static bool     s_topics      = false;
static bool     s_handler     = false;
static bool     s_subscribed  = false;

static bool     s_nvsRetry    = false;  // the last write failed: again after a settle
static uint32_t s_nvsRetryMs  = 0;
static uint32_t s_nvsWrites   = 0, s_nvsFails = 0;
static size_t   s_blobBytes   = 0;

static bool     s_cfgToSend   = false;  // the retained config still has to go out
static bool     s_cfgForce    = false;  // and even if it is the one last published
static bool     s_cfgUnchanged = false; // the last one due was identical and not sent
static uint32_t s_cfgSkipped  = 0;
static bool     s_cfgTooBig   = false;
static size_t   s_cfgBytes    = 0;
static uint32_t s_cfgSentMs   = 0, s_cfgSent = 0;

static bool     s_bridgeHave   = false;
static bool     s_bridgeOnline = false;
static uint32_t s_rxMs         = 0;
static uint32_t s_accepted     = 0;
static uint16_t s_refused      = 0;
static size_t   s_jsonPeak     = 0;

static bool     s_fsReady    = false;
static uint32_t s_fsNoSpace  = 0;      // writes skipped because the record would not fit
static size_t   s_fsFreeBytes = 0;     // LittleFS free space at the last check
static bool     s_fsHave     = false;   // a record was read at boot
static bool     s_fsDirty    = false;   // a history payload was accepted since the record
static uint32_t s_fsDirtyMs  = 0;       // millis() of the last one
static uint32_t s_fsGen      = 0;       // the model's gen the record holds
static uint32_t s_fsWrittenMs = 0;
static uint32_t s_fsWrites = 0, s_fsFails = 0, s_fsLastMs = 0;
static bool     s_fsWriting = false;
static bool     s_fsRenamePending = false;   // a whole last.tmp is on flash and still to be renamed
static File     s_fsFile;
static size_t   s_fsPos = 0;
static uint32_t s_fsStartMs = 0, s_fsWriteGen = 0;

// ── accessors ───────────────────────────────────────────────────────────────
bool ready() { return s_st != nullptr; }
const Model &model() { return s_st->m; }
const mks::Settings &settings() { return s_st->s; }
const char *deviceId() { return s_st ? s_st->dev : ""; }
bool bridgeOnline() { return s_bridgeOnline; }
bool haveBridge() { return s_bridgeHave; }
uint32_t lastRxMs() { return s_rxMs; }
const char *selectedTicker() { return s_st ? mks::effectiveTicker(s_st->s) : ""; }
PageBuf &pageBuf() { return s_st->page; }

// ── the settings into the model ─────────────────────────────────────────────
// The watch lists give the model its slots; a symbol kept keeps its series.
static void applyWatch() {
  const char *ps[kMaxWatch], *pn[kMaxWatch];
  for (int t = 0; t < 2; t++) {
    const int n = mks::parseSymList(mks::str(s_st->s, t ? mks::I_TICKERS : mks::I_INDICES), s_st->watchSyms,
                                    s_st->watchNames, kMaxWatch);
    for (int i = 0; i < n; i++) { ps[i] = s_st->watchSyms[i]; pn[i] = s_st->watchNames[i]; }
    setWatch(s_st->m, s_st->sp, t == 1, ps, pn, (uint8_t)(n < 0 ? 0 : n));
  }
}

// A stored display.ticker the tickers list no longer has (settings from an
// older build, or a local defaults header that disagrees with itself): the
// first ticker takes its place, saved at the next settle, and the log says so.
static void clampTicker() {
  if (!*mks::str(s_st->s, mks::I_TICKER) || mks::tickerListed(s_st->s)) return;
  char first[kSymLen];
  snprintf(first, sizeof(first), "%s", mks::effectiveTicker(s_st->s));
  Serial.printf("[market] display.ticker %s is not in tickers: %s instead\n", mks::str(s_st->s, mks::I_TICKER),
                first[0] ? first : "none");
  if (first[0]) mks::setTicker(s_st->s, first);
  else mks::str(s_st->s, mks::I_TICKER)[0] = '\0';
  s_st->settle.note(millis(), false);
}

// ── ingest ──────────────────────────────────────────────────────────────────
static void refuse(const char *leaf, const char *why, uint16_t len) {
  if (s_refused < 65535) s_refused++;
  snprintf(s_st->refusal, sizeof(s_st->refusal), "%.24s: %.20s", leaf, why);
  Serial.printf("[market] refused %s (%s, %u B)\n", leaf, why, (unsigned)len);
}

static void onMessage(const char *topic, const uint8_t *payload, uint16_t len) {
  if (!s_st) return;
  const char *leaf = topic + strlen(s_st->root);
  if (!strcmp(leaf, "config")) return;   // ours, echoed back: the subscription is market/#
  if (!strcmp(leaf, "ha")) {
    if (!len) { s_bridgeHave = false; return; }   // a cleared retained topic
    if (len == 6 && !memcmp(payload, "online", 6))       { s_bridgeHave = true; s_bridgeOnline = true; }
    else if (len == 7 && !memcmp(payload, "offline", 7)) { s_bridgeHave = true; s_bridgeOnline = false; }
    else refuse(leaf, "not online or offline", len);
    return;
  }
  if (!len) return;                      // cleared: keep what is shown

  JsonDocument doc(&s_alloc);
  const DeserializationError err = deserializeJson(doc, (const char *)payload, len);
  if (s_alloc.peak() > s_jsonPeak) s_jsonPeak = s_alloc.peak();
  if (err) { refuse(leaf, err.c_str(), len); return; }
  char ticker[kSymLen];
  snprintf(ticker, sizeof(ticker), "%s", mks::effectiveTicker(s_st->s));
  const char *why = ingest(s_st->m, s_st->sp, leaf, doc.as<JsonObjectConst>(), ticker);
  if (why) { refuse(leaf, why, len); return; }
  s_rxMs = millis() | 1;
  s_accepted++;
  // The history goes to LittleFS; the quotes, the session and the tape are
  // this minute's and would be stale after a reboot anyway.
  if (strcmp(leaf, "live") && strcmp(leaf, "tape") && strncmp(leaf, "intraday/", 9)) {
    s_fsDirty   = true;
    s_fsDirtyMs = millis();
  }
}

// The device id needs the MAC, which mqtt_bus reads the same way for its
// client id. Retried from marketLoop() until it is known.
static void ensureTopics() {
  if (s_topics || !s_st) return;
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  if (!(mac[3] | mac[4] | mac[5])) return;
  // -Wformat-truncation here is a false alarm: three bytes are six hex digits.
  snprintf(s_st->dev, sizeof(s_st->dev), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  const int n = snprintf(s_st->root, sizeof(s_st->root), MQTT_BASE "/%s/market/", s_st->dev);
  if (n <= 0 || (size_t)n + 8 >= sizeof(s_st->root)) { Serial.println("[market] MQTT_BASE too long for the topics"); return; }
  snprintf(s_st->sub, sizeof(s_st->sub), "%s#", s_st->root);
  snprintf(s_st->tConfig, sizeof(s_st->tConfig), "%sconfig", s_st->root);
  s_topics     = true;
  s_handler    = mqttBusOnMessage(s_st->root, onMessage);
  s_subscribed = mqttBusSubscribe(s_st->sub);
  Serial.printf("[market] topics %s{config,status,index/+/+,ticker/+/+,portfolio/+/+,holdings/+,live,intraday/+,tape,ha}%s%s\n",
                s_st->root, s_handler ? "" : " - MQTT bus is full: handler REFUSED",
                s_subscribed ? "" : " - MQTT bus is full: subscription REFUSED");
}

// ── NVS: one blob ───────────────────────────────────────────────────────────
static void loadSettings() {
  mks::defaults(s_st->s);
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error on
  // every boot (src/panel/panel.cpp, arduino-esp32 2.0.17). isKey() first.
  if (!p.begin(NVS_NS, false)) { snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "no namespace"); return; }
  if (p.isKey(NVS_BLOB)) {
    const size_t n = p.getBytesLength(NVS_BLOB);
    if (n > 0 && n < mks::kBlobMax && p.getBytes(NVS_BLOB, s_st->blob, n) == n) {
      const int rows = mks::blobRead(s_st->blob, n, s_st->s);
      s_blobBytes = n;
      snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "%d rows, %u B", rows, (unsigned)n);
    } else {
      snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "blob unreadable");
    }
  } else {
    snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "defaults");
  }
  p.end();
}

// Now. False when the blob does not fit or NVS refused it; retried after a settle.
static bool writeSettings() {
  const size_t n = mks::blobWrite(s_st->s, s_st->blob, mks::kBlobMax);
  if (!n) {
    s_nvsFails++;
    snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "blob too long");
    return false;
  }
  Preferences p;
  bool ok = p.begin(NVS_NS, false);
  if (ok) {
    ok = p.putBytes(NVS_BLOB, s_st->blob, n) == n;
    p.putUShort(NVS_VER, mks::kSchema);
    p.end();
  }
  if (ok) {
    s_nvsWrites++;
    s_blobBytes = n;
    s_nvsRetry  = false;
    snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "saved %u B", (unsigned)n);
  } else {
    s_nvsFails++;
    s_nvsRetry   = true;
    s_nvsRetryMs = millis();
    snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "write failed");
  }
  return ok;
}

// The knob's changes once it has been quiet: one NVS write, and one config
// publish when the ticker on screen is not the one last published. A failed
// write is tried again after another settle.
static void settleTick() {
  const uint32_t ms = millis();
  const uint8_t act = s_st->settle.tick(ms, (uint32_t)MARKET_NVS_SETTLE_MS);
  if (act & SETTLE_SAVE) writeSettings();
  else if (s_nvsRetry && (uint32_t)(ms - s_nvsRetryMs) >= (uint32_t)MARKET_NVS_SETTLE_MS) writeSettings();
  if (act & SETTLE_PUBLISH) s_cfgToSend = true;   // the gate drops it if the ticker came back
}

void noteWindow(uint8_t preset) {
  if (!s_st || !mks::setWindow(s_st->s, preset)) return;
  s_st->settle.note(millis(), false);   // the panel's own: the app never hears it
}

void noteTicker(const char *sym) {
  if (!s_st || !mks::setTicker(s_st->s, sym)) return;
  s_st->settle.note(millis(), true);    // the app polls this one intraday, once the knob rests
}

// ── the config out ──────────────────────────────────────────────────────────
// Retained, so the app learns the settings after its own restart as well as
// at the moment they change; sent again on every reconnect, since the broker
// may have lost it. Streamed: with every row of docs/19 the payload passes the
// bus buffer's 1 900 B (mqttBusPublishLarge). Held while the knob is still
// turning, so a stop on the way never goes out.
static void configTick() {
  const bool up = mqttBusConnected();
  // Every connect, by the bus's counter: a reconnect inside one mqttBusLoop()
  // never shows as a down-up edge here, and a broker without persistence would
  // be left with no retained config.
  if (s_st->connects.fresh(mqttBusConnects())) { s_cfgToSend = true; s_cfgForce = true; s_cfgTooBig = false; }
  if (!up || !s_cfgToSend || s_cfgTooBig || !s_topics || s_st->settle.pending()) return;
  JsonDocument doc(&s_setAlloc);
  mks::payloadJson(s_st->s, doc.to<JsonObject>());
  if (doc.overflowed()) { s_cfgTooBig = true; Serial.println("[market] config: JSON memory"); return; }
  const size_t n = measureJson(doc);
  if (n >= mks::kConfigMax) {
    s_cfgTooBig = true;
    s_cfgBytes  = n;
    Serial.printf("[market] config %u B over %u: not published\n", (unsigned)n, (unsigned)mks::kConfigMax);
    return;
  }
  serializeJson(doc, s_st->cfg, mks::kConfigMax);
  const uint32_t crc = crc32((const uint8_t *)s_st->cfg, n);
  if (!s_cfgForce && !s_st->gate.differs(n, crc)) {
    s_cfgToSend    = false;
    s_cfgUnchanged = true;
    s_cfgSkipped++;
    return;
  }
  if (mqttBusPublishLarge(s_st->tConfig, (const uint8_t *)s_st->cfg, n, true)) {
    s_st->gate.sent(n, crc);
    s_cfgToSend    = false;
    s_cfgForce     = false;
    s_cfgUnchanged = false;
    s_cfgBytes  = n;
    s_cfgSentMs = millis() | 1;
    s_cfgSent++;
    snprintf(s_st->cfgTicker, sizeof(s_st->cfgTicker), "%s", doc["ticker"] | "");
  }
}

// A portal save or reset: the slots follow the lists, NVS now, then the
// config out, in that order, so what the app reads is what flash holds. The
// knob's pending change is in this write too.
static void portalChanged(const bool *changed) {
  if (!changed || changed[mks::I_INDICES] || changed[mks::I_TICKERS]) applyWatch();
  clampTicker();
  s_st->settle.clear();
  writeSettings();
  s_cfgToSend = true;
  s_cfgTooBig = false;
  configTick();
}

// ── LittleFS: the last records ──────────────────────────────────────────────
// A file that reads as a whole record: its length, then recordRead's magic,
// version, size and CRC. On a reason the model may be half written.
static const char *readRecordFile(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "cannot open";
  const size_t n = f.size();
  if (n != recordSize()) { f.close(); return "wrong length"; }
  const size_t got = f.read(s_rec, n);
  f.close();
  if (got != n) return "short read";
  return recordRead(s_rec, n, &s_st->m);
}

static void fsBegin() {
  s_fsReady = LittleFS.begin(true);   // already mounted by the animation store: returns true again
  if (!s_fsReady) { snprintf(s_st->fsNote, sizeof(s_st->fsNote), "mount failed"); return; }
  if (!LittleFS.exists(FS_DIR)) LittleFS.mkdir(FS_DIR);
  // A temporary file that reads whole is a write that finished everything but
  // the rename: the newest record there is, so it takes the record's place,
  // whatever last.bin holds. One that does not read is a write cut short.
  if (LittleFS.exists(FS_TMP)) {
    const char *why = readRecordFile(FS_TMP);
    if (!why) {
      // Whole: the model is the newest record, renamed or not. A rename that
      // fails leaves last.tmp where it is, retried before the next write.
      s_fsHave = true;
      s_fsRenamePending = !LittleFS.rename(FS_TMP, FS_PATH);
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), s_fsRenamePending ? "last.tmp read, rename failed, gen %lu"
                                                                     : "promoted last.tmp, gen %lu",
               (unsigned long)s_st->m.gen);
    } else {
      memset(&s_st->m, 0, sizeof(Model));
      LittleFS.remove(FS_TMP);
    }
  }
  if (!s_fsHave) {
    const char *why = LittleFS.exists(FS_PATH) ? readRecordFile(FS_PATH) : "no record";
    if (why) {
      memset(&s_st->m, 0, sizeof(Model));
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "%s", why);
    } else {
      s_fsHave = true;
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "read %u B, gen %lu", (unsigned)recordSize(), (unsigned long)s_st->m.gen);
    }
  }
  s_fsGen = s_st->m.gen;
}

// The record is written in MARKET_FS_CHUNK pieces, one per pass, into a
// temporary file that replaces the record when complete: the picture never
// waits for a flash write. The rename replaces the old record in one littlefs
// metadata commit (README, "LittleFS"), so a power cut leaves the old record
// or the new one, never neither.
static void fsTick() {
  if (!s_fsReady) return;
  const uint32_t ms = millis();
  if (s_fsWriting) {
    const size_t total = recordSize();
    const size_t n = total - s_fsPos < (size_t)MARKET_FS_CHUNK ? total - s_fsPos : (size_t)MARKET_FS_CHUNK;
    if (s_fsFile.write(s_rec + s_fsPos, n) != n) {
      s_fsFile.close();
      LittleFS.remove(FS_TMP);
      s_fsWriting = false;
      s_fsFails++;
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "write failed at %u", (unsigned)s_fsPos);
      return;
    }
    s_fsPos += n;
    if (s_fsPos < total) return;
    s_fsFile.close();
    const bool ok = LittleFS.rename(FS_TMP, FS_PATH);
    s_fsWriting = false;
    s_fsGen       = s_fsWriteGen;     // on flash either way: last.bin, or a whole last.tmp the boot promotes
    s_fsWrittenMs = ms | 1;
    if (!ok) {
      s_fsRenamePending = true;
      s_fsFails++;
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "rename failed: last.tmp kept");
      return;
    }
    s_fsWrites++;
    s_fsLastMs    = ms - s_fsStartMs;
    s_fsHave      = true;
    snprintf(s_st->fsNote, sizeof(s_st->fsNote), "wrote %u B in %lu ms", (unsigned)total, (unsigned long)s_fsLastMs);
    return;
  }
  if (!s_fsDirty || (uint32_t)(ms - s_fsDirtyMs) < (uint32_t)MARKET_FS_SETTLE_MS) return;
  if (s_fsWrittenMs && (uint32_t)(ms - s_fsWrittenMs) < (uint32_t)MARKET_FS_MIN_GAP_MS) return;
  s_fsDirty = false;
  if (s_st->m.gen == s_fsGen) return;
  // A whole last.tmp waiting for its rename: renamed first, since the write
  // below would truncate it. Still failing, the write waits too, so a power
  // cut cannot take the only whole copy; tried again at the next write.
  if (s_fsRenamePending) {
    if (!LittleFS.rename(FS_TMP, FS_PATH)) {
      s_fsFails++;
      s_fsDirty = true;
      s_fsDirtyMs = ms;
      s_fsWrittenMs = ms | 1;   // the next attempt after MARKET_FS_MIN_GAP_MS
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "rename failed again: last.tmp kept");
      return;
    }
    s_fsRenamePending = false;
  }
  // Only a record that fits is written. littlefs (f53a0cc, pinned by esp_littlefs 41873c2) divides by
  // cfg->block_count in its own "No more free space" log line (lfs.c:689-691), and esp_littlefs sets that
  // to 0 to autodetect the block count (esp_littlefs.c:945): a write that runs out of space reboots the
  // panel with IntegerDivideByZero instead of failing. Seen 2026-09-15: 12 KB free of 3.5 MB after the
  // animation uploads, a reboot every ~16 s while the market data kept arriving. The margin covers the
  // file's block pointers and metadata, and small writes from other modules during the ~21 passes.
  {
    const size_t totalB = LittleFS.totalBytes(), usedB = LittleFS.usedBytes();
    const size_t freeB  = totalB > usedB ? totalB - usedB : 0;
    const size_t needB  = recordSize() + recordSize() / 4 + 4 * 4096;
    s_fsFreeBytes = freeB;
    if (freeB < needB) {
      s_fsNoSpace++;
      snprintf(s_st->fsNote, sizeof(s_st->fsNote), "no space: needs %u B, %u B free", (unsigned)needB, (unsigned)freeB);
      s_fsGen       = s_st->m.gen;   // not tried again for this generation
      s_fsWrittenMs = ms | 1;        // nor before MARKET_FS_MIN_GAP_MS
      return;
    }
  }
  if (!recordWrite(s_st->m, s_rec, recordSize())) return;
  s_fsFile = LittleFS.open(FS_TMP, "w");
  if (!s_fsFile) { s_fsFails++; snprintf(s_st->fsNote, sizeof(s_st->fsNote), "cannot open tmp"); return; }
  s_fsWriting  = true;
  s_fsPos      = 0;
  s_fsStartMs  = ms;
  s_fsWriteGen = s_st->m.gen;
}

// ── lifecycle ───────────────────────────────────────────────────────────────
}  // namespace market

void marketBegin() {
  using namespace market;
  s_st = static_cast<Store *>(heap_caps_calloc(1, sizeof(Store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_st) s_rec = static_cast<uint8_t *>(heap_caps_calloc(1, recordSize(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_st || !s_rec) {
    Serial.printf("[market] no PSRAM for the store (%u + %u B): the pages and the portal say so\n", (unsigned)sizeof(Store),
                  (unsigned)recordSize());
    if (s_st) heap_caps_free(s_st);
    s_st = nullptr;
    return;
  }
  mks::setScratch(&s_st->msc);          // before any settings call
  mks::setJsonAllocator(&s_setAlloc);
  if (!mks::poolFits()) Serial.println("[market] settings pool too small: rows will be cut");
  if (const char *bad = mks::checkLocalDefaults(s_st->scratch))   // the spare Settings: no heap
    Serial.printf("[market] local default %s does not read: the neutral default instead\n", bad);
  snprintf(s_st->nvsNote, sizeof(s_st->nvsNote), "not read");
  snprintf(s_st->fsNote, sizeof(s_st->fsNote), "not mounted");
  mks::defaults(s_st->dflt);
  loadSettings();
  fsBegin();
  applyWatch();      // the slots follow the settings; a record's series for a symbol kept stay
  clampTicker();
  ensureTopics();
  s_cfgToSend = true;
  s_cfgForce  = true;
  Serial.printf("[market] store %u B in PSRAM, record %u B, settings %s (%s defaults), LittleFS %s\n",
                (unsigned)sizeof(Store), (unsigned)recordSize(), s_st->nvsNote, mks::localDefaults() ? "local" : "neutral",
                s_st->fsNote);
}

void marketLoop() {
  using namespace market;
  if (!s_st) return;
  ensureTopics();
  settleTick();
  configTick();
  fsTick();
  pageTick();
}

const char *marketPageName(uint8_t sub) { return market::kPageNames[sub < market::PG_COUNT ? sub : 0]; }

bool marketPageEnabled(uint8_t sub) {
  using namespace market;
  if (!s_st || sub >= PG_COUNT) return true;
  return mks::flag(s_st->s, (mks::Idx)(mks::I_PAGE_MARKETS + sub));
}

uint16_t marketDwellS() {
  using namespace market;
  return s_st ? (uint16_t)mks::option(s_st->s, mks::I_DWELL_S) : 20;
}

uint32_t marketEnterTimeoutMs() { return MARKET_ENTER_TIMEOUT_MS; }

// ── the portal ──────────────────────────────────────────────────────────────
static void dateJson(JsonVariant out, int32_t days) {
  if (days == market::kNoDate) { out.set(nullptr); return; }
  int y, m, d;
  market::civilFromDays(days, &y, &m, &d);
  char iso[12];
  snprintf(iso, sizeof(iso), "%04d-%02d-%02d", y, m, d);
  out.set(iso);
}

static void seriesJson(JsonObject o, const market::Series &p) {
  o["have"] = p.have;
  if (!p.have) return;
  o["cur"]  = (const char *)p.cur;
  o["last"] = p.last;
  o["chg"]  = p.chg;
  o["hi"]   = p.hi;
  o["lo"]   = p.lo;
  if (p.hasCagr) o["cagr"] = p.cagr;
  if (p.hasAnn) o["ann"] = p.ann;
  o["mdd"]  = p.mdd;
  dateJson(o["from"].to<JsonVariant>(), p.from);
  dateJson(o["to"].to<JsonVariant>(), p.to);
}

void marketWebJson(JsonObject out) {
  using namespace market;
  out["ready"]      = ready();
  out["dev"]        = deviceId();
  out["root"]       = s_st ? s_st->root : "";
  out["handler"]    = s_handler;
  out["subscribed"] = s_subscribed;
  out["bridge"]     = !s_bridgeHave ? "unknown" : (s_bridgeOnline ? "online" : "offline");
  out["accepted"]   = s_accepted;
  out["refused"]    = s_refused;
  out["lastRefusal"] = s_st ? s_st->refusal : "";
  out["jsonPeak"]   = (uint32_t)s_jsonPeak;
  if (s_rxMs) out["rxAgoS"] = (millis() - s_rxMs) / 1000UL;
  if (!s_st) {
    out["storeBytes"]  = (uint32_t)sizeof(Store);
    out["recordBytes"] = (uint32_t)recordSize();
    return;
  }
  const Model &m = s_st->m;
  const mks::Settings &s = s_st->s;

  mks::configJson(s, out["config"].to<JsonObject>());
  mks::registryJson(out["registry"].to<JsonObject>(), s_st->dflt);
  out["defaults"] = mks::localDefaults() ? "local" : "neutral";
  JsonObject nvs = out["nvs"].to<JsonObject>();
  nvs["saved"]  = !s_st->settle.pending() && !s_nvsRetry;
  nvs["note"]   = (const char *)s_st->nvsNote;
  nvs["bytes"]  = (uint32_t)s_blobBytes;
  nvs["writes"] = s_nvsWrites;
  nvs["fails"]  = s_nvsFails;
  nvs["max"]    = (uint32_t)mks::kBlobMax;
  JsonObject cfg = out["cfgOut"].to<JsonObject>();
  cfg["sent"]   = !s_cfgToSend;
  cfg["tooBig"] = s_cfgTooBig;
  cfg["bytes"]  = (uint32_t)s_cfgBytes;
  cfg["max"]    = (uint32_t)mks::kConfigMax;
  cfg["count"]  = s_cfgSent;
  cfg["unchanged"] = s_cfgUnchanged;
  cfg["skipped"] = s_cfgSkipped;
  cfg["ticker"] = (const char *)s_st->cfgTicker;
  if (s_cfgSentMs) cfg["agoS"] = (millis() - s_cfgSentMs) / 1000UL;
  JsonObject fs = out["fs"].to<JsonObject>();
  fs["ready"]   = s_fsReady;
  fs["free"]    = (unsigned long)s_fsFreeBytes;
  fs["noSpace"] = s_fsNoSpace;
  fs["have"]    = s_fsHave;
  fs["note"]    = (const char *)s_st->fsNote;
  fs["bytes"]   = (uint32_t)recordSize();
  fs["writes"]  = s_fsWrites;
  fs["fails"]   = s_fsFails;
  fs["lastMs"]  = s_fsLastMs;
  fs["pending"] = s_fsDirty || s_fsWriting;
  fs["renamePending"] = s_fsRenamePending;
  fs["gen"]     = s_fsGen;

  const uint8_t preset = mks::windowPreset(s);
  const uint8_t mode   = mks::shownMode(s);
  JsonObject view = out["view"].to<JsonObject>();
  view["window"] = kPresetKeys[preset];
  view["mode"]   = kModeKeys[mode];
  view["ticker"] = mks::effectiveTicker(s);
  JsonArray avail = view["presets"].to<JsonArray>();
  const uint16_t mask = presetsAvailable(m);
  for (uint8_t p = 0; p < kPresets; p++)
    if (mask & (1u << p)) avail.add(kPresetKeys[p]);

  JsonObject md = out["model"].to<JsonObject>();
  md["gen"]  = m.gen;
  md["bytes"] = (uint32_t)sizeof(Model);
  for (int t = 0; t < 2; t++) {
    JsonArray list = md[t ? "tickers" : "indices"].to<JsonArray>();
    const Symbol *syms = t ? m.tk : m.idx;
    const uint8_t n = t ? m.nTk : m.nIdx;
    for (uint8_t i = 0; i < n; i++) {
      JsonObject o = list.add<JsonObject>();
      o["sym"]  = (const char *)syms[i].sym;
      o["name"] = (const char *)syms[i].name;
      uint16_t have = 0;
      for (uint8_t p = 0; p < kPresets; p++) if (syms[i].p[p].have) have |= (uint16_t)(1u << p);
      o["windows"] = have;
      seriesJson(o["w"].to<JsonObject>(), syms[i].p[preset]);
      for (uint8_t q = 0; q < m.live.n; q++) {
        const Quote &lq = m.live.q[q];
        if (strcmp(lq.sym, syms[i].sym)) continue;
        JsonObject l = o["live"].to<JsonObject>();
        l["last"]  = lq.last;
        if (lq.hasDay) l["day"] = lq.day;
        l["state"] = kExStateKeys[lq.state < X_COUNT ? lq.state : (uint8_t)X_NONE];
        l["feed"]  = kFeedKeys[lq.feed < F_COUNT ? lq.feed : (uint8_t)F_NONE];
        l["delayS"] = lq.delayS;
      }
    }
  }
  JsonObject pf = md["portfolio"].to<JsonObject>();
  for (uint8_t k = 0; k < M_COUNT; k++) {
    JsonObject o = pf[kModeKeys[k]].to<JsonObject>();
    const Portfolio &p = m.pf[k][preset];
    o["have"] = p.have;
    uint16_t have = 0;
    for (uint8_t q = 0; q < kPresets; q++) if (m.pf[k][q].have) have |= (uint16_t)(1u << q);
    o["windows"] = have;
    if (!p.have) continue;
    o["cur"] = (const char *)p.cur;
    o["value"] = p.value;
    o["chg"] = p.chg;
    o["sinceStart"] = p.sinceStart;
    if (p.hasCagr) o["cagr"] = p.cagr;
    o["mdd"] = p.mdd;
    o["div"] = p.div;
    o["terDrag"] = p.terDrag;
    o["cash"] = p.cash;
    o["bench"] = p.hasBench;
    o["gross"] = p.hasGross;
    if (p.hasTwr) o["twr"] = p.twr;
    if (p.hasAnn) o["ann"] = p.ann;
    if (p.hasXirr) o["xirrAnn"] = p.xirrAnn;
    o["flows"] = p.flows;
    dateJson(o["mddPeak"].to<JsonVariant>(), p.mddPeak);
    dateJson(o["mddTrough"].to<JsonVariant>(), p.mddTrough);
    dateJson(o["mddRecovery"].to<JsonVariant>(), p.mddRecovery);
    dateJson(o["from"].to<JsonVariant>(), p.from);
    dateJson(o["to"].to<JsonVariant>(), p.to);
  }
  JsonObject hd = md["holdings"].to<JsonObject>();
  for (uint8_t k = 0; k < M_COUNT; k++) {
    JsonObject o = hd[kModeKeys[k]].to<JsonObject>();
    const Holdings &h = m.hd[k];
    o["have"] = h.have;
    if (!h.have) continue;
    dateJson(o["asof"].to<JsonVariant>(), h.asof);
    o["cashNow"] = h.cashNow;
    o["dropped"] = h.dropped;
    JsonArray rows = o["rows"].to<JsonArray>();
    for (uint8_t i = 0; i < h.n; i++) {
      JsonObject r = rows.add<JsonObject>();
      r["sym"] = (const char *)h.rows[i].sym;
      r["tgt"] = h.rows[i].tgt;
      r["now"] = h.rows[i].now;
      if (h.rows[i].hasRet) r["ret"] = h.rows[i].ret;
      dateJson(r["entry"].to<JsonVariant>(), h.rows[i].entry);
    }
  }
  JsonObject live = md["live"].to<JsonObject>();
  live["have"] = m.live.have;
  live["n"] = m.live.n;
  live["dropped"] = m.live.dropped;
  if (m.live.have) live["ts"] = m.live.ts;
  JsonObject intra = md["intraday"].to<JsonObject>();
  intra["have"] = m.intra.have;
  if (m.intra.have) { intra["sym"] = (const char *)m.intra.sym; intra["n"] = m.intra.n; intra["ts"] = m.intra.ts; }
  JsonObject tape = md["tape"].to<JsonObject>();
  tape["have"] = m.tape.have;
  if (m.tape.have) {
    tape["ts"] = m.tape.ts;
    JsonArray x = tape["x"].to<JsonArray>();
    for (uint8_t i = 0; i < m.tape.n; i++) {
      JsonObject o = x.add<JsonObject>();
      o["n"] = (const char *)m.tape.x[i].name;
      o["s"] = kExStateKeys[m.tape.x[i].state < X_COUNT ? m.tape.x[i].state : (uint8_t)X_NONE];
      o["t"] = (const char *)m.tape.x[i].next;
    }
  }
  JsonObject st = md["status"].to<JsonObject>();
  st["have"] = m.status.have;
  if (m.status.have) {
    dateJson(st["asof"].to<JsonVariant>(), m.status.asof);
    st["fetched"] = (const char *)m.status.fetched;
    st["state"] = kAppStateKeys[m.status.state < A_COUNT ? m.status.state : (uint8_t)A_ERROR];
    st["err"] = (const char *)m.status.err;
    JsonArray syms = st["symbols"].to<JsonArray>();
    for (uint8_t i = 0; i < m.status.n; i++) {
      JsonObject o = syms.add<JsonObject>();
      o["sym"] = (const char *)m.status.s[i].sym;
      dateJson(o["asof"].to<JsonVariant>(), m.status.s[i].asof);
      o["bars"] = m.status.s[i].bars;
      if (m.status.s[i].hasTer) o["ter"] = m.status.s[i].ter;
      o["terSrc"] = (const char *)m.status.s[i].terSrc;
    }
  }
}

int marketWebPost(JsonObjectConst in, char *err, size_t cap, int *showSub) {
  using namespace market;
  static const char *const kKeys[] = {"config", "show", "republish", "reset"};
  *showSub = -1;
  int present = 0;
  for (JsonPairConst kv : in) {
    bool known = false;
    for (const char *k : kKeys) known = known || !strcmp(kv.key().c_str(), k);
    if (!known) { snprintf(err, cap, "unknown key: send one of config, show, republish, reset"); return 400; }
    present++;
  }
  if (present != 1) { snprintf(err, cap, "send exactly one of config, show, republish, reset"); return 400; }
  if (!s_st) { snprintf(err, cap, "the market pages have no memory on this panel"); return 503; }

  JsonVariantConst v = in["config"];
  if (!v.isNull()) {
    bool changed[mks::I_COUNT];
    if (mks::apply(v.as<JsonObjectConst>(), s_st->s, s_st->scratch, err, cap, changed)) return 400;
    portalChanged(changed);
    return 200;
  }
  if (!(v = in["show"]).isNull()) {
    const char *name = v.is<const char *>() ? v.as<const char *>() : nullptr;
    for (uint8_t p = 0; name && p < PG_COUNT; p++)
      if (!strcasecmp(name, kPageNames[p])) { *showSub = p; return 200; }
    snprintf(err, cap, "show must be markets, ticker, portfolio or holdings");
    return 400;
  }
  if (!(v = in["republish"]).isNull()) {
    if (!v.is<bool>() || !v.as<bool>()) { snprintf(err, cap, "republish must be true"); return 400; }
    s_cfgToSend = true;
    s_cfgForce  = true;
    s_cfgTooBig = false;
    return 200;
  }
  v = in["reset"];
  const char *group = v.is<const char *>() ? v.as<const char *>() : nullptr;
  if (!group) { snprintf(err, cap, "reset must be a group key or all"); return 400; }
  int g = -1;
  for (int k = 0; k < mks::G_COUNT; k++)
    if (!strcmp(group, mks::kGroupKeys[k])) g = k;
  if (g < 0 && strcmp(group, "all")) { snprintf(err, cap, "reset: no such group"); return 400; }
  bool changed[mks::I_COUNT];
  for (int i = 0; i < mks::I_COUNT; i++) {
    changed[i] = false;
    if (g >= 0 && mks::kDefs[i].group != g) continue;
    if (mks::isDefault(s_st->s, (mks::Idx)i)) continue;
    if (mks::fromText(s_st->s, (mks::Idx)i, mks::defaultText((mks::Idx)i)))
      mks::fromText(s_st->s, (mks::Idx)i, mks::kDefs[i].def);   // a local row that does not read: the neutral one
    changed[i] = true;
  }
  portalChanged(changed);
  return 200;
}

#endif  // MARKET_ENABLED

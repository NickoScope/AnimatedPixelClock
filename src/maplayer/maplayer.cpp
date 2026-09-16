// The panel as a Music Assistant player: the client half.
//
// This file owns the socket and the protocol state machine. It does NOT own
// any audio hardware: no I2S, no ES8311 register is written here, and the
// amplifier is never enabled. That half lives behind MAPLAYER_AUDIO_ENABLED,
// which refuses to build - see maplayer.h for the two things that must land
// first, and docs/25-ma-media-player.md §7 for why that order is not caution
// but arithmetic.
//
// What this half is for, then: to prove the transport on real hardware without
// risking the heap that the microphones already nearly exhausted. It connects,
// speaks the protocol, tracks volume both ways, counts everything, and reports
// through /api/info. When the output half is built, it plugs into pcmReady().

#include "maplayer.h"

#if defined(MAPLAYER_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "../config/config.h"   // FIRMWARE_VERSION, as every module here takes it
#include "snap_proto.h"

namespace {

// ── tunables, all named as choices rather than standards ────────────────────

// The assembly buffer has to hold one base header plus the largest body the
// server sends. At the built-in snapserver's defaults - 48 kHz, 16 bit, stereo,
// chunk_ms 26 (read from MA's own provider.py) - a wire chunk's payload is
// 48000 * 2 * 2 * 0.026 = about 5 KB. 16 KB leaves room for a server configured
// with a longer chunk without a reallocation on the audio path.
constexpr size_t kRxBufferBytes = 16 * 1024;

// WiFiClient::connect() blocks. src/mqtt/mqtt_bus learned this the hard way
// (debt D3 of docs/22 §12.3: a dead broker held loop() for 3 s every 5 s), and
// the lesson is written into this module from the start rather than after an
// incident: the timeout is set in SECONDS, and a connect is only attempted on
// the backoff schedule, never every pass.
constexpr uint32_t kConnectTimeoutS = 1;

// How long without a single byte before the link is treated as gone. The
// server sends wire chunks continuously while a stream is playing and nothing
// at all while it is idle, so this only fires on a socket that is open and
// dead - which a TCP socket can be for minutes without noticing.
constexpr uint32_t kRxSilenceMs = 15000;

// NVS namespace of this module alone, like src/ir's "irmap": a server host
// survives a reflash without touching the shared settings blob.
constexpr char kNvsNamespace[] = "maplayer";
constexpr char kNvsHostKey[] = "host";
constexpr char kNvsVolumeKey[] = "vol";

// ── state ───────────────────────────────────────────────────────────────────

maplayer::State s_state = maplayer::State::Off;
maplayer::Stats s_stats;
maplayer::Volume s_volume;
maplayer::HeapGate s_gate;
maplayer::GateVerdict s_lastVerdict = maplayer::GateVerdict::Ok;

WiFiClient s_client;
char s_host[64] = {0};
uint16_t s_port = MAPLAYER_SERVER_PORT;

uint8_t *s_rx = nullptr;         // PSRAM: the assembly buffer
size_t s_rxLen = 0;              // bytes held
bool s_haveBase = false;
snap::Base s_base;

uint32_t s_backoffMs = 0;
uint32_t s_retryAtMs = 0;
uint32_t s_lastRxMs = 0;
uint32_t s_lastTimeSyncMs = 0;
uint16_t s_msgId = 1;

snap::PcmFormat s_format;
bool s_formatKnown = false;
long s_bufferMs = -1;
int64_t s_serverOffsetUs = 0;
bool s_offsetKnown = false;

// A volume we sent ourselves comes back in the next Server Settings. Without
// this, that echo would be read as the server overriding the user and written
// straight back - the loop AirPlay guards against with its own echo grace.
uint32_t s_volumeEchoUntilMs = 0;
constexpr uint32_t kVolumeEchoGraceMs = 2000;

// ── small helpers ───────────────────────────────────────────────────────────

uint32_t freeInternal() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
uint32_t largestBlock() {
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

snap::TimePoint nowTimePoint() {
  const uint32_t ms = millis();
  snap::TimePoint t;
  t.sec = (int32_t)(ms / 1000UL);
  t.usec = (int32_t)((ms % 1000UL) * 1000UL);
  return t;
}

void loadHost() {
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  const String h = p.getString(kNvsHostKey, "");
  const int v = p.getInt(kNvsVolumeKey, -1);
  p.end();
  strncpy(s_host, h.c_str(), sizeof(s_host) - 1);
  s_host[sizeof(s_host) - 1] = '\0';
  if (v >= 0) {
    s_volume.level = maplayer::clampVolume(v);
    s_volume.known = true;
  }
}

void saveVolume() {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putInt(kNvsVolumeKey, s_volume.level);
  p.end();
}

void dropConnection(const char *why) {
  if (s_client.connected()) s_client.stop();
  if (s_state == maplayer::State::Streaming || s_state == maplayer::State::Greeting ||
      s_state == maplayer::State::WaitingCodec || s_state == maplayer::State::Connecting) {
    s_stats.drops++;
    Serial.printf("[maplayer] connection lost: %s\n", why);
  }
  s_rxLen = 0;
  s_haveBase = false;
  s_formatKnown = false;
  s_backoffMs = maplayer::nextBackoffMs(s_backoffMs);
  s_retryAtMs = millis() + s_backoffMs;
  s_state = maplayer::State::Lost;
}

// ── sending ─────────────────────────────────────────────────────────────────

bool sendFramed(snap::Type type, const char *json, uint32_t jsonLen) {
  uint8_t out[320];
  const size_t n =
      snap::frameJson(out, sizeof(out), type, s_msgId++, nowTimePoint(), json, jsonLen);
  if (n == 0) return false;
  return s_client.write(out, n) == n;
}

bool sendHello() {
  snap::HelloInfo info;
  const String mac = WiFi.macAddress();
  const String host = WiFi.getHostname() ? String(WiFi.getHostname()) : String("pixelclock");
  info.id = mac.c_str();
  info.mac = mac.c_str();
  info.hostName = host.c_str();
  info.clientName = "AnimatedPixelClock";
  info.version = FIRMWARE_VERSION;
  info.os = "arduino-esp32";
  info.arch = "xtensa";
  info.instance = 1;

  char json[288];
  const size_t n = snap::buildHelloJson(json, sizeof(json), info);
  if (n == 0) return false;
  return sendFramed(snap::Type::Hello, json, (uint32_t)n);
}

bool sendClientInfo() {
  char json[64];
  const size_t n = snap::buildClientInfoJson(json, sizeof(json), s_volume.level, s_volume.muted);
  if (n == 0) return false;
  s_volumeEchoUntilMs = millis() + kVolumeEchoGraceMs;
  return sendFramed(snap::Type::ClientInfo, json, (uint32_t)n);
}

void sendTime() {
  uint8_t body[8];
  const snap::TimePoint zero{0, 0};
  if (snap::writeTimeBody(body, sizeof(body), zero) != 8) return;
  uint8_t out[snap::kBaseSize + 8];
  const size_t n = snap::frame(out, sizeof(out), snap::Type::Time, s_msgId++, nowTimePoint(),
                               body, 8);
  if (n > 0) s_client.write(out, n);
  s_lastTimeSyncMs = millis();
}

// ── receiving ───────────────────────────────────────────────────────────────

void handleServerSettings(const uint8_t *body, uint32_t len) {
  const char *json = nullptr;
  uint32_t jsonLen = 0;
  if (!snap::parseJsonPayload(body, len, json, jsonLen)) {
    s_stats.badMessages++;
    return;
  }
  snap::ServerSettings ss;
  if (!snap::parseServerSettings(json, jsonLen, ss)) {
    s_stats.badMessages++;
    return;
  }
  if (ss.bufferMs >= 0) s_bufferMs = ss.bufferMs;
  const bool echo = (int32_t)(millis() - s_volumeEchoUntilMs) < 0;
  if (ss.volume >= 0 && !echo) {
    s_volume.level = maplayer::clampVolume(ss.volume);
    s_volume.known = true;
  }
  if (ss.haveMuted && !echo) s_volume.muted = ss.muted;
  if (s_state == maplayer::State::Greeting) s_state = maplayer::State::WaitingCodec;
}

void handleCodecHeader(const uint8_t *body, uint32_t len) {
  snap::CodecHeader h;
  if (!snap::parseCodecHeader(body, len, h)) {
    s_stats.badMessages++;
    return;
  }
  // Only "pcm" is accepted, and deliberately: carrying a decoder is the thing
  // this design exists to avoid (docs/25 §5.1). The server is told to send PCM
  // by setting snapcast_server_built_in_codec = pcm in Music Assistant.
  if (!snap::codecIs(h, "pcm")) {
    Serial.printf("[maplayer] refused: the server offers codec '%.*s', this build plays pcm only\n",
                  (int)h.codecLen, h.codec ? h.codec : "");
    s_state = maplayer::State::Refused;
    return;
  }
  snap::PcmFormat f;
  if (!snap::parseRiffWave(h.payload, h.size, f)) {
    Serial.println("[maplayer] refused: the pcm codec header is not a readable WAVE header");
    s_stats.badMessages++;
    s_state = maplayer::State::Refused;
    return;
  }
  if (!maplayer::formatPlayable(f.sampleRate, f.bits, f.channels)) {
    // Not a fault of the server: this board's bit clock is shared with the
    // microphone ADC, so one rate serves both directions (docs/25 §4).
    Serial.printf("[maplayer] refused: %u Hz %u-bit %u-channel; this board plays %u Hz %u-bit\n",
                  (unsigned)f.sampleRate, (unsigned)f.bits, (unsigned)f.channels,
                  (unsigned)maplayer::kSampleRate, (unsigned)maplayer::kBitsPerSample);
    s_state = maplayer::State::Refused;
    return;
  }
  s_format = f;
  s_formatKnown = true;
  s_state = maplayer::State::Streaming;
  Serial.printf("[maplayer] streaming: pcm %u Hz, %u bit, %u channels, server buffer %ld ms\n",
                (unsigned)f.sampleRate, (unsigned)f.bits, (unsigned)f.channels, s_bufferMs);
}

// Where the output half will attach. Today it only counts: nothing is played,
// and saying so in one place is better than a silent stub that looks wired up.
void pcmReady(const uint8_t *pcm, uint32_t bytes) {
  (void)pcm;
  (void)bytes;
  s_stats.chunks++;
}

void handleWireChunk(const uint8_t *body, uint32_t len) {
  snap::WireChunk wc;
  if (!snap::parseWireChunk(body, len, wc)) {
    s_stats.badMessages++;
    return;
  }
  if (!maplayer::playable(s_state)) {
    s_stats.chunksBeforeCodec++;   // the protocol forbids playing these
    return;
  }
  pcmReady(wc.payload, wc.size);
}

void handleTime(const uint8_t *body, uint32_t len) {
  snap::TimePoint latency;
  if (!snap::parseTimeBody(body, len, latency)) {
    s_stats.badMessages++;
    return;
  }
  // The specification's arithmetic needs both one-way latencies. The server's
  // response carries c2s; s2c is derived from our own receive instant against
  // the timestamp the server put in the base header.
  const int64_t c2s = snap::toUs(latency);
  const int64_t s2c = snap::toUs(nowTimePoint()) - snap::toUs(s_base.sent);
  s_serverOffsetUs = snap::serverTimeDiffUs(c2s, s2c);
  s_offsetKnown = true;
}

void dispatch(const snap::Base &b, const uint8_t *body) {
  switch ((snap::Type)b.type) {
    case snap::Type::ServerSettings: handleServerSettings(body, b.size); break;
    case snap::Type::CodecHeader: handleCodecHeader(body, b.size); break;
    case snap::Type::WireChunk: handleWireChunk(body, b.size); break;
    case snap::Type::Time: handleTime(body, b.size); break;
    case snap::Type::Error: {
      const char *json = nullptr;
      uint32_t n = 0;
      if (snap::parseJsonPayload(body + 4, b.size > 4 ? b.size - 4 : 0, json, n)) {
        Serial.printf("[maplayer] the server refused us: %.*s\n", (int)n, json);
      } else {
        Serial.println("[maplayer] the server sent an error");
      }
      s_state = maplayer::State::Refused;
      break;
    }
    default:
      // Not an error: the protocol may carry types this build does not need.
      break;
  }
}

// Reads what is available and consumes whole messages. A message that spans
// two reads stays in the buffer until the rest arrives - which is the normal
// case for a wire chunk, not an edge case.
void pump() {
  int avail = s_client.available();
  while (avail > 0) {
    const size_t room = kRxBufferBytes - s_rxLen;
    if (room == 0) {
      // A body larger than the whole buffer cannot be assembled; rather than
      // spin, say so and reconnect with a clean buffer.
      Serial.println("[maplayer] a message did not fit the receive buffer; reconnecting");
      s_stats.badMessages++;
      dropConnection("oversized message");
      return;
    }
    const size_t want = (size_t)avail < room ? (size_t)avail : room;
    const int got = s_client.read(s_rx + s_rxLen, want);
    if (got <= 0) break;
    s_rxLen += (size_t)got;
    s_lastRxMs = millis();
    avail = s_client.available();
  }

  for (;;) {
    if (!s_haveBase) {
      if (s_rxLen < snap::kBaseSize) return;
      if (!snap::parseBase(s_rx, s_rxLen, s_base)) return;
      s_haveBase = true;
    }
    const size_t total = snap::kBaseSize + s_base.size;
    if (total > kRxBufferBytes) {
      Serial.printf("[maplayer] the server announced a %u byte body; reconnecting\n",
                    (unsigned)s_base.size);
      s_stats.badMessages++;
      dropConnection("announced body too large");
      return;
    }
    if (s_rxLen < total) return;   // the rest is still on the wire
    dispatch(s_base, s_rx + snap::kBaseSize);
    memmove(s_rx, s_rx + total, s_rxLen - total);
    s_rxLen -= total;
    s_haveBase = false;
    if (s_state == maplayer::State::Refused) return;
  }
}

void tryConnect() {
  if (s_host[0] == '\0') {
    s_state = maplayer::State::Searching;
    return;
  }
  const maplayer::GateVerdict v = maplayer::gateCheck(s_gate, freeInternal(), largestBlock());
  s_lastVerdict = v;
  if (v != maplayer::GateVerdict::Ok) {
    if (s_state != maplayer::State::Gated) {
      Serial.printf("[maplayer] not starting: %s (free internal %u B, largest block %u B)\n",
                    maplayer::gateVerdictName(v), (unsigned)freeInternal(),
                    (unsigned)largestBlock());
    }
    s_stats.gateRefusals++;
    s_state = maplayer::State::Gated;
    s_backoffMs = maplayer::nextBackoffMs(s_backoffMs);
    s_retryAtMs = millis() + s_backoffMs;
    return;
  }

  s_state = maplayer::State::Connecting;
  s_client.setTimeout(kConnectTimeoutS);   // seconds, per WiFiClient
  if (!s_client.connect(s_host, s_port)) {
    dropConnection("connect refused");
    return;
  }
  s_client.setNoDelay(true);
  s_rxLen = 0;
  s_haveBase = false;
  s_lastRxMs = millis();
  s_state = maplayer::State::Greeting;
  if (!sendHello()) {
    dropConnection("could not send Hello");
    return;
  }
  if (s_volume.known) sendClientInfo();
  s_stats.connects++;
  s_backoffMs = 0;
  Serial.printf("[maplayer] connected to %s:%u\n", s_host, (unsigned)s_port);
}

}  // namespace

// ── the module's interface ──────────────────────────────────────────────────

void maplayerBegin() {
  loadHost();
  s_rx = (uint8_t *)heap_caps_malloc(kRxBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rx) {
    // PSRAM is where every buffer of this module lives, by rule. Without it the
    // module stays off rather than falling back to internal SRAM, which is the
    // memory the panel has none of.
    Serial.println("[maplayer] no PSRAM for the receive buffer; the player stays off");
    s_state = maplayer::State::Off;
    return;
  }
  s_state = s_host[0] ? maplayer::State::Lost : maplayer::State::Searching;
  s_retryAtMs = millis();
  Serial.printf("[maplayer] ready, %u B receive buffer in PSRAM, server '%s'\n",
                (unsigned)kRxBufferBytes, s_host[0] ? s_host : "(not set)");
}

void maplayerLoop() {
  if (!s_rx) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (s_client.connected()) dropConnection("wifi down");
    return;
  }

  if (s_client.connected()) {
    pump();
    if (s_state == maplayer::State::Refused) {
      dropConnection("refused by us");
      return;
    }
    if (millis() - s_lastRxMs > kRxSilenceMs) {
      dropConnection("no bytes for 15 s");
      return;
    }
    if (maplayer::playable(s_state) && millis() - s_lastTimeSyncMs >= MAPLAYER_TIME_SYNC_MS) {
      sendTime();
    }
    return;
  }

  if ((int32_t)(millis() - s_retryAtMs) < 0) return;
  tryConnect();
}

maplayer::State maplayerState() { return s_state; }
const char *maplayerStateName() { return maplayer::stateName(s_state); }
bool maplayerStreaming() { return maplayer::playable(s_state); }
const char *maplayerServerHost() { return s_host; }
maplayer::Volume maplayerVolume() { return s_volume; }
maplayer::Stats maplayerStats() { return s_stats; }

bool maplayerFormat(uint32_t &sampleRate, uint16_t &bits, uint16_t &channels) {
  if (!s_formatKnown) {
    sampleRate = 0;
    bits = 0;
    channels = 0;
    return false;
  }
  sampleRate = s_format.sampleRate;
  bits = s_format.bits;
  channels = s_format.channels;
  return true;
}

bool maplayerSetVolume(int level) {
  s_volume.level = maplayer::clampVolume(level);
  s_volume.known = true;
  saveVolume();
  if (!s_client.connected()) return false;
  return sendClientInfo();
}

bool maplayerSetMuted(bool muted) {
  s_volume.muted = muted;
  s_volume.known = true;
  if (!s_client.connected()) return false;
  return sendClientInfo();
}

void maplayerSettingsChanged() {
  loadHost();
  // A changed host takes effect on the next attempt rather than mid-stream.
  if (!s_client.connected()) {
    s_backoffMs = 0;
    s_retryAtMs = millis();
  }
}

void maplayerInfoJson(JsonObject out) {
  out["state"] = maplayer::stateName(s_state);
  out["server"] = s_host;
  out["port"] = s_port;
  out["audio"] = false;   // the output half is not built: maplayer.h says why
  if (s_volume.known) {
    out["volume"] = s_volume.level;
    out["muted"] = s_volume.muted;
  }
  if (s_formatKnown) {
    out["sampleRate"] = s_format.sampleRate;
    out["bits"] = s_format.bits;
    out["channels"] = s_format.channels;
  }
  if (s_bufferMs >= 0) out["serverBufferMs"] = s_bufferMs;
  if (s_offsetKnown) out["serverOffsetUs"] = (long)s_serverOffsetUs;
  out["connects"] = s_stats.connects;
  out["drops"] = s_stats.drops;
  out["chunks"] = s_stats.chunks;
  out["chunksBeforeCodec"] = s_stats.chunksBeforeCodec;
  out["badMessages"] = s_stats.badMessages;
  out["gateRefusals"] = s_stats.gateRefusals;
  if (s_state == maplayer::State::Gated) out["gate"] = maplayer::gateVerdictName(s_lastVerdict);
  out["freeInternal"] = freeInternal();
  out["largestBlock"] = largestBlock();
}

#endif  // MAPLAYER_ENABLED

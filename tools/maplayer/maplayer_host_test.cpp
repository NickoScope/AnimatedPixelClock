// Host test of the Music Assistant player's protocol and rules.
//
// src/maplayer/snap_proto.h and src/maplayer/maplayer_model.h are plain C++ -
// no Arduino, no sockets, no I2S - so they are compiled here and held to the
// Snapcast binary protocol as its own specification defines it
// (badaix/snapcast, doc/binary_protocol.md): little endian, a 26-byte base
// message, the join order Hello -> Server Settings -> Codec Header -> Wire
// Chunk, and a RIFF WAVE header carrying the PCM format.
//
// This is the whole test surface. No stream has ever run on the panel, so
// every claim the module makes about the wire is either proven here or is not
// proven at all.
//
//   python3 tools/maplayer/check_maplayer.py

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "maplayer_model.h"
#include "snap_proto.h"

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char *what) {
  g_checks++;
  if (!ok) {
    g_failed++;
    std::printf("  FAIL  %s\n", what);
  }
}

// ── helpers for building wire data ──────────────────────────────────────────

static void push16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back((uint8_t)(x & 0xFF));
  v.push_back((uint8_t)((x >> 8) & 0xFF));
}
static void push32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x & 0xFF));
  v.push_back((uint8_t)((x >> 8) & 0xFF));
  v.push_back((uint8_t)((x >> 16) & 0xFF));
  v.push_back((uint8_t)((x >> 24) & 0xFF));
}
static void pushStr(std::vector<uint8_t> &v, const char *s) {
  for (uint32_t i = 0; s[i] != '\0'; i++) v.push_back((uint8_t)s[i]);
}

// A minimal RIFF WAVE header, optionally with a filler chunk before "fmt " so
// the walk is exercised rather than a fixed offset.
static std::vector<uint8_t> riffWave(uint32_t rate, uint16_t channels, uint16_t bits,
                                     bool extraChunkFirst) {
  std::vector<uint8_t> v;
  pushStr(v, "RIFF");
  push32(v, 0);   // size: the parser does not rely on it
  pushStr(v, "WAVE");
  if (extraChunkFirst) {
    pushStr(v, "LIST");
    push32(v, 4);
    pushStr(v, "INFO");
  }
  pushStr(v, "fmt ");
  push32(v, 16);
  push16(v, 1);                                            // PCM
  push16(v, channels);                                     // f+2
  push32(v, rate);                                         // f+4
  push32(v, rate * channels * (bits / 8u));                // f+8  byte rate
  push16(v, (uint16_t)(channels * (bits / 8u)));           // f+12 block align
  push16(v, bits);                                         // f+14
  pushStr(v, "data");
  push32(v, 0);
  return v;
}

// ── the base message ────────────────────────────────────────────────────────

static void testBase() {
  std::printf("base message\n");
  check(snap::kBaseSize == 26, "the base message is 26 bytes");

  snap::Base out;
  out.type = (uint16_t)snap::Type::WireChunk;
  out.id = 0x1234;
  out.refersTo = 0x5678;
  out.sent = {0x01020304, 0x0A0B0C0D};
  out.received = {-1, -2};
  out.size = 0xDEADBEEF;

  uint8_t buf[64] = {};
  check(snap::writeBase(buf, sizeof(buf), out) == snap::kBaseSize, "writeBase returns 26");

  // little endian, verified byte by byte on the first field
  check(buf[0] == 0x02 && buf[1] == 0x00, "type is little endian");
  check(buf[2] == 0x34 && buf[3] == 0x12, "id is little endian");

  snap::Base in;
  check(snap::parseBase(buf, snap::kBaseSize, in), "parseBase accepts a full header");
  check(in.type == out.type && in.id == out.id && in.refersTo == out.refersTo,
        "base header round-trips its ids");
  check(in.sent.sec == out.sent.sec && in.sent.usec == out.sent.usec,
        "base header round-trips the sent timestamp");
  check(in.received.sec == -1 && in.received.usec == -2,
        "base header round-trips negative timestamps");
  check(in.size == out.size, "base header round-trips the body size");

  check(!snap::parseBase(buf, snap::kBaseSize - 1, in), "a short header is refused");
  check(!snap::parseBase(nullptr, 64, in), "a null buffer is refused");
}

// ── wire chunk ──────────────────────────────────────────────────────────────

static void testWireChunk() {
  std::printf("wire chunk\n");
  std::vector<uint8_t> body;
  push32(body, 7);            // timestamp.sec
  push32(body, 123456);       // timestamp.usec
  push32(body, 4);            // size
  body.push_back(0x11);
  body.push_back(0x22);
  body.push_back(0x33);
  body.push_back(0x44);

  snap::WireChunk wc;
  check(snap::parseWireChunk(body.data(), (uint32_t)body.size(), wc), "a wire chunk parses");
  check(wc.timestamp.sec == 7 && wc.timestamp.usec == 123456, "its timestamp survives");
  check(wc.size == 4 && wc.payload != nullptr, "its payload is found");
  check(wc.payload[0] == 0x11 && wc.payload[3] == 0x44, "the payload is the caller's bytes");

  // The message claims more payload than arrived: this is the case that matters,
  // because a stream is read in segments and a chunk regularly spans two reads.
  check(!snap::parseWireChunk(body.data(), (uint32_t)body.size() - 1, wc),
        "a truncated wire chunk is refused, not half-played");
  check(!snap::parseWireChunk(body.data(), 11, wc), "a header shorter than 12 bytes is refused");
}

// ── codec header and the PCM format ─────────────────────────────────────────

static void testCodecHeader() {
  std::printf("codec header\n");
  const std::vector<uint8_t> wave = riffWave(48000, 2, 16, false);

  std::vector<uint8_t> body;
  push32(body, 3);
  pushStr(body, "pcm");
  push32(body, (uint32_t)wave.size());
  for (uint8_t b : wave) body.push_back(b);

  snap::CodecHeader h;
  check(snap::parseCodecHeader(body.data(), (uint32_t)body.size(), h), "a codec header parses");
  check(h.codecLen == 3, "the codec name length is read");
  check(snap::codecIs(h, "pcm"), "codecIs matches the exact name");
  check(!snap::codecIs(h, "pc"), "codecIs refuses a prefix");
  check(!snap::codecIs(h, "pcmx"), "codecIs refuses a longer name");
  check(!snap::codecIs(h, "flac"), "codecIs refuses a different codec");
  check(h.size == (uint32_t)wave.size(), "the payload size is read");

  snap::PcmFormat f;
  check(snap::parseRiffWave(h.payload, h.size, f), "the RIFF WAVE payload parses");
  check(f.sampleRate == 48000 && f.channels == 2 && f.bits == 16,
        "the PCM format comes out of the WAVE header");

  // fmt is not always the first chunk
  const std::vector<uint8_t> waveLater = riffWave(44100, 1, 24, true);
  snap::PcmFormat f2;
  check(snap::parseRiffWave(waveLater.data(), (uint32_t)waveLater.size(), f2),
        "a WAVE header with a chunk before fmt still parses");
  check(f2.sampleRate == 44100 && f2.channels == 1 && f2.bits == 24,
        "the walk found the right fmt chunk");

  // not a WAVE at all
  const uint8_t junk[16] = {'N', 'O', 'P', 'E'};
  snap::PcmFormat f3;
  check(!snap::parseRiffWave(junk, sizeof(junk), f3), "a non-RIFF payload is refused");
  check(!snap::parseRiffWave(wave.data(), 8, f3), "a truncated WAVE header is refused");

  check(!snap::parseCodecHeader(body.data(), 3, h), "a truncated codec header is refused");
}

// ── server settings and the JSON scanner ────────────────────────────────────

static void testServerSettings() {
  std::printf("server settings\n");
  const char *json = "{\"bufferMs\": 1000, \"latency\": 0, \"muted\": false, \"volume\": 100}";
  const uint32_t len = (uint32_t)std::strlen(json);

  snap::ServerSettings s;
  check(snap::parseServerSettings(json, len, s), "the specification's own sample payload parses");
  check(s.bufferMs == 1000, "bufferMs is read");
  check(s.latency == 0, "latency is read");
  check(s.volume == 100, "volume is read");
  check(s.haveMuted && !s.muted, "muted is read as false");

  // A key that merely starts with the key being looked for must not match.
  const char *trap = "{\"volumex\": 5, \"volume\": 42}";
  long v = 0;
  check(snap::jsonInt(trap, (uint32_t)std::strlen(trap), "volume", v) && v == 42,
        "a longer key that starts with ours is not mistaken for it");

  // Out-of-range volume is clamped to the protocol's 0..100
  const char *loud = "{\"volume\": 4000}";
  snap::ServerSettings s2;
  check(snap::parseServerSettings(loud, (uint32_t)std::strlen(loud), s2) && s2.volume == 100,
        "a volume above 100 is clamped");

  const char *neg = "{\"latency\": -250}";
  long l = 0;
  check(snap::jsonInt(neg, (uint32_t)std::strlen(neg), "latency", l) && l == -250,
        "a negative number is read");

  // A value that is not a whole number is refused rather than silently zeroed
  const char *bad = "{\"volume\": \"loud\"}";
  long b = 0;
  check(!snap::jsonInt(bad, (uint32_t)std::strlen(bad), "volume", b),
        "a non-numeric value is refused, not read as 0");

  bool m = false;
  const char *mt = "{\"muted\":true}";
  check(snap::jsonBool(mt, (uint32_t)std::strlen(mt), "muted", m) && m,
        "muted true is read without spaces");
  check(!snap::jsonBool(mt, (uint32_t)std::strlen(mt), "volume", m),
        "an absent key is reported absent");

  // A payload with none of the four is not something to act on
  snap::ServerSettings s3;
  const char *empty = "{\"other\": 1}";
  check(!snap::parseServerSettings(empty, (uint32_t)std::strlen(empty), s3),
        "a payload carrying none of the known fields is refused");

  // A quoted string value comes back without its quotes
  const char *str = "{\"ID\": \"aa:bb:cc\"}";
  const char *val = nullptr;
  uint32_t vlen = 0;
  check(snap::jsonFindScalar(str, (uint32_t)std::strlen(str), "ID", val, vlen) && vlen == 8 &&
            std::strncmp(val, "aa:bb:cc", 8) == 0,
        "a quoted value is returned without quotes");

  // An unterminated string must not run off the end
  const char *broken = "{\"ID\": \"aa";
  check(!snap::jsonFindScalar(broken, (uint32_t)std::strlen(broken), "ID", val, vlen),
        "an unterminated string is refused");
}

// ── what the client sends ───────────────────────────────────────────────────

static void testClientMessages() {
  std::printf("client messages\n");
  char json[512];

  snap::HelloInfo info;
  info.id = "a1:b2:c3:d4:e5:f6";
  info.mac = "a1:b2:c3:d4:e5:f6";
  info.hostName = "pixelclock";
  info.clientName = "AnimatedPixelClock";
  info.version = "0.0.1";
  info.os = "arduino-esp32";
  info.arch = "xtensa";
  info.instance = 1;

  const size_t n = snap::buildHelloJson(json, sizeof(json), info);
  check(n > 0, "Hello builds");
  check(json[0] == '{' && json[n - 1] == '}', "Hello is a JSON object");
  json[n] = '\0';
  check(std::strstr(json, "\"SnapStreamProtocolVersion\":2") != nullptr,
        "Hello carries the protocol version the specification names");
  check(std::strstr(json, "\"ID\":\"a1:b2:c3:d4:e5:f6\"") != nullptr, "Hello carries the ID");
  check(std::strstr(json, "\"Instance\":1") != nullptr, "Hello carries the instance");
  check(std::strstr(json, ",,") == nullptr, "Hello has no empty field separators");

  // A buffer too small must refuse rather than emit truncated JSON
  char tiny[8];
  check(snap::buildHelloJson(tiny, sizeof(tiny), info) == 0,
        "Hello refuses a buffer it does not fit in");

  // An absent optional field is omitted entirely
  snap::HelloInfo bare;
  bare.id = "x";
  bare.instance = 2;
  const size_t nb = snap::buildHelloJson(json, sizeof(json), bare);
  json[nb] = '\0';
  check(nb > 0 && std::strstr(json, "\"OS\"") == nullptr, "an absent field is omitted");

  // A quote inside a host name must be escaped, not emitted raw
  snap::HelloInfo odd;
  odd.hostName = "we\"ird";
  odd.id = "x";
  const size_t no = snap::buildHelloJson(json, sizeof(json), odd);
  json[no] = '\0';
  check(no > 0 && std::strstr(json, "we\\\"ird") != nullptr, "a quote in a value is escaped");

  // Client Info is what carries our volume back to Music Assistant
  const size_t nc = snap::buildClientInfoJson(json, sizeof(json), 42, false);
  json[nc] = '\0';
  check(std::strcmp(json, "{\"volume\":42,\"muted\":false}") == 0, "Client Info is exact");
  const size_t nm = snap::buildClientInfoJson(json, sizeof(json), 300, true);
  json[nm] = '\0';
  check(std::strcmp(json, "{\"volume\":100,\"muted\":true}") == 0,
        "Client Info clamps the volume to the protocol's range");
}

// ── framing ─────────────────────────────────────────────────────────────────

static void testFraming() {
  std::printf("framing\n");
  uint8_t out[256];
  const snap::TimePoint sent{5, 6};

  const uint8_t body[4] = {1, 2, 3, 4};
  const size_t n = snap::frame(out, sizeof(out), snap::Type::Time, 9, sent, body, 4);
  check(n == snap::kBaseSize + 4, "frame writes the header and the body");

  snap::Base b;
  check(snap::parseBase(out, (uint32_t)n, b), "the framed message parses back");
  check(b.type == (uint16_t)snap::Type::Time, "the type survives framing");
  check(b.id == 9, "the id survives framing");
  check(b.size == 4, "the size field counts only the body");
  check(b.sent.sec == 5 && b.sent.usec == 6, "the sent timestamp survives framing");

  const char *json = "{\"volume\":1}";
  const uint32_t jlen = (uint32_t)std::strlen(json);
  const size_t nj = snap::frameJson(out, sizeof(out), snap::Type::ClientInfo, 3, sent, json, jlen);
  check(nj == snap::kBaseSize + 4 + jlen, "frameJson writes header, length and JSON");
  check(snap::parseBase(out, (uint32_t)nj, b) && b.size == 4 + jlen,
        "the JSON body is length prefixed inside the sized body");

  const char *jsonOut = nullptr;
  uint32_t jsonLen = 0;
  check(snap::parseJsonPayload(out + snap::kBaseSize, b.size, jsonOut, jsonLen) && jsonLen == jlen,
        "the length prefix reads back");

  check(snap::frame(out, snap::kBaseSize, snap::Type::Time, 0, sent, body, 4) == 0,
        "framing refuses a buffer that cannot hold the body");
  check(snap::frameJson(out, 8, snap::Type::ClientInfo, 0, sent, json, jlen) == 0,
        "framing JSON refuses a buffer that cannot hold it");
}

// ── time synchronisation ────────────────────────────────────────────────────

static void testTime() {
  std::printf("time\n");
  const snap::TimePoint t{2, 500000};
  check(snap::toUs(t) == 2500000, "a timestamp converts to microseconds");
  const snap::TimePoint back = snap::fromUs(2500000);
  check(back.sec == 2 && back.usec == 500000, "and back again");

  // The specification's arithmetic: the diff halves the difference of the two
  // one-way latencies, which cancels a symmetric network delay.
  check(snap::serverTimeDiffUs(1000, 200) == 400, "the server/client offset is half the difference");
  check(snap::serverTimeDiffUs(200, 1000) == -400, "the offset is signed");

  uint8_t body[8];
  check(snap::writeTimeBody(body, sizeof(body), t) == 8, "the Time body is 8 bytes");
  snap::TimePoint parsed;
  check(snap::parseTimeBody(body, 8, parsed) && parsed.sec == 2 && parsed.usec == 500000,
        "the Time body round-trips");
  check(!snap::parseTimeBody(body, 7, parsed), "a short Time body is refused");
}

// ── the module's own rules ──────────────────────────────────────────────────

static void testModel() {
  std::printf("model\n");

  // The format this board can accept is fixed by the shared bit clock, not by taste.
  check(maplayer::formatPlayable(48000, 16, 2), "48 kHz 16-bit stereo is playable");
  check(maplayer::formatPlayable(48000, 16, 1), "48 kHz 16-bit mono is playable");
  check(!maplayer::formatPlayable(44100, 16, 2), "44.1 kHz is refused: the clock is shared");
  check(!maplayer::formatPlayable(48000, 24, 2), "24-bit is refused");
  check(!maplayer::formatPlayable(16000, 16, 1), "16 kHz is refused");

  // The downmix exists because the ES8311 would otherwise take the left channel
  // only and drop the right one on the floor.
  check(maplayer::downmix(100, -100) == 0, "opposite samples cancel");
  check(maplayer::downmix(32767, 32767) == 32767, "two full-scale positives do not wrap");
  check(maplayer::downmix(-32768, -32768) == -32768, "two full-scale negatives do not wrap");
  check(maplayer::downmix(32767, -32768) == 0 || maplayer::downmix(32767, -32768) == -1,
        "a full-scale pair averages near zero");

  int16_t pcm[6] = {100, 300, -100, -300, 32767, 32767};
  check(maplayer::downmixInPlace(pcm, 3) == 3, "the downmix reports the mono sample count");
  check(pcm[0] == 200 && pcm[1] == -200 && pcm[2] == 32767, "the downmix works in place");

  // The gate. Its thresholds are starting values; what is tested is the rule.
  maplayer::HeapGate g;
  g.minFreeInternal = 24576;
  g.minLargestBlock = 12288;
  check(maplayer::gateCheck(g, 30000, 20000) == maplayer::GateVerdict::Ok,
        "a healthy heap passes the gate");
  check(maplayer::gateCheck(g, 20000, 20000) == maplayer::GateVerdict::LowInternalHeap,
        "too little free internal heap is refused");
  check(maplayer::gateCheck(g, 30000, 8000) == maplayer::GateVerdict::NoLargeBlock,
        "a fragmented heap is refused even when the total looks fine");
  // The measured idle figure from the panel, with capture running, must not pass
  // a gate meant to protect the same memory: 32,952 - 10,400 = 22,552.
  check(maplayer::gateCheck(g, 22552, 13000) == maplayer::GateVerdict::LowInternalHeap,
        "the measured heap with the microphones running does not pass");

  // Backoff: the failure this guards against is a tight retry loop taking Wi-Fi down.
  uint32_t ms = 0;
  ms = maplayer::nextBackoffMs(ms);
  check(ms == maplayer::kBackoffFirstMs, "the first backoff is the configured first step");
  uint32_t prev = ms;
  for (int i = 0; i < 20; i++) {
    ms = maplayer::nextBackoffMs(ms);
    check(ms != 0, "backoff never collapses to zero");
    check(ms >= prev, "backoff never shrinks");
    prev = ms;
  }
  check(ms == maplayer::kBackoffMaxMs, "backoff saturates at the ceiling");

  check(maplayer::clampVolume(-5) == 0 && maplayer::clampVolume(500) == 100 &&
            maplayer::clampVolume(42) == 42,
        "volume is clamped to the protocol's 0..100");

  check(maplayer::playable(maplayer::State::Streaming), "only streaming is playable");
  check(!maplayer::playable(maplayer::State::WaitingCodec),
        "a chunk before the codec header is not playable");
  check(std::strcmp(maplayer::stateName(maplayer::State::Gated), "gated") == 0,
        "every state has a name for /api/info");
  check(std::strcmp(maplayer::gateVerdictName(maplayer::GateVerdict::NoLargeBlock),
                    "heap too fragmented") == 0,
        "every gate verdict has a name");
}

int main() {
  std::printf("Music Assistant player: protocol and rules\n\n");
  testBase();
  testWireChunk();
  testCodecHeader();
  testServerSettings();
  testClientMessages();
  testFraming();
  testTime();
  testModel();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

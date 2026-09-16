#pragma once
// The Snapcast binary protocol, as its own specification defines it.
//
// Source: badaix/snapcast, doc/binary_protocol.md, read in full 2026-09-16.
// Every layout below is from that document, not from memory:
//   - the protocol is little endian;
//   - the client opens a TCP socket (default stream port 1704), sends Hello,
//     then receives Server Settings, then a Codec Header, and only then may it
//     play the Wire Chunks that follow;
//   - Time messages are exchanged both ways to derive the server/client offset.
//
// Why this file has no Arduino in it: it is the whole test surface. The panel
// cannot be flashed to try a parser, so the parser is held to the specification
// on the host instead (tools/maplayer/check_maplayer.py). The same reason
// ir_map.h and climate_model.h are plain C++.
//
// This header decodes and encodes; it owns no buffers and allocates nothing.
// Every parse takes a caller's span and returns views into it, so a message
// that spans two reads is the caller's problem to assemble, not a hidden copy.
//
// docs/25-ma-media-player.md in the LED-MATRIX APOLLO knowledge base has the
// design and why Snapcast with codec "pcm" was chosen over the alternatives.

#include <stddef.h>
#include <stdint.h>

namespace snap {

// ── the wire ────────────────────────────────────────────────────────────────

// "Client opens a TCP socket to the server (default port is 1704)".
// Music Assistant's built-in snapserver registers exactly this as its
// _snapcast-stream._tcp mDNS service; 1705 is the JSON-RPC control port and
// 1780 the web interface, neither of which this client speaks.
constexpr uint16_t kStreamPort = 1704;

// The typed message IDs of the table in the specification. 6 is absent there.
enum class Type : uint16_t {
  Base = 0,
  CodecHeader = 1,
  WireChunk = 2,
  ServerSettings = 3,
  Time = 4,
  Hello = 5,
  ClientInfo = 7,
  Error = 8,
};

// type + id + refersTo (3 x uint16) + sent.sec/usec + received.sec/usec
// (4 x int32) + size (uint32) = 6 + 16 + 4.
constexpr size_t kBaseSize = 26;

// The specification's SnapStreamProtocolVersion, as sent in Hello.
constexpr int kStreamProtocolVersion = 2;

struct TimePoint {
  int32_t sec;
  int32_t usec;

  // Explicit, with defaults, rather than default member initialisers: a class
  // with those stopped being an aggregate in C++11, and this firmware builds as
  // gnu++11 while the host test builds at C++17 too. Written as a constructor,
  // both `TimePoint t;` and `TimePoint{2, 500000}` work under either standard.
  TimePoint(int32_t s = 0, int32_t u = 0) : sec(s), usec(u) {}
};

struct Base {
  uint16_t type = 0;
  uint16_t id = 0;
  uint16_t refersTo = 0;
  TimePoint sent;
  TimePoint received;
  uint32_t size = 0;   // bytes of the typed message that follows this header
};

// ── little-endian scalars ───────────────────────────────────────────────────
// Written out rather than memcpy'd so the file makes no assumption about the
// host's own byte order: the host test runs on a little-endian machine and the
// panel is little-endian too, and neither fact is allowed to matter.

inline uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int32_t rdi32(const uint8_t *p) { return (int32_t)rd32(p); }

inline void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}
inline void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
inline void wri32(uint8_t *p, int32_t v) { wr32(p, (uint32_t)v); }

// ── the base message ────────────────────────────────────────────────────────

// False when fewer than kBaseSize bytes are present. Never reads past len.
inline bool parseBase(const uint8_t *buf, size_t len, Base &out) {
  if (!buf || len < kBaseSize) return false;
  out.type = rd16(buf + 0);
  out.id = rd16(buf + 2);
  out.refersTo = rd16(buf + 4);
  out.sent.sec = rdi32(buf + 6);
  out.sent.usec = rdi32(buf + 10);
  out.received.sec = rdi32(buf + 14);
  out.received.usec = rdi32(buf + 18);
  out.size = rd32(buf + 22);
  return true;
}

// Returns bytes written (kBaseSize), or 0 when the buffer is too small.
inline size_t writeBase(uint8_t *buf, size_t cap, const Base &b) {
  if (!buf || cap < kBaseSize) return 0;
  wr16(buf + 0, b.type);
  wr16(buf + 2, b.id);
  wr16(buf + 4, b.refersTo);
  wri32(buf + 6, b.sent.sec);
  wri32(buf + 10, b.sent.usec);
  wri32(buf + 14, b.received.sec);
  wri32(buf + 18, b.received.usec);
  wr32(buf + 22, b.size);
  return kBaseSize;
}

// ── typed messages, server to client ────────────────────────────────────────

// "A part of an audio stream": timestamp.sec, timestamp.usec, size, payload.
struct WireChunk {
  TimePoint timestamp;
  const uint8_t *payload = nullptr;   // a view into the caller's buffer
  uint32_t size = 0;
};

inline bool parseWireChunk(const uint8_t *body, uint32_t len, WireChunk &out) {
  if (!body || len < 12) return false;
  out.timestamp.sec = rdi32(body + 0);
  out.timestamp.usec = rdi32(body + 4);
  out.size = rd32(body + 8);
  if ((uint64_t)out.size + 12 > (uint64_t)len) return false;   // truncated
  out.payload = body + 12;
  return true;
}

// "The codec-specific data to put at the start of a stream to allow decoding":
// codec_size, codec (not null terminated), size, payload.
struct CodecHeader {
  const char *codec = nullptr;
  uint32_t codecLen = 0;
  const uint8_t *payload = nullptr;
  uint32_t size = 0;
};

inline bool parseCodecHeader(const uint8_t *body, uint32_t len, CodecHeader &out) {
  if (!body || len < 4) return false;
  const uint32_t codecLen = rd32(body + 0);
  if ((uint64_t)codecLen + 8 > (uint64_t)len) return false;
  const uint32_t payloadLen = rd32(body + 4 + codecLen);
  if ((uint64_t)codecLen + 8 + (uint64_t)payloadLen > (uint64_t)len) return false;
  out.codec = (const char *)(body + 4);
  out.codecLen = codecLen;
  out.payload = body + 4 + codecLen + 4;
  out.size = payloadLen;
  return true;
}

// The codec string is not null terminated, so it is compared by length.
inline bool codecIs(const CodecHeader &h, const char *name) {
  if (!h.codec || !name) return false;
  uint32_t i = 0;
  for (; i < h.codecLen; i++) {
    if (name[i] == '\0' || name[i] != h.codec[i]) return false;
  }
  return name[i] == '\0';
}

// ── the PCM codec header ────────────────────────────────────────────────────
// "PCM: a RIFF WAVE header ... PCM is not encoded, but the decoder must know
// the samplerate, bit depth and number of channels, which is encoded into the
// header". This is the whole reason the firmware needs no decoder: with
// codec "pcm" the wire chunks are the samples themselves.

struct PcmFormat {
  uint32_t sampleRate = 0;
  uint16_t bits = 0;
  uint16_t channels = 0;
};

// Walks the RIFF chunk list to "fmt " rather than assuming it sits at a fixed
// offset: a WAVE header may legally carry other chunks before it.
inline bool parseRiffWave(const uint8_t *p, uint32_t len, PcmFormat &out) {
  if (!p || len < 12) return false;
  if (!(p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F')) return false;
  if (!(p[8] == 'W' && p[9] == 'A' && p[10] == 'V' && p[11] == 'E')) return false;
  uint32_t off = 12;
  while ((uint64_t)off + 8 <= (uint64_t)len) {
    const uint32_t chunkLen = rd32(p + off + 4);
    const bool isFmt = p[off] == 'f' && p[off + 1] == 'm' && p[off + 2] == 't' && p[off + 3] == ' ';
    if (isFmt) {
      if ((uint64_t)off + 8 + 16 > (uint64_t)len || chunkLen < 16) return false;
      const uint8_t *f = p + off + 8;
      out.channels = rd16(f + 2);
      out.sampleRate = rd32(f + 4);
      out.bits = rd16(f + 14);
      return out.sampleRate != 0 && out.channels != 0 && out.bits != 0;
    }
    // chunks are word aligned
    const uint64_t next = (uint64_t)off + 8 + chunkLen + (chunkLen & 1u);
    if (next <= off || next > (uint64_t)len) return false;
    off = (uint32_t)next;
  }
  return false;
}

// ── length-prefixed JSON payloads ───────────────────────────────────────────
// Server Settings, Hello and Client Info all carry "size" then a JSON string
// that is not null terminated.

inline bool parseJsonPayload(const uint8_t *body, uint32_t len, const char *&json,
                             uint32_t &jsonLen) {
  if (!body || len < 4) return false;
  const uint32_t n = rd32(body + 0);
  if ((uint64_t)n + 4 > (uint64_t)len) return false;
  json = (const char *)(body + 4);
  jsonLen = n;
  return true;
}

// A scanner for the handful of scalars Server Settings carries, so the clean
// core stays free of ArduinoJson (which would drag Arduino in and cost heap on
// a path that runs per connection). It looks for "key" then the next ':' and
// reads one scalar; it does not walk nested objects, and it does not need to:
// the specification's payload is flat.
//   {"bufferMs": 1000, "latency": 0, "muted": false, "volume": 100}
inline uint32_t slen(const char *s) {
  uint32_t n = 0;
  if (!s) return 0;
  while (s[n] != '\0') n++;
  return n;
}

inline bool jsonIsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Finds "key" at any depth, then the scalar after the next ':'. A quoted value
// is returned without its quotes; a bare value ends at a comma, a brace or
// whitespace. Never reads past len.
inline bool jsonFindScalar(const char *json, uint32_t len, const char *key, const char *&val,
                           uint32_t &valLen) {
  if (!json || !key) return false;
  const uint32_t klen = slen(key);
  if (klen == 0) return false;
  for (uint32_t i = 0; i < len; i++) {
    if (json[i] != '"') continue;
    if ((uint64_t)i + 1 + klen + 1 > (uint64_t)len) return false;   // "key" cannot fit
    bool same = true;
    for (uint32_t k = 0; k < klen; k++) {
      if (json[i + 1 + k] != key[k]) {
        same = false;
        break;
      }
    }
    if (!same) continue;
    uint32_t j = i + 1 + klen;
    if (json[j] != '"') continue;   // a longer key that merely starts with ours
    j++;
    while (j < len && jsonIsSpace(json[j])) j++;
    if (j >= len || json[j] != ':') continue;
    j++;
    while (j < len && jsonIsSpace(json[j])) j++;
    if (j >= len) return false;
    if (json[j] == '"') {
      j++;
      const uint32_t start = j;
      while (j < len && json[j] != '"') {
        if (json[j] == '\\' && j + 1 < len) j++;   // an escaped quote is not the end
        j++;
      }
      if (j >= len) return false;   // unterminated string
      val = json + start;
      valLen = j - start;
      return true;
    }
    const uint32_t start = j;
    while (j < len && json[j] != ',' && json[j] != '}' && !jsonIsSpace(json[j])) j++;
    if (j == start) return false;
    val = json + start;
    valLen = j - start;
    return true;
  }
  return false;
}

// Whole numbers only: a value that is not one is refused rather than truncated,
// so a malformed payload cannot quietly become a volume of 0.
inline bool jsonInt(const char *json, uint32_t len, const char *key, long &out) {
  const char *v = nullptr;
  uint32_t n = 0;
  if (!jsonFindScalar(json, len, key, v, n) || n == 0) return false;
  uint32_t i = 0;
  bool neg = false;
  if (v[0] == '-') {
    neg = true;
    i = 1;
  } else if (v[0] == '+') {
    i = 1;
  }
  if (i >= n) return false;
  long acc = 0;
  for (; i < n; i++) {
    if (v[i] < '0' || v[i] > '9') return false;
    acc = acc * 10 + (long)(v[i] - '0');
  }
  out = neg ? -acc : acc;
  return true;
}

inline bool jsonBool(const char *json, uint32_t len, const char *key, bool &out) {
  const char *v = nullptr;
  uint32_t n = 0;
  if (!jsonFindScalar(json, len, key, v, n)) return false;
  if (n == 4 && v[0] == 't' && v[1] == 'r' && v[2] == 'u' && v[3] == 'e') {
    out = true;
    return true;
  }
  if (n == 5 && v[0] == 'f' && v[1] == 'a' && v[2] == 'l' && v[3] == 's' && v[4] == 'e') {
    out = false;
    return true;
  }
  return false;
}

struct ServerSettings {
  long bufferMs = -1;   // -1: the server did not send this field
  long latency = -1;
  int volume = -1;      // 0..100 inclusive per the specification
  bool muted = false;
  bool haveMuted = false;
};

inline bool parseServerSettings(const char *json, uint32_t len, ServerSettings &out) {
  if (!json) return false;
  long v = 0;
  if (jsonInt(json, len, "bufferMs", v)) out.bufferMs = v;
  if (jsonInt(json, len, "latency", v)) out.latency = v;
  if (jsonInt(json, len, "volume", v)) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    out.volume = (int)v;
  }
  bool b = false;
  if (jsonBool(json, len, "muted", b)) {
    out.muted = b;
    out.haveMuted = true;
  }
  // A payload that carried none of the four is not a Server Settings message
  // we can act on; say so rather than reporting a default-filled struct.
  return out.bufferMs >= 0 || out.latency >= 0 || out.volume >= 0 || out.haveMuted;
}

// ── client to server ────────────────────────────────────────────────────────

struct HelloInfo {
  const char *id = nullptr;          // the specification's "ID"; the MAC is customary
  const char *mac = nullptr;
  const char *hostName = nullptr;
  const char *clientName = nullptr;  // "Snapclient" for the reference client
  const char *version = nullptr;
  const char *os = nullptr;
  const char *arch = nullptr;
  int instance = 1;
};

// A bounded appender: it never writes past cap and reports whether everything
// it was asked for actually fit, so a truncated Hello is refused rather than
// sent as malformed JSON.
struct Appender {
  char *out;
  size_t cap;
  size_t n;
  bool ok;

  // An explicit constructor rather than aggregate initialisation, and the
  // reason is worth keeping: a class with default member initialisers stopped
  // being an aggregate in C++11. The host test compiles at C++17, where it is
  // one again, so `Appender a{out, cap}` passed 136 checks on the host while
  // the firmware - built as gnu++11 - would not compile at all. The test now
  // builds at both standards so this cannot happen again quietly.
  Appender(char *outBuf, size_t capacity) : out(outBuf), cap(capacity), n(0), ok(true) {}

  void ch(char c) {
    if (n < cap) {
      out[n++] = c;
    } else {
      ok = false;
    }
  }
  void raw(const char *s) {
    if (!s) return;
    for (uint32_t i = 0; s[i] != '\0'; i++) ch(s[i]);
  }
  void esc(const char *s) {
    if (!s) return;
    for (uint32_t i = 0; s[i] != '\0'; i++) {
      const char c = s[i];
      if (c == '"' || c == '\\') {
        ch('\\');
        ch(c);
      } else if ((unsigned char)c < 0x20) {
        ch(' ');   // a control character in a host name cannot travel as itself
      } else {
        ch(c);
      }
    }
  }
  void num(long v) {
    if (v < 0) {
      ch('-');
      v = -v;
    }
    char tmp[20];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) {
      tmp[t++] = (char)('0' + (int)(v % 10));
      v /= 10;
    }
    while (t > 0) ch(tmp[--t]);
  }
  void field(const char *k, const char *v, bool &first) {
    if (!v) return;   // an absent field is omitted, not sent empty
    if (!first) ch(',');
    first = false;
    ch('"');
    raw(k);
    ch('"');
    ch(':');
    ch('"');
    esc(v);
    ch('"');
  }
};

// Builds the JSON body only (no base header, no length prefix). Returns the
// number of bytes written, or 0 when it would not fit.
inline size_t buildHelloJson(char *out, size_t cap, const HelloInfo &info) {
  if (!out || cap == 0) return 0;
  Appender a{out, cap};
  bool first = true;
  a.ch('{');
  a.field("Arch", info.arch, first);
  a.field("ClientName", info.clientName, first);
  a.field("HostName", info.hostName, first);
  a.field("ID", info.id, first);
  if (!first) a.ch(',');
  first = false;
  a.raw("\"Instance\":");
  a.num(info.instance);
  a.field("MAC", info.mac, first);
  a.field("OS", info.os, first);
  a.raw(",\"SnapStreamProtocolVersion\":");
  a.num(kStreamProtocolVersion);
  a.field("Version", info.version, first);
  a.ch('}');
  return a.ok ? a.n : 0;
}

// {"volume": N, "muted": true|false} - how a player reports its own level back,
// which is what makes the knob on the panel move the level in Music Assistant.
inline size_t buildClientInfoJson(char *out, size_t cap, int volume, bool muted) {
  if (!out || cap == 0) return 0;
  Appender a{out, cap};
  a.raw("{\"volume\":");
  a.num(volume < 0 ? 0 : (volume > 100 ? 100 : volume));
  a.raw(",\"muted\":");
  a.raw(muted ? "true" : "false");
  a.ch('}');
  return a.ok ? a.n : 0;
}

// Wraps an already-built body in a base header. Returns total bytes, or 0.
inline size_t frame(uint8_t *out, size_t cap, Type type, uint16_t id, const TimePoint &sent,
                    const uint8_t *body, uint32_t bodyLen) {
  if (!out) return 0;
  if ((uint64_t)kBaseSize + (uint64_t)bodyLen > (uint64_t)cap) return 0;
  Base b;
  b.type = (uint16_t)type;
  b.id = id;
  b.refersTo = 0;
  b.sent = sent;
  b.size = bodyLen;
  if (writeBase(out, cap, b) != kBaseSize) return 0;
  for (uint32_t i = 0; i < bodyLen; i++) out[kBaseSize + i] = body ? body[i] : (uint8_t)0;
  return kBaseSize + bodyLen;
}

// Wraps a length-prefixed JSON body (size + chars) in a base header.
inline size_t frameJson(uint8_t *out, size_t cap, Type type, uint16_t id, const TimePoint &sent,
                        const char *json, uint32_t jsonLen) {
  if (!out || !json) return 0;
  const uint64_t total = (uint64_t)kBaseSize + 4 + (uint64_t)jsonLen;
  if (total > (uint64_t)cap) return 0;
  Base b;
  b.type = (uint16_t)type;
  b.id = id;
  b.refersTo = 0;
  b.sent = sent;
  b.size = 4 + jsonLen;
  if (writeBase(out, cap, b) != kBaseSize) return 0;
  wr32(out + kBaseSize, jsonLen);
  for (uint32_t i = 0; i < jsonLen; i++) out[kBaseSize + 4 + i] = (uint8_t)json[i];
  return (size_t)total;
}

// The Time message body: latency.sec, latency.usec.
inline size_t writeTimeBody(uint8_t *out, size_t cap, const TimePoint &latency) {
  if (!out || cap < 8) return 0;
  wri32(out + 0, latency.sec);
  wri32(out + 4, latency.usec);
  return 8;
}

inline bool parseTimeBody(const uint8_t *body, uint32_t len, TimePoint &out) {
  if (!body || len < 8) return false;
  out.sec = rdi32(body + 0);
  out.usec = rdi32(body + 4);
  return true;
}

// ── time synchronisation ────────────────────────────────────────────────────
// The specification's own arithmetic:
//   latency_c2s = t_server-recv - t_client-sent + t_network-latency
//   latency_s2c = t_client-recv - t_server-sent + t_network-latency
//   diff        = (latency_c2s - latency_s2c) / 2
// which cancels the network latency on the assumption that it is symmetric.
// Kept in microseconds as int64 so a clock tens of seconds apart cannot wrap.

inline int64_t toUs(const TimePoint &t) { return (int64_t)t.sec * 1000000LL + (int64_t)t.usec; }

inline TimePoint fromUs(int64_t us) {
  TimePoint t;
  t.sec = (int32_t)(us / 1000000LL);
  t.usec = (int32_t)(us % 1000000LL);
  return t;
}

// latencyC2S is what the server returned in the Time response body.
inline int64_t serverTimeDiffUs(int64_t latencyC2Us, int64_t latencyS2Cus) {
  return (latencyC2Us - latencyS2Cus) / 2;
}

}  // namespace snap

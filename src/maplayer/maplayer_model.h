#pragma once
// The rules of the Music Assistant player, with no Arduino in them.
//
// What belongs here: the connection state machine, the heap gate that decides
// whether a stream may start at all, the PCM format this board can actually
// accept, and the stereo-to-mono downmix. What does not: sockets, I2S, the
// codec's I2C registers, anything that needs hardware to be true.
//
// The reason for the split is written into docs/25-ma-media-player.md and into
// the scar it comes from: the microphone visualizer hangs this panel because a
// second consumer of internal SRAM met a ~20 KB spike from the portal and Wi-Fi
// lost its buffers (docs/22 §12.2). A player is a third consumer. So the gate
// below is a first-class part of the model, tested on the host, rather than a
// check somebody remembers to write in the connect path.

#include <stdint.h>

namespace maplayer {

// ── what this board can accept ──────────────────────────────────────────────
// Not a preference - a constraint of the wiring. ES8311 and ES7210 share one
// BCLK and one WS, and an I2S port carries a single sample rate for both
// directions (docs/25 §4, confirmed in the HAL this firmware compiles against).
// The microphone DSP already runs at 48 kHz with MCLK at 256 fs, so the player
// runs there too and the shared clock costs nothing.
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kBitsPerSample = 16;

// The ES8311 is a mono codec and takes the LEFT channel only: register 0x09
// bit 7 SDP_IN_SEL defaults to "left channel data to DAC" and the chip does no
// summing (datasheet Rev 6.0, read in full). A stereo stream played untouched
// would silently lose whatever lives only in the right channel, so the downmix
// below is not an optimisation, it is what keeps the music intact.
constexpr uint16_t kStreamChannels = 2;

// True when a Snapcast codec header describes a stream this module can play
// without a decoder. Anything else is refused with a reason rather than played
// as noise.
constexpr bool formatPlayable(uint32_t sampleRate, uint16_t bits, uint16_t channels) {
  return sampleRate == kSampleRate && bits == kBitsPerSample &&
         (channels == 1 || channels == kStreamChannels);
}

// (L + R) / 2 with the sum taken in 32 bits, so two full-scale samples cannot
// wrap on the way to being halved. Saturated on the way out because -32768 is
// representable and +32768 is not.
inline int16_t downmix(int16_t l, int16_t r) {
  const int32_t sum = (int32_t)l + (int32_t)r;
  int32_t m = sum / 2;
  if (m > 32767) m = 32767;
  if (m < -32768) m = -32768;
  return (int16_t)m;
}

// In place, front to back: 'frames' stereo pairs become 'frames' mono samples.
// Safe to run over the receive buffer itself; returns the mono sample count.
inline uint32_t downmixInPlace(int16_t *interleaved, uint32_t frames) {
  if (!interleaved) return 0;
  for (uint32_t i = 0; i < frames; i++) {
    interleaved[i] = downmix(interleaved[2 * i], interleaved[2 * i + 1]);
  }
  return frames;
}

// ── the heap gate ───────────────────────────────────────────────────────────
// Internal SRAM is the scarce resource on this board: the HUB75 double buffer
// has to stay in it (moving it to PSRAM striped the picture), task stacks
// cannot live in PSRAM, and neither can DMA descriptors.
//
// MEASURED on this panel, 2026-09-15, with the microphones running
// (docs/22 §12.1 and §12.2): 32,952 B free when idle, largest free block
// 20,468 B, capture costs 10.4 KB, and a portal page load spiked the minimum to
// 504 B - at which point Wi-Fi's own allocations began to fail.
//
// The two numbers below are STARTING VALUES, not a standard and not a
// measurement of a player: no player has ever run on this board. They exist so
// the gate is a number rather than an opinion, and they are to be replaced by
// figures measured on the panel before the stream half is enabled. Until then
// they are deliberately conservative - refusing to start is a reported state,
// while starting and exhausting the heap is the hang this module exists to
// avoid repeating.
#ifndef MAPLAYER_MIN_FREE_INTERNAL
#define MAPLAYER_MIN_FREE_INTERNAL 24576   // starting value: 24 KB
#endif
#ifndef MAPLAYER_MIN_LARGEST_BLOCK
#define MAPLAYER_MIN_LARGEST_BLOCK 12288   // starting value: 12 KB
#endif

struct HeapGate {
  uint32_t minFreeInternal = MAPLAYER_MIN_FREE_INTERNAL;
  uint32_t minLargestBlock = MAPLAYER_MIN_LARGEST_BLOCK;
};

enum class GateVerdict : uint8_t {
  Ok = 0,
  LowInternalHeap,     // total free internal SRAM below the floor
  NoLargeBlock,        // enough total, but too fragmented to hold a buffer
};

inline GateVerdict gateCheck(const HeapGate &g, uint32_t freeInternal, uint32_t largestBlock) {
  if (freeInternal < g.minFreeInternal) return GateVerdict::LowInternalHeap;
  if (largestBlock < g.minLargestBlock) return GateVerdict::NoLargeBlock;
  return GateVerdict::Ok;
}

inline const char *gateVerdictName(GateVerdict v) {
  switch (v) {
    case GateVerdict::Ok: return "ok";
    case GateVerdict::LowInternalHeap: return "low internal heap";
    case GateVerdict::NoLargeBlock: return "heap too fragmented";
  }
  return "unknown";
}

// ── the connection state machine ────────────────────────────────────────────
// The order is the one the protocol specifies: Hello, then Server Settings,
// then a Codec Header, and only then may wire chunks be played. A chunk that
// arrives before the codec header is dropped and counted, because playing it
// would mean guessing the sample format.

enum class State : uint8_t {
  Off = 0,        // the module is built but switched off in settings
  Gated,          // refused to start: the heap gate said no
  Searching,      // looking for a server (mDNS or a stored host)
  Connecting,     // socket opening
  Greeting,       // Hello sent, waiting for Server Settings
  WaitingCodec,   // settings in hand, waiting for the Codec Header
  Streaming,      // chunks are playable
  Refused,        // the server offered a codec or format we cannot play
  Lost,           // the connection dropped; backing off before a retry
};

inline const char *stateName(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::Gated: return "gated";
    case State::Searching: return "searching";
    case State::Connecting: return "connecting";
    case State::Greeting: return "greeting";
    case State::WaitingCodec: return "waiting for codec";
    case State::Streaming: return "streaming";
    case State::Refused: return "refused";
    case State::Lost: return "lost";
  }
  return "unknown";
}

inline bool playable(State s) { return s == State::Streaming; }

// ── reconnect backoff ───────────────────────────────────────────────────────
// The same shape the rest of this firmware uses for a lost peer: double from a
// short first retry up to a ceiling, and reset on a connect that sticks. The
// values are ours, not a standard; they are here so the host test can prove the
// sequence never collapses to a tight retry loop, which is the failure that
// costs a panel its Wi-Fi.
constexpr uint32_t kBackoffFirstMs = 1000;
constexpr uint32_t kBackoffMaxMs = 60000;

inline uint32_t nextBackoffMs(uint32_t currentMs) {
  if (currentMs == 0) return kBackoffFirstMs;
  const uint64_t doubled = (uint64_t)currentMs * 2;
  return doubled >= kBackoffMaxMs ? kBackoffMaxMs : (uint32_t)doubled;
}

// ── volume ──────────────────────────────────────────────────────────────────
// Snapcast carries volume as 0..100 inclusive (its specification says so for
// both Server Settings and Client Info). The ES8311's own register 0x32 is a
// different scale entirely - 0x00 is -95.5 dB, one step is 0.5 dB, 0xBF is
// 0 dB - and mapping one onto the other is a hardware decision that needs the
// amplifier on a bench, so it does NOT live in this header. What lives here is
// only the clamp, and the rule that a level we sent ourselves is not read back
// as the user turning the knob.

inline int clampVolume(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

struct Volume {
  int level = 0;        // 0..100, as the server last agreed
  bool muted = false;
  bool known = false;   // false until the server or the user set one
};

// ── counters, for /api/info ─────────────────────────────────────────────────
// Every one of these is a number the panel can be asked for over HTTP, because
// "it stopped playing" is not a diagnosis.
struct Stats {
  uint32_t connects = 0;
  uint32_t drops = 0;
  uint32_t chunks = 0;
  uint32_t chunksBeforeCodec = 0;   // dropped: arrived before the codec header
  uint32_t badMessages = 0;         // failed to parse against the specification
  uint32_t gateRefusals = 0;
  uint32_t underruns = 0;
};

}  // namespace maplayer

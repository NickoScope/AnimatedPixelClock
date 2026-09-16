# src/maplayer: the panel as a Music Assistant player

`src/media/` is the screen and the remote for somebody else's player. This
module is the player itself.

The design, the routes that were compared and the numbers behind every decision
are in `docs/25-ma-media-player.md` in the LED-MATRIX APOLLO knowledge base.
The three things worth knowing before reading the code:

1. **The transport is Snapcast with the server's codec set to `pcm`.** That is
   the only shape in which this board can play music with **no audio decoder**:
   no 28–89 KB of decoder state, no 6–8 % of a core. Music Assistant ships the
   snapserver itself, so nothing extra is installed.
2. **Announcements with the music ducked are not possible here**, and not for
   want of trying. MA implements true ducking on exactly one route — AirPlay —
   where the *server* mixes the clip into the outgoing music. On Snapcast an
   announcement switches streams; on slimproto it stops and resumes. No firmware
   of ours changes that.
3. **The output half is not built.** `MAPLAYER_AUDIO_ENABLED` refuses to
   compile. Debt D1 of `docs/22` §12.3 — the portal's ~20 KB internal-heap spike
   that hangs the panel with the microphones on — has to land first, because a
   stream is a third consumer of the same memory.

## Flags

| Flag | Needs | In |
|---|---|---|
| `MAPLAYER_ENABLED` | `BOARD_WAVESHARE_RGB_MATRIX` (`#error` otherwise) | **not set yet** |
| `MAPLAYER_AUDIO_ENABLED` | `MAPLAYER_ENABLED`, and the two items above | **refuses to build** |

`MAPLAYER_ENABLED` alone gives the client: the socket, the protocol, volume in
both directions, the heap gate, the counters and `/api/info`. It touches no
audio hardware at all, which is what makes it safe to flash before the heap
debt is paid.

Build-time values in `maplayer.h`, each a choice and named as one:
`MAPLAYER_SERVER_PORT` 1704 (Snapcast's stream port, from its specification) and
`MAPLAYER_TIME_SYNC_MS` 1000. The two heap thresholds live in
`maplayer_model.h` with the measurements they came from.

## Files

| | |
|---|---|
| `snap_proto.h` | the Snapcast binary protocol: base header, wire chunk, codec header, RIFF WAVE, the JSON payloads, framing, the time arithmetic. No Arduino, tested on the host |
| `maplayer_model.h` | the rules: the format this board can accept, the stereo→mono downmix, the heap gate, the state machine, the reconnect backoff. No Arduino, tested on the host |
| `maplayer.cpp` | the socket, the protocol state machine, NVS, `/api/info` |
| `../../tools/maplayer/check_maplayer.py` | the host test — **136 checks** over the protocol and the rules |

## The protocol, and where it comes from

Every layout in `snap_proto.h` is from `badaix/snapcast`, `doc/binary_protocol.md`,
read in full — not from memory. The parts that shape the code:

- the protocol is **little endian**, and the base message is **26 bytes**:
  type, id, refersTo (three `uint16`), sent.sec/usec and received.sec/usec
  (four `int32`), size (`uint32`);
- a client joins by opening TCP to **port 1704**, sending **Hello**, then
  receiving **Server Settings** and a **Codec Header** — and *"until the server
  sends this, the client shouldn't play any Wire Chunk messages"*. That sentence
  is why `State::WaitingCodec` exists and why chunks arriving before it are
  counted in `chunksBeforeCodec` rather than played;
- for codec `pcm` the codec header **is a RIFF WAVE header**, which is where the
  sample rate, bit depth and channel count come from. The parser walks the chunk
  list rather than assuming `fmt ` is first, because a WAVE header may legally
  carry other chunks ahead of it;
- **Client Info** (`{"volume": N, "muted": bool}`) is how a player reports its
  own level *back*. That is what makes the knob here move the level in Music
  Assistant rather than only locally.

## The two rules that are not negotiable

**48 kHz, because the bit clock is shared.** ES8311 and ES7210 sit on one BCLK
and one WS on this board, and an I2S port carries a single sample rate for both
directions — confirmed in the HAL of the SDK this firmware compiles against and
in the legacy driver's own comment (*"Since bck and ws are shared, only tx or rx
can be master"*). The microphone DSP already runs at 48 kHz, so the player does
too and the constraint costs nothing. A stream at any other rate is **refused
with a reason**, not resampled silently.

**The downmix, because the codec is mono.** The ES8311 takes the **left channel
only** — register `0x09` bit 7 `SDP_IN_SEL` defaults to it and the chip does no
summing. Playing a stereo stream untouched would silently lose whatever lives
only in the right channel, so `(L+R)/2` is done on the CPU, with the sum taken
in 32 bits so two full-scale samples cannot wrap.

## The heap gate

The panel hangs today with the microphones on: capture holds 10.4 KB of internal
SRAM, a portal page load spikes ~20 KB more, the minimum reached **504 B**, and
Wi-Fi's own allocations began to fail (`docs/22` §12.2). A player would be a
third consumer of the same memory.

So the gate is part of the model, tested on the host, rather than a check
somebody remembers to write in the connect path: below the thresholds the module
**refuses to start and says so** (`state: "gated"`, with the reason and the two
live figures in `/api/info`).

Its two numbers are **starting values, not measurements of a player** — no
player has ever run on this board. They are to be replaced by figures measured
on the panel before the output half is enabled. What the host test proves is the
rule, including that the heap this panel actually has while capturing
(32,952 − 10,400 = 22,552 B) does **not** pass it.

## Cost

| | |
|---|---|
| internal heap | the socket and the module's own statics; every buffer of this module is PSRAM by rule |
| PSRAM | 16 KB receive buffer, allocated once in `maplayerBegin()`. At the built-in snapserver's defaults (48 kHz, 16 bit, stereo, `chunk_ms` 26) one wire chunk is about 5 KB |
| network | 187.5 KB/s while streaming, which is the price of carrying no decoder |
| flash | no library: the protocol is this module's own code |

## What is not done

- **Nothing has ever run on the panel.** No socket has been opened, no byte has
  been received. Everything below the protocol is proven by the host test only.
- **No audio.** `pcmReady()` counts chunks and discards them. The I2S and ES8311
  half is behind a flag that refuses to build.
- **No server discovery.** The host is read from this module's own NVS namespace
  (`maplayer`/`host`); when it is empty the state is `searching` and nothing
  happens. Snapcast advertises `_snapcast-stream._tcp` over mDNS and Music
  Assistant registers it, so discovery is the obvious next step — it is simply
  not written yet, rather than written and untested.
- **No portal card and no page**, and no entry in the flag matrix: this module
  is not in any build's flags yet.
- **`WiFiClient::connect()` blocks.** The timeout is set to 1 second and a
  connect is only attempted on the backoff schedule, so the worst case is one
  ~1 s stall per backoff step rather than `src/mqtt`'s 3 s every 5 s (debt D3).
  Not measured on hardware.
- **The volume map to the codec is missing**, deliberately: Snapcast's 0..100
  and the ES8311's register `0x32` (0.5 dB per step, `0xBF` = 0 dB) are
  different scales, and matching them is a bench decision, not an arithmetic
  one.

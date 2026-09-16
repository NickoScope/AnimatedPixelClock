#pragma once
// The panel as a player in Music Assistant.
//
// What this module is for, and what it deliberately is not: docs/25-ma-media-player.md
// in the LED-MATRIX APOLLO knowledge base. The short version, because it decides
// the shape of everything below.
//
// MA talks to a player over one of several routes. The one chosen here is
// Snapcast with the server's transport codec set to "pcm", because that is the
// only shape in which this board can play music WITHOUT carrying an audio
// decoder: no 28-89 KB of decoder state, no 6-8 % of a core. Music Assistant
// ships the snapserver itself, so nothing extra is installed to use it.
//
// Two things the owner asked for are NOT here, and saying so in the header is
// the point:
//   - Announcements with the music ducked. Music Assistant implements true
//     ducking on exactly one route, AirPlay, and there the SERVER mixes the
//     clip into the music. On Snapcast an announcement switches streams; on
//     slimproto it stops and resumes. No firmware of ours can change that.
//   - The microphones, echo cancellation and a wake word. Espressif's own
//     figure for two microphones plus a reference channel is 79.1 KB of
//     internal SRAM; this panel measured 32,952 B free. It does not fit, and
//     docs/25 §6 shows the arithmetic.
//
// src/media/ is a different module and stays: it is the screen and the remote
// for somebody ELSE's player. This one is the player.

#include <ArduinoJson.h>
#include <stdint.h>

#include "maplayer_model.h"   // the rules, host-tested, in every build that includes us

// Outside the enabled block, so each fires when the flag it needs is missing.
#if defined(MAPLAYER_AUDIO_ENABLED) && !defined(MAPLAYER_ENABLED)
#error "MAPLAYER_AUDIO_ENABLED needs MAPLAYER_ENABLED: the output half feeds this module's client"
#endif

// The output half is designed and not built. Two things must land first, in
// this order, and both are measurements rather than opinions:
//   1. Debt D1 of docs/22 §12.3 - the portal's ~20 KB internal-heap spike. It
//      is the live cause of the panel hanging with the microphones on, and a
//      stream is a third consumer of the same scarce memory. Fixing it first is
//      not caution, it is the difference between a bug and a repeat of a bug.
//   2. The heap gate's two thresholds in maplayer_model.h, which are starting
//      values until they are measured on the panel with a stream running.
#if defined(MAPLAYER_AUDIO_ENABLED)
#error "MAPLAYER_AUDIO_ENABLED is not built: docs/25-ma-media-player.md §7, and debt D1 of docs/22 §12.3 comes first"
#endif

#if defined(MAPLAYER_ENABLED)

#if !defined(BOARD_WAVESHARE_RGB_MATRIX)
#error "MAPLAYER_ENABLED needs BOARD_WAVESHARE_RGB_MATRIX: the ES8311 codec and its amplifier are on this board (src/board/board_i2c.h)"
#endif

// Build-time values. None is a standard; each is a design choice named here,
// and the two heap thresholds live in maplayer_model.h with their measurements.
#ifndef MAPLAYER_SERVER_PORT
#define MAPLAYER_SERVER_PORT 1704   // snapcast's stream port, per its binary protocol
#endif
#ifndef MAPLAYER_TIME_SYNC_MS
#define MAPLAYER_TIME_SYNC_MS 1000  // how often a Time message goes out while streaming
#endif

// ── lifecycle ───────────────────────────────────────────────────────────────
void maplayerBegin();   // setup(), after loadSettings() and the network: NVS, buffers in PSRAM
void maplayerLoop();    // every loop() pass: the socket, the protocol, the gate. Never blocks.

// ── state, for the page and the portal ──────────────────────────────────────
maplayer::State maplayerState();
const char     *maplayerStateName();
bool            maplayerStreaming();
const char     *maplayerServerHost();   // "" until one is found or configured
maplayer::Volume maplayerVolume();
maplayer::Stats  maplayerStats();

// The negotiated stream format, valid only while streaming. Zeroed otherwise.
bool maplayerFormat(uint32_t &sampleRate, uint16_t &bits, uint16_t &channels);

// ── control ─────────────────────────────────────────────────────────────────
// Volume travels to the server as a Client Info message, which is what makes
// the knob here move the level in Music Assistant rather than only locally.
bool maplayerSetVolume(int level);    // 0..100, clamped
bool maplayerSetMuted(bool muted);
void maplayerSettingsChanged();       // after the portal or an import changed one of ours

// ── the portal and /api/info ────────────────────────────────────────────────
void maplayerInfoJson(JsonObject out);   // /api/info's "maplayer"

#endif  // MAPLAYER_ENABLED

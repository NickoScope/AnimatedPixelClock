/*
 * The board's two microphones as a source for the audio visualizer.
 *
 * A task on core 0 captures 48 kHz from the ES7210 over I2S and runs the DSP
 * (audio_dsp.h). Every 40 ms it has the same 164-byte "FFT1" packet the PC
 * companion sends; audioPoll() in loop() hands it to vizIngest(), so every
 * existing effect draws the room without knowing where the bytes came from.
 *
 * Source: settings.audioSource. Auto lets the PC's packets win while they keep
 * arriving and falls back to the microphones 1.5 s after they stop. PC ignores
 * the microphones; mic drops the PC's spectrum packets (its stats JSON is
 * untouched). -DAUDIO_MIC_ONLY fixes the source at mic.
 *
 * Needs -DBOARD_WAVESHARE_RGB_MATRIX and PSRAM. docs/22 in the KB.
 */
#pragma once

#if defined(AUDIO_MIC_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>

#include "audio_dsp.h"

enum AudioSource : uint8_t { AUDIO_SRC_AUTO = 0, AUDIO_SRC_PC = 1, AUDIO_SRC_MIC = 2 };

// setup(): allocates the DSP buffers in PSRAM, once. Starts nothing: the capture
// task and the I2S driver, ~10 KB of internal RAM, exist only while audioPoll()
// wants them.
void audioBegin();

// loop(), every pass: vizShown is whether the visualizer is on screen. It starts
// the capture task when the visualizer shows the microphones (auto without a PC
// stream, or mic) and asks it to stop 25 s after that ends; the task uninstalls
// I2S and deletes itself, then this powers the ES7210 down. The DSP itself runs
// only while the microphones are on screen (and 5 s after).
// This is also where the ES7210 is configured over I2C, once MCLK runs and the
// shared bus is started (boardI2cBegin), and again after a stall.
void audioPoll(bool vizShown);

// The microphone was asked for and has neither come up nor given up for good.
// Read it beside vizShouldDisplay(): that answer is true because the microphone
// is feeding the visualiser, so it cannot by itself decide whether the
// microphone may run - a first attempt that failed would be shut out before its
// own retry came round.
bool audioStartPending();

// network.cpp, for each "FFT1" packet from the PC: false when it must be dropped.
bool audioAcceptPcPacket();

// The newest DSP frame, for effects that want levels, peaks and beats.
// False until the first frame.
bool audioSnapshot(audiodsp::Frame &out);

// After a settings change: clamps them and hands them to the task.
void audioApplySettings();

// /api/info
void audioInfoJson(JsonObject out);

#endif  // AUDIO_MIC_ENABLED

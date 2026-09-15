# src/audio: the onboard microphones

The Waveshare ESP32-S3-RGB-Matrix has two analog microphones on an ES7210 ADC.
This module turns them into a source for the audio visualizer
(`-DAUDIO_MIC_ENABLED`, matrix-waveshare-rgb only). The design, and a source for
every pin, register and threshold: `docs/22-audio-visualizer-onboard-mic.md` in
the LED-MATRIX APOLLO knowledge base.

| File | What it is |
|---|---|
| `es7210.h/.cpp` | the ADC's I2C register sequence, as Espressif's esp_codec_dev runs it |
| `audio_dsp.h/.cpp` | portable DSP: bands, gate, AGC, levels, peaks, beats, waveform, the `FFT1` packet; held to `tools/audiofx/dsp.py` by `make -C tools/audiofx/host check` |
| `audio_mic.h/.cpp` | the capture task, the source switch (auto / PC / mic), the hand-over to the visualizer, `/api/info` |

## Tasks

| Task | Core | Does |
|---|---|---|
| `audio` (priority 5) | 0 | I2S0 at 48 kHz, MCLK on IO12; the DSP while the visualizer shows the microphones; frames into a spinlocked handover |
| `loopTask` | 1 | `setup()`, then `loop()`: `audioPoll()` configures the ES7210 once MCLK runs, feeds the visualizer; web handlers change settings |

`setup()` and `loop()` both run in `loopTask` (arduino-esp32 2.0.17,
`cores/esp32/main.cpp`: `loopTask()` calls `setup()` once, then `loop()`).

## The I2C bus: board_i2c, and the loop task only

IO47/IO48 carry the SHTC3, ES7210, ES8311, PCF85063 and QMI8658. One module owns
the bus: `src/board/board_i2c`, which `setup()` begins once with
`boardI2cBegin()` (100 kHz, Wire's 50 ms timeout). Do not edit those files here;
they are shared byte for byte with the climate branch.

**Rule: every Wire call in this fork runs on the loop task.** Wire's mutex covers
a write (`beginTransmission()` to `endTransmission()`) and a read's transfer
(`requestFrom()`), but the bytes a read received wait in Wire's single receive
buffer, and `read()` / `available()` take no lock. A second task's `requestFrom()`
between this task's `requestFrom()` and its `read()` overwrites them, and this
task reads the other's bytes. `src/board/board_i2c.h` lays this out with file
and line in arduino-esp32 2.0.17's `Wire.cpp`.

How this module keeps the rule:
- `es7210::*` is called from `codecBringUp()` in `audioPoll()` and from
  `audioApplySettings()`, which `audioBegin()` in `setup()` and the portal's
  `/save` and settings import call. All run on `loopTask`.
- `captureTask` never touches I2C. After an I2S stall it only clears
  `s_codecUp`; the loop task brings the codec up again.
- Before a sequence the driver checks `boardI2cReady()` and `boardI2cLinesHigh()`.
  It times every transaction, and the first one over 100 ms, a timeout or a NACK
  ends the sequence with nothing more sent. A held line costs no transaction; a
  bus that stalls mid-sequence costs one second, and the bring-up backs off a
  minute. `/api/info`: `audioMic` "no i2c bus", "i2c held low", "i2c stalled";
  `audioI2cHeld`, `audioI2cStalls`.
- Build with `-DAUDIO_DEBUG` to make the driver's I2C helpers print the calling
  task and `abort()` when it is not `loopTask`. flag_matrix.py compiles it.

A new I2C user: call `boardI2cBegin()` (idempotent), check `boardI2cLinesHigh()`
before a transaction, and run from the loop task. Work that belongs elsewhere
sets a flag that `loop()` services.

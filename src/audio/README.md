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
| `audio` (priority 5), only while wanted | 0 | I2S0 at 48 kHz, MCLK on IO12; the DSP while the visualizer shows the microphones; frames into a spinlocked handover |
| `loopTask` | 1 | `setup()`, then `loop()`: `audioPoll()` configures the ES7210 once MCLK runs, feeds the visualizer; web handlers change settings |

Visualizer styles 2 (Code EQ, in `VIZ_WOW_ENABLED` builds) and 7-14 draw from
this module's DSP frames (`src/viz/wow`); from a PC stream they get the same
frame rebuilt from each packet (`viz_frame.cpp`).

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

## Start and stop

The task (5,120 B stack and its TCB, both internal RAM: `portmacro.h:436-437` in
the S3 port) and the I2S driver (4 x 1,024 B of DMA buffers, `MALLOC_CAP_DMA`, plus
its object and queues from `malloc`: IDF 4.4.7 `driver/i2s.c:740, 747, 851-868,
1964, 2018`) exist only while needed; the DSP buffers are PSRAM, allocated once in
`audioBegin()`.
- **Start**, from `audioPoll()`: the visualizer is on screen and it wants the
  microphones (source mic, `AUDIO_MIC_ONLY`, or auto with no PC stream for 1.5 s).
  A failed start retries after 30 s.
- **Stop**, 25 s after that ends (the visualizer off, or a PC stream arriving):
  `audioPoll()` sets a request; the task leaves its read loop within 100 ms, calls
  `i2s_driver_uninstall()`, records its own stack high-water mark and deletes
  itself; the next `audioPoll()` sees it gone, powers the ES7210 down with
  `es7210::end()` and reports `audioMic` "idle". No other task ever calls
  FreeRTOS on the task's handle.
- `/api/info`: `audioMicRunning`, `audioMicStarts` (churn), `audioStackFreeBytes`
  (the task's own last reading), `audioInternalBytes` (free internal heap before
  the start minus after the codec came up).

## Backlog

- `es7210::begin()` sends about 80 I2C transactions in one loop pass (40-80 ms at 100 kHz), again every ~6 s while I2S keeps stalling.
- `audioApplySettings()` ignores `es7210::lastStalled()`: a gain write that stalls is not backed off.
- `/api/info` does not report the loop task's own stack margin.

From the delta audit of `e7ad859` (2026-09-15), LOW, not fixed:
- `es7210::end()` runs after MCLK has stopped; if the codec ignores I2C without MCLK, the mic bias stays on.
- A stop that coincides with an I2S install failure reports "idle" instead of "i2s failed".
- `startCapture()` resets `s_codecRetryMs`, which drops the 60 s I2C back-off.
- `s_meterDb` and `s_meterClipped` are not reset on stop, so an idle `/api/info` shows a stale level.
- A partial PSRAM failure in `audioBegin()` leaks the blocks it did get (once, in `setup()`).
- A start right after `finishStop()` can hold two task stacks (~5.5 KB) until the idle task frees the old one.
- GPIO11 floats from reset until `setup()` drives it low after `loadSettings()`.

Measured on the panel, build `c71bdb5`, 2026-09-15 (KB docs/22 §12.1):
- DSP 11.8–15.1 ms per 20 ms frame on core 0, max 19.8 ms; 1 overrun in about 110 s. Try the esp-dsp FFT.
- Task stack: 1,232 of 5,120 B used. There is room to trim to about 3 KB.
- `audioInternalBytes` reported 6,340 B while free internal heap fell by 10.4 KB on start, so the figure under-reports.
- `loopMaxMs` up to 43 ms while styles render (5–11 ms idle).

From the audits of the heap diagnostics (`4687134`) and of Code EQ (`a55a477`), 2026-09-15, LOW, not fixed:
- `loopMark()` reports only minimum drops of 1 KB or more; smaller steps add up silently.
- `loopMark()` sets `s_markUs` before printing, so the print time is charged to the next part.
- The failed-allocation counter and task name can tear when two cores write at once; diagnostics only.
- `wow_matrix.cpp:138` has no clamp of its own: a level above 1.0625 would write `stack[c][-1]`. It is unreachable today because both sources feed an EMA of byte/255. Clamp `hgt` to [0, ROWS] and `bass`/`treble`/`m` to [0, 1].
- When the stream stops, frozen bars stay under "No audio data" for up to 10 s (the same in styles 7-14).
- `visualizer.cpp:49`: the Phosphor Waterfall's 832 B of BSS are dead in flag builds; the comments in `wow.h` ("0..7") and `viz_frame.h` ("7-14") are stale.
- `wow_matrix.cpp:39,48,66,125`: a local `col` shadows `wow::col()`.

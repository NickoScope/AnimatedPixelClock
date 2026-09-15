# The board's temperature and humidity sensor

The Waveshare ESP32-S3-RGB-Matrix carries a Sensirion **SHTC3**. This module
finds it and reads it without ever making `loop()` wait on the bus. It reports:
- the reading in `/api/info`;
- a card in the portal's Clock page;
- two Home Assistant sensors, on request, over the MQTT bus.

The weather screen draws it as design B, "outside | inside". The owner chose it
on 2026-09-15 from the previews in `tools/climate/preview/`. The design and its
sources are in the knowledge base, `docs/21-onboard-climate-sensor.md`.

## Hardware

| | | Source |
|---|---|---|
| Part | SHTC3, U6 on the schematic, 100 nF at its VDD, supplied from 3V3 | Waveshare schematic; `pedrominatel/shtc3` in the vendor BSP |
| Address | 0x70 | datasheet Table 8; the vendor driver's `SHTC3_I2C_ADDR` |
| Pins | SDA GPIO47, SCL GPIO48 | vendor BSP `config.h`, `08_Sensor_Test.ino` |
| Bus | IO47/IO48 are 1.8 V on this module. The schematic's NDC7002N (M2) shifts them to the 3.3 V `I2C_SDA`/`I2C_SCL`, with 4.7 k pull-ups on both sides | ESP32-S3 datasheet, note on GPIO47/48 of S3R16V; schematic |
| Same bus | PCF85063 RTC (0x51), QMI8658 IMU, ES8311 and ES7210 codecs. This firmware uses none of them | schematic, vendor BSP |

## Flags

| Flag | Needs | In |
|---|---|---|
| `CLIMATE_ENABLED` | `BOARD_WAVESHARE_RGB_MATRIX` (`#error` otherwise): the bus is `src/board/board_i2c.h` | `matrix-waveshare-rgb` |
| `MQTT_BUS_ENABLED` | optional: adds the Home Assistant discovery and its switch | `matrix-waveshare-rgb` |

`tools/flag_matrix.py` builds three rows with the module:
- "climate + bus";
- "climate without MQTT";
- "everything".

## Files

| | |
|---|---|
| `shtc3.h` | the datasheet: commands, timing, CRC-8, ID mask, conversions. No I/O, tested on the host |
| `climate_model.h` | after the sensor: smoothing, offsets, humidity at the corrected temperature (Magnus), staleness, the settings' bounds. Tested on the host |
| `climate_reader.h` | the reading cycle as a state machine over a Port: one transaction per pass at most, the held-line check, the stall back-off. Tested on the host on a mock bus |
| `climate.h/.cpp` | Wire behind the reader's Port, the corrections, `/api/info`'s object, the pause, the Home Assistant discovery |
| `../board/board_i2c.h/.cpp` | the board's shared I2C bus: begun once, 100 kHz, the line check, and what Wire's mutex covers across tasks |
| `../clocks/weather_layout.h` | the weather clock on any GFX target, design B included; render.py's `CL_B_*` name for name |
| `../clocks/clock_weather.cpp` | gathers the time, the weather and `climateGet()` for it |

## The cycle

`climate_reader.h`, tested on the host. Each `loop()` pass makes **at most
one** I2C transaction; a step that needs two spreads over two passes.

| Pass | Transaction | Then |
|---|---|---|
| due | none. SDA and SCL must both read high; if one is low, look again 5 ms later, and if it still is, skip the cycle for a minute (`busStuck`) | |
| wake up | write 0x3517 | wait 1 ms (tPU is at most 240 µs) |
| after three failed cycles in a row | write the soft reset 0x805D | wait 1 ms |
| first contact, and after a soft reset or a stall | write read-ID 0xEFC8 | |
| | read 3 bytes: the CRC and the product code (`xxxx'1xxx'xx00'0111`) | |
| measure, normal mode, temperature first, **clock stretching off** | write 0x7866 | wait 15 ms (tMEAS is at most 12.1 ms) |
| | read 6 bytes and check both CRCs. A NACK means it is still measuring: up to three more tries, 5 ms apart | |
| sleep | write 0xB098 | the next reading one interval later (default 10 s) |

A first reading is six passes with a transaction; later readings are four.

**A held bus.** Wire's timeout does not bound a transaction. ESP-IDF v4.4.7
waits at least a second for the I2C driver's next event, and a line held low
sends none (`src/board/board_i2c.h` gives the source lines). A NACK still comes
back at once. So:
- the line check above keeps transactions off a bus whose lines read low;
- every transaction is timed. One slower than 100 ms, or that Wire reports as a
  timeout (code 5), ends the cycle with nothing more sent, not even the sleep
  command. Nothing goes on the bus for a minute (`busStalls`), and the ID is
  read again afterwards.

A bus that fails that way costs `loop()` at most one second a minute.

**The bus belongs to the board.** `boardI2cBegin()` (100 kHz, Wire's 50 ms
timeout) runs once in `setup()` before any module; `climateBegin()` calls it
again, which costs nothing.

- **Absent.** Three unanswered looks, 2 s apart, make the sensor absent. It is
  looked for again once a minute. Something at 0x70 with a foreign ID counts as
  absent, and gets nothing more than that look.
- **Failing.** A sensor that was found and then stops answering, or keeps
  failing its CRC, is retried 2 s later. Every third failed cycle in a row
  starts with a soft reset (0x805D) and a new ID check. After that it is retried
  once an interval.
- **Stale.** A reading older than three intervals, and at least 30 s old, is
  stale. The panel then shows dashes.

## What is reported

The reported temperature and humidity are built in this order:
1. The sensor's values are smoothed with a 60 s time constant.
2. The temperature offset is added.
3. With "Correct the humidity with the temperature" on (the default), the
   humidity is recomputed at the corrected temperature.
4. The humidity offset is added.

Step 3 keeps the vapour pressure and scales the RH by `e_w(t_sensor) /
e_w(t_room)`. It follows Sensirion's "Introduction to Humidity", eq. (3) and
(7); their design guide asks for it. Because smoothing comes before the
offsets, a new offset shows at once.

`/api/info` carries a `climate` object:
- `state`: `off`, `probing`, `ok`, `stale` or `absent`;
- `intervalS`;
- `tempC`, `humidity`, `sensorTempC`, `sensorHumidity`, `ageS`, while a reading
  exists;
- the counters `reads`, `crcErrors`, `i2cErrors`, `softResets`, `busStuck`
  (cycles skipped on a held line) and `busStalls` (transactions that stalled);
- `id`, the ID register as read (`"0x...."`). An SHTC3 has
  `id & 0x083F == 0x0807` (datasheet Table 15);
- `foreignDevice`, if something other than an SHTC3 answered;
- `pausedS`, while `/api/climate/pause` holds the reader;
- with the bus, `ha` and `haPublishes`.

`GET /api/climate/pause?s=0-600` starts no new reading for that long, so the
weather screen's stale state can be seen on a healthy board; `s=0` resumes. It
is runtime only and not saved.

## Settings

The settings live in the main `Settings` struct, NVS namespace `pcmonitor`. They
are exported and imported with the rest. Their bounds are in
`climate_model.h`.

| Portal field | Key | NVS | Default | Bounds |
|---|---|---|---|---|
| Read the board's sensor | `climateEnabled` | `climEn` | on | |
| Read every, seconds | `climateIntervalS` | `climIvl` | 10 | 5-300 |
| Temperature offset, °C | `climateTempOffset` (tenths) | `climTOff` | 0 | ±20.0 |
| Humidity offset, %RH | `climateHumOffset` (tenths) | `climHOff` | 0 | ±20.0 |
| Correct the humidity with the temperature | `climateRhFollowsT` | `climRhT` | on | |
| On the weather screen | `climateShow` | `climShow` | 1 Outside \| inside | 0 off |
| Publish to Home Assistant | `climateHa` | `climHa` | **off** | only in builds with the bus |

The card is `data-need="climate"`: `/api/portal` lists the feature only in
`CLIMATE_ENABLED` builds. `handleSave()` touches these settings only when the
card's fields were posted.

## The weather screen

Design B, drawn by `drawWeatherScreen()` in `src/clocks/weather_layout.h`.
`climate::weatherIndoor()` decides what it shows (host tested):

| `/api/info` `climate.state` | On the weather screen | Screen |
|---|---|---|
| `ok` | Outside \| inside | the split with the values |
| `stale` | Outside \| inside | the split with `--.-°` and `--%`, all dim |
| `absent`, `probing`, `off` | any | today's screen |
| any | Off | today's screen |

- **Probing** counts as absent, so a board without the part shows no dashes
  while it is being looked for.
- **Found but silent.** A sensor found but giving no reading turns stale after
  three intervals (at least 30 s).
- **The unit letter.** A three-character outside temperature (-10 °C and below,
  100 °F and above) keeps its degree mark and drops the letter.
- **No heap and no I2C on a frame.** `climateGet()` reads the reader's
  snapshot, and the corrected reading is cached, so a frame that changed
  nothing runs no `exp()`.

`tools/climate/check_weather_screen.py` holds the header's frames to the
previews, pixel for pixel.

## Home Assistant

The switch is off by default, so no entities appear until someone asks for
them. When it is on and the sensor has been found:

**Configs**, retained, sent again on every connect:
- topics `homeassistant/sensor/nickoscope_matrix_<mac3>/indoor_temperature/config`
  and `.../indoor_humidity/config`;
- `device_class` temperature (°C) and humidity (%);
- `state_class` measurement;
- `expire_after` three times the larger of the interval and 60 s;
- one device, "ESP32-S3-RGB-Matrix".

**State**, not retained:
- topic `nickoscope_matrix/<mac3>/climate/state`, payload `{"t":23.4,"rh":45.0}`;
- sent when the value changes by 0.1 °C or 1 %RH, and at least once a minute
  while readings arrive.

Switching the setting (or the sensor) off publishes empty configs, which
removes the entities.

**Why retained.** Home Assistant's docs prefer re-sending on its birth message.
That costs a subscription and a handler, and `mqtt_bus.cpp` has eight of each
with seven taken.

## Checks

- `python3 tools/climate/check_climate.py`: the arithmetic against the
  datasheet's examples. The pre-commit hook runs it.
- `python3 tools/climate/render.py`: the previews and their layout budget.
- `python3 tools/climate/check_weather_screen.py`: the weather screen's firmware
  drawing on the host's Adafruit GFX against the previews, pixel for pixel. The
  pre-commit hook runs it.
- `python3 tools/flag_matrix.py`.

## Not verified on hardware

- That the SHTC3 answers at 0x70 (an I2C scan).
- The ID the part returns.
- Whether 100 kHz through the level shifter is clean.
- **The self-heating offset.** Measure it on the panel: after 30 minutes of
  normal display use, compare with a reference thermometer next to it. The
  plan is in docs/21.
- The heap the I2C driver takes (`freeInternalHeap` in `/api/info` before and
  after).
- The time `loop()` spends in "climate" (`loopSlowPart`). Home Assistant's
  publishing is marked "climate ha", so "climate" is I2C only.
- Whether the line check ever reads a low line from another module's
  transaction once the ES7210 shares the bus (`busStuck` should stay 0).
- Design B on the panel: live, stale (with the pause) and the fallback, and the
  indoor colours as the panel shows them.

## Upstream

**Keralots' AnimatedPixelClock** would take the SHTC3 as local temperature for
the weather clock. For that PR:
- `shtc3.h`, `climate_model.h` and the reader;
- no flag: compiled for the Waveshare env and found by its ID;
- the portal card, and `/api/info`;
- design B: `weather_layout.h` and `clock_weather.cpp`. Compare upstream's
  weather clock with the fork's first.

**Stays in the fork:**
- the Home Assistant discovery (the MQTT bus is fork-only);
- the flag and its flag-matrix rows.

## Backlog

From the code audit of 776fc04..7a0c49b (2026-09-15). Not fixed on purpose:
- **Export units.** `/api/export` writes the offsets in tenths, while the
  portal shows and posts °C and %RH.
- **Offset fields.** An empty or non-numeric offset field goes through
  `toFloat()` and `lroundf()` unchecked (NaN).
- **`clampShow()`.** An out-of-range stored value falls back to 0 (off), not to
  the default split.
- **The bus while disabled.** `Wire.begin` runs even when the sensor is
  switched off (now `boardI2cBegin()` in `setup()`).
- **Empty retained configs.** While Home Assistant publishing is off, empty
  retained configs go out on every connect.
- **AM/PM twice.** `weather_layout.h` repeats the logic of
  `drawMeridiemIndicator()`.
- **C-style casts** in the new code.
- **Pillow.** The pre-commit hook's `check_weather_screen.py` needs it.
- **`%.1f`.** The panel formats a float and the previews a double, so they can
  differ at an exact .x5.

From the final audit of the merged branch `b26f764` (2026-09-15), MINOR, not fixed:
- **`Reader::stop()`.** The sleep command goes out without the per-transaction
  timing the read path has.
- **Loop-task assert.** `WirePort` has no `-DAUDIO_DEBUG`-style check that it
  runs on the loop task; es7210 has one.
- **`/api/climate/pause?s=abc`.** A non-numeric value parses as 0 and silently
  ends the pause instead of answering 400.

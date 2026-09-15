# The board's temperature and humidity sensor

The Waveshare ESP32-S3-RGB-Matrix carries a Sensirion **SHTC3**. This module
finds it and reads it without ever making `loop()` wait on the bus. It reports:
- the reading in `/api/info`;
- a card in the portal's Clock page;
- two Home Assistant sensors, on request, over the MQTT bus.

The weather screen does not draw it yet. Three designs wait for the owner in
`tools/climate/preview/`. The design and its sources are in the knowledge base,
`docs/21-onboard-climate-sensor.md`.

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
| `CLIMATE_ENABLED` | `BOARD_WAVESHARE_RGB_MATRIX`, or `CLIMATE_I2C_SDA` and `CLIMATE_I2C_SCL` (`#error` otherwise) | `matrix-waveshare-rgb` |
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
| `climate.h/.cpp` | the reader in `loop()`, `/api/info`'s object, the Home Assistant discovery |

## The cycle

One reading follows the datasheet's section 5.4 and is one I2C transaction per
`loop()` pass:

| Step | Command | Then |
|---|---|---|
| wake up | 0x3517 | wait 1 ms (tPU is at most 240 µs) |
| on first contact, and after a soft reset | read ID 0xEFC8, check the CRC and the product code (`xxxx'1xxx'xx00'0111`) | |
| measure, normal mode, temperature first, **clock stretching off** | 0x7866 | wait 15 ms (tMEAS is at most 12.1 ms) |
| read six bytes | | a NACK means it is still measuring: up to three more tries 5 ms apart |
| check both CRCs | | |
| sleep | 0xB098 | the next reading one interval later (default 10 s) |

The bus runs at 100 kHz with a 20 ms transaction timeout. No command asks for
stretching, so a healthy transaction takes about a millisecond.

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
- the counters `reads`, `crcErrors`, `i2cErrors`, `softResets`;
- `foreignDevice`, if something other than an SHTC3 answered;
- with the bus, `ha` and `haPublishes`.

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
| On the weather screen | `climateShow` | `climShow` | 0 off | 1 line, 2 badge, 3 split. Stored, not drawn yet |
| Publish to Home Assistant | `climateHa` | `climHa` | **off** | only in builds with the bus |

The card is `data-need="climate"`: `/api/portal` lists the feature only in
`CLIMATE_ENABLED` builds. `handleSave()` touches these settings only when the
card's fields were posted.

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
- The time `loop()` spends in "climate" (`loopSlowPart`).

## Upstream

**Keralots' AnimatedPixelClock** would take the SHTC3 as local temperature for
the weather clock. For that PR:
- `shtc3.h`, `climate_model.h` and the reader;
- no flag: compiled for the Waveshare env and found by its ID;
- the portal card, and `/api/info`;
- the weather-screen design once chosen.

**Stays in the fork:**
- the Home Assistant discovery (the MQTT bus is fork-only);
- the flag and its flag-matrix rows.

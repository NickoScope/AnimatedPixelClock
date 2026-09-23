#pragma once
// The board's own temperature and humidity sensor: the Sensirion SHTC3 on the
// Waveshare ESP32-S3-RGB-Matrix, read from loop(), at most one I2C
// transaction a pass and none while a line is held low. Found by its ID register; nothing to set up. src/climate/README.md,
// and docs/21-onboard-climate-sensor.md in the knowledge base for the sources.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "climate_model.h"   // ClimateState, and the bounds of the settings

#if defined(CLIMATE_ENABLED)

#if !defined(BOARD_WAVESHARE_RGB_MATRIX)
#error "CLIMATE_ENABLED needs BOARD_WAVESHARE_RGB_MATRIX: the SHTC3 is on the board's I2C bus (src/board/board_i2c.h)"
#endif

struct ClimateReading {
  ClimateState state;
  bool     have;             // a good reading exists; the values below are meaningless without one
  float    tempC;            // what the panel reports: smoothed, offset, humidity compensated
  float    humidity;
  float    sensorTempC;      // smoothed, as the sensor reads, before the offsets
  float    sensorHumidity;
  uint32_t ageMs;            // since the last good reading
};

void climateBegin();                    // in setup(), after loadSettings()
void climateLoop();                     // every loop() pass: at most one I2C transaction, none on a held bus
#if defined(MQTT_BUS_ENABLED)
void climateHaLoop();                   // every loop() pass after it: the Home Assistant sensors, MQTT only
#endif
ClimateReading climateGet();
void climateNoteShown();                 // a screen shows the indoor reading now: keep reading (see climate.cpp)
const char *climateStateName(ClimateState s);
void climateSettingsChanged();          // after the portal or an import changed a climate setting
void climatePause(uint32_t seconds);    // no new reading for that long (0 resumes), so the stale state can be seen; not saved
void climateInfoJson(JsonObject out);   // /api/info's "climate"

#endif  // CLIMATE_ENABLED

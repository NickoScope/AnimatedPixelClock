#pragma once
// The board's own temperature and humidity sensor: the Sensirion SHTC3 on the
// Waveshare ESP32-S3-RGB-Matrix, read from loop() without ever waiting on the
// bus. Found by its ID register; nothing to set up. src/climate/README.md,
// and docs/21-onboard-climate-sensor.md in the knowledge base for the sources.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "climate_model.h"   // ClimateState, and the bounds of the settings

#if defined(CLIMATE_ENABLED)

#if !defined(CLIMATE_I2C_SDA) || !defined(CLIMATE_I2C_SCL)
#if defined(BOARD_WAVESHARE_RGB_MATRIX)
// Waveshare's BSP, example/idf_v5.5.2/components/bsp/esp32_s3_matrix/include/bsp/config.h:
// BSP_I2C_SDA GPIO_NUM_47, BSP_I2C_SCL GPIO_NUM_48. On the schematic IO47/IO48 run at
// 1.8 V and reach the 3.3 V sensor bus through the level shifter M2 (NDC7002N).
#define CLIMATE_I2C_SDA 47
#define CLIMATE_I2C_SCL 48
#else
#error "CLIMATE_ENABLED needs the board's I2C pins: define CLIMATE_I2C_SDA and CLIMATE_I2C_SCL"
#endif
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
void climateLoop();                     // every loop() pass: at most one short I2C transaction
ClimateReading climateGet();
const char *climateStateName(ClimateState s);
void climateSettingsChanged();          // after the portal or an import changed a climate setting
void climateInfoJson(JsonObject out);   // /api/info's "climate"

#endif  // CLIMATE_ENABLED

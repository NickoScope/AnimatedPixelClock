// The board's shared I2C bus: see board_i2c.h.

#include "board_i2c.h"

#if defined(BOARD_WAVESHARE_RGB_MATRIX)

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

static bool s_begun = false;
static bool s_ready = false;

void boardI2cBegin() {
  if (s_begun) return;
  s_begun = true;
  s_ready = Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_HZ);
  if (s_ready) {
    Wire.setTimeOut(BOARD_I2C_TIMEOUT_MS);
  } else {
    Serial.println("[board] Wire.begin failed on SDA 47 / SCL 48: no I2C");
  }
}

bool boardI2cReady() { return s_ready; }

bool boardI2cLinesHigh() {
  return gpio_get_level((gpio_num_t)BOARD_I2C_SDA) == 1 && gpio_get_level((gpio_num_t)BOARD_I2C_SCL) == 1;
}

#endif  // BOARD_WAVESHARE_RGB_MATRIX

// Stand-ins for Arduino.h and the panel in host builds of src/worldclock
// (tools/luasim/wchost). The page draws through display.color565, drawPixel
// and fillRect and reads millis(); nothing else of the firmware is needed.
#pragma once
#include <cstdint>

// As arduino-esp32 2.0.17 cores/esp32/Arduino.h defines them.
#define PI         3.1415926535897932384626433832795
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

uint32_t millis();

struct WcHostDisplay {
  uint16_t px[64][128];

  // MatrixPanel_I2S_DMA::color565 and ::color565to888, copied from
  // ESP32-HUB75-MatrixPanel-I2S-DMA.h (3.0.14), so the host packs a colour and
  // spreads it back exactly as the panel's driver does.
  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  static void color565to888(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (c >> 8) & 0xf8;
    g = (c >> 3) & 0xfc;
    b = (uint8_t)(c << 3);
    r |= r >> 5;
    g |= g >> 6;
    b |= b >> 5;
  }
  void drawPixel(int16_t x, int16_t y, uint16_t c) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) px[y][x] = c;
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    for (int16_t j = 0; j < h; j++)
      for (int16_t i = 0; i < w; i++) drawPixel(x + i, y + j, c);
  }
};

extern WcHostDisplay display;

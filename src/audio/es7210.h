/*
 * ES7210 four-channel audio ADC (Everest Semiconductor), U10 on the Waveshare
 * ESP32-S3-RGB-Matrix. Its MIC1 and MIC2 inputs carry the board's two analog
 * microphones; MIC3/MIC4 are not used here.
 *
 * Control over I2C (SDA 47, SCL 48), audio out on I2S as a slave: the S3
 * drives MCLK, BCLK and LRCK. The register sequence is the one Espressif's
 * esp_codec_dev runs for this chip (esp-adf components/esp_codec_dev/device/
 * es7210/es7210.c, Apache-2.0): es7210_open(), es7210_set_fs() for 16-bit I2S,
 * es7210_start(). Waveshare's BSP drives the same chip through that driver
 * (example/idf_v5.5.2/components/bsp/esp32_s3_matrix/esp32_s3_matrix.c).
 * docs/22 in the KB has the line-by-line sources.
 */
#pragma once

#include <stdint.h>

namespace es7210 {

// 7-bit address. esp_codec_dev spells it 8-bit, 0x80 (es7210_adc.h), which is
// AD1 = AD0 = 0: R36 and R37 are 0 ohm to AGND on the schematic, R32/R33 not fitted.
constexpr uint8_t kAddr = 0x40;

// ES7210 PGA steps (es7210_reg.h, es7210_gain_value_t): 0..33 dB in 3 dB, then 34.5, 36, 37.5.
constexpr float kGainMaxDb = 37.5f;

// Every call below talks I2C on the board's shared bus, which src/board/board_i2c
// starts at 100 kHz. Call them from the loop task only, so all traffic on that
// bus stays on one core. Each returns false while the bus is not started.

// True once boardI2cBegin() has started Wire.
bool busReady();

// True when the chip ACKs its address.
bool probe();

// Reset and configure for MIC1 + MIC2, 16-bit Philips I2S, slave, no TDM, then
// power up and set the PGA. MCLK must already be running. False on any NACK.
bool begin(float gainDb);

// PGA gain for MIC1 and MIC2, rounded to the nearest step the chip has.
bool setGainDb(float gainDb);

// Power the analog side down (es7210_stop()).
bool end();

}  // namespace es7210

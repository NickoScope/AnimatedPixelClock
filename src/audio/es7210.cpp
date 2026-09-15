/*
 * ES7210 bring-up over Arduino Wire. See es7210.h for what the chip is and
 * where the sequence comes from. Each block below names the esp_codec_dev
 * function it mirrors (esp-adf components/esp_codec_dev/device/es7210/es7210.c).
 */
#if defined(AUDIO_MIC_ENABLED)

#include "es7210.h"

#include <Arduino.h>
#include <Wire.h>

namespace es7210 {
namespace {

// BSP_I2C_SDA / BSP_I2C_SCL (bsp/config.h) and BSP_I2C_FREQ_HZ (bsp/esp32_s3_matrix.h).
constexpr int kSda = 47;
constexpr int kScl = 48;
constexpr uint32_t kI2cHz = 400000;

// es7210_reg.h
enum : uint8_t {
  RESET_00 = 0x00, CLOCK_OFF_01 = 0x01, MAINCLK_02 = 0x02, POWER_DOWN_06 = 0x06, OSR_07 = 0x07,
  MODE_CONFIG_08 = 0x08, TIME_CONTROL0_09 = 0x09, TIME_CONTROL1_0A = 0x0A,
  SDP_INTERFACE1_11 = 0x11, SDP_INTERFACE2_12 = 0x12,
  ADC34_HPF2_20 = 0x20, ADC34_HPF1_21 = 0x21, ADC12_HPF1_22 = 0x22, ADC12_HPF2_23 = 0x23,
  ANALOG_40 = 0x40, MIC12_BIAS_41 = 0x41, MIC34_BIAS_42 = 0x42,
  MIC1_GAIN_43 = 0x43, MIC2_GAIN_44 = 0x44,
  MIC1_POWER_47 = 0x47, MIC2_POWER_48 = 0x48, MIC3_POWER_49 = 0x49, MIC4_POWER_4A = 0x4A,
  MIC12_POWER_4B = 0x4B, MIC34_POWER_4C = 0x4C,
};

bool s_wire = false;

bool startWire() {
  // Wire.begin() on a bus another module already started logs a warning and
  // returns true (Wire.cpp, "Bus already started in Master Mode"); it has its
  // own lock, so the capture task on core 0 may share it with loop().
  if (!s_wire) s_wire = Wire.begin(kSda, kScl, kI2cHz);
  return s_wire;
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)kAddr, (size_t)1, true) != 1) return false;
  value = (uint8_t)Wire.read();
  return true;
}

// es7210_update_reg_bit()
bool updateReg(uint8_t reg, uint8_t mask, uint8_t bits) {
  uint8_t v = 0;
  if (!readReg(reg, v)) return false;
  return writeReg(reg, (uint8_t)((v & ~mask) | (mask & bits)));
}

// es7210_gain_value_t: codes 0..11 are 0..33 dB in 3 dB steps, 12/13/14 are 34.5/36/37.5.
uint8_t gainCode(float db) {
  if (!(db > 0.0f)) return 0;
  if (db >= 36.75f) return 14;
  if (db >= 35.25f) return 13;
  if (db >= 33.75f) return 12;
  int step = (int)lroundf(db / 3.0f);
  return (uint8_t)(step > 11 ? 11 : step);
}

// es7210_mic_select() for ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2. The driver
// writes codec->gain here, which nothing ever sets, so on its path the PGA
// lands at 0 dB until esp_codec_dev_set_in_gain(); Waveshare's example sets 30
// dB right after opening. This writes the wanted gain directly: the same end state.
bool micSelect(uint8_t code) {
  bool ok = true;
  for (uint8_t i = 0; i < 4; i++) ok &= updateReg(MIC1_GAIN_43 + i, 0x10, 0x00);
  ok &= writeReg(MIC12_POWER_4B, 0xFF);
  ok &= writeReg(MIC34_POWER_4C, 0xFF);
  for (uint8_t reg : {MIC1_GAIN_43, MIC2_GAIN_44}) {
    ok &= updateReg(CLOCK_OFF_01, 0x0B, 0x00);
    ok &= writeReg(MIC12_POWER_4B, 0x00);
    ok &= updateReg(reg, 0x10, 0x10);
    ok &= updateReg(reg, 0x0F, code);
  }
  ok &= writeReg(SDP_INTERFACE2_12, 0x00);   // not TDM: the driver switches to TDM from three mics
  return ok;
}

}  // namespace

bool probe() {
  if (!startWire()) return false;
  Wire.beginTransmission(kAddr);
  return Wire.endTransmission() == 0;
}

bool begin(float gainDb) {
  if (!startWire()) return false;
  const uint8_t code = gainCode(gainDb);

  // es7210_open()
  bool ok = true;
  ok &= writeReg(RESET_00, 0xFF);
  ok &= writeReg(RESET_00, 0x41);
  ok &= writeReg(CLOCK_OFF_01, 0x3F);
  ok &= writeReg(TIME_CONTROL0_09, 0x30);
  ok &= writeReg(TIME_CONTROL1_0A, 0x30);
  ok &= writeReg(ADC12_HPF2_23, 0x2A);
  ok &= writeReg(ADC12_HPF1_22, 0x0A);
  ok &= writeReg(ADC34_HPF2_20, 0x0A);
  ok &= writeReg(ADC34_HPF1_21, 0x2A);
  if (!ok) return false;                         // nothing ACKs: no chip, stop before the rest
  ok &= updateReg(MODE_CONFIG_08, 0x01, 0x00);   // slave: the S3 drives the clocks
  ok &= writeReg(ANALOG_40, 0x43);
  ok &= writeReg(MIC12_BIAS_41, 0x70);           // 2.87 V
  ok &= writeReg(MIC34_BIAS_42, 0x70);
  ok &= writeReg(OSR_07, 0x20);
  ok &= writeReg(MAINCLK_02, 0xC1);              // with MCLK = 256 fs this matches the 16/32/44.1/48 kHz coefficients
  ok &= micSelect(code);
  uint8_t offReg = 0;
  ok &= readReg(CLOCK_OFF_01, offReg);

  // es7210_set_fs(): es7210_set_bits(16) then es7210_config_fmt(ES_I2S_NORMAL).
  // In slave mode es7210_config_sample() returns before touching the dividers.
  uint8_t iface = 0;
  ok &= readReg(SDP_INTERFACE1_11, iface);
  iface = (uint8_t)(((iface & 0x1F) | 0x60) & 0xFC);
  ok &= writeReg(SDP_INTERFACE1_11, iface);

  // es7210_start()
  ok &= writeReg(CLOCK_OFF_01, offReg);
  ok &= writeReg(POWER_DOWN_06, 0x00);
  ok &= writeReg(ANALOG_40, 0x43);
  ok &= writeReg(MIC1_POWER_47, 0x08);
  ok &= writeReg(MIC2_POWER_48, 0x08);
  ok &= writeReg(MIC3_POWER_49, 0x08);
  ok &= writeReg(MIC4_POWER_4A, 0x08);
  ok &= micSelect(code);
  ok &= writeReg(ANALOG_40, 0x43);
  ok &= writeReg(RESET_00, 0x71);
  ok &= writeReg(RESET_00, 0x41);
  return ok;
}

bool setGainDb(float gainDb) {
  const uint8_t code = gainCode(gainDb);
  bool ok = updateReg(MIC1_GAIN_43, 0x0F, code);
  ok &= updateReg(MIC2_GAIN_44, 0x0F, code);
  return ok;
}

// es7210_stop()
bool end() {
  bool ok = true;
  ok &= writeReg(MIC1_POWER_47, 0xFF);
  ok &= writeReg(MIC2_POWER_48, 0xFF);
  ok &= writeReg(MIC3_POWER_49, 0xFF);
  ok &= writeReg(MIC4_POWER_4A, 0xFF);
  ok &= writeReg(MIC12_POWER_4B, 0xFF);
  ok &= writeReg(MIC34_POWER_4C, 0xFF);
  ok &= writeReg(ANALOG_40, 0xC0);
  ok &= writeReg(CLOCK_OFF_01, 0x7F);
  ok &= writeReg(POWER_DOWN_06, 0x07);
  return ok;
}

}  // namespace es7210

#endif  // AUDIO_MIC_ENABLED

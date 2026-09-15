/*
 * ES7210 bring-up over Arduino Wire. See es7210.h for what the chip is and
 * where the sequence comes from. Each block below names the esp_codec_dev
 * function it mirrors (esp-adf components/esp_codec_dev/device/es7210/es7210.c).
 */
#if defined(AUDIO_MIC_ENABLED)

#include "es7210.h"

#include <Arduino.h>
#include <Wire.h>

#include "../board/board_i2c.h"

#if defined(AUDIO_DEBUG)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// arduino-esp32 2.0.17, cores/esp32/main.cpp:20: the task that runs setup() and loop().
extern TaskHandle_t loopTaskHandle;
#endif

namespace es7210 {
namespace {

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

// One over this, or a timeout (endTransmission() code 5), counts as a stalled bus:
// the sequence it belongs to sends nothing more. The climate reader's limit.
constexpr uint32_t kStallMs = 100;
bool s_stalled = false;

// All Wire use in this fork runs on the loop task (src/audio/README.md): a second
// task's requestFrom() can overwrite Wire's one receive buffer before read()
// (board_i2c.h, point 1). AUDIO_DEBUG builds stop dead on any other caller.
#if defined(AUDIO_DEBUG)
void assertLoopTask(const char *where) {
  if (loopTaskHandle && xTaskGetCurrentTaskHandle() != loopTaskHandle) {
    Serial.printf("[audio] es7210 %s called from task '%s', not loopTask: I2C belongs to the loop task\n", where,
                  pcTaskGetName(nullptr));
    abort();
  }
}
#define ASSERT_LOOP_TASK(where) assertLoopTask(where)
#else
#define ASSERT_LOOP_TASK(where)
#endif

bool timed(uint32_t startMs, bool ok, bool timedOut) {
  if (timedOut || millis() - startMs > kStallMs) s_stalled = true;
  return ok && !s_stalled;
}

bool writeReg(uint8_t reg, uint8_t value) {
  ASSERT_LOOP_TASK("writeReg");
  if (s_stalled) return false;
  const uint32_t t0 = millis();
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t code = Wire.endTransmission();   // Wire.cpp: 0 sent, 2 no ACK, 5 timeout, 4 other
  return timed(t0, code == 0, code == 5);
}

bool readReg(uint8_t reg, uint8_t &value) {
  ASSERT_LOOP_TASK("readReg");
  if (s_stalled) return false;
  const uint32_t t0 = millis();
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  const uint8_t code = Wire.endTransmission(false);
  if (code != 0) return timed(t0, false, code == 5);
  // Loop task only: nobody else's requestFrom() can land between this one and read() (board_i2c.h, point 1).
  const bool got = Wire.requestFrom((uint16_t)kAddr, (size_t)1, true) == 1;
  if (got) value = (uint8_t)Wire.read();
  return timed(t0, got, false);
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
  for (uint8_t i = 0; i < 4; i++) ok = ok && updateReg(MIC1_GAIN_43 + i, 0x10, 0x00);
  ok = ok && writeReg(MIC12_POWER_4B, 0xFF);
  ok = ok && writeReg(MIC34_POWER_4C, 0xFF);
  for (uint8_t reg : {MIC1_GAIN_43, MIC2_GAIN_44}) {
    ok = ok && updateReg(CLOCK_OFF_01, 0x0B, 0x00);
    ok = ok && writeReg(MIC12_POWER_4B, 0x00);
    ok = ok && updateReg(reg, 0x10, 0x10);
    ok = ok && updateReg(reg, 0x0F, code);
  }
  ok = ok && writeReg(SDP_INTERFACE2_12, 0x00);   // not TDM: the driver switches to TDM from three mics
  return ok;
}

}  // namespace

// The bus belongs to src/board/board_i2c: boardI2cBegin() in setup() starts
// Wire on SDA 47 / SCL 48 at 100 kHz, one clock and one timeout for every device
// on it. This driver never begins the bus or sets its clock.
// 100 kHz is also the speed esp_codec_dev drives this chip at
// (components/esp_codec_dev/platform/audio_codec_ctrl_i2c.c, DEFAULT_I2C_CLOCK).
bool busReady() { return boardI2cReady(); }

// A held line sends the IDF driver no event, so a transaction then takes about
// a second whatever Wire's timeout says (board_i2c.h). Look before sending.
bool busFree() { return boardI2cLinesHigh(); }

bool lastStalled() { return s_stalled; }

bool probe() {
  ASSERT_LOOP_TASK("probe");
  s_stalled = false;
  if (!busReady() || !busFree()) return false;
  Wire.beginTransmission(kAddr);
  return Wire.endTransmission() == 0;
}

bool begin(float gainDb) {
  s_stalled = false;
  if (!busReady() || !busFree()) return false;
  const uint8_t code = gainCode(gainDb);

  // es7210_open()
  bool ok = true;
  ok = ok && writeReg(RESET_00, 0xFF);
  ok = ok && writeReg(RESET_00, 0x41);
  ok = ok && writeReg(CLOCK_OFF_01, 0x3F);
  ok = ok && writeReg(TIME_CONTROL0_09, 0x30);
  ok = ok && writeReg(TIME_CONTROL1_0A, 0x30);
  ok = ok && writeReg(ADC12_HPF2_23, 0x2A);
  ok = ok && writeReg(ADC12_HPF1_22, 0x0A);
  ok = ok && writeReg(ADC34_HPF2_20, 0x0A);
  ok = ok && writeReg(ADC34_HPF1_21, 0x2A);
  if (!ok) return false;                         // nothing ACKs: no chip, stop before the rest
  ok = ok && updateReg(MODE_CONFIG_08, 0x01, 0x00);   // slave: the S3 drives the clocks
  ok = ok && writeReg(ANALOG_40, 0x43);
  ok = ok && writeReg(MIC12_BIAS_41, 0x70);           // 2.87 V
  ok = ok && writeReg(MIC34_BIAS_42, 0x70);
  ok = ok && writeReg(OSR_07, 0x20);
  ok = ok && writeReg(MAINCLK_02, 0xC1);              // with MCLK = 256 fs this matches the 16/32/44.1/48 kHz coefficients
  ok = ok && micSelect(code);
  uint8_t offReg = 0;
  ok = ok && readReg(CLOCK_OFF_01, offReg);

  // es7210_set_fs(): es7210_set_bits(16) then es7210_config_fmt(ES_I2S_NORMAL).
  // In slave mode es7210_config_sample() returns before touching the dividers.
  uint8_t iface = 0;
  ok = ok && readReg(SDP_INTERFACE1_11, iface);
  iface = (uint8_t)(((iface & 0x1F) | 0x60) & 0xFC);
  ok = ok && writeReg(SDP_INTERFACE1_11, iface);

  // es7210_start()
  ok = ok && writeReg(CLOCK_OFF_01, offReg);
  ok = ok && writeReg(POWER_DOWN_06, 0x00);
  ok = ok && writeReg(ANALOG_40, 0x43);
  ok = ok && writeReg(MIC1_POWER_47, 0x08);
  ok = ok && writeReg(MIC2_POWER_48, 0x08);
  ok = ok && writeReg(MIC3_POWER_49, 0x08);
  ok = ok && writeReg(MIC4_POWER_4A, 0x08);
  ok = ok && micSelect(code);
  ok = ok && writeReg(ANALOG_40, 0x43);
  ok = ok && writeReg(RESET_00, 0x71);
  ok = ok && writeReg(RESET_00, 0x41);
  return ok;
}

bool setGainDb(float gainDb) {
  s_stalled = false;
  if (!busReady() || !busFree()) return false;
  const uint8_t code = gainCode(gainDb);
  bool ok = updateReg(MIC1_GAIN_43, 0x0F, code);
  ok = ok && updateReg(MIC2_GAIN_44, 0x0F, code);
  return ok;
}

// es7210_stop()
bool end() {
  s_stalled = false;
  if (!busReady() || !busFree()) return false;
  bool ok = true;
  ok = ok && writeReg(MIC1_POWER_47, 0xFF);
  ok = ok && writeReg(MIC2_POWER_48, 0xFF);
  ok = ok && writeReg(MIC3_POWER_49, 0xFF);
  ok = ok && writeReg(MIC4_POWER_4A, 0xFF);
  ok = ok && writeReg(MIC12_POWER_4B, 0xFF);
  ok = ok && writeReg(MIC34_POWER_4C, 0xFF);
  ok = ok && writeReg(ANALOG_40, 0xC0);
  ok = ok && writeReg(CLOCK_OFF_01, 0x7F);
  ok = ok && writeReg(POWER_DOWN_06, 0x07);
  return ok;
}

}  // namespace es7210

#endif  // AUDIO_MIC_ENABLED

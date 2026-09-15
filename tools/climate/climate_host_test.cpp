// Host test for src/climate/shtc3.h and src/climate/climate_model.h, against
// the numbers Sensirion prints: the SHTC3 datasheet (Version 4, December 2022)
// and the humidity design guide (Version 2, March 2024). Built and run by
// tools/climate/check_climate.py.

#include <cmath>
#include <cstdio>

#include "climate_model.h"
#include "shtc3.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((double)(a) - (double)(b)) <= (tol))

static void datasheetCrc() {
  // Table 16: "CRC (0x00) = 0xAC", "CRC (0xBEEF) = 0x92".
  const uint8_t zero[] = {0x00};
  const uint8_t beef[] = {0xBE, 0xEF};
  CHECK(shtc3::crc8(zero, 1) == 0xAC);
  CHECK(shtc3::crc8(beef, 2) == 0x92);
}

static void datasheetFigure7() {
  // Figure 7 is a humidity-first read; its caption: "The physical values of the
  // transmitted measurement results are 63 %RH and 23.7 °C". The bytes, read bit
  // by bit off the figure: humidity 1010'0001 0011'0011, CRC 0001'1100;
  // temperature 0110'0100 1000'1011, CRC 1100'0111.
  const uint8_t rh[] = {0xA1, 0x33, 0x1C};
  const uint8_t t[]  = {0x64, 0x8B, 0xC7};
  CHECK(shtc3::wordValid(rh));
  CHECK(shtc3::wordValid(t));
  NEAR(shtc3::centiPercent(shtc3::wordValue(rh)) / 100.0, 63.0, 0.05);
  NEAR(shtc3::centiDegrees(shtc3::wordValue(t)) / 100.0, 23.7, 0.05);

  // The same words in the order the reader asks for (temperature first).
  uint8_t frame[] = {0x64, 0x8B, 0xC7, 0xA1, 0x33, 0x1C};
  uint16_t st = 0, srh = 0;
  CHECK(shtc3::decodeTemperatureFirst(frame, &st, &srh) == shtc3::Frame::Ok);
  CHECK(st == 0x648B && srh == 0xA133);

  frame[1] ^= 0x01;   // one bit flipped in the temperature word
  CHECK(shtc3::decodeTemperatureFirst(frame, &st, &srh) == shtc3::Frame::BadTemperatureCrc);
  frame[1] ^= 0x01;
  frame[5] ^= 0x80;   // and in the humidity CRC
  CHECK(shtc3::decodeTemperatureFirst(frame, &st, &srh) == shtc3::Frame::BadHumidityCrc);
}

static void conversionEnds() {
  // Section 5.11 at the ends and the middle of the 16-bit range.
  CHECK(shtc3::centiDegrees(0) == -4500);
  CHECK(shtc3::centiPercent(0) == 0);
  CHECK(shtc3::centiDegrees(0x8000) == 4250);    // -45 + 175 / 2
  CHECK(shtc3::centiPercent(0x8000) == 5000);
  NEAR(shtc3::centiDegrees(0xFFFF) / 100.0, 130.0, 0.01);
  NEAR(shtc3::centiPercent(0xFFFF) / 100.0, 100.0, 0.01);
}

static void idRegister() {
  // Table 15: xxxx'1xxx'xx00'0111.
  CHECK(shtc3::isShtc3Id(0x0807));
  CHECK(shtc3::isShtc3Id(0x0887));   // x bits set
  CHECK(shtc3::isShtc3Id(0xF7C7 | 0x0800));
  CHECK(!shtc3::isShtc3Id(0x0007));  // bit 11 clear
  CHECK(!shtc3::isShtc3Id(0x0806));
  CHECK(!shtc3::isShtc3Id(0x0827));  // bit 5 set
  CHECK(!shtc3::isShtc3Id(0xFFFF));
}

static void magnus() {
  // Introduction to Humidity, Table 1: alpha = 6.112 hPa is e_w(0 °C).
  NEAR(climate::saturationHpa(0.0), 6.112, 1e-9);
  CHECK(climate::humidityAt(45.0, 23.0, 23.0) == 45.0);
  NEAR(climate::humidityAt(climate::humidityAt(45.0, 30.0, 23.0), 23.0, 30.0), 45.0, 1e-9);

  // Design guide, section 3: "In extreme cases such as at 90 %RH, a deviation of
  // 1 °C will result in a deviation of the humidity signal of 5 %RH." The guide
  // names no temperature; across a room's 15-30 °C the formula gives 5.0-5.4.
  for (double t = 15.0; t <= 30.0; t += 5.0) {
    const double sensorReads = climate::humidityAt(90.0, t, t + 1.0);   // air at t, sensor 1 °C warmer
    NEAR(90.0 - sensorReads, 5.0, 0.6);
  }
  CHECK(climate::humidityAt(95.0, 30.0, 20.0) == 100.0);   // clamped
}

static void correction() {
  // A board 2 °C warm: offset -2.0 °C, humidity follows.
  climate::Reading r = climate::correct(25.1f, 40.0f, -20, 0, true);
  NEAR(r.tempC, 23.1, 1e-4);
  NEAR(r.humidity, climate::humidityAt(40.0, 25.1, 23.1), 1e-3);
  CHECK(r.humidity > 44.0f && r.humidity < 46.5f);

  r = climate::correct(25.1f, 40.0f, -20, 0, false);   // humidity left alone
  NEAR(r.humidity, 40.0, 1e-4);

  r = climate::correct(25.1f, 40.0f, -20, 30, true);   // then +3.0 %RH
  NEAR(r.humidity, climate::humidityAt(40.0, 25.1, 23.1) + 3.0, 1e-3);

  r = climate::correct(20.0f, 99.0f, 0, 50, true);      // clamped at 100
  CHECK(r.humidity == 100.0f);
  r = climate::correct(20.0f, 1.0f, 0, -50, true);      // and at 0
  CHECK(r.humidity == 0.0f);
}

static void smoothing() {
  climate::Smoother s;
  s.add(24.0f, 40.0f, 10.0f);
  CHECK(s.have && s.t == 24.0f && s.rh == 40.0f);   // the first sample is taken whole
  s.add(26.0f, 50.0f, 60.0f, 60.0f);                 // dt == tau: halfway
  NEAR(s.t, 25.0, 1e-5);
  NEAR(s.rh, 45.0, 1e-5);
  s.reset();
  s.add(10.0f, 10.0f, 10.0f);
  CHECK(s.t == 10.0f);
}

static void staleness() {
  CHECK(climate::staleAfterMs(10) == 30000UL);
  CHECK(climate::staleAfterMs(60) == 180000UL);
  CHECK(!climate::isStale(130000UL, 100000UL, 10));
  CHECK(climate::isStale(130001UL, 100000UL, 10));
  CHECK(!climate::isStale(5000UL, 4294960000UL, 10));   // across the millis() wrap
}

static void bounds() {
  CHECK(climate::clampInterval(0) == 5);
  CHECK(climate::clampInterval(10) == 10);
  CHECK(climate::clampInterval(100000) == 300);
  CHECK(climate::clampOffset(-500) == -200);
  CHECK(climate::clampOffset(-35) == -35);
  CHECK(climate::clampOffset(999) == 200);
  CHECK(climate::clampShow(2) == 2);
  CHECK(climate::clampShow(7) == 0);
  CHECK(climate::clampShow(-1) == 0);
}

int main() {
  datasheetCrc();
  datasheetFigure7();
  conversionEnds();
  idRegister();
  magnus();
  correction();
  smoothing();
  staleness();
  bounds();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}

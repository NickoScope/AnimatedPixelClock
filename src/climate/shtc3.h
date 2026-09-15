#pragma once
// Sensirion SHTC3 humidity and temperature sensor: the protocol constants and
// the arithmetic, with no I/O and no Arduino, so tools/climate/ can test them
// on the host against the datasheet's own examples.
//
// Every number here is from the SHTC3 datasheet, Version 4 - December 2022
// (https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf).
// The table or section is named on each line.

#include <cstddef>
#include <cstdint>

namespace shtc3 {

static const uint8_t kAddress = 0x70;              // Table 8, 7-bit

// Commands: 16 bits, sent MSB first after a write header (section 5).
static const uint16_t kWakeup    = 0x3517;         // Table 10
static const uint16_t kSleep     = 0xB098;         // Table 9
static const uint16_t kSoftReset = 0x805D;         // Table 12
static const uint16_t kReadId    = 0xEFC8;         // Table 14
// Table 11: temperature first, clock stretching DISABLED. With stretching off
// the sensor refuses a read header while it measures (NACK, section 5.5)
// instead of holding SCL low, so the reader polls from loop() and never waits
// on the bus.
static const uint16_t kMeasureNormal   = 0x7866;
static const uint16_t kMeasureLowPower = 0x609C;

// Table 5 maxima (the max column is measured at -40 °C, section 3.1).
static const uint32_t kPowerUpMaxUs       = 240;     // tPU after power-up, tSR after soft reset
static const uint32_t kMeasureNormalMaxUs = 12100;   // tMEAS, normal mode
static const uint32_t kMeasureLowMaxUs    = 800;     // tMEAS, low power mode

// Section 5.10, Table 16: CRC-8, polynomial 0x31 (x^8 + x^5 + x^4 + 1),
// initialisation 0xFF, no reflection, final XOR 0x00, over the two data bytes.
inline uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// One data word as it arrives: two bytes, then their CRC (section 5.6).
inline bool     wordValid(const uint8_t *w) { return crc8(w, 2) == w[2]; }
inline uint16_t wordValue(const uint8_t *w) { return (uint16_t)((w[0] << 8) | w[1]); }

// Section 5.9, Table 15: bits 11 and 5:0 carry the SHTC3 product code,
// xxxx'1xxx'xx00'0111; the x bits may differ from sensor to sensor.
inline bool isShtc3Id(uint16_t id) { return (id & 0x083F) == 0x0807; }

// Section 5.11: T = -45 + 175 * S_T / 2^16 °C and RH = 100 * S_RH / 2^16 %RH.
// In hundredths, rounded, so a test compares integers.
inline int32_t centiDegrees(uint16_t s) { return -4500 + (int32_t)((17500LL * s + 32768) >> 16); }
inline int32_t centiPercent(uint16_t s) { return (int32_t)((10000LL * s + 32768) >> 16); }

enum class Frame : uint8_t { Ok, BadTemperatureCrc, BadHumidityCrc };

// The six bytes a temperature-first measurement returns (section 5.6):
// T MSB, T LSB, T CRC, RH MSB, RH LSB, RH CRC.
inline Frame decodeTemperatureFirst(const uint8_t *b, uint16_t *st, uint16_t *srh) {
  if (!wordValid(b)) return Frame::BadTemperatureCrc;
  if (!wordValid(b + 3)) return Frame::BadHumidityCrc;
  *st  = wordValue(b);
  *srh = wordValue(b + 3);
  return Frame::Ok;
}

}  // namespace shtc3

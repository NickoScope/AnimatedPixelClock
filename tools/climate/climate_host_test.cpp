// Host test for src/climate/shtc3.h and src/climate/climate_model.h, against
// the numbers Sensirion prints: the SHTC3 datasheet (Version 4, December 2022)
// and the humidity design guide (Version 2, March 2024). Built and run by
// tools/climate/check_climate.py.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "climate_model.h"
#include "climate_reader.h"
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
  CHECK(climate::clampShow(1) == 1);
  CHECK(climate::clampShow(2) == 0);
  CHECK(climate::clampShow(7) == 0);
  CHECK(climate::clampShow(-1) == 0);
}

static void weatherScreen() {
  // What the weather screen draws for each state (design B, 2026-09-15 19:10).
  using climate::WeatherIndoor;
  const uint8_t on = climate::kShowSplit, off = climate::kShowOff;
  CHECK(climate::weatherIndoor(true, on, ClimateState::Ok) == WeatherIndoor::Live);
  CHECK(climate::weatherIndoor(true, on, ClimateState::Stale) == WeatherIndoor::Stale);
  CHECK(climate::weatherIndoor(true, on, ClimateState::Absent) == WeatherIndoor::None);    // today's screen
  CHECK(climate::weatherIndoor(true, on, ClimateState::Probing) == WeatherIndoor::None);
  CHECK(climate::weatherIndoor(true, on, ClimateState::Off) == WeatherIndoor::None);
  CHECK(climate::weatherIndoor(false, on, ClimateState::Ok) == WeatherIndoor::None);       // the sensor switched off
  CHECK(climate::weatherIndoor(true, off, ClimateState::Ok) == WeatherIndoor::None);       // "On the weather screen" off
  CHECK(climate::weatherIndoor(true, off, ClimateState::Stale) == WeatherIndoor::None);
  CHECK(climate::weatherIndoor(true, 7, ClimateState::Ok) == WeatherIndoor::None);
}

// ── the reader, on a mock bus (climate_reader.h) ────────────────────────────
// The bus and the clock are simulated: a transaction takes 0.3-0.7 ms, a held
// line makes one take the driver's one-second ceiling, and loop() passes come
// 1 ms apart.
class MockBus : public climate::Port {
 public:
  uint64_t nowUs = 5000000;      // 5 s after boot
  bool present = true;           // an SHTC3 answers at 0x70
  bool sda = true, scl = true;   // the lines' levels
  uint32_t stallUs = 0;          // every transaction takes this long and times out
  uint32_t slowUs = 0;           // the next transaction succeeds, but takes this long
  int transactions = 0;
  std::vector<uint16_t> commands;

  int write(uint16_t cmd) override {
    transactions++;
    commands.push_back(cmd);
    if (stallUs) {
      nowUs += stallUs;
      return 5;                  // Wire's code for ESP_ERR_TIMEOUT
    }
    nowUs += take(300);
    if (!present) return 2;
    if (!awake_ && cmd != shtc3::kWakeup) return 2;   // asleep, it answers nothing but the wake-up (section 5.2)
    switch (cmd) {
      case shtc3::kWakeup: awake_ = true; break;
      case shtc3::kSleep: awake_ = false; break;
      case shtc3::kReadId: pending_ = 1; break;
      case shtc3::kMeasureNormal: pending_ = 2; readyUs_ = nowUs + 10800; break;   // tMEAS, Table 5
      default: break;
    }
    return 0;
  }

  size_t read(uint8_t *buf, size_t n) override {
    transactions++;
    if (stallUs) {
      nowUs += stallUs;
      return 0;
    }
    nowUs += take(700);
    if (!present || !awake_) return 0;
    if (pending_ == 1 && n == 3) {
      const uint8_t id[2] = {0x08, 0x87};                // xxxx'1xxx'xx00'0111 with x bits set
      buf[0] = id[0];
      buf[1] = id[1];
      buf[2] = shtc3::crc8(id, 2);
      pending_ = 0;
      return 3;
    }
    if (pending_ == 2 && n == 6) {
      if (nowUs < readyUs_) return 0;                      // still measuring: NACK (section 5.5)
      const uint8_t frame[6] = {0x64, 0x8B, 0xC7, 0xA1, 0x33, 0x1C};   // Figure 7: 23.7 °C, 63 %RH
      std::memcpy(buf, frame, sizeof(frame));
      pending_ = 0;
      return 6;
    }
    return 0;
  }

  bool linesHigh() override { return sda && scl; }
  uint32_t ms() override { return (uint32_t)(nowUs / 1000); }
  uint32_t us() override { return (uint32_t)nowUs; }
  bool asleep() const { return !awake_; }

 private:
  uint32_t take(uint32_t normal) {
    const uint32_t t = slowUs ? slowUs : normal;
    slowUs = 0;
    return t;
  }
  bool awake_ = false;
  int pending_ = 0;
  uint64_t readyUs_ = 0;
};

// One loop() pass, 1 ms after the one before; the transactions it made.
static int pass(climate::Reader &r, MockBus &bus) {
  bus.nowUs += 1000;
  const int before = bus.transactions;
  r.loop(10);
  return bus.transactions - before;
}

// Passes until done() or maxMs of simulated time. No pass may make more than one transaction.
template <class F>
static bool runUntil(climate::Reader &r, MockBus &bus, uint32_t maxMs, F done) {
  const uint64_t end = bus.nowUs + (uint64_t)maxMs * 1000;
  int worst = 0;
  while (bus.nowUs < end && !done()) {
    const int n = pass(r, bus);
    if (n > worst) worst = n;
  }
  CHECK(worst <= 1);
  return done();
}

static void readerReads() {
  using shtc3::kMeasureNormal;
  using shtc3::kReadId;
  using shtc3::kSleep;
  using shtc3::kWakeup;
  MockBus bus;
  climate::Reader r(bus);
  CHECK(runUntil(r, bus, 1000, [&] { return r.counters().reads == 1; }));
  NEAR(r.temperature(), 23.7, 0.05);
  NEAR(r.humidity(), 63.0, 0.05);
  CHECK(r.state(bus.ms(), 10) == ClimateState::Ok);
  CHECK(r.idRead() && r.id() == 0x0887 && r.ever());
  CHECK(runUntil(r, bus, 100, [&] { return bus.asleep(); }));    // the sleep command, on its own pass
  CHECK(bus.commands == (std::vector<uint16_t>{kWakeup, kReadId, kMeasureNormal, kSleep}));
  CHECK(bus.transactions == 6);                                 // wake, ID, ID read, measure, read, sleep
  bus.commands.clear();
  CHECK(runUntil(r, bus, 11000, [&] { return r.counters().reads == 2; }));   // the next one, without the ID
  CHECK(runUntil(r, bus, 100, [&] { return bus.asleep(); }));
  CHECK(bus.commands == (std::vector<uint16_t>{kWakeup, kMeasureNormal, kSleep}));
  CHECK(r.counters().busStuck == 0 && r.counters().busStalls == 0 && r.counters().i2cErrors == 0);
}

static void readerHeldLine() {
  // SCL held low from the start: nothing is ever sent, the cycle is skipped once a minute.
  MockBus bus;
  climate::Reader r(bus);
  bus.scl = false;
  CHECK(runUntil(r, bus, 1000, [&] { return r.counters().busStuck == 1; }));
  CHECK(bus.transactions == 0);
  CHECK(!runUntil(r, bus, 59000, [&] { return r.counters().busStuck == 2; }));
  CHECK(runUntil(r, bus, 2000, [&] { return r.counters().busStuck == 2; }));
  CHECK(bus.transactions == 0);
  CHECK(r.state(bus.ms(), 10) == ClimateState::Probing);        // never found: the weather screen keeps today's layout
  r.settingsChanged(10);                                        // a settings save does not cut the wait short
  CHECK(!runUntil(r, bus, 30000, [&] { return bus.transactions > 0; }));
  bus.scl = true;                                               // released: the next look reads the sensor
  CHECK(runUntil(r, bus, 31000, [&] { return r.counters().reads == 1; }));
  CHECK(r.counters().busStuck == 2);

  // SDA low for one look only, another module's transaction: a 5 ms wait, not a minute.
  MockBus bus2;
  climate::Reader r2(bus2);
  bus2.sda = false;
  CHECK(pass(r2, bus2) == 0);                                   // Off -> Due
  CHECK(pass(r2, bus2) == 0);                                   // Due: SDA low, look again in 5 ms
  bus2.sda = true;
  CHECK(runUntil(r2, bus2, 100, [&] { return r2.counters().reads == 1; }));
  CHECK(r2.counters().busStuck == 0);
}

static void readerStall() {
  using shtc3::kMeasureNormal;
  using shtc3::kSleep;
  // Found and read once; then every transaction takes the driver's one-second ceiling.
  MockBus bus;
  climate::Reader r(bus);
  CHECK(runUntil(r, bus, 1000, [&] { return r.counters().reads == 1 && bus.asleep(); }));
  bus.stallUs = 1000000;
  const int before = bus.transactions;
  CHECK(runUntil(r, bus, 12000, [&] { return r.counters().busStalls == 1; }));
  CHECK(bus.transactions == before + 1);                        // the wake-up that stalled, and no sleep command after it
  const int after = bus.transactions;
  CHECK(!runUntil(r, bus, 59000, [&] { return bus.transactions != after; }));   // a minute with nothing on the bus
  CHECK(runUntil(r, bus, 2000, [&] { return r.counters().busStalls == 2; }));
  CHECK(bus.transactions == after + 1);
  CHECK(r.state(bus.ms(), 10) == ClimateState::Stale);          // the weather screen shows dashes
  bus.stallUs = 0;                                              // the bus recovers: the ID is read again first
  bus.commands.clear();
  CHECK(runUntil(r, bus, 62000, [&] { return r.counters().reads == 2; }));
  CHECK(bus.commands.size() >= 3 && bus.commands[0] == shtc3::kWakeup && bus.commands[1] == shtc3::kReadId);
  CHECK(r.state(bus.ms(), 10) == ClimateState::Ok);

  // The measurement's read stalls: no sleep command after it either.
  MockBus b2;
  climate::Reader r2(b2);
  CHECK(runUntil(r2, b2, 1000, [&] { return !b2.commands.empty() && b2.commands.back() == kMeasureNormal; }));
  b2.stallUs = 1000000;
  CHECK(runUntil(r2, b2, 2000, [&] { return r2.counters().busStalls == 1; }));
  const size_t sent = b2.commands.size();
  CHECK(b2.commands.back() == kMeasureNormal);
  CHECK(!runUntil(r2, b2, 59000, [&] { return b2.commands.size() != sent; }));
  CHECK(r2.counters().reads == 0);
  CHECK(std::find(b2.commands.begin(), b2.commands.end(), kSleep) == b2.commands.end());

  // Successful but past kStallUs: a stall all the same. Just under: a normal cycle.
  MockBus b3;
  climate::Reader r3(b3);
  CHECK(pass(r3, b3) == 0);
  b3.slowUs = climate::kStallUs + 50000;
  CHECK(pass(r3, b3) == 1);
  CHECK(r3.counters().busStalls == 1);
  CHECK(!runUntil(r3, b3, 59000, [&] { return b3.transactions != 1; }));
  MockBus b4;
  climate::Reader r4(b4);
  CHECK(pass(r4, b4) == 0);
  b4.slowUs = climate::kStallUs - 10000;
  CHECK(runUntil(r4, b4, 1000, [&] { return r4.counters().reads == 1; }));
  CHECK(r4.counters().busStalls == 0);

  CHECK(climate::classifyWrite(0, 400) == climate::Xfer::Ok);
  CHECK(climate::classifyWrite(2, 400) == climate::Xfer::Fail);      // NACK: fast, the sensor absent
  CHECK(climate::classifyWrite(5, 400) == climate::Xfer::Stall);     // Wire's timeout, however fast
  CHECK(climate::classifyWrite(0, climate::kStallUs + 1) == climate::Xfer::Stall);
  CHECK(climate::classifyRead(true, climate::kStallUs) == climate::Xfer::Ok);
  CHECK(climate::classifyRead(false, 700) == climate::Xfer::Fail);
  CHECK(climate::classifyRead(true, 1000000) == climate::Xfer::Stall);
}

static void readerAbsent() {
  MockBus bus;
  bus.present = false;
  climate::Reader r(bus);
  CHECK(runUntil(r, bus, 10000, [&] { return r.state(bus.ms(), 10) == ClimateState::Absent; }));
  CHECK(bus.transactions == 3);                                 // three NACKed wake-ups, 2 s apart
  CHECK(r.counters().busStalls == 0 && r.counters().i2cErrors == 0 && r.counters().busStuck == 0);
  CHECK(!runUntil(r, bus, 55000, [&] { return bus.transactions != 3; }));   // then once a minute
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
  weatherScreen();
  readerReads();
  readerHeldLine();
  readerStall();
  readerAbsent();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}

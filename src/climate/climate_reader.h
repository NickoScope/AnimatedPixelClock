#pragma once
// The SHTC3's reading cycle as a state machine over a Port, with no Arduino.
// climate.cpp gives it Wire on the board's bus, the bus lines and the clock;
// tools/climate/climate_host_test.cpp gives it a mock bus that can hold a line
// low or make a transaction take a second.
//
// One loop() call makes at most one I2C transaction. A first reading takes six
// passes that touch the bus (wake-up, the ID command, its read, the measurement
// command, its read, sleep); later readings four (wake-up, measure, read,
// sleep), plus a soft reset after repeated failures and read retries while the
// sensor still measures. The waits between them (datasheet Table 5) pass while
// loop() does its other work.
//
// Why the reader guards the bus itself. Wire's timeout does not bound a
// transaction: ESP-IDF v4.4.7 waits for the I2C driver's next event for at least
// 1000 ms, and a line held low sends none (src/board/board_i2c.h, with the
// source lines). A NACK, the sensor absent, still comes back at once. So:
//  - before a cycle the reader looks at SDA and SCL, and again kLineRecheckUs
//    later; if a line is still low it skips the cycle for kReprobeMs;
//  - every transaction is timed; one slower than kStallUs, or that Wire reports
//    as a timeout, ends the cycle for kStallBackoffMs with nothing more sent -
//    not even the sleep command, which would stall again - and the ID is read
//    again afterwards.

#include <cstddef>
#include <cstdint>

#include "climate_model.h"
#include "shtc3.h"

namespace climate {

// What the reader needs from the board.
class Port {
 public:
  // Sends a 16-bit command; returns Wire.endTransmission()'s code (arduino-esp32
  // 2.0.17 Wire.cpp:465-471): 0 sent, 2 NACK, 5 timeout, 4 anything else.
  virtual int write(uint16_t cmd) = 0;
  // Reads n bytes; returns how many arrived (n when complete).
  virtual size_t read(uint8_t *buf, size_t n) = 0;
  // SDA and SCL both read high.
  virtual bool linesHigh() = 0;
  virtual uint32_t ms() = 0;
  virtual uint32_t us() = 0;

 protected:
  ~Port() = default;
};

// ── the cycle's numbers ─────────────────────────────────────────────────────
static const uint32_t kWakeWaitUs     = 1000;    // after a wake-up or soft reset: tPU and tSR are 240 us at most (Table 5)
static const uint32_t kMeasureWaitUs  = 15000;   // normal mode: tMEAS is 12.1 ms at most (Table 5)
static const uint32_t kReadRetryUs    = 5000;
static const uint8_t  kReadTries      = 4;       // 15 ms + 3 x 5 ms, then the measurement counts as lost
static const uint8_t  kFailsToReset   = 3;       // every third failed cycle in a row starts with a soft reset (Table 12)
static const uint8_t  kProbesToAbsent = 3;       // a sensor not found in three tries is absent
static const uint32_t kReprobeMs      = 60000;   // it is looked for again once a minute; so is a held bus
static const uint32_t kRetryMs        = 2000;    // the first failed cycles are retried this soon
// Choices, not measurements. A healthy transaction at 100 kHz is well under a
// millisecond; the driver's ceiling is a second. A line low for 5 ms is not
// another module's transaction of a few bytes.
static const uint32_t kLineRecheckUs  = 5000;
static const uint32_t kStallUs        = 100000;
static const uint32_t kStallBackoffMs = 60000;
static const int      kWireTimeout    = 5;       // Wire.endTransmission() for ESP_ERR_TIMEOUT (Wire.cpp:468)

struct Counters {
  uint32_t reads = 0;
  uint32_t crcErrors = 0;
  uint32_t i2cErrors = 0;    // a found sensor did not answer
  uint32_t softResets = 0;
  uint32_t busStuck = 0;     // cycles skipped: SDA or SCL low twice, kLineRecheckUs apart
  uint32_t busStalls = 0;    // transactions slower than kStallUs, or reported as a timeout
};

enum class Xfer : uint8_t { Ok, Fail, Stall };

inline Xfer classifyWrite(int code, uint32_t tookUs) {
  if (code == kWireTimeout || tookUs > kStallUs) return Xfer::Stall;
  return code == 0 ? Xfer::Ok : Xfer::Fail;
}

inline Xfer classifyRead(bool complete, uint32_t tookUs) {
  if (tookUs > kStallUs) return Xfer::Stall;
  return complete ? Xfer::Ok : Xfer::Fail;
}

class Reader {
 public:
  explicit Reader(Port &port) : port_(port) {}

  // One loop() pass: at most one I2C transaction.
  void loop(uint16_t intervalS) {
    switch (step_) {
      case Step::Off:
        step_ = Step::Due;
        dueMs_ = port_.ms();
        return;

      case Step::Due:
        if (paused_) {
          if ((int32_t)(port_.ms() - pauseUntilMs_) < 0) return;
          paused_ = false;
        }
        if ((int32_t)(port_.ms() - dueMs_) < 0) return;
        if (!port_.linesHigh()) {                  // another module's transaction, or a held line: look again
          step_ = Step::LineCheck;
          waitUs(kLineRecheckUs);
          return;
        }
        wake(intervalS);
        return;

      case Step::LineCheck:
        if (!waited()) return;
        if (!port_.linesHigh()) {                  // still low: any transaction would stall for a second
          counters_.busStuck++;
          backoff_ = true;
          step_ = Step::Due;
          dueMs_ = port_.ms() + kReprobeMs;
          return;
        }
        wake(intervalS);
        return;

      case Step::Awake:
        if (!waited()) return;
        if (reset_) {                              // section 5.7: from idle, not during a measurement
          switch (send(shtc3::kSoftReset)) {
            case Xfer::Stall: stalled(); return;
            case Xfer::Fail: failed(true, true, intervalS); return;
            case Xfer::Ok: break;
          }
          reset_ = false;
          counters_.softResets++;
          waitUs(kWakeWaitUs);                     // tSR; the ID goes out on the next pass
          return;
        }
        if (!found_) {                             // section 5.9: presence and proper communication
          switch (send(shtc3::kReadId)) {
            case Xfer::Stall: stalled(); return;
            case Xfer::Fail: failed(true, true, intervalS); return;
            case Xfer::Ok: break;
          }
          step_ = Step::IdRead;
          return;
        }
        switch (send(shtc3::kMeasureNormal)) {
          case Xfer::Stall: stalled(); return;
          case Xfer::Fail: failed(true, true, intervalS); return;
          case Xfer::Ok: break;
        }
        step_ = Step::Measuring;
        tries_ = 0;
        waitUs(kMeasureWaitUs);
        return;

      case Step::IdRead: {
        uint8_t id[3];
        switch (receive(id, sizeof(id))) {
          case Xfer::Stall: stalled(); return;
          case Xfer::Fail: failed(true, true, intervalS); return;
          case Xfer::Ok: break;
        }
        if (!shtc3::wordValid(id)) {
          counters_.crcErrors++;
          failed(false, true, intervalS);
          return;
        }
        id_ = shtc3::wordValue(id);
        idRead_ = true;
        if (!shtc3::isShtc3Id(id_)) {              // something else answers at 0x70: a look once a minute, nothing more
          foreign_ = true;
          probes_ = kProbesToAbsent;
          step_ = Step::Due;
          dueMs_ = port_.ms() + kReprobeMs;
          return;
        }
        found_ = true;
        foreign_ = false;
        probes_ = 0;
        if (!ever_) {
          ever_ = true;
          foundMs_ = port_.ms();
        }
        step_ = Step::Awake;                       // the measurement goes out on the next pass
        return;
      }

      case Step::Measuring: {
        if (!waited()) return;
        uint8_t b[6];
        switch (receive(b, sizeof(b))) {
          case Xfer::Stall: stalled(); return;
          case Xfer::Fail:                         // section 5.5: refused while it still measures
            if (++tries_ < kReadTries) {
              waitUs(kReadRetryUs);
              return;
            }
            failed(true, true, intervalS);
            return;
          case Xfer::Ok: break;
        }
        uint16_t st = 0, srh = 0;
        if (shtc3::decodeTemperatureFirst(b, &st, &srh) != shtc3::Frame::Ok) {
          counters_.crcErrors++;
          failed(false, true, intervalS);
          return;
        }
        accept(st, srh, intervalS);
        step_ = Step::Sleep;                       // section 5.4's fourth command, on the next pass
        return;
      }

      case Step::Sleep:
        if (send(shtc3::kSleep) == Xfer::Stall) {
          stalled();
          return;
        }
        step_ = Step::Due;                         // accept() or failed() has set when
        return;
    }
  }

  // Switched off: a sensor left awake mid-cycle is sent to sleep, when the bus
  // is free, and the rest is forgotten. The counters stay.
  void stop() {
    const bool awake = step_ == Step::Awake || step_ == Step::IdRead || step_ == Step::Measuring || step_ == Step::Sleep;
    if (awake && port_.linesHigh()) send(shtc3::kSleep);
    step_ = Step::Off;
    found_ = ever_ = foreign_ = reset_ = have_ = fresh_ = backoff_ = false;
    fails_ = probes_ = 0;
    smooth_.reset();
  }

  // No cycle starts for that long; 0 resumes.
  void pause(uint32_t seconds) {
    paused_ = seconds > 0;
    pauseUntilMs_ = port_.ms() + seconds * 1000UL;
  }

  // A shorter interval takes effect now rather than after the wait already set;
  // a back-off after a held or stalled bus is kept.
  void settingsChanged(uint16_t intervalS) {
    if (step_ != Step::Due || !ever_ || backoff_) return;
    const uint32_t next = port_.ms() + intervalMs(intervalS);
    if ((int32_t)(dueMs_ - next) > 0) dueMs_ = next;
  }

  // While running; climate.cpp reports Off and a bus that never began itself.
  ClimateState state(uint32_t nowMs, uint16_t intervalS) const {
    if (have_) return isStale(nowMs, lastOkMs_, intervalS) ? ClimateState::Stale : ClimateState::Ok;
    // Found, but no good reading since: stale once that has lasted as long as a reading may.
    if (ever_) return isStale(nowMs, foundMs_, intervalS) ? ClimateState::Stale : ClimateState::Probing;
    return (foreign_ || probes_ >= kProbesToAbsent) ? ClimateState::Absent : ClimateState::Probing;
  }

  bool running() const { return step_ != Step::Off; }
  bool have() const { return have_; }
  float temperature() const { return smooth_.t; }   // smoothed, °C
  float humidity() const { return smooth_.rh; }     // smoothed, %RH
  uint32_t lastOkMs() const { return lastOkMs_; }
  bool takeFresh() {
    const bool f = fresh_;
    fresh_ = false;
    return f;
  }
  bool ever() const { return ever_; }
  bool foreign() const { return foreign_; }
  bool idRead() const { return idRead_; }
  uint16_t id() const { return id_; }
  bool paused() const { return paused_; }
  uint32_t pauseUntilMs() const { return pauseUntilMs_; }
  const Counters &counters() const { return counters_; }

 private:
  enum class Step : uint8_t { Off, Due, LineCheck, Awake, IdRead, Measuring, Sleep };

  static uint32_t intervalMs(uint16_t s) { return (uint32_t)clampInterval(s) * 1000UL; }

  Xfer send(uint16_t cmd) {
    const uint32_t t0 = port_.us();
    const int code = port_.write(cmd);
    return classifyWrite(code, port_.us() - t0);
  }

  Xfer receive(uint8_t *buf, size_t n) {
    const uint32_t t0 = port_.us();
    const size_t got = port_.read(buf, n);
    return classifyRead(got == n, port_.us() - t0);
  }

  void waitUs(uint32_t us) {
    stepUs_ = port_.us();
    waitUs_ = us;
  }

  bool waited() { return (uint32_t)(port_.us() - stepUs_) >= waitUs_; }

  void wake(uint16_t intervalS) {
    backoff_ = false;
    switch (send(shtc3::kWakeup)) {
      case Xfer::Stall: stalled(); return;
      case Xfer::Fail: failed(true, false, intervalS); return;
      case Xfer::Ok: break;
    }
    step_ = Step::Awake;
    waitUs(kWakeWaitUs);
  }

  // A transaction that ran into the driver's ceiling, or past kStallUs: nothing
  // more goes on the bus for kStallBackoffMs, and the ID is read again after.
  void stalled() {
    counters_.busStalls++;
    found_ = false;
    backoff_ = true;
    step_ = Step::Due;
    dueMs_ = port_.ms() + kStallBackoffMs;
  }

  // A cycle that ended without a reading. `awake`: the sensor was woken, and the
  // sleep command goes out on the next pass, so a failure does not leave it
  // idling at 45 uA (Table 3).
  void failed(bool noAnswer, bool awake, uint16_t intervalS) {
    // A look for a sensor never found is not an error: counting it would grow
    // i2cErrors by one a minute on a board without the part.
    if (noAnswer && ever_) counters_.i2cErrors++;
    const uint32_t now = port_.ms();
    step_ = awake ? Step::Sleep : Step::Due;
    if (!ever_) {
      if (probes_ < 255) probes_++;
      dueMs_ = now + (probes_ >= kProbesToAbsent ? kReprobeMs : kRetryMs);
      return;
    }
    if (fails_ < 255) fails_++;
    if (fails_ % kFailsToReset == 0) {
      reset_ = true;
      found_ = false;                              // and read the ID again after it
    }
    dueMs_ = now + (fails_ < kFailsToReset ? kRetryMs : intervalMs(intervalS));
  }

  void accept(uint16_t st, uint16_t srh, uint16_t intervalS) {
    const uint32_t now = port_.ms();
    const float t = shtc3::centiDegrees(st) / 100.0f;
    const float rh = shtc3::centiPercent(srh) / 100.0f;
    // After a stale spell the old average must not drag the new value.
    if (have_ && isStale(now, lastOkMs_, intervalS)) smooth_.reset();
    smooth_.add(t, rh, have_ ? (now - lastOkMs_) / 1000.0f : 0.0f);
    have_ = true;
    fresh_ = true;
    lastOkMs_ = now;
    counters_.reads++;
    fails_ = 0;
    dueMs_ = now + intervalMs(intervalS);
  }

  Port &port_;
  Step step_ = Step::Off;
  bool found_ = false;     // the ID register matched since the last soft reset or stall
  bool ever_ = false;      // found at least once since it was switched on
  bool foreign_ = false;   // something at 0x70 answered with another ID
  bool reset_ = false;     // the next cycle starts with a soft reset
  bool have_ = false;      // a good reading exists
  bool fresh_ = false;     // a reading nobody has taken yet (the MQTT side)
  bool backoff_ = false;   // waiting out a held or stalled bus
  bool idRead_ = false;    // the ID register was read with a good CRC
  bool paused_ = false;
  uint16_t id_ = 0;
  uint8_t tries_ = 0, fails_ = 0, probes_ = 0;
  uint32_t stepUs_ = 0, waitUs_ = 0;
  uint32_t dueMs_ = 0, lastOkMs_ = 0, foundMs_ = 0, pauseUntilMs_ = 0;
  Smoother smooth_;
  Counters counters_;
};

}  // namespace climate

#pragma once
// The onboard climate reading after the sensor has answered: smoothing, the
// user's offsets, the humidity at the corrected temperature, staleness, and
// the bounds of the settings. Plain C++, no Arduino: tested on the host by
// tools/climate/check_climate.py.

#include <cmath>
#include <cstdint>

// What the reader knows of the sensor (climate.cpp, climateGet()).
enum class ClimateState : uint8_t {
  Off,       // switched off in the portal
  Probing,   // looking for the sensor, no reading yet
  Ok,        // a reading younger than climate::staleAfterMs()
  Stale,     // the last good reading is older than that, or a found sensor has given none for as long
  Absent,    // nothing answered (or something that is not an SHTC3 did)
};

namespace climate {

// ── settings' bounds ────────────────────────────────────────────────────────
// Design choices, not measurements. How warm the board runs is unknown until it
// is measured on the panel (docs/21-onboard-climate-sensor.md in the knowledge
// base), so the offset ranges are wide and symmetric and the default is 0.
static const uint16_t kIntervalMinS     = 5;
static const uint16_t kIntervalMaxS     = 300;
static const uint16_t kIntervalDefaultS = 10;    // the sensor's own response time is 5-30 s (datasheet Table 2)
static const int16_t  kOffsetMinTenths  = -200;  // -20.0 °C or %RH
static const int16_t  kOffsetMaxTenths  = 200;   // +20.0
// What the weather screen draws of the indoor reading: 0 nothing, 1 design B,
// the "outside | inside" split the owner chose on 2026-09-15 19:10
// (tools/climate/preview/b_split_*). The split is the default.
static const uint8_t  kShowOff   = 0;
static const uint8_t  kShowSplit = 1;
static const uint8_t  kShowCount = 2;

inline uint16_t clampInterval(long s) {
  return s < kIntervalMinS ? kIntervalMinS : s > kIntervalMaxS ? kIntervalMaxS : (uint16_t)s;
}
inline int16_t clampOffset(long tenths) {
  return tenths < kOffsetMinTenths ? kOffsetMinTenths : tenths > kOffsetMaxTenths ? kOffsetMaxTenths : (int16_t)tenths;
}
inline uint8_t clampShow(long v) { return (v >= 0 && v < kShowCount) ? (uint8_t)v : 0; }

// What the weather screen shows of the reading: the split with values while one
// is fresh, the split with dashes once it is stale, and today's screen when the
// sensor is switched off, absent or still being looked for, or the portal's
// "On the weather screen" is off.
enum class WeatherIndoor : uint8_t { None, Live, Stale };

inline WeatherIndoor weatherIndoor(bool enabled, uint8_t show, ClimateState state) {
  if (!enabled || show != kShowSplit) return WeatherIndoor::None;
  if (state == ClimateState::Ok) return WeatherIndoor::Live;
  if (state == ClimateState::Stale) return WeatherIndoor::Stale;
  return WeatherIndoor::None;
}

// ── humidity at another temperature ─────────────────────────────────────────
// Sensirion, "Introduction to Humidity", Version 2.0, August 2009: the Magnus
// formula, eq. (3), with Table 1's parameters above water (valid -45..60 °C),
// in hPa.
inline double saturationHpa(double t) { return 6.112 * std::exp(17.62 * t / (243.12 + t)); }

// The same note, eq. (7): RH = e / e_w(t). Air warmed by the board gains heat,
// not water, so its vapour pressure e is the room's; only e_w changes. The RH
// the room has at tAir is the sensor's RH scaled by e_w(tSensor) / e_w(tAir).
// Sensirion's design guide (Version 2, March 2024, section 3.1) asks for this:
// correcting the temperature for self-heating "also requires a compensation of
// the RH signal".
inline double humidityAt(double rhSensor, double tSensor, double tAir) {
  const double rh = rhSensor * saturationHpa(tSensor) / saturationHpa(tAir);
  return rh < 0.0 ? 0.0 : rh > 100.0 ? 100.0 : rh;
}

struct Reading {
  float tempC;
  float humidity;
};

// The reading the panel reports: the temperature offset first, then, when
// `follow` is set, the humidity at that temperature, then the humidity offset.
// Offsets are tenths of a °C and of a %RH.
inline Reading correct(float tSensor, float rhSensor, int16_t tOffsetTenths, int16_t hOffsetTenths, bool follow) {
  Reading r;
  r.tempC = tSensor + tOffsetTenths / 10.0f;
  double rh = rhSensor;
  if (follow && tOffsetTenths != 0) rh = humidityAt(rhSensor, tSensor, r.tempC);
  rh += hOffsetTenths / 10.0;
  r.humidity = (float)(rh < 0.0 ? 0.0 : rh > 100.0 ? 100.0 : rh);
  return r;
}

// ── smoothing ───────────────────────────────────────────────────────────────
// First-order smoothing with a time constant, so a changed interval keeps the
// same response. It is applied to the sensor's values, before correct(): a new
// offset shows at once instead of creeping in. 60 s is a choice for a steady
// last digit on the panel, not a sourced figure; the sensor's repeatability is
// 0.1 °C and 0.1 %RH (datasheet Tables 1 and 2).
static const float kSmoothTauS = 60.0f;

struct Smoother {
  bool  have = false;
  float t = 0.0f, rh = 0.0f;

  void reset() { have = false; }

  void add(float tSensor, float rhSensor, float dtS, float tauS = kSmoothTauS) {
    if (!have) {
      t = tSensor;
      rh = rhSensor;
      have = true;
      return;
    }
    const float a = dtS <= 0.0f ? 1.0f : dtS / (tauS + dtS);
    t  += a * (tSensor - t);
    rh += a * (rhSensor - rh);
  }
};

// ── staleness ───────────────────────────────────────────────────────────────
// Stale once the last good reading is older than three intervals, and never
// sooner than 30 s. A choice: one failed cycle is a hiccup, three are a fault.
inline uint32_t staleAfterMs(uint16_t intervalS) {
  const uint32_t ms = 3UL * intervalS * 1000UL;
  return ms < 30000UL ? 30000UL : ms;
}
inline bool isStale(uint32_t nowMs, uint32_t lastOkMs, uint16_t intervalS) {
  return (uint32_t)(nowMs - lastOkMs) > staleAfterMs(intervalS);
}

}  // namespace climate

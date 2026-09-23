/*
 * AnimatedPixelClock - Weather Clock (style 14)
 *
 * Time on top, animated condition icon + big temperature in the middle,
 * details row (min/max + humidity alternating with sunrise/sunset) at the
 * bottom. With the board's own sensor, the indoor reading in a column beside
 * the temperature: design B, "outside | inside", chosen 2026-09-15.
 *
 * This file gathers what the screen shows; src/clocks/weather_layout.h draws
 * it, on the panel and in the host check that holds it to the previews
 * (tools/climate/check_weather_screen.py).
 */

#include "clocks.h"
#include "clock_globals.h"
#include "../display/display.h"
#include "../weather/weather.h"
#include "weather_layout.h"
#if defined(CLIMATE_ENABLED)
#include "../climate/climate.h"
#endif

static_assert(SCREEN_WIDTH == kWeatherScreenW, "weather_layout.h centres its rows on a 128 px panel");

void displayClockWithWeather() {
  WeatherScreen s{};

  struct tm timeinfo;
  s.haveTime = getTimeWithTimeout(&timeinfo);
  if (s.haveTime) {
    int displayHour, displayMin;
    bool isPM;
    formatTimeForDisplay(timeinfo.tm_hour, timeinfo.tm_min, displayHour,
                         displayMin, isPM);
    s.hour = (uint8_t)displayHour;
    s.minute = (uint8_t)displayMin;
    s.pm = isPM;
    s.colon = shouldShowColon();
  }
  s.ntpSynced = ntpSynced;
  s.h12 = !settings.use24Hour;
  s.wifi = wifiConnected;
  s.digitColor = digitColor();
  s.iconColor = SPRITE_COLOR(COL_WEATHER_ICON);
  s.accentColor = SPRITE_COLOR(COL_WEATHER_ACCENT);
  s.tempColor = SPRITE_COLOR(COL_WEATHER_TEMP);
  s.fahrenheit = settings.weatherUseFahrenheit;

  weatherNoteShown();   // fetches run only while this page is drawn
  const WeatherData wx = getWeather();
  if (!settings.weatherEnabled || !weatherConfigured()) {
    s.state = WeatherState::NotSetUp;
  } else if (!wx.valid) {
    s.state = WeatherState::Fetching;
  } else {
    s.state = WeatherState::Ready;
    s.icon = weatherIconFromCode(wx.weatherCode);
    s.tempC = wx.tempC;
    s.tempMaxC = wx.tempMaxC;
    s.tempMinC = wx.tempMinC;
    s.humidity = wx.humidity;
    memcpy(s.sunrise, wx.sunrise, sizeof(s.sunrise));
    memcpy(s.sunset, wx.sunset, sizeof(s.sunset));
  }

#if defined(CLIMATE_ENABLED)
  // The snapshot the reader keeps from loop(): no I2C on a frame. The reader
  // only runs while a screen shows the figure (src/climate/climate.cpp).
  if (settings.climateEnabled && settings.climateShow == climate::kShowSplit) climateNoteShown();
  const ClimateReading in = climateGet();
  s.indoor = climate::weatherIndoor(settings.climateEnabled, settings.climateShow, in.state);
  s.inTempC = in.tempC;
  s.inHumidity = in.humidity;
#endif

  s.nowMs = millis();
  drawWeatherScreen(display, s);
}

#pragma once
// The weather clock (style 14), drawn on any Adafruit GFX target.
//
// clock_weather.cpp gathers what the screen shows into a WeatherScreen and
// passes the panel's `display`. tools/climate/weather_screen_host.cpp passes a
// host canvas on the same Adafruit GFX library, and
// tools/climate/check_weather_screen.py holds its pixels against the previews
// tools/climate/render.py draws. render.py reads the W* defines below.
//
// A template rather than a function on Adafruit_GFX&: every call resolves on
// the concrete class, as `display.fillRect(...)` did before this file, so the
// panel keeps the DMA library's own overrides.
//
// No Arduino, no heap, no I/O. The indoor column is the design the owner chose
// on 2026-09-15 19:10, B "outside | inside" (tools/climate/preview/b_split_*).

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../climate/climate_model.h"
#include "../weather/weather_icons.h"
#include "wifi_icon.h"

// ── today's screen ──────────────────────────────────────────────────────────
#define WTIME_Y 2
#define WICON_X 10
#define WICON_Y 24
#define WICON_SIZE 24
#define WTEMP_X 52          // the big temperature, at WICON_Y + WTEMP_DY
#define WTEMP_DY 3
#define WMERIDIEM_X 112     // AM / PM, at WTIME_Y + WMERIDIEM_DY
#define WMERIDIEM_DY 4
#define WDETAIL_Y 55
#define WDETAIL_SWAP_MS 5000

static const int16_t kWeatherScreenW = 128;
// Arduino's PI, the same literal: the sun's rays come out on the same pixels.
static const double kWeatherPi = 3.1415926535897932384626433832795;

// ── B: outside | inside. render.py's CL_* block, name for name ──────────────
static const int16_t CL_B_ICON_X = 1;       // 0 would put the fog's drifting lines off the panel
static const int16_t CL_B_TEMP_X = 28;
static const int16_t CL_B_RULE_X = 89;
static const int16_t CL_B_RULE_Y0 = 26;     // the span of the big digits and their degree mark
static const int16_t CL_B_RULE_Y1 = 47;
static const int16_t CL_B_COL_X = 92;       // the indoor column, x 92-127
static const int16_t CL_B_GLYPH_Y = 28;
static const int16_t CL_B_IN_T_Y = 28;      // 5x7, beside the house
static const int16_t CL_B_IN_H_Y = 39;      // 5x7, under the temperature
static const int16_t CL_B_IN_TEXT_X = CL_B_COL_X + 9;

// Colours before color565: the house colours of the rail board and the market pages.
static const uint8_t CL_RULE[3] = {52, 60, 64};
static const uint8_t CL_IN_GLYPH[3] = {255, 150, 0};     // amber, the colour of a heading
static const uint8_t CL_IN_TEMP[3] = {255, 255, 255};    // white, like the outdoor details row
static const uint8_t CL_IN_HUM[3] = {110, 122, 128};     // dim, the least important number on the screen
static const uint8_t CL_STALE[3] = {110, 122, 128};      // a stale reading: glyph and dashes, all dim

static const char *const CL_HOUSE7[7] = {
    "...#...",
    "..###..",
    ".#####.",
    "#######",
    ".#####.",
    ".##.##.",
    ".##.##.",
};
static const char *const IN_DASH_T = "--.-";
static const char *const IN_DASH_H = "--%";

// MatrixPanel_I2S_DMA::color565, the same arithmetic.
inline uint16_t weatherColor565(const uint8_t c[3]) {
  return (uint16_t)(((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3));
}

// ── what the screen shows ───────────────────────────────────────────────────
enum class WeatherState : uint8_t { NotSetUp, Fetching, Ready };

struct WeatherScreen {
  bool     haveTime;
  bool     ntpSynced;
  uint8_t  hour, minute;          // as formatTimeForDisplay() gives them
  bool     colon;                 // shouldShowColon()
  bool     h12;                   // !settings.use24Hour: AM / PM beside the time
  bool     pm;
  bool     wifi;                  // false draws the no-WiFi icon
  uint32_t nowMs;                 // the icons' animation and the details row's swap
  uint16_t digitColor, iconColor, accentColor, tempColor;
  WeatherState state;
  WeatherIconKind icon;
  float    tempC, tempMaxC, tempMinC;
  int      humidity;
  char     sunrise[6], sunset[6];
  bool     fahrenheit;
  climate::WeatherIndoor indoor;  // None: today's screen
  float    inTempC, inHumidity;   // what the panel reports; read only when indoor is Live
};

// What was drawn, for the host check. The panel passes nothing.
struct WeatherScreenNotes {
  char outdoor[8];
  char details[30];
  char inTemp[12];
  char inHum[8];
  bool split;
  bool unitLetter;
};

// The big number: whole degrees in the weather's unit.
inline void weatherOutdoorString(const WeatherScreen &s, char *out, size_t n) {
  const float t = s.fahrenheit ? s.tempC * 9.0f / 5.0f + 32.0f : s.tempC;
  snprintf(out, n, "%d", (int)roundf(t));
}

// The indoor temperature: one decimal while it fits four characters, whole
// degrees beyond (-10 and below, 100 and above); humidity in whole percent (the
// sensor is +-2 %RH typical, datasheet Table 1). Dashes when stale.
inline void weatherIndoorStrings(const WeatherScreen &s, char *t, size_t tn, char *h, size_t hn) {
  if (s.indoor != climate::WeatherIndoor::Live) {
    snprintf(t, tn, "%s", IN_DASH_T);
    snprintf(h, hn, "%s", IN_DASH_H);
    return;
  }
  const float v = s.fahrenheit ? s.inTempC * 9.0f / 5.0f + 32.0f : s.inTempC;
  if (v > -9.95f && v < 99.95f)
    snprintf(t, tn, "%.1f", v);
  else
    snprintf(t, tn, "%d", (int)roundf(v));
  snprintf(h, hn, "%d%%", (int)lroundf(s.inHumidity));
}

// ── drawing ─────────────────────────────────────────────────────────────────
template <class G>
void weatherCloud(G &g, int cx, int cy, uint16_t color) {
  // Puffy cloud built from three discs on a base bar, ~20px wide.
  g.fillCircle(cx - 6, cy + 2, 4, color);
  g.fillCircle(cx, cy - 1, 5, color);
  g.fillCircle(cx + 6, cy + 2, 4, color);
  g.fillRect(cx - 6, cy + 2, 13, 4, color);
}

// Icon animation phases run off the clock, so there is no state to reset
// between style switches.
template <class G>
void weatherIcon(G &g, int x, int y, WeatherIconKind kind, uint32_t now, uint16_t body, uint16_t accent) {
  int cx = x + WICON_SIZE / 2;
  int cy = y + WICON_SIZE / 2;

  switch (kind) {
    case WICON_SUN: {
      g.fillCircle(cx, cy, 6, body);
      // Eight rays; every other ray pulses longer on a slow beat.
      int pulse = (now / 400) % 2;
      for (int i = 0; i < 8; i++) {
        float a = i * (kWeatherPi / 4.0f);
        int len = 10 + ((i % 2 == pulse) ? 1 : -1);
        int x0 = cx + (int)(cosf(a) * 8);
        int y0 = cy + (int)(sinf(a) * 8);
        int x1 = cx + (int)(cosf(a) * len);
        int y1 = cy + (int)(sinf(a) * len);
        g.drawLine(x0, y0, x1, y1, body);
      }
      break;
    }
    case WICON_PARTCLOUD: {
      g.fillCircle(cx - 4, cy - 4, 5, body);
      for (int i = 0; i < 4; i++) {
        float a = i * (kWeatherPi / 2.0f) - kWeatherPi / 4.0f;
        g.drawLine(cx - 4 + (int)(cosf(a) * 6), cy - 4 + (int)(sinf(a) * 6),
                   cx - 4 + (int)(cosf(a) * 9), cy - 4 + (int)(sinf(a) * 9),
                   body);
      }
      weatherCloud(g, cx + 3, cy + 4, accent);
      break;
    }
    case WICON_CLOUD: {
      // Slow 1px horizontal bob.
      int bob = ((now / 700) % 2) ? 1 : 0;
      weatherCloud(g, cx + bob, cy - 1, body);
      break;
    }
    case WICON_FOG: {
      // Four haze lines drifting in alternating directions.
      for (int i = 0; i < 4; i++) {
        int yy = y + 5 + i * 5;
        int shift = (int)((now / 150 + i * 4) % 8) - 4;
        if (i % 2) shift = -shift;
        g.drawFastHLine(x + 3 + shift, yy, 16, i % 2 ? accent : body);
      }
      break;
    }
    case WICON_RAIN: {
      weatherCloud(g, cx, cy - 5, body);
      // Three drop columns falling below the cloud.
      for (int i = 0; i < 3; i++) {
        int dropY = (int)((now / 60 + i * 5) % 12);
        g.drawFastVLine(cx - 6 + i * 6, cy + 2 + dropY, 3, accent);
      }
      break;
    }
    case WICON_SNOW: {
      weatherCloud(g, cx, cy - 5, body);
      // Drifting flakes with a slight side sway.
      for (int i = 0; i < 3; i++) {
        int fy = (int)((now / 120 + i * 6) % 12);
        int fx = cx - 6 + i * 6 + (((now / 240 + i) % 2) ? 1 : -1);
        g.drawPixel(fx, cy + 2 + fy, accent);
        g.drawPixel(fx, cy + 3 + fy, accent);
      }
      break;
    }
    case WICON_STORM: {
      weatherCloud(g, cx, cy - 5, body);
      for (int i = 0; i < 2; i++) {
        int dropY = (int)((now / 60 + i * 7) % 10);
        g.drawFastVLine(cx - 7 + i * 13, cy + 2 + dropY, 3, accent);
      }
      // Lightning bolt flashes ~300ms out of every 1.6s.
      if ((now % 1600) < 300) {
        g.drawLine(cx + 1, cy + 1, cx - 2, cy + 6, DISPLAY_WHITE);
        g.drawLine(cx - 2, cy + 6, cx + 2, cy + 6, DISPLAY_WHITE);
        g.drawLine(cx + 2, cy + 6, cx - 1, cy + 12, DISPLAY_WHITE);
      }
      break;
    }
  }
}

// Big temperature: size-3 digits plus a small degree circle and unit letter.
template <class G>
void weatherTemperature(G &g, int x, int y, const WeatherScreen &s, const char *digits, bool unitLetter) {
  g.setTextSize(3);
  g.setTextColor(s.tempColor);
  g.setCursor(x, y);
  g.print(digits);

  int endX = x + strlen(digits) * 18;
  g.drawCircle(endX + 2, y + 1, 2, s.tempColor);
  g.setTextSize(1);
  if (unitLetter) {
    g.setCursor(endX + 7, y);
    g.print(s.fahrenheit ? "F" : "C");
  }
  g.setTextColor(DISPLAY_WHITE);
}

// B's indoor column: a dim rule, the house, the temperature with a thin dot and
// a 3x3 degree mark, the humidity under it.
template <class G>
void weatherIndoorColumn(G &g, const WeatherScreen &s, char *t, size_t tn, char *h, size_t hn) {
  weatherIndoorStrings(s, t, tn, h, hn);
  const bool live = s.indoor == climate::WeatherIndoor::Live;
  const uint16_t glyph = weatherColor565(live ? CL_IN_GLYPH : CL_STALE);
  const uint16_t temp = weatherColor565(live ? CL_IN_TEMP : CL_STALE);
  const uint16_t hum = weatherColor565(live ? CL_IN_HUM : CL_STALE);

  g.drawFastVLine(CL_B_RULE_X, CL_B_RULE_Y0, CL_B_RULE_Y1 - CL_B_RULE_Y0 + 1, weatherColor565(CL_RULE));

  for (int j = 0; j < 7; j++)
    for (int i = 0; CL_HOUSE7[j][i]; i++)
      if (CL_HOUSE7[j][i] == '#') g.drawPixel(CL_B_COL_X + i, CL_B_GLYPH_Y + j, glyph);

  // A 5x7 number whose dot is drawn 2 columns left and advances 3, as the
  // market pages' numbers do, so 23.4 reads as one number.
  int x = CL_B_IN_TEXT_X;
  for (const char *p = t; *p; p++) {
    if (*p == '.') {
      g.drawChar(x - 2, CL_B_IN_T_Y, (unsigned char)*p, temp, temp, 1);
      x += 3;
    } else {
      g.drawChar(x, CL_B_IN_T_Y, (unsigned char)*p, temp, temp, 1);
      x += 6;
    }
  }
  x -= 1;                                                   // the column after the number's ink
  g.drawCircle(x + 2, CL_B_IN_T_Y + 1, 1, temp);            // the degree mark, x+1..x+3

  g.setTextColor(hum);
  g.setCursor(CL_B_IN_TEXT_X, CL_B_IN_H_Y);
  g.print(h);
  g.setTextColor(DISPLAY_WHITE);                            // as today's screen leaves it
}

template <class G>
void drawWeatherScreen(G &g, const WeatherScreen &s, WeatherScreenNotes *notes = nullptr) {
  if (notes) memset(notes, 0, sizeof(*notes));

  // --- Time row (size 2, centered) ---
  if (s.haveTime) {
    char timeStr[9];
    snprintf(timeStr, sizeof(timeStr), "%02d%c%02d", s.hour, s.colon ? ':' : ' ', s.minute);

    g.setTextSize(2);
    g.setCursor((kWeatherScreenW - 5 * 12) / 2, WTIME_Y);
    g.setTextColor(s.digitColor);
    g.print(timeStr);
    g.setTextColor(DISPLAY_WHITE);
    if (s.h12) {
      g.setTextSize(1);
      g.setCursor(WMERIDIEM_X, WTIME_Y + WMERIDIEM_DY);
      g.print(s.pm ? "PM" : "AM");
    }
  } else {
    g.setTextSize(1);
    g.setCursor(20, WTIME_Y + 4);
    g.print(!s.ntpSynced ? "Syncing time..." : "Time Error");
  }

  // --- Weather block ---
  g.setTextSize(1);

  if (s.state == WeatherState::NotSetUp) {
    g.setCursor(22, 34);
    g.print("Weather not set up");
    g.setCursor(13, 46);
    g.print("Enable it in the web UI");
    return;
  }
  if (s.state == WeatherState::Fetching) {
    g.setCursor(28, 38);
    g.print("Fetching weather...");
    return;
  }

  const bool split = s.indoor != climate::WeatherIndoor::None;
  char digits[8];
  weatherOutdoorString(s, digits, sizeof(digits));
  // In B a three-character temperature (-10 °C and below, 100 °F and above)
  // would run its unit letter into the rule: the degree mark stays, the letter goes.
  const bool unitLetter = !(split && strlen(digits) >= 3);

  weatherIcon(g, split ? CL_B_ICON_X : WICON_X, WICON_Y, s.icon, s.nowMs, s.iconColor, s.accentColor);
  weatherTemperature(g, split ? CL_B_TEMP_X : WTEMP_X, WICON_Y + WTEMP_DY, s, digits, unitLetter);

  // --- Details row: min/max + humidity alternating with sun times ---
  char line[30];
  if ((s.nowMs / WDETAIL_SWAP_MS) % 2 == 0) {
    float lo = s.tempMinC, hi = s.tempMaxC;
    if (s.fahrenheit) {
      lo = lo * 9.0f / 5.0f + 32.0f;
      hi = hi * 9.0f / 5.0f + 32.0f;
    }
    snprintf(line, sizeof(line), "%d\x18 %d\x19  %d%% RH", (int)roundf(hi),
             (int)roundf(lo), s.humidity);
  } else {
    snprintf(line, sizeof(line), "\x18%s  \x19%s", s.sunrise, s.sunset);
  }
  int w = strlen(line) * 6;
  g.setCursor((kWeatherScreenW - w) / 2, WDETAIL_Y);
  g.print(line);

  if (!s.wifi) drawNoWiFiIconOn(g, 0, 0);

  char inT[12] = "", inH[8] = "";
  if (split) weatherIndoorColumn(g, s, inT, sizeof(inT), inH, sizeof(inH));

  if (notes) {
    snprintf(notes->outdoor, sizeof(notes->outdoor), "%s", digits);
    snprintf(notes->details, sizeof(notes->details), "%s", line);
    snprintf(notes->inTemp, sizeof(notes->inTemp), "%s", inT);
    snprintf(notes->inHum, sizeof(notes->inHum), "%s", inH);
    notes->split = split;
    notes->unitLetter = unitLetter;
  }
}

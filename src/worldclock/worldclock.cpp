#include "worldclock.h"

#if defined(WORLDCLOCK_ENABLED)

#include <Arduino.h>
#include <math.h>
#include <time.h>

#include "../display/display.h"
#include "worldmap.h"

// Colours. Deliberately monochrome: the map is the texture and the time is the
// point; only the cities carry colour.
static const uint8_t DAY_R = 225, DAY_G = 228, DAY_B = 232;
static const uint8_t NIG_R =  52, NIG_G =  56, NIG_B =  64;

// Bold 6x11 digits, 2 px strokes, one byte per row with bit 5 leftmost. Solid
// digits against a dotted map is the whole look, and six pixels wide is what
// lets HH:MM fit the 32 px of empty South Pacific at the bottom left.
static const uint8_t kDigits[10][11] = {
  {0x1E, 0x3F, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x1E},  // 0
  {0x0C, 0x1C, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x3F},  // 1
  {0x1E, 0x3F, 0x33, 0x03, 0x07, 0x0E, 0x1C, 0x38, 0x30, 0x3F, 0x3F},  // 2
  {0x1E, 0x3F, 0x33, 0x03, 0x0E, 0x0F, 0x03, 0x03, 0x33, 0x3F, 0x1E},  // 3
  {0x07, 0x0F, 0x1B, 0x33, 0x33, 0x3F, 0x3F, 0x03, 0x03, 0x03, 0x03},  // 4
  {0x3F, 0x3F, 0x30, 0x30, 0x3E, 0x3F, 0x03, 0x03, 0x33, 0x3F, 0x1E},  // 5
  {0x1E, 0x3F, 0x30, 0x30, 0x3E, 0x3F, 0x33, 0x33, 0x33, 0x3F, 0x1E},  // 6
  {0x3F, 0x3F, 0x03, 0x03, 0x06, 0x06, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C},  // 7
  {0x1E, 0x3F, 0x33, 0x33, 0x1E, 0x1E, 0x33, 0x33, 0x33, 0x3F, 0x1E},  // 8
  {0x1E, 0x3F, 0x33, 0x33, 0x33, 0x3F, 0x1F, 0x03, 0x03, 0x3F, 0x1E},  // 9
};
static const int16_t TIME_X = 1, TIME_Y = 50;

// Colour per dot, ready for the panel. The terminator moves a quarter of a
// degree a minute and a dot is 5.6 degrees wide, so this is recomputed when
// the minute changes and not seventy times a second. Storing the final colour
// rather than a brightness keeps the blend identical to the Lua prototype's.
static uint16_t s_colour[WORLD_ROWS][WORLD_COLS];
static int     s_forMinute = -1;

static bool landAt(uint8_t r, uint8_t c) {
  return (kWorldMask[r] >> c) & 1ULL;
}

// Declination by the usual cosine approximation, subsolar longitude straight
// from UTC. Not ephemeris-grade - the equation of time alone moves the line by
// up to four degrees - but one dot spans 5.6, so the error stays inside a dot.
static void recompute(const struct tm &utc, bool synced) {
  const uint16_t night = display.color565(NIG_R, NIG_G, NIG_B);
  if (!synced) {                           // no time yet: all night, honestly
    for (auto &row : s_colour) for (auto &dot : row) dot = night;
    return;
  }
  const float decl   = -23.44f * (float)DEG_TO_RAD *
                       cosf(2.0f * (float)PI * (utc.tm_yday + 10) / 365.0f);
  const float utcMin = (float)(utc.tm_hour * 60 + utc.tm_min);
  const float subLon = -15.0f * (utcMin / 60.0f - 12.0f) * (float)DEG_TO_RAD;
  const float sd = sinf(decl), cd = cosf(decl);
  const float dLat = (WORLD_TOP - WORLD_BOTTOM) / WORLD_ROWS;
  for (uint8_t r = 0; r < WORLD_ROWS; r++) {
    const float lat = (WORLD_TOP - (r + 0.5f) * dLat) * (float)DEG_TO_RAD;
    const float sl = sinf(lat), cl = cosf(lat);
    for (uint8_t c = 0; c < WORLD_COLS; c++) {
      if (!landAt(r, c)) { s_colour[r][c] = 0; continue; }
      const float lon = (-180.0f + (c + 0.5f) * 360.0f / WORLD_COLS) * (float)DEG_TO_RAD;
      const float el  = asinf(sl * sd + cl * cd * cosf(lon - subLon)) * (float)RAD_TO_DEG;
      // 0 at the end of civil twilight (-6 degrees), 1 once the sun is up
      float k = (el + 6.0f) / 6.0f;
      if (k < 0) k = 0;
      if (k > 1) k = 1;
      s_colour[r][c] = display.color565((uint8_t)(NIG_R + (DAY_R - NIG_R) * k),
                                        (uint8_t)(NIG_G + (DAY_G - NIG_G) * k),
                                        (uint8_t)(NIG_B + (DAY_B - NIG_B) * k));
    }
  }
}

static void drawDigit(int16_t x, int16_t y, uint8_t d, uint16_t col) {
  for (int16_t row = 0; row < 11; row++)
    for (int16_t bit = 0; bit < 6; bit++)
      if (kDigits[d][row] & (0x20 >> bit)) display.drawPixel(x + bit, y + row, col);
}

void worldClockRender() {
  const time_t now = time(nullptr);
  // Before NTP the clock reads 1970; say so rather than draw a confident lie.
  const bool synced = now > 1700000000;
  struct tm lt, ut;
  localtime_r(&now, &lt);
  gmtime_r(&now, &ut);

  if (ut.tm_min != s_forMinute || !synced) {
    recompute(ut, synced);
    s_forMinute = synced ? ut.tm_min : -1;
  }

  for (uint8_t r = 0; r < WORLD_ROWS; r++)
    for (uint8_t c = 0; c < WORLD_COLS; c++) {
      if (landAt(r, c)) display.drawPixel(c * 2, r * 2, s_colour[r][c]);
    }

  // Cities are 2x2 - the dot and its gap - so they read as bigger dots on the
  // same grid rather than as something drawn on top of it.
  for (uint8_t i = 0; i < WORLD_CITY_COUNT; i++) {
    const WorldCity &city = kWorldCities[i];
    const int16_t c = (int16_t)((city.lon + 180.0f) / 360.0f * WORLD_COLS);
    const int16_t r = (int16_t)((WORLD_TOP - city.lat) / (WORLD_TOP - WORLD_BOTTOM) * WORLD_ROWS);
    if (c < 0 || c >= WORLD_COLS || r < 0 || r >= WORLD_ROWS) continue;
    float a = 1.0f;
    if (i == 0) a = 0.65f + 0.35f * fabsf(sinf((float)PI * millis() / 1000.0f));  // home breathes
    display.fillRect(c * 2, r * 2, 2, 2,
                     display.color565((uint8_t)(255 * a), (uint8_t)(140 * a), (uint8_t)(40 * a)));
  }

  // HH:MM in the South Pacific. Digits are 6 wide with a 1 px gap; the colon
  // is two 2x2 dots.
  const uint16_t white = display.color565(255, 255, 255);
  int16_t x = TIME_X;
  if (synced) {
    drawDigit(x, TIME_Y, lt.tm_hour / 10, white); x += 7;
    drawDigit(x, TIME_Y, lt.tm_hour % 10, white); x += 7;
  } else {
    display.fillRect(x, TIME_Y + 5, 13, 2, white); x += 14;
  }
  display.fillRect(x, TIME_Y + 3, 2, 2, white);
  display.fillRect(x, TIME_Y + 7, 2, 2, white);
  x += 3;
  if (synced) {
    drawDigit(x, TIME_Y, lt.tm_min / 10, white); x += 7;
    drawDigit(x, TIME_Y, lt.tm_min % 10, white);
  } else {
    display.fillRect(x, TIME_Y + 5, 13, 2, white);
  }
}

#endif  // WORLDCLOCK_ENABLED

#include "worldclock.h"

#if defined(WORLDCLOCK_ENABLED)

#include <math.h>
#include <string.h>

#if defined(WORLDCLOCK_HOST)
#include "wc_host.h"                 // tools/luasim/host: a stand-in display and millis()
#else
#include <Arduino.h>
#include <time.h>
#include "../display/display.h"
#endif
#include "../fonts/picopixel_fb.h"
#include "posix_tz.h"
#include "worldmap.h"

// Colours. Deliberately monochrome: the map is the texture and the time is the
// point; only the cities carry colour, and home's name shares their orange so
// it reads as the label of its dot.
static const uint8_t DAY_R = 225, DAY_G = 228, DAY_B = 232;
static const uint8_t NIG_R =  52, NIG_G =  56, NIG_B =  64;
static const uint8_t CITY_R = 255, CITY_G = 140, CITY_B = 40;

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

// Home's name in Picopixel, bottom-aligned with the digits: a 5 px capital from
// y 56 to 60. On this mask rows 28-30 are open Southern Ocean from Tierra del
// Fuego's dots (x 36-40) to New Zealand's (x 116), so the name starts at 43
// and gets WC_NAME_PX of advance, ending two pixels short of the next dot. No
// backing box: nothing of the map is under it, and the time ends at x 30.
static const int16_t NAME_X = 43, NAME_BASE = 60;

// A change of home is announced, then the name holds still. The time now
// belongs to whichever city is home, so the label has to stay; what does not
// have to stay is movement, on a page that is on the wall all day and already
// has one breathing dot. PULSE_S is half the 20 s the carousel gives this page
// (main.cpp), so a change made while the page is up is seen pulsing and then
// seen settled within one visit; the last FADE_S ease the breathing out, so it
// never stops on a dim frame.
static const float PULSE_S = 10.0f, FADE_S = 2.0f;

// Colour per dot, ready for the panel. The terminator moves a quarter of a
// degree a minute and a dot is 5.6 degrees wide, so this is recomputed when
// the minute changes and not seventy times a second. Storing the final colour
// rather than a brightness keeps the blend identical to the Lua prototype's.
static uint16_t s_colour[WORLD_ROWS][WORLD_COLS];
static const int64_t NEVER = INT64_MIN, NO_TIME = INT64_MIN + 1;
static int64_t  s_forMinute = NEVER;

static WcCity   s_custom[WC_CUSTOM_MAX];
static bool     s_used[WC_CUSTOM_MAX];
static WcCity   s_auto;
static bool     s_haveAuto = false;
static uint8_t  s_home = 0;
static bool     s_chosen = false;
static bool     s_homeReady = false;
static WcCity   s_homeCity;              // a copy: a slot may change under the page
static PosixTz  s_homeTz;
static bool     s_pulseDue = false;      // home changed; the pulse starts with the next frame
static bool     s_pulsing = false;
static uint32_t s_pulseAt = 0;

static bool landAt(uint8_t r, uint8_t c) {
  return (kWorldMask[r] >> c) & 1ULL;
}

static int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
  return q;
}

// Declination by the usual cosine approximation, subsolar longitude straight
// from UTC. Not ephemeris-grade - the equation of time alone moves the line by
// up to four degrees - but one dot spans 5.6, so the error stays inside a dot.
// The date comes from UTC seconds by arithmetic rather than gmtime_r, the way
// the prototype has to do it in Lua.
static void recompute(int64_t utc) {
  const int64_t days = floorDiv(utc, 86400);
  int64_t year;
  unsigned mon, mday;
  posixCivilFromDays(days, &year, &mon, &mday);
  const int yday = (int)(days - posixDaysFromCivil(year, 1, 1));
  const float decl   = -23.44f * (float)DEG_TO_RAD * cosf(2.0f * (float)PI * (yday + 10) / 365.0f);
  const float utcMin = (float)(floorDiv(utc, 60) - days * 1440);
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

static void allNight() {                   // no time yet: all night, honestly
  const uint16_t night = display.color565(NIG_R, NIG_G, NIG_B);
  for (auto &row : s_colour) for (auto &dot : row) dot = night;
}

static void drawDigit(int16_t x, int16_t y, uint8_t d, uint16_t col) {
  for (int16_t row = 0; row < 11; row++)
    for (int16_t bit = 0; bit < 6; bit++)
      if (kDigits[d][row] & (0x20 >> bit)) display.drawPixel(x + bit, y + row, col);
}

static const GFXglyph &glyphFor(char ch) {
  unsigned c = (unsigned char)ch;
  if (c < PicopixelFB.first || c > PicopixelFB.last) c = ' ';
  return PicopixelFB.glyph[c - PicopixelFB.first];
}

// Picopixel read through the GFXfont tables with drawChar's bit order, as
// lua_px.cpp's px.text reads it - which is how the prototype draws the name.
static void drawName(const char *s, uint16_t col) {
  int16_t x = NAME_X;
  for (; *s; s++) {
    const GFXglyph &g = glyphFor(*s);
    int bit = 0, bo = g.bitmapOffset;
    for (int gy = 0; gy < g.height; gy++)
      for (int gx = 0; gx < g.width; gx++) {
        if (!(bit & 7)) bo++;
        if ((PicopixelFB.bitmap[bo - 1] >> (7 - (bit & 7))) & 1)
          display.drawPixel(x + gx + g.xOffset, NAME_BASE + gy + g.yOffset, col);
        bit++;
      }
    x += g.xAdvance;
  }
}

// Cities are 2x2 - the dot and its gap - so they read as bigger dots on the
// same grid rather than as something drawn on top of it.
static void drawCity(float lat, float lon, float a) {
  const int16_t c = (int16_t)((lon + 180.0f) / 360.0f * WORLD_COLS);
  const int16_t r = (int16_t)((WORLD_TOP - lat) / (WORLD_TOP - WORLD_BOTTOM) * WORLD_ROWS);
  if (c < 0 || c >= WORLD_COLS || r < 0 || r >= WORLD_ROWS) return;
  display.fillRect(c * 2, r * 2, 2, 2,
                   display.color565((uint8_t)(CITY_R * a), (uint8_t)(CITY_G * a), (uint8_t)(CITY_B * a)));
}

// ---------------------------------------------------------------- cities
uint8_t worldClockDefaultCount() { return (uint8_t)WORLD_CITY_COUNT; }

static void copyText(char *dst, size_t n, const char *src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = '\0';
}

bool worldClockCity(uint8_t id, WcCity *out) {
  if (id < WORLD_CITY_COUNT) {
    const WorldCity &k = kWorldCities[id];
    memset(out, 0, sizeof(*out));
    copyText(out->name, sizeof(out->name), k.name);
    out->lat = k.lat;
    out->lon = k.lon;
    copyText(out->posix, sizeof(out->posix), k.posix);
    copyText(out->iana, sizeof(out->iana), k.iana);
    return true;
  }
  if (id >= WC_ID_CUSTOM && id < WC_ID_CUSTOM + WC_CUSTOM_MAX && s_used[id - WC_ID_CUSTOM]) {
    *out = s_custom[id - WC_ID_CUSTOM];
    return true;
  }
  if (id == WC_ID_AUTO && s_haveAuto) {
    *out = s_auto;
    return true;
  }
  return false;
}

int worldClockNameWidth(const char *name) {
  int w = 0;
  for (; *name; name++) w += glyphFor(*name).xAdvance;
  return w;
}

const char *worldClockCheck(const WcCity &c) {
  const size_t n = strnlen(c.name, sizeof(c.name));
  if (n == 0 || n > WC_NAME_MAX) return "name must be 1 to 20 characters";
  for (size_t i = 0; i < n; i++) {
    const char ch = c.name[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' || ch == '-' || ch == '\''))
      return "name may use only A-Z, 0-9, space and . - '";
  }
  if (c.name[0] == ' ' || c.name[n - 1] == ' ' || strstr(c.name, "  "))
    return "name has a space at an end or two in a row";
  if (worldClockNameWidth(c.name) > WC_NAME_PX) return "name is too wide for the panel";
  // Written negated so that NaN is refused too.
  if (!(c.lat >= WORLD_BOTTOM && c.lat <= WORLD_TOP)) return "latitude is off the map, which spans 58 S to 78 N";
  if (!(c.lon >= -180.0f && c.lon < 180.0f)) return "longitude must be from -180 up to 180";
  if (strnlen(c.posix, sizeof(c.posix)) == sizeof(c.posix)) return "time zone string too long";
  PosixTz tz;
  if (!posixTzParse(c.posix, &tz)) return "time zone string not understood";
  const size_t z = strnlen(c.iana, sizeof(c.iana));
  if (z == sizeof(c.iana)) return "zone name too long";
  for (size_t i = 0; i < z; i++) {
    const char ch = c.iana[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '/' || ch == '_' || ch == '-' || ch == '+'))
      return "zone name has unexpected characters";
  }
  return nullptr;
}

static void refreshHome() {
  WcCity c;
  if (!worldClockCity(s_home, &c)) {
    s_home = 0;
    worldClockCity(0, &c);
  }
  const bool moved = !s_homeReady || strcmp(c.name, s_homeCity.name) != 0 || c.lat != s_homeCity.lat ||
                     c.lon != s_homeCity.lon || strcmp(c.posix, s_homeCity.posix) != 0;
  s_homeCity = c;
  s_homeReady = true;
  // Every way in is checked by worldClockCheck, and the built-in strings by
  // fx_parity.py; UTC is only the answer to a string that got past both.
  if (!posixTzParse(c.posix, &s_homeTz)) posixTzParse("UTC0", &s_homeTz);
  if (moved) s_pulseDue = true;
}

bool worldClockSetCustom(uint8_t slot, const WcCity *city) {
  if (slot >= WC_CUSTOM_MAX) return false;
  if (city && worldClockCheck(*city)) return false;
  if (city) s_custom[slot] = *city;
  s_used[slot] = city != nullptr;
  if (s_homeReady && s_home == WC_ID_CUSTOM + slot) refreshHome();
  return true;
}

bool worldClockSlotUsed(uint8_t slot) { return slot < WC_CUSTOM_MAX && s_used[slot]; }

void worldClockSetAuto(const WcCity *city) {
  if (city && worldClockCheck(*city)) return;
  if (city) s_auto = *city;
  s_haveAuto = city != nullptr;
  if (s_homeReady && s_home == WC_ID_AUTO) refreshHome();
}

// U+00C0 to U+017F, Latin-1 Supplement and Latin Extended-A, as base capitals.
// '?' marks the few that become two letters (below) or nothing (the signs).
static const char kLatinBase[] =
  "AAAAAA?CEEEEIIII" "DNOOOOO?OUUUUY??"   // U+00C0: A-grave .. I-diaeresis, Eth .. sharp s
  "AAAAAA?CEEEEIIII" "DNOOOOO?OUUUUY?Y"   // U+00E0: the lower case
  "AAAAAACCCCCCCCDD" "DDEEEEEEEEEEGGGG"   // U+0100: A-macron .. d-caron, D-stroke .. g-breve
  "GGGGHHHHIIIIIIII" "II??JJKKKLLLLLLL"   // U+0120: G-dot .. i-ogonek, I-dot .. L-middle-dot
  "LLLNNNNNNNNNOOOO" "OO??RRRRRRSSSSSS"   // U+0140: l-middle-dot .. o-breve, O-double-acute .. s-cedilla
  "SSTTTTTTUUUUUUUU" "UUUUWWYYYZZZZZZS";  // U+0160: S-caron .. u-ring, U-double-acute .. long s

void worldClockFitName(const char *utf8, char *out, size_t n) {
  char buf[64];
  size_t k = 0;
  auto put = [&](char ch) {
    if (k + 1 < sizeof(buf)) buf[k++] = ch;
  };
  const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
  while (*p) {
    unsigned cp = 0xFFFD;
    size_t len = 1;
    if (*p < 0x80) {
      cp = *p;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
      len = 2;
    } else {
      while ((p[len] & 0xC0) == 0x80) len++;   // longer or broken: skipped whole
    }
    p += len;
    const char *two = (cp == 0xC6 || cp == 0xE6) ? "AE" : (cp == 0xDE || cp == 0xFE) ? "TH" : cp == 0xDF ? "SS"
                    : (cp == 0x132 || cp == 0x133) ? "IJ" : (cp == 0x152 || cp == 0x153) ? "OE" : nullptr;
    if (two) { put(two[0]); put(two[1]); continue; }
    char ch = ' ';                              // spaces, and anything the font cannot show, part words
    if (cp >= 'a' && cp <= 'z') ch = (char)(cp - 32);
    else if ((cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '\'') ch = (char)cp;
    else if (cp >= 0xC0 && cp <= 0x17F && kLatinBase[cp - 0xC0] != '?') ch = kLatinBase[cp - 0xC0];
    put(ch);
  }
  // One space between words, none at the ends.
  size_t m = 0;
  for (size_t i = 0; i < k; i++) {
    if (buf[i] == ' ' && (m == 0 || buf[m - 1] == ' ')) continue;
    buf[m++] = buf[i];
  }
  while (m && buf[m - 1] == ' ') m--;
  // As much as fits; whole words when at least one whole word does.
  size_t cut = 0, whole = 0;
  int w = 0;
  for (size_t i = 0; i < m && i < WC_NAME_MAX; i++) {
    w += glyphFor(buf[i]).xAdvance;
    if (w > WC_NAME_PX) break;
    cut = i + 1;
    if (i + 1 == m || buf[i + 1] == ' ' || buf[i + 1] == '-') whole = i + 1;
  }
  if (cut < m && whole) cut = whole;
  while (cut && (buf[cut - 1] == ' ' || buf[cut - 1] == '-')) cut--;
  if (cut > n - 1) cut = n - 1;
  memcpy(out, buf, cut);
  out[cut] = '\0';
}

// ---------------------------------------------------------------- home
uint8_t worldClockHome()       { return s_home; }
bool    worldClockHomeChosen() { return s_chosen; }

void worldClockSetHome(uint8_t id, bool chosen) {
  s_home = id;
  s_chosen = chosen;
  refreshHome();
}

int32_t worldClockHomeOffset(int64_t utc) {
  if (!s_homeReady) refreshHome();
  return posixTzOffset(s_homeTz, utc);
}

uint32_t worldClockPulseLeftMs() {
  if (!s_pulsing) return 0;
  const uint32_t done = millis() - s_pulseAt, total = (uint32_t)(PULSE_S * 1000.0f);
  return done < total ? total - done : 0;
}

// ---------------------------------------------------------------- drawing
void worldClockDraw(int64_t utc, bool synced, float breath, float since) {
  if (!s_homeReady) refreshHome();
  if (!synced) {
    if (s_forMinute != NO_TIME) { allNight(); s_forMinute = NO_TIME; }
  } else if (floorDiv(utc, 60) != s_forMinute) {
    recompute(utc);
    s_forMinute = floorDiv(utc, 60);
  }

  for (uint8_t r = 0; r < WORLD_ROWS; r++)
    for (uint8_t c = 0; c < WORLD_COLS; c++) {
      if (landAt(r, c)) display.drawPixel(c * 2, r * 2, s_colour[r][c]);
    }

  // Home breathes, and so does its name while a change is announced: the same
  // value, so the two rise and fall together and read as one thing.
  const float breathA = 0.65f + 0.35f * fabsf(sinf((float)PI * breath));
  for (uint8_t i = 0; i < WORLD_CITY_COUNT; i++)
    drawCity(kWorldCities[i].lat, kWorldCities[i].lon, i == s_home ? breathA : 1.0f);
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++)
    if (s_used[i]) drawCity(s_custom[i].lat, s_custom[i].lon, s_home == WC_ID_CUSTOM + i ? breathA : 1.0f);
  if (s_haveAuto) drawCity(s_auto.lat, s_auto.lon, s_home == WC_ID_AUTO ? breathA : 1.0f);

  float a = 1.0f;
  if (since >= 0.0f && since < PULSE_S) {
    float ease = (PULSE_S - since) / FADE_S;
    if (ease > 1.0f) ease = 1.0f;
    a = 1.0f - (1.0f - breathA) * ease;
  }
  drawName(s_homeCity.name,
           display.color565((uint8_t)(CITY_R * a), (uint8_t)(CITY_G * a), (uint8_t)(CITY_B * a)));

  // HH:MM in the South Pacific, in home's zone. Digits are 6 wide with a 1 px
  // gap; the colon is two 2x2 dots.
  const uint16_t white = display.color565(255, 255, 255);
  int16_t x = TIME_X;
  int hh = 0, mm = 0;
  if (synced) {
    const int64_t local = utc + posixTzOffset(s_homeTz, utc);
    hh = (int)(floorDiv(local, 3600) % 24);
    mm = (int)(floorDiv(local, 60) % 60);
    drawDigit(x, TIME_Y, (uint8_t)(hh / 10), white); x += 7;
    drawDigit(x, TIME_Y, (uint8_t)(hh % 10), white); x += 7;
  } else {
    display.fillRect(x, TIME_Y + 5, 13, 2, white); x += 14;
  }
  display.fillRect(x, TIME_Y + 3, 2, 2, white);
  display.fillRect(x, TIME_Y + 7, 2, 2, white);
  x += 3;
  if (synced) {
    drawDigit(x, TIME_Y, (uint8_t)(mm / 10), white); x += 7;
    drawDigit(x, TIME_Y, (uint8_t)(mm % 10), white);
  } else {
    display.fillRect(x, TIME_Y + 5, 13, 2, white);
  }
}

#if !defined(WORLDCLOCK_HOST)
void worldClockRender() {
  const time_t now = time(nullptr);
  const uint32_t ms = millis();
  // Started here rather than when home changed: this only runs while the page
  // is on screen, so a change made elsewhere is announced when it is seen.
  if (s_pulseDue) {
    s_pulseDue = false;
    s_pulsing = true;
    s_pulseAt = ms;
  }
  float since = -1.0f;
  if (s_pulsing) {
    const uint32_t d = ms - s_pulseAt;
    if (d < (uint32_t)(PULSE_S * 1000.0f)) since = (float)d / 1000.0f;
    else s_pulsing = false;
  }
  // The breathing is |sin(pi t)| with a period of one second, so millis()
  // modulo a whole minute keeps its phase. A float of raw millis(), as this
  // used to take, is exact only up to 2^24 ms: 4.7 hours of uptime.
  // Before NTP the clock reads 1970; say so rather than draw a confident lie.
  worldClockDraw((int64_t)now, now > 1700000000, (float)(ms % 60000UL) / 1000.0f, since);
}

void worldClockMapJson(JsonObject out) {
  out["cols"]   = WORLD_COLS;
  out["rows"]   = WORLD_ROWS;
  out["top"]    = WORLD_TOP;
  out["bottom"] = WORLD_BOTTOM;
  out["home"]   = s_home;
  // One 16-digit hex string a row, bit c (from the least significant end) set =
  // land at column c, exactly as kWorldMask stores it. JSON numbers would lose
  // the low bits of a 64-bit row in a browser.
  JsonArray mask = out["mask"].to<JsonArray>();
  for (uint8_t r = 0; r < WORLD_ROWS; r++) {
    char hex[17];
    snprintf(hex, sizeof(hex), "%08lX%08lX", (unsigned long)(kWorldMask[r] >> 32),
             (unsigned long)(kWorldMask[r] & 0xFFFFFFFFULL));
    mask.add(hex);   // a char array: copied into the document
  }
  JsonArray cities = out["cities"].to<JsonArray>();
  auto add = [&](uint8_t id, const char *kind) {
    WcCity c;
    if (!worldClockCity(id, &c)) return;
    JsonObject o = cities.add<JsonObject>();
    o["id"]   = id;
    o["kind"] = kind;
    o["name"] = (char *)c.name;   // non-const: ArduinoJson copies it, and c is a local
    o["lat"]  = c.lat;
    o["lon"]  = c.lon;
    o["tz"]   = (char *)c.iana;
  };
  for (uint8_t i = 0; i < WORLD_CITY_COUNT; i++) add(i, "builtin");
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++) add((uint8_t)(WC_ID_CUSTOM + i), "custom");
  add(WC_ID_AUTO, "auto");
}
#endif

#endif  // WORLDCLOCK_ENABLED

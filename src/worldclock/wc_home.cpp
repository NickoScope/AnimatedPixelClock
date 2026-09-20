#include "wc_home.h"

#if defined(WORLDCLOCK_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../config/config.h"
#include "../network/net_lock.h"
#include "../timezones.h"
#include "posix_tz.h"
#include "worldclock.h"

#include <esp_heap_caps.h>
#include "../network/net_turns.h"
#include "../network/network.h"
#include "../util/psram_json.h"


// "At the panel's location" means the same city, not the nearest famous one.
// A circle as large as Greater London (1 572 km2, Wikipedia) has a radius of
// 22.4 km, so 25 km names a panel anywhere in a city of that size after it,
// while Nice - 26.5 km from Cannes by the geocoder's own coordinates - stays a
// place of its own. One map dot is some 450 km across at these latitudes, so
// this is about the name, not the picture.
static const float HOME_RADIUS_KM = 25.0f;

// freeipapi.com: HTTPS on the free tier, no key, 60 requests a minute, and
// "commercial and non-commercial use" (freeipapi.com/docs, read 2026-09-14).
// Its answer carries latitude, longitude, cityName and timeZones, IANA names.
// ipapi.co's free plan is "not for production use", ip-api.com keeps HTTPS for
// paying users, and ipinfo.io's free plan stops at the country.
static const char *const IP_URL = "https://free.freeipapi.com/api/v1/json";

enum : uint8_t { IP_IDLE, IP_RUNNING, IP_DONE, IP_FAILED };

// The lookup task writes s_ipCity, then sets s_ipState to IP_DONE; loop() reads
// the city only after it sees IP_DONE, so the two never touch it at once.
static volatile uint8_t s_ipState = IP_IDLE;
static WcCity           s_ipCity;

static const char *s_source   = "zone";
// When this lookup first stood aside for someone at the portal, 0 if it is not
// waiting (net_turns.h). It was a counter inside a compound condition, which
// meant it was never incremented at all and the ceiling never applied: a browser
// left open blocked the lookup for ever. A function, so there is somewhere to
// record the waiting.
static uint32_t s_wcYieldingSince = 0;
static bool wcYieldToPortal() {
  const uint32_t now = millis();
  if (!netTurnYield(netMsSinceHttp(), s_wcYieldingSince, now)) { s_wcYieldingSince = 0; return false; }
  if (!s_wcYieldingSince) s_wcYieldingSince = now ? now : 1;
  return true;
}
static bool        s_dirty    = true;
static bool        s_wasChosen = false;
static float       s_lat = NAN, s_lon = NAN;
static char        s_zone[WC_POSIX_MAX + 1] = "";
static uint32_t    s_zoneAt = 0;
static bool        s_ipSeen = false;

static bool locationSet() {
  // 0,0 (the Gulf of Guinea) doubles as "unset", as in weather.cpp.
  return !(settings.weatherLat == 0 && settings.weatherLon == 0);
}

// The zone the panel's own clock runs in, as applyTimezone() (network.cpp)
// hands it to configTzTime(): the stored string, else the old offset's
// default region, else that offset with no summer time.
static void panelZone(char *out, size_t n) {
  PosixTz tz;
  if (settings.timezoneString[0] && posixTzParse(settings.timezoneString, &tz)) {
    strncpy(out, settings.timezoneString, n - 1);
    out[n - 1] = '\0';
    return;
  }
  const char *legacy = getDefaultTimezoneForOffset(settings.gmtOffset);
  if (legacy && posixTzParse(legacy, &tz)) {
    strncpy(out, legacy, n - 1);
    out[n - 1] = '\0';
    return;
  }
  const int m = settings.gmtOffset, a = m < 0 ? -m : m;
  if (a % 60) snprintf(out, n, "<%c%02d%02d>%s%d:%02d", m < 0 ? '-' : '+', a / 60, a % 60, m > 0 ? "-" : "", a / 60, a % 60);
  else        snprintf(out, n, "<%c%02d>%s%d", m < 0 ? '-' : '+', a / 60, m > 0 ? "-" : "", a / 60);
}

static float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float k = (float)DEG_TO_RAD;
  const float s1 = sinf((lat2 - lat1) * k / 2), s2 = sinf((lon2 - lon1) * k / 2);
  const float a = s1 * s1 + cosf(lat1 * k) * cosf(lat2 * k) * s2 * s2;
  return 2.0f * 6371.0f * asinf(sqrtf(a));   // haversine on a sphere of the Earth's mean radius
}

// The built-in city whose offset is the panel's own now; the first on a tie,
// the closest when none is equal.
static void zoneHome() {
  char zone[WC_POSIX_MAX + 1];
  panelZone(zone, sizeof(zone));
  PosixTz panel, city;
  if (!posixTzParse(zone, &panel)) posixTzParse("UTC0", &panel);
  const int64_t now = (int64_t)time(nullptr);
  const int32_t want = posixTzOffset(panel, now);
  uint8_t best = 0;
  int32_t bestDiff = INT32_MAX;
  WcCity c;
  for (uint8_t i = 0; i < worldClockDefaultCount(); i++) {
    if (!worldClockCity(i, &c) || !posixTzParse(c.posix, &city)) continue;
    int32_t d = posixTzOffset(city, now) - want;
    if (d < 0) d = -d;
    if (d < bestDiff) { bestDiff = d; best = i; }
  }
  worldClockSetHome(best, false);
  worldClockSetAuto(nullptr);
  s_source = "zone";
}

// Home at lat/lon: the nearest city within the radius, else one made there -
// named by the caller, or HOME in the panel's own zone.
static bool placeHome(float lat, float lon, const WcCity *named, const char *source) {
  WcCity c;
  int best = -1;
  float bestKm = HOME_RADIUS_KM;
  auto consider = [&](uint8_t id) {
    if (!worldClockCity(id, &c)) return;
    const float d = distanceKm(lat, lon, c.lat, c.lon);
    if (d <= bestKm) { bestKm = d; best = id; }
  };
  for (uint8_t i = 0; i < worldClockDefaultCount(); i++) consider(i);
  for (uint8_t i = 0; i < WC_CUSTOM_MAX; i++) consider((uint8_t)(WC_ID_CUSTOM + i));
  if (best >= 0) {
    worldClockSetHome((uint8_t)best, false);   // before the auto city goes, so home never lands on it
    worldClockSetAuto(nullptr);
    s_source = source;
    return true;
  }
  WcCity made;
  if (named) {
    made = *named;
  } else {
    memset(&made, 0, sizeof(made));
    strcpy(made.name, "HOME");
    made.lat = lat;
    made.lon = lon;
    panelZone(made.posix, sizeof(made.posix));
  }
  if (worldClockCheck(made)) return false;      // a location off the map, say
  worldClockSetAuto(&made);
  worldClockSetHome(WC_ID_AUTO, false);
  s_source = source;
  return true;
}

static void decide() {
  if (locationSet() && placeHome(settings.weatherLat, settings.weatherLon, nullptr, "location")) return;
  if (!locationSet() && s_ipState == IP_DONE && placeHome(s_ipCity.lat, s_ipCity.lon, &s_ipCity, "ip")) return;
  zoneHome();
}

static void ipTask(void *) {
  WcCity c;
  memset(&c, 0, sizeof(c));
  bool ok = false;
  {
    NetLockGuard net(2 * NET_LOCK_WAIT_MS);   // declared first: released after the connection closes
    WiFiClientSecure client;
    client.setInsecure();   // as the weather fetch: public, non-sensitive data, no certificate bundle
    HTTPClient http;
    http.setTimeout(10000);
    http.useHTTP10(true);
    if (net.held() && http.begin(client, IP_URL)) {
      const int code = http.GET();
      if (code == HTTP_CODE_OK) {
        JsonDocument doc(psramJson());   // the location lookup
        if (!deserializeJson(doc, http.getStream())) {
          c.lat = doc["latitude"] | NAN;
          c.lon = doc["longitude"] | NAN;
          worldClockFitName(doc["cityName"] | "", c.name, sizeof(c.name));
          const char *iana  = doc["timeZones"][0] | "";
          const char *posix = tzdbPosix(iana);
          if (posix) {
            strncpy(c.posix, posix, sizeof(c.posix) - 1);
            strncpy(c.iana, iana, sizeof(c.iana) - 1);
            ok = worldClockCheck(c) == nullptr;
          }
        }
      }
      Serial.printf("World clock: IP location HTTP %d, %s\n", code, ok ? c.name : "not used");
      http.end();
    }
  }
  if (ok) s_ipCity = c;
  s_ipState = ok ? IP_DONE : IP_FAILED;
  vTaskDelete(nullptr);
}

const char *wcHomeSource() { return worldClockHomeChosen() ? "chosen" : s_source; }

void wcHomeRethink() {
  s_dirty = true;
  wcHomeTick();
}

void wcHomeBegin() {
  s_wasChosen = !worldClockHomeChosen();   // makes the first tick notice whichever it is
  wcHomeRethink();
}

void wcHomeTick() {
  const bool chosen = worldClockHomeChosen();
  if (chosen != s_wasChosen) {
    s_wasChosen = chosen;
    s_dirty = true;
  }
  if (chosen) {
    if (s_dirty) {
      s_dirty = false;
      worldClockSetAuto(nullptr);             // a kept choice needs no made-up city beside it
    }
    return;
  }

  char zone[WC_POSIX_MAX + 1];
  panelZone(zone, sizeof(zone));
  if (settings.weatherLat != s_lat || settings.weatherLon != s_lon || strcmp(zone, s_zone)) {
    s_lat = settings.weatherLat;              // a location or zone saved in the settings page
    s_lon = settings.weatherLon;
    memcpy(s_zone, zone, sizeof(s_zone));
    s_dirty = true;
  }
  if (s_ipState == IP_DONE && !s_ipSeen) {
    s_ipSeen = true;
    s_dirty = true;
  }
  // By the offset, home follows the clock: NTP arriving, or a summer time change.
  if (!strcmp(s_source, "zone") && millis() - s_zoneAt > 60000UL) s_dirty = true;
  if (s_dirty) {
    s_dirty = false;
    s_zoneAt = millis();
    decide();
  }

  // One lookup per boot, once NTP has proved the way out works, and only when
  // nothing better will ever answer.
  if (s_ipState == IP_IDLE && !locationSet() && WiFi.status() == WL_CONNECTED && time(nullptr) > 1700000000 &&
      !netLockBusy() && !wcYieldToPortal()) {
    s_ipState = IP_RUNNING;
    // Core 0 and 8 KB, as the weather task that does the same HTTPS and JSON work.
    if (xTaskCreatePinnedToCore(ipTask, "wcHomeIp", 8192, nullptr, 0, nullptr, 0) != pdPASS) s_ipState = IP_FAILED;
  }
}

#endif  // WORLDCLOCK_ENABLED

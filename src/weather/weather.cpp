/*
 * AnimatedPixelClock - Weather Module (Open-Meteo)
 *
 * The fetch (DNS + TLS handshake + transfer) can block for seconds, so it
 * runs in a task pinned to core 0 - never in loop(), where it would visibly
 * freeze a 60 Hz animation. The task lives for one fetch: loop() starts it
 * when a fetch is due and it deletes itself, so its stack is not held in
 * internal SRAM for the ten minutes between fetches. Results are copied into
 * `published` under a spinlock; the render loop takes snapshots via
 * getWeather().
 */

#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "../config/config.h"
#include "../clocks/cycle_config.h"
#include "../network/network.h"
#include "../network/net_lock.h"

#include <esp_heap_caps.h>
#include "../network/net_turns.h"
#include "../network/network.h"
#include "../net/net_broker.h"
#include "../util/psram_json.h"


#define WEATHER_FETCH_INTERVAL_MS (10UL * 60UL * 1000UL)
#define WEATHER_RETRY_INTERVAL_MS (60UL * 1000UL)
#define WEATHER_CHECK_MS 1000UL
#define WEATHER_TASK_STACK 8192

static WeatherData published = {};
static portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
// fetchBusy marks "a fetch is out". On the broker path only the loop task ever
// touches it or nextFetchMs - it is set at submit and cleared when nbTake()
// collects, which is strictly simpler than what follows. On the fallback path
// it is set by loop() before the task starts and cleared by the task last,
// after it has written nextFetchMs; loop() reads nextFetchMs only while
// fetchBusy is clear.
static volatile bool fetchBusy = false;
static volatile bool fetchKick = false;
static volatile unsigned long nextFetchMs = 0;   // 0: due now

bool weatherConfigured() {
  // 0,0 (middle of the Atlantic) doubles as the "unset" marker.
  return settings.weatherEnabled &&
         !(settings.weatherLat == 0 && settings.weatherLon == 0);
}

// A fetch runs only while the weather clock is on the panel: when it comes into
// view with data older than the interval, and once an interval while it stays.
// Off screen nothing is fetched, and no task, TLS session or buffer exists (the
// owner's brief, 2026-09-14). The last result is a few bytes in `published`.
static unsigned long shownMs = 0;   // loop() only: the page draws, weatherLoop() reads

void weatherNoteShown() { shownMs = millis() | 1; }

static bool weatherOnScreen() {
  return shownMs && millis() - shownMs < 3000UL;
}

WeatherData getWeather() {
  portENTER_CRITICAL(&weatherMux);
  WeatherData copy = published;
  portEXIT_CRITICAL(&weatherMux);
  return copy;
}

WeatherIconKind weatherIconFromCode(int code) {
  if (code == 0) return WICON_SUN;
  if (code <= 2) return WICON_PARTCLOUD;
  if (code == 3) return WICON_CLOUD;
  if (code == 45 || code == 48) return WICON_FOG;
  if (code >= 71 && code <= 77) return WICON_SNOW;
  if (code == 85 || code == 86) return WICON_SNOW;
  if (code >= 95) return WICON_STORM;
  // Everything else in the 5x/6x/8x ranges is some form of rain/drizzle.
  return WICON_RAIN;
}

// Copy "HH:MM" out of an ISO-8601 timestamp ("2026-07-03T04:30").
static void extractClockTime(const char* iso, char out[6]) {
  out[0] = '\0';
  if (!iso) return;
  const char* t = strchr(iso, 'T');
  if (t && strlen(t) >= 6) {
    memcpy(out, t + 1, 5);
    out[5] = '\0';
  }
}

// The URL, built the same way whoever does the fetching. Kept apart from the
// fetch so the broker path and the old task path cannot drift.
static void weatherBuildUrl(char *url, size_t cap) {
  bool hasKey = settings.weatherApiKey[0] != '\0';
  // The commercial tier uses the same API on a customer- host with an apikey.
  snprintf(url, cap,
           "https://%s/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&timezone=auto&forecast_days=1%s%s",
           hasKey ? "customer-api.open-meteo.com" : "api.open-meteo.com",
           settings.weatherLat, settings.weatherLon,
           hasKey ? "&apikey=" : "", hasKey ? settings.weatherApiKey : "");
}

// The response, read straight off the socket into a PSRAM document, again in
// one place for both paths. It runs on whichever task did the fetching, and
// touches nothing but `published`, under `published`'s own spinlock.
// Everything downstream of "we have valid JSON", shared by both paths so they
// cannot drift apart.
static bool weatherPublish(JsonDocument &doc) {
  JsonObject current = doc["current"];
  JsonObject daily = doc["daily"];
  if (current.isNull() || daily.isNull()) return false;

  WeatherData fresh = {};
  fresh.valid = true;
  fresh.tempC = current["temperature_2m"] | 0.0f;
  fresh.humidity = current["relative_humidity_2m"] | 0;
  fresh.weatherCode = current["weather_code"] | 3;
  fresh.windKmh = current["wind_speed_10m"] | 0.0f;
  fresh.tempMaxC = daily["temperature_2m_max"][0] | 0.0f;
  fresh.tempMinC = daily["temperature_2m_min"][0] | 0.0f;
  extractClockTime(daily["sunrise"][0], fresh.sunrise);
  extractClockTime(daily["sunset"][0], fresh.sunset);
  fresh.fetchedAt = millis();

  portENTER_CRITICAL(&weatherMux);
  published = fresh;
  portEXIT_CRITICAL(&weatherMux);
  netMarkOutboundOk();

  Serial.printf("Weather: %.1fC code %d (RH %d%%)\n", fresh.tempC,
                fresh.weatherCode, fresh.humidity);
  return true;
}

// The broker path's parse: a NUL-terminated body already in PSRAM.
static bool weatherParse(const char *body, size_t len) {
  JsonDocument doc(psramJson());
  const DeserializationError err = deserializeJson(doc, body, len);
  if (err) {
    Serial.printf("Weather JSON error: %s\n", err.c_str());
    return false;
  }
  return weatherPublish(doc);
}

// The last mailbox sequence this module has acted on. Loop task only.
static uint32_t s_weatherSeq = 0;

// The broker path: the body is already in PSRAM and this runs on the loop
// task, so the parse is an ordinary loop-side cost like any other, and the
// broker's own stack never has to be big enough to hold it.
// Returns whether an answer arrived at all; *ok says whether THIS answer was
// good. The two are separate on purpose: `published.valid` may still be true
// from an earlier success, so deciding the back-off from it would give a failed
// fetch the ten-minute interval instead of the one-minute retry.
static bool weatherCollect(bool *ok) {
  *ok = false;
  NbMailbox *mb = nbMailbox(NB_WEATHER);
  if (!mb || mb->seq == s_weatherSeq) return false;   // nothing new
  s_weatherSeq = mb->seq;                             // read once, then act
  if (mb->code != HTTP_CODE_OK || !mb->bodyLen) {
    Serial.printf("Weather fetch failed: code %ld\n", (long)mb->code);
    return true;                                      // answered, badly
  }
  *ok = weatherParse(mb->body, mb->bodyLen);
  if (!*ok) Serial.println("Weather: the body did not parse");
  return true;
}

static bool fetchWeather() {
  char url[320];
  weatherBuildUrl(url, sizeof url);

  WiFiClientSecure client;
  client.setInsecure();  // public, non-sensitive data; saves a cert bundle
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather fetch failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  // The fallback path still streams, because it has no mailbox to stream into.
  // It shares everything downstream of the JSON with the broker path.
  JsonDocument doc(psramJson());
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Weather JSON error: %s\n", err.c_str());
    return false;
  }
  return weatherPublish(doc);
}

static void weatherFetchTask(void*) {
  bool ok = false;
  {
    NetLockGuard net(NET_LOCK_WAIT_MS);
    ok = net.held() && fetchWeather();
  }
  nextFetchMs = millis() + (ok ? WEATHER_FETCH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS);
  fetchBusy = false;
  vTaskDelete(nullptr);
}

// When this starter first stood aside for someone at the portal, 0 if it is not
// waiting. File scope so the early returns above can clear it: the deadline
// measures time since the FIRST standing aside, so a pass that left for another
// reason - the page off screen, the link down - would otherwise come back with
// the deadline already spent and skip its first yield.
static uint32_t s_weatherYieldingSince = 0;

void weatherLoop() {
  // Collect first, and unconditionally: the answer has to be taken even if the
  // page has since left the screen, or fetchBusy would never clear and this
  // module would go quiet until a reboot.
  { bool ok = false;
    if (weatherCollect(&ok)) {
      nextFetchMs = millis() + (ok ? WEATHER_FETCH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS);
      fetchBusy = false;
    } }
  if (fetchBusy) return;
  const unsigned long now = millis();
  static unsigned long lastCheckMs = 0;
  // The kick no longer skips this. It used to, so that a settings change acted
  // at once - but fetchKick now survives a refused submit (which is right: a
  // person is still waiting), and the two together let a refusal retry on every
  // single pass of loop(). One second's delay on a kick is not perceptible; an
  // unthrottled retry loop is exactly the kind of thing that only shows up on
  // the consumer with the longest URL, months later.
  if (now - lastCheckMs < WEATHER_CHECK_MS) return;
  lastCheckMs = now;
  // Not the moment to fetch at all: forget any standing aside, or the deadline
  // is already spent when the page comes back.
  if (!weatherConfigured() || !weatherOnScreen() || WiFi.status() != WL_CONNECTED) { s_weatherYieldingSince = 0; return; }
  // A settings change (new location, toggle) fetches now instead of waiting.
  if (!fetchKick && nextFetchMs && (long)(now - nextFetchMs) < 0) return;
  // A person at the portal beats a refresh that can wait (net_turns.h). The
  // deadline stops a browser left open from starving this for ever - counted in
  // time, because this runs on every pass of loop() and passes are free.
  { const uint32_t nowTurn = millis();
    if (netTurnYield(netMsSinceHttp(), s_weatherYieldingSince, nowTurn)) {
      if (!s_weatherYieldingSince) s_weatherYieldingSince = nowTurn ? nowTurn : 1;
      return;
    }
    s_weatherYieldingSince = 0; }
  // **The broker if it is up, this module's own task if it is not.** A runtime
  // choice, not a compile-time one, and deliberately so: nbBegin() can fail
  // (no PSRAM for its buffers), and a module whose only path is the broker
  // would then go silent until a reboot - a worse failure than the one the
  // broker exists to fix. The old path costs flash, which is not scarce here.
  if (nbReady(NB_WEATHER)) {
    // Neither of the two checks below is needed here, and that is the point of
    // the broker: there is no task to create, so there is no 9 KB contiguous
    // internal block to find first, and no reason to stand down because someone
    // else is on the wire - the queue is what handles that, without losing the
    // request. A settings change is interactive: the owner is looking at the
    // panel waiting for the new place's weather.
    char url[320];
    weatherBuildUrl(url, sizeof url);
    NbRequest req = {};
    req.url = url;
    req.timeoutMs = 10000;   // as this module has always used
    const bool interactive = fetchKick;
    fetchBusy = true;
    if (nbSubmitRequest(NB_WEATHER, req, interactive)) {
      // Only now: a refused submit must not throw away the fact that a person
      // was waiting, or the retry goes out as an ordinary scheduled refresh.
      fetchKick = false;
    } else {
      fetchBusy = false;
      nextFetchMs = now + WEATHER_RETRY_INTERVAL_MS;
    }
    return;
  }
  if (netLockBusy()) return;   // another fetch holds the network; check again in a second
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < WEATHER_TASK_STACK + 1024) {
    nextFetchMs = now + WEATHER_RETRY_INTERVAL_MS;
    fetchKick = false;
    return;
  }
  fetchKick = false;
  fetchBusy = true;
  // Core 0 below the Lua effect task: the Arduino loop (and the HUB75 DMA
  // refresh) live on core 1.
  if (xTaskCreatePinnedToCore(weatherFetchTask, "weather", WEATHER_TASK_STACK, nullptr, 0,
                              nullptr, 0) != pdPASS) {
    fetchBusy = false;
    nextFetchMs = now + WEATHER_RETRY_INTERVAL_MS;
  }
}

void weatherSettingsChanged() {
  fetchKick = true;
}

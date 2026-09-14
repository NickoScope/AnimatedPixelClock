// The portal's Panel group: JSON routes.
//
// Every route answers GET with its state and POST with a JSON body that changes
// it, then answers with the new state. A POST is validated whole before any of
// it is applied, and out-of-range input is refused with 400, never clamped.
// CORS and {"success":...} as the other /api handlers. No route reports a broker
// host, a user, a password or a key - only whether one is stored.
//
//   GET  /api/panel         now, carousel, pages, styles, time
//   POST /api/panel         {"show":{"page":i[,"card":"name"]}} | {"style":id}
//                           | {"enable":{"key":"world","on":false}}
//                           | {"carousel":{"enabled":b,"idleS":n,"slotS":n,"allStyles":b}}
//   GET  /api/flightboard   airport (id), dir, side, airports [{id, code, iata, name, tz,
//                           kind builtin|custom}], limits, board, direct {key, state, budget,
//                           bounds, usage, lists, last, sample}, tracked [...], mqtt
//   POST /api/flightboard   {"airport":id,"dir":"arr"|"dep"|"alt"} and at most one of
//                           {"add":{"icao":"KJFK","iata":"JFK","name":"NEW YORK",
//                           "tz":"America/New_York"[,"select":true]}} | {"remove":id}
//                           | {"track":"AF7301"} | {"untrack":"AF7301"}
//                           | {"budget":{"floor_min":5..240,"day_cap":0..1000,"month_cap":0..20000}}
//                           400 names the field; 409 a full list, a code already listed,
//                           a flight already tracked; 404 untrack of a flight not tracked.
//                           (src/flightboard/fb_settings.h, aero_direct.h)
//   GET  /api/railboard     station, what is showing, lists, Home Assistant status, config, mqtt
//   POST /api/railboard     {"crs":"GLD"} | {"diag":b} | {"reset":true}
//                           | {"config":{"rows":1..8,"switch_s":3..600,"level":10..100,
//                           "stale_s":30..3600,"due_min":0..15,"clock_seconds":b,
//                           "row_color"|"head_color"|"due_color":"amber"|"yellow"|
//                           "orange"|"white"|"green"|"cyan"}}   (src/railboard/rb_settings.h)
//   GET  /api/worldclock    mask, cities [{id, kind builtin|custom|auto, name, lat, lon, tz}],
//                           home, homeChosen, homeSource chosen|location|ip|zone, homeTime,
//                           homeOffset, pulseMs, limits {custom, name, namePx, advance}, tzdb, utc
//   POST /api/worldclock    one of {"home":id}
//                           | {"add":{"name":"SAN FRANCISCO","lat":37.77,"lon":-122.42,
//                           "tz":"America/Los_Angeles"[,"home":true]}}
//                           | {"remove":id} | {"auto":true}
//                           400 bad input; 409 no free slot, or the name is on the map.
//                           A POST that changes home puts the page on the panel.
//   GET  /api/knob          settings, defaults, bounds, stats
//   POST /api/knob          {"reverse":b,"lockoutMs":n,"debounceMs":n,"detent":-1..1}
//                           | {"defaults":true}
//   GET  /api/yachtradar    keyPresent, stream, vessels
//   POST /api/yachtradar    {"bySize":b}
//   GET  /api/lua           effects, current            (LUA_EFFECTS_ENABLED only)
//   POST /api/lua           {"show":i}
//   GET  /api/clips         card {mounted, type, totalKB, freeKB}, reason, maxFrames, maxBytes,
//                           current, playing, clips [{name, bytes, frames, ms}],
//                           stream {state idle|playing|failed, clip, frames, reads, readAvgMs,
//                           readMaxMs, underruns, loops, queued, stackFree}   (CLIPS_SD_ENABLED only)
//   POST /api/clips         one of {"play":"name"} | {"delete":"name"} | {"mount":true}
//                           400 bad input; 404 no such clip; 422 not a valid clip; 503 no card.
//                           Play puts the clip on screen, as /api/anim/play does a flash one.
//   Two routes of the card's gallery are not JSON, and answer as below:
//   POST /api/clips/upload  multipart, ?name=<1-24 of A-z 0-9 _ ->[&bytes=<file size>]; streams
//                           to /clips/upload.tmp, validated and renamed into place when complete.
//                           {"success":true,"name":...} or 400 {"success":false,"error":...}
//   GET  /api/clips/frame   ?name=&i=: header, palette and frame i, application/octet-stream
//
// A route whose module is not built is not registered, so it answers 404.

#include "web_panel.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <ArduinoJson.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "../ambient/ambient.h"
#include "../cards/cards.h"
#include "../config/config.h"
#include "../config/settings.h"
#include "../control/carousel.h"
#include "../control/clock_styles.h"
#include "../control/control.h"
#include "../display/display.h"
#include "../flightboard/aero_direct.h"
#include "../flightboard/fb_mqtt.h"
#include "../flightboard/fb_settings.h"
#include "../flightboard/flightboard.h"
#include "../mqtt/mqtt_bus.h"
#include "../panel/panel.h"
#include "../railboard/railboard.h"
#include "../utils/utils.h"
#include "../viz/visualizer.h"
#include "../worldclock/posix_tz.h"
#include "../worldclock/wc_home.h"
#include "../worldclock/worldclock.h"
#include "../yachtradar/yachtradar.h"
#if defined(LUA_EFFECTS_ENABLED)
#include "../lua/lua_effects.h"
#endif
#if defined(CLIPS_SD_ENABLED)
#include <SD_MMC.h>
#include <esp_task_wdt.h>

#include "../ambient/anim_store.h"
#include "../clips/clip_sd.h"
#endif
#include "web.h"

extern bool httpForceClock;
extern bool httpForceAmbient;
extern bool httpForceViz;
int getOptimalRefreshRate();

// ---------------------------------------------------------------- plumbing
// Documents for these routes come from PSRAM when there is some. Internal SRAM
// is the scarce heap on this board - the HUB75 buffers and lwip live in it and
// /api/info has shown its low-water mark near 16 KB - and a response here is
// built, sent and freed within one request.
class PanelJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
};
static PanelJsonAllocator s_alloc;

// Every request body here is a few dozen bytes.
static const size_t BODY_MAX = 1024;

static void failOom() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(503, "application/json", "{\"success\":false,\"error\":\"out of memory\"}");
}

static void sendDoc(JsonDocument &doc, int code = 200) {
  if (doc.overflowed()) { failOom(); return; }
  const size_t n = measureJson(doc);
  char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_8BIT);
  if (!buf) { failOom(); return; }
  serializeJson(doc, buf, n + 1);
  sendJsonBytesGuarded(code, buf, n);
  heap_caps_free(buf);
}

static void fail(int code, const char *why) {
  JsonDocument doc(&s_alloc);
  doc["success"] = false;
  doc["error"]   = why;
  sendDoc(doc, code);
}

#define REJECT(code, why) do { fail(code, why); return; } while (0)

static bool isPost() { return server.method() == HTTP_POST; }

static bool readBody(JsonDocument &doc) {
  // Only a JSON content type. With it, a browser must ask this server first (a
  // CORS preflight, an OPTIONS request) before another site's page may post
  // here, and no route answers OPTIONS. A text/plain body needs no such
  // question and would still arrive in arg("plain") - so it is refused.
  if (!server.header("Content-Type").startsWith("application/json")) {
    fail(415, "Content-Type must be application/json");
    return false;
  }
  if (!server.hasArg("plain")) { fail(400, "missing JSON body"); return false; }
  const String body = server.arg("plain");
  if (body.length() > BODY_MAX) { fail(413, "body too large"); return false; }
  if (deserializeJson(doc, body) || !doc.is<JsonObject>()) { fail(400, "invalid JSON"); return false; }
  return true;
}

// An integer within [lo, hi]. Refuses 1.5, "3", true and anything out of range.
static bool intIn(JsonVariantConst v, long lo, long hi, long *out) {
  if (!v.is<long>()) return false;
  const long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = x;
  return true;
}

// A present key must be a boolean; an absent one leaves *out alone.
static bool optBool(JsonVariantConst v, bool *out) {
  if (v.isNull()) return true;
  if (!v.is<bool>()) return false;
  *out = v.as<bool>();
  return true;
}

static const char *const KEY_NAMES[PANEL_KEY_COUNT] = {
  "clock", "world", "flights", "trains", "yachts", "cards", "lua"};

static const char *keyName(uint8_t key) {
  return key < PANEL_KEY_COUNT ? KEY_NAMES[key] : "other";
}

// Cards follow the fixed pages, so the first card's page is the fixed count.
static uint8_t fixedPageCount() {
  const uint8_t n = panelPageCount();
  uint8_t i = 0;
  while (i < n && panelPageKey(i) != PANEL_KEY_CARDS) i++;
  return i;
}

// The page index of a fixed page, for a module route's "show now"; -1 if absent.
static int pageOf(uint8_t key) {
  const uint8_t n = fixedPageCount();
  for (uint8_t i = 0; i < n; i++)
    if (panelPageKey(i) == key) return i;
  return -1;
}

#if defined(FLIGHTBOARD_ENABLED) || defined(RAILBOARD_ENABLED) || defined(WORLDCLOCK_ENABLED) || \
    defined(YACHTRADAR_ENABLED)
static void pageInfo(JsonDocument &doc, uint8_t key) {
  doc["page"]    = pageOf(key);
  doc["showing"] = panelPageKey(panelCurrentPage()) == key;
}
#endif

static const char *styleName(uint8_t id) {
  for (uint8_t i = 0; i < CLOCK_STYLE_COUNT; i++)
    if (kClockStyles[i].id == id) return kClockStyles[i].name;
  return "?";
}

static bool styleKnown(long id) {
  for (uint8_t i = 0; i < CLOCK_STYLE_COUNT; i++)
    if (kClockStyles[i].id == id) return true;
  return false;
}

#if defined(MQTT_BUS_ENABLED) && (defined(FB_MQTT_ENABLED) || defined(RAILBOARD_ENABLED))
static void mqttJson(JsonObject o, const char *status) {
  o["configured"] = mqttBusConfigured();
  o["connected"]  = mqttBusConnected();
  o["status"]     = status;
}
#endif

String panelWebFeatures() {
  String f = "panel";
#if defined(CAROUSEL_ENABLED)
  f += " carousel";
#endif
#if defined(CARDS_ENABLED)
  f += " cards";
#endif
#if defined(FLIGHTBOARD_ENABLED)
  f += " flights";
#endif
#if defined(RAILBOARD_ENABLED)
  f += " trains";
#endif
#if defined(WORLDCLOCK_ENABLED)
  f += " world";
#endif
#if defined(YACHTRADAR_ENABLED)
  f += " yachts";
#endif
#if defined(CLIPS_SD_ENABLED)
  f += " sdclips";
#endif
#if defined(LUA_EFFECTS_ENABLED)
  f += " lua";
#endif
#if defined(MQTT_BUS_ENABLED)
  f += " mqtt";
#endif
  return f;
}

// ---------------------------------------------------------------- /api/panel
static void buildPanel(JsonDocument &doc) {
  doc["success"] = true;
  const uint8_t n     = panelPageCount();
  const uint8_t fixed = fixedPageCount();
  const uint8_t cur   = panelCurrentPage();

  JsonObject now = doc["now"].to<JsonObject>();
  now["page"]      = cur;
  now["key"]       = keyName(panelPageKey(cur));
  now["name"]      = panelPageName(cur);
#if defined(CARDS_ENABLED)
  if (cur >= fixed && cur < n) now["card"] = cardsName((uint8_t)(cur - fixed));
  now["notify"]    = cardsNotifyActive();
#endif
  now["style"]     = settings.clockStyle;
  now["styleName"] = styleName(settings.clockStyle);
  now["entered"]   = panelEnteredPage();
  now["hz"]        = getOptimalRefreshRate();
  now["off"]       = isDisplayForcedOff() || isDisplayScheduledOff();
  // What the clock page actually shows: the older modes win there, in the order
  // loop() renders them.
  const bool showViz   = httpForceViz && vizShouldDisplay();
  const bool showStats = !showViz && metricData.online && !httpForceClock && !httpForceAmbient;
  now["mode"] = showViz ? "viz" : (showStats ? "metrics" : (ambientActive() ? "ambient" : "clock"));
#if defined(LUA_EFFECTS_ENABLED)
  now["lua"] = luaEffectCurrent();
#endif
  char hm[6] = "--:--";
  const time_t t = time(nullptr);
  if (t > 1700000000) {                      // before NTP the clock reads 1970
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(hm, sizeof(hm), "%02d:%02d", lt.tm_hour, lt.tm_min);
  }
  now["time"] = hm;

  const PanelCarousel &c = panelCarousel();
  JsonObject car = doc["carousel"].to<JsonObject>();
  car["enabled"]   = c.enabled;
  car["idleS"]     = c.idleS;
  car["slotS"]     = c.slotS;
  car["allStyles"] = c.allStyles;
#if defined(CAROUSEL_ENABLED)
  const uint16_t secs  = panelPageSeconds(cur);
  const uint32_t pageS = carouselPageMs() / 1000UL;
  const uint32_t holdS = (carouselHoldMs() + 999UL) / 1000UL;
  car["running"] = carouselRunning();
  car["holdS"]   = holdS;
  car["pageS"]   = pageS;
  car["secs"]    = secs;
  if (c.enabled && secs) {
    // After a hold the page has already had the idle time, so it moves when
    // the hold ends or when its own time is up, whichever is later.
    long left = (long)secs - (long)pageS;
    if (left < (long)holdS) left = (long)holdS;
    car["nextS"] = left;
  }
#endif

  JsonArray pages = doc["pages"].to<JsonArray>();
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t key = panelPageKey(i);
    JsonObject p = pages.add<JsonObject>();
    p["i"]    = i;
    p["key"]  = keyName(key);
    p["name"] = panelPageName(i);
    p["on"]   = panelPageEnabled(key);
#if defined(CARDS_ENABLED)
    if (i >= fixed) {
      p["card"]  = cardsName((uint8_t)(i - fixed));
      p["title"] = cardsTitle((uint8_t)(i - fixed));
    }
#endif
  }
  doc["cardsOn"] = panelPageEnabled(PANEL_KEY_CARDS);

  JsonArray styles = doc["styles"].to<JsonArray>();
  for (uint8_t i = 0; i < CLOCK_STYLE_COUNT; i++) {
    JsonObject s = styles.add<JsonObject>();
    s["id"]   = kClockStyles[i].id;
    s["name"] = kClockStyles[i].name;
  }
}

static void handlePanel() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;

    long showPage = -1;
    JsonVariantConst show = in["show"];
    if (!show.isNull()) {
      if (!intIn(show["page"], 0, (long)panelPageCount() - 1, &showPage)) REJECT(400, "show.page out of range");
      const char *want = show["card"].as<const char *>();
      if (want) {
#if defined(CARDS_ENABLED)
        // Card indexes shift when one expires between reading the list and
        // the click; the name makes sure it is still the card that was meant.
        const uint8_t fixed = fixedPageCount();
        if (showPage < fixed || strcmp(cardsName((uint8_t)(showPage - fixed)), want)) REJECT(409, "that card is gone");
#else
        REJECT(400, "no cards in this build");
#endif
      }
    }

    long styleId = -1;
    if (!in["style"].isNull() && !(intIn(in["style"], 0, 255, &styleId) && styleKnown(styleId)))
      REJECT(400, "unknown clock style");

    int enableKey = -1;
    bool enableOn = true;
    JsonVariantConst en = in["enable"];
    if (!en.isNull()) {
      const char *k = en["key"].as<const char *>();
      for (uint8_t i = 0; k && i < PANEL_KEY_COUNT; i++)
        if (!strcmp(k, KEY_NAMES[i])) enableKey = i;
      if (enableKey < 0) REJECT(400, "enable.key unknown");
#if defined(CARDS_ENABLED)
      const bool built = enableKey == PANEL_KEY_CARDS || pageOf((uint8_t)enableKey) >= 0;
#else
      const bool built = pageOf((uint8_t)enableKey) >= 0;
#endif
      if (!built) REJECT(400, "that page is not in this build");
      if (!en["on"].is<bool>()) REJECT(400, "enable.on must be true or false");
      enableOn = en["on"].as<bool>();
      if (enableKey == PANEL_KEY_CLOCK && !enableOn) REJECT(400, "the clock page cannot be switched off");
    }

    PanelCarousel car = panelCarousel();
    JsonVariantConst jc = in["carousel"];
    const bool haveCar = !jc.isNull();
    if (haveCar) {
#if defined(CAROUSEL_ENABLED)
      long v;
      if (!jc.is<JsonObjectConst>()) REJECT(400, "carousel must be an object");
      if (!optBool(jc["enabled"], &car.enabled)) REJECT(400, "carousel.enabled must be true or false");
      if (!optBool(jc["allStyles"], &car.allStyles)) REJECT(400, "carousel.allStyles must be true or false");
      if (!jc["idleS"].isNull()) {
        if (!intIn(jc["idleS"], PANEL_IDLE_MIN_S, PANEL_IDLE_MAX_S, &v)) REJECT(400, "carousel.idleS out of range");
        car.idleS = (uint16_t)v;
      }
      if (!jc["slotS"].isNull()) {
        if (!intIn(jc["slotS"], 0, PANEL_SLOT_MAX_S, &v) || (v && v < PANEL_SLOT_MIN_S))
          REJECT(400, "carousel.slotS out of range");
        car.slotS = (uint16_t)v;
      }
#else
      REJECT(400, "no carousel in this build");
#endif
    }

    // Everything checked: apply.
    if (enableKey >= 0) panelSetPageEnabled((uint8_t)enableKey, enableOn);
    if (haveCar) panelSetCarousel(car);
    if (styleId >= 0) panelShowStyle((uint8_t)styleId);
    if (showPage >= 0) panelShowPage((uint8_t)showPage);
  }
  JsonDocument doc(&s_alloc);
  buildPanel(doc);
  sendDoc(doc);
}

// ---------------------------------------------------------------- /api/flightboard
#if defined(FLIGHTBOARD_ENABLED)
// A string of at most max bytes, or refused. An absent key gives def.
static bool fbStr(JsonVariantConst v, size_t max, const char **out, const char *def = nullptr) {
  if (v.isNull() && def) { *out = def; return true; }
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  if (!s || strlen(s) > max) return false;
  *out = s;
  return true;
}

static void handleFlightboard() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    // Every key known; at most one change besides the selection.
    int actions = 0;
    for (JsonPairConst kv : in.as<JsonObjectConst>()) {
      const char *k = kv.key().c_str();
      if (!strcmp(k, "airport") || !strcmp(k, "dir")) continue;
      if (strcmp(k, "add") && strcmp(k, "remove") && strcmp(k, "track") && strcmp(k, "untrack") && strcmp(k, "budget")) {
        char why[64];
        snprintf(why, sizeof(why), "%.24s is not a field of /api/flightboard", k);
        REJECT(400, why);
      }
      actions++;
    }
    if (actions > 1) REJECT(400, "send at most one of add, remove, track, untrack, budget");

    long apt = flightboardAirportId();
    uint8_t mode = flightboardDirMode();
    FbAirport known;
    if (!in["airport"].isNull() && (!intIn(in["airport"], 0, 255, &apt) || !flightboardAirportById((uint8_t)apt, &known)))
      REJECT(400, "airport: no airport with that id");
    JsonVariantConst d = in["dir"];
    if (!d.isNull()) {
      const char *s = d.as<const char *>();
      if (!s || (strcmp(s, "arr") && strcmp(s, "dep") && strcmp(s, "alt"))) REJECT(400, "dir must be arr, dep or alt");
      mode = !strcmp(s, "arr") ? FB_DIR_ARR : !strcmp(s, "dep") ? FB_DIR_DEP : FB_DIR_ALT;
    }

#if defined(FLIGHTBOARD_DIRECT_ENABLED)
    JsonVariantConst jAdd = in["add"], jRemove = in["remove"], jTrack = in["track"], jUntrack = in["untrack"],
                     jBudget = in["budget"];
    if (!jAdd.isNull()) {
      if (!jAdd.is<JsonObjectConst>()) REJECT(400, "add must be an object");
      for (JsonPairConst kv : jAdd.as<JsonObjectConst>()) {
        const char *k = kv.key().c_str();
        if (strcmp(k, "icao") && strcmp(k, "iata") && strcmp(k, "name") && strcmp(k, "tz") && strcmp(k, "select"))
          REJECT(400, "add takes icao, iata, name, tz and select only");
      }
      const char *icao, *iata, *name, *tz;
      // Refused rather than folded: the portal already writes these in capitals.
      if (!fbStr(jAdd["icao"], 4, &icao)) REJECT(400, "add.icao must be 4 capitals or digits, starting with a letter");
      if (!fbStr(jAdd["iata"], 3, &iata, "")) REJECT(400, "add.iata must be 3 capitals, or empty");
      if (!fbStr(jAdd["name"], FB_APT_NAME_MAX, &name)) REJECT(400, "add.name must be 1 to 12 characters");
      if (!fbStr(jAdd["tz"], FB_APT_TZ_MAX, &tz, "")) REJECT(400, "add.tz must be an IANA zone name such as Europe/Paris");
      bool select = false;
      if (!optBool(jAdd["select"], &select)) REJECT(400, "add.select must be true or false");
      FbAirport a;
      memset(&a, 0, sizeof(a));
      strncpy(a.icao, icao, sizeof(a.icao) - 1);
      strncpy(a.iata, iata, sizeof(a.iata) - 1);
      strncpy(a.name, name, sizeof(a.name) - 1);
      strncpy(a.tz, tz, sizeof(a.tz) - 1);
      if (const char *why = flightboardAirportCheck(a)) REJECT(400, why);
      uint8_t id;
      if (const char *why = panelAddFlightAirport(a, &id)) REJECT(409, why);
      if (select) apt = id;
    } else if (!jRemove.isNull()) {
      long id;
      if (!intIn(jRemove, FB_APT_CUSTOM, FB_APT_CUSTOM + FB_APT_CUSTOM_MAX - 1, &id) ||
          !flightboardCustomUsed((uint8_t)(id - FB_APT_CUSTOM)))
        REJECT(400, "remove: only a custom airport can be deleted");
      panelRemoveFlightAirport((uint8_t)id);
      if (apt == id) apt = flightboardAirportId();          // the selection went with it
    } else if (!jTrack.isNull()) {
      const char *ident;
      if (!fbStr(jTrack, 16, &ident)) REJECT(400, "track must be a flight ident such as AF7301");
      bool full = false;
      if (const char *why = aeroTrackAdd(ident, &full)) REJECT(full ? 409 : 400, why);
    } else if (!jUntrack.isNull()) {
      const char *ident;
      char id[FB_IDENT_LEN];
      if (!fbStr(jUntrack, 16, &ident) || !aero::normaliseIdent(ident, id, sizeof(id)))
        REJECT(400, "untrack must be a flight ident such as AF7301");
      if (!aeroTrackRemove(id)) REJECT(404, "untrack: that flight is not tracked");
    } else if (!jBudget.isNull()) {
      if (!jBudget.is<JsonObjectConst>()) REJECT(400, "budget must be an object");
      fbs::Budget b = aeroDirectBudget();
      char err[96];
      if (fbs::apply(jBudget.as<JsonObjectConst>(), b, err, sizeof(err))) REJECT(400, err);
      aeroDirectSetBudget(b);                                // in force now, in NVS "fbcfg" 2.5 s later
    }
#else
    if (actions) REJECT(400, "custom airports, tracking and the budget need the direct AeroAPI build");
#endif
    // Tells the MQTT transport itself when the selection really changed.
    if (!panelSetFlightboard((uint8_t)apt, mode)) REJECT(400, "airport: no airport with that id");
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_FLIGHTS);
  doc["airport"] = flightboardAirportId();
  doc["dir"]     = flightboardModeKey();
  doc["side"]    = flightboardShowingDepartures() ? "dep" : "arr";
  doc["altS"]    = FB_ALT_SECONDS;
  JsonArray airports = doc["airports"].to<JsonArray>();
  auto listAirport = [&airports](uint8_t id) {
    FbAirport a;
    if (!flightboardAirportById(id, &a)) return;
    JsonObject o = airports.add<JsonObject>();
    o["id"]   = id;
    o["code"] = (const char *)a.icao;
    o["iata"] = (const char *)a.iata;
    o["name"] = (const char *)a.name;
    o["tz"]   = (const char *)a.tz;
    o["kind"] = flightboardAirportBuiltin(id) ? "builtin" : "custom";
  };
  for (uint8_t i = 0; i < flightboardBuiltinCount(); i++) listAirport(i);
  for (uint8_t i = 0; i < FB_APT_CUSTOM_MAX; i++) listAirport((uint8_t)(FB_APT_CUSTOM + i));
  JsonObject lim = doc["limits"].to<JsonObject>();
  lim["custom"] = FB_APT_CUSTOM_MAX;
  lim["name"]   = FB_APT_NAME_MAX;
  lim["namePx"] = FB_APT_NAME_PX;
  lim["track"]  = FB_TRACK_MAX;
  // The header font's advance for ' ' to '~', so the portal measures a name
  // the way flightboardAirportCheck will.
  JsonArray adv = lim["advance"].to<JsonArray>();
  for (char ch = ' '; ch <= '~'; ch++) {
    const char one[2] = {ch, 0};
    adv.add(flightboardNameWidth(one));
  }
  flightboardStatusJson(doc["board"].to<JsonObject>());
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  aeroDirectStatusJson(doc["direct"].to<JsonObject>());
  JsonArray tracked = doc["tracked"].to<JsonArray>();
  aeroDirectTracksJson(tracked);
  for (uint8_t i = 0; i < tracked.size(); i++) {
    char word[16], hm[6];
    flightboardTrackLine(i, word, sizeof(word), hm, sizeof(hm));
    tracked[i]["w"]  = word;                                 // as the pinned row prints it
    tracked[i]["tm"] = hm;
  }
#else
  doc["direct"]["built"] = false;
#endif
#if defined(FB_MQTT_ENABLED)
  mqttJson(doc["mqtt"].to<JsonObject>(), fbMqttStatus());
#endif
  sendDoc(doc);
}
#endif

// ---------------------------------------------------------------- /api/railboard
#if defined(RAILBOARD_ENABLED)
static void handleRailboard() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    JsonVariantConst diag = in["diag"];
    if (!diag.isNull() && !diag.is<bool>()) REJECT(400, "diag must be true or false");

    // Exactly three capitals, as the panel stores it and as the MQTT topic
    // carries it. Lower case is refused rather than folded: the portal folds
    // it as you type, so anything else reaching here is not the portal.
    char crs[4] = "";
    JsonVariantConst jcrs = in["crs"];
    if (!jcrs.isNull()) {
      const char *s = jcrs.as<const char *>();
      if (!s || !railboardValidStation(s)) REJECT(400, "crs must be three capital letters A-Z");
      memcpy(crs, s, sizeof(crs));
    }

    // Settings, checked against the ones in force by rb_settings.h: an object
    // with any subset of the keys, every key known, every value in range, or
    // a 400 that names the field and nothing changes. Refused, never clamped.
    rbs::Settings next = railboardSettings();
    const bool haveCfg = !in["config"].isNull();
    if (haveCfg) {
      JsonVariantConst cfg = in["config"];
      if (!cfg.is<JsonObjectConst>()) REJECT(400, "config must be an object");
      char err[96];
      if (rbs::apply(cfg.as<JsonObjectConst>(), next, err, sizeof(err))) REJECT(400, err);
    }
    bool reset = false;
    if (!optBool(in["reset"], &reset)) REJECT(400, "reset must be true or false");
    if (reset && haveCfg) REJECT(400, "send config or reset, not both");

    // Kept across reboots by src/panel, which also tells the rail board.
    if (crs[0] && !panelSetRailStation(crs)) REJECT(500, "station refused");
    if (reset)   railboardResetSettings();       // back to Home Assistant's config, else the build's
    if (haveCfg) railboardSetSettings(next);     // in force now, in NVS "rbcfg" 2.5 s later
    if (!diag.isNull()) railboardSetDiag(diag.as<bool>());
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_TRAINS);
  railboardStatusJson(doc.as<JsonObject>());
  mqttJson(doc["mqtt"].to<JsonObject>(), mqttBusStatus());
  sendDoc(doc);
}
#endif

// ---------------------------------------------------------------- /api/worldclock
#if defined(WORLDCLOCK_ENABLED)
// A string of at most max bytes; anything else is refused.
static bool strIn(JsonVariantConst v, size_t max, const char **out) {
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  if (!s || strlen(s) > max) return false;
  *out = s;
  return true;
}

static void handleWorldclock() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    JsonVariantConst jHome = in["home"], jAdd = in["add"], jRemove = in["remove"], jAuto = in["auto"];
    if ((int)!jHome.isNull() + !jAdd.isNull() + !jRemove.isNull() + !jAuto.isNull() != 1)
      REJECT(400, "send exactly one of home, add, remove, auto");
    const uint8_t beforeId = worldClockHome();
    WcCity before, after;
    worldClockCity(beforeId, &before);

    if (!jHome.isNull()) {
      long id;
      WcCity c;
      if (!intIn(jHome, 0, 255, &id) || !worldClockCity((uint8_t)id, &c)) REJECT(400, "home: no city with that id");
      if (const char *why = panelSetWorldHome((uint8_t)id)) REJECT(409, why);
    } else if (!jAdd.isNull()) {
      if (!jAdd.is<JsonObjectConst>()) REJECT(400, "add must be an object");
      WcCity c;
      memset(&c, 0, sizeof(c));
      const char *name, *tz;
      // Refused rather than folded: the portal already writes the name in the
      // panel's capitals, so anything else reaching here is not the portal.
      if (!strIn(jAdd["name"], WC_NAME_MAX, &name)) REJECT(400, "add.name must be text of at most 20 characters");
      // is<float>() holds for any JSON number, integers included (ArduinoJson
      // 7.4.3, Converter<float>::checkJson tests the number bit), and for no string.
      if (!jAdd["lat"].is<float>() || !jAdd["lon"].is<float>()) REJECT(400, "add.lat and add.lon must be numbers");
      if (!strIn(jAdd["tz"], WC_IANA_MAX, &tz)) REJECT(400, "add.tz must be an IANA zone name");
      const char *posix = tzdbPosix(tz);
      if (!posix) REJECT(400, "add.tz is not a zone this panel knows");
      bool makeHome = false;
      if (!optBool(jAdd["home"], &makeHome)) REJECT(400, "add.home must be true or false");
      strncpy(c.name, name, sizeof(c.name) - 1);
      strncpy(c.iana, tz, sizeof(c.iana) - 1);
      strncpy(c.posix, posix, sizeof(c.posix) - 1);
      c.lat = jAdd["lat"].as<float>();
      c.lon = jAdd["lon"].as<float>();
      if (const char *why = worldClockCheck(c)) REJECT(400, why);
      uint8_t id;
      if (const char *why = panelAddWorldCity(c, &id)) REJECT(409, why);
      if (makeHome) panelSetWorldHome(id);          // cannot fail: the city was just added
    } else if (!jRemove.isNull()) {
      long id;
      if (!intIn(jRemove, WC_ID_CUSTOM, WC_ID_CUSTOM + WC_CUSTOM_MAX - 1, &id) ||
          !worldClockSlotUsed((uint8_t)(id - WC_ID_CUSTOM)))
        REJECT(400, "remove: only a custom city can be deleted");
      panelRemoveWorldCity((uint8_t)id);
    } else {
      if (!jAuto.is<bool>() || !jAuto.as<bool>()) REJECT(400, "auto must be true");
      panelForgetWorldHome();
    }

    // The owner asked that a new home show on the panel, name pulsing: so it
    // is put on screen, held like a knob turn, whichever page was up.
    worldClockCity(worldClockHome(), &after);
    if (worldClockHome() != beforeId || strcmp(after.name, before.name)) {
      const int page = pageOf(PANEL_KEY_WORLD);
      if (page >= 0) panelShowPage((uint8_t)page);
    }
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_WORLD);
  worldClockMapJson(doc.as<JsonObject>());
  const time_t t = time(nullptr);
  const bool synced = t > 1700000000;
  doc["utc"] = synced ? (uint32_t)t : 0;   // 0: no NTP yet, the page is all night
  doc["homeChosen"] = worldClockHomeChosen();
  doc["homeSource"] = wcHomeSource();
  if (synced) {
    // The panel's own reading, so the portal shows the time the page draws.
    const int32_t off = worldClockHomeOffset((int64_t)t);
    const int64_t local = (int64_t)t + off;
    char hm[6];
    snprintf(hm, sizeof(hm), "%02d:%02d", (int)((local / 3600) % 24), (int)((local / 60) % 60));
    doc["homeTime"]   = hm;
    doc["homeOffset"] = off;
  }
  doc["pulseMs"] = worldClockPulseLeftMs();
  JsonObject lim = doc["limits"].to<JsonObject>();
  lim["custom"] = WC_CUSTOM_MAX;
  lim["name"]   = WC_NAME_MAX;
  lim["namePx"] = WC_NAME_PX;
  // The page font's advance for ' ' to '~': the portal measures a name the way
  // worldClockCheck will, rather than guessing at a font it does not have.
  JsonArray adv = lim["advance"].to<JsonArray>();
  for (char ch = ' '; ch <= '~'; ch++) {
    const char one[2] = {ch, 0};
    adv.add(worldClockNameWidth(one));
  }
  doc["tzdb"] = tzdbVersion();
  sendDoc(doc);
}
#endif

// ---------------------------------------------------------------- /api/knob
static const char *eventName(uint8_t e) {
  switch (e) {
  case CTRL_CW:    return "cw";
  case CTRL_CCW:   return "ccw";
  case CTRL_PRESS: return "click";
  case CTRL_LONG:  return "long";
  default:         return "";
  }
}

static void knobJson(JsonObject o, const PanelKnob &k) {
  o["reverse"]    = k.reverse;
  o["lockoutMs"]  = k.lockoutMs;
  o["debounceMs"] = k.debounceMs;
  o["detent"]     = k.detent;
}

static void handleKnob() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    PanelKnob k = panelKnob();
    bool defaults = false;
    if (!optBool(in["defaults"], &defaults)) REJECT(400, "defaults must be true or false");
    if (defaults) k = panelKnobDefaults();
    long v;
    if (!optBool(in["reverse"], &k.reverse)) REJECT(400, "reverse must be true or false");
    if (!in["lockoutMs"].isNull()) {
      if (!intIn(in["lockoutMs"], 0, PANEL_LOCKOUT_MAX_MS, &v)) REJECT(400, "lockoutMs out of range");
      k.lockoutMs = (uint16_t)v;
    }
    if (!in["debounceMs"].isNull()) {
      if (!intIn(in["debounceMs"], PANEL_DEBOUNCE_MIN_MS, PANEL_DEBOUNCE_MAX_MS, &v)) REJECT(400, "debounceMs out of range");
      k.debounceMs = (uint16_t)v;
    }
    if (!in["detent"].isNull()) {
      if (!intIn(in["detent"], -1, 1, &v)) REJECT(400, "detent must be -1, 0 or 1");
      k.detent = (int8_t)v;
    }
    if (!panelSetKnob(k)) REJECT(400, "knob settings out of range");
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  knobJson(doc.as<JsonObject>(), panelKnob());
  knobJson(doc["defaults"].to<JsonObject>(), panelKnobDefaults());
  JsonObject b = doc["bounds"].to<JsonObject>();
  b["lockoutMax"]  = PANEL_LOCKOUT_MAX_MS;
  b["debounceMin"] = PANEL_DEBOUNCE_MIN_MS;
  b["debounceMax"] = PANEL_DEBOUNCE_MAX_MS;
  CtrlStats st;
  controlStats(&st);
  JsonObject s = doc["stats"].to<JsonObject>();
  s["cw"]     = st.cw;
  s["ccw"]    = st.ccw;
  s["click"]  = st.press;
  s["long"]   = st.longPress;
  s["last"]   = eventName(st.last);
  if (st.lastMs) s["agoMs"] = millis() - st.lastMs;
  s["detent"] = st.detent;
  s["timer"]  = st.timer;
  s["held"]   = controlHeld();
  sendDoc(doc);
}

// ---------------------------------------------------------------- /api/yachtradar
#if defined(YACHTRADAR_ENABLED)
static void handleYachtradar() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    JsonVariantConst by = in["bySize"];
    if (!by.is<bool>()) REJECT(400, "bySize must be true or false");
    yachtRadarSetSortBySize(by.as<bool>());
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_YACHTS);
  yachtRadarStatusJson(doc.as<JsonObject>());
  sendDoc(doc);
}
#endif

// ---------------------------------------------------------------- /api/lua
#if defined(LUA_EFFECTS_ENABLED)
static void handleLua() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    long i;
    if (!intIn(in["show"], 0, (long)luaEffectCount() - 1, &i)) REJECT(400, "show out of range");
    luaEffectShow((uint8_t)i);
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  doc["current"] = luaEffectCurrent();
  JsonArray list = doc["effects"].to<JsonArray>();
  for (uint8_t i = 0; i < luaEffectCount(); i++) {
    const char *name = luaEffectName(i);
    list.add(name ? name : "");
  }
  sendDoc(doc);
}
#endif

// ---------------------------------------------------------------- /api/clips
#if defined(CLIPS_SD_ENABLED)
static void listClips(JsonArray out) {
  File dir = SD_MMC.open(CLIP_SD_DIR);
  if (!dir) return;
  // getNextFileName opens nothing, so the listing holds one file at a time.
  for (String p = dir.getNextFileName(); p.length(); p = dir.getNextFileName()) {
    if (!p.endsWith(".pca")) continue;
    const String name = p.substring(p.lastIndexOf('/') + 1, p.length() - 4);
    if (!animValidName(name.c_str())) continue;
    File f = SD_MMC.open(String(CLIP_SD_DIR "/") + name + ".pca", FILE_READ);
    PcaHeader hdr;
    const bool ok = animValidatePcaMax(f, &hdr, CLIP_SD_MAX_FRAMES);
    const uint32_t bytes = f ? (uint32_t)f.size() : 0;
    if (f) f.close();
    if (!ok) continue;
    JsonObject o = out.add<JsonObject>();
    o["name"] = name;
    o["bytes"] = bytes;
    o["frames"] = hdr.frameCount;
    o["ms"] = hdr.defaultFrameMs;
  }
  dir.close();
}

static const char *clipName(JsonVariantConst v) {
  return v.is<const char *>() && animValidName(v.as<const char *>()) ? v.as<const char *>() : nullptr;
}

static void handleClips() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    bool mount = false;
    if (!optBool(in["mount"], &mount)) REJECT(400, "mount must be a boolean");
    const bool hasPlay = !in["play"].isNull(), hasDelete = !in["delete"].isNull();
    if (hasPlay + hasDelete + mount != 1) REJECT(400, "one of play, delete or mount");
    if (mount) {
      if (!clipSdMount()) REJECT(503, clipSdReason());
    } else {
      const char *name = clipName(hasPlay ? in["play"] : in["delete"]);
      if (!name) REJECT(400, "name must be 1-24 of A-z 0-9 _ -");
      if (!clipSdMounted()) REJECT(503, clipSdReason());
      const String path = clipSdPath(name);
      if (!SD_MMC.exists(path)) REJECT(404, "no such clip on the card");
      if (hasPlay) {
        File f = SD_MMC.open(path, FILE_READ);
        const bool ok = animValidatePcaMax(f, nullptr, CLIP_SD_MAX_FRAMES);
        if (f) f.close();
        if (!ok) REJECT(422, "not a valid clip");
        snprintf(settings.ambientCustomFile, sizeof(settings.ambientCustomFile), CLIP_SD_REF "%s", name);
        settings.ambientStyle = 6;
        ambientCustomInvalidate();
        httpForceAmbient = true;
        httpForceClock = false;
        httpForceViz = false;
      } else {
        ambientCustomInvalidate();  // the reader may hold this very file open
        const bool gone = SD_MMC.remove(path);
        clipSdRefresh();
        if (!gone) REJECT(500, "the card did not delete the clip");
      }
    }
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  const bool mounted = clipSdMounted();
  JsonObject card = doc["card"].to<JsonObject>();
  card["mounted"] = mounted;
  card["type"] = clipSdCardType();
  card["totalKB"] = (uint32_t)(clipSdTotalBytes() >> 10);
  card["freeKB"] = (uint32_t)(clipSdFreeBytes() >> 10);
  doc["reason"] = clipSdReason();
  const uint64_t room = clipSdFreeBytes() > CLIP_SD_FREE_MARGIN ? clipSdFreeBytes() - CLIP_SD_FREE_MARGIN : 0;
  doc["maxFrames"] = mounted ? CLIP_SD_MAX_FRAMES : 0;
  doc["maxBytes"] = (uint32_t)(room < CLIP_SD_MAX_BYTES ? room : CLIP_SD_MAX_BYTES);
  doc["current"] = settings.ambientCustomFile;
  doc["playing"] = ambientCustomPlaying();
  const ClipSdStats s = clipSdStats();
  static const char *const STATES[] = {"idle", "playing", "failed"};
  JsonObject st = doc["stream"].to<JsonObject>();
  st["state"] = STATES[s.state];
  st["clip"] = s.clip;
  st["frames"] = s.frames;
  st["reads"] = s.reads;
  st["readAvgMs"] = s.readUsAvg / 1000.0f;
  st["readMaxMs"] = s.readUsMax / 1000.0f;
  st["underruns"] = s.underruns;
  st["loops"] = s.loops;
  st["queued"] = s.queued;
  st["stackFree"] = s.stackFree;
  JsonArray clips = doc["clips"].to<JsonArray>();
  if (mounted) listClips(clips);
  sendDoc(doc);
}

// POST /api/clips/upload streams to a temp file on the card with the size cap
// enforced per chunk, validates the finished file, then renames it into place:
// the flash store's upload (web.cpp), with the card's limits.
static File clipUpFile;
static uint32_t clipUpWritten = 0;
static uint32_t clipUpCap = 0;
static String clipUpName;
static const char *clipUpError = "no file in the request";

static void clipUploadAbort(const char *why) {
  if (clipUpFile) clipUpFile.close();
  if (SD_MMC.exists(CLIP_SD_TMP)) SD_MMC.remove(CLIP_SD_TMP);
  clipSdRefresh();
  if (!clipUpError) clipUpError = why;
}

static void handleClipUploadChunk() {
  HTTPUpload &up = server.upload();
  esp_task_wdt_reset();  // a long upload keeps loop() here; same reason as web.cpp's
  if (up.status == UPLOAD_FILE_START) {
    clipUpError = nullptr;
    clipUpWritten = 0;
    clipUpName = server.arg("name");
    if (!clipSdMounted()) { clipUpError = clipSdReason(); return; }
    if (!animValidName(clipUpName.c_str())) { clipUpError = "bad name (use 1-24 of A-z 0-9 _ -)"; return; }
    const uint64_t freeB = clipSdFreeBytes();
    const uint64_t room = freeB > CLIP_SD_FREE_MARGIN ? freeB - CLIP_SD_FREE_MARGIN : 0;
    clipUpCap = (uint32_t)(room < CLIP_SD_MAX_BYTES ? room : CLIP_SD_MAX_BYTES);
    if (server.hasArg("bytes")) {  // told the size: refuse before writing a byte
      const long want = server.arg("bytes").toInt();
      if (want <= 0 || (unsigned long)want > CLIP_SD_MAX_BYTES) { clipUpError = "longer than a card clip may be"; return; }
      if ((uint32_t)want > clipUpCap) { clipUpError = "not enough free space on the card"; return; }
      clipUpCap = (uint32_t)want;
    }
    if (clipUpCap < PCA_HEADER_BYTES + PCA_FRAME_BYTES) { clipUpError = "not enough free space on the card"; return; }
    ambientCustomInvalidate();  // lets go of a card clip the reader holds
    clipUpFile = SD_MMC.open(CLIP_SD_TMP, FILE_WRITE);
    if (!clipUpFile) clipUpError = "cannot open a temp file on the card";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (clipUpError) return;
    if (clipUpWritten + up.currentSize > clipUpCap) { clipUploadAbort("file too large"); return; }
    if (clipUpFile.write(up.buf, up.currentSize) != up.currentSize) { clipUploadAbort("write failed (card full?)"); return; }
    clipUpWritten += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (clipUpError) return;
    clipUpFile.close();
    File f = SD_MMC.open(CLIP_SD_TMP, FILE_READ);
    const bool ok = animValidatePcaMax(f, nullptr, CLIP_SD_MAX_FRAMES);
    if (f) f.close();
    if (!ok) { clipUploadAbort("not a valid .pca clip"); return; }
    const String target = clipSdPath(clipUpName.c_str());
    // Whether FAT's rename replaces an existing file is not verified here, so
    // the old clip goes first; if the rename then fails, it is gone.
    if (SD_MMC.exists(target)) SD_MMC.remove(target);
    if (!SD_MMC.rename(CLIP_SD_TMP, target)) clipUploadAbort("rename failed");
    clipSdRefresh();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    clipUploadAbort("upload aborted");
  }
}

static void handleClipUploadDone() {
  JsonDocument doc(&s_alloc);
  doc["success"] = clipUpError == nullptr;
  if (clipUpError) doc["error"] = clipUpError;
  else doc["name"] = clipUpName;
  sendDoc(doc, clipUpError ? 400 : 200);
  clipUpError = "no file in the request";  // until the next upload starts
}

static void handleClipFrame() {
  const String name = server.arg("name");
  const long i = server.arg("i").toInt();
  if (!clipSdMounted()) REJECT(503, clipSdReason());
  if (!animValidName(name.c_str())) REJECT(400, "name must be 1-24 of A-z 0-9 _ -");
  File f = SD_MMC.open(clipSdPath(name.c_str()), FILE_READ);
  PcaHeader hdr;
  const bool ok = animValidatePcaMax(f, &hdr, CLIP_SD_MAX_FRAMES);
  const size_t head = ok ? PCA_HEADER_BYTES + (size_t)hdr.paletteLen * 2 : 0;
  // PSRAM for the 4 KB, per request, like the documents here.
  uint8_t *buf = ok ? (uint8_t *)heap_caps_malloc(head + PCA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  const bool read = buf && i >= 0 && i < hdr.frameCount && f.seek(0) && f.read(buf, head) == head &&
                    f.seek(head + (uint32_t)hdr.frameCount * 2 + (uint32_t)i * PCA_FRAME_BYTES) &&
                    f.read(buf + head, PCA_FRAME_BYTES) == PCA_FRAME_BYTES;
  if (f) f.close();
  if (!ok) REJECT(404, "no such clip on the card");
  if (!buf) { failOom(); return; }
  if (read) {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/octet-stream", (PGM_P)buf, head + PCA_FRAME_BYTES);
  }
  heap_caps_free(buf);
  if (!read) REJECT(400, "i out of range, or the card did not read");
}
#endif

static void route(const char *uri, WebServer::THandlerFunction fn) {
  server.on(uri, HTTP_GET, fn);
  server.on(uri, HTTP_POST, fn);
}

void panelWebBegin() {
  // WebServer keeps only the request headers it was told to collect, and
  // readBody() needs this one.
  static const char *kHeaders[] = {"Content-Type"};
  server.collectHeaders(kHeaders, 1);
  route("/api/panel", handlePanel);
  route("/api/knob", handleKnob);
#if defined(FLIGHTBOARD_ENABLED)
  route("/api/flightboard", handleFlightboard);
#endif
#if defined(RAILBOARD_ENABLED)
  route("/api/railboard", handleRailboard);
#endif
#if defined(WORLDCLOCK_ENABLED)
  route("/api/worldclock", handleWorldclock);
#endif
#if defined(YACHTRADAR_ENABLED)
  route("/api/yachtradar", handleYachtradar);
#endif
#if defined(LUA_EFFECTS_ENABLED)
  route("/api/lua", handleLua);
#endif
#if defined(CLIPS_SD_ENABLED)
  route("/api/clips", handleClips);
  server.on("/api/clips/upload", HTTP_POST, handleClipUploadDone, handleClipUploadChunk);
  server.on("/api/clips/frame", HTTP_GET, handleClipFrame);
#endif
}

#endif  // CONTROL_ENCODER_ENABLED

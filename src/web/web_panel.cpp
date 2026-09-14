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
//   GET  /api/flightboard   airports, airport, dir, board, mqtt
//   POST /api/flightboard   {"airport":i,"dir":"arr"|"dep"}
//   GET  /api/railboard     station, lists, Home Assistant status, config, mqtt
//   POST /api/railboard     {"diag":b} | {"config":{"panels":1..2,"rows":1..3,
//                           "font":"small"|"large","level":10..100,"switch_s":3..600,
//                           "stale_s":30..3600}}
//   GET  /api/worldclock    mask, cities, home, utc
//   POST /api/worldclock    {"home":i}
//   GET  /api/knob          settings, defaults, bounds, stats
//   POST /api/knob          {"reverse":b,"lockoutMs":n,"debounceMs":n,"detent":-1..1}
//                           | {"defaults":true}
//   GET  /api/yachtradar    keyPresent, stream, vessels
//   POST /api/yachtradar    {"bySize":b}
//   GET  /api/lua           effects, current            (LUA_EFFECTS_ENABLED only)
//   POST /api/lua           {"show":i}
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
#include "../flightboard/fb_mqtt.h"
#include "../flightboard/flightboard.h"
#include "../mqtt/mqtt_bus.h"
#include "../panel/panel.h"
#include "../railboard/railboard.h"
#include "../utils/utils.h"
#include "../viz/visualizer.h"
#include "../worldclock/worldclock.h"
#include "../yachtradar/yachtradar.h"
#if defined(LUA_EFFECTS_ENABLED)
#include "../lua/lua_effects.h"
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
  "clock", "world", "flights", "trains", "yachts", "cards"};

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
static void handleFlightboard() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    long apt = flightboardAirportIndex();
    bool dep = flightboardDeparturesSelected();
    if (!in["airport"].isNull() && !intIn(in["airport"], 0, (long)flightboardAirportCount() - 1, &apt))
      REJECT(400, "airport out of range");
    JsonVariantConst d = in["dir"];
    if (!d.isNull()) {
      const char *s = d.as<const char *>();
      if (!s || (strcmp(s, "arr") && strcmp(s, "dep"))) REJECT(400, "dir must be arr or dep");
      dep = !strcmp(s, "dep");
    }
    panelSetFlightboard((uint8_t)apt, dep);
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_FLIGHTS);
  doc["airport"] = flightboardAirportIndex();
  doc["dir"]     = flightboardDirection();
  JsonArray airports = doc["airports"].to<JsonArray>();
  for (uint8_t i = 0; i < flightboardAirportCount(); i++) {
    JsonObject a = airports.add<JsonObject>();
    a["code"] = flightboardAirportCode(i);
    a["name"] = flightboardAirportLabel(i);
  }
  flightboardStatusJson(doc["board"].to<JsonObject>());
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

    char cfgJson[160] = "";
    JsonVariantConst cfg = in["config"];
    if (!cfg.isNull()) {
      if (!cfg.is<JsonObjectConst>()) REJECT(400, "config must be an object");
      // Ranges from the .../config table in src/railboard/README.md, which
      // railboard.cpp clamps to. Refused here rather than clamped there, so a
      // wrong number is an error the portal can show.
      JsonDocument out(&s_alloc);
      out["v"] = 1;
      static const struct { const char *key; long lo, hi; } R[] = {
        {"panels", 1, 2}, {"rows", 1, 3}, {"level", 10, 100}, {"switch_s", 3, 600}, {"stale_s", 30, 3600}};
      for (const auto &r : R) {
        if (cfg[r.key].isNull()) continue;
        long v;
        if (!intIn(cfg[r.key], r.lo, r.hi, &v)) REJECT(400, "config value out of range");
        out[r.key] = v;
      }
      JsonVariantConst font = cfg["font"];
      if (!font.isNull()) {
        const char *s = font.as<const char *>();
        if (!s || (strcmp(s, "small") && strcmp(s, "large"))) REJECT(400, "config.font must be small or large");
        out["font"] = s;
      }
      if (measureJson(out) >= sizeof(cfgJson)) REJECT(400, "config too large");
      serializeJson(out, cfgJson, sizeof(cfgJson));
    }

    if (cfgJson[0] && !railboardApplyConfig(cfgJson, (uint16_t)strlen(cfgJson))) REJECT(500, "config refused");
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
static void handleWorldclock() {
  if (isPost()) {
    JsonDocument in(&s_alloc);
    if (!readBody(in)) return;
    long home;
    if (!intIn(in["home"], 0, (long)worldClockCityCount() - 1, &home)) REJECT(400, "home out of range");
    panelSetWorldHome((uint8_t)home);
  }
  JsonDocument doc(&s_alloc);
  doc["success"] = true;
  pageInfo(doc, PANEL_KEY_WORLD);
  worldClockMapJson(doc.as<JsonObject>());
  const time_t t = time(nullptr);
  doc["utc"] = t > 1700000000 ? (uint32_t)t : 0;   // 0: no NTP yet, the page is all night
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

static void route(const char *uri, WebServer::THandlerFunction fn) {
  server.on(uri, HTTP_GET, fn);
  server.on(uri, HTTP_POST, fn);
}

void panelWebBegin() {
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
}

#endif  // CONTROL_ENCODER_ENABLED

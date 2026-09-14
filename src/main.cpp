/*
 * AnimatedPixelClock - Main Entry Point
 *
 * ESP32-S3 with 128x64 HUB75 RGB matrix (2x 64x64 panels chained)
 * Dual-mode: PC monitoring metrics OR animated clock displays
 */

// ========== User Configuration ==========
// Edit src/config/user_config.h to configure WiFi and device options
#include "config/user_config.h"

#include <Adafruit_GFX.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "display/matrix_display.h" // HUB75 RGB matrix (ESP32-S3)
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <time.h>


#include "config/config.h"
#include "utils/utils.h"
#include "timezones.h"
#if defined(CLIPS_SD_ENABLED)
#include "clips/clip_sd.h"
#endif

// ========== External Objects ==========
extern WiFiUDP udp;              // Defined in network.cpp
extern WebServer server;         // Defined in web.cpp
extern Preferences preferences;  // Defined in settings.cpp

// ========== Display Object ==========
// HUB75 RGB matrix (128x64). The shim adds the OLED-era clearDisplay()/
// display() frame methods so the animation code runs unchanged.
// DISPLAY_WHITE/DISPLAY_BLACK come from display.h (centralized).
MatrixDisplay display(makeMatrixConfig());

// ========== Global State ==========
Settings settings;
MetricData metricData;
bool displayAvailable = false;
bool ntpSynced = false;
unsigned long lastNtpSyncTime = 0;
unsigned long lastReceived = 0;
unsigned long wifiDisconnectTime = 0;
unsigned long nextDisplayUpdate = 0;
bool wifiConnected = false;  // WiFi connection status for icon display
bool httpForceClock = false;  // HTTP override to force clock mode (via /api/mode/clock)
bool httpForceAmbient = false;  // HTTP override to force the ambient screen (via /api/mode/ambient)
#if defined(FLIGHTBOARD_ENABLED)
bool httpForceFlightboard = false;  // flight board page override
#endif
#if defined(YACHTRADAR_ENABLED)
bool httpForceYachtRadar = false;   // yacht radar page override
#endif
#if defined(LUA_EFFECTS_ENABLED)
#include "lua/lua_effects_page.h"    // LUA_EFFECT_COUNT, which the page enum below needs
#endif
#if defined(CONTROL_ENCODER_ENABLED)
// Pages the knob cycles through, in the order a long press walks them. The
// clock is first because it is what the panel should fall back to.
enum CtrlPage : uint8_t {
  PAGE_CLOCK = 0,
#if defined(LUA_EFFECTS_ENABLED)
  // Each Lua effect is a page of its own, straight after the clock styles, so
  // the knob and the carousel walk them one at a time as they walk the styles.
  PAGE_LUA_FIRST,
  PAGE_LUA_LAST = PAGE_LUA_FIRST + LUA_EFFECT_COUNT - 1,
#endif
#if defined(WORLDCLOCK_ENABLED)
  PAGE_WORLDCLOCK,               // a clock too, so it sits next to the clock
#endif
#if defined(FLIGHTBOARD_ENABLED)
  PAGE_FLIGHTBOARD,
#endif
#if defined(RAILBOARD_ENABLED)
  PAGE_RAILBOARD,                // the other board, so the two sit together
#endif
#if defined(YACHTRADAR_ENABLED)
  PAGE_YACHTRADAR,
#endif
  PAGE_COUNT
};
static uint8_t ctrlPage = PAGE_CLOCK;
#if defined(LUA_EFFECTS_ENABLED)
// The effect a page shows, or -1 if it is not an effect page.
static inline int16_t ctrlLuaEffect(uint8_t page) {
  return (page >= PAGE_LUA_FIRST && page <= PAGE_LUA_LAST) ? (int16_t)(page - PAGE_LUA_FIRST) : -1;
}
#endif

#endif
bool httpForceViz = false;  // HTTP override to force the audio visualizer (via /api/mode/viz)

// ========== Forward Declarations ==========
// Redundant forward declarations removed (covered by headers)
void displayStats();
void displayStatsCompactGrid();
void displayMetricCompact(Metric *m);
void drawProgressBar(int x, int y, int width, Metric *m);
int getOptimalRefreshRate();
// ========== Module Includes ==========
#include "ambient/ambient.h"
#include "ambient/anim_store.h"
#include "display/display.h"
#include "clocks/clocks.h"
#include "clocks/clock_globals.h"
#include "metrics/metrics.h"
#include "flightboard/flightboard.h"
#include "flightboard/fb_mqtt.h"
#include "flightboard/aero_direct.h"
#include "mqtt/mqtt_bus.h"
#include "cards/cards.h"
#include "control/carousel.h"
#include "panel/panel.h"
#include "worldclock/worldclock.h"
#include "lua/nslua_bench.h"
#include "railboard/railboard.h"
#include "network/tls_psram.h"

#if defined(CAROUSEL_ENABLED) && defined(CONTROL_ENCODER_ENABLED)
// How long each page holds the screen when the panel is cycling on its own.
// The clock gets the longest turn because it is the one you glance at; the data
// pages are there to be noticed, not studied.
//
// The slot is a run-time setting (src/panel). Its default is CAROUSEL_SLOT_S
// with CAROUSEL_ALL_STYLES and 0 without, where 0 keeps the times below - so
// both builds behave as they did until someone changes it in the portal.
static uint16_t ctrlPageSeconds(uint8_t page) {
  const uint16_t slot = panelCarousel().slotS;
#if defined(CARDS_ENABLED)
  if (page >= PAGE_COUNT) {
    const uint16_t own = cardsDuration((uint8_t)(page - PAGE_COUNT));
    return own ? own : (slot ? slot : 10);   // a card may still ask for its own time
  }
#endif
  if (slot) return slot;                     // every page, and every clock style, alike
#if defined(WORLDCLOCK_ENABLED)
  if (page == PAGE_WORLDCLOCK) return 20;
#endif
#if defined(RAILBOARD_ENABLED)
  if (page == PAGE_RAILBOARD) return 20;   // one list at a time: both get a turn at the default 10 s
#endif
  return (page == PAGE_CLOCK) ? 25 : 15;
}
#endif

#if defined(CONTROL_ENCODER_ENABLED)
// Cards are pages too, but they come and go while the panel is running, so they
// live past the fixed ones rather than in the enum: page PAGE_COUNT + n is the
// n-th live card. Defined here rather than beside the enum because it needs
// cardsCount(), and the module includes come after the declarations.
static inline uint8_t ctrlPageCount() {
#if defined(CARDS_ENABLED)
  return (uint8_t)(PAGE_COUNT + cardsCount());
#else
  return PAGE_COUNT;
#endif
}
#endif
#include "yachtradar/yachtradar.h"
#include "control/control.h"
#include "control/clock_style.h"
#include "lua/nslua.h"
#include "lua/nslua_bindings.h"
#include "network/network.h"
#include "notify/notify.h"
#include "viz/visualizer.h"
#include "weather/weather.h"
#include "web/web.h"


// ========== Helper Functions ==========

// One non-blocking read of the clock. getLocalTime(info, 0) cannot do this: it
// stamps millis() then loops while elapsed <= budget, so a tick landing in
// between skips every attempt and reports failure with the time available.
bool peekLocalTime(struct tm *info) {
  time_t now;
  time(&now);
  localtime_r(&now, info);
  return info->tm_year > 120;
}

// Helper function to get time with short timeout
bool getTimeWithTimeout(struct tm *timeinfo, unsigned long timeout_ms) {
  if (!ntpSynced) {
    if (getLocalTime(timeinfo, timeout_ms)) {
      // Verify time is reasonable (year > 2020) before accepting
      if (timeinfo->tm_year > 120) { // tm_year is years since 1900
        ntpSynced = true;
        lastNtpSyncTime = millis();
        Serial.println("NTP successfully synchronized");
        return true;
      }
    }
    return false;
  }
  return getLocalTime(timeinfo, timeout_ms);
}

// Returns optimal refresh rate in Hz based on current display mode
int getOptimalRefreshRate() {
#if defined(YACHTRADAR_ENABLED)
  // Vessels crawl, but the page is a map and a stutter reads as a fault.
  if (httpForceYachtRadar) return 10;
#endif
#if defined(WORLDCLOCK_ENABLED) && defined(CONTROL_ENCODER_ENABLED)
  // The map changes once a minute; only the breathing home dot needs frames.
  if (ctrlPage == PAGE_WORLDCLOCK) return 10;
#endif
#if defined(LUA_EFFECTS_ENABLED)
  // The effect's own frame cap, lowered to what its task actually delivers.
  if (luaEffectCurrent() >= 0) return luaEffectsRefreshHz();
#endif
#if defined(RAILBOARD_ENABLED)
  // The header clock shows seconds; the data itself changes every 20 s.
  if (ctrlPage == PAGE_RAILBOARD) return 5;
#endif
#if defined(FLIGHTBOARD_ENABLED)
  // A board that changes twice a minute; anything faster is wasted DMA.
  if (httpForceFlightboard) return 5;
#endif
  // Always adaptive. The manual fixed-Hz override (and its web control) was
  // removed - a user-pinned low rate only made animations choppy. The adaptive
  // rates below are what keep motion smooth.

  // A notification banner may scroll over any screen - keep it silky.
  if (notifyActive()) {
    return 60;
  }

  // Audio visualizer: 60 Hz render smooths the ~25 Hz packet stream.
  if (httpForceViz && vizShouldDisplay()) {
    return 60;
  }

  if (!metricData.online || httpForceClock || httpForceAmbient) {
    // Clock mode (offline OR forced via HTTP)

    // Ambient effects (Space Invaders, starfield...) redraw the FULL panel every frame.
    // 30 Hz, not 60: the effects look identical, the per-frame pixel load
    // halves, and the buffer-flip rate stops beating against the panel's
    // DMA scan (visible as rolling dim bands on full-screen content).
    if (ambientActive()) {
      return 30;
    }

    // Boost to 60 Hz during active motion for silky-smooth animation (always on;
    // the old opt-out checkbox was removed).
    if (isAnimationActive()) {
      return 60;
    }

    int rate;
    if (settings.clockStyle == 0 || settings.clockStyle == 3 ||
        settings.clockStyle == 4 || settings.clockStyle == 5 ||
        settings.clockStyle == 6 || settings.clockStyle == 7 ||
        settings.clockStyle == 8 || settings.clockStyle == 9 ||
        settings.clockStyle == 10 || settings.clockStyle == 11 ||
        settings.clockStyle == 12 || settings.clockStyle == 14 || settings.clockStyle == 15 || settings.clockStyle == 16) {
      // Animated clocks (Mario, Space Invaders, Space Ship, Pong, Pac-Man, Snake, Tetris, Cycle, Asteroids, Dino, Matrix, Weather)
      rate = 20; // 20 Hz keeps character movement smooth
    } else {
      // Static clocks (Standard, Large)
      rate = 2; // 2 Hz is plenty for clock that updates once/second
    }
    return rate;
  } else {
    // Metrics mode (online)
    return 10; // 10 Hz for PC stats (updates every 500ms from Python)
  }
}

// Rotation uses elapsed time, independent of wall-clock adjustments.
#include "clocks/cycle_config.h"
void cycleClockScreens() {
  static char previous[128] = "";
  static CycleEntry entries[CYCLE_COUNT];
  static unsigned index = 0;
  static uint32_t started = 0, lastRendered = 0;
  uint32_t now = millis();
  if (strcmp(previous, settings.cycleConfig) || now - lastRendered > 2000) {
    if (!parseCycleConfig(settings.cycleConfig, entries)) parseCycleConfig(CYCLE_DEFAULT, entries);
    strcpy(previous, settings.cycleConfig);
    index = 0; started = now;
  }
  lastRendered = now;
  if (entries[index].seconds && now - started >= entries[index].seconds * 1000UL) {
    index = (index + 1) % CYCLE_COUNT; started = now;
  }
  for (unsigned n = 0; n < CYCLE_COUNT; ++n) {
    if (entries[index].seconds && (entries[index].style != 14 || weatherConfigured())) break;
    index = (index + 1) % CYCLE_COUNT; started = now;
  }
  static int lastStyle = -1;
  if (lastStyle != entries[index].style) {
    resetClockAnimationState(); lastStyle = entries[index].style;
  }
  switch (entries[index].style) {
    case 0: displayClockWithMario(); break;
    case 1: displayStandardClock(); break;
    case 2: displayLargeClock(); break;
    case 3: displayClockWithSpaceInvader(); break;
    case 5: displayClockWithPong(); break;
    case 6: displayClockWithPacman(); break;
    case 7: displayClockWithSnake(); break;
    case 8: displayClockWithTetris(); break;
    case 10: displayClockWithAsteroids(); break;
    case 11: displayClockWithDino(); break;
    case 12: displayClockWithMatrixRain(); break;
    case 14: displayClockWithWeather(); break;
    case 15: displayClockWithBomberman(); break;
    case 16: displayClockWithTron(); break;
  }
}

// ========== setup() ==========

// MEM_TRACE: internal heap after each step of setup(), to find what eats it.
// Build with PLATFORMIO_BUILD_FLAGS=-DMEM_TRACE; never in a release.
#if defined(MEM_TRACE)
#include <esp_heap_caps.h>
#define MEMTRACE(tag) Serial.printf("[mem] %-16s internal %6u largest %6u psram %8u\n", tag, \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM))
#else
#define MEMTRACE(tag)
#endif

void setup() {
#if defined(BOARD_HAS_PSRAM)
  tlsUsePsram();   // before anything opens TLS - see network/tls_psram.cpp
#endif
  Serial.begin(115200);
  delay(1000);

  // Load settings from flash
  loadSettings();
  MEMTRACE("settings");

  // Mount the animation filesystem (formats the partition on first use)
  animStoreInit();
  MEMTRACE("animstore");
#if defined(CLIPS_SD_ENABLED)
  // The clip gallery on the TF card; without a card it says so and carries on.
  clipSdInit();
  MEMTRACE("clip sd");
#endif

  // Initialize display
  displayAvailable = initDisplay();
  MEMTRACE("display");
#if defined(MEM_TRACE)
  Serial.printf("[mem] display refresh %d Hz, DMA buffer in %s\n", display.refreshRateHz(),
#if defined(SPIRAM_DMA_BUFFER)
                "PSRAM");
#else
                "internal SRAM");
#endif
#endif

  // Apply saved brightness setting
  if (displayAvailable) {
    applyDisplayBrightness();
  }

  if (!displayAvailable) {
    Serial.println("WARNING: Display not available, continuing without display");
  } else {
    display.clearDisplay();
    display.setTextColor(DISPLAY_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.println("PIXEL CLOCK");
    display.setCursor(10, 35);
    display.println("INSERT COIN");
    display.display();
  }

  // Check if hardcoded WiFi credentials are provided
  bool useManualWiFi = (strlen(HARDCODED_WIFI_SSID) > 0);

  if (useManualWiFi) {
    Serial.println("\n*** USING HARDCODED WIFI CREDENTIALS ***");
    if (!connectManualWiFi(HARDCODED_WIFI_SSID, HARDCODED_WIFI_PASSWORD)) {
      Serial.println("Manual WiFi connection failed!");
      Serial.println("Falling back to WiFiManager portal...");
      useManualWiFi = false;
    }
  }

  if (!useManualWiFi) {
    initNetwork();
  MEMTRACE("network");
  }

  // Keep the radio awake: WiFi modem sleep delays inbound ACKs to the beacon
  // interval, which stalls large web-page transfers for seconds. Power draw
  // is irrelevant next to the LED matrix.
  WiFi.setSleep(false);

  // Initialize NTP
  initNTP();
  MEMTRACE("ntp");

  // Apply the scheduled dim/off level now that the time is (usually) synced, so
  // the panel comes up at the correct night brightness instead of the un-dimmed
  // boot value. The loop's checkScheduledBrightness() covers the case where NTP
  // was not yet ready here.
  if (displayAvailable) {
    refreshDisplayBrightnessNow();
  }

  // Initialize WiFi connection status flag
  wifiConnected = (WiFi.status() == WL_CONNECTED);

#if defined(CONTROL_ENCODER_ENABLED)
  // Run-time panel settings from NVS: the knob's feel, the carousel, the flight
  // board selection, the world clock's home and the rail board's station.
  // Before controlBegin(), fbMqttBegin() and railboardBegin(), which subscribe
  // to whatever airport and station are selected by then.
  panelBegin();
  MEMTRACE("panel");
  controlBegin();
  MEMTRACE("control");
#endif
#if defined(NSLUA_ENABLED)
  // Phase 1: prove the runtime exists on this board and that the PSRAM
  // allocator works, before anything is built on top of it. Stateless by
  // design - a persistent state belongs to the render path and comes later.
  if (nslua_begin()) {
    char err[128];
    const bool ok = nslua_run("local t={} for i=1,10 do t[i]=i*i end "
                              "return #t == 10 and t[10] == 100", err, sizeof(err));
    Serial.printf("[nslua] self-test %s%s%s\n",
                  ok ? "PASSED" : "FAILED", ok ? "" : ": ", ok ? "" : err);
    nslua_bindings_dump();
#if defined(LUA_EFFECTS_ENABLED)
    luaEffectsBegin();           // the effect task on core 0 and its frame buffers
  MEMTRACE("lua effects");
#endif
#if defined(NSLUA_BENCH)
    nsluaBenchBegin();           // phase 6b bench build only
#endif
  } else {
    Serial.println("[nslua] runtime unavailable (no PSRAM?)");
  }
#endif
#if defined(MQTT_BUS_ENABLED)
  mqttBusBegin();
  MEMTRACE("mqtt");
#endif
#if defined(FLIGHTBOARD_DIRECT_ENABLED)
  // Before fbMqttBegin(): with a key stored, the MQTT transport never subscribes.
  // After panelBegin(), which restored the custom airports and the selection.
  aeroDirectBegin();
  MEMTRACE("aero");
#endif
#if defined(FLIGHTBOARD_ENABLED) && defined(FB_MQTT_ENABLED)
  fbMqttBegin();
  MEMTRACE("fb mqtt");
#endif
#if defined(CARDS_ENABLED)
  cardsBegin();
  MEMTRACE("cards");
#endif
#if defined(RAILBOARD_ENABLED)
  // Subscribes now, whatever page is up: the retained boards arrive within a
  // moment of the broker connecting, so the page is populated after a reboot
  // before anyone turns to it.
  railboardBegin();
  MEMTRACE("railboard");
#if defined(RAILBOARD_BOOT_PAGE)
  ctrlPage = PAGE_RAILBOARD;       // a panel that is a station board first
#endif
#endif

  // Configure hardware watchdog timer
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  // Initialize metricData
  metricData.count = 0;
  metricData.online = false;
  metricData.status = 0;  // No status received yet
  Serial.println("Waiting for PC stats data...");

  // Setup web server
  setupWebServer();
  MEMTRACE("web");

  // Background weather fetcher (idles cheaply while weather is disabled)
  MEMTRACE("weather");

  // Show IP address for 5 seconds (configurable via web interface)
  if (displayAvailable && settings.showIPAtBoot) {
    displayConnected();
    delay(5000);
  }
}

#if defined(CONTROL_ENCODER_ENABLED)
// ---------------------------------------------------------------- the knob
// Rotation browses everything in one line - each clock style, then each page,
// then the cards. A click selects: on a page with controls of its own it
// enters, rotation then acts inside, and another click comes back out.
static bool     ctrlEntered = false;
static uint32_t ctrlLastEventMs = 0;
static const uint32_t CTRL_ENTER_TIMEOUT_MS = 30000;   // walk away and it browses again

static bool ctrlPageHasControls(uint8_t page) {
#if defined(FLIGHTBOARD_ENABLED)
  if (page == PAGE_FLIGHTBOARD) return true;
#endif
#if defined(YACHTRADAR_ENABLED)
  if (page == PAGE_YACHTRADAR) return true;
#endif
#if defined(RAILBOARD_ENABLED)
  if (page == PAGE_RAILBOARD) return true;
#endif
  (void)page;
  return false;
}

static const char *ctrlPageName(uint8_t page) {
  if (page >= PAGE_COUNT) return "CARD";
#if defined(LUA_EFFECTS_ENABLED)
  if (ctrlLuaEffect(page) >= 0) return luaEffectName((uint8_t)ctrlLuaEffect(page));
#endif
#if defined(WORLDCLOCK_ENABLED)
  if (page == PAGE_WORLDCLOCK) return "WORLD CLOCK";
#endif
#if defined(FLIGHTBOARD_ENABLED)
  if (page == PAGE_FLIGHTBOARD) return "FLIGHTS";
#endif
#if defined(RAILBOARD_ENABLED)
  if (page == PAGE_RAILBOARD) return "TRAINS";
#endif
#if defined(YACHTRADAR_ENABLED)
  if (page == PAGE_YACHTRADAR) return "YACHTS";
#endif
  return "CLOCK";
}

static const char *ctrlEnterHint(uint8_t page) {
#if defined(FLIGHTBOARD_ENABLED)
  if (page == PAGE_FLIGHTBOARD) return "TURN: AIRPORT";
#endif
#if defined(YACHTRADAR_ENABLED)
  if (page == PAGE_YACHTRADAR) return "TURN: SCROLL";
#endif
#if defined(RAILBOARD_ENABLED)
  if (page == PAGE_RAILBOARD) return "TURN: LISTS";
#endif
  (void)page;
  return "";
}

// Stable keys for the portal's per-page switches (src/panel/panel.h). A page
// added to CtrlPage without a case here is PANEL_KEY_NONE: always visited.
uint8_t panelPageKey(uint8_t page) {
  if (page >= PAGE_COUNT) return PANEL_KEY_CARDS;
#if defined(LUA_EFFECTS_ENABLED)
  // The effects are a range of pages, not one case: one switch for all of them.
  // Found when the two helpers' branches met: without it they reported "other"
  // and could not be switched off.
  if (ctrlLuaEffect(page) >= 0) return PANEL_KEY_LUA;
#endif
  switch (page) {
  case PAGE_CLOCK:       return PANEL_KEY_CLOCK;
#if defined(WORLDCLOCK_ENABLED)
  case PAGE_WORLDCLOCK:  return PANEL_KEY_WORLD;
#endif
#if defined(FLIGHTBOARD_ENABLED)
  case PAGE_FLIGHTBOARD: return PANEL_KEY_FLIGHTS;
#endif
#if defined(RAILBOARD_ENABLED)
  case PAGE_RAILBOARD:   return PANEL_KEY_TRAINS;
#endif
#if defined(YACHTRADAR_ENABLED)
  case PAGE_YACHTRADAR:  return PANEL_KEY_YACHTS;
#endif
  default:               return PANEL_KEY_NONE;
  }
}

// The next page the knob and the carousel may land on, skipping the ones
// switched off in the portal. The clock cannot be switched off, so the walk
// always ends within one lap.
static uint8_t ctrlNextVisited(uint8_t from, int8_t d) {
  const int n = ctrlPageCount();
  if (!n) return from;
  int p = from;
  for (int k = 0; k < n; k++) {
    p = (p + (d > 0 ? 1 : -1) + n) % n;
    if (panelPageEnabled(panelPageKey((uint8_t)p))) break;
  }
  return (uint8_t)p;
}

static void ctrlBrowse(int8_t d) {
  if (ctrlPage == PAGE_CLOCK && clockStyleBrowse(d)) return;   // next style, same page
  if (!ctrlPageCount()) return;
  // A mode forced from the web - an uploaded clip via /api/anim/play, or
  // /api/mode/ambient|clock|viz - held the screen whatever the knob chose:
  // on the panel, 2026-09-14, a clip could not be left. Choosing a page
  // releases it.
  httpForceAmbient = httpForceClock = httpForceViz = false;
  ctrlPage = ctrlNextVisited(ctrlPage, d);
  if (ctrlPage == PAGE_CLOCK) clockStyleBrowseEnter(d);
  else ctrlToast(ctrlPageName(ctrlPage));
}

#if defined(FLIGHTBOARD_ENABLED)
// Inside the flight board the knob walks the airports. Arrivals and departures
// take turns on their own every FB_ALT_SECONDS, so the knob no longer has to.
static char fbToast[32];
static void fbKnob(int8_t d) {
  flightboardStepAirport(d > 0 ? +1 : -1);
#if defined(FB_MQTT_ENABLED)
  fbMqttSelectionChanged();   // resubscribes once the knob settles
#endif
  panelNoteFlightboard();     // and it is still this airport after a reboot
  snprintf(fbToast, sizeof(fbToast), "%s", flightboardAirportLabel(flightboardAirportId()));
  ctrlToast(fbToast);
}
#endif

// ---------------------------------------------------------------- the portal
// What the web portal needs from the page model above (src/panel/panel.h).
// Web handlers run inside loop(), on this task, so they may touch it directly.
uint8_t     panelPageCount()             { return ctrlPageCount(); }
const char *panelPageName(uint8_t page)  { return ctrlPageName(page); }
uint8_t     panelCurrentPage()           { return ctrlPage; }
bool        panelEnteredPage()           { return ctrlEntered; }

uint16_t panelPageSeconds(uint8_t page) {
#if defined(CAROUSEL_ENABLED)
  return ctrlPageSeconds(page);
#else
  (void)page;
  return 0;
#endif
}

bool panelShowPage(uint8_t page) {
  if (page >= ctrlPageCount()) return false;
#if defined(CAROUSEL_ENABLED)
  carouselNote();             // held for the idle time, as if the knob had put it there
#endif
  ctrlEntered = false;
  httpForceAmbient = httpForceClock = httpForceViz = false;   // as a knob turn does
  ctrlPage = page;
  ctrlToast(page == PAGE_CLOCK ? nullptr : ctrlPageName(page));   // null: the style name
  // loop() sets these before it serves the web, so without this the frame drawn
  // right after the request would still be the previous page.
#if defined(FLIGHTBOARD_ENABLED)
  httpForceFlightboard = (ctrlPage == PAGE_FLIGHTBOARD);
#endif
#if defined(YACHTRADAR_ENABLED)
  httpForceYachtRadar  = (ctrlPage == PAGE_YACHTRADAR);
#endif
  return true;
}

bool panelShowStyle(uint8_t styleId) {
  if (!clockStyleSelect(styleId)) return false;
  return panelShowPage(PAGE_CLOCK);
}
#endif  // CONTROL_ENCODER_ENABLED

// ========== loop() ==========
// The longest loop() pass in the last 10 s, for /api/info: a slow page change or
// a stuttering effect shows up here before anyone has to guess.
static uint32_t s_loopPrevUs = 0, s_loopMaxUs = 0, s_loopMaxLastUs = 0, s_loopWinMs = 0;
uint32_t loopMaxMs() { return s_loopMaxLastUs / 1000UL; }

void loop() {
  {
    const uint32_t nowUs = micros();
    if (s_loopPrevUs && nowUs - s_loopPrevUs > s_loopMaxUs) s_loopMaxUs = nowUs - s_loopPrevUs;
    s_loopPrevUs = nowUs;
    if (millis() - s_loopWinMs >= 10000UL) { s_loopWinMs = millis(); s_loopMaxLastUs = s_loopMaxUs; s_loopMaxUs = 0; }
  }
  // Feed watchdog
  esp_task_wdt_reset();
#if defined(NSLUA_BENCH)
  nsluaBenchLoop();
#endif

#if defined(CONTROL_ENCODER_ENABLED)
  // One knob: rotation browses, a click selects (see "the knob" above loop()).
  //
  // The httpForce* flags these drive are named after the HTTP routes, but no
  // web route writes these two - they are set here and nowhere else. Two
  // consequences worth knowing: with the encoder compiled out both pages are
  // unreachable, and /api/status does not report them.
  controlLoop();
  if (ctrlEntered && millis() - ctrlLastEventMs > CTRL_ENTER_TIMEOUT_MS) ctrlEntered = false;
  for (CtrlEvent e = controlTake(); e != CTRL_NONE; e = controlTake()) {
#if defined(CAROUSEL_ENABLED)
    carouselNote();            // somebody is here; stop advancing on our own
#endif
    ctrlLastEventMs = millis();
    // One meaning for the switch: held a little too long, it is still a click.
    if (e == CTRL_LONG) e = CTRL_PRESS;
#if defined(CARDS_ENABLED)
    // A notification owns the screen, so the first click dismisses it and does
    // nothing else. Anything other than that would act on a page you cannot see.
    if (cardsNotifyActive() && e == CTRL_PRESS) {
      cardsNotifyDismiss();
      continue;
    }
    if (ctrlPage >= ctrlPageCount()) { ctrlPage = PAGE_CLOCK; ctrlEntered = false; }  // card expired
#endif
    if (e == CTRL_PRESS) {
      if (ctrlPageHasControls(ctrlPage)) {
        ctrlEntered = !ctrlEntered;
        ctrlToast(ctrlEntered ? ctrlEnterHint(ctrlPage) : "TURN: PAGES");
      }
      continue;                // a page without controls has nothing to select
    }
    const int8_t d = (e == CTRL_CW) ? 1 : -1;
    if (!ctrlEntered) { ctrlBrowse(d); continue; }
    switch (ctrlPage) {
#if defined(FLIGHTBOARD_ENABLED)
    case PAGE_FLIGHTBOARD: fbKnob(d); break;
#endif
#if defined(YACHTRADAR_ENABLED)
    case PAGE_YACHTRADAR:  yachtRadarScroll(d); break;
#endif
#if defined(RAILBOARD_ENABLED)
    // Inside the rail board the knob steps departures, arrivals, diagnostics;
    // the choice holds the 10 s alternation for RB_HOLD_S (railboard.h).
    case PAGE_RAILBOARD:   railboardKnob(d); break;
#endif
    default:               ctrlEntered = false; ctrlBrowse(d); break;
    }
  }
#if defined(CAROUSEL_ENABLED)
  // Advance only when the knob has been quiet for a while, so the page you
  // chose stays where you left it until you have walked away from it.
  if (carouselDue(ctrlPageSeconds(ctrlPage))) {
    // With "walk every clock style" (CAROUSEL_ALL_STYLES by default, switchable
    // in the portal) each style takes a slot of its own on the clock page, and
    // the page moves on once the last style has had its turn.
    const bool stay = (ctrlPage == PAGE_CLOCK) && panelCarousel().allStyles &&
                      clockStyleCarouselNext();
    if (!stay) ctrlPage = ctrlNextVisited(ctrlPage, +1);
  }
  if (ctrlPage >= ctrlPageCount()) ctrlPage = PAGE_CLOCK;
#endif
#if defined(LUA_EFFECTS_ENABLED)
  // The web UI picks an effect by index: it becomes the page, as a knob turn
  // would make it, and holds the carousel as a knob turn does.
  {
    const int16_t asked = luaEffectsTakeShowRequest();
    if (asked >= 0) {
      ctrlPage = (uint8_t)(PAGE_LUA_FIRST + asked);
      ctrlEntered = false;
#if defined(CAROUSEL_ENABLED)
      carouselNote();
#endif
    }
  }
  // Opens the effect on its task when its page arrives, closes it when it goes.
  luaEffectsSelect(ctrlLuaEffect(ctrlPage));
#endif
#if defined(FLIGHTBOARD_ENABLED)
  httpForceFlightboard = (ctrlPage == PAGE_FLIGHTBOARD);
#endif
  clockStyleTick();          // deferred NVS write, once the knob settles
  panelTick();               // the same for the portal's panel settings
#if defined(YACHTRADAR_ENABLED)
  httpForceYachtRadar  = (ctrlPage == PAGE_YACHTRADAR);
#endif
#endif  // CONTROL_ENCODER_ENABLED

#if defined(MQTT_BUS_ENABLED)
  // Runs regardless of which page is up: unlike the AIS websocket this costs
  // almost nothing idle, and a retained payload that arrives while the clock is
  // showing means the page is already populated when you turn to it.
  mqttBusLoop();
#endif
#if defined(FLIGHTBOARD_ENABLED) && defined(FB_MQTT_ENABLED)
  fbMqttLoop();
#endif
#if defined(FLIGHTBOARD_ENABLED)
  // The AeroAPI fetch, while the page is up or was a moment ago, and trackers
  // on their own cadence; nothing without FLIGHTBOARD_DIRECT_ENABLED.
  flightboardTick(httpForceFlightboard);
#endif
#if defined(CARDS_ENABLED)
  cardsLoop();
#endif
#if defined(RAILBOARD_ENABLED)
  railboardLoop();           // the retained station selection, once connected
#endif
  weatherLoop();             // starts a one-shot fetch task when one is due

#if defined(YACHTRADAR_ENABLED)
  // The AIS stream is held open only while its page is up: a websocket to
  // aisstream.io costs a TLS session's worth of heap and a steady trickle of
  // traffic, and neither is worth paying for a page nobody is looking at.
  // Same gating fx34 uses on the NickoScope32 side.
  {
    static bool yrWasOn = false;
    if (httpForceYachtRadar != yrWasOn) {
      if (httpForceYachtRadar) yachtRadarBegin();
      else                     yachtRadarStop();
      yrWasOn = httpForceYachtRadar;
    }
    if (httpForceYachtRadar) yachtRadarLoop();
  }
#endif

  // Check and apply scheduled brightness (time-based dimming)
  checkScheduledBrightness();

  // Handle web server requests
  server.handleClient();

  // Handle UDP packets - always process to track PC online status accurately
  handleUDP();

  // Check timeout
  if (millis() - lastReceived > TIMEOUT && metricData.online) {
    metricData.online = false;
    Serial.println("PC stats timeout - switching to clock mode");
  }

  // Retry NTP sync periodically if not synced
  if (!ntpSynced && millis() - lastNtpSyncTime > 30000) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
      if (timeinfo.tm_year > 120) {
        ntpSynced = true;
        lastNtpSyncTime = millis();
        Serial.println("NTP sync successful (retry)");
      }
    } else {
      applyTimezone();  // SNTP client might be dead - restart it
      Serial.println("NTP retry: restarted SNTP client");
    }
    lastNtpSyncTime = millis();
  }

  // Periodic NTP re-sync even when already synced (safety net). SNTP keeps the
  // system clock valid between refreshes, so refresh the timezone/SNTP client
  // without clearing ntpSynced - dropping it would needlessly re-anchor the
  // clocks (below) every hour and could flash "Syncing time..." for a frame.
  if (ntpSynced && millis() - lastNtpSyncTime > NTP_RESYNC_INTERVAL) {
    applyTimezone();
    lastNtpSyncTime = millis();
    Serial.println("Periodic NTP re-sync triggered");
  }

  // Re-anchor the animated clocks when NTP first becomes valid (or after a
  // reconnect resync). At boot the clocks seed displayed_hour/min with a 00:00
  // fallback; if the first sync lands inside a clock's minute-change window the
  // animation advances from 00:00 instead of jumping to the real time, leaving
  // the clock stuck near 00:00 until a reboot or clock-cycle. Resetting the
  // animation state clears the stale time override, and syncDisplayedTime()
  // forces displayed_hour/min to match reality.
  static bool prevNtpSynced = false;
  if (ntpSynced && !prevNtpSynced) {
    resetClockAnimationState();
    struct tm now_tm;
    if (getLocalTime(&now_tm, 10)) {
      syncDisplayedTime(&now_tm);
    }
  }
  prevNtpSynced = ntpSynced;

  // Display update with adaptive refresh rate
  int targetHz = getOptimalRefreshRate();
  unsigned long frameInterval = 1000 / targetHz;

  if (millis() >= nextDisplayUpdate && displayAvailable && !isDisplayForcedOff()) {
    // Schedule from the previous deadline, not from "now": scheduling from now
    // adds the loop latency to every frame, so frames drift off the fixed grid
    // and the animation tick gates beat against them (visible micro-stutter).
    nextDisplayUpdate += frameInterval;
    if (millis() >= nextDisplayUpdate) {
      // Fell behind (stall or refresh-rate change) - resync to now.
      nextDisplayUpdate = millis() + frameInterval;
    }
#if defined(NSLUA_BENCH)
    nsluaBenchFrameBegin();
#endif

    // Visualizer wins over everything while forced AND fed; when the packet
    // stream dies for 10s it falls through (and auto-resumes when it's back).
    bool showViz = httpForceViz && vizShouldDisplay();
    bool showStats =
        !showViz && metricData.online && !httpForceClock && !httpForceAmbient;

    // The DMA flip only takes effect at the end of the panel's current scan
    // (the library does not wait for the buffer to be free), so anything we
    // draw in the first ms after flipping can appear on screen. Clearing to
    // black is the worst offender - a visible dark flash on full-screen
    // content. The custom animation player overwrites every pixel of the
    // frame, so skip the redundant clear when it is what renders this tick.
    bool animFullRepaint = !showViz && !showStats && ambientActive() &&
                           settings.ambientStyle == 6 && ambientCustomPlaying();
#if defined(LUA_EFFECTS_ENABLED)
    // An effect's blit writes all 8192 pixels; a clear first would only flash black.
    if (luaEffectCurrent() >= 0) animFullRepaint = true;
#endif
    // Bright starfield details make partial scans visible. Do not clear/reuse
    // the previous front buffer until the queued flip has settled.
    if (showViz && settings.vizStyle == 5) display.waitForScanCompletion();
    if (!animFullRepaint) display.clearDisplay();

#if defined(CARDS_ENABLED) && defined(CONTROL_ENCODER_ENABLED)
    // Requires the knob: a card is only ever reached by walking the pages, so
    // without an encoder there is no way to select one and nothing to render.
    // A notification still works - it takes the screen by itself.
    if (ctrlPage >= PAGE_COUNT && (ctrlPage - PAGE_COUNT) < cardsCount()) {
      cardsRender((uint8_t)(ctrlPage - PAGE_COUNT));
    } else
#endif
#if defined(WORLDCLOCK_ENABLED) && defined(CONTROL_ENCODER_ENABLED)
    if (ctrlPage == PAGE_WORLDCLOCK) {
      worldClockRender();
    } else
#endif
#if defined(LUA_EFFECTS_ENABLED)
    // The newest frame the effect task finished, blitted from its PSRAM canvas.
    if (luaEffectCurrent() >= 0) {
      luaEffectsRender();
    } else
#endif
#if defined(RAILBOARD_ENABLED)
    // Drawn into the back buffer between the clear above and the flip below,
    // like every page: the panel never shows a half-drawn board.
    if (ctrlPage == PAGE_RAILBOARD) {
      railboardRender();
    } else
#endif
#if defined(YACHTRADAR_ENABLED)
    if (httpForceYachtRadar) {
      yachtRadarRender();
    } else
#endif
#if defined(FLIGHTBOARD_ENABLED)
    if (httpForceFlightboard) {
      flightboardRender();
    } else
#endif
    if (showViz) {
      displayVisualizer();
    } else
    // Show error status if PC is connected but LHM has issues
    if (showStats && metricData.status != STATUS_OK && metricData.status != 0) {
      displayErrorStatus(metricData.status);
    } else if (showStats) {
      displayStats();
    } else if (ambientActive()) {
      // Scheduled (or forced) ambient screensaver replaces the clock.
      displayAmbient();
    } else {
      switch (settings.clockStyle) {
      case 0:
        displayClockWithMario();
        break;
      case 1:
        displayStandardClock();
        break;
      case 2:
        displayLargeClock();
        break;
      case 3:
      case 4:
        displayClockWithSpaceInvader();
        break;
      case 5:
        displayClockWithPong();
        break;
      case 6:
        displayClockWithPacman();
        break;
      case 7:
        displayClockWithSnake();
        break;
      case 8:
        displayClockWithTetris();
        break;
      case 9:
        cycleClockScreens();
        break;
      case 10:
        displayClockWithAsteroids();
        break;
      case 11:
        displayClockWithDino();
        break;
      case 12:
        displayClockWithMatrixRain();
        break;
      case 16:
        displayClockWithTron();
        break;
      case 15:
        displayClockWithBomberman();
        break;
      case 14:
        displayClockWithWeather();
        break;
      default:
        displayStandardClock();
        break;
      }
    }

    // Notification banner draws over whatever screen is active.
    if (notifyActive()) {
      drawNotifyOverlay();
    }

#if defined(CONTROL_ENCODER_ENABLED)
    // After everything else, before the flip: a toast has to sit on top of
    // whatever the page drew. Every page now, since pages announce themselves.
    clockStyleOverlay();
    // While a click has entered a page, an amber mark in the corner says the
    // knob acts inside it rather than browsing.
    if (ctrlEntered) display.fillRect(display.width() - 3, 0, 3, 3, display.color565(255, 160, 0));
#endif
#if defined(CARDS_ENABLED)
    // Last of all, because it replaces the page rather than decorating it.
    if (cardsNotifyActive()) cardsNotifyRender();
#endif

    display.display();
#if defined(NSLUA_BENCH)
    nsluaBenchFrameEnd();
#endif

    // Right after the flip = maximum headroom before the next render tick;
    // the custom animation reads its next frame from flash here so the I/O
    // never delays a flip (delayed flips beat against the DMA scan).
    ambientCustomPrefetch();
  }

  // WiFi reconnection handling
  handleWiFiReconnection();
}

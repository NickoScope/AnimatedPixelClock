#pragma once
// Stock market dashboard: four pages - MARKETS, TICKER, PORTFOLIO, HOLDINGS -
// drawn from what Home Assistant has computed, over the MQTT bus.
//
// Home Assistant (an AppDaemon app, the other helper's) fetches the history
// and the quotes, runs the portfolio ledger and publishes compact retained
// payloads; the panel keeps the last of each in PSRAM and on LittleFS, draws
// them, and publishes its settings for the app to follow. Everything lives
// under one device's prefix:
//
//   MQTT_BASE/<dev>/market/config                 retained, panel -> HA: the settings (v2, market_settings.h)
//   MQTT_BASE/<dev>/market/status                 retained, HA -> panel: the app's health and per-symbol diagnostics
//   MQTT_BASE/<dev>/market/index/<sym>/<preset>   retained, HA -> panel: one window of one index
//   MQTT_BASE/<dev>/market/ticker/<sym>/<preset>  retained, HA -> panel: one window of one ticker
//   MQTT_BASE/<dev>/market/portfolio/<mode>/<preset>  retained, HA -> panel: the ledger, hold or rebal
//   MQTT_BASE/<dev>/market/holdings/<mode>        retained, HA -> panel: target and current weights
//   MQTT_BASE/<dev>/market/live                   retained, HA -> panel: quotes while an exchange is open
//   MQTT_BASE/<dev>/market/intraday/<sym>         retained, HA -> panel: today's session of the ticker on screen
//   MQTT_BASE/<dev>/market/tape                   retained, HA -> panel: the exchanges and their state
//   MQTT_BASE/<dev>/market/ha                     retained, HA -> panel: online | offline (the app's will)
//
// <dev> is the last three bytes of the Wi-Fi station MAC in lower-case hex, as
// the media player uses it. The panel takes one bus handler and one wildcard
// subscription, market/#.
//
// Pages, knob and screens: market_page.cpp. Payloads, NVS, LittleFS, routes:
// src/market/README.md. Design: docs/18-stock-dashboard.md and the settings
// inventory docs/19-market-dashboard-research.md in the knowledge base.

#include <stdint.h>
#include <ArduinoJson.h>

#if defined(MARKET_ENABLED)

#if !defined(MQTT_BUS_ENABLED)
#error "MARKET_ENABLED needs MQTT_BUS_ENABLED: every number on the pages arrives over src/mqtt/mqtt_bus"
#endif
#if !defined(CONTROL_ENCODER_ENABLED)
#error "MARKET_ENABLED needs CONTROL_ENCODER_ENABLED: the pages are reached and driven with the knob"
#endif

#include "market_layout.h"
#include "market_model.h"
#include "market_settings.h"
#include "market_settle.h"

// Build-time values. None is a standard; each is a design choice named here.
#ifndef MARKET_ENTER_TIMEOUT_MS
#define MARKET_ENTER_TIMEOUT_MS 10000    // 10 s of quiet leaves an entered page (docs/18, the knob)
#endif
#ifndef MARKET_FS_SETTLE_MS
#define MARKET_FS_SETTLE_MS     5000     // the record goes to LittleFS this long after the last accepted payload
#endif
#ifndef MARKET_FS_MIN_GAP_MS
#define MARKET_FS_MIN_GAP_MS    600000   // and at most this often: the app republishes every 6 h, flash wears
#endif
#ifndef MARKET_FS_CHUNK
#define MARKET_FS_CHUNK         4096     // bytes of the record written per loop() pass, so the picture never waits
#endif
#ifndef MARKET_NVS_SETTLE_MS
#define MARKET_NVS_SETTLE_MS    2500     // as src/panel: a burst of changes is one write
#endif

#define MARKET_PAGE_COUNT (market::PG_COUNT)   // the four pages, each a CtrlPage of its own

void    marketBegin();                 // setup(), after mqttBusBegin(): NVS, LittleFS, handler, subscription
void    marketLoop();                  // loop(): the config out, deferred NVS and LittleFS writes, the knob's timers
void    marketRender(uint8_t sub);     // one frame of page `sub` (market::Page); the caller has cleared the screen
uint8_t marketRefreshHz();             // 20 while the tape moves, else 5

const char *marketPageName(uint8_t sub);   // "MARKETS" "TICKER" "PORTFOLIO" "HOLDINGS"
bool        marketPageEnabled(uint8_t sub);   // display.pages.*: visited by the knob and the carousel
uint16_t    marketDwellS();            // display.dwell_s: the carousel's seconds per market page
uint32_t    marketEnterTimeoutMs();    // MARKET_ENTER_TIMEOUT_MS

// The knob inside a page (docs/18): a click enters WINDOW (rotate steps the
// preset, shown in a status row for 1.5 s); on MARKETS, TICKER and HOLDINGS a
// second click enters the page's own list (the primary index, the ticker, the
// page of rows); the next click, or 10 s of quiet, leaves. marketKnobClick()
// takes whether the page is entered now and returns whether it stays entered.
bool        marketKnobClick(uint8_t sub, bool entered);
const char *marketKnobHint();          // the toast for the mode just entered
void        marketKnob(uint8_t sub, int8_t delta);

// ---- the web portal (src/web/web_panel.cpp, /api/market)
void marketWebJson(JsonObject out);
// One of {"config":{...}} (market_settings.h: known keys validated whole,
// unknown keys kept for the app) | {"show":"markets|ticker|portfolio|holdings"}
// | {"republish":true} | {"reset":"<group key>"|"all"}. Returns an HTTP status;
// on anything but 200 err says what was wrong. *showSub is the page to put on
// screen, or -1.
int  marketWebPost(JsonObjectConst in, char *err, size_t cap, int *showSub);

// ---- shared by market_ha.cpp and market_page.cpp
namespace market {
bool  ready();                         // the model has its memory; false: every accessor below is off limits
const Model &model();
const mks::Settings &settings();
const char *deviceId();                // six hex digits, "" before the MAC is known
bool  bridgeOnline();                  // the app's retained "ha" topic says online
bool  haveBridge();                    // it has said anything at all
uint32_t lastRxMs();                   // millis() of the last accepted payload, 0 = none
const char *selectedTicker();          // the ticker on screen (mks::effectiveTicker), "" without tickers
// The knob chose a preset / a ticker: the settings now, NVS once the knob has
// been quiet MARKET_NVS_SETTLE_MS (market_settle.h), and for the ticker one
// config publish then, if it differs from the last one published. The window
// never leaves the panel.
void  noteWindow(uint8_t preset);
void  noteTicker(const char *sym);
// The page's per-frame buffers, in the PSRAM store with the rest.
struct PageBuf {
  char tape[kMaxTape][kExNameLen];
  char currency[8];
};
PageBuf &pageBuf();
void  pageTick();                      // market_page.cpp: the knob's timers; called by marketLoop()
}  // namespace market

#endif  // MARKET_ENABLED

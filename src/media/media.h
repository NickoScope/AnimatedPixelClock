#pragma once
// Media player, phase 1: a "now playing" page for a Home Assistant or Music
// Assistant player, and a remote for it. No audio on the panel.
//
// Home Assistant (an AppDaemon app, tools/media/appdaemon/) follows one player
// and publishes compact retained payloads; the panel draws them and sends
// commands back. Everything lives under one device's prefix:
//
//   MQTT_BASE/<dev>/media/state     retained, HA -> panel: the followed player
//   MQTT_BASE/<dev>/media/players   retained, HA -> panel: up to 8 players
//   MQTT_BASE/<dev>/media/favs      retained, HA -> panel: up to 16 MA radio favourites
//   MQTT_BASE/<dev>/media/ha        retained, HA -> panel: online | offline (the app's will)
//   MQTT_BASE/<dev>/media/select    retained, panel -> HA: the player chosen in the portal
//   MQTT_BASE/<dev>/media/cmd       not retained, panel -> HA: transport, volume, favourites
//
// <dev> is the last three bytes of the Wi-Fi station MAC in lower-case hex -
// the six digits of the broker client id apc-XXYYZZ - so it survives a rename
// and differs between two panels. The portal's Media page shows it.
//
// Page, knob and screens: media_page.cpp. Payloads, NVS, routes and the Home
// Assistant install: src/media/README.md. Design: docs/17-media-player.md in
// the knowledge base (phase 1 only).

#include <stdint.h>
#include <ArduinoJson.h>

// Outside the enabled block, so each fires when the flag it needs is missing.
#if defined(MEDIAPLAYER_RADIO_ENABLED)
#error "MEDIAPLAYER_RADIO_ENABLED is phase 2 (local radio) and is not built: docs/17-media-player.md"
#endif

#if defined(MEDIAPLAYER_ENABLED)

#if !defined(MQTT_BUS_ENABLED)
#error "MEDIAPLAYER_ENABLED needs MQTT_BUS_ENABLED: state and commands travel over src/mqtt/mqtt_bus"
#endif
#if !defined(CONTROL_ENCODER_ENABLED)
#error "MEDIAPLAYER_ENABLED needs CONTROL_ENCODER_ENABLED: the page is reached and driven with the knob"
#endif

#include "media_model.h"

// Build-time values. None is a standard; each is a design choice named here.
#ifndef MEDIA_STALE_S
#define MEDIA_STALE_S      150    // the app's keepalive is 60 s: two and a half of them missed
#endif
#ifndef MEDIA_TUNE_REST_MS
#define MEDIA_TUNE_REST_MS 1200   // a favourite starts once the knob rests this long (docs/17 3.5)
#endif
#ifndef MEDIA_VOL_STEP
#define MEDIA_VOL_STEP     2      // percent per detent (docs/17 3.5)
#endif
#ifndef MEDIA_VOL_SEND_MS
#define MEDIA_VOL_SEND_MS  250    // at most one volume command this often while the knob turns
#endif
#ifndef MEDIA_OVERLAY_MS
#define MEDIA_OVERLAY_MS   1600   // the volume band stays this long after a change (docs/17 6)
#endif

void    mediaBegin();       // setup(), after mqttBusBegin(): NVS, handler, subscription
void    mediaLoop();        // loop(): selection out, coalesced volume, the tune timer, NVS
void    mediaRender();      // one frame; the caller has cleared the screen
uint8_t mediaRefreshHz();   // what the page needs now: more while something moves

// The knob inside the page (docs/17 3.5). A click walks TUNE -> VOLUME -> out;
// mediaKnobClick() takes whether the page is entered now and returns whether
// it stays entered. Rotation acts in the current mode.
bool        mediaKnobClick(bool entered);
const char *mediaKnobHint();        // the toast for the mode just entered
void        mediaKnob(int8_t delta);

// ---- the web portal (src/web/web_panel.cpp, /api/media)
void mediaStatusJson(JsonObject out);
// One of {"select":id} {"cmd":"toggle|play|pause|next|prev"} {"vol":0..100}
// {"vol_step":-20..20} {"mute":bool} {"play_fav":id}. Validated whole first.
// Returns an HTTP status; on anything but 200 *why says what was wrong.
int  mediaWebPost(JsonObjectConst in, const char **why);

// ---- shared by media_ha.cpp and media_page.cpp
namespace media {
bool  ready();                      // the model has its memory; false: every accessor below is off limits
const NowPlaying &now();
const Players    &players();
const Favs       &favs();
bool  stale();                      // no state, or Home Assistant's ts older than MEDIA_STALE_S
long  ageS();                       // seconds since that ts, -1 without a clock or a state
const char *selected();             // the portal's choice, "" = none yet
const char *deviceId();             // six hex digits, "" before the MAC is known
bool  bridgeOnline();               // the app's retained "ha" topic says online
bool  haveBridge();                 // it has said anything at all
uint32_t newTrackMs();              // millis() the track key last changed, 0 = never
// Commands for the player on screen (the one Home Assistant follows, else the
// portal's choice). False: not connected, no player, or not valid.
bool  sendSimple(const char *cmd);  // toggle play pause next prev
bool  sendMute(bool muted);
bool  sendFav(const char *favId);
bool  sendVolStep(int step);        // -20..20, once
// The knob's volume: absolute when the volume is known, a relative step when
// it is not, coalesced to one command every MEDIA_VOL_SEND_MS either way.
void  nudgeVolume(int delta);
void  setVolume(int target);        // absolute 0..100, coalesced the same way
int   volumeShown();                // the pending target while there is one, else the reported volume
bool  setSelected(const char *playerId);   // validated; NVS "media"/"player" once it settles
void  pageTick();                   // media_page.cpp: the knob's timers; called by mediaLoop()
}  // namespace media

#endif  // MEDIAPLAYER_ENABLED

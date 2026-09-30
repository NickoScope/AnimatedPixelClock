#pragma once
// Instant sync events for the virtual twin (tools/twin): what is on the screen
// and every gesture, pushed the moment they happen instead of being polled.
//
// The twin's sync read /api/panel every 3 s: a change of page reached the other
// side in 1-3 s, and a press inside an effect (FLOW's next scene) never did,
// because nothing in /api/panel changed. Polling faster is not the answer - each
// GET /api/panel holds loop() ~40 ms. So a listener subscribes once and the
// panel sends it a small UDP datagram per change (the owner's ask, 2026-09-30;
// the input path measured by the simulation session on two engines):
//
//   POST /api/sync/listen {"port":N}     up to 2 listeners, the sender's own IP,
//                                        60 s, renewed by the same request;
//                                        {"port":N,"stop":true} leaves
//   UDP {"seq":N,"t":"screen","page":..,"key":..,"name":..,"style":..,
//        "entered":..,"off":..,"bright":..,"fx":{"id","run","clicks"},"by":..}
//   UDP {"seq":N,"t":"input","kind":"press"|"cw"|"ccw","by":..,"page":..,"entered":..}
//
// seq runs through both kinds: a gap tells the listener a datagram was lost and
// to read /api/panel again. A screen change is sent at most once a 100 ms per
// listener (the rest coalesces into the next one); an input goes at once.
// Nothing is repeated or waited for.
//
// "by" is who made the change: knob, ir (the remote), http (a request from
// anything else), sync (a request carrying X-Twin-Sync: 1 - the twin's own
// writes, so it never mirrors them back), carousel, schedule, or auto.
//
// The last 8 inputs also stay in a ring for GET /api/panel?input=N (after seq N)
// - a double or triple press within 0.45 s is a gesture of its own on KINETIC
// DIGITS and OCEANARIUM, and a lost datagram must not lose one of those.

#include <stddef.h>
#include <stdint.h>

#if defined(SYNC_EVENTS_ENABLED)

#if !defined(CONTROL_ENCODER_ENABLED)
#error "SYNC_EVENTS_ENABLED needs CONTROL_ENCODER_ENABLED: it reports the page model"
#endif

#include <ArduinoJson.h>
#include <IPAddress.h>

enum SyncBy : uint8_t {
  SYNC_BY_AUTO = 0, SYNC_BY_KNOB, SYNC_BY_IR, SYNC_BY_HTTP, SYNC_BY_SYNC,
  SYNC_BY_CAROUSEL, SYNC_BY_SCHEDULE,
};
enum SyncInputKind : uint8_t { SYNC_IN_PRESS = 0, SYNC_IN_CW, SYNC_IN_CCW };

// Who is changing the screen right now. The next change seen within 1.5 s is
// put down to it; anything older is "auto".
void   syncNoteCause(SyncBy by);
// For a web handler: "sync" when the request carries X-Twin-Sync: 1, else "http".
SyncBy syncHttpBy();
// loop(), right after server.handleClient(): a request that was served there
// becomes the cause (http or sync). reqCount is the web server's running count.
void   syncAfterHttp(uint32_t reqCount);
// A gesture that reached a screen or an effect: into the ring, and out at once.
void   syncInput(SyncInputKind kind, SyncBy by);
// loop(), every pass: a changed screen goes out, coalesced to 100 ms.
void   syncEventsLoop();
// POST /api/sync/listen: false when both places are taken by others.
bool   syncListen(const IPAddress &ip, uint16_t port, bool stop);
uint32_t syncSeq();
// /api/panel: "inputSeq" and, when asked, "input":[events after seq].
void   syncPanelJson(JsonObject now, JsonDocument &doc, int32_t inputAfter);

#endif  // SYNC_EVENTS_ENABLED

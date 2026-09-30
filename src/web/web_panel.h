#pragma once
// The portal's Panel group: what is on screen, the pages, the carousel, the
// flight board, the rail board, the world clock, the knob, the yacht radar and
// the Lua effects. Routes and JSON shapes: web_panel.cpp. Markup, style and
// script: web_panel_page.h.
//
// Only on builds with the knob: without it there are no pages to control, and
// the 4MB boards pay nothing for any of this.

#include <Arduino.h>
#include <ArduinoJson.h>

// web.cpp's guarded JSON sender, for a body that is not an Arduino String
// (bounded blocking, watchdog fed, a stalled client dropped). Every build.
void sendJsonBytesGuarded(int code, const char *data, size_t len);

// web.cpp's guarded sender for a body too large to hold in memory: `read` fills
// each piece from wherever the body lives (a flash partition, a LittleFS file),
// in order, and returns false when it cannot. The piece is on the calling
// task's stack (web.cpp says why). Whatever headers the caller queued go out first, then
// Content-Length `len` and the body. `totalMs` bounds the whole transfer; 0 is
// the cap every other response has. true when the whole body went; after a
// false the body stopped short and the connection closes, which a client that
// checks Content-Length sees. Every build.
typedef bool (*WebStreamRead)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
bool sendStreamGuarded(int code, const char *contentType, uint32_t len, WebStreamRead read, void *ctx,
                       uint32_t totalMs);

// The queue's door. True when this request has already been answered with 503
// because the network is busy with a fetch or the memory from the previous
// client has not come back yet - the caller must return at once. Never refuses
// /api/info or /api/status: those are how anyone sees what is happening.
bool webBusyRefuse();

// web.cpp's allocator for response documents: PSRAM when there is some. Every build.
ArduinoJson::Allocator *webJsonAllocator();

#if defined(CONTROL_ENCODER_ENABLED)

void   panelWebBegin();       // registers the /api routes; called by setupWebServer()
String panelWebFeatures();    // space-separated module names, for the page's data-f

#endif

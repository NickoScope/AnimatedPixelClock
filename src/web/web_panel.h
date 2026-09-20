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

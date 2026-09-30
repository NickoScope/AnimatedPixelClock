/*
 * AnimatedPixelClock - Web Server Module
 *
 * Web server handlers for configuration interface.
 */

#ifndef WEB_H
#define WEB_H

#include "../config/config.h"
#include <Update.h>
#include <WebServer.h>


#include "../config/settings.h"

// Global web server object
extern WebServer server;

// ========== Web Server Functions ==========

// Initialize web server with all routes
void setupWebServer();

// Web handlers
void handleRoot();
void handlePortalCss();
void handlePortalJs();
void handleFavicon();
void handlePortalValues();   // GET /api/portal: the values the page fills in
void handleSave();
void handleReset();
// True for a request a browser made on behalf of another site's page: an
// Origin that is not exactly http://<Host>, or a Host that is not this panel's
// own (an IPv4 address or an mDNS .local name - a DNS-rebound domain is
// neither). No Origin at all (curl, the agent tools, the twin app) is not
// foreign. POST /reset and the Panel group's writes (web_panel.cpp) use it.
bool webOriginForeign();
void handleMetricsAPI();
void handleDeviceInfo();
void handleRename();
void handleExportConfig();
void handleImportConfig();
void handleNtpTest();

// Notification banner API
void handleNotify();
void handleNotifyDismiss();

// Custom animation storage API (uploaded .pca files on LittleFS)
void handleAnimList();
void handleAnimPlay();
void handleAnimDelete();
void handleAnimUploadDone();
void handleAnimUploadChunk();

// Runtime control API (display power, mode, brightness, clock style, reboot)
void handleStatus();
void handleDisplayOn();
void handleDisplayOff();
void handleSetBrightness();
void handleModeClock();
void handleModeAuto();
void handleModeAmbient();
void handleModeViz();
void handleSetClockStyle();
void handleReboot();

#endif // WEB_H

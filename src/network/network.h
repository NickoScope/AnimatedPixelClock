/*
 * AnimatedPixelClock - Network Module
 *
 * WiFi connection management, UDP packet handling, and NTP sync.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "../config/config.h"

// ========== Global Network Objects ==========
extern WiFiUDP udp;
extern WiFiManager wifiManager;

// ========== Network Functions ==========

// Initialize WiFi and UDP
void initNetwork();

// Initialize mDNS service discovery
void initMDNS();

// Apply static IP settings if configured
void applyStaticIP();

// Initialize NTP time synchronization
void initNTP();

// Apply timezone settings
void applyTimezone();

// Handle UDP packet reception
void handleUDP();

// Parse incoming stats JSON
bool parseStats(const char* json);
void parseStatsV2(JsonDocument& doc);

// WiFi reconnection handling
void handleWiFiReconnection();

// Link health. WiFi.status() can report WL_CONNECTED while the stack moves no
// traffic at all, so real traffic is tracked and idle links are probed.
void netMarkHttp();
void netMarkInbound();
void netMarkOutboundOk();
uint32_t netHttpServed();
uint32_t netSecsSinceHttp();
#include "net_turns.h"   // NET_HTTP_NEVER
uint32_t netMsSinceHttp();   // milliseconds since the last HTTP request we served
uint32_t netSecsSinceTraffic();
uint32_t netRecoveryCount();
const char* netLastRecoveryReason();

// Seconds the panel has been unable to tell whether its link works - it cannot
// raise a probe and nothing is arriving. 0 when it can tell. This is NOT a
// claim that the gateway is down; it is the absence of any claim at all, which
// is the state that left the panel invisible for an hour on 2026-09-21.
uint32_t netBlindSeconds();

// Display connection status screens
void displaySetupInstructions();
void displayConnecting();
void displayConnected();
void displayErrorStatus(uint8_t status);

// WiFi callbacks for WiFiManager
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();

// Manual WiFi connection (for hardcoded credentials)
bool connectManualWiFi(const char* ssid, const char* password);

#if QR_SETUP_ENABLED
void displayQRCodeSetup();
#endif

#endif // NETWORK_H

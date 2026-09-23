/*
 * AnimatedPixelClock - Network Module
 *
 * WiFi connection management, UDP packet handling, and NTP sync.
 */

#include "network.h"
#include "../display/display.h"
#include "../utils/utils.h"
#include "../timezones.h"
#include "../viz/visualizer.h"
#if defined(AUDIO_MIC_ENABLED)
#include "../audio/audio_mic.h"
#endif
#include "improv_setup.h"
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include "ping/ping_sock.h"

#if QR_SETUP_ENABLED
#include "qrcode.h"
#endif

// Global network objects
WiFiUDP udp;
WiFiManager wifiManager;
extern Preferences preferences;

// ========== WiFi Callbacks ==========
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Config mode entered");
  Serial.println(WiFi.softAPIP());

  if (displayAvailable) {
#if QR_SETUP_ENABLED
    displayQRCodeSetup();
#else
    displaySetupInstructions();
#endif
  }
}

void saveConfigCallback() {
  if (displayAvailable) {
    displayConnecting();
  }
}

// ========== Static IP Application ==========
// Routers name a device by the hostname in its DHCP request (option 12), not
// by mDNS. arduino-esp32 2.0.17 sends "esp32s3-" and the last three MAC bytes
// (esp32s3-XXXXXX) unless told otherwise, and it only pushes a name to the
// station interface inside WiFi.mode() (WiFiGeneric.cpp: setHostname() stores
// it, mode(WIFI_STA) applies it). The firmware never called setHostname(), so
// a router listed the panel by that default while mDNS answered to its real
// name. Set before every mode change, and pushed to a running interface too;
// ESP-IDF says a change on a live interface takes effect no later than the
// next reconnect (esp_netif.h), lwIP may carry it in a DHCP renewal before.
//
// The factory name "pixelclock" is left out: every unnamed panel has it, and
// the chip's default is at least unique on the router's list. Accepted limit:
// renaming a panel back to "pixelclock" leaves its previous name in DHCP until
// the next reboot.
void netApplyHostname() {
  if (!settings.deviceName[0] || strcmp(settings.deviceName, "pixelclock") == 0) return;
  WiFi.setHostname(settings.deviceName);
  wifiManager.setHostname(settings.deviceName);
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta) {
    const esp_err_t err = esp_netif_set_hostname(sta, settings.deviceName);
    if (err != ESP_OK) log_w("DHCP hostname not set: %s", esp_err_to_name(err));
  }
}

void applyStaticIP() {
  if (settings.useStaticIP) {
    IPAddress local_IP, gateway_IP, subnet_IP, dns1_IP;

    if (local_IP.fromString(settings.staticIP) &&
        gateway_IP.fromString(settings.gateway) &&
        subnet_IP.fromString(settings.subnet) &&
        dns1_IP.fromString(settings.dns1)) {

      Serial.println("Configuring Static IP...");
      Serial.print("IP: "); Serial.println(local_IP);
      Serial.print("Gateway: "); Serial.println(gateway_IP);
      Serial.print("Subnet: "); Serial.println(subnet_IP);
      Serial.print("DNS1: "); Serial.println(dns1_IP);

      wifiManager.setSTAStaticIPConfig(local_IP, gateway_IP, subnet_IP, dns1_IP);
    } else {
      Serial.println("Invalid static IP configuration, using DHCP");
    }
  }
}

// ========== Manual WiFi Connection ==========
bool connectManualWiFi(const char* ssid, const char* password) {
  if (displayAvailable) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.println("Connecting to");
    display.setCursor(10, 35);
    display.println(ssid);
    display.display();
  }

  netApplyHostname();
  WiFi.mode(WIFI_STA);

  // Apply static IP configuration if enabled
  if (settings.useStaticIP) {
    IPAddress local_IP, gateway_IP, subnet_IP, dns1_IP, dns2_IP;

    if (local_IP.fromString(settings.staticIP) &&
        gateway_IP.fromString(settings.gateway) &&
        subnet_IP.fromString(settings.subnet) &&
        dns1_IP.fromString(settings.dns1)) {

      dns2_IP.fromString(settings.dns2);

      Serial.println("Configuring Static IP for manual WiFi...");
      if (!WiFi.config(local_IP, gateway_IP, subnet_IP, dns1_IP, dns2_IP)) {
        Serial.println("Static IP configuration failed!");
      }
    } else {
      Serial.println("Invalid static IP configuration, using DHCP");
    }
  }

  WiFi.begin(ssid, password);

  int attempts = 0;
  int maxAttempts = 30;

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(1000);
    attempts++;

    if (displayAvailable && attempts % 5 == 0) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(10, 20);
      display.println("Connecting...");
      display.setCursor(10, 35);
      display.print("Attempt: ");
      display.print(String(attempts).c_str());
      display.print("/");
      display.println(String(maxAttempts).c_str());
      display.display();
    }
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi Connection Failed!");
    return false;
  }
}

// ========== Network Initialization ==========
void initNetwork() {
  netApplyHostname();   // before WiFiManager starts the station

  // Apply static IP if configured
  applyStaticIP();

  // Configure WiFiManager
  wifiManager.setConnectTimeout(30);
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setDebugOutput(false);

#if IMPROV_SETUP_ENABLED
  // Run the config portal non-blocking so we can pump the Improv-Serial
  // listener in parallel (the web-flasher "Configure WiFi" path). setup() still
  // blocks here until WiFi connects or the portal times out - see the loop
  // below - so the rest of boot behaves exactly as before.
  wifiManager.setConfigPortalBlocking(false);
#endif

  bool connected = (strlen(AP_PASSWORD) > 0)
    ? wifiManager.autoConnect(AP_NAME, AP_PASSWORD)
    : wifiManager.autoConnect(AP_NAME);

#if IMPROV_SETUP_ENABLED
  if (!connected) {
    // autoConnect() started the captive portal (non-blocking). On genuinely
    // fresh devices (no stored SSID) also open an Improv-Serial window so a
    // browser that just flashed via the web flasher can push credentials over
    // USB. Returning users who only mistyped a password get the AP portal
    // alone - they need to fix what they typed, not a serial dialog over a
    // port that may not even be connected anymore.
    bool freshDevice = !wifiManager.getWiFiIsSaved();
    if (freshDevice) {
      improvSetupBegin(IMPROV_SETUP_WINDOW_MS);
    }

    while (!connected && wifiManager.getConfigPortalActive()) {
      // Service the captive portal (DNS + web server).
      if (wifiManager.process()) {
        connected = true;
        break;
      }
      // Service Improv-Serial. On success the library has already saved the
      // credentials and connected STA, so restart for a clean STA-only boot.
      if (freshDevice && improvSetupTick()) {
        Serial.println("Improv: credentials received, restarting");
        Serial.flush();
        delay(200);  // let the response reach the browser before reset
        ESP.restart();
      }
      delay(5);
    }
    improvSetupEnd();
  }
#endif

  if (!connected) {
    Serial.println("Failed to connect and hit timeout");
    if (displayAvailable) {
      display.clearDisplay();
      display.setCursor(10, 20);
      display.println("WiFi Timeout!");
      display.setCursor(10, 35);
      display.println("Restarting...");
      display.display();
    }
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Set WiFi TX power to maximum for better range
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Start UDP listener
  udp.begin(UDP_PORT);
  Serial.print("UDP listening on port ");
  Serial.println(UDP_PORT);

  // Start mDNS for app discovery
  initMDNS();
}

// ========== mDNS Service Discovery ==========
void initMDNS() {
  MDNS.end();  // Stop any previous mDNS instance
  if (MDNS.begin(settings.deviceName)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
    MDNS.addServiceTxt("http", "tcp", "model", "AnimatedPixelClock");
    MDNS.addServiceTxt("http", "tcp", "mac", WiFi.macAddress().c_str());
    Serial.printf("mDNS started: %s.local\n", settings.deviceName);
  } else {
    Serial.println("mDNS failed to start");
  }
}

// ========== NTP Functions ==========
void applyTimezone() {
  // Configured servers, falling back to the compiled defaults. A blank
  // secondary is passed as nullptr so SNTP skips the slot.
  const char* ntp1 = strlen(settings.ntpServer1) > 0 ? settings.ntpServer1
                                                     : NTP_SERVER_PRIMARY;
  const char* ntp2 = strlen(settings.ntpServer2) > 0 ? settings.ntpServer2
                                                     : nullptr;

  // If timezone string is set, use automatic DST with configTzTime()
  if (strlen(settings.timezoneString) > 0) {
    configTzTime(settings.timezoneString, ntp1, ntp2);
    Serial.printf("Timezone set (automatic DST): %s\n", settings.timezoneString);
  }
  else {
    // Fallback: Try to map old GMT offset to default timezone
    const char* defaultTz = getDefaultTimezoneForOffset(settings.gmtOffset);
    if (defaultTz != nullptr) {
      configTzTime(defaultTz, ntp1, ntp2);
      Serial.printf("Auto-detected timezone: %s\n", defaultTz);
    }
    else {
      // Ultimate fallback: Manual offset without DST
      int gmtOffset_sec = settings.gmtOffset * 60;
      configTime(gmtOffset_sec, 0, ntp1, ntp2);
      Serial.printf("Manual offset (no DST): GMT%+d\n", settings.gmtOffset / 60);
    }
  }
  Serial.printf("NTP servers: %s, %s\n", ntp1, ntp2 ? ntp2 : "(none)");
}

void initNTP() {
  applyTimezone();
  ntpSynced = false;

  if (displayAvailable) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.println("Syncing time...");
    display.display();
  }

  struct tm timeinfo;
  for (int i = 0; i < 30; i++) {
    if (getLocalTime(&timeinfo, 100)) {
      if (timeinfo.tm_year > 120) {
        ntpSynced = true;
        lastNtpSyncTime = millis();
        Serial.println("NTP time synchronized successfully");
        break;
      }
    }
    delay(100);
  }

  if (!ntpSynced) {
    Serial.println("NTP sync pending, will retry in background");
  }
}

// ========== Link Health ==========
// WiFi.status() stays WL_CONNECTED even when the stack moves no traffic at all,
// so the association record and a gateway ping decide instead.

#define NET_IDLE_BEFORE_PROBE_MS 120000UL
#define NET_PROBE_RETRY_MS 60000UL
#define NET_PROBE_FAILS_BEFORE_RECOVERY 2
// Consecutive probe ROUNDS in which not one ping left the board. One such round
// is a transient - 2026-09-20 was exactly that, and restarting the radio over it
// took down a working link. Two in a row, NET_PROBE_RETRY_MS apart, is not: on
// 2026-09-22 a portal load starved the Wi-Fi task of its 1,626 B DMA buffer and
// the transmit path stayed broken after the heap had recovered - 23:30:06 three
// send errors, memory back at 25 KB free, still nothing leaving. Two rounds is
// about three minutes after the last traffic; the blind timer would have taken
// fifteen, and any stray inbound packet resets that.
#define NET_UNSENT_ROUNDS_BEFORE_RECOVERY 2
#define NET_REBOOT_AFTER_MS 360000UL

// The blind timer: how long the panel may be unreachable AND unable to find out
// why before it reboots. Deliberately far longer than NET_REBOOT_AFTER_MS,
// because the path that arms it has already caused one outage by acting too
// eagerly (see the comment at the `!sent` branch below). Fifteen minutes of a
// clock that is invisible on the network beats an hour of it.
#define NET_BLIND_REBOOT_MS 900000UL

static uint32_t netLastHttpMs = 0;
static uint32_t netLastTrafficMs = 0;
static uint32_t netBadSinceMs = 0;
static uint32_t netBlindSinceMs = 0;
static uint32_t netLastRecoverMs = 0;
static uint32_t netNextProbeMs = 0;
static uint32_t netHttpCount = 0;
static uint32_t netRecoverCount = 0;
static uint8_t netProbeFails = 0;
static uint8_t netUnsentRounds = 0;   // consecutive rounds with nothing sent
static const char* netRecoverReason = "";
static esp_ping_handle_t netPing = nullptr;
static volatile bool netPingReplied = false;
static volatile bool netPingDone = false;
static volatile uint32_t netPingSent = 0;   // requests the socket actually took

static void netMarkAlive() {
  netLastTrafficMs = millis();
  netBadSinceMs = 0;
  netBlindSinceMs = 0;
  netProbeFails = 0;
  netUnsentRounds = 0;
}

// Arm, or keep armed, the blind timer: we cannot say whether the link works,
// and nothing has reached us. Deliberately separate from netBadSinceMs, which
// means "the gateway did not answer" - a claim this state cannot make.
static void netBlind(const char *why) {
  uint32_t now = millis();
  if (!netBlindSinceMs) {
    netBlindSinceMs = now ? now : 1;   // 0 is the disarmed value
    Serial.printf("Link blind (%s): cannot probe and nothing is arriving\n", why);
    return;
  }
  if (now - netBlindSinceMs > NET_BLIND_REBOOT_MS) {
    // Whose fault it is remains unknown, and that is the point: in this state
    // the panel is useless to everyone on the network, and a fresh boot is the
    // one action that reliably gets its buffers back. Confirmed needed on
    // 2026-09-21: the panel sat unreachable for over an hour, loop() running
    // and the clock still drawing on the wall, because both blind paths
    // returned without arming anything. resetReason came back POWERON, so the
    // six-minute backstop never fired once.
    Serial.printf("Link blind for %lu s (%s), restarting\n",
                  (unsigned long)((now - netBlindSinceMs) / 1000), why);
    Serial.flush();
    delay(100);
    ESP.restart();
  }
}

// Seconds the panel has been unable to tell whether its link works, 0 when it
// can. Exposed so the next diagnosis is a reading rather than an afternoon.
uint32_t netBlindSeconds() {
  return netBlindSinceMs ? (millis() - netBlindSinceMs) / 1000 : 0;
}

void netMarkHttp() {
  netHttpCount++;
  netLastHttpMs = millis();
  netMarkAlive();
}

void netMarkInbound() { netMarkAlive(); }
void netMarkOutboundOk() { netMarkAlive(); }

uint32_t netHttpServed() { return netHttpCount; }
uint32_t netSecsSinceHttp() { return netLastHttpMs ? (millis() - netLastHttpMs) / 1000 : 0; }

// The same in milliseconds, which is the scale a fetch has to decide on: the
// starters use it to stand aside for a browser that is mid-page. NET_HTTP_NEVER
// when nothing has asked yet, so "never" always reads as quiet.
uint32_t netMsSinceHttp() { return netLastHttpMs ? millis() - netLastHttpMs : NET_HTTP_NEVER; }
uint32_t netSecsSinceTraffic() { return (millis() - netLastTrafficMs) / 1000; }
uint32_t netRecoveryCount() { return netRecoverCount; }
const char* netLastRecoveryReason() { return netRecoverReason; }

static void netPingSuccess(esp_ping_handle_t, void*) { netPingReplied = true; }
static void netPingEnd(esp_ping_handle_t hdl, void*) {
  // How many of the three actually left the board. ESP-IDF only counts a
  // request once the socket has taken it (ping_sock.c: `transmitted++` sits in
  // the else of the send-failure branch), so zero here means nothing was sent
  // and the silence that follows says nothing whatever about the gateway.
  uint32_t sent = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
  netPingSent = sent;
  netPingDone = true;
}

static void netPingRelease() {
  if (!netPing) return;
  esp_ping_stop(netPing);
  esp_ping_delete_session(netPing);
  netPing = nullptr;
}

static bool netStartProbe() {
  if (netPing) return false;
  uint32_t gw = (uint32_t)WiFi.gatewayIP();
  if (!gw) return false;

  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count = 3;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1000;
  cfg.data_size = 16;
  cfg.ttl = 64;
  cfg.task_stack_size = 3072;
  cfg.task_prio = 2;
  cfg.target_addr.type = IPADDR_TYPE_V4;
  cfg.target_addr.u_addr.ip4.addr = gw;

  esp_ping_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.on_ping_success = netPingSuccess;
  cb.on_ping_end = netPingEnd;

  netPingReplied = false;
  netPingDone = false;
  if (esp_ping_new_session(&cfg, &cb, &netPing) != ESP_OK) {
    netPing = nullptr;
    return false;
  }
  esp_ping_start(netPing);
  return true;
}

// Full radio restart. WiFi.reconnect() alone does not recover this state.
static void netRecover(const char* why) {
  uint32_t now = millis();
  if (!netBadSinceMs) netBadSinceMs = now;
  if (now - netBadSinceMs > NET_REBOOT_AFTER_MS) {
    Serial.printf("Link dead (%s) despite recovery, restarting\n", why);
    Serial.flush();
    delay(100);
    ESP.restart();
  }

  netPingRelease();
  netRecoverReason = why;
  netRecoverCount++;
  netProbeFails = 0;
  netLastRecoverMs = now;
  netNextProbeMs = now + NET_PROBE_RETRY_MS;
  netLastTrafficMs = now;

  Serial.printf("Link recovery (%s): restarting WiFi\n", why);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  netApplyHostname();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin();

  wifiConnected = false;
  wifiDisconnectTime = now;  // makes the reconnect branch re-init UDP and mDNS
}

static void netHealthTick() {
  uint32_t now = millis();
  if (!netLastTrafficMs) netLastTrafficMs = now;
  bool cooling = (now - netLastRecoverMs) < NET_PROBE_RETRY_MS;

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    if (!cooling) netRecover("no AP association");
    return;
  }

  if (netPing) {
    if (!netPingDone) return;
    bool replied = netPingReplied;
    netPingRelease();
    netNextProbeMs = now + NET_PROBE_RETRY_MS;
    const uint32_t sent = netPingSent;
    if (sent) netUnsentRounds = 0;         // the transmit path works, whatever the reply
    if (replied) {
      netMarkAlive();
    } else if (!sent) {
      // Nothing left the board: the probe failed on this side, not out there.
      // Measured on the panel 2026-09-20 - "ping_sock: send error=0" three
      // times, then this watchdog restarted a Wi-Fi link that was working, and
      // the panel was unreachable for minutes. The cause was on the line above
      // it in the log: the Wi-Fi task could not get its 1,626 B buffer. Counting
      // that as "gateway unreachable" made this watchdog the outage it exists
      // to prevent. Try again later; do not count it, and never restart the
      // radio on it.
      //
      // What that reasoning missed, and what 2026-09-21 cost: declining to
      // blame the gateway is right, but declining to notice anything at all
      // left the panel with no way out. netBadSinceMs is set only inside
      // netRecover(), so a board that can never raise a probe can never count
      // a failure, never recover, and never reach the six-minute reboot. It
      // sits there, drawing the clock, invisible. Hence the blind timer, which
      // makes no claim about the gateway.
      Serial.println("Link probe could not be sent (no buffer): not counted against the gateway");
      netBlind("probe not sent");
      // What neither of the above reasoned about: the round REPEATING. A second
      // round a minute later with nothing sent is not a buffer that was briefly
      // short - it is a transmit path that has stopped. The gateway is still not
      // blamed; the radio is restarted because the radio is what is broken, and
      // restarting it frees and re-allocates exactly the buffers it could not get.
      if (++netUnsentRounds >= NET_UNSENT_ROUNDS_BEFORE_RECOVERY && !cooling) {
        Serial.printf("Link probe unsendable %u rounds running: the Wi-Fi transmit path is stuck\n",
                      (unsigned)netUnsentRounds);
        netUnsentRounds = 0;
        netRecover("transmit path stuck");
      }
    } else if (++netProbeFails >= NET_PROBE_FAILS_BEFORE_RECOVERY && !cooling) {
      netRecover("gateway unreachable");
    }
    return;
  }

  if (now - netLastTrafficMs < NET_IDLE_BEFORE_PROBE_MS) return;
  if ((int32_t)(now - netNextProbeMs) < 0) return;
  netNextProbeMs = now + NET_PROBE_RETRY_MS;
  // The other blind path, and the quieter one: the session itself could not be
  // created. It wants a 3 KB task stack, which is exactly what a board short of
  // internal RAM cannot spare - so the watchdog goes deaf at the moment it is
  // most needed, and until now it did so silently.
  if (!netStartProbe()) netBlind("probe session not created");
}

// ========== WiFi Reconnection Handling ==========
// Reconnection interval in milliseconds (try every 30 seconds)
#define WIFI_RECONNECT_INTERVAL 30000

void handleWiFiReconnection() {
  static unsigned long lastReconnectAttempt = 0;

  if (WiFi.status() != WL_CONNECTED) {
    // Update global flag for icon display
    wifiConnected = false;
    metricData.online = false;

    if (wifiDisconnectTime == 0) {
      wifiDisconnectTime = millis();
      Serial.println("WiFi disconnected");
    }

    // Periodic reconnection attempt every 30 seconds
    unsigned long currentMillis = millis();
    if (currentMillis - lastReconnectAttempt > WIFI_RECONNECT_INTERVAL) {
      Serial.println("Attempting WiFi reconnection...");
      WiFi.reconnect();
      lastReconnectAttempt = currentMillis;
    }

    // NOTE: Auto-reboot removed - device continues as clock-only
    // NOTE: Display drawing removed - clock functions show small icon instead
  } else {
    // WiFi is connected
    wifiConnected = true;

    if (wifiDisconnectTime != 0) {
      Serial.println("WiFi reconnected successfully!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      wifiDisconnectTime = 0;
      ntpSynced = false;  // Force NTP resync after reconnection
      applyTimezone();    // Restart SNTP client and reapply timezone
      udp.stop();         // Re-initialize UDP socket (old fd is stale)
      udp.begin(UDP_PORT);
      initMDNS();         // Re-register mDNS after reconnection
      netMarkOutboundOk();
    }

    netHealthTick();
  }
}

// ========== UDP Packet Handling ==========
void handleUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    netMarkInbound();
    static char buffer[2048];

    // Check size BEFORE reading to avoid processing truncated data
    if (packetSize > (int)sizeof(buffer) - 1) {
      Serial.printf("ERROR: Packet %d bytes exceeds buffer %d bytes, discarding.\n",
                    packetSize, (int)sizeof(buffer));
      udp.flush();
      return;  // Don't update lastReceived for bad packets
    }

    int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = '\0';

      // Binary spectrum packets ("FFT1" + 32 bands) arrive at ~25 Hz - take
      // the fast path with no JSON parse and no serial logging. They do NOT
      // touch lastReceived/online: stats freshness stays truthful.
#if defined(AUDIO_MIC_ENABLED)
      // When the microphones own the visualizer a PC spectrum packet is
      // consumed here and dropped, never parsed as stats.
      if (len >= VIZ_PACKET_LEN && memcmp(buffer, "FFT1", 4) == 0 && !audioAcceptPcPacket()) {
        return;
      }
#endif
      if (vizIngest((const uint8_t*)buffer, len)) {
        return;
      }

      Serial.print("UDP packet: ");
      Serial.print(packetSize);
      Serial.print(" bytes, read: ");
      Serial.print(len);
      Serial.println(" bytes");

      // Only a packet that actually parsed proves the PC is alive.
      if (parseStats(buffer)) lastReceived = millis();
    }
  }
}

// ========== Stats Parsing ==========
void parseStatsV2(JsonDocument& doc) {
  // Parse status code (1=OK, 2=API error, 3=LHM not running, etc.)
  uint8_t newStatus = doc["status"] | STATUS_OK;

  if (newStatus != metricData.status) {
    // Status changed - log it
    switch (newStatus) {
      case STATUS_OK:
        Serial.println("Status: LHM OK");
        break;
      case STATUS_API_ERROR:
        Serial.println("Status: LHM API error - check REST API");
        break;
      case STATUS_LHM_NOT_RUNNING:
        Serial.println("Status: LHM not running!");
        break;
      case STATUS_LHM_STARTING:
        Serial.println("Status: LHM starting up...");
        break;
      default:
        Serial.printf("Status: Unknown error (%d)\n", newStatus);
        break;
    }
  }
  metricData.status = newStatus;

  const char* ts = doc["timestamp"];
  if (newStatus == STATUS_OK && ts && strlen(ts) > 0) {
    // Only stamp "Last OK" when status is healthy. Otherwise psutil metrics
    // (CPU/RAM/Disk) still send a fresh timestamp while LHM is down, which
    // would make the error screen's "Last OK" track the current time.
    strncpy(metricData.timestamp, ts, 5);
    metricData.timestamp[5] = '\0';
  } else if (!(ts && strlen(ts) > 0)) {
    // Empty timestamp signals stale data from Python script (LHM may be down)
    // Keep the previous timestamp - don't overwrite with empty
    Serial.println("Warning: Empty timestamp received (LHM may be recovering)");
  }

  JsonArray metricsArray = doc["metrics"];
  metricData.count = 0;

  for (JsonObject metricObj : metricsArray) {
    if (metricData.count >= MAX_METRICS) break;

    Metric& m = metricData.metrics[metricData.count];

    m.id = metricObj["id"] | 0;

    const char* name = metricObj["name"];
    if (name) {
      strncpy(m.name, name, METRIC_NAME_LEN - 1);
      m.name[METRIC_NAME_LEN - 1] = '\0';
      trimTrailingSpaces(m.name);
    }

    const char* unit = metricObj["unit"];
    if (unit) {
      strncpy(m.unit, unit, METRIC_UNIT_LEN - 1);
      m.unit[METRIC_UNIT_LEN - 1] = '\0';
    }

    m.value = metricObj["value"] | 0;

    if (m.id > 0 && m.id <= MAX_METRICS) {
      bool nameMatches = (settings.metricNames[m.id - 1][0] == '\0' ||
                          strcmp(settings.metricNames[m.id - 1], m.name) == 0);

      if (nameMatches) {
        if (settings.metricLabels[m.id - 1][0] != '\0') {
          strncpy(m.label, settings.metricLabels[m.id - 1], METRIC_NAME_LEN - 1);
          m.label[METRIC_NAME_LEN - 1] = '\0';
        } else {
          strncpy(m.label, m.name, METRIC_NAME_LEN - 1);
          m.label[METRIC_NAME_LEN - 1] = '\0';
        }

        m.displayOrder = settings.metricOrder[m.id - 1];
        m.companionId = settings.metricCompanions[m.id - 1];
        m.position = settings.metricPositions[m.id - 1];
        m.barPosition = settings.metricBarPositions[m.id - 1];
        m.barMin = settings.metricBarMin[m.id - 1];
        m.barMax = settings.metricBarMax[m.id - 1];
        m.barWidth = settings.metricBarWidths[m.id - 1];
        m.barOffsetX = settings.metricBarOffsets[m.id - 1];

        strncpy(settings.metricNames[m.id - 1], m.name, METRIC_NAME_LEN - 1);
        settings.metricNames[m.id - 1][METRIC_NAME_LEN - 1] = '\0';
      } else {
        Serial.printf("Metric ID %d name changed: '%s' -> '%s', using defaults\n",
                      m.id, settings.metricNames[m.id - 1], m.name);

        strncpy(m.label, m.name, METRIC_NAME_LEN - 1);
        m.label[METRIC_NAME_LEN - 1] = '\0';
        m.displayOrder = metricData.count;
        m.companionId = 0;
        m.position = 255;
        m.barPosition = 255;
        m.barMin = 0;
        m.barMax = 100;
        m.barWidth = 60;
        m.barOffsetX = 0;

        strncpy(settings.metricNames[m.id - 1], m.name, METRIC_NAME_LEN - 1);
        settings.metricNames[m.id - 1][METRIC_NAME_LEN - 1] = '\0';
        settings.metricLabels[m.id - 1][0] = '\0';
      }
    } else {
      strncpy(m.label, m.name, METRIC_NAME_LEN - 1);
      m.label[METRIC_NAME_LEN - 1] = '\0';
      m.displayOrder = metricData.count;
      m.companionId = 0;
      m.position = 255;
      m.barPosition = 255;
      m.barMin = 0;
      m.barMax = 100;
      m.barWidth = 60;
      m.barOffsetX = 0;
    }

    metricData.count++;
  }

  metricData.online = true;

  Serial.print("Received ");
  Serial.print(metricData.count);
  Serial.print(" metrics, ");
  int visibleCount = 0;
  for (int i = 0; i < metricData.count; i++) {
    if (metricData.metrics[i].position != 255) visibleCount++;
  }
  Serial.print(visibleCount);
  Serial.println(" visible (position assigned)");
}

bool parseStats(const char* json) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return false;
  }

  parseStatsV2(doc);
  return true;
}

// ========== Display Status Screens ==========
void displaySetupInstructions() {
  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(20, 0);
  display.println("WiFi Setup");
  display.drawLine(0, 10, 128, 10, DISPLAY_WHITE);

  display.setCursor(0, 14);
  display.println("1.Connect to WiFi:");

  display.setCursor(0, 26);
  display.print("  ");
  display.println(AP_NAME);

  display.setCursor(0, 38);
  if (strlen(AP_PASSWORD) > 0) {
    display.print("  Pass: ");
    display.println(AP_PASSWORD);
  } else {
    display.println("  (no password)");
  }

  display.setCursor(0, 50);
  display.println("2.Open 192.168.4.1");

  display.display();
}

#if QR_SETUP_ENABLED
void displayQRCodeSetup() {
  // WiFi QR format: open AP uses T:nopass, secured uses T:WPA
  char qrData[80];
  if (strlen(AP_PASSWORD) > 0) {
    snprintf(qrData, sizeof(qrData), "WIFI:T:WPA;S:%s;P:%s;;", AP_NAME, AP_PASSWORD);
  } else {
    snprintf(qrData, sizeof(qrData), "WIFI:T:nopass;S:%s;;", AP_NAME);
  }

  // QR Version 3 = 29x29 modules, fits 53 alphanumeric chars with ECC_LOW
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeBytes, 3, ECC_LOW, qrData);

  display.clearDisplay();

  // Layout: text on left, QR code on right
  // QR: 29x29 modules * 2px = 58x58 pixels, right-aligned, vertically centered
  const uint8_t qrSize = qrcode.size;       // 29 for Version 3
  const uint8_t pixelSize = 2;              // 2x2 pixels per module
  const uint8_t qrDisplaySize = qrSize * pixelSize;  // 58 pixels
  const uint8_t qrX = SCREEN_WIDTH - qrDisplaySize - 1;  // right side with 1px margin
  const uint8_t qrY = (SCREEN_HEIGHT - qrDisplaySize) / 2;  // vertically centered

  // Draw QR code
  for (uint8_t y = 0; y < qrSize; y++) {
    for (uint8_t x = 0; x < qrSize; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        display.fillRect(qrX + (x * pixelSize), qrY + (y * pixelSize),
                         pixelSize, pixelSize, DISPLAY_WHITE);
      }
    }
  }

  // Text labels on the left side (68px available, ~11 chars at 6px each)
  display.setTextSize(1);
  display.setCursor(0, 4);
  display.println("Scan QR");
  display.setCursor(0, 14);
  display.println("to join");
  display.setCursor(0, 24);
  display.println("WiFi");

  display.setCursor(0, 42);
  display.println("Open:");
  display.setCursor(0, 52);
  display.println("192.168.4.1");

  display.display();
}
#endif

void displayConnecting() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 25);
  display.println("Connecting to");
  display.setCursor(30, 40);
  display.println("WiFi...");
  display.display();
}

void displayConnected() {
  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(25, 4);
  display.println("Connected!");

  display.setCursor(8, 18);
  display.println("IP (for Python):");

  String ip = WiFi.localIP().toString();
  int ip_width = ip.length() * 6;
  int ip_x = (SCREEN_WIDTH - ip_width) / 2;
  display.setCursor(ip_x, 30);
  display.println(ip.c_str());

  display.drawLine(0, 42, 128, 42, DISPLAY_WHITE);

  display.setCursor(4, 48);
  display.println("Open IP in browser");
  display.setCursor(12, 56);
  display.println("to change settings");

  display.display();
}

void displayErrorStatus(uint8_t status) {
  display.clearDisplay();
  display.setTextSize(1);

  // Header with warning
  display.setCursor(30, 0);
  display.println("PC MONITOR");
  display.drawLine(0, 10, 128, 10, DISPLAY_WHITE);

  // Status icon (exclamation mark in box)
  display.drawRect(4, 16, 20, 20, DISPLAY_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 18);
  display.print("!");
  display.setTextSize(1);

  // Status message
  display.setCursor(30, 18);
  switch (status) {
    case STATUS_API_ERROR:
      display.println("LHM API Error");
      display.setCursor(30, 28);
      display.println("Check REST API");
      break;
    case STATUS_LHM_NOT_RUNNING:
      display.println("LHM Not Running");
      display.setCursor(30, 28);
      display.println("Start LHM app");
      break;
    case STATUS_LHM_STARTING:
      display.println("LHM Starting");
      display.setCursor(30, 28);
      display.println("Please wait...");
      break;
    default:
      display.println("Unknown Error");
      display.setCursor(30, 28);
      display.print("Code: ");
      display.println(status);
      break;
  }

  display.drawLine(0, 42, 128, 42, DISPLAY_WHITE);

  // Show timestamp if available
  if (metricData.timestamp[0] != '\0') {
    display.setCursor(4, 48);
    display.print("Last OK: ");
    display.println(metricData.timestamp);
  }

  // Show IP for reference
  display.setCursor(4, 56);
  display.print("IP: ");
  display.println(WiFi.localIP().toString().c_str());

  display.display();
}

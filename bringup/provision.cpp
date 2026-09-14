// One-shot provisioning: put the secrets this board needs into NVS.
//
// Nothing secret is committed. Values arrive as build flags from
// provision_secrets.ini (gitignored; template provision_secrets.example.ini,
// filled by tools/provision_secrets.py), are written once, and the board is
// then flashed with the real firmware. Reading them back is not offered - NVS
// is where they live now - and nothing here prints a value, only a length.
//
//   python3 tools/provision_secrets.py check
//   pio run -e provision -t upload
//   pio run -e provision -t clean                           # drop the cache
//
// Namespaces, matching the modules that read them:
//   "fb" host/port/user/pass   flight board broker   (src/mqtt/mqtt_bus.cpp)
//   "yr" ais                   aisstream.io key      (src/yachtradar/yachtradar.cpp)
//   "rb" token/kind            Realtime Trains       (src/railboard/rtt_direct.cpp)
//   "aero" key                 FlightAware AeroAPI   (src/flightboard/aero_direct.cpp)

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static void report(const char *what, bool ok) {
  Serial.printf("  %-22s %s\n", what, ok ? "written" : "skipped (not supplied)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== provisioning NVS ===");

  Preferences p;
  bool any = false;

  p.begin("fb", false);
#ifdef PROV_MQTT_HOST
  p.putString("host", PROV_MQTT_HOST); any = true;
#endif
#ifdef PROV_MQTT_PORT
  p.putUShort("port", (uint16_t)PROV_MQTT_PORT); any = true;
#endif
#ifdef PROV_MQTT_USER
  p.putString("user", PROV_MQTT_USER); any = true;
#endif
#ifdef PROV_MQTT_PASS
  p.putString("pass", PROV_MQTT_PASS); any = true;
#endif
  p.end();
#ifdef PROV_MQTT_HOST
  report("fb/host", true);
#else
  report("fb/host", false);
#endif
#ifdef PROV_MQTT_USER
  report("fb/user + fb/pass", true);
#else
  report("fb/user + fb/pass", false);
#endif

  p.begin("yr", false);
#ifdef PROV_AIS_KEY
  p.putString("ais", PROV_AIS_KEY); any = true;
  report("yr/ais", true);
#else
  report("yr/ais", false);
#endif
  p.end();

  // Realtime Trains, for the rail board's direct fetch. The token as issued,
  // without "Bearer ". kind is "refresh" or "access"; left out, the firmware
  // tries the token as an access token and exchanges it on a 401.
  p.begin("rb", false);
#ifdef PROV_RTT_CLEAR
  p.remove("token");
  p.remove("kind");
  Serial.printf("  %-22s cleared\n", "rb/token + rb/kind");
  any = true;
#endif
#ifdef PROV_RTT_TOKEN
  if (p.putString("token", PROV_RTT_TOKEN)) {
    Serial.printf("  %-22s written, %u chars\n", "rb/token", (unsigned)strlen(PROV_RTT_TOKEN));
    any = true;
  } else {
    Serial.printf("  %-22s FAILED to write\n", "rb/token");
  }
#else
  report("rb/token", false);
#endif
#ifdef PROV_RTT_KIND
  p.putString("kind", PROV_RTT_KIND); any = true;
  Serial.printf("  %-22s %s\n", "rb/kind", PROV_RTT_KIND);
#endif
  p.end();

  // FlightAware AeroAPI, for the flight board's direct fetch: the key as
  // issued, sent as the x-apikey header. Only its length is printed.
  p.begin("aero", false);
#ifdef PROV_AEROAPI_CLEAR
  if (p.isKey("key")) p.remove("key");
  Serial.printf("  %-22s cleared\n", "aero/key");
  any = true;
#endif
#ifdef PROV_AEROAPI_KEY
  if (p.putString("key", PROV_AEROAPI_KEY)) {
    Serial.printf("  %-22s written, %u chars\n", "aero/key", (unsigned)strlen(PROV_AEROAPI_KEY));
    any = true;
  } else {
    Serial.printf("  %-22s FAILED to write\n", "aero/key");
  }
#else
  report("aero/key", false);
#endif
  p.end();

  Serial.println(any ? "=== done - now flash matrix-waveshare-rgb ==="
                     : "=== nothing supplied; pass -DPROV_... build flags ===");
}

void loop() { delay(1000); }

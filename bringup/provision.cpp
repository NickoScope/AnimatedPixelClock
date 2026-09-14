// One-shot provisioning: put the secrets this board needs into NVS.
//
// Nothing secret is committed. Values arrive as build flags from
// provision_secrets.ini (gitignored; template provision_secrets.example.ini),
// are written once, and the board is then flashed with the real firmware.
// Reading them back is not offered - NVS is where they live now.
//
//   cp provision_secrets.example.ini provision_secrets.ini   # then edit it
//   pio run -e provision -t upload
//   pio run -e provision -t clean                           # drop the cache
//
// An earlier version of this comment passed the values with
// --project-option, which this PlatformIO (6.1) does not have.
//
// Namespaces, matching the modules that read them:
//   "fb" host/port/user/pass   flight board broker   (src/flightboard/fb_mqtt.cpp)
//   "yr" ais                   aisstream.io key      (src/yachtradar/yachtradar.cpp)

#include <Arduino.h>
#include <Preferences.h>

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

  Serial.println(any ? "=== done - now flash matrix-waveshare-rgb ==="
                     : "=== nothing supplied; pass -DPROV_... build flags ===");
}

void loop() { delay(1000); }

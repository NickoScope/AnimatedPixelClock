#include "icon_store.h"

#if defined(CARDS_ENABLED)

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

#define ICON_DIR "/icons"

static bool     s_ready = false;
static char     s_cachedName[20] = "";
static uint16_t s_cached[ICON_PX];
static bool     s_cachedValid = false;

// Names come off an MQTT topic, so they are attacker-shaped by default. Only
// this alphabet, and no path separators or dots - a name is a name, not a path.
static bool validName(const char *n) {
  if (!n || !*n || strlen(n) > 16) return false;
  for (const char *p = n; *p; p++) {
    const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
    if (!ok) return false;
  }
  return true;
}

static String pathFor(const char *name) { return String(ICON_DIR "/") + name; }

void iconStoreBegin() {
  // begin(true) formats on a failed mount. Harmless if the animation store
  // already mounted it: LittleFS returns true for an existing mount.
  s_ready = LittleFS.begin(true);
  if (s_ready && !LittleFS.exists(ICON_DIR)) LittleFS.mkdir(ICON_DIR);
}

bool iconSave(const char *name, const uint8_t *data, size_t len) {
  if (!s_ready || !validName(name) || len != ICON_BYTES) return false;
  const String tmp = pathFor(name) + ".t";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.write(data, len);
  f.close();
  if (n != len) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(pathFor(name));
  if (!LittleFS.rename(tmp, pathFor(name))) { LittleFS.remove(tmp); return false; }
  if (!strcmp(s_cachedName, name)) s_cachedValid = false;   // it changed under us
  return true;
}

void iconRemove(const char *name) {
  if (!s_ready || !validName(name)) return;
  LittleFS.remove(pathFor(name));
  if (!strcmp(s_cachedName, name)) { s_cachedName[0] = '\0'; s_cachedValid = false; }
}

const uint16_t *iconGet(const char *name) {
  if (!s_ready || !validName(name)) return NULL;
  if (s_cachedValid && !strcmp(s_cachedName, name)) return s_cached;

  File f = LittleFS.open(pathFor(name), "r");
  if (!f) return NULL;
  if (f.size() != ICON_BYTES) { f.close(); return NULL; }
  uint8_t raw[ICON_BYTES];
  const size_t n = f.read(raw, sizeof(raw));
  f.close();
  if (n != ICON_BYTES) return NULL;
  // Big-endian on the wire, because that is how a human writes a colour down
  // and how every tool that makes one will emit it.
  for (uint16_t i = 0; i < ICON_PX; i++)
    s_cached[i] = (uint16_t)(raw[i * 2] << 8) | raw[i * 2 + 1];
  strncpy(s_cachedName, name, sizeof(s_cachedName) - 1);
  s_cachedName[sizeof(s_cachedName) - 1] = '\0';
  s_cachedValid = true;
  return s_cached;
}

#endif

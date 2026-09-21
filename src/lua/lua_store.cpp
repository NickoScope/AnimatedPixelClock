#include "lua_store.h"

#if defined(LUA_STORE_ENABLED)

#include <LittleFS.h>
#include <esp_heap_caps.h>

namespace {

struct Entry {
  char     stem[25];
  char     name[25];
  uint32_t bytes;
};

Entry   s_list[LUA_USER_MAX];
uint8_t s_count = 0;
bool    s_usable = false;

File     s_up;
char     s_upStem[25];
uint32_t s_upBytes = 0;
bool     s_upOpen = false;

bool validStem(const char *s) {
  if (!s || !*s) return false;
  size_t n = strlen(s);
  if (n > 24) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (!isalnum((unsigned char)c) && c != '_') return false;
  }
  return true;
}

// "my_effect" -> "MY EFFECT", exactly as gen_effects.py names a compiled-in
// one, so a script reads the same on the banner whichever way it arrived.
void displayName(const char *stem, char *out, size_t cap) {
  size_t i = 0;
  for (; stem[i] && i + 1 < cap; i++) out[i] = stem[i] == '_' ? ' ' : toupper((unsigned char)stem[i]);
  out[i] = '\0';
}

void rescan() {
  s_count = 0;
  if (!s_usable) return;
  File dir = LittleFS.open(LUA_STORE_DIR);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f && s_count < LUA_USER_MAX; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    const char *p = f.name();
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;
    const size_t n = strlen(p);
    if (n < 5 || strcmp(p + n - 4, ".lua") != 0) continue;   // upload.tmp and anything else
    char stem[25];
    const size_t sn = n - 4;
    if (sn > 24) continue;
    memcpy(stem, p, sn);
    stem[sn] = '\0';
    if (!validStem(stem)) continue;
    Entry &e = s_list[s_count];
    strncpy(e.stem, stem, sizeof(e.stem) - 1);
    e.stem[sizeof(e.stem) - 1] = '\0';
    displayName(e.stem, e.name, sizeof(e.name));
    e.bytes = (uint32_t)f.size();
    s_count++;
  }
}

void pathOf(const char *stem, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s.lua", LUA_STORE_DIR, stem);
}

}  // namespace

void luaStoreInit() {
  // The animation store mounts the same filesystem; LittleFS.begin() is
  // idempotent and returns true when it is already up, so the order the two
  // modules start in does not matter.
  s_usable = LittleFS.begin(true);
  if (!s_usable) {
    Serial.println("[luastore] LittleFS did not mount: uploaded effects are off");
    return;
  }
  if (!LittleFS.exists(LUA_STORE_DIR)) LittleFS.mkdir(LUA_STORE_DIR);
  if (LittleFS.exists(LUA_STORE_TMP)) LittleFS.remove(LUA_STORE_TMP);
  rescan();
  Serial.printf("[luastore] %u uploaded effect%s, %u B free\n",
                (unsigned)s_count, s_count == 1 ? "" : "s", (unsigned)luaStoreFreeBytes());
}

bool   luaStoreUsable()     { return s_usable; }
size_t luaStoreFreeBytes()  { return s_usable ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0; }
uint8_t luaStoreCount()     { return s_count; }

const char *luaStoreStem(uint8_t i) { return i < s_count ? s_list[i].stem : ""; }
const char *luaStoreName(uint8_t i) { return i < s_count ? s_list[i].name : ""; }
uint32_t    luaStoreBytes(uint8_t i) { return i < s_count ? s_list[i].bytes : 0; }

// --- the checks -----------------------------------------------------------

bool luaStoreValidate(const char *src, size_t len, char *err, size_t errlen) {
  if (len == 0) { snprintf(err, errlen, "the file is empty"); return false; }
  if (len > LUA_USER_SRC_MAX) {
    snprintf(err, errlen, "%u B is over the %u B a script may be",
             (unsigned)len, (unsigned)LUA_USER_SRC_MAX);
    return false;
  }

  // A script with no draw() cannot run, and lua_fx would refuse it after
  // spending the load budget. Saying so here costs nothing.
  if (!strstr(src, "draw")) {
    snprintf(err, errlen, "the script defines no draw()");
    return false;
  }

  // Bracket depth. Strings and comments are skipped so an honest script is not
  // refused for the brackets inside a message; anything missed can only refuse
  // a file that would have been fine, never admit one that would not.
  int depth = 0, worst = 0;
  size_t line = 1;
  for (size_t i = 0; i < len; i++) {
    const char c = src[i];
    if (c == '\n') { line++; continue; }
    if (c == '-' && i + 1 < len && src[i + 1] == '-') {
      if (i + 3 < len && src[i + 2] == '[' && src[i + 3] == '[') {      // --[[ ... ]]
        const char *e = strstr(src + i + 4, "]]");
        if (!e) break;
        i = (size_t)(e - src) + 1;
      } else {
        while (i < len && src[i] != '\n') i++;
        line++;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      const char q = c;
      for (i++; i < len && src[i] != q; i++) if (src[i] == '\\') i++;
      continue;
    }
    if (c == '[' && i + 1 < len && src[i + 1] == '[') {                 // [[ long string ]]
      const char *e = strstr(src + i + 2, "]]");
      if (!e) break;
      i = (size_t)(e - src) + 1;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') {
      depth++;
      if (depth > worst) worst = depth;
      if (depth > LUA_USER_DEPTH_MAX) {
        snprintf(err, errlen,
                 "brackets nested %d deep at line %u; this panel takes %d. The "
                 "effect task has a 12 KB stack and Lua's parser recurses with "
                 "the nesting",
                 depth, (unsigned)line, LUA_USER_DEPTH_MAX);
        return false;
      }
    } else if (c == ')' || c == ']' || c == '}') {
      if (depth > 0) depth--;
    }
  }
  return true;
}

// --- reading --------------------------------------------------------------

char *luaStoreRead(uint8_t i, size_t *lenOut) {
  if (lenOut) *lenOut = 0;
  if (i >= s_count) return nullptr;
  char path[48];
  pathOf(s_list[i].stem, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  const size_t n = f.size();
  if (n == 0 || n > LUA_USER_SRC_MAX) { f.close(); return nullptr; }
  // PSRAM: the internal heap is this board's scarce one and a script is
  // kilobytes. The buffer lives only while the effect does.
  char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); return nullptr; }
  const size_t got = f.read((uint8_t *)buf, n);
  f.close();
  buf[got] = '\0';
  if (lenOut) *lenOut = got;
  return buf;
}

void luaStoreRelease(char *src) { if (src) heap_caps_free(src); }

bool luaStoreDelete(const char *stem) {
  if (!s_usable || !validStem(stem)) return false;
  char path[48];
  pathOf(stem, path, sizeof(path));
  if (!LittleFS.exists(path)) return false;
  const bool ok = LittleFS.remove(path);
  rescan();
  return ok;
}

// --- upload ---------------------------------------------------------------

bool luaStoreBegin(const char *stem, char *err, size_t errlen) {
  luaStoreAbort();
  if (!s_usable) { snprintf(err, errlen, "no filesystem"); return false; }
  if (!validStem(stem)) {
    snprintf(err, errlen, "the name must be 1 to 24 of letters, digits and underscore");
    return false;
  }
  // A replacement is not a new slot. Only a genuinely new name needs one.
  char path[48];
  pathOf(stem, path, sizeof(path));
  if (!LittleFS.exists(path) && s_count >= LUA_USER_MAX) {
    snprintf(err, errlen, "all %d uploaded slots are used; delete one first", LUA_USER_MAX);
    return false;
  }
  s_up = LittleFS.open(LUA_STORE_TMP, "w");
  if (!s_up) { snprintf(err, errlen, "could not open the upload file"); return false; }
  strncpy(s_upStem, stem, sizeof(s_upStem) - 1);
  s_upStem[sizeof(s_upStem) - 1] = '\0';
  s_upBytes = 0;
  s_upOpen = true;
  return true;
}

bool luaStoreWrite(const uint8_t *data, size_t len) {
  if (!s_upOpen) return false;
  if (s_upBytes + len > LUA_USER_SRC_MAX) { luaStoreAbort(); return false; }
  if (s_up.write(data, len) != len) { luaStoreAbort(); return false; }
  s_upBytes += len;
  return true;
}

bool luaStoreFinish(char *err, size_t errlen) {
  if (!s_upOpen) { snprintf(err, errlen, "nothing was uploaded"); return false; }
  s_up.close();
  s_upOpen = false;

  // Validate what actually landed, not what was promised. The file is read back
  // into PSRAM for it, because the checks want the whole source at once and the
  // internal heap is not the place for 24 KB.
  File f = LittleFS.open(LUA_STORE_TMP, "r");
  if (!f) { snprintf(err, errlen, "could not read the upload back"); LittleFS.remove(LUA_STORE_TMP); return false; }
  const size_t n = f.size();
  char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); LittleFS.remove(LUA_STORE_TMP); snprintf(err, errlen, "no PSRAM to check it"); return false; }
  const size_t got = f.read((uint8_t *)buf, n);
  f.close();
  buf[got] = '\0';

  const bool ok = luaStoreValidate(buf, got, err, errlen);
  heap_caps_free(buf);
  if (!ok) { LittleFS.remove(LUA_STORE_TMP); return false; }

  char path[48];
  pathOf(s_upStem, path, sizeof(path));
  if (LittleFS.exists(path)) LittleFS.remove(path);
  if (!LittleFS.rename(LUA_STORE_TMP, path)) {
    LittleFS.remove(LUA_STORE_TMP);
    snprintf(err, errlen, "could not store the script");
    return false;
  }
  rescan();
  Serial.printf("[luastore] %s.lua stored, %u B\n", s_upStem, (unsigned)got);
  return true;
}

void luaStoreAbort() {
  if (s_upOpen) { s_up.close(); s_upOpen = false; }
  if (s_usable && LittleFS.exists(LUA_STORE_TMP)) LittleFS.remove(LUA_STORE_TMP);
  s_upBytes = 0;
}

#endif  // LUA_STORE_ENABLED

// The guard comes first: lua_store.h #errors without LUA_EFFECTS_ENABLED, and
// the bench environments inherit LUA_STORE_ENABLED while unflagging the
// effects. Including the header unconditionally broke both of their builds.
#if defined(LUA_STORE_ENABLED)
#include "lua_store.h"

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

// rescan() runs on the loop task, from an upload or a delete; luaStoreStem(),
// luaStoreName() and luaStoreRead() run on the effect task on core 0. Without
// this the reader could be inside an entry while the writer zeroes the count
// and overwrites it - and open the wrong file, or a half-written name.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

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
  // Built aside and published in one critical section, so a reader never sees
  // a list that is half rebuilt. The filesystem walk itself is far too slow to
  // hold a spinlock across.
  Entry   fresh[LUA_USER_MAX];
  uint8_t kept = 0;
  if (!s_usable) {
    portENTER_CRITICAL(&s_mux);
    s_count = 0;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  File dir = LittleFS.open(LUA_STORE_DIR);
  if (!dir || !dir.isDirectory()) {
    portENTER_CRITICAL(&s_mux);
    s_count = 0;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  for (File f = dir.openNextFile(); f && kept < LUA_USER_MAX; f = dir.openNextFile()) {
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
    Entry &e = fresh[kept];
    strncpy(e.stem, stem, sizeof(e.stem) - 1);
    e.stem[sizeof(e.stem) - 1] = '\0';
    displayName(e.stem, e.name, sizeof(e.name));
    e.bytes = (uint32_t)f.size();
    kept++;
  }
  portENTER_CRITICAL(&s_mux);
  memcpy(s_list, fresh, sizeof(Entry) * kept);
  s_count = kept;
  portEXIT_CRITICAL(&s_mux);
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
  const uint8_t n = luaStoreCount();
  Serial.printf("[luastore] %u uploaded effect%s, %u B free\n",
                (unsigned)n, n == 1 ? "" : "s", (unsigned)luaStoreFreeBytes());
}

bool   luaStoreUsable()     { return s_usable; }
size_t luaStoreFreeBytes()  { return s_usable ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0; }
uint8_t luaStoreCount() {
  portENTER_CRITICAL(&s_mux);
  const uint8_t n = s_count;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

namespace {
bool copyEntry(uint8_t i, Entry *out) {
  portENTER_CRITICAL(&s_mux);
  const bool ok = i < s_count;
  if (ok) *out = s_list[i];
  portEXIT_CRITICAL(&s_mux);
  return ok;
}
}  // namespace

bool luaStoreStem(uint8_t i, char *out, size_t cap) {
  if (!out || cap == 0) return false;
  Entry e;
  if (!copyEntry(i, &e)) { out[0] = '\0'; return false; }
  strncpy(out, e.stem, cap - 1);
  out[cap - 1] = '\0';
  return true;
}

bool luaStoreName(uint8_t i, char *out, size_t cap) {
  if (!out || cap == 0) return false;
  Entry e;
  if (!copyEntry(i, &e)) { out[0] = '\0'; return false; }
  strncpy(out, e.name, cap - 1);
  out[cap - 1] = '\0';
  return true;
}

uint32_t luaStoreBytes(uint8_t i) {
  Entry e;
  return copyEntry(i, &e) ? e.bytes : 0;
}

// --- the checks -----------------------------------------------------------
//
// This is a lexer, not a bracket counter, and the first version of it was a
// bracket counter - which the audit broke in two ways worth recording:
//
//   * it stopped at an unterminated `--[[` and returned TRUE, so a file that
//     never closed its comment was ACCEPTED. The header promised the opposite.
//   * it did not know levelled long brackets, so `[=[ )))) ]=]` was scanned as
//     code and the stray closers pushed the counter back down. Depth measured
//     below depth real.
//
// And it was measuring the wrong thing. Lua's parser calls enterlevel from
// subexpr, statement and restassign (lparser.c:1262, 1846, 1384), so a nested
// block costs a level with no bracket in sight - and the most expensive cycle
// of all, nested `local function`, is exactly that: statement 96 + body 144 +
// statlist 32 = 272 bytes a level, all of it invisible to brackets.
//
// So both are counted, and every path that cannot make sense of the source
// refuses it.

namespace {

// Is there a long bracket at i - `[[`, `[=[`, `[==[` ...? Returns its level, or
// -1. A bare `[` is indexing and is not one.
int longOpen(const char *s, size_t len, size_t i) {
  if (s[i] != '[') return -1;
  size_t j = i + 1, eq = 0;
  while (j < len && s[j] == '=') { eq++; j++; }
  return (j < len && s[j] == '[') ? (int)eq : -1;
}

// Past the matching close of a long bracket of this level, or len + 1 when there
// is none. Not len: a string that closes on the very last byte of the file ends
// exactly at len, and returning len for both cases refused it as unterminated.
size_t longClose(const char *s, size_t len, size_t i, int level) {
  for (; i < len; i++) {
    if (s[i] != ']') continue;
    size_t j = i + 1, eq = 0;
    while (j < len && s[j] == '=') { eq++; j++; }
    if ((int)eq == level && j < len && s[j] == ']') return j + 1;
  }
  return len + 1;
}

inline bool wordChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// A keyword at i, on its own rather than inside an identifier.
bool wordAt(const char *s, size_t len, size_t i, const char *kw) {
  const size_t n = strlen(kw);
  if (i + n > len || memcmp(s + i, kw, n) != 0) return false;
  if (i > 0 && wordChar(s[i - 1])) return false;
  return i + n == len || !wordChar(s[i + n]);
}

}  // namespace

bool luaStoreValidate(const char *src, size_t len, char *err, size_t errlen) {
  if (!src) { snprintf(err, errlen, "no source"); return false; }
  if (len == 0) { snprintf(err, errlen, "the file is empty"); return false; }
  if (len > LUA_USER_SRC_MAX) {
    snprintf(err, errlen, "%u B is over the %u B a script may be",
             (unsigned)len, (unsigned)LUA_USER_SRC_MAX);
    return false;
  }

  int  brackets = 0, blocks = 0, worst = 0;
  bool sawDraw = false;
  unsigned line = 1;

  for (size_t i = 0; i < len; i++) {
    const char c = src[i];

    if (c == '\n') { line++; continue; }

    // Comments, short and long, before anything else reads a '-'.
    if (c == '-' && i + 1 < len && src[i + 1] == '-') {
      const int lvl = (i + 2 < len) ? longOpen(src, len, i + 2) : -1;
      if (lvl >= 0) {
        const size_t after = longClose(src, len, i + 2 + (size_t)lvl + 2, lvl);
        if (after > len) {
          snprintf(err, errlen, "a long comment opened at line %u is never closed", line);
          return false;
        }
        for (size_t k = i; k < after; k++) if (src[k] == '\n') line++;
        i = after - 1;
      } else {
        while (i + 1 < len && src[i + 1] != '\n') i++;
      }
      continue;
    }

    // Long strings.
    {
      const int lvl = longOpen(src, len, i);
      if (lvl >= 0) {
        const size_t after = longClose(src, len, i + (size_t)lvl + 2, lvl);
        if (after > len) {
          snprintf(err, errlen, "a long string opened at line %u is never closed", line);
          return false;
        }
        for (size_t k = i; k < after; k++) if (src[k] == '\n') line++;
        i = after - 1;
        continue;
      }
    }

    // Quoted strings. An escape takes the next byte with it; a newline inside
    // one is a syntax error in Lua, and refusing here says so earlier.
    if (c == '"' || c == '\'') {
      const char q = c;
      size_t j = i + 1;
      for (; j < len && src[j] != q; j++) {
        if (src[j] == '\n') {
          snprintf(err, errlen, "a string on line %u runs past the end of the line", line);
          return false;
        }
        if (src[j] == '\\') j++;
      }
      if (j >= len) {
        snprintf(err, errlen, "a string opened at line %u is never closed", line);
        return false;
      }
      i = j;
      continue;
    }

    if (c == '(' || c == '[' || c == '{') {
      brackets++;
    } else if (c == ')' || c == ']' || c == '}') {
      if (--brackets < 0) {
        snprintf(err, errlen, "a closing bracket at line %u has nothing to close", line);
        return false;
      }
    } else if (wordChar(c)) {
      // `for`/`while` are not counted: their own `do` opens the block, and
      // counting both would refuse honest code. `if` closes with `end`,
      // `repeat` with `until`. This mapping is exact, not conservative.
      if (wordAt(src, len, i, "do") || wordAt(src, len, i, "if") ||
          wordAt(src, len, i, "function") || wordAt(src, len, i, "repeat")) {
        blocks++;
      } else if (wordAt(src, len, i, "end") || wordAt(src, len, i, "until")) {
        if (--blocks < 0) {
          snprintf(err, errlen, "an `%s` at line %u closes a block that was never opened",
                   wordAt(src, len, i, "end") ? "end" : "until", line);
          return false;
        }
      } else if (wordAt(src, len, i, "draw")) {
        sawDraw = true;
      }
      while (i + 1 < len && wordChar(src[i + 1])) i++;   // past the identifier
      continue;
    }

    const int depth = brackets + blocks;
    if (depth > worst) worst = depth;
    if (depth > LUA_USER_DEPTH_MAX) {
      snprintf(err, errlen,
               "nested %d deep at line %u; this panel takes %d. Lua's parser "
               "recurses with the nesting and the effect task has a 12 KB stack",
               depth, line, LUA_USER_DEPTH_MAX);
      return false;
    }
  }

  if (brackets != 0) {
    snprintf(err, errlen, "%d bracket%s left open at the end of the file",
             brackets, brackets == 1 ? "" : "s");
    return false;
  }
  if (blocks != 0) {
    snprintf(err, errlen, "%d block%s left open at the end of the file",
             blocks, blocks == 1 ? "" : "s");
    return false;
  }
  if (!sawDraw) {
    // A word search, not a parse: it catches the script that forgot draw()
    // entirely, which is the common mistake, and lets `local draw = 1` through.
    // lua_fx.cpp does the real check after the chunk has run, where a global
    // called draw either is a function or is not.
    snprintf(err, errlen, "nothing in the script is called draw");
    return false;
  }
  return true;
}

// --- reading --------------------------------------------------------------

char *luaStoreRead(uint8_t i, size_t *lenOut) {
  if (lenOut) *lenOut = 0;
  Entry e;
  if (!copyEntry(i, &e)) return nullptr;
  char path[48];
  pathOf(e.stem, path, sizeof(path));
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

int luaStoreDelete(const char *stem) {
  if (!s_usable || !validStem(stem)) return LUA_STORE_ABSENT;
  char path[48];
  pathOf(stem, path, sizeof(path));
  if (!LittleFS.exists(path)) return LUA_STORE_ABSENT;
  const bool ok = LittleFS.remove(path);
  rescan();
  return ok ? LUA_STORE_OK : LUA_STORE_BUSY;
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
  if (!LittleFS.exists(path) && luaStoreCount() >= LUA_USER_MAX) {
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

  // The old copy is only destroyed once the new one is safely in place. The
  // other order - remove then rename - loses the script outright when the
  // rename fails, which on a full filesystem is exactly when it will.
  char path[48], old[56];
  pathOf(s_upStem, path, sizeof(path));
  const bool replacing = LittleFS.exists(path);
  snprintf(old, sizeof(old), "%s.old", path);
  if (replacing) {
    LittleFS.remove(old);
    if (!LittleFS.rename(path, old)) {
      LittleFS.remove(LUA_STORE_TMP);
      snprintf(err, errlen, "could not set the old script aside");
      return false;
    }
  }
  if (!LittleFS.rename(LUA_STORE_TMP, path)) {
    if (replacing) LittleFS.rename(old, path);      // put it back
    LittleFS.remove(LUA_STORE_TMP);
    snprintf(err, errlen, "could not store the script");
    return false;
  }
  if (replacing) LittleFS.remove(old);
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

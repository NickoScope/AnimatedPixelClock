#pragma once
// ============================================================
// lua_store.h - Lua effects that arrive over the air
// ============================================================
// Until now an effect could only be compiled in: gen_effects.py turned every
// script in tools/luasim/scripts/ into a C string literal and a new screen cost
// a full build and a flash. This is the other half - scripts uploaded to
// LittleFS at run time, shown beside the built-in ones, and removable the same
// way.
//
// Why it was not here already, from lua_effects.cpp:55-62: the effect task's
// stack is 12 KB *because* the scripts are compiled in, so "the parser never
// sees a hostile nesting depth", and "scripts arriving at run time would need
// that back [32 KB], and the internal heap would have to be measured for it".
// That measurement says no: the panel runs with about 21 KB of free internal
// heap and a largest block near 12 KB, so a 32 KB internal stack cannot be had.
//
// So the depth is bounded instead of the stack being grown. Two things do it,
// and it is worth being exact about which carries the weight, because an
// earlier version of this comment had it backwards.
//
//   * **The scan below is the load-bearing one.** Nothing reaches the parser
//     without passing it: luaStoreFinish runs it on the way in, and the load
//     path runs it again, so a file that arrives in /lua by some other route -
//     a filesystem image, a rolled-back firmware - is still checked. It counts
//     BLOCKS as well as brackets, because the expensive nesting has no bracket
//     in it at all, and it is a small lexer rather than a counter: levelled
//     long brackets and strings are understood, and every path that cannot make
//     sense of the source refuses the file. tools/luasim/store_test.py drives
//     24 cases through it, every one of them a bypass that once worked.
//
//   * LUAI_MAXCCALLS, lowered from Lua's default 200 (llimits.h:254) in
//     platformio.ini where the arithmetic is written out, is the backstop. Lua
//     counts its own recursion through luaE_incCstack and raises a catchable
//     "C stack overflow" past the limit. It is the second line, not the first:
//     at the cap alone the worst cycle would not fit the stack, which is why
//     the scan comes first and is not merely a nicer error message.
//
// The source itself is read into PSRAM, never the internal heap.
// ============================================================

#include <Arduino.h>
#include <stdint.h>

#if defined(LUA_STORE_ENABLED) && !defined(LUA_EFFECTS_ENABLED)
#error "LUA_STORE_ENABLED needs LUA_EFFECTS_ENABLED: it adds slots to the effect list"
#endif

// How many uploaded scripts the panel will hold. Each one is a page, and the
// pages are an enum fixed at build time, so the slots are reserved whether or
// not anything is in them; an empty one is simply not visitable.
#define LUA_USER_MAX 4

// The largest script accepted. la_gioconda, the biggest thing written for this
// panel so far, is about 9 KB with its whole picture embedded as a table.
#define LUA_USER_SRC_MAX (24U * 1024U)

// Combined bracket and block nesting refused beyond this. Chosen well below the
// LUAI_MAXCCALLS the interpreter enforces, so the clear error arrives first;
// nothing written by hand comes close - the deepest script in this repository
// reaches 6.
#define LUA_USER_DEPTH_MAX 16

#define LUA_STORE_DIR "/lua"
#define LUA_STORE_TMP "/lua/upload.tmp"

void        luaStoreInit();
bool        luaStoreUsable();
size_t      luaStoreFreeBytes();

uint8_t     luaStoreCount();

// Names are copied into the caller's buffer, never returned as a pointer. An
// earlier version handed out rotating statics and it was wrong twice: two slots
// are not enough for /api/lua, which asks for every name in one loop, and the
// two callers are on different tasks, so the rotation itself was a racing
// read-modify-write. ctrlToast keeps the pointer it is given until the banner
// goes, which made it worse. Both return false past the end and leave the
// buffer with an empty string.
bool luaStoreStem(uint8_t i, char *out, size_t cap);   // "my_effect"
bool luaStoreName(uint8_t i, char *out, size_t cap);   // "MY EFFECT", as the banner shows it
#define LUA_STORE_NAME_CAP 25

uint32_t    luaStoreBytes(uint8_t i);

// The source, in PSRAM. NUL-terminated. nullptr when it cannot be read; the
// caller owns it and must pass it back to luaStoreRelease().
char *luaStoreRead(uint8_t i, size_t *lenOut);
void  luaStoreRelease(char *src);

bool luaStoreDelete(const char *stem);

// Upload, chunk by chunk, the way the animation store takes a file. The name
// must be 1..24 of [A-Za-z0-9_] - the same rule gen_effects.py enforces on a
// compiled-in script, so a script can move either way without being renamed.
bool luaStoreBegin(const char *stem, char *err, size_t errlen);
bool luaStoreWrite(const uint8_t *data, size_t len);
bool luaStoreFinish(char *err, size_t errlen);
void luaStoreAbort();

// Everything the checks above would refuse, without writing anything. Exposed
// so a tool can be told why before it spends the upload.
//
// `src` must be NUL-terminated at src[len]; the scan reads no further than len
// but the contract is written down here because it is not obvious from the
// signature.
bool luaStoreValidate(const char *src, size_t len, char *err, size_t errlen);

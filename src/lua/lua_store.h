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
// So the depth is bounded instead of the stack being grown, in two places:
//
//   * LUAI_MAXCCALLS is lowered from Lua's default 200 (llimits.h:254) to
//     LUA_CCALLS_MAX below. Lua's parser counts its own recursion through
//     luaE_incCstack and raises "C stack overflow" past the limit, so the
//     interpreter guards itself - which is more trustworthy than anything
//     written here. The arithmetic: the built-in effects leave at least 7 KB of
//     the 12 KB stack free, a parser frame on Xtensa is of the order of 120
//     bytes, and 40 levels is therefore about 4.8 KB - inside the margin.
//
//   * a cheap scan before the source is ever handed to the parser, which
//     refuses a file that is too big or too deeply bracketed. It exists for the
//     error message: "nested too deeply" tells an author what to change, where
//     "C stack overflow" tells them nothing. A string or comment containing a
//     bracket can only make it refuse something it need not have - never the
//     reverse - and that is the right way round.
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

// Bracket nesting refused beyond this. Chosen well below LUA_CCALLS_MAX so the
// clear error arrives first; nothing written by hand comes close.
#define LUA_USER_DEPTH_MAX 32

#define LUA_STORE_DIR "/lua"
#define LUA_STORE_TMP "/lua/upload.tmp"

void        luaStoreInit();
bool        luaStoreUsable();
size_t      luaStoreFreeBytes();

uint8_t     luaStoreCount();
const char *luaStoreStem(uint8_t i);     // "my_effect"; "" past the end
const char *luaStoreName(uint8_t i);     // "MY EFFECT", as the banner shows it
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
bool luaStoreValidate(const char *src, size_t len, char *err, size_t errlen);

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
// The two bound different things, and saying which is which has taken three
// attempts to get right - both earlier versions of this comment overstated one
// of them.
//
//   * **The scan below bounds bracket and block nesting**, which is the
//     expensive kind for the PARSER: a `local function` inside a `local
//     function` costs 272 bytes of C stack a level and a table constructor
//     384, and a counter that watched only brackets would see neither. It is a
//     small lexer - levelled long brackets and strings are understood - and
//     every path that cannot make sense of the source refuses the file rather
//     than passing it on. luaStoreFinish runs it on the way in and the load
//     path runs it again, so a file that reaches /lua by some other route is
//     still measured. tools/luasim/store_test.py drives 34 cases through it,
//     every one a bypass that once worked.
//
//     What it does NOT bound: unary and right-associative operator chains
//     (`not not not x`), which recurse through subexpr with no bracket and no
//     block, and every kind of runtime recursion. Those are cheap - 64 bytes a
//     level - but they are not the scan's business.
//
//   * **LUAI_MAXCCALLS bounds everything else**, including the runtime. Lua
//     counts its own C recursion through luaE_incCstack and raises a catchable
//     "C stack overflow" past the limit. It is lowered from Lua's default 200
//     in platformio.ini, where the arithmetic is written out in full.
//
//     The worst case is not in the parser at all: nested pcall cost 560 bytes
//     a level, and `local function f() pcall(f) end` is three lines that no
//     amount of reading the source can recognise as deep. pcall and xpcall are
//     out of the sandbox for that reason (nslua_sandbox.cpp).
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
//
// Raised from 4 on 2026-09-22, with the arithmetic done rather than guessed.
// What a slot actually costs:
//
//   * 56 bytes of INTERNAL RAM in s_list[], permanently (lua_store.cpp:18 -
//     Entry is two 25-byte names and a uint32, padded). 4 slots were 224 B;
//     12 are 672. **+448 B of the roughly 21 KB free internal heap**, and that
//     heap is this board's scarce one - it is the only real price here.
//   * the same 56 bytes again in rescan()'s `fresh[]`, on the LOOP TASK's
//     8 KB stack, but only while a rescan runs. 672 B of frame is well inside
//     the -Wstack-usage=2048 ratchet this build enforces.
//   * one entry in the page enum (main.cpp), which ctrlPageVisitable() hides
//     while the slot is empty.
//
// What a slot does NOT cost: PSRAM (a script's buffer is allocated at the
// file's real size when it loads, and freed when the effect closes) and disk
// (LittleFS had 20.3 MB free with four scripts on it).
#define LUA_USER_MAX 12

// The largest script accepted. Raised from 24 KB on 2026-09-22: aquarium.lua
// reached 24,059 B of the old 24,576 and the next change to it would have had
// to buy its space by deleting prose.
//
// This one is free in internal RAM. Both buffers that hold a whole script -
// the read-back that luaStoreFinish validates and the one luaStoreRead hands
// the parser - are heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) at the file's
// REAL size, not at this cap, and PSRAM has 15.6 MB free. Twelve full 50 KB
// scripts are 600 KB of a 20.3 MB filesystem.
//
// What it does cost is parse time on the effect task: a 20 KB photograph
// script opens in about 40 ms, so 50 KB is of the order of 100 ms, inside the
// 500 ms a draw is allowed. Nesting depth, not length, is what bounds the
// parser's C stack, and that is LUA_USER_DEPTH_MAX and LUAI_MAXCCALLS below.
#define LUA_USER_SRC_MAX (50U * 1024U)

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

// Removing one. The three outcomes are kept apart because a caller answering
// HTTP has to: a file that is not there is a 404, and one the filesystem would
// not let go of - a reader still has it open - is not.
#define LUA_STORE_OK      0
#define LUA_STORE_ABSENT  1
#define LUA_STORE_BUSY    2
int luaStoreDelete(const char *stem);

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

#pragma once
// ============================================================
// lua_effects.h - the Lua effects, as pages the panel can show
// ============================================================
// The scripts in tools/luasim/scripts/ are embedded at build time
// (tools/luasim/gen_effects.py). Each one is a browse item of its own: the
// knob walks the clock styles, then each effect, then the other pages, and
// the carousel does the same. See src/lua/README.md.
//
// For the web UI. These four are all it needs, and all of them are safe to
// call from loop() - which is where WebServer handlers run. Only envs built
// with LUA_EFFECTS_ENABLED (and +<lua/> in build_src_filter) define them, so
// guard any call with #if defined(LUA_EFFECTS_ENABLED).
// ============================================================

#include <stdint.h>

#if defined(LUA_EFFECTS_ENABLED) && !defined(NSLUA_ENABLED)
#error "LUA_EFFECTS_ENABLED needs NSLUA_ENABLED: the effects run on the nslua runtime"
#endif
#if defined(LUA_EFFECTS_ENABLED) && !defined(CONTROL_ENCODER_ENABLED)
#error "LUA_EFFECTS_ENABLED needs CONTROL_ENCODER_ENABLED: each effect is one of the knob's pages"
#endif

uint8_t     luaEffectCount();             // compiled-in plus uploaded; changes at run time
uint8_t     luaEffectSlots();             // how many pages are reserved, empty ones included
bool        luaEffectUploaded(uint8_t i); // true when this one came over the air
const char *luaEffectName(uint8_t i);     // "ROOM RADAR"; "" past the end
void        luaEffectShow(uint8_t i);     // make it the page on screen now
int16_t     luaEffectCurrent();           // -1 when none is showing
// Step off whatever is running. Deleting an uploaded script moves every index
// above it, so the selection has to be dropped rather than left pointing at a
// slot that now holds something else.
void        luaEffectStop();

#pragma once
// ============================================================
// lua_effects_page.h - what main.cpp needs to run the Lua effects as pages
// ============================================================
// lua_effects.h is the public face (the web UI codes against it); these are
// the hooks the page machinery in main.cpp calls. All from loop() or setup().
// ============================================================

#include "lua_effects.h"
#include "lua_effects_scripts.h"   // LUA_EFFECT_COUNT, for the page enum

#if defined(LUA_EFFECTS_ENABLED)

#if LUA_EFFECT_COUNT < 1
#error "LUA_EFFECTS_ENABLED with no embedded scripts: run tools/luasim/gen_effects.py"
#endif

// setup(), once nslua_begin() has found PSRAM: the effect task and its frame
// buffers. Without it every effect page shows "unavailable".
void    luaEffectsBegin();
// loop(): the index luaEffectShow() asked for since the last call, or -1.
int16_t luaEffectsTakeShowRequest();
// loop(), every pass: which effect the current page is, -1 for none. A change
// opens that effect on the task (closing the previous one) and names it in
// the banner.
void    luaEffectsSelect(int16_t index);
// Render tick, between the clear and the flip: the newest finished frame, a
// black screen while the script loads, or its error message. Writes every
// pixel, so the frame need not be cleared first.
void    luaEffectsRender();
// For getOptimalRefreshRate(): the effect's frame cap, lowered to the rate
// the task is actually producing frames at, so a slow effect is not blitted
// three times per frame.
int     luaEffectsRefreshHz();

#endif  // LUA_EFFECTS_ENABLED

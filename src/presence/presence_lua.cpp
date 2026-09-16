// The `presence` table the room_radar scene reads, bound into an effect's Lua
// state by lua_fx.cpp before the chunk runs. See src/presence/presence.h.
//
// Every function here pushes numbers and nothing else: no table is built, no
// string is made, nothing is allocated. draw() calls into this 19 times a
// frame (three targets, the four counts behind the empty-room fade, and
// fourteen trail steps per target), and the effect's Lua heap is PSRAM shared
// with the HUB75 driver, so a live frame must cost no memory at all.
//
// A slot that is empty answers nil, which is what the scene tests. Multiple
// return values carry x, y and the speed, so the scene fills its own three
// preallocated tables rather than being handed new ones.

#include "presence.h"

#if defined(PRESENCE_ENABLED)

extern "C" {
#include "../lua/vendor/lua/lauxlib.h"
#include "../lua/vendor/lua/lua.h"
}

namespace {

int l_scale(lua_State *L) {
  lua_pushnumber(L, (lua_Number)presenceScaleM());
  return 1;
}

int l_state(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)presenceSourceCode());
  return 1;
}

// presence.count(k): targets held k steps of 0.1 s ago; k = 0 is now.
int l_count(lua_State *L) {
  const lua_Integer k = luaL_optinteger(L, 1, 0);
  lua_pushinteger(L, (lua_Integer)presenceCountBack(k < 0 || k > 255 ? 255 : (uint8_t)k));
  return 1;
}

// presence.target(i) -> x mm, y mm, speed cm/s, or nil.
int l_target(lua_State *L) {
  const lua_Integer i = luaL_checkinteger(L, 1);
  int32_t x = 0, y = 0;
  int16_t v = 0;
  if (i < 1 || i > 3 || !presenceTarget((uint8_t)(i - 1), &x, &y, &v)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, x);
  lua_pushinteger(L, y);
  lua_pushinteger(L, v);
  return 3;
}

// presence.trail(i, k) -> x mm, y mm, or nil.
int l_trail(lua_State *L) {
  const lua_Integer i = luaL_checkinteger(L, 1);
  const lua_Integer k = luaL_checkinteger(L, 2);
  int32_t x = 0, y = 0;
  if (i < 1 || i > 3 || k < 0 || k > 255 ||
      !presenceTrail((uint8_t)(i - 1), (uint8_t)k, &x, &y)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, x);
  lua_pushinteger(L, y);
  return 2;
}

const luaL_Reg kFuncs[] = {
  {"scale", l_scale}, {"state", l_state}, {"count", l_count},
  {"target", l_target}, {"trail", l_trail}, {nullptr, nullptr},
};

}  // namespace

void presenceLuaOpen(lua_State *L) {
  luaL_newlib(L, kFuncs);        // once, at load: the table itself is not per-frame
  lua_setglobal(L, "presence");
}

#endif  // PRESENCE_ENABLED

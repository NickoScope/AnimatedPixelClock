// ============================================================
// px_snapshot.h - px.save() and px.restore(): the canvas put aside and back
// ============================================================
// A scene whose camera stands still - a golfer at the tee, a putt - is the
// same picture every frame except for what moves in it. Drawing the ground,
// the trees and the sky again each frame is most of a frame's time on the
// panel (410 ns a Lua instruction). With these a script draws the still part
// once, px.save()s it, and every later frame px.restore()s it and draws only
// what moves: the canvas is copied, 24 KB, in a fraction of a millisecond.
//
//   px.save()        -- the canvas as it is now, kept (one copy; a new save replaces it)
//   px.restore()     -- -> true, the kept copy back on the canvas; false if nothing was saved
//
// The copy is a Lua userdata held in the registry, so it comes out of the
// effect's own heap (the 4 MB cap counts it) and goes when the effect closes.
// Shared by the firmware (src/lua/lua_px.cpp) and luasim, like px_terrain.h.
// ============================================================
#ifndef PX_SNAPSHOT_H
#define PX_SNAPSHOT_H

#include <string.h>

static char pxs_key;   // its address is the registry key

static unsigned char *pxs_buffer(lua_State *L, size_t bytes, int create) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &pxs_key);
  unsigned char *b = (unsigned char *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (b || !create) return b;
  b = (unsigned char *)lua_newuserdatauv(L, bytes, 0);   // raises on no memory, as any allocation
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxs_key);
  return b;
}

static int px_save_lua(lua_State *L, const unsigned char *fb, size_t bytes) {
  unsigned char *b = pxs_buffer(L, bytes, 1);
  memcpy(b, fb, bytes);
  return 0;
}

static int px_restore_lua(lua_State *L, unsigned char *fb, size_t bytes) {
  const unsigned char *b = pxs_buffer(L, bytes, 0);
  if (!b) { lua_pushboolean(L, 0); return 1; }
  memcpy(fb, b, bytes);
  lua_pushboolean(L, 1);
  return 1;
}

#endif

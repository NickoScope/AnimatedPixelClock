// ============================================================
// px_snapshot.h - the canvas put aside, back, and mixed back in
// ============================================================
// A scene whose camera stands still - a golfer at the tee, a putt - is the
// same picture every frame except for what moves in it. Drawing the ground,
// the trees and the sky again each frame is most of a frame's time on the
// panel (410 ns a Lua instruction). With these a script draws the still part
// once, px.save()s it, and every later frame px.restore()s it and draws only
// what moves: the canvas is copied, 24 KB, in a fraction of a millisecond.
//
// Since 2.7.5 there are four of them, and they can be mixed back in - which is
// how one scene flows into the next instead of cutting or going through black:
// keep the last frame of the old scene, draw the new one, and mix the old one
// over it by a share that falls from 1 to 0.
//
//   px.save([n])            -- the canvas as it is now, kept in slot n (1..4, default 1)
//   px.restore([n])         -- -> true, slot n back on the canvas; false if it is empty
//   px.mix(n, a [, x, y, w, h])
//                           -- -> true: slot n over the canvas by a (0..1), in the box
//                              (the canvas by default); false if it is empty
//   px.forget(n)            -- slot n emptied, its 24 KB given back
//
// Each copy is a Lua userdata held in the registry, so it comes out of the
// effect's own heap (the 4 MB cap counts it) and goes when the effect closes.
// px.mix is in integers (px_raster.h's Q8), so it draws the same on the panel
// and in luasim. Shared by the firmware (src/lua/lua_px.cpp) and luasim, like
// px_terrain.h; px_raster.h must be included first.
// ============================================================
#ifndef PX_SNAPSHOT_H
#define PX_SNAPSHOT_H

#include <string.h>

#define PXS_SLOTS 4

// Their addresses are the registry keys. Include this header in one file of a
// program only: a second file would have its own keys, and its own snapshots.
static char pxs_key[PXS_SLOTS];

static int pxs_slot(lua_State *L, int arg, int optional) {
  const lua_Integer n = optional ? luaL_optinteger(L, arg, 1) : luaL_checkinteger(L, arg);
  luaL_argcheck(L, n >= 1 && n <= PXS_SLOTS, arg, "a snapshot slot is 1 to 4");
  return (int)n - 1;
}

static unsigned char *pxs_buffer(lua_State *L, int slot, size_t bytes, int create) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &pxs_key[slot]);
  unsigned char *b = (unsigned char *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (b || !create) return b;
  b = (unsigned char *)lua_newuserdatauv(L, bytes, 0);   // raises on no memory, as any allocation
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxs_key[slot]);
  return b;
}

static int px_save_lua(lua_State *L, const unsigned char *fb, size_t bytes) {
  unsigned char *b = pxs_buffer(L, pxs_slot(L, 1, 1), bytes, 1);
  memcpy(b, fb, bytes);
  return 0;
}

static int px_restore_lua(lua_State *L, unsigned char *fb, size_t bytes) {
  const unsigned char *b = pxs_buffer(L, pxs_slot(L, 1, 1), bytes, 0);
  if (!b) { lua_pushboolean(L, 0); return 1; }
  memcpy(fb, b, bytes);
  lua_pushboolean(L, 1);
  return 1;
}

static int px_forget_lua(lua_State *L) {
  const int slot = pxs_slot(L, 1, 0);
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxs_key[slot]);
  return 0;
}

// px.mix(n, a [, x, y, w, h]). *work: the pixels mixed.
static int px_mix_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  const int slot = pxs_slot(L, 1, 0);
  const int a = pxr_q8(luaL_checknumber(L, 2));
  int x0, y0, x1, y1;
  const int inBox = pxr_box(L, 3, w, h, &x0, &y0, &x1, &y1);   // checked first: a bad box is an error either way
  const unsigned char *b = pxs_buffer(L, slot, (size_t)w * h * 3, 0);
  *work = 0;
  if (!b) { lua_pushboolean(L, 0); return 1; }
  if (a > 0 && inBox) {
    const int ia = 256 - a;
    for (int y = y0; y <= y1; y++) {
      unsigned char *p = &fb[(y * w + x0) * 3];
      const unsigned char *q = &b[(y * w + x0) * 3];
      for (int i = 0; i < (x1 - x0 + 1) * 3; i++) p[i] = (unsigned char)((p[i] * ia + q[i] * a) >> 8);
    }
    *work = (unsigned long)(x1 - x0 + 1) * (unsigned long)(y1 - y0 + 1);
  }
  lua_pushboolean(L, 1);
  return 1;
}

#endif

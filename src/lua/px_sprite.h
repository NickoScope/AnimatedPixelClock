// ============================================================
// px_sprite.h - px.grab() and px.blit(): a picture cut out once, stamped often
// ============================================================
// A fish is forty calls a frame: a line per column of its body, its tail, its
// fins, its eye. On the panel (410 ns a Lua instruction) the fish are half of
// the aquarium's frame. But a fish at a given size, turn and beat of its tail
// looks the same every time it is in that pose. With these a script draws
// each pose once, cuts it out, and from then on puts the fish on the canvas
// with one call - mirrored when it swims the other way, dimmed and hazed by
// the water it is behind.
//
//   s, dx, dy = px.grab(x, y, w, h)
//     -- the canvas rectangle as a sprite. Black (0,0,0) is transparent, and
//     -- the sprite is trimmed to what is not: dx, dy say where its top-left
//     -- corner is from (x, y). nil when the rectangle holds only black.
//   px.blit(s, x, y [, flip [, mul [, r, g, b, a]]])
//     -- s's top-left at (x, y), clipped to the canvas, transparent pixels
//     -- skipped. flip mirrors it left to right. Each colour is scaled by
//     -- mul (default 1, held to 0..4), then mixed toward (r, g, b) by a
//     -- (0..1, default 0): out = c*mul*(1-a) + tint*a, clamped to 255.
//
// A sprite is a Lua string: its width and height in the first two bytes
// (1..255 each), then R G B for each pixel, row by row. A script can build
// one with string.char as well as grab it. Its bytes come out of the
// effect's own heap, and it goes when nothing holds it.
//
// Shared by the firmware (src/lua/lua_px.cpp) and luasim, like px_terrain.h;
// fx_parity.py compares them. Every loop is bounded by the canvas: a C loop
// never reaches the instruction hook. *work returns the pixels touched, which
// the firmware charges against the frame's budget.
// ============================================================
#ifndef PX_SPRITE_H
#define PX_SPRITE_H

#include <string.h>

// A coordinate: any finite number, floored; NaN and huge values held off the
// canvas so the arithmetic below cannot overflow.
static int pxsp_coord(lua_State *L, int i) {
  lua_Number v = luaL_checknumber(L, i);
  if (!(v >= -32768)) v = -32768;          // NaN lands here too
  if (v > 32767) v = 32767;
  int t = (int)v;
  return t - (v < (lua_Number)t);
}

static lua_Number pxsp_byte(lua_Number v) { return v >= 0 ? (v <= 255 ? v : 255) : 0; }

static int px_grab_lua(lua_State *L, const unsigned char *fb, int fbw, int fbh,
                       unsigned long *work) {
  int x = pxsp_coord(L, 1), y = pxsp_coord(L, 2);
  int w = pxsp_coord(L, 3), h = pxsp_coord(L, 4);
  if (w > 255) w = 255;
  if (h > 255) h = 255;
  *work = 16;
  // The rectangle, clipped to the canvas.
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > fbw ? fbw : x + w, y1 = y + h > fbh ? fbh : y + h;
  if (w < 1 || h < 1 || x0 >= x1 || y0 >= y1) { lua_pushnil(L); return 1; }
  *work += (unsigned long)((x1 - x0) * (y1 - y0)) / 16;
  // Trim to what is not black.
  int bx0 = x1, by0 = y1, bx1 = x0 - 1, by1 = y0 - 1;
  for (int yy = y0; yy < y1; yy++) {
    const unsigned char *p = fb + ((size_t)yy * fbw + x0) * 3;
    for (int xx = x0; xx < x1; xx++, p += 3) {
      if (p[0] | p[1] | p[2]) {
        if (xx < bx0) bx0 = xx;
        if (xx > bx1) bx1 = xx;
        if (yy < by0) by0 = yy;
        if (yy > by1) by1 = yy;
      }
    }
  }
  if (bx1 < bx0) { lua_pushnil(L); return 1; }
  const int sw = bx1 - bx0 + 1, sh = by1 - by0 + 1;
  const size_t n = 2 + (size_t)sw * sh * 3;
  luaL_Buffer b;
  unsigned char *o = (unsigned char *)luaL_buffinitsize(L, &b, n);
  o[0] = (unsigned char)sw;
  o[1] = (unsigned char)sh;
  for (int yy = 0; yy < sh; yy++)
    memcpy(o + 2 + (size_t)yy * sw * 3, fb + ((size_t)(by0 + yy) * fbw + bx0) * 3, (size_t)sw * 3);
  luaL_pushresultsize(&b, n);
  lua_pushinteger(L, bx0 - x);
  lua_pushinteger(L, by0 - y);
  return 3;
}

static int px_blit_lua(lua_State *L, unsigned char *fb, int fbw, int fbh,
                       unsigned long *work) {
  size_t len = 0;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
  if (len < 2) return luaL_argerror(L, 1, "not a sprite");
  const int sw = s[0], sh = s[1];
  if (sw < 1 || sh < 1 || len != 2 + (size_t)sw * sh * 3)
    return luaL_argerror(L, 1, "not a sprite (its size does not match its bytes)");
  const int x = pxsp_coord(L, 2), y = pxsp_coord(L, 3);
  const int flip = lua_toboolean(L, 4);
  lua_Number mul = luaL_optnumber(L, 5, 1), a = luaL_optnumber(L, 9, 0);
  if (!(mul >= 0)) mul = 0;                 // NaN too
  if (mul > 4) mul = 4;
  if (!(a >= 0)) a = 0;
  if (a > 1) a = 1;
  const lua_Number tr = pxsp_byte(luaL_optnumber(L, 6, 0));
  const lua_Number tg = pxsp_byte(luaL_optnumber(L, 7, 0));
  const lua_Number tb = pxsp_byte(luaL_optnumber(L, 8, 0));
  // Fixed point: the scale in 1/256ths, the tint's share added after.
  const int m = (int)(mul * (1 - a) * 256 + 0.5);
  const int ar = (int)(tr * a + 0.5), ag = (int)(tg * a + 0.5), ab = (int)(tb * a + 0.5);
  *work = 16;
  // The sprite rows and columns that land on the canvas.
  int c0 = x < 0 ? -x : 0, c1 = x + sw > fbw ? fbw - x : sw;
  int r0 = y < 0 ? -y : 0, r1 = y + sh > fbh ? fbh - y : sh;
  if (c0 >= c1 || r0 >= r1) return 0;
  *work += (unsigned long)((c1 - c0) * (r1 - r0)) / 16;
  for (int r = r0; r < r1; r++) {
    const unsigned char *row = s + 2 + (size_t)r * sw * 3;
    unsigned char *d = fb + ((size_t)(y + r) * fbw + x + c0) * 3;
    for (int c = c0; c < c1; c++, d += 3) {
      const unsigned char *p = row + (size_t)(flip ? sw - 1 - c : c) * 3;
      if (!(p[0] | p[1] | p[2])) continue;
      int vr = ((p[0] * m) >> 8) + ar, vg = ((p[1] * m) >> 8) + ag, vb = ((p[2] * m) >> 8) + ab;
      d[0] = (unsigned char)(vr > 255 ? 255 : vr);
      d[1] = (unsigned char)(vg > 255 ? 255 : vg);
      d[2] = (unsigned char)(vb > 255 ? 255 : vb);
    }
  }
  return 0;
}

#endif

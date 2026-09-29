// ============================================================
// px_layer.h - palettes, 8-bit layers, and moving the canvas as a whole
// ============================================================
// Shared by the firmware (src/lua/lua_px.cpp) and luasim, like px_raster.h,
// and in integers only for the same reason: the two must draw the same pixels
// whatever libm each has.
//
//   px.palette(stops)               -> pal, a 768-byte string: 256 RGB colours
//       stops = { {pos, r, g, b}, ... } with pos 0..255 rising; the colours
//       between two stops are interpolated, before the first and after the
//       last they hold
//   px.palette{"cos", {ar,ag,ab}, {br,bg,bb}, {cr,cg,cb}, {dr,dg,db}}
//       Inigo Quilez's cosine palette, colour(t) = a + b*cos(2pi(c*t + d)),
//       t = index/256, each of a, b, c, d per channel in 0..1 terms
//       (https://iquilezles.org/articles/palettes/). Clamped to [-16, 16].
//   px.pal(pal, i [, bri])           -> r, g, b: colour i (wrapped to 0..255),
//                                       scaled by bri in 0..1
//   px.layer([v])                    -> a layer: one byte a pixel, 128x64,
//                                       filled with v (0). Methods:
//       L:set(x, y, v)   L:get(x, y) -> v   L:fill(v)
//   px.capture(L)                    -- the canvas into the layer, each pixel's
//                                       brightest channel
//   px.show(L, pal [, offset [, bri [, mode]]])
//                                    -- the layer onto the canvas through the
//                                       palette: colour (v + offset) & 255,
//                                       scaled by bri. mode "set" (default),
//                                       "add" (saturating), "max" (per channel),
//                                       "skip0" (a 0 in the layer leaves the
//                                       canvas as it is). Stepping offset each
//                                       frame is palette cycling.
//   px.scroll(dx, dy [, wrap])       -- the canvas moved by whole pixels; what
//                                       comes in is black, or with wrap the part
//                                       that went out the other side
//   px.mirror(mode)                  -- "h": the left half onto the right,
//                                       mirrored; "v": the top onto the bottom;
//                                       "hv": the top-left quarter onto all four
// ============================================================
#ifndef PX_LAYER_H
#define PX_LAYER_H

#include <string.h>

#define PXL_META "px.layer"

// round(32767 * cos(2 pi k / 256)), k = 0..256: one table, the same everywhere.
static const short pxl_cos[257] = {
 32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,  32137,  31971,  31785,  31580,
   31356,  31113,  30852,  30571,  30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
   27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,  23170,  22594,  22005,  21403,
   20787,  20159,  19519,  18868,  18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
   12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,   6393,   5602,   4808,   4011,
    3212,   2410,   1608,    804,      0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
   -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793, -12539, -13279, -14010, -14732,
  -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
  -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510,
  -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
  -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757, -32767, -32757, -32728, -32678,
  -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
  -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832,
  -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
  -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793, -11039, -10278,
   -9512,  -8739,  -7962,  -7179,  -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
       0,    804,   1608,   2410,   3212,   4011,   4808,   5602,   6393,   7179,   7962,   8739,
    9512,  10278,  11039,  11793,  12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
   18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,
   25329,  25832,  26319,  26790,  27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
   30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,  32137,  32285,  32412,  32521,
   32609,  32678,  32728,  32757,  32767
};

// cos of a phase in 1/65536 of a turn, Q15, interpolated between table steps.
static int pxl_cosq(unsigned phase) {
  const unsigned k = (phase >> 8) & 255, f = phase & 255;
  return pxl_cos[k] + (((pxl_cos[k + 1] - pxl_cos[k]) * (int)f) >> 8);
}

static int pxl_clamp255(long long v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }
// A Lua number to 0..255 before any conversion: NaN to 0, and nothing out of
// range ever reaches the cast (that would be undefined).
static int pxl_num255(lua_Number v) { return !(v > 0) ? 0 : v >= 255 ? 255 : (int)v; }

// A Lua number to Q16, NaN to 0, clamped to [-16, 16]: scaling by 65536 is
// exact, and the one rounding is the conversion.
static long long pxl_q16(lua_Number v) {
  if (!(v == v)) return 0;
  if (v > 16) v = 16;
  if (v < -16) v = -16;
  return (long long)(v * 65536);
}

static int pxl_rawnum(lua_State *L, int t, int i, lua_Number *out) {
  lua_rawgeti(L, t, i);
  const int ok = lua_type(L, -1) == LUA_TNUMBER;
  if (ok) *out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return ok;
}

// px.palette(spec) -> a 768-byte string
static int px_palette_lua(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  unsigned char pal[768];
  lua_rawgeti(L, 1, 1);
  const int cosine = lua_type(L, -1) == LUA_TSTRING && strcmp(lua_tostring(L, -1), "cos") == 0;
  lua_pop(L, 1);
  if (cosine) {
    long long q[4][3];
    for (int p = 0; p < 4; p++) {
      lua_rawgeti(L, 1, p + 2);
      if (!lua_istable(L, -1)) return luaL_error(L, "px.palette: cos wants four tables of three numbers");
      for (int c = 0; c < 3; c++) {
        lua_Number v = 0;
        if (!pxl_rawnum(L, -1, c + 1, &v)) return luaL_error(L, "px.palette: cos wants four tables of three numbers");
        q[p][c] = pxl_q16(v);
      }
      lua_pop(L, 1);
    }
    for (int i = 0; i < 256; i++)
      for (int c = 0; c < 3; c++) {
        // phase in 1/65536 turn: c * i/256 + d, taken modulo a turn
        const long long ph = ((q[2][c] * i) >> 8) + q[3][c];
        const long long v16 = q[0][c] + ((q[1][c] * pxl_cosq((unsigned)(ph & 0xFFFF))) >> 15);
        pal[i * 3 + c] = (unsigned char)pxl_clamp255((v16 * 255) >> 16);
      }
  } else {
    const int n = (int)lua_rawlen(L, 1);
    if (n < 1 || n > 256) return luaL_error(L, "px.palette: 1 to 256 stops {pos, r, g, b}");
    unsigned char pos[256], col[256][3];   // bytes: the effect task's stack is small
    for (int k = 0; k < n; k++) {
      lua_rawgeti(L, 1, k + 1);
      if (!lua_istable(L, -1)) return luaL_error(L, "px.palette: stop %d is not {pos, r, g, b}", k + 1);
      lua_Number v[4];
      for (int j = 0; j < 4; j++)
        if (!pxl_rawnum(L, -1, j + 1, &v[j])) return luaL_error(L, "px.palette: stop %d is not {pos, r, g, b}", k + 1);
      lua_pop(L, 1);
      pos[k] = (unsigned char)pxl_num255(v[0]);
      for (int c = 0; c < 3; c++) col[k][c] = (unsigned char)pxl_num255(v[c + 1]);
      if (k > 0 && pos[k] < pos[k - 1]) return luaL_error(L, "px.palette: stop %d comes before the one above it", k + 1);
    }
    int k = 0;
    for (int i = 0; i < 256; i++) {
      while (k < n - 1 && pos[k + 1] <= i) k++;
      for (int c = 0; c < 3; c++) {
        int v;
        if (i <= pos[0]) v = col[0][c];
        else if (k >= n - 1) v = col[n - 1][c];
        else {
          const int span = pos[k + 1] - pos[k];
          v = span ? col[k][c] + ((col[k + 1][c] - col[k][c]) * (i - pos[k])) / span : col[k + 1][c];
        }
        pal[i * 3 + c] = (unsigned char)v;
      }
    }
  }
  lua_pushlstring(L, (const char *)pal, sizeof(pal));
  return 1;
}

static const unsigned char *pxl_checkpal(lua_State *L, int arg) {
  size_t n = 0;
  const char *s = luaL_checklstring(L, arg, &n);
  luaL_argcheck(L, n == 768, arg, "a palette is 768 bytes: px.palette makes one");
  return (const unsigned char *)s;
}

// px.pal(pal, i [, bri]) -> r, g, b
static int px_pal_lua(lua_State *L) {
  const unsigned char *p = pxl_checkpal(L, 1);
  const int i = (int)(luaL_checkinteger(L, 2) & 255);
  const int b = lua_isnoneornil(L, 3) ? 256 : pxr_q8(luaL_checknumber(L, 3));
  for (int c = 0; c < 3; c++) lua_pushinteger(L, (p[i * 3 + c] * b) >> 8);
  return 3;
}

// ---- layers: a full userdata of w * h bytes after a two-short header ----
typedef struct { unsigned short w, h; unsigned char v[1]; } PxLayer;

static PxLayer *pxl_check(lua_State *L, int arg) { return (PxLayer *)luaL_checkudata(L, arg, PXL_META); }

static int pxl_set(lua_State *L) {
  PxLayer *l = pxl_check(L, 1);
  const lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3), v = luaL_checkinteger(L, 4);
  if (x >= 0 && x < l->w && y >= 0 && y < l->h) l->v[y * l->w + x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
  return 0;
}
static int pxl_get(lua_State *L) {
  PxLayer *l = pxl_check(L, 1);
  const lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
  lua_pushinteger(L, (x >= 0 && x < l->w && y >= 0 && y < l->h) ? l->v[y * l->w + x] : 0);
  return 1;
}
static int pxl_fill(lua_State *L) {
  PxLayer *l = pxl_check(L, 1);
  const lua_Integer v = luaL_optinteger(L, 2, 0);
  memset(l->v, v < 0 ? 0 : v > 255 ? 255 : (int)v, (size_t)l->w * l->h);
  return 0;
}

// px.layer([v]) -> L
static int px_layer_lua(lua_State *L, int w, int h) {
  const lua_Integer v = luaL_optinteger(L, 1, 0);
  PxLayer *l = (PxLayer *)lua_newuserdatauv(L, sizeof(PxLayer) - 1 + (size_t)w * h, 0);
  l->w = (unsigned short)w; l->h = (unsigned short)h;
  memset(l->v, v < 0 ? 0 : v > 255 ? 255 : (int)v, (size_t)w * h);
  if (luaL_newmetatable(L, PXL_META)) {
    static const luaL_Reg m[] = {{"set", pxl_set}, {"get", pxl_get}, {"fill", pxl_fill}, {NULL, NULL}};
    luaL_newlib(L, m);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "px.layer");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
  return 1;
}

// px.capture(L): the canvas's brightest channel into the layer
static int px_capture_lua(lua_State *L, const unsigned char *fb, int w, int h, unsigned long *work) {
  PxLayer *l = pxl_check(L, 1);
  *work = 0;
  if (l->w != w || l->h != h) return luaL_error(L, "px.capture: the layer is not the canvas's size");
  for (int i = 0; i < w * h; i++) {
    const unsigned char *p = &fb[i * 3];
    unsigned char m = p[0] > p[1] ? p[0] : p[1];
    l->v[i] = m > p[2] ? m : p[2];
  }
  *work = (unsigned long)w * h;
  return 0;
}

// px.show(L, pal [, offset [, bri [, mode]]])
static const char *const pxl_modes[] = {"set", "add", "max", "skip0", NULL};
static int px_show_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  PxLayer *l = pxl_check(L, 1);
  const unsigned char *pal = pxl_checkpal(L, 2);
  const int off = (int)(luaL_optinteger(L, 3, 0) & 255);
  const int b = lua_isnoneornil(L, 4) ? 256 : pxr_q8(luaL_checknumber(L, 4));
  const int mode = luaL_checkoption(L, 5, "set", pxl_modes);
  *work = 0;
  if (l->w != w || l->h != h) return luaL_error(L, "px.show: the layer is not the canvas's size");
  unsigned char lut[768];                                   // the palette at this brightness, once
  for (int k = 0; k < 768; k++) lut[k] = (unsigned char)((pal[k] * b) >> 8);
  for (int i = 0; i < w * h; i++) {
    const int v = l->v[i];
    if (mode == 3 && v == 0) continue;
    const unsigned char *c = &lut[((v + off) & 255) * 3];
    unsigned char *p = &fb[i * 3];
    if (mode == 1) {
      for (int k = 0; k < 3; k++) { const int s = p[k] + c[k]; p[k] = (unsigned char)(s > 255 ? 255 : s); }
    } else if (mode == 2) {
      for (int k = 0; k < 3; k++) if (c[k] > p[k]) p[k] = c[k];
    } else {
      p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
    }
  }
  *work = (unsigned long)w * h;
  return 0;
}

// Reverse n cells of `size` bytes each, in place.
static void pxl_reverse(unsigned char *a, int n, int size) {
  unsigned char t[3];
  for (int i = 0, j = n - 1; i < j; i++, j--) {
    memcpy(t, a + i * size, (size_t)size);
    memcpy(a + i * size, a + j * size, (size_t)size);
    memcpy(a + j * size, t, (size_t)size);
  }
}
// Rotate n cells right by k (0 <= k < n): three reversals, no buffer.
static void pxl_rotate(unsigned char *a, int n, int k, int size) {
  if (k == 0) return;
  pxl_reverse(a, n, size);
  pxl_reverse(a, k, size);
  pxl_reverse(a + k * size, n - k, size);
}
static void pxl_reverse_rows(unsigned char *fb, int rows, int rowb) {
  for (int i = 0, j = rows - 1; i < j; i++, j--)
    for (int k = 0; k < rowb; k++) { const unsigned char t = fb[i * rowb + k]; fb[i * rowb + k] = fb[j * rowb + k]; fb[j * rowb + k] = t; }
}

// px.scroll(dx, dy [, wrap])
static int px_scroll_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  const lua_Integer dx = luaL_checkinteger(L, 1), dy = luaL_checkinteger(L, 2);
  const int wrap = lua_toboolean(L, 3);
  const int rowb = w * 3;
  *work = 0;
  if (wrap) {
    const int kx = (int)(((dx % w) + w) % w), ky = (int)(((dy % h) + h) % h);
    if (kx) for (int y = 0; y < h; y++) pxl_rotate(&fb[y * rowb], w, kx, 3);
    if (ky) { pxl_reverse_rows(fb, h, rowb); pxl_reverse_rows(fb, ky, rowb); pxl_reverse_rows(fb + ky * rowb, h - ky, rowb); }
  } else {
    if (dx >= w || dx <= -w || dy >= h || dy <= -h) { memset(fb, 0, (size_t)rowb * h); *work = (unsigned long)w * h; return 0; }
    const int ix = (int)dx, iy = (int)dy;
    if (ix) for (int y = 0; y < h; y++) {
      unsigned char *r = &fb[y * rowb];
      if (ix > 0) { memmove(r + ix * 3, r, (size_t)(w - ix) * 3); memset(r, 0, (size_t)ix * 3); }
      else        { memmove(r, r - ix * 3, (size_t)(w + ix) * 3); memset(r + (w + ix) * 3, 0, (size_t)(-ix) * 3); }
    }
    if (iy > 0) { memmove(fb + iy * rowb, fb, (size_t)(h - iy) * rowb); memset(fb, 0, (size_t)iy * rowb); }
    else if (iy < 0) { memmove(fb, fb - iy * rowb, (size_t)(h + iy) * rowb); memset(fb + (h + iy) * rowb, 0, (size_t)(-iy) * rowb); }
  }
  *work = (unsigned long)w * h;
  return 0;
}

// px.mirror(mode)
static const char *const pxl_mirrors[] = {"h", "v", "hv", NULL};
static int px_mirror_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  const int m = luaL_checkoption(L, 1, NULL, pxl_mirrors);
  const int rowb = w * 3;
  if (m == 0 || m == 2)
    for (int y = 0; y < (m == 2 ? h / 2 : h); y++)
      for (int x = 0; x < w / 2; x++) memcpy(&fb[y * rowb + (w - 1 - x) * 3], &fb[y * rowb + x * 3], 3);
  if (m == 1 || m == 2)
    for (int y = 0; y < h / 2; y++) memcpy(&fb[(h - 1 - y) * rowb], &fb[y * rowb], (size_t)rowb);
  *work = (unsigned long)w * h / 2;
  return 0;
}

#endif

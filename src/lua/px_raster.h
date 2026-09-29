// ============================================================
// px_raster.h - whole-canvas passes and blending, in integers
// ============================================================
// Shared by the firmware (src/lua/lua_px.cpp) and luasim, like px_sprite.h,
// so the two draw the same pixels by construction.
//
//   px.blend(x, y, r, g, b, a)       -- mix a colour into one pixel, a in [0,1]
//   px.glow(cx, cy, rad, r, g, b [, amp])
//                                    -- a radial light, (1 - d/rad)^2 * amp
//   px.fade(a [, r, g, b [, x, y, w, h]])
//                                    -- every pixel of the box (the canvas by
//                                       default) the fraction a of the way to
//                                       the colour (black by default)
//   px.blur(a [, x, y, w, h])        -- spread light to the neighbours, a in [0,1]
//   px.mode("add" | "set")           -- -> the mode before. "add" makes pixel,
//                                       rect, line, circle and text add their
//                                       colour to the canvas, saturating at
//                                       255, instead of overwriting it
//
// Why integers. The panel's FPU does single precision only, so blend and glow
// in double ran in software: 13.5 us a px.blend against 6.5 us a px.pixel, and
// 5.6 ms a glow of radius 10 (AGENTS.md, measured 2026-09-23). Integers are
// also the same on every compiler: no fused multiply-add, no libm sqrt to
// round differently between the Mac and the panel. Alphas are Q8 (256 = 1).
//
// blur is FastLED's blur2d (src/fl/gfx/blur.cpp.hpp, blur1d over the rows and
// then over the columns; scale8 with FASTLED_SCALE8_FIXED, adds saturating;
// read at FastLED master ec0a0f3, 2026-09-29): each pixel keeps (255 - a) and
// gives a/2 to each neighbour. FastLED's own note on it: light is not quite
// conserved, so repeated blurs also fade to black. a here is FastLED's
// blur_amount / 255.
// ============================================================
#ifndef PX_RASTER_H
#define PX_RASTER_H

// Q8 from a Lua fraction: NaN and anything <= 0 give 0, anything >= 1 gives 256.
static int pxr_q8(lua_Number a) {
  if (!(a > 0)) return 0;
  if (a >= 1) return 256;
  return (int)(a * 256 + (lua_Number)0.5);
}

// A channel from a Lua number: clamped to [0,255], NaN to 0, then truncated.
static int pxr_chan(lua_Number v) {
  if (!(v > 0)) return 0;
  if (v >= 255) return 255;
  return (int)v;
}

static void pxr_put(unsigned char *fb, int w, int h, int x, int y, int r, int g, int b, int add) {
  if (x < 0 || x >= w || y < 0 || y >= h) return;
  unsigned char *p = &fb[(y * w + x) * 3];
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  if (add) {
    r += p[0]; g += p[1]; b += p[2];
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
  }
  p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
}

// One pixel toward (r, g, b) by a/256, truncated.
static void pxr_mix(unsigned char *p, int r, int g, int b, int a) {
  const int ia = 256 - a;
  p[0] = (unsigned char)((p[0] * ia + r * a) >> 8);
  p[1] = (unsigned char)((p[1] * ia + g * a) >> 8);
  p[2] = (unsigned char)((p[2] * ia + b * a) >> 8);
}

static void pxr_blend(unsigned char *fb, int w, int h, lua_Integer x, lua_Integer y,
                      lua_Number r, lua_Number g, lua_Number b, lua_Number a) {
  if (x < 0 || x >= w || y < 0 || y >= h) return;
  const int q = pxr_q8(a);
  if (q == 0) return;
  pxr_mix(&fb[(y * w + x) * 3], pxr_chan(r), pxr_chan(g), pxr_chan(b), q);
}

// floor(sqrt(v))
static unsigned pxr_isqrt(unsigned v) {
  unsigned r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}

// Distances are in 1/Q of a pixel: Q = 64 up to a radius of 256, Q = 4 up to
// 4096 (so a squared distance stays inside 31 bits); beyond that nothing is
// drawn - the canvas is 128 wide. The box is clipped to the canvas first.
static void pxr_glow(unsigned char *fb, int w, int h, lua_Number cx, lua_Number cy, lua_Number rad,
                     lua_Number r, lua_Number g, lua_Number b, lua_Number amp) {
  if (!(rad >= (lua_Number)0.5) || !(rad <= 4096) || cx != cx || cy != cy) return;
  if (cx + rad < -2 || cx - rad > w + 1 || cy + rad < -2 || cy - rad > h + 1) return;
  const int Q = rad <= 256 ? 64 : 4;
  int x0 = (int)(cx - rad), x1 = (int)(cx + rad), y0 = (int)(cy - rad), y1 = (int)(cy + rad);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w - 1) x1 = w - 1;
  if (y1 > h - 1) y1 = h - 1;
  const int CX = (int)(cx * Q), CY = (int)(cy * Q), R = (int)(rad * Q);
  if (R <= 0) return;
  const unsigned R2 = (unsigned)R * (unsigned)R;
  lua_Number am = amp > 0 ? amp : 0;
  if (am > 4096) am = 4096;
  const int AMP = (int)(am * 256);
  if (AMP <= 0) return;
  const int cr = pxr_chan(r), cg = pxr_chan(g), cb = pxr_chan(b);
  for (int y = y0; y <= y1; y++) {
    const int dy = y * Q + Q / 2 - CY;
    for (int x = x0; x <= x1; x++) {
      const int dx = x * Q + Q / 2 - CX;
      const unsigned d2 = (unsigned)(dx * dx) + (unsigned)(dy * dy);
      if (d2 > R2) continue;
      const int d = (int)pxr_isqrt(d2);
      const int f = ((R - d) << 8) / R;                 // Q8, 1 at the centre
      int a = (int)(((long long)AMP * ((f * f) >> 8)) >> 8);
      if (a <= 0) continue;
      if (a > 256) a = 256;
      pxr_mix(&fb[(y * w + x) * 3], cr, cg, cb, a);
    }
  }
}

// The optional box of fade and blur, clipped to the canvas; false if nothing is left.
static int pxr_box(lua_State *L, int arg, int w, int h, int *x0, int *y0, int *x1, int *y1) {
  if (lua_isnoneornil(L, arg)) { *x0 = 0; *y0 = 0; *x1 = w - 1; *y1 = h - 1; return 1; }
  const long long x = luaL_checkinteger(L, arg), y = luaL_checkinteger(L, arg + 1);
  const long long bw = luaL_checkinteger(L, arg + 2), bh = luaL_checkinteger(L, arg + 3);
  const long long xa = x < 0 ? 0 : x, ya = y < 0 ? 0 : y;
  const long long xb = x + bw - 1 > w - 1 ? w - 1 : x + bw - 1;
  const long long yb = y + bh - 1 > h - 1 ? h - 1 : y + bh - 1;
  if (xa > xb || ya > yb) return 0;
  *x0 = (int)xa; *y0 = (int)ya; *x1 = (int)xb; *y1 = (int)yb;
  return 1;
}

// px.fade(a [, r, g, b [, x, y, w, h]]). Rounded toward the colour, so a pixel
// arrives at it instead of stopping one short. *work: the pixels touched.
static int px_fade_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  const int a = pxr_q8(luaL_checknumber(L, 1));
  const int tr = pxr_chan(luaL_optnumber(L, 2, 0)), tg = pxr_chan(luaL_optnumber(L, 3, 0)),
            tb = pxr_chan(luaL_optnumber(L, 4, 0));
  int x0, y0, x1, y1;
  *work = 0;
  if (!pxr_box(L, 5, w, h, &x0, &y0, &x1, &y1) || a == 0) return 0;
  const int ia = 256 - a;
  const int t[3] = {tr, tg, tb};
  for (int y = y0; y <= y1; y++) {
    unsigned char *p = &fb[(y * w + x0) * 3];
    for (int x = x0; x <= x1; x++)
      for (int c = 0; c < 3; c++, p++) {
        const int v = *p, tc = t[c];
        *p = (unsigned char)((v * ia + tc * a + (tc > v ? 255 : 0)) >> 8);
      }
  }
  *work = (unsigned long)(x1 - x0 + 1) * (unsigned long)(y1 - y0 + 1);
  return 0;
}

static unsigned char pxr_scale8(int i, int s) { return (unsigned char)((i * (1 + s)) >> 8); }
static unsigned char pxr_qadd8(int i, int j) { const int s = i + j; return (unsigned char)(s > 255 ? 255 : s); }

// blur1d over n pixels starting at p, stepping by `step` bytes.
static void pxr_blur1d(unsigned char *p, int n, int step, int keep, int seep) {
  int cr = 0, cg = 0, cb = 0;                         // the carry-over, FastLED's `carryover`
  for (int i = 0; i < n; i++, p += step) {
    const int pr = pxr_scale8(p[0], seep), pg = pxr_scale8(p[1], seep), pb = pxr_scale8(p[2], seep);
    const int r = pxr_qadd8(pxr_scale8(p[0], keep), cr), g = pxr_qadd8(pxr_scale8(p[1], keep), cg),
              b = pxr_qadd8(pxr_scale8(p[2], keep), cb);
    if (i) { p[-step] = pxr_qadd8(p[-step], pr); p[1 - step] = pxr_qadd8(p[1 - step], pg); p[2 - step] = pxr_qadd8(p[2 - step], pb); }
    p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
    cr = pr; cg = pg; cb = pb;
  }
}

// px.blur(a [, x, y, w, h]). *work: the pixels touched, twice (rows, then columns).
static int px_blur_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  lua_Number a = luaL_checknumber(L, 1);
  int amount = !(a > 0) ? 0 : a >= 1 ? 255 : (int)(a * 255 + (lua_Number)0.5);
  int x0, y0, x1, y1;
  *work = 0;
  if (!pxr_box(L, 2, w, h, &x0, &y0, &x1, &y1) || amount == 0) return 0;
  const int keep = 255 - amount, seep = amount >> 1;
  for (int y = y0; y <= y1; y++) pxr_blur1d(&fb[(y * w + x0) * 3], x1 - x0 + 1, 3, keep, seep);
  for (int x = x0; x <= x1; x++) pxr_blur1d(&fb[(y0 * w + x) * 3], y1 - y0 + 1, w * 3, keep, seep);
  *work = 2ul * (unsigned long)(x1 - x0 + 1) * (unsigned long)(y1 - y0 + 1);
  return 0;
}

// px.mode(m) -> the mode before; *add is the mode's home in the caller.
static const char *const pxr_modes[] = {"set", "add", NULL};
static int px_mode_lua(lua_State *L, int *add) {
  const int before = *add;
  if (!lua_isnoneornil(L, 1)) *add = luaL_checkoption(L, 1, NULL, pxr_modes);
  lua_pushstring(L, pxr_modes[before ? 1 : 0]);
  return 1;
}

#endif

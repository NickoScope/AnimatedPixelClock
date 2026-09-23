// ============================================================
// lua_px.cpp - px.*, ported call for call from tools/luasim/luasim.c
// ============================================================
// Every drawing rule here is luasim's: the colour clamp in put(), the wrap
// in clear(), the Bresenham variant, the midpoint circle, the text baseline
// at y + (yAdvance - 1) + yOffset, blend's truncation, glow's (1 - d/rad)^2.
// fx_parity.py holds this file to luasim pixel for pixel.
//
// The only departures are bounds on work the reference would do off-canvas,
// because a C loop never reaches the instruction hook and a stuck one would
// freeze the effect task:
//   - spans, rect rows and glow's box are clipped to the canvas first. That
//     cannot change a pixel: put() and blend_px() drop off-canvas writes.
//   - text stops at the right edge. Exact for Picopixel: every xOffset is 0
//     and every advance is positive, so no later glyph can come back.
//   - a circle whose bounding box misses the canvas draws nothing (exact);
//     one with a radius over 16384 draws nothing (luasim would spend minutes).
//   - a line with an endpoint beyond +-4096 is clipped to a box just around
//     the canvas and then drawn: it can land a pixel away from luasim's.
// No shipped script comes near any of these.
// ============================================================
#include "lua_px.h"

#if defined(LUA_EFFECTS_ENABLED) || !defined(ARDUINO)

#include <math.h>
#include <stdlib.h>

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
}

#include "../fonts/pxfb_text.h"   // Picopixel, Latin and Cyrillic

#define W LUA_PX_W
#define H LUA_PX_H

static inline LuaPxCanvas *canvasOf(lua_State *L) {
  return static_cast<LuaPxCanvas *>(lua_touserdata(L, lua_upvalueindex(1)));
}

static inline int clampInt(int64_t v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : (int)v;
}

static void put(uint8_t *fb, int x, int y, int r, int g, int b) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  uint8_t *p = &fb[(y * W + x) * 3];
  p[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
  p[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
  p[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

static int l_size(lua_State *L) { lua_pushinteger(L, W); lua_pushinteger(L, H); return 2; }
static int l_t(lua_State *L)    { lua_pushnumber(L, (lua_Number)canvasOf(L)->clock.phase); return 1; }

static int l_now(lua_State *L) {
  const LuaPxClock &c = canvasOf(L)->clock;
  lua_newtable(L);
  lua_pushinteger(L, c.hour); lua_setfield(L, -2, "hour");
  lua_pushinteger(L, c.min);  lua_setfield(L, -2, "min");
  lua_pushinteger(L, c.sec);  lua_setfield(L, -2, "sec");
  lua_pushinteger(L, c.yday); lua_setfield(L, -2, "yday");
  // luasim hands over whole hours as an integer. Keep that type wherever the
  // zone allows it; India's +5:30 has to arrive as 5.5.
  if (c.utcMinutes % 60 == 0) lua_pushinteger(L, c.utcMinutes / 60);
  else                        lua_pushnumber(L, (lua_Number)(c.utcMinutes / 60.0));
  lua_setfield(L, -2, "utc");
  lua_pushinteger(L, c.year); lua_setfield(L, -2, "year");
  return 1;
}

static int l_clear(lua_State *L) {
  const int r = (int)luaL_optinteger(L, 1, 0);
  const int g = (int)luaL_optinteger(L, 2, 0);
  const int b = (int)luaL_optinteger(L, 3, 0);
  uint8_t *fb = canvasOf(L)->rgb;
  // Not clamped, as in luasim: the int is narrowed to a byte.
  for (int i = 0; i < W * H; i++) { fb[i*3] = (uint8_t)r; fb[i*3+1] = (uint8_t)g; fb[i*3+2] = (uint8_t)b; }
  return 0;
}

static int l_pixel(lua_State *L) {
  put(canvasOf(L)->rgb,
      (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
      (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
      (int)luaL_checkinteger(L, 5));
  return 0;
}

// x0 and x1 arrive already limited to [-1, W], which keeps their order.
static void hline(uint8_t *fb, int x0, int x1, int y, int r, int g, int b) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  if (y < 0 || y >= H) return;
  if (x0 < 0) x0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  for (int x = x0; x <= x1; x++) put(fb, x, y, r, g, b);
}

static int l_rect(lua_State *L) {
  const int64_t x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
  const int64_t w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
  const int r = (int)luaL_checkinteger(L, 5), g = (int)luaL_checkinteger(L, 6),
            b = (int)luaL_checkinteger(L, 7);
  const int fill = lua_toboolean(L, 8);
  uint8_t *fb = canvasOf(L)->rgb;
  const int xa = clampInt(x, -1, W), xb = clampInt(x + w - 1, -1, W);
  // Only the rows j in [0, h) that land on the canvas.
  const int64_t j0 = y < 0 ? -y : 0;
  const int64_t j1 = (h - 1 < (H - 1) - y) ? h - 1 : (H - 1) - y;
  if (fill) {
    for (int64_t j = j0; j <= j1; j++) hline(fb, xa, xb, (int)(y + j), r, g, b);
  } else {
    hline(fb, xa, xb, clampInt(y, -1, H), r, g, b);
    hline(fb, xa, xb, clampInt(y + h - 1, -1, H), r, g, b);
    for (int64_t j = j0; j <= j1; j++) {
      put(fb, xa, (int)(y + j), r, g, b);
      put(fb, xb, (int)(y + j), r, g, b);
    }
  }
  return 0;
}

// Liang-Barsky against a box one pixel wider than the canvas all round.
static bool clipLine(double &x0, double &y0, double &x1, double &y1) {
  const double xmin = -2, ymin = -2, xmax = W + 1, ymax = H + 1;
  const double dx = x1 - x0, dy = y1 - y0;
  double t0 = 0, t1 = 1;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) { if (q[i] < 0) return false; continue; }
    const double t = q[i] / p[i];
    if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
    else          { if (t < t0) return false; if (t < t1) t1 = t; }
  }
  const double ox = x0, oy = y0;
  x0 = ox + t0 * dx; y0 = oy + t0 * dy;
  x1 = ox + t1 * dx; y1 = oy + t1 * dy;
  return true;
}

static int l_line(lua_State *L) {
  int x0 = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2);
  int x1 = (int)luaL_checkinteger(L, 3), y1 = (int)luaL_checkinteger(L, 4);
  const int r = (int)luaL_checkinteger(L, 5), g = (int)luaL_checkinteger(L, 6),
            b = (int)luaL_checkinteger(L, 7);
  uint8_t *fb = canvasOf(L)->rgb;
  const int LIM = 4096;
  if (abs(x0) > LIM || abs(y0) > LIM || abs(x1) > LIM || abs(y1) > LIM) {
    double fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    if (!clipLine(fx0, fy0, fx1, fy1)) return 0;
    x0 = (int)lround(fx0); y0 = (int)lround(fy0);
    x1 = (int)lround(fx1); y1 = (int)lround(fy1);
  }
  const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int e = dx + dy;
  for (;;) {
    put(fb, x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
  return 0;
}

static int l_circle(lua_State *L) {
  const int64_t cx = luaL_checkinteger(L, 1), cy = luaL_checkinteger(L, 2);
  const int64_t rad = luaL_checkinteger(L, 3);
  const int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
            b = (int)luaL_checkinteger(L, 6);
  const int fill = lua_toboolean(L, 7);
  if (rad < 0 || rad > 16384) return 0;
  if (cx + rad < 0 || cx - rad >= W || cy + rad < 0 || cy - rad >= H) return 0;
  uint8_t *fb = canvasOf(L)->rgb;
  const int icx = (int)cx, icy = (int)cy;   // within 16384 of the canvas here
  int x = (int)rad, y = 0, d = 1 - (int)rad;
  while (x >= y) {
    if (fill) {
      hline(fb, clampInt(icx - x, -1, W), clampInt(icx + x, -1, W), icy + y, r, g, b);
      hline(fb, clampInt(icx - x, -1, W), clampInt(icx + x, -1, W), icy - y, r, g, b);
      hline(fb, clampInt(icx - y, -1, W), clampInt(icx + y, -1, W), icy + x, r, g, b);
      hline(fb, clampInt(icx - y, -1, W), clampInt(icx + y, -1, W), icy - x, r, g, b);
    } else {
      put(fb, icx+x, icy+y, r, g, b); put(fb, icx+y, icy+x, r, g, b);
      put(fb, icx-x, icy+y, r, g, b); put(fb, icx-y, icy+x, r, g, b);
      put(fb, icx+x, icy-y, r, g, b); put(fb, icx+y, icy-x, r, g, b);
      put(fb, icx-x, icy-y, r, g, b); put(fb, icx-y, icy-x, r, g, b);
    }
    y++;
    if (d < 0) d += 2 * y + 1; else { x--; d += 2 * (y - x) + 1; }
  }
  return 0;
}

// Picopixel as the rest of the firmware draws it: the corrected font, read
// through the GFXfont tables with drawChar's bit order, now through
// src/fonts/pxfb_text.h so a UTF-8 string draws its Cyrillic. Latin keeps its
// case folding (a..z as A..Z); ASCII draws exactly as it always has.
static int l_text(lua_State *L) {
  const int x = (int)luaL_checkinteger(L, 1);
  const int y = (int)luaL_checkinteger(L, 2);
  const char *s = luaL_checkstring(L, 3);
  const int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
            b = (int)luaL_checkinteger(L, 6);
  uint8_t *fb = canvasOf(L)->rgb;
  const int base = y + (PicopixelFB.yAdvance - 1);
  // stopX = W: see the note at the top
  pxfbDraw(s, x, base, true, W, [&](int px, int py) { put(fb, px, py, r, g, b); });
  return 0;
}

static int l_width(lua_State *L) {
  lua_pushinteger(L, pxfbWidth(luaL_checkstring(L, 1), true));
  return 1;
}

static int l_get(lua_State *L) {
  const int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  if (x < 0 || x >= W || y < 0 || y >= H) {
    lua_pushinteger(L, 0); lua_pushinteger(L, 0); lua_pushinteger(L, 0);
    return 3;
  }
  const uint8_t *p = &canvasOf(L)->rgb[(y * W + x) * 3];
  lua_pushinteger(L, p[0]); lua_pushinteger(L, p[1]); lua_pushinteger(L, p[2]);
  return 3;
}

static void blend_px(uint8_t *fb, int x, int y, double r, double g, double b, double a) {
  if (!(a > 0.0) || x < 0 || x >= W || y < 0 || y >= H) return;   // also NaN
  if (a > 1.0) a = 1.0;
  // Out-of-range or NaN channels would make the (int) casts below undefined.
  r = r >= 0.0 ? (r <= 255.0 ? r : 255.0) : 0.0;
  g = g >= 0.0 ? (g <= 255.0 ? g : 255.0) : 0.0;
  b = b >= 0.0 ? (b <= 255.0 ? b : 255.0) : 0.0;
  uint8_t *p = &fb[(y * W + x) * 3];
  const double ir = 1.0 - a;
  const int nr = (int)(p[0] * ir + r * a), ng = (int)(p[1] * ir + g * a), nb = (int)(p[2] * ir + b * a);
  p[0] = (uint8_t)(nr > 255 ? 255 : nr);
  p[1] = (uint8_t)(ng > 255 ? 255 : ng);
  p[2] = (uint8_t)(nb > 255 ? 255 : nb);
}

static int l_blend(lua_State *L) {
  blend_px(canvasOf(L)->rgb,
           (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
           luaL_checknumber(L, 3), luaL_checknumber(L, 4), luaL_checknumber(L, 5),
           luaL_checknumber(L, 6));
  return 0;
}

static int l_glow(lua_State *L) {
  const double cx = luaL_checknumber(L, 1), cy = luaL_checknumber(L, 2), rad = luaL_checknumber(L, 3);
  const double r = luaL_checknumber(L, 4), g = luaL_checknumber(L, 5), b = luaL_checknumber(L, 6);
  const double amp = luaL_optnumber(L, 7, 1.0);
  if (!(rad >= 0.5) || cx != cx || cy != cy) return 0;   // also refuses NaN
  // luasim's box is (int)(c -+ rad), truncated toward zero. Limiting the
  // doubles to just outside the canvas first keeps the conversion defined and
  // drops only columns and rows blend_px would have ignored.
  auto box = [](double v, int hi) { return (int)(v < -2 ? -2 : v > hi + 1 ? hi + 1 : v); };
  int x0 = box(cx - rad, W), x1 = box(cx + rad, W), y0 = box(cy - rad, H), y1 = box(cy + rad, H);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;
  uint8_t *fb = canvasOf(L)->rgb;
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
      const double d = sqrt(dx*dx + dy*dy);
      if (d > rad) continue;
      const double f = 1.0 - d / rad;
      blend_px(fb, x, y, r, g, b, amp * f * f);
    }
  return 0;
}

static const luaL_Reg kPxLib[] = {
  {"get", l_get}, {"blend", l_blend}, {"glow", l_glow},
  {"size", l_size}, {"t", l_t}, {"now", l_now}, {"clear", l_clear},
  {"pixel", l_pixel}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle},
  {"text", l_text}, {"width", l_width}, {NULL, NULL}
};

void luaPxOpen(lua_State *L, LuaPxCanvas *canvas) {
  lua_createtable(L, 0, (int)(sizeof(kPxLib) / sizeof(kPxLib[0]) - 1));
  lua_pushlightuserdata(L, canvas);
  luaL_setfuncs(L, kPxLib, 1);           // every function gets the canvas as upvalue 1
  lua_setglobal(L, "px");
}

#endif  // LUA_EFFECTS_ENABLED || host

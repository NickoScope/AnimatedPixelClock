// ============================================================
// lua_px.cpp - px.*, ported call for call from tools/luasim/luasim.c
// ============================================================
// Every drawing rule here is luasim's: the wrap in clear(), the Bresenham
// variant, the midpoint circle, the text baseline at y + (yAdvance - 1) +
// yOffset. The colour clamp, the add mode, blend, glow, fade and blur are one
// code for both, px_raster.h. fx_parity.py holds this file to luasim pixel
// for pixel.
//
// The only departures are bounds on work the reference would do off-canvas,
// because a C loop never reaches the instruction hook and a stuck one would
// freeze the effect task:
//   - spans and rect rows are clipped to the canvas first. That cannot
//     change a pixel: put() drops off-canvas writes.
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
#include "../fonts/sys_text.h"    // the system font: the classic 5x7 too
#include "px_sprite.h"             // px.grab, px.blit, shared with luasim
#include "px_terrain.h"            // px.terrain, shared with luasim
#include "px_snapshot.h"           // px.save, px.restore, shared with luasim
#include "px_raster.h"             // put, blend, glow, fade, blur, mode, shared with luasim
#include "lua_fx.h"                // LuaFx::charge

#define W LUA_PX_W
#define H LUA_PX_H
static_assert(LUA_PX_H <= 64, "pxr_circle_fill keeps a span for each of at most 64 rows");

static inline LuaPxCanvas *canvasOf(lua_State *L) {
  return static_cast<LuaPxCanvas *>(lua_touserdata(L, lua_upvalueindex(1)));
}

static inline int clampInt(int64_t v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : (int)v;
}

// px.mode: 1 while "add". One Lua state draws at a time (the effect task
// closes the effect before it tries an upload), and luaPxOpen resets it, so a
// script never inherits another's mode.
static int s_add = 0;

static void put(uint8_t *fb, int x, int y, int r, int g, int b) {
  pxr_put(fb, W, H, x, y, r, g, b, s_add);
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
  // Every pixel once, so that px.mode("add") lights it once: the bottom row
  // only when it is not the top one, the sides only between them, the right
  // side only when it is not the left one. In "set" the pixels are the same.
  if (fill) {
    for (int64_t j = j0; j <= j1; j++) hline(fb, xa, xb, (int)(y + j), r, g, b);
  } else {
    hline(fb, xa, xb, clampInt(y, -1, H), r, g, b);
    if (h != 1) hline(fb, xa, xb, clampInt(y + h - 1, -1, H), r, g, b);
    for (int64_t j = j0 > 1 ? j0 : 1; j <= j1 && j <= h - 2; j++) {
      put(fb, xa, (int)(y + j), r, g, b);
      if (w != 1) put(fb, xb, (int)(y + j), r, g, b);
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
  const long long LIM = 4096;
  // In 64 bits: abs(INT_MIN) is undefined, and on the panel it stays negative,
  // which skipped the clip and left ~2^31 steps for the loop below.
  if (llabs((long long)x0) > LIM || llabs((long long)y0) > LIM ||
      llabs((long long)x1) > LIM || llabs((long long)y1) > LIM) {
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
  // Every pixel once, so that px.mode("add") lights it once (pxr_circle_*).
  if (fill) pxr_circle_fill(fb, W, H, icx, icy, (int)rad, r, g, b, s_add);
  else      pxr_circle_ring(fb, W, H, icx, icy, (int)rad, r, g, b, s_add);
  return 0;
}

// The system font (src/fonts/sys_text.h), UTF-8, Latin and Cyrillic. The
// optional last argument picks the font:
//   "small" (the default)  Picopixel with lowercase drawn as capitals, as
//                          px.text has always drawn: ASCII exactly as before
//   "pico"                 Picopixel with its own lowercase, Latin and Cyrillic
//   "5x7"                  the classic 5x7 font every screen prints with,
//                          capitals and lowercase; y is the top of the cell
// Glyphs are read with drawChar's bit order; a glyph that would start at or
// past the canvas's right edge is not drawn (see the note at the top).
static const char *const kFonts[] = {"small", "pico", "5x7", nullptr};

static int l_text(lua_State *L) {
  const int x = (int)luaL_checkinteger(L, 1);
  const int y = (int)luaL_checkinteger(L, 2);
  const char *s = luaL_checkstring(L, 3);
  const int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
            b = (int)luaL_checkinteger(L, 6);
  const int font = luaL_checkoption(L, 7, "small", kFonts);
  uint8_t *fb = canvasOf(L)->rgb;
  auto dot = [&](int px, int py) { put(fb, px, py, r, g, b); };
  if (font == 2) {
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
      if (cx >= W) break;
      const uint8_t *cols = sysClassicGlyph(utf8Next(&p));
      for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 8; gy++)
          if ((cols[gx] >> gy) & 1) dot(cx + gx, y + gy);
      cx += 6;
    }
    return 0;
  }
  const int base = y + (PicopixelFB.yAdvance - 1);
  pxfbDraw(s, x, base, font == 0, W, dot);
  return 0;
}

static int l_width(lua_State *L) {
  const char *s = luaL_checkstring(L, 1);
  const int font = luaL_checkoption(L, 2, "small", kFonts);
  lua_pushinteger(L, font == 2 ? sysTextLetters(s) * 6 : pxfbWidth(s, font == 0));
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

static int l_blend(lua_State *L) {
  pxr_blend(canvasOf(L)->rgb, W, H, luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
            luaL_checknumber(L, 3), luaL_checknumber(L, 4), luaL_checknumber(L, 5),
            luaL_checknumber(L, 6));
  return 0;
}

// glow, fade and blur are charged a quarter of an instruction a pixel they
// look at, the way px.blit is: our estimate until the panel measures them.
static int l_glow(lua_State *L) {
  const unsigned long work = pxr_glow(canvasOf(L)->rgb, W, H, luaL_checknumber(L, 1),
           luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4),
           luaL_checknumber(L, 5), luaL_checknumber(L, 6), luaL_optnumber(L, 7, 1));
  LuaFx::charge(L, (uint32_t)(work / 4));
  return 0;
}

static int l_fade(lua_State *L) {
  unsigned long work = 0;
  px_fade_lua(L, canvasOf(L)->rgb, W, H, &work);
  LuaFx::charge(L, (uint32_t)(work / 4));
  return 0;
}
static int l_blur(lua_State *L) {
  unsigned long work = 0;
  px_blur_lua(L, canvasOf(L)->rgb, W, H, &work);
  LuaFx::charge(L, (uint32_t)(work / 4));
  return 0;
}
static int l_mode(lua_State *L) { return px_mode_lua(L, &s_add); }

// A sample of the ground costs about what a Lua instruction does on the panel
// (100-odd cycles), so each one is charged as one: the frame's instruction
// budget and its deadline see the native work too.
static int l_terrain(lua_State *L) {
  unsigned long work = 0;
  px_terrain_lua(L, canvasOf(L)->rgb, W, H, &work);
  LuaFx::charge(L, (uint32_t)work);
  return 0;
}

// A 24 KB copy is charged as 1000 instructions, the way px.terrain charges its
// samples: a loop of them then meets the budget and the deadline like any other.
static int l_save(lua_State *L) {
  px_save_lua(L, canvasOf(L)->rgb, LUA_PX_BYTES);
  LuaFx::charge(L, 1000);
  return 0;
}
static int l_restore(lua_State *L) {
  const int n = px_restore_lua(L, canvasOf(L)->rgb, LUA_PX_BYTES);
  LuaFx::charge(L, 1000);
  return n;
}

// Charged like px.terrain: a quarter of an instruction a pixel touched, which
// is about what a stamp costs on the panel against a Lua instruction (the gate
// audit's estimate, 2026-09-24: a sixteenth under-charged it three to four times).
static int l_grab(lua_State *L) {
  unsigned long work = 0;
  const int n = px_grab_lua(L, canvasOf(L)->rgb, W, H, &work);
  LuaFx::charge(L, (uint32_t)work);
  return n;
}
static int l_blit(lua_State *L) {
  unsigned long work = 0;
  px_blit_lua(L, canvasOf(L)->rgb, W, H, &work);
  LuaFx::charge(L, (uint32_t)work);
  return 0;
}

// px.button() -> the click count (lua_px.h): a script reacts when it changes.
static int l_button(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)(canvasOf(L)->clicks & 0x7fffffff));
  return 1;
}

static const luaL_Reg kPxLib[] = {
  {"get", l_get}, {"blend", l_blend}, {"glow", l_glow},
  {"size", l_size}, {"t", l_t}, {"now", l_now}, {"clear", l_clear},
  {"pixel", l_pixel}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle},
  {"text", l_text}, {"width", l_width}, {"terrain", l_terrain},
  {"save", l_save}, {"restore", l_restore}, {"grab", l_grab}, {"blit", l_blit},
  {"button", l_button}, {"fade", l_fade}, {"blur", l_blur}, {"mode", l_mode},
  {NULL, NULL}
};

void luaPxOpen(lua_State *L, LuaPxCanvas *canvas) {
  s_add = 0;
  lua_createtable(L, 0, (int)(sizeof(kPxLib) / sizeof(kPxLib[0]) - 1));
  lua_pushlightuserdata(L, canvas);
  luaL_setfuncs(L, kPxLib, 1);           // every function gets the canvas as upvalue 1
  lua_setglobal(L, "px");
}

#endif  // LUA_EFFECTS_ENABLED || host

// ============================================================
// luasim.c - host simulator for the panel's Lua runtime
// ============================================================
// Runs the SAME vendored Lua 5.4.8 the firmware runs, against the same px.*
// raster API, on a 128x64 framebuffer - so an effect can be written and judged
// before the panels exist, and without a flash cycle.
//
// What it deliberately does NOT simulate: the PSRAM allocator, the instruction
// budget hook, the dedicated task's C stack. Those are hardware properties and
// the point of the bring-up bench, not of this tool.
//
//   cc -O2 -I../../src/lua/vendor/lua -o luasim luasim.c <lua .c files>
//   ./luasim script.lua frames out.raw
//
// Writes frames as raw RGB888, 128*64*3 bytes each; render.py makes the PNG.
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define W 128
#define H 64

static unsigned char fb[W * H * 3];
static double g_phase = 0.0;
static int    g_hour = 12, g_min = 34, g_sec = 56;

static void put(int x, int y, int r, int g, int b) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  unsigned char *p = &fb[(y * W + x) * 3];
  p[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
  p[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
  p[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
}

/* ---- px.* : the raster API the firmware will expose ---- */

static int l_size(lua_State *L)  { lua_pushinteger(L, W); lua_pushinteger(L, H); return 2; }
static int l_t(lua_State *L)     { lua_pushnumber(L, g_phase); return 1; }

static int l_now(lua_State *L) {
  lua_newtable(L);
  lua_pushinteger(L, g_hour); lua_setfield(L, -2, "hour");
  lua_pushinteger(L, g_min);  lua_setfield(L, -2, "min");
  lua_pushinteger(L, g_sec);  lua_setfield(L, -2, "sec");
  return 1;
}

static int l_clear(lua_State *L) {
  int r = (int)luaL_optinteger(L, 1, 0);
  int g = (int)luaL_optinteger(L, 2, 0);
  int b = (int)luaL_optinteger(L, 3, 0);
  for (int i = 0; i < W * H; i++) { fb[i*3] = r; fb[i*3+1] = g; fb[i*3+2] = b; }
  return 0;
}

static int l_pixel(lua_State *L) {
  put((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
      (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
      (int)luaL_checkinteger(L, 5));
  return 0;
}

static void hline(int x0, int x1, int y, int r, int g, int b) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) put(x, y, r, g, b);
}

static int l_rect(lua_State *L) {
  int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
  int r = (int)luaL_checkinteger(L, 5), g = (int)luaL_checkinteger(L, 6),
      b = (int)luaL_checkinteger(L, 7);
  int fill = lua_toboolean(L, 8);
  if (fill) { for (int j = 0; j < h; j++) hline(x, x + w - 1, y + j, r, g, b); }
  else {
    hline(x, x + w - 1, y, r, g, b);
    hline(x, x + w - 1, y + h - 1, r, g, b);
    for (int j = 0; j < h; j++) { put(x, y + j, r, g, b); put(x + w - 1, y + j, r, g, b); }
  }
  return 0;
}

static int l_line(lua_State *L) {
  int x0 = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2);
  int x1 = (int)luaL_checkinteger(L, 3), y1 = (int)luaL_checkinteger(L, 4);
  int r = (int)luaL_checkinteger(L, 5), g = (int)luaL_checkinteger(L, 6),
      b = (int)luaL_checkinteger(L, 7);
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    put(x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
  return 0;
}

static int l_circle(lua_State *L) {
  int cx = (int)luaL_checkinteger(L, 1), cy = (int)luaL_checkinteger(L, 2);
  int rad = (int)luaL_checkinteger(L, 3);
  int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
      b = (int)luaL_checkinteger(L, 6);
  int fill = lua_toboolean(L, 7);
  int x = rad, y = 0, d = 1 - rad;
  while (x >= y) {
    if (fill) {
      hline(cx - x, cx + x, cy + y, r, g, b); hline(cx - x, cx + x, cy - y, r, g, b);
      hline(cx - y, cx + y, cy + x, r, g, b); hline(cx - y, cx + y, cy - x, r, g, b);
    } else {
      put(cx+x,cy+y,r,g,b); put(cx+y,cy+x,r,g,b); put(cx-x,cy+y,r,g,b); put(cx-y,cy+x,r,g,b);
      put(cx+x,cy-y,r,g,b); put(cx+y,cy-x,r,g,b); put(cx-x,cy-y,r,g,b); put(cx-y,cy-x,r,g,b);
    }
    y++;
    if (d < 0) d += 2 * y + 1; else { x--; d += 2 * (y - x) + 1; }
  }
  return 0;
}

/* Picopixel, the same corrected font the firmware draws with. */
#include "font_picopixel.inc"

static int l_text(lua_State *L) {
  int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  const char *s = luaL_checkstring(L, 3);
  int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
      b = (int)luaL_checkinteger(L, 6);
  for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
    unsigned ch = *c;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 0x20 || ch > 0x7E) ch = ' ';
    const PicoGlyph *gl = &kPicoGlyphs[ch - 0x20];
    int bit = 0, bo = gl->off;
    for (int gy = 0; gy < gl->h; gy++)
      for (int gx = 0; gx < gl->w; gx++) {
        if (!(bit & 7)) bo++;
        int on = (kPicoBitmap[bo - 1] >> (7 - (bit & 7))) & 1;
        bit++;
        if (on) put(x + gx + gl->xo, y + gy + (PICO_YADV - 1) + gl->yo, r, g, b);
      }
    x += gl->adv;
  }
  return 0;
}

static int l_width(lua_State *L) {
  const char *s = luaL_checkstring(L, 1);
  int w = 0;
  for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
    unsigned ch = *c;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 0x20 || ch > 0x7E) ch = ' ';
    w += kPicoGlyphs[ch - 0x20].adv;
  }
  lua_pushinteger(L, w); return 1;
}

static const luaL_Reg px_lib[] = {
  {"size", l_size}, {"t", l_t}, {"now", l_now}, {"clear", l_clear},
  {"pixel", l_pixel}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle},
  {"text", l_text}, {"width", l_width}, {NULL, NULL}
};

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: luasim script.lua frames out.raw\n"); return 2; }
  const int frames = atoi(argv[2]);

  lua_State *L = luaL_newstate();
  if (!L) { fprintf(stderr, "no state\n"); return 1; }
  // The same sandbox surface the firmware opens: base, table, string, math.
  // No io, no os, no package - those are not even compiled in.
  luaL_requiref(L, LUA_GNAME,     luaopen_base,   1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME,luaopen_table,  1); lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME,luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME,luaopen_math,  1); lua_pop(L, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);
  luaL_newlib(L, px_lib); lua_setglobal(L, "px");

  if (luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "script error: %s\n", lua_tostring(L, -1)); return 1;
  }
  FILE *out = fopen(argv[3], "wb");
  if (!out) { perror("open"); return 1; }

  for (int f = 0; f < frames; f++) {
    g_phase = frames > 1 ? (double)f / (double)frames : 0.0;
    g_sec = (56 + f / 30) % 60;
    lua_getglobal(L, "draw");
    if (!lua_isfunction(L, -1)) { fprintf(stderr, "script defines no draw()\n"); return 1; }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
      fprintf(stderr, "frame %d: %s\n", f, lua_tostring(L, -1)); return 1;
    }
    fwrite(fb, 1, sizeof(fb), out);
  }
  fclose(out);
  fprintf(stderr, "luasim: %d frames, %dx%d\n", frames, W, H);
  lua_close(L);
  return 0;
}

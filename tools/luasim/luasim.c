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
static int    g_yday = 255;      /* 0-based day of year; 255 = 13 September */
static int    g_utc  = 2;        /* local time minus UTC, hours; CEST */
static int    g_year = 2026;     /* the year yday counts in */

/* put, blend, glow, fade, blur and px.mode: one code with the firmware */
#include "../../src/lua/px_raster.h"
static int g_add = 0;   /* px.mode: 1 while "add" */
_Static_assert(H <= 64, "pxr_circle_fill keeps a span for each of at most 64 rows");

static void put(int x, int y, int r, int g, int b) { pxr_put(fb, W, H, x, y, r, g, b, g_add); }

/* ---- px.* : the raster API the firmware will expose ---- */

static int l_size(lua_State *L)  { lua_pushinteger(L, W); lua_pushinteger(L, H); return 2; }
static int l_t(lua_State *L)     { lua_pushnumber(L, g_phase); return 1; }

static int l_now(lua_State *L) {
  lua_newtable(L);
  lua_pushinteger(L, g_hour); lua_setfield(L, -2, "hour");
  lua_pushinteger(L, g_min);  lua_setfield(L, -2, "min");
  lua_pushinteger(L, g_sec);  lua_setfield(L, -2, "sec");
  /* Anything that depends on where the sun is needs the date and UTC, not just
     the wall clock: the day/night line moves with both. */
  lua_pushinteger(L, g_yday); lua_setfield(L, -2, "yday");
  lua_pushinteger(L, g_utc);  lua_setfield(L, -2, "utc");
  /* A zone's summer time falls on weekdays, so a script that shows another
     zone's time needs the year as well. */
  lua_pushinteger(L, g_year); lua_setfield(L, -2, "year");
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
  /* every pixel once, so px.mode("add") lights it once; the same pixels in "set" */
  if (fill) { for (int j = 0; j < h; j++) hline(x, x + w - 1, y + j, r, g, b); }
  else {
    hline(x, x + w - 1, y, r, g, b);
    if (h != 1) hline(x, x + w - 1, y + h - 1, r, g, b);
    for (int j = 1; j <= h - 2; j++) { put(x, y + j, r, g, b); if (w != 1) put(x + w - 1, y + j, r, g, b); }
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
  if (fill) pxr_circle_fill(fb, W, H, cx, cy, rad, r, g, b, g_add);
  else      pxr_circle_ring(fb, W, H, cx, cy, rad, r, g, b, g_add);
  return 0;
}

/* Picopixel, the same corrected font the firmware draws with, Latin and
   Cyrillic, decoded by the firmware's own src/fonts/utf8_next.h. The routing
   below is src/fonts/pxfb_text.h's pxfbGlyph, written again in C; fx_parity
   holds the two to the same pixels. The classic 5x7 font is the firmware's
   own table, src/fonts/classic_font.h. */
#include "font_picopixel.inc"
#include "../../src/fonts/utf8_next.h"
#include "../../src/fonts/classic_font.h"

/* The optional font argument of px.text and px.width, as src/lua/lua_px.cpp:
   0 "small" (lowercase as capitals, the default), 1 "pico" (its own
   lowercase), 2 "5x7" (the classic font, both cases). */
static const char *const kFonts[] = {"small", "pico", "5x7", NULL};

static const unsigned char *classic_glyph(uint32_t cp) {   /* sys_text.h's sysClassicGlyph */
  if (cp >= 0x20 && cp < 0x7F) return kClassicAscii[cp - 0x20];
  if (cp >= 0xA0 && cp <= 0xFF) return kClassicLatin1[cp - 0xA0];
  if (cp >= kClassicCyrFirst && cp <= kClassicCyrLast) return kClassicCyr[cp - kClassicCyrFirst];
  return kClassicMissing;
}

static const PicoGlyph *pico_glyph_f(uint32_t cp, int fold, const unsigned char **bitmap) {
  if (fold && cp >= 0x0430 && cp <= 0x044F) cp -= 0x20;
  else if (fold && cp >= 0x0450 && cp <= 0x045F) cp -= 0x50;
  if (cp < 0x80) {
    if (fold && cp >= 'a' && cp <= 'z') cp -= 32;
    if (cp < 0x20 || cp > 0x7E) cp = ' ';
    *bitmap = kPicoBitmap;
    return &kPicoGlyphs[cp - 0x20];
  }
  *bitmap = kPicoCyrBitmap;
  if (cp >= PICO_CYR_FIRST && cp <= PICO_CYR_LAST) return &kPicoCyrGlyphs[cp - PICO_CYR_FIRST];
  return &kPicoMissing;
}

static int l_text(lua_State *L) {
  int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  const char *s = luaL_checkstring(L, 3);
  int r = (int)luaL_checkinteger(L, 4), g = (int)luaL_checkinteger(L, 5),
      b = (int)luaL_checkinteger(L, 6);
  const int font = luaL_checkoption(L, 7, "small", kFonts);
  if (font == 2) {
    for (const unsigned char *c = (const unsigned char *)s; *c;) {
      if (x >= W) break;
      const unsigned char *cols = classic_glyph(utf8Next(&c));
      for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 8; gy++)
          if ((cols[gx] >> gy) & 1) put(x + gx, y + gy, r, g, b);
      x += 6;
    }
    return 0;
  }
  for (const unsigned char *c = (const unsigned char *)s; *c;) {
    if (x >= W) break;
    const unsigned char *bm;
    const PicoGlyph *gl = pico_glyph_f(utf8Next(&c), font == 0, &bm);
    int bit = 0, bo = gl->off;
    for (int gy = 0; gy < gl->h; gy++)
      for (int gx = 0; gx < gl->w; gx++) {
        if (!(bit & 7)) bo++;
        int on = (bm[bo - 1] >> (7 - (bit & 7))) & 1;
        bit++;
        if (on) put(x + gx + gl->xo, y + gy + (PICO_YADV - 1) + gl->yo, r, g, b);
      }
    x += gl->adv;
  }
  return 0;
}

static int l_width(lua_State *L) {
  const char *s = luaL_checkstring(L, 1);
  const int font = luaL_checkoption(L, 2, "small", kFonts);
  int w = 0;
  for (const unsigned char *c = (const unsigned char *)s; *c;) {
    const unsigned char *bm;
    const uint32_t cp = utf8Next(&c);
    w += font == 2 ? 6 : pico_glyph_f(cp, font == 0, &bm)->adv;
  }
  lua_pushinteger(L, w); return 1;
}

/* Blending. The raster API overwrites by default, which makes a dim colour a
   dark blot rather than a faint light - so glows, shadows and anti-aliasing all
   need read-modify-write. Doing it per pixel in Lua would burn the instruction
   budget, so it lives here: the same reasoning that put beam.kit in C. */
static int l_get(lua_State *L) {
  int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  if (x < 0 || x >= W || y < 0 || y >= H) { lua_pushinteger(L,0); lua_pushinteger(L,0); lua_pushinteger(L,0); return 3; }
  unsigned char *p = &fb[(y * W + x) * 3];
  lua_pushinteger(L, p[0]); lua_pushinteger(L, p[1]); lua_pushinteger(L, p[2]);
  return 3;
}

static int l_blend(lua_State *L) {
  pxr_blend(fb, W, H, luaL_checkinteger(L,1), luaL_checkinteger(L,2),
            luaL_checknumber(L,3), luaL_checknumber(L,4), luaL_checknumber(L,5),
            luaL_checknumber(L,6));
  return 0;
}

/* A radial light: one call instead of a Lua loop over a few hundred pixels.
   Falls off as (1 - d/rad)^2, which reads as a lamp rather than a disc. */
static int l_glow(lua_State *L) {
  (void)pxr_glow(fb, W, H, luaL_checknumber(L,1), luaL_checknumber(L,2), luaL_checknumber(L,3),
           luaL_checknumber(L,4), luaL_checknumber(L,5), luaL_checknumber(L,6),
           luaL_optnumber(L,7, 1));
  return 0;
}

static int l_fade(lua_State *L) { unsigned long work; return px_fade_lua(L, fb, W, H, &work); }
static int l_blur(lua_State *L) { unsigned long work; return px_blur_lua(L, fb, W, H, &work); }
static int l_mode(lua_State *L) { return px_mode_lua(L, &g_add); }

/* palette, pal, layer, capture, show, scroll, mirror: one code with the firmware */
#include "../../src/lua/px_layer.h"
static int l_palette(lua_State *L) { return px_palette_lua(L); }
static int l_pal(lua_State *L)     { return px_pal_lua(L); }
static int l_layer(lua_State *L)   { return px_layer_lua(L, W, H); }
static int l_capture(lua_State *L) { unsigned long work; return px_capture_lua(L, fb, W, H, &work); }
static int l_show(lua_State *L)    { unsigned long work; return px_show_lua(L, fb, W, H, &work); }
static int l_scroll(lua_State *L)  { unsigned long work; return px_scroll_lua(L, fb, W, H, &work); }
static int l_mirror(lua_State *L)  { unsigned long work; return px_mirror_lua(L, fb, W, H, &work); }

#include "../../src/lua/px_terrain.h"   /* px.terrain, shared with the firmware */
#include "../../src/lua/px_snapshot.h"  /* px.save, px.restore, shared with the firmware */
static int l_save(lua_State *L)    { return px_save_lua(L, fb, sizeof(fb)); }
static int l_restore(lua_State *L) { return px_restore_lua(L, fb, sizeof(fb)); }
static int l_terrain(lua_State *L) { unsigned long work; return px_terrain_lua(L, fb, W, H, &work); }
#include "../../src/lua/px_sprite.h"    /* px.grab, px.blit, shared with the firmware */
static int l_grab(lua_State *L)    { unsigned long work; return px_grab_lua(L, fb, W, H, &work); }
static int l_blit(lua_State *L)    { unsigned long work; return px_blit_lua(L, fb, W, H, &work); }
/* px.weather and px.city: nil unless --weather T / --city NAME give the same
   sample fxhost gives (T, T-4 .. T+3, 60 %, 10 km/h, code 1) */
static int g_haveWeather = 0; static float g_weatherT = 0; static char g_city[33] = "";
static int l_weather(lua_State *L) {
  if (!g_haveWeather) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 7);
  lua_pushnumber(L, g_weatherT);      lua_setfield(L, -2, "temp");
  lua_pushnumber(L, g_weatherT - 4);  lua_setfield(L, -2, "min");
  lua_pushnumber(L, g_weatherT + 3);  lua_setfield(L, -2, "max");
  lua_pushinteger(L, 60);             lua_setfield(L, -2, "humidity");
  lua_pushnumber(L, 10);              lua_setfield(L, -2, "wind");
  lua_pushinteger(L, 1);              lua_setfield(L, -2, "code");
  lua_pushboolean(L, 0);              lua_setfield(L, -2, "fahrenheit");
  return 1;
}
static int l_city(lua_State *L) { if (!g_city[0]) lua_pushnil(L); else lua_pushstring(L, g_city); return 1; }
/* px.button: --clicks f1,f2,... clicks the effect's button at those frames */
static int g_clicks = 0;
static int l_button(lua_State *L)  { lua_pushinteger(L, g_clicks); return 1; }

static const luaL_Reg px_lib[] = {
  {"get", l_get}, {"blend", l_blend}, {"glow", l_glow},
  {"size", l_size}, {"t", l_t}, {"now", l_now}, {"clear", l_clear},
  {"pixel", l_pixel}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle},
  {"text", l_text}, {"width", l_width}, {"terrain", l_terrain},
  {"save", l_save}, {"restore", l_restore}, {"grab", l_grab}, {"blit", l_blit},
  {"button", l_button}, {"fade", l_fade}, {"blur", l_blur}, {"mode", l_mode},
  {"palette", l_palette}, {"pal", l_pal}, {"layer", l_layer}, {"capture", l_capture},
  {"show", l_show}, {"scroll", l_scroll}, {"mirror", l_mirror},
  {"weather", l_weather}, {"city", l_city},
  {NULL, NULL}
};

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: luasim script.lua frames out.raw "
                    "[--start HH:MM] [--yday N] [--utc H] [--year Y] [--sweep]\n");
    return 2;
  }
  const int frames = atoi(argv[2]);
  int start_min = g_hour * 60 + g_min, sweep = 0;
  const char *clicks = NULL;
  for (int a = 4; a < argc; a++) {
    if (!strcmp(argv[a], "--start") && a + 1 < argc) {
      int hh = 0, mm = 0; sscanf(argv[++a], "%d:%d", &hh, &mm); start_min = hh * 60 + mm;
    } else if (!strcmp(argv[a], "--yday") && a + 1 < argc) g_yday = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--utc") && a + 1 < argc)   g_utc  = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--year") && a + 1 < argc)  g_year = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--sweep"))                 sweep  = 1;   /* a whole day over the frames */
    else if (!strcmp(argv[a], "--clicks") && a + 1 < argc) clicks = argv[++a];
    else if (!strcmp(argv[a], "--weather") && a + 1 < argc) { g_haveWeather = 1; g_weatherT = (float)atof(argv[++a]); }
    else if (!strcmp(argv[a], "--city") && a + 1 < argc) { snprintf(g_city, sizeof(g_city), "%s", argv[++a]); }
  }
  g_hour = start_min / 60 % 24; g_min = start_min % 60;

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
    if (clicks) {                  /* how many of the listed frames have come */
      int n = 0; const char *p = clicks;
      while (*p) { if (atoi(p) <= f) n++; p = strchr(p, ','); if (!p) break; p++; }
      g_clicks = n;
    }
    if (sweep) {
      const int m = (start_min + f * 1440 / (frames > 0 ? frames : 1)) % 1440;
      g_hour = m / 60; g_min = m % 60; g_sec = 0;
    } else {
      g_sec = (56 + f / 30) % 60;
    }
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

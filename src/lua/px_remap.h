// ============================================================
// px_remap.h - the screen redrawn through a map: tunnels, globes, floors
// ============================================================
// A map says, for every pixel of the screen, where in a texture its colour
// comes from, and how bright it is. Built once, it turns a flat picture into
// a tunnel flown through, a planet turning, a floor rushing below - by
// sliding the texture under it, a frame at a time. The demoscene's oldest
// trick, and one call a frame here:
//
//   M = px.uvmap(kind [, params])       a map, computed in C:
//     "tunnel" {cx, cy, depth = 24}     u the angle round (cx, cy), v depth/distance,
//                                       darker toward the far end
//     "polar"  {cx, cy, scale = 2}      u the angle, v the distance * scale
//     "sphere" {cx, cy, r = 28}         a globe: longitude and latitude, lit from the front
//     "plane"  {horizon = 20, height = 16, fov = 1}
//                                       a floor below the horizon (mode 7), darker far off
//     "swirl"  {cx, cy, turn = 3}       the screen itself, twisted round (cx, cy);
//                                       turn in radians at the centre, less
//                                       farther out; corners turned off it
//                                       reflected back in
//   M:set(x, y, u, v [, shade])         any map, a pixel at a time; u, v 0..255
//                                       wrap round the texture, shade 0..255
//   px.remap(M, src, du, dv [, pal])    the canvas redrawn: each pixel from the
//                                       texture at (u + du, v + dv), times its shade.
//                                       src is a snapshot slot (1..4, px.save)
//                                       or a layer (then pal is its palette)
//
// u runs round the tunnel and the polar map, and v on into the distance, so
// those and the floor want a texture that tiles (sines a whole number of
// times across the texture do); the swirl stays inside the screen.
// A map is 3 bytes a pixel (u, v, shade: 24 KB of the effect's heap).
// Integers only, like the rest: px_raster.h's square root, px_field.h's
// atan2, px_layer.h's cosine table. px_raster.h, px_layer.h, px_snapshot.h
// and px_field.h must be included first.
// ============================================================
#ifndef PX_REMAP_H
#define PX_REMAP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PXU_META "px.uvmap"

typedef void (*PxuCharge)(lua_State *L, unsigned long instructions);

typedef struct {
  int w, h;
  unsigned char m[1];         // u, v, shade a pixel
} PxUV;

static PxUV *pxu_check(lua_State *L, int i) { return (PxUV *)luaL_checkudata(L, i, PXU_META); }

static lua_Number pxu_opt(lua_State *L, int t, const char *k, lua_Number def, lua_Number lo, lua_Number hi) {
  if (!t) return def;
  lua_pushstring(L, k);
  lua_rawget(L, t);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) { v = lua_tonumber(L, -1); if (!(v == v)) v = def; }
  else if (!lua_isnil(L, -1)) { lua_pop(L, 1); return luaL_error(L, "px.uvmap: %s wants a number", k); }
  lua_pop(L, 1);
  return v < lo ? lo : v > hi ? hi : v;
}

static int pxu_set(lua_State *L) {
  PxUV *u = pxu_check(L, 1);
  const lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
  const lua_Integer uu = luaL_checkinteger(L, 4), vv = luaL_checkinteger(L, 5);
  const lua_Integer sh = luaL_optinteger(L, 6, 255);
  if (x < 0 || x >= u->w || y < 0 || y >= u->h) return 0;
  unsigned char *p = &u->m[(y * u->w + x) * 3];
  p[0] = (unsigned char)(uu & 255); p[1] = (unsigned char)(vv & 255);
  p[2] = (unsigned char)(sh < 0 ? 0 : sh > 255 ? 255 : sh);
  return 0;
}

// a coordinate reflected back into 0..n-1, as a mirror-repeated texture
static int32_t pxu_fold(int32_t a, int32_t n) {
  int32_t m = a % (2 * n);
  if (m < 0) m += 2 * n;
  return m < n ? m : 2 * n - 1 - m;
}

static const char *const pxu_kinds[] = {"tunnel", "polar", "sphere", "plane", "swirl", "blank", NULL};

// px.uvmap(kind [, params]) -> M
static int px_uvmap_lua(lua_State *L, int w, int h, PxuCharge charge) {
  const int kind = luaL_checkoption(L, 1, "blank", pxu_kinds);
  const int t = lua_istable(L, 2) ? 2 : 0;
  if (!t && !lua_isnoneornil(L, 2)) return luaL_error(L, "px.uvmap: the parameters are a table");
  const int32_t cx = (int32_t)(pxu_opt(L, t, "cx", (lua_Number)(w - 1) / 2, -1024, 1024) * 16);   // 1/16 px
  const int32_t cy = (int32_t)(pxu_opt(L, t, "cy", (lua_Number)(h - 1) / 2, -1024, 1024) * 16);
  const int32_t depth = (int32_t)(pxu_opt(L, t, "depth", 24, 1, 1000) * 16);
  const int32_t scale = (int32_t)(pxu_opt(L, t, "scale", 2, 0, 64) * 256);                     // Q8
  const int32_t rad = (int32_t)(pxu_opt(L, t, "r", 28, 1, 512) * 16);
  const int32_t horizon = (int32_t)pxu_opt(L, t, "horizon", 20, -64, 128);
  const int32_t height = (int32_t)(pxu_opt(L, t, "height", 16, 1, 1000) * 256);              // Q8
  const int32_t fov = (int32_t)(pxu_opt(L, t, "fov", 1, 0.05, 20) * 256);                     // Q8
  const int32_t turn = pxn_turns(pxu_opt(L, t, "turn", 3, -100, 100));                          // Q16 turns
  // measured on the panel: 12-21 ms a map by kind (sphere 17, swirl 18-21),
  // 29 ms at worst with the allocation; 8 a pixel is ~27 ms at 410 ns an instruction
  if (charge) charge(L, (unsigned long)w * h * 8);
  PxUV *u = (PxUV *)lua_newuserdatauv(L, offsetof(PxUV, m) + (size_t)w * h * 3, 0);
  u->w = w; u->h = h;
  memset(u->m, 0, (size_t)w * h * 3);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      unsigned char *p = &u->m[(y * w + x) * 3];
      const int32_t dx = x * 16 - cx, dy = y * 16 - cy;                   // 1/16 px, within +-(1024+128)*16
      const int32_t d = (int32_t)pxr_isqrt((unsigned)(dx * dx) + (unsigned)(dy * dy));   // 1/16 px
      int uu = 0, vv = 0, sh = 255;
      if (kind == 0) {                               // tunnel
        uu = (pxn_atan2(dy, dx) >> 8) & 255;
        // depth and d both in 1/16 px: v = 256 * depth / distance, round and round
        vv = d > 0 ? (int)(((int64_t)depth * 256 / (d > 16 ? d : 16)) & 255) : 0;
        const int s = d / 16 * 12;                   // dark at the centre, the far end
        sh = s > 255 ? 255 : s;
      } else if (kind == 1) {                        // polar
        uu = (pxn_atan2(dy, dx) >> 8) & 255;
        vv = (int)(((int64_t)d * scale >> 12) & 255);
      } else if (kind == 2) {                        // sphere
        if (d >= rad) { sh = 0; }
        else {
          // on the globe: z toward the viewer, longitude from x against z, latitude from y
          const int32_t zz = (int32_t)pxr_isqrt((unsigned)((int64_t)rad * rad - (int64_t)dx * dx - (int64_t)dy * dy));
          uu = (pxn_atan2(zz, dx) >> 8) & 255;                         // a half turn across the face
          const int32_t lat = pxn_atan2(dy, (int32_t)pxr_isqrt((unsigned)((int64_t)dx * dx + (int64_t)zz * zz)));
          vv = ((lat + 16384) >> 7) & 255;                             // -1/4..1/4 turn -> 0..255
          sh = 40 + (int)((int64_t)zz * 215 / rad);                    // lit from the front
        }
      } else if (kind == 3) {                        // plane
        const int32_t dyh = y - horizon;
        if (dyh <= 0) { sh = 0; }
        else {
          const int32_t dist = height / dyh;                           // Q8 units ahead
          vv = (int)((dist >> 2) & 255);                               // 64 texture steps a unit
          // across: the pixel's offset from the middle, 32 px a unit at depth 1
          // (fov 1), grown with the depth; 64 texture steps a unit again
          const int64_t dxq = (int64_t)x * 256 - (int64_t)(w - 1) * 128;   // Q8 px
          uu = (int)((dxq * dist * 64 / ((int64_t)fov * 32 * 256)) & 255);
          const int s = 255 - (int)(dist >> 6);
          sh = s < 30 ? 30 : s;
        }
      } else if (kind == 4) {                        // swirl: the screen, turned more near the centre
        const int32_t k = d > 0 ? (int32_t)((int64_t)turn * 16 * 16 / (d + 64)) : turn;   // turns, Q16
        const int32_t c = pxl_cosq((uint32_t)k & 0xFFFF), sn = pxl_cosq(((uint32_t)k - 16384u) & 0xFFFF);
        const int32_t rx = pxu_fold((int32_t)(((int64_t)dx * c - (int64_t)dy * sn) >> 15) + cx, w * 16);   // 1/16 px
        const int32_t ry = pxu_fold((int32_t)(((int64_t)dx * sn + (int64_t)dy * c) >> 15) + cy, h * 16);
        // u, v: the screen as the texture, 256 steps for its width and height;
        // a corner turned off the screen is reflected back in, so any
        // picture (a snapshot of the screen itself) twists without a seam
        uu = (int)((int64_t)rx * 256 / (w * 16));
        vv = (int)((int64_t)ry * 256 / (h * 16));
      }
      p[0] = (unsigned char)uu; p[1] = (unsigned char)vv; p[2] = (unsigned char)sh;
    }
  if (luaL_newmetatable(L, PXU_META)) {
    static const luaL_Reg mt[] = {{"set", pxu_set}, {NULL, NULL}};
    luaL_newlib(L, mt);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "px.uvmap");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
  return 1;
}

// px.remap(M, src, du, dv [, pal])
static int px_remap_lua(lua_State *L, unsigned char *fb, int w, int h, PxuCharge charge) {
  PxUV *u = pxu_check(L, 1);
  if (u->w != w || u->h != h) return luaL_error(L, "px.remap: the map is not the canvas's size");
  const int du = (int)(luaL_checkinteger(L, 3) & 255), dv = (int)(luaL_checkinteger(L, 4) & 255);
  const unsigned char *rgb = NULL, *lay = NULL, *pal = NULL;
  int tw = w, th = h;
  if (lua_type(L, 2) == LUA_TNUMBER) {
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_argcheck(L, n >= 1 && n <= PXS_SLOTS, 2, "a snapshot slot is 1 to 4");
    rgb = pxs_buffer(L, (int)n - 1, (size_t)w * h * 3, 0);
    if (!rgb) { lua_pushboolean(L, 0); return 1; }
  } else {
    PxLayer *l = pxl_check(L, 2);
    lay = l->v; tw = l->w; th = l->h;
    pal = pxl_checkpal(L, 5);
  }
  // measured on the panel: 3.2 ms the canvas from a layer, 2.2 ms from a
  // snapshot; one a pixel is ~3.4 ms at 410 ns an instruction
  if (charge) charge(L, (unsigned long)w * h);
  for (int i = 0; i < w * h; i++) {
    const unsigned char *m = &u->m[i * 3];
    unsigned char *p = &fb[i * 3];
    const int sh = m[2];
    if (sh == 0) { p[0] = p[1] = p[2] = 0; continue; }
    const int tx = (((m[0] + du) & 255) * tw) >> 8, ty = (((m[1] + dv) & 255) * th) >> 8;
    const unsigned char *c = rgb ? &rgb[(ty * tw + tx) * 3] : &pal[lay[ty * tw + tx] * 3];
    p[0] = (unsigned char)((c[0] * (sh + 1)) >> 8);
    p[1] = (unsigned char)((c[1] * (sh + 1)) >> 8);
    p[2] = (unsigned char)((c[2] * (sh + 1)) >> 8);
  }
  lua_pushboolean(L, 1);
  return 1;
}

#endif

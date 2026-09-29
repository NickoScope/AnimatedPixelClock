// ============================================================
// px_particles.h - a particle system in C: fireworks, snow, sparks, flow
// ============================================================
// Hundreds of particles moved and drawn in a call each, where a Lua loop over
// them costs a frame. One system holds up to 4096; each has a position, a
// velocity, a colour and a life, in fixed point.
//
//   P = px.particles(max [, seed])      -- a system; seed for its own random numbers
//   P:emit{ x, y, n = 1, w = 0, h = 0,  -- n particles from (x, y), or from a
//           speed = 20, speedj = 0,     --   w x h box at x, y; speed px/s and
//           angle = 0, spread = 2pi,    --   its jitter 0..1; direction (radians,
//           life = 2, lifej = 0,        --   0 = right, clockwise on the panel)
//           r, g, b | pal, idx, idxj }  --   and spread; life s and its jitter;
//                                       --   a colour, or palette + index + jitter
//   P:step{ dt = 1/15, gx = 0, gy = 0,  -- gravity px/s^2, drag a second,
//           drag = 0, flow = 0,         --   a flow field (curl of Perlin noise,
//           flowscale = 0.03, flowz = 0,--   strength px/s^2), and what the edge
//           edge = "kill" }             --   does: "kill", "wrap", "bounce"
//   P:draw{ mode = "add", bri = 1,      -- each particle a pixel (size 2: 2x2),
//           fade = true, size = 1 }     --   dimmed toward the end of its life
//   P:count() -> alive      P:clear()
//
// Integers only, like the other shared headers (the random numbers are the
// system's own xorshift, the directions px_layer.h's cosine table, the flow
// px_field.h's noise), so the panel and luasim move the same particles.
// px_raster.h, px_layer.h and px_field.h must be included first.
// ============================================================
#ifndef PX_PARTICLES_H
#define PX_PARTICLES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PXP_META "px.particles"
#define PXP_MAX 4096

typedef struct {
  int32_t x, y, vx, vy;       // Q16 px, Q16 px/s
  int32_t life, life0;        // Q16 s
  unsigned char r, g, b, pad;
} PxPart;

typedef void (*PxpCharge)(lua_State *L, unsigned long instructions);

typedef struct {
  int max, n;
  uint32_t seed;
  unsigned char *fb;          // the canvas it draws on, and its size
  int w, h;
  PxpCharge charge;           // NULL on the host
  PxPart p[1];
} PxPS;

static PxPS *pxp_check(lua_State *L) { return (PxPS *)luaL_checkudata(L, 1, PXP_META); }

static uint32_t pxp_rnd(PxPS *s) {            // xorshift32
  uint32_t x = s->seed;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return s->seed = x;
}
static int32_t pxp_u16(PxPS *s) { return (int32_t)(pxp_rnd(s) >> 16); }   // 0..65535

// A field of the table at 2, raw, clamped; NaN or missing -> def.
static lua_Number pxp_num(lua_State *L, const char *k, lua_Number def, lua_Number lo, lua_Number hi) {
  lua_pushstring(L, k);
  lua_rawget(L, 2);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) { v = lua_tonumber(L, -1); if (!(v == v)) v = def; }
  else if (!lua_isnil(L, -1)) { lua_pop(L, 1); return luaL_error(L, "particles: %s wants a number", k); }
  lua_pop(L, 1);
  return v < lo ? lo : v > hi ? hi : v;
}
static int32_t pxp_q16(lua_Number v) { return (int32_t)(v * 65536); }   // callers clamp first

static void pxp_charge(lua_State *L, PxPS *s, unsigned long n) { if (s->charge) s->charge(L, n); }

// P:emit{...}
static int pxp_emit(lua_State *L) {
  PxPS *s = pxp_check(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  const int n = (int)pxp_num(L, "n", 1, 0, PXP_MAX);
  const int32_t x = pxp_q16(pxp_num(L, "x", s->w / 2, -2048, 2048)), y = pxp_q16(pxp_num(L, "y", s->h / 2, -2048, 2048));
  const int32_t bw = pxp_q16(pxp_num(L, "w", 0, 0, 2048)), bh = pxp_q16(pxp_num(L, "h", 0, 0, 2048));
  const int32_t speed = pxp_q16(pxp_num(L, "speed", 20, 0, 1000));
  const int32_t sj = (int32_t)(pxp_num(L, "speedj", 0, 0, 1) * 65536);
  const int32_t ang = pxn_turns(pxp_num(L, "angle", 0, -1000, 1000));
  const int32_t spread = pxn_turns(pxp_num(L, "spread", 6.2831853, 0, 6.2831853));
  const int32_t life = pxp_q16(pxp_num(L, "life", 2, 0, 600));
  const int32_t lj = (int32_t)(pxp_num(L, "lifej", 0, 0, 1) * 65536);
  // the colour: a palette and an index, or r, g, b
  lua_pushliteral(L, "pal");
  lua_rawget(L, 2);
  const unsigned char *pal = NULL;
  if (!lua_isnil(L, -1)) {
    size_t len = 0;
    pal = (const unsigned char *)lua_tolstring(L, -1, &len);
    if (!pal || len != 768) return luaL_error(L, "particles: pal is a palette from px.palette");
  }
  const int idx = (int)pxp_num(L, "idx", 0, -100000, 100000), idxj = (int)pxp_num(L, "idxj", 0, 0, 255);
  const int cr = pxl_num255(pxp_num(L, "r", 255, 0, 255)), cg = pxl_num255(pxp_num(L, "g", 255, 0, 255)),
            cb = pxl_num255(pxp_num(L, "b", 255, 0, 255));
  pxp_charge(L, s, (unsigned long)n * 3);
  for (int k = 0; k < n && s->n < s->max; k++) {
    PxPart *p = &s->p[s->n++];
    p->x = x + (int32_t)(((int64_t)bw * pxp_u16(s)) >> 16);
    p->y = y + (int32_t)(((int64_t)bh * pxp_u16(s)) >> 16);
    const int32_t a = (int32_t)((uint32_t)ang + (uint32_t)((((int64_t)spread * pxp_u16(s)) >> 16) - spread / 2));
    const int32_t sp = speed - (int32_t)(((int64_t)speed * (((int64_t)sj * pxp_u16(s)) >> 16)) >> 16);
    p->vx = (int32_t)(((int64_t)sp * pxl_cosq((uint32_t)a & 0xFFFF)) >> 15);
    p->vy = (int32_t)(((int64_t)sp * pxl_cosq(((uint32_t)a - 16384u) & 0xFFFF)) >> 15);   // sin
    p->life0 = p->life = life - (int32_t)(((int64_t)life * (((int64_t)lj * pxp_u16(s)) >> 16)) >> 16);
    if (pal) {
      const int i = (idx + (idxj ? (int)(pxp_rnd(s) % (uint32_t)(idxj + 1)) : 0)) & 255;
      p->r = pal[i * 3]; p->g = pal[i * 3 + 1]; p->b = pal[i * 3 + 2];
    } else { p->r = (unsigned char)cr; p->g = (unsigned char)cg; p->b = (unsigned char)cb; }
  }
  lua_pop(L, 1);             // pal
  return 0;
}

// P:step{...}
static const char *const pxp_edges[] = {"kill", "wrap", "bounce", NULL};
static int pxp_step(lua_State *L) {
  PxPS *s = pxp_check(L);
  if (lua_isnoneornil(L, 2)) { lua_settop(L, 1); lua_newtable(L); }
  luaL_checktype(L, 2, LUA_TTABLE);
  const int32_t dt = pxp_q16(pxp_num(L, "dt", 1.0 / 15, 0, 0.5));
  const int32_t gx = pxp_q16(pxp_num(L, "gx", 0, -2000, 2000)), gy = pxp_q16(pxp_num(L, "gy", 0, -2000, 2000));
  const int32_t drag = (int32_t)(((int64_t)pxp_q16(pxp_num(L, "drag", 0, 0, 30)) * dt) >> 16);   // Q16 a step
  const int32_t flow = pxp_q16(pxp_num(L, "flow", 0, -2000, 2000));
  const int32_t fsc = pxp_q16(pxp_num(L, "flowscale", 0.03, 0, 4));
  const int32_t fz = pxn_q16(pxp_num(L, "flowz", 0, -32767, 32767));
  lua_pushliteral(L, "edge");
  lua_rawget(L, 2);
  int edge = 0;
  if (!lua_isnil(L, -1)) {
    const char *e = lua_tostring(L, -1);
    for (edge = 0; e && pxp_edges[edge] && strcmp(pxp_edges[edge], e) != 0; edge++) {}
    if (!e || !pxp_edges[edge]) return luaL_error(L, "particles: edge is \"kill\", \"wrap\" or \"bounce\"");
  }
  lua_pop(L, 1);
  pxp_charge(L, s, (unsigned long)s->n * (flow ? 22 : 3));
  const int32_t W16 = s->w << 16, H16 = s->h << 16;
  for (int i = 0; i < s->n;) {
    PxPart *p = &s->p[i];
    p->life -= dt;
    if (p->life <= 0) { *p = s->p[--s->n]; continue; }
    int64_t ax = gx, ay = gy;
    if (flow) {
      // the curl of the noise: along its contour lines, so the flow never sinks
      const int32_t nx = (int32_t)(((int64_t)p->x * fsc >> 16) & 0x7FFFFFFF);
      const int32_t ny = (int32_t)(((int64_t)p->y * fsc >> 16) & 0x7FFFFFFF);
      const int32_t e = 4096;                                          // 1/16 of a noise unit
      const int32_t dy = pxn_noise3(nx, ny + e, fz) - pxn_noise3(nx, ny - e, fz);
      const int32_t dx = pxn_noise3(nx + e, ny, fz) - pxn_noise3(nx - e, ny, fz);
      // derivative per noise unit = d * 65536 / (2e) = d * 8, then times strength
      ax += ((int64_t)flow * dy * 8) >> 16;
      ay -= ((int64_t)flow * dx * 8) >> 16;
    }
    // velocities held to +-16384 px/s (Q16 within 31 bits), however long gravity pulls
    const int64_t VMAX = (int64_t)16384 << 16;
    int64_t vx = p->vx + ((ax * dt) >> 16), vy = p->vy + ((ay * dt) >> 16);
    vx = vx > VMAX ? VMAX : vx < -VMAX ? -VMAX : vx;
    vy = vy > VMAX ? VMAX : vy < -VMAX ? -VMAX : vy;
    p->vx = (int32_t)vx; p->vy = (int32_t)vy;
    if (drag) { p->vx -= (int32_t)(((int64_t)p->vx * drag) >> 16); p->vy -= (int32_t)(((int64_t)p->vy * drag) >> 16); }
    p->x += (int32_t)(((int64_t)p->vx * dt) >> 16);
    p->y += (int32_t)(((int64_t)p->vy * dt) >> 16);
    if (edge == 0) {
      if (p->x < -(2 << 16) || p->x >= W16 + (2 << 16) || p->y < -(2 << 16) || p->y >= H16 + (2 << 16)) { *p = s->p[--s->n]; continue; }
    } else if (edge == 1) {
      p->x %= W16; if (p->x < 0) p->x += W16;
      p->y %= H16; if (p->y < 0) p->y += H16;
    } else {
      if (p->x < 0) { p->x = -p->x; p->vx = -p->vx; }
      if (p->x >= W16) { p->x = 2 * (W16 - 1) - p->x; p->vx = -p->vx; }
      if (p->y < 0) { p->y = -p->y; p->vy = -p->vy; }
      if (p->y >= H16) { p->y = 2 * (H16 - 1) - p->y; p->vy = -p->vy; }
      if (p->x < 0 || p->x >= W16 || p->y < 0 || p->y >= H16) { *p = s->p[--s->n]; continue; }
    }
    i++;
  }
  return 0;
}

// P:draw{...}
static int pxp_draw(lua_State *L) {
  PxPS *s = pxp_check(L);
  if (lua_isnoneornil(L, 2)) { lua_settop(L, 1); lua_newtable(L); }
  luaL_checktype(L, 2, LUA_TTABLE);
  const int bri = pxr_q8(pxp_num(L, "bri", 1, 0, 1));
  const int size = (int)pxp_num(L, "size", 1, 1, 2);
  lua_pushliteral(L, "fade");
  lua_rawget(L, 2);
  const int fade = lua_isnil(L, -1) ? 1 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_pushliteral(L, "mode");
  lua_rawget(L, 2);
  const char *m = lua_tostring(L, -1);
  const int add = !m || strcmp(m, "set") != 0;
  if (m && strcmp(m, "set") != 0 && strcmp(m, "add") != 0) return luaL_error(L, "particles: mode is \"add\" or \"set\"");
  lua_pop(L, 1);
  pxp_charge(L, s, (unsigned long)s->n * (unsigned long)(size * size));
  for (int i = 0; i < s->n; i++) {
    const PxPart *p = &s->p[i];
    int k = bri;
    if (fade && p->life0 > 0) k = (int)(((int64_t)k * p->life) / p->life0);
    const int r = (p->r * k) >> 8, g = (p->g * k) >> 8, b = (p->b * k) >> 8;
    const int x0 = p->x >> 16, y0 = p->y >> 16;
    for (int dy = 0; dy < size; dy++)
      for (int dx = 0; dx < size; dx++) pxr_put(s->fb, s->w, s->h, x0 + dx, y0 + dy, r, g, b, add);
  }
  return 0;
}

static int pxp_count(lua_State *L) { lua_pushinteger(L, pxp_check(L)->n); return 1; }
static int pxp_clear(lua_State *L) { pxp_check(L)->n = 0; return 0; }

// px.particles(max [, seed]) -> P
static int px_particles_lua(lua_State *L, unsigned char *fb, int w, int h, PxpCharge charge) {
  const lua_Integer max = luaL_checkinteger(L, 1);
  luaL_argcheck(L, max >= 1 && max <= PXP_MAX, 1, "a system holds 1 to 4096 particles");
  const lua_Integer seed = luaL_optinteger(L, 2, 1);
  PxPS *s = (PxPS *)lua_newuserdatauv(L, offsetof(PxPS, p) + (size_t)max * sizeof(PxPart), 0);
  s->max = (int)max; s->n = 0; s->seed = (uint32_t)seed ? (uint32_t)seed : 1u;
  s->fb = fb; s->w = w; s->h = h; s->charge = charge;
  if (charge) charge(L, (unsigned long)max / 8);
  if (luaL_newmetatable(L, PXP_META)) {
    static const luaL_Reg m[] = {{"emit", pxp_emit}, {"step", pxp_step}, {"draw", pxp_draw},
                                 {"count", pxp_count}, {"clear", pxp_clear}, {NULL, NULL}};
    luaL_newlib(L, m);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "px.particles");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
  return 1;
}

#endif

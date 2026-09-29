// ============================================================
// px_sim.h - a step of a simulation on a layer: fire, life, waves, and
// reaction-diffusion
// ============================================================
// Each is a whole-screen update a frame, which in Lua would cost the frame:
//
//   px.step(L, "fire", {cool = 0.3, heat = 1, seed = n})
//       heat rises from the bottom row (seeded at random, heat 0..1 of it
//       hot), each cell the average of the three below and the one under
//       those, less the cooling (0..1: up to 16 levels a row; lodev's fire)
//   px.step(L, "life", {decay = 0.1, born = "3", survive = "23"})
//       Conway's Life on the layer: a cell is alive above 127; the living are
//       set to 255, the dead fade by decay each step so they leave trails
//   px.step(L, "wave", {prev = L2, damp = 0.02})
//       water: the next height is half the four neighbours less the one
//       before, damped (Hugo Elias's ripples); heights are v - 128. It writes
//       the next into prev: swap the two layers after each step
//   R = px.reaction{f = 0.055, k = 0.062, da = 1, db = 0.5}
//       Gray-Scott reaction-diffusion over the canvas (Karl Sims's tutorial
//       parameters), two fields in Q12, a 9-point Laplacian (corners 0.05,
//       sides 0.2, centre -1), edges wrapping.
//     R:seed(x, y, r)      a disc of B at (x, y)
//     R:step([n])          n steps (1..32), default 1
//     R:params{f, k, da, db}
//     R:show(L)            B into the layer, 0..255
//
// Integers only, as the other shared headers (the fire's randomness is a
// xorshift seeded by the call), so the panel and luasim agree. px_raster.h
// and px_layer.h must be included first.
// ============================================================
#ifndef PX_SIM_H
#define PX_SIM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*PxsCharge)(lua_State *L, unsigned long instructions);

static lua_Number pxsim_num(lua_State *L, int t, const char *k, lua_Number def, lua_Number lo, lua_Number hi) {
  if (t == 0) return def;
  lua_pushstring(L, k);
  lua_rawget(L, t);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) { v = lua_tonumber(L, -1); if (!(v == v)) v = def; }
  else if (!lua_isnil(L, -1)) { lua_pop(L, 1); return luaL_error(L, "px.step: %s wants a number", k); }
  lua_pop(L, 1);
  return v < lo ? lo : v > hi ? hi : v;
}

// A rule "0".."8" digits into a 9-bit mask.
static int pxsim_rule(lua_State *L, int t, const char *k, const char *def) {
  const char *r = def;
  if (t) {
    lua_pushstring(L, k);
    lua_rawget(L, t);
    if (!lua_isnil(L, -1)) {
      r = lua_tostring(L, -1);
      if (!r) return luaL_error(L, "px.step: %s is a string of digits 0-8", k);
    }
  }
  int m = 0;
  for (const char *c = r; *c; c++) {
    if (*c < '0' || *c > '8') return luaL_error(L, "px.step: %s is a string of digits 0-8", k);
    m |= 1 << (*c - '0');
  }
  if (t) lua_pop(L, 1);
  return m;
}

// Scratch for the step that needs the layer as it was (life, fire's rows): a
// registry userdata, reused.
static char pxsim_key;
static unsigned char *pxsim_scratch(lua_State *L, size_t bytes) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &pxsim_key);
  unsigned char *b = (unsigned char *)lua_touserdata(L, -1);
  const size_t have = b ? lua_rawlen(L, -1) : 0;
  lua_pop(L, 1);
  if (b && have >= bytes) return b;
  b = (unsigned char *)lua_newuserdatauv(L, bytes, 0);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxsim_key);
  return b;
}

static const char *const pxsim_kinds[] = {"fire", "life", "wave", NULL};

// px.step(L, kind [, params])
static int px_step_lua(lua_State *L, PxsCharge charge) {
  PxLayer *l = pxl_check(L, 1);
  const int kind = luaL_checkoption(L, 2, NULL, pxsim_kinds);
  const int t = lua_istable(L, 3) ? 3 : 0;
  if (!t && !lua_isnoneornil(L, 3)) return luaL_error(L, "px.step: the parameters are a table");
  const int w = l->w, h = l->h;
  unsigned char *v = l->v;
  if (kind == 0) {                                   // fire
    const int cool = (int)(pxsim_num(L, t, "cool", 0.3, 0, 1) * 256);     // up to 16 levels a row
    const int heat = pxr_q8(pxsim_num(L, t, "heat", 1, 0, 1));
    uint32_t seed = (uint32_t)(int64_t)pxsim_num(L, t, "seed", 1, -2147483647.0, 2147483647.0) * 2654435761u | 1u;
    if (charge) charge(L, (unsigned long)w * h);
    // the bottom row: hot at random
    for (int x = 0; x < w; x++) {
      seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
      v[(h - 1) * w + x] = (unsigned char)(((seed >> 24) & 255) < (unsigned)heat ? 255 : (seed >> 16) & 63);
    }
    // everything above: the average of the row below (three) and two below, cooled
    for (int y = 0; y < h - 1; y++) {
      const unsigned char *b1 = &v[(y + 1) * w], *b2 = &v[(y + (y + 2 < h ? 2 : 1)) * w];
      unsigned char *o = &v[y * w];
      for (int x = 0; x < w; x++) {
        const int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : w - 1;
        const int s = (b1[xl] + b1[x] + b1[xr] + b2[x]) * 64;       // x 256 / 4
        int n = (s - cool * 16) >> 8;
        o[x] = (unsigned char)(n < 0 ? 0 : n > 255 ? 255 : n);
      }
    }
  } else if (kind == 1) {                            // life
    const int decay = pxr_q8(pxsim_num(L, t, "decay", 0.1, 0, 1));
    const int born = pxsim_rule(L, t, "born", "3"), survive = pxsim_rule(L, t, "survive", "23");
    if (charge) charge(L, (unsigned long)w * h * 2);
    unsigned char *old = pxsim_scratch(L, (size_t)w * h);
    memcpy(old, v, (size_t)w * h);
    for (int y = 0; y < h; y++) {
      const int yu = (y + h - 1) % h, yd = (y + 1) % h;
      for (int x = 0; x < w; x++) {
        const int xl = (x + w - 1) % w, xr = (x + 1) % w;
        const int n = (old[yu * w + xl] > 127) + (old[yu * w + x] > 127) + (old[yu * w + xr] > 127) +
                      (old[y * w + xl] > 127) + (old[y * w + xr] > 127) +
                      (old[yd * w + xl] > 127) + (old[yd * w + x] > 127) + (old[yd * w + xr] > 127);
        const int alive = old[y * w + x] > 127;
        const int next = alive ? (survive >> n) & 1 : (born >> n) & 1;
        if (next) v[y * w + x] = 255;
        else {
          const int f = alive ? 127 : old[y * w + x];                 // a death starts its trail at half
          v[y * w + x] = (unsigned char)((f * (256 - decay)) >> 8);
        }
      }
    }
  } else {                                           // wave
    if (!t) return luaL_error(L, "px.step: \"wave\" wants {prev = another layer}");
    lua_pushliteral(L, "prev");
    lua_rawget(L, t);
    PxLayer *p = (PxLayer *)luaL_testudata(L, -1, PXL_META);
    if (!p || p == l || p->w != w || p->h != h) return luaL_error(L, "px.step: prev is another layer of the same size");
    lua_pop(L, 1);
    const int damp = pxr_q8(pxsim_num(L, t, "damp", 0.02, 0, 1));
    if (charge) charge(L, (unsigned long)w * h);
    unsigned char *q = p->v;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const int c = y * w + x;
        const int s = (x > 0 ? v[c - 1] - 128 : 0) + (x < w - 1 ? v[c + 1] - 128 : 0) +
                      (y > 0 ? v[c - w] - 128 : 0) + (y < h - 1 ? v[c + w] - 128 : 0);
        int n = s / 2 - (q[c] - 128);
        n = n * (256 - damp) / 256;           // toward zero both ways: a shift would sink the water
        n = n < -128 ? -128 : n > 127 ? 127 : n;
        q[c] = (unsigned char)(n + 128);
      }
  }
  return 0;
}

// ---- Gray-Scott reaction-diffusion ----
#define PXR_META "px.reaction"
typedef struct {
  int w, h;
  int32_t f, k, da, db;           // Q12
  PxsCharge charge;
  int16_t *a, *b, *a2, *b2;       // into data
  int16_t data[1];
} PxRD;

static PxRD *pxrd_check(lua_State *L) { return (PxRD *)luaL_checkudata(L, 1, PXR_META); }

static void pxrd_params(lua_State *L, PxRD *r, int t) {
  r->f = (int32_t)(pxsim_num(L, t, "f", (lua_Number)r->f / 4096, 0, 0.2) * 4096);
  r->k = (int32_t)(pxsim_num(L, t, "k", (lua_Number)r->k / 4096, 0, 0.2) * 4096);
  r->da = (int32_t)(pxsim_num(L, t, "da", (lua_Number)r->da / 4096, 0, 1) * 4096);
  r->db = (int32_t)(pxsim_num(L, t, "db", (lua_Number)r->db / 4096, 0, 1) * 4096);
}

static int pxrd_params_lua(lua_State *L) {
  PxRD *r = pxrd_check(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  pxrd_params(L, r, 2);
  return 0;
}

// R:seed(x, y, r): B = 1 in a disc
static int pxrd_seed(lua_State *L) {
  PxRD *r = pxrd_check(L);
  const lua_Integer cx = luaL_checkinteger(L, 2), cy = luaL_checkinteger(L, 3), rad = luaL_optinteger(L, 4, 3);
  if (rad < 0 || rad > 64) return luaL_error(L, "reaction: a seed's radius is 0 to 64");
  for (lua_Integer y = cy - rad; y <= cy + rad; y++)
    for (lua_Integer x = cx - rad; x <= cx + rad; x++) {
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > rad * rad) continue;
      const int xx = (int)(((x % r->w) + r->w) % r->w), yy = (int)(((y % r->h) + r->h) % r->h);
      r->b[yy * r->w + xx] = 4096;
      r->a[yy * r->w + xx] = 0;
    }
  return 0;
}

// R:step([n])
static int pxrd_step(lua_State *L) {
  PxRD *r = pxrd_check(L);
  const lua_Integer n = luaL_optinteger(L, 2, 1);
  if (n < 1 || n > 32) return luaL_error(L, "reaction: 1 to 32 steps a call");
  const int w = r->w, h = r->h;
  if (r->charge) r->charge(L, (unsigned long)w * h * 4 * (unsigned long)n);   // an estimate until measured
  for (lua_Integer s = 0; s < n; s++) {
    for (int y = 0; y < h; y++) {
      // rows above and below, wrapping; columns wrap only at the two ends
      const int16_t *au = &r->a[((y + h - 1) % h) * w], *ac = &r->a[y * w], *ad = &r->a[((y + 1) % h) * w];
      const int16_t *bu = &r->b[((y + h - 1) % h) * w], *bc = &r->b[y * w], *bd = &r->b[((y + 1) % h) * w];
      int16_t *oa = &r->a2[y * w], *ob = &r->b2[y * w];
      for (int x = 0; x < w; x++) {
        const int xl = x ? x - 1 : w - 1, xr = x < w - 1 ? x + 1 : 0;
        const int32_t A = ac[x], B = bc[x];
        // 9-point Laplacian, weights in 1/20: sides 4, corners 1, centre -20;
        // the /20 as * 3277 >> 16 (a division a cell is slow on this core)
        const int32_t la = ((4 * (ac[xl] + ac[xr] + au[x] + ad[x]) + au[xl] + au[xr] + ad[xl] + ad[xr] - 20 * A) * 3277) >> 16;
        const int32_t lb = ((4 * (bc[xl] + bc[xr] + bu[x] + bd[x]) + bu[xl] + bu[xr] + bd[xl] + bd[xr] - 20 * B) * 3277) >> 16;
        const int32_t abb = (int32_t)(((int64_t)A * B >> 12) * B >> 12);                 // A B^2, Q12
        int32_t na = A + ((r->da * la) >> 12) - abb + ((r->f * (4096 - A)) >> 12);
        int32_t nb = B + ((r->db * lb) >> 12) + abb - (((r->k + r->f) * B) >> 12);
        oa[x] = (int16_t)(na < 0 ? 0 : na > 4096 ? 4096 : na);
        ob[x] = (int16_t)(nb < 0 ? 0 : nb > 4096 ? 4096 : nb);
      }
    }
    int16_t *ta = r->a, *tb = r->b;
    r->a = r->a2; r->b = r->b2; r->a2 = ta; r->b2 = tb;
  }
  return 0;
}

// R:show(L): B into the layer
static int pxrd_show(lua_State *L) {
  PxRD *r = pxrd_check(L);
  lua_remove(L, 1);
  PxLayer *l = pxl_check(L, 1);
  if (l->w != r->w || l->h != r->h) return luaL_error(L, "reaction: the layer is not the reaction's size");
  for (int i = 0; i < r->w * r->h; i++) {
    const int v = r->b[i] >> 4;                      // Q12 -> 0..256
    l->v[i] = (unsigned char)(v > 255 ? 255 : v);
  }
  return 0;
}

static int pxrd_clear(lua_State *L) {
  PxRD *r = pxrd_check(L);
  for (int i = 0; i < r->w * r->h; i++) { r->a[i] = 4096; r->b[i] = 0; }
  return 0;
}

// px.reaction{f, k, da, db} -> R
static int px_reaction_lua(lua_State *L, int w, int h, PxsCharge charge) {
  const int t = lua_istable(L, 1) ? 1 : 0;
  const size_t cells = (size_t)w * h;
  PxRD *r = (PxRD *)lua_newuserdatauv(L, offsetof(PxRD, data) + 4 * cells * sizeof(int16_t), 0);
  r->w = w; r->h = h; r->charge = charge;
  r->f = 225; r->k = 254; r->da = 4096; r->db = 2048;        // 0.055, 0.062, 1, 0.5
  r->a = r->data; r->b = r->a + cells; r->a2 = r->b + cells; r->b2 = r->a2 + cells;
  const int top = lua_gettop(L);
  if (t) pxrd_params(L, r, t);
  lua_settop(L, top);
  for (size_t i = 0; i < cells; i++) { r->a[i] = 4096; r->b[i] = 0; }
  if (luaL_newmetatable(L, PXR_META)) {
    static const luaL_Reg m[] = {{"seed", pxrd_seed}, {"step", pxrd_step}, {"params", pxrd_params_lua},
                                 {"show", pxrd_show}, {"clear", pxrd_clear}, {NULL, NULL}};
    luaL_newlib(L, m);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "px.reaction");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
  if (charge) charge(L, (unsigned long)cells / 2);
  return 1;
}

#endif

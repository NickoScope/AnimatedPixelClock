// ============================================================
// px_draw.h - anti-aliased lines and dots, triangles, and 3D meshes
// ============================================================
// Smooth, slow motion on a 128x64 grid needs sub-pixel drawing, or a line
// crawls in steps; a solid needs triangles; a turning body needs its
// vertices turned, projected and drawn every frame - which in Lua is most of
// a frame. All in integers, shared by the firmware and luasim:
//
//   px.aline(x0, y0, x1, y1, r, g, b [, a])   an anti-aliased line, ends in
//       fractions of a pixel (Xiaolin Wu's idea: the light of each step split
//       between the two pixels it falls between); a is 0..1
//   px.dot(x, y, r, g, b [, a])               a dot at a fraction of a pixel,
//       its light split over four
//   px.tri(x0, y0, x1, y1, x2, y2, r, g, b)   a filled triangle, pixel centres
//       inside it (top-left rule)
//   M = px.model{ v = {x, y, z, ...}, e = {i, j, ...}, f = {i, j, k, ...} }
//       vertices, edges (index pairs), faces (index triples, 1-based,
//       counter-clockwise seen from outside, or orient = true for a convex
//       body round the origin); up to 1024 / 2048 / 1024
//   px.mesh(M, { ax, ay, az, scale = 20, x = 63.5, y = 31.5, dist = 4,
//                r, g, b, mode = "wire" | "solid" | "both" })
//       M turned (radians about x, then y, then z), placed, seen from dist
//       units away, and drawn: wire - every edge anti-aliased, dimmer with
//       depth; solid - the faces toward the viewer, farthest first, lit by a
//       light at the viewer (some ambient); both - solid, then the edges of the faces toward the viewer.
//
// All four drawing calls follow px.mode: in "add" the light adds up, in "set"
// it is mixed over the canvas by its coverage. Integers only: fractions in
// Q8, the turn from px_layer.h's cosine table. px_raster.h and px_layer.h
// must be included first.
// ============================================================
#ifndef PX_DRAW_H
#define PX_DRAW_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*PxdCharge)(lua_State *L, unsigned long instructions);

// one pixel with coverage c (Q8, 0..256)
static void pxd_plot(unsigned char *fb, int w, int h, int x, int y, int r, int g, int b, int c, int add) {
  if (x < 0 || x >= w || y < 0 || y >= h || c <= 0) return;
  if (c > 256) c = 256;
  unsigned char *p = &fb[(y * w + x) * 3];
  if (add) {
    const int nr = p[0] + ((r * c) >> 8), ng = p[1] + ((g * c) >> 8), nb = p[2] + ((b * c) >> 8);
    p[0] = (unsigned char)(nr > 255 ? 255 : nr); p[1] = (unsigned char)(ng > 255 ? 255 : ng);
    p[2] = (unsigned char)(nb > 255 ? 255 : nb);
  } else {
    pxr_mix(p, r, g, b, c);
  }
}

// A coordinate to Q8, clamped to +-8192 px first (NaN to 0).
static int32_t pxd_q8(lua_Number v) {
  if (!(v == v)) return 0;
  if (v > 8192) v = 8192;
  if (v < -8192) v = -8192;
  return (int32_t)(v * 256);
}

// The line in Q8 from (x0, y0) to (x1, y1), clipped to a box around the
// canvas first so the loop is short whatever the ends. Returns the steps.
static int pxd_aline(unsigned char *fb, int w, int h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int r, int g, int b, int a, int add) {
  const int steep = (y1 - y0 < 0 ? y0 - y1 : y1 - y0) > (x1 - x0 < 0 ? x0 - x1 : x1 - x0);
  if (steep) { int32_t t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
  if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  const int lim = (steep ? h : w) + 2;
  const int32_t dx = x1 - x0, dy = y1 - y0;
  // the gradient, Q16 minor per major
  const int64_t grad = dx ? ((int64_t)dy * 65536) / dx : 0;     // multiplied, not shifted: dy may be negative
  int xs = (x0 + 255) >> 8, xe = x1 >> 8;                 // pixel columns whose centres lie on the line
  if (xs < -2) xs = -2;
  if (xe > lim) xe = lim;
  int n = 0;
  for (int x = xs; x <= xe; x++, n++) {
    const int64_t yq = (int64_t)y0 * 256 + grad * ((int64_t)x * 256 - x0) / 256;   // Q16
    const int yi = (int)(yq >> 16), f = (int)((yq >> 8) & 255);
    if (steep) {
      pxd_plot(fb, w, h, yi, x, r, g, b, ((256 - f) * a) >> 8, add);
      pxd_plot(fb, w, h, yi + 1, x, r, g, b, (f * a) >> 8, add);
    } else {
      pxd_plot(fb, w, h, x, yi, r, g, b, ((256 - f) * a) >> 8, add);
      pxd_plot(fb, w, h, x, yi + 1, r, g, b, (f * a) >> 8, add);
    }
  }
  return n;
}

// A dot at (x, y) in Q8, its light over four pixels.
static void pxd_dot(unsigned char *fb, int w, int h, int32_t x, int32_t y, int r, int g, int b, int a, int add) {
  const int ix = x >> 8, iy = y >> 8, fx = x & 255, fy = y & 255;
  pxd_plot(fb, w, h, ix, iy, r, g, b, (((256 - fx) * (256 - fy)) >> 8) * a >> 8, add);
  pxd_plot(fb, w, h, ix + 1, iy, r, g, b, ((fx * (256 - fy)) >> 8) * a >> 8, add);
  pxd_plot(fb, w, h, ix, iy + 1, r, g, b, (((256 - fx) * fy) >> 8) * a >> 8, add);
  pxd_plot(fb, w, h, ix + 1, iy + 1, r, g, b, ((fx * fy) >> 8) * a >> 8, add);
}

// A triangle in Q8, pixel centres inside by edge functions (top-left rule).
// Returns the pixels looked at.
static long pxd_tri(unsigned char *fb, int w, int h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    int32_t x2, int32_t y2, int r, int g, int b, int add) {
  // counter-clockwise on screen (y down) as the edge functions want it
  int64_t area = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(y1 - y0) * (x2 - x0);
  if (area == 0) return 0;
  if (area < 0) { int32_t t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
  int minx = (int)((x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2)) >> 8);
  int maxx = (int)((x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) >> 8) + 1;
  int miny = (int)((y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2)) >> 8);
  int maxy = (int)((y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) >> 8) + 1;
  if (minx < 0) minx = 0;
  if (miny < 0) miny = 0;
  if (maxx > w - 1) maxx = w - 1;
  if (maxy > h - 1) maxy = h - 1;
  if (minx > maxx || miny > maxy) return 0;
  const int32_t X[3] = {x0, x1, x2}, Y[3] = {y0, y1, y2};
  long looked = 0;
  for (int py = miny; py <= maxy; py++) {
    const int32_t cy = py * 256 + 128;
    for (int px = minx; px <= maxx; px++, looked++) {
      const int32_t cx = px * 256 + 128;
      int in = 1;
      for (int e = 0; e < 3 && in; e++) {
        const int32_t ax = X[e], ay = Y[e], bx = X[(e + 1) % 3], by = Y[(e + 1) % 3];
        const int64_t ef = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);
        // top-left: a pixel on an edge belongs to the top or left edge only
        const int topleft = (by == ay && bx < ax) || (by < ay);
        in = ef > 0 || (ef == 0 && topleft);
      }
      if (in) pxd_plot(fb, w, h, px, py, r, g, b, 256, add);
    }
  }
  return looked;
}

// ---- Lua bindings for the three primitives ----
static int pxd_colour(lua_State *L, int i) {
  return pxl_num255(luaL_checknumber(L, i));
}

static int px_aline_lua(lua_State *L, unsigned char *fb, int w, int h, int add, PxdCharge charge) {
  const int32_t x0 = pxd_q8(luaL_checknumber(L, 1)), y0 = pxd_q8(luaL_checknumber(L, 2));
  const int32_t x1 = pxd_q8(luaL_checknumber(L, 3)), y1 = pxd_q8(luaL_checknumber(L, 4));
  const int r = pxd_colour(L, 5), g = pxd_colour(L, 6), b = pxd_colour(L, 7);
  const int a = lua_isnoneornil(L, 8) ? 256 : pxr_q8(luaL_checknumber(L, 8));
  if (charge) charge(L, 2 * 136);                          // at most ~130 steps, two pixels each
  pxd_aline(fb, w, h, x0, y0, x1, y1, r, g, b, a, add);
  return 0;
}
static int px_dot_lua(lua_State *L, unsigned char *fb, int w, int h, int add) {
  const int32_t x = pxd_q8(luaL_checknumber(L, 1)), y = pxd_q8(luaL_checknumber(L, 2));
  const int r = pxd_colour(L, 3), g = pxd_colour(L, 4), b = pxd_colour(L, 5);
  const int a = lua_isnoneornil(L, 6) ? 256 : pxr_q8(luaL_checknumber(L, 6));
  pxd_dot(fb, w, h, x, y, r, g, b, a, add);
  return 0;
}
static int px_tri_lua(lua_State *L, unsigned char *fb, int w, int h, int add, PxdCharge charge) {
  int32_t c[6];
  for (int i = 0; i < 6; i++) c[i] = pxd_q8(luaL_checknumber(L, i + 1));
  const int r = pxd_colour(L, 7), g = pxd_colour(L, 8), b = pxd_colour(L, 9);
  // the box, before the work: at most the canvas
  if (charge) {
    int32_t mnx = c[0], mxx = c[0], mny = c[1], mxy = c[1];
    for (int i = 2; i < 6; i += 2) {
      mnx = c[i] < mnx ? c[i] : mnx; mxx = c[i] > mxx ? c[i] : mxx;
      mny = c[i + 1] < mny ? c[i + 1] : mny; mxy = c[i + 1] > mxy ? c[i + 1] : mxy;
    }
    long bw = (mxx >> 8) - (mnx >> 8) + 2, bh = (mxy >> 8) - (mny >> 8) + 2;
    bw = bw > w ? w : bw; bh = bh > h ? h : bh;
    charge(L, (unsigned long)(bw * bh) / 2);
  }
  pxd_tri(fb, w, h, c[0], c[1], c[2], c[3], c[4], c[5], r, g, b, add);
  return 0;
}

// ---- models and meshes ----
#define PXM_META "px.model"
#define PXM_V 1024
#define PXM_E 2048
#define PXM_F 1024
typedef struct {
  int nv, ne, nf;
  int32_t *v;          // Q12 x, y, z
  uint16_t *e, *f;     // 0-based indices
  int32_t *sx, *sy, *sz;   // the last projection: Q8 screen, Q12 depth
  int32_t *depth;          // the faces' depths and order for the sort: in the
  uint16_t *order;         // model, not on the effect task's small stack
  unsigned char *front;    // a face toward the viewer, this frame
  int16_t *ef;             // each edge's two faces (-1: none), found once
  int32_t *fn;             // each face's normal in the model, Q12
  int32_t data[1];
} PxModel;

static int pxm_read(lua_State *L, int t, const char *k, int stride, int maxn, int nv, int32_t *outI, uint16_t *outU, int isVerts) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  int n = 0;
  if (lua_istable(L, -1)) {
    const int len = (int)lua_rawlen(L, -1);
    if (len % stride) return luaL_error(L, "px.model: %s has %d numbers, not a multiple of %d", k, len, stride);
    n = len / stride;
    if (n > maxn) return luaL_error(L, "px.model: at most %d in %s", maxn, k);
    if (outI || outU) {
      for (int i = 0; i < len; i++) {
        lua_rawgeti(L, -1, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "px.model: %s holds numbers", k);
        const lua_Number x = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (isVerts) {
          lua_Number c = !(x == x) ? 0 : x > 64 ? 64 : x < -64 ? -64 : x;
          outI[i] = (int32_t)(c * 4096);
        } else {
          if (!(x >= 1 && x <= nv) || (lua_Number)(int)x != x) return luaL_error(L, "px.model: %s index %d is not a vertex", k, i + 1);
          outU[i] = (uint16_t)((int)x - 1);
        }
      }
    }
  } else if (!lua_isnil(L, -1)) return luaL_error(L, "px.model: %s is a table of numbers", k);
  lua_pop(L, 1);
  return n;
}

static int px_model_lua(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const int nv = pxm_read(L, 1, "v", 3, PXM_V, 0, NULL, NULL, 1);
  const int ne = pxm_read(L, 1, "e", 2, PXM_E, nv, NULL, NULL, 0);
  const int nf = pxm_read(L, 1, "f", 3, PXM_F, nv, NULL, NULL, 0);
  if (nv < 1) return luaL_error(L, "px.model: no vertices");
  const size_t words = (size_t)nv * 3 + (size_t)nv * 3 + (size_t)nf + (size_t)nf * 3;   // v; sx sy sz; depth; fn
  const size_t shorts = (size_t)ne * 2 + (size_t)nf * 3 + (size_t)nf + (size_t)ne * 2;  // e; f; order; ef
  PxModel *m = (PxModel *)lua_newuserdatauv(L, offsetof(PxModel, data) + words * 4 + shorts * 2 + (size_t)nf + 4, 0);
  m->nv = nv; m->ne = ne; m->nf = nf;
  m->v = m->data;
  m->sx = m->v + nv * 3; m->sy = m->sx + nv; m->sz = m->sy + nv;
  m->depth = m->sz + nv;
  m->fn = m->depth + nf;
  m->e = (uint16_t *)(m->fn + nf * 3);
  m->f = m->e + ne * 2;
  m->order = m->f + nf * 3;
  m->ef = (int16_t *)(m->order + nf);
  m->front = (unsigned char *)(m->ef + ne * 2);
  const int ud = lua_gettop(L);
  pxm_read(L, 1, "v", 3, PXM_V, 0, m->v, NULL, 1);
  pxm_read(L, 1, "e", 2, PXM_E, nv, NULL, m->e, 0);
  pxm_read(L, 1, "f", 3, PXM_F, nv, NULL, m->f, 0);
  lua_settop(L, ud);
  // orient = true: a convex body round the origin; each face turned to face
  // outward (its normal away from the centre), whatever order it was given in
  lua_pushliteral(L, "orient");
  lua_rawget(L, 1);
  const int orient = lua_toboolean(L, -1);
  lua_pop(L, 1);
  if (orient)
    for (int i = 0; i < nf; i++) {
      const int32_t *a = &m->v[m->f[i * 3] * 3], *b = &m->v[m->f[i * 3 + 1] * 3], *c = &m->v[m->f[i * 3 + 2] * 3];
      const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
      const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
      const int64_t nx = (uy * vz - uz * vy) >> 12, ny = (uz * vx - ux * vz) >> 12, nz = (ux * vy - uy * vx) >> 12;
      const int64_t cx = (int64_t)a[0] + b[0] + c[0], cy = (int64_t)a[1] + b[1] + c[1], cz = (int64_t)a[2] + b[2] + c[2];
      if ((nx * cx + ny * cy + nz * cz) < 0) { const uint16_t t = m->f[i * 3 + 1]; m->f[i * 3 + 1] = m->f[i * 3 + 2]; m->f[i * 3 + 2] = t; }
    }
  // each face's normal, (b - a) x (c - a) in the model, scaled to Q12 length
  for (int i = 0; i < nf; i++) {
    const int32_t *a = &m->v[m->f[i * 3] * 3], *b = &m->v[m->f[i * 3 + 1] * 3], *c = &m->v[m->f[i * 3 + 2] * 3];
    const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    int64_t nx = (uy * vz - uz * vy) >> 12, ny = (uz * vx - ux * vz) >> 12, nz = (ux * vy - uy * vx) >> 12;
    // to about Q12 length: halve until it fits, then divide by the length
    while (nx > 32767 || nx < -32767 || ny > 32767 || ny < -32767 || nz > 32767 || nz < -32767) { nx /= 2; ny /= 2; nz /= 2; }
    const int64_t len = pxr_isqrt((unsigned)(nx * nx + ny * ny + nz * nz));
    m->fn[i * 3] = len ? (int32_t)(nx * 4096 / len) : 0;
    m->fn[i * 3 + 1] = len ? (int32_t)(ny * 4096 / len) : 0;
    m->fn[i * 3 + 2] = len ? (int32_t)(nz * 4096 / len) : 0;
  }
  // each edge's faces: once, here (up to 2048 x 1024 comparisons of a load)
  for (int i = 0; i < ne; i++) {
    const int a = m->e[i * 2], b = m->e[i * 2 + 1];
    int k = 0;
    m->ef[i * 2] = m->ef[i * 2 + 1] = -1;
    for (int j = 0; j < nf && k < 2; j++) {
      const int p = m->f[j * 3], q = m->f[j * 3 + 1], r = m->f[j * 3 + 2];
      const int hasA = a == p || a == q || a == r, hasB = b == p || b == q || b == r;
      if (hasA && hasB) m->ef[i * 2 + k++] = (int16_t)j;
    }
  }
  if (luaL_newmetatable(L, PXM_META)) {
    lua_pushliteral(L, "px.model");
    lua_setfield(L, -2, "__metatable");
  }
  lua_setmetatable(L, -2);
  return 1;
}

static lua_Number pxd_opt(lua_State *L, int t, const char *k, lua_Number def, lua_Number lo, lua_Number hi) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) { v = lua_tonumber(L, -1); if (!(v == v)) v = def; }
  else if (!lua_isnil(L, -1)) { lua_pop(L, 1); return luaL_error(L, "px.mesh: %s wants a number", k); }
  lua_pop(L, 1);
  return v < lo ? lo : v > hi ? hi : v;
}

static const char *const pxm_modes[] = {"wire", "solid", "both", NULL};

// px.mesh(M, opts)
static int px_mesh_lua(lua_State *L, unsigned char *fb, int w, int h, int add, PxdCharge charge) {
  PxModel *m = (PxModel *)luaL_checkudata(L, 1, PXM_META);
  luaL_checktype(L, 2, LUA_TTABLE);
  const int32_t ax = pxn_turns(pxd_opt(L, 2, "ax", 0, -1000, 1000));
  const int32_t ay = pxn_turns(pxd_opt(L, 2, "ay", 0, -1000, 1000));
  const int32_t az = pxn_turns(pxd_opt(L, 2, "az", 0, -1000, 1000));
  const int32_t scale = (int32_t)(pxd_opt(L, 2, "scale", 20, 0, 1000) * 256);       // Q8 px a unit
  const int32_t ox = pxd_q8(pxd_opt(L, 2, "x", (lua_Number)(w - 1) / 2, -4096, 4096));
  const int32_t oy = pxd_q8(pxd_opt(L, 2, "y", (lua_Number)(h - 1) / 2, -4096, 4096));
  const int32_t dist = (int32_t)(pxd_opt(L, 2, "dist", 4, 1.5, 1000) * 4096);       // Q12 units
  const int cr = pxl_num255(pxd_opt(L, 2, "r", 255, 0, 255)), cg = pxl_num255(pxd_opt(L, 2, "g", 255, 0, 255)),
            cb = pxl_num255(pxd_opt(L, 2, "b", 255, 0, 255));
  lua_pushliteral(L, "mode");
  lua_rawget(L, 2);
  int mode = 0;
  if (!lua_isnil(L, -1)) {
    const char *s = lua_tostring(L, -1);
    for (mode = 0; s && pxm_modes[mode] && strcmp(pxm_modes[mode], s) != 0; mode++) {}
    if (!s || !pxm_modes[mode]) return luaL_error(L, "px.mesh: mode is \"wire\", \"solid\" or \"both\"");
  }
  lua_pop(L, 1);
  if (charge) charge(L, (unsigned long)m->nv * 20 + (unsigned long)(mode != 1 ? m->ne * 300 : 0) +
                        (unsigned long)(mode != 0 ? m->nf * 20 : 0));          // the faces' fill: below, by their boxes

  // the turn: x, then y, then z, Q15 cosines and sines
  const int64_t cxr = pxl_cosq((uint32_t)ax & 0xFFFF), sxr = pxl_cosq(((uint32_t)ax - 16384u) & 0xFFFF);
  const int64_t cyr = pxl_cosq((uint32_t)ay & 0xFFFF), syr = pxl_cosq(((uint32_t)ay - 16384u) & 0xFFFF);
  const int64_t czr = pxl_cosq((uint32_t)az & 0xFFFF), szr = pxl_cosq(((uint32_t)az - 16384u) & 0xFFFF);
  for (int i = 0; i < m->nv; i++) {
    const int64_t x = m->v[i * 3], y = m->v[i * 3 + 1], z = m->v[i * 3 + 2];      // Q12
    const int64_t y1 = (y * cxr - z * sxr) >> 15, z1 = (y * sxr + z * cxr) >> 15;
    const int64_t x2 = (x * cyr + z1 * syr) >> 15, z2 = (z1 * cyr - x * syr) >> 15;
    const int64_t x3 = (x2 * czr - y1 * szr) >> 15, y3 = (x2 * szr + y1 * czr) >> 15;
    // perspective: the camera dist units in front; y up in the model, down on screen
    int64_t d = z2 + dist;
    if (d < 410) d = 410;                                  // 0.1 of a unit: nothing behind the eye
    const int64_t k = ((int64_t)dist << 12) / d;           // Q12, 1 at the model's centre
    // held to +-8192 px as px.aline's and px.tri's own ends are, so every
    // difference later stays in 32 bits (a vertex at the eye with a big scale
    // otherwise ran past them)
    const int64_t LIM = (int64_t)8192 * 256;
    int64_t X = ox + ((x3 * scale >> 12) * k >> 12), Y = oy - ((y3 * scale >> 12) * k >> 12);
    X = X > LIM ? LIM : X < -LIM ? -LIM : X;
    Y = Y > LIM ? LIM : Y < -LIM ? -LIM : Y;
    m->sx[i] = (int32_t)X;
    m->sy[i] = (int32_t)Y;
    m->sz[i] = (int32_t)z2;
  }
  if (mode != 0 && m->nf) {
    // the faces toward the viewer, farthest first (insertion sort: stable, and
    // a model's faces are few)
    uint16_t *order = m->order;
    int32_t *depth = m->depth;
    int n = 0;
    memset(m->front, 0, (size_t)m->nf);
    for (int i = 0; i < m->nf; i++) {
      const int a = m->f[i * 3], b = m->f[i * 3 + 1], c = m->f[i * 3 + 2];
      const int64_t cross = (int64_t)(m->sx[b] - m->sx[a]) * (m->sy[c] - m->sy[a]) -
                            (int64_t)(m->sy[b] - m->sy[a]) * (m->sx[c] - m->sx[a]);
      if (cross <= 0) continue;                            // a face turned away (its vertices run the other way round on screen)
      m->front[i] = 1;
      const int32_t dz = m->sz[a] + m->sz[b] + m->sz[c];
      int j = n++;
      while (j > 0 && depth[j - 1] < dz) { depth[j] = depth[j - 1]; order[j] = order[j - 1]; j--; }
      depth[j] = dz; order[j] = (uint16_t)i;
    }
    // the fill costs its faces' boxes (clipped to the canvas), charged before
    // any is drawn, as px.tri charges its own
    if (charge) {
      unsigned long area = 0;
      for (int k = 0; k < n; k++) {
        const int i = order[k];
        const int a = m->f[i * 3], b = m->f[i * 3 + 1], c = m->f[i * 3 + 2];
        int32_t x0 = m->sx[a], x1 = m->sx[a], y0 = m->sy[a], y1 = m->sy[a];
        x0 = m->sx[b] < x0 ? m->sx[b] : x0; x1 = m->sx[b] > x1 ? m->sx[b] : x1;
        x0 = m->sx[c] < x0 ? m->sx[c] : x0; x1 = m->sx[c] > x1 ? m->sx[c] : x1;
        y0 = m->sy[b] < y0 ? m->sy[b] : y0; y1 = m->sy[b] > y1 ? m->sy[b] : y1;
        y0 = m->sy[c] < y0 ? m->sy[c] : y0; y1 = m->sy[c] > y1 ? m->sy[c] : y1;
        long bw = (x1 >> 8) - (x0 >> 8) + 2, bh = (y1 >> 8) - (y0 >> 8) + 2;
        bw = bw > w ? w : bw; bh = bh > h ? h : bh;
        area += (unsigned long)(bw * bh);
      }
      charge(L, area / 2);
    }
    for (int k = 0; k < n; k++) {
      const int i = order[k];
      const int a = m->f[i * 3], b = m->f[i * 3 + 1], c = m->f[i * 3 + 2];
      // light from the viewer: the face's normal, turned as the vertices are;
      // the viewer looks along +z, so a face toward it has a normal with z < 0
      const int64_t nx = m->fn[i * 3], ny = m->fn[i * 3 + 1], nz = m->fn[i * 3 + 2];
      const int64_t nz1 = (ny * sxr + nz * cxr) >> 15;
      const int64_t nz2 = (nz1 * cyr - nx * syr) >> 15;
      int64_t lz = -nz2 * 256 / 4096;                      // 0..256 toward the viewer
      lz = lz < 0 ? 0 : lz > 256 ? 256 : lz;
      const int shade = 40 + (int)(lz * 216 / 256);
      pxd_tri(fb, w, h, m->sx[a], m->sy[a], m->sx[b], m->sy[b], m->sx[c], m->sy[c],
              (cr * shade) >> 8, (cg * shade) >> 8, (cb * shade) >> 8, add);
    }
  }
  if (mode != 1) {
    const int onlyFront = mode == 2 && m->nf;
    for (int i = 0; i < m->ne; i++) {
      const int a = m->e[i * 2], b = m->e[i * 2 + 1];
      if (onlyFront) {                                     // an edge of no face toward the viewer is behind
        const int f0 = m->ef[i * 2], f1 = m->ef[i * 2 + 1];
        if (f0 >= 0 && !m->front[f0] && (f1 < 0 || !m->front[f1])) continue;
      }
      // nearer is brighter: depth -1..1 unit maps to 1.25..0.45
      int64_t zc = ((int64_t)m->sz[a] + m->sz[b]) / 2;
      int k = 256 - (int)((zc * 100) >> 12);
      k = k < 110 ? 110 : k > 320 ? 320 : k;
      // over a solid, an edge lighter than its faces: halfway to white
      const int er = onlyFront ? (cr + 255) / 2 : cr, eg = onlyFront ? (cg + 255) / 2 : cg, eb = onlyFront ? (cb + 255) / 2 : cb;
      const int r = (er * k) >> 8, g = (eg * k) >> 8, b2 = (eb * k) >> 8;
      pxd_aline(fb, w, h, m->sx[a], m->sy[a], m->sx[b], m->sy[b], r > 255 ? 255 : r, g > 255 ? 255 : g,
                b2 > 255 ? 255 : b2, 256, add);
    }
  }
  return 0;
}

#endif

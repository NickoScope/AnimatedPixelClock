// ============================================================
// px_terrain.h - px.terrain(t): a height field seen from a camera
// ============================================================
// One call draws ground that goes into the distance: every column of the
// screen walks out from the camera and fills each piece of ground that rises
// above what the column already has (the "voxel space" of Comanche, 1992),
// lit by the sun, with a surface pattern per kind of ground, water that
// carries the sky, and haze with distance.
//
// Why it is native. In Lua the same walk is about a million instructions a
// frame - 450 ms at the panel's 410 ns an instruction - so a script could only
// replay views made in advance and never move its camera. Here it is a few
// milliseconds, and a golf hole can be flown over at the frame rate.
//
// Shared, word for word, by the firmware (src/lua/lua_px.cpp), the simulator
// (tools/luasim/luasim.c) and fxhost: fx_parity.py compares the last two
// pixel for pixel, and they can only agree because this is one source. Plain
// C, single-precision floats (the ESP32-S3 has a float unit, not a double
// one), and every loop bounded by its arguments: a C loop never reaches the
// instruction hook, so the limits below are what keeps a frame finite.
//
//   px.terrain{
//     grid = {w=, h=, cell=, x0=, y0=,        -- cells, metres a cell, the origin
//             height="...", hbase=, hscale=,  -- a byte a cell: hbase + byte*hscale m
//             kind="...",                     -- a byte a cell: which entry of kinds
//             light="..."},                   -- optional, a byte a cell, 128 = as lit
//     cam = {x=, y=, z=, yaw=, hor=, f=},     -- metres, radians, horizon row, focal px
//     kinds = "...",   -- 6 bytes a kind: r, g, b, pattern, period (m), amount (%)
//                      -- pattern 0 plain, 1 stripes across x, 2 checks, 3 grain,
//                      -- 4 water (the sky in it, glints)
//     haze = {r,g,b}, fog0 = m, fogr = m,     -- mixed in from fog0 over fogr, squared
//     far = {r,g,b}, farh = m,                -- beyond the grid: woods rising
//     sky = {r,g,b}, deep = {r,g,b},          -- what water shows, near and far
//     sun = {x,y,z},                          -- towards the sun, any length
//     colw = 1, step = 1.028, zfar = 900, grass = 60, frame = 0,
//     bilinear = 5000,   -- metres within which heights blend from four cells
//   }
// Numbers must be numbers: a number given as a string ("1.03") is ignored and
// the default used. step is held to 1.02..1.5, zfar to 2 km, colw to 1..8.
// ============================================================
#ifndef PX_TERRAIN_H
#define PX_TERRAIN_H

#include <math.h>
#include <string.h>

#define PX_TERRAIN_MAX_KINDS 32

typedef struct {
  float x, y, z, yaw, hor, f;
} PxTerrainCam;

// Every field is read with rawget: no __index of the script's runs in the
// middle of the call, so nothing can collect a string this function is still
// reading. A string that is used stays on the stack until the end (the audit
// of 2026-09-24 had an __index free them under our feet - ASan, use after free).
static void pxt_get(lua_State *L, int t, const char *k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
}

// NaN compares false both ways, so it lands on the low bound, never through
static inline float pxt_clamp(float v, float a, float b) { return v >= a ? (v <= b ? v : b) : a; }
static inline int pxt_finite(float v) { return v == v && v - v == 0.0f; }
// float to int only after the value is held to +-2^24, where the cast is defined
static inline int pxt_floori(float v) {
  v = pxt_clamp(v, -16777216.0f, 16777216.0f);
  const int t = (int)v;                    // truncates towards zero; exact floor after the fix-up,
  return t - (v < (float)t);               // and no floorf call (a library call on the Xtensa)
}
// hashes wrap as unsigned, where overflow is defined
static inline int pxt_hmod(unsigned v, unsigned m) { return (int)(v % m); }

static float pxt_num(lua_State *L, int t, const char *k, float dflt) {
  pxt_get(L, t, k);
  float v = lua_type(L, -1) == LUA_TNUMBER ? (float)lua_tonumber(L, -1) : dflt;
  lua_pop(L, 1);
  return v;
}

// a number that has to be finite: the camera, the grid's placement
static float pxt_fin(lua_State *L, int t, const char *k, float dflt) {
  const float v = pxt_num(L, t, k, dflt);
  if (!pxt_finite(v)) luaL_error(L, "px.terrain: %s is not a finite number", k);
  return v;
}

static void pxt_rgb(lua_State *L, int t, const char *k, float out[3], float r, float g, float b) {
  out[0] = r; out[1] = g; out[2] = b;
  pxt_get(L, t, k);
  if (lua_type(L, -1) == LUA_TTABLE) {
    const int i = lua_gettop(L);
    for (int c = 0; c < 3; c++) {
      lua_rawgeti(L, i, c + 1);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        const float v = (float)lua_tonumber(L, -1);
        if (pxt_finite(v)) out[c] = v;
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

// a string, left on the stack so it lives until the call returns
static const unsigned char *pxt_str(lua_State *L, int t, const char *k, size_t *len) {
  pxt_get(L, t, k);
  if (lua_type(L, -1) == LUA_TSTRING) return (const unsigned char *)lua_tolstring(L, -1, len);
  lua_pop(L, 1);
  *len = 0;
  return NULL;
}

// The most steps a column may take. A step multiplies the distance, so 320
// steps reach about 1.5 km at the lowest step allowed (1.02) and the full
// 2 km from 1.021 up; the golf's 1.03 needs about 210 to 900 m. One call is
// thus at most 128 x 320 samples, whatever the arguments say.
#define PX_TERRAIN_MAX_STEPS 320

// The one hot loop in px.*: the firmware is built with -Os, and this function
// alone is worth optimising for speed (GCC only; the host compilers ignore it).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif

// Returns the number of samples taken in *work (the firmware charges them to
// the frame's budget: src/lua/lua_px.cpp).
static int px_terrain_lua(lua_State *L, unsigned char *fb, int fbw, int fbh, unsigned long *work) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkstack(L, 24, "px.terrain");
  const int T = 1;
  *work = 0;

  // ---- the grid
  pxt_get(L, T, "grid");
  luaL_argcheck(L, lua_type(L, -1) == LUA_TTABLE, 1, "grid must be a table");
  const int G = lua_gettop(L);
  const int nx = (int)pxt_clamp(pxt_num(L, G, "w", 0), 0, 4096), ny = (int)pxt_clamp(pxt_num(L, G, "h", 0), 0, 4096);
  const float cell = pxt_fin(L, G, "cell", 1), gx0 = pxt_fin(L, G, "x0", 0), gy0 = pxt_fin(L, G, "y0", 0);
  const float hbase = pxt_fin(L, G, "hbase", 0), hscale = pxt_fin(L, G, "hscale", 0.05f);
  size_t hl = 0, kl = 0, ll = 0;
  const unsigned char *hgt = pxt_str(L, G, "height", &hl);
  const unsigned char *kind = pxt_str(L, G, "kind", &kl);
  const unsigned char *light = pxt_str(L, G, "light", &ll);
  luaL_argcheck(L, nx >= 2 && ny >= 2 && nx <= 1024 && ny <= 1024 && cell > 0.01f && cell < 1000, 1, "grid size");
  luaL_argcheck(L, hgt && hl >= (size_t)nx * ny, 1, "grid.height is shorter than w*h");
  luaL_argcheck(L, kind && kl >= (size_t)nx * ny, 1, "grid.kind is shorter than w*h");
  if (light && ll < (size_t)nx * ny) light = NULL;

  // ---- the camera
  pxt_get(L, T, "cam");
  luaL_argcheck(L, lua_type(L, -1) == LUA_TTABLE, 1, "cam must be a table");
  const int CI = lua_gettop(L);
  PxTerrainCam cam = {pxt_fin(L, CI, "x", 0), pxt_fin(L, CI, "y", 0), pxt_fin(L, CI, "z", 2),
                      pxt_fin(L, CI, "yaw", 0), pxt_fin(L, CI, "hor", fbh / 2), pxt_fin(L, CI, "f", 100)};
  luaL_argcheck(L, cam.f > 1 && cam.f < 10000, 1, "cam.f");
  luaL_argcheck(L, cam.x > -1e6f && cam.x < 1e6f && cam.y > -1e6f && cam.y < 1e6f &&
                cam.z > -1e5f && cam.z < 1e5f && cam.hor > -1e5f && cam.hor < 1e5f &&
                cam.yaw > -1e4f && cam.yaw < 1e4f, 1, "cam out of range");

  // ---- the look
  size_t kn = 0;
  const unsigned char *kinds = pxt_str(L, T, "kinds", &kn);
  luaL_argcheck(L, kinds && kn >= 6, 1, "kinds: 6 bytes a kind");
  const int nkinds = (int)(kn / 6) < PX_TERRAIN_MAX_KINDS ? (int)(kn / 6) : PX_TERRAIN_MAX_KINDS;
  float haze[3], far[3], sky[3], deep[3], sun[3];
  pxt_rgb(L, T, "haze", haze, 180, 200, 215);
  pxt_rgb(L, T, "far", far, 50, 80, 45);
  pxt_rgb(L, T, "sky", sky, 60, 120, 200);
  pxt_rgb(L, T, "deep", deep, 25, 60, 90);
  pxt_rgb(L, T, "sun", sun, -0.45f, -0.35f, 0.82f);
  {
    const float n = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (n > 1e-6f) { sun[0] /= n; sun[1] /= n; sun[2] /= n; }
  }
  const float fog0 = pxt_clamp(pxt_num(L, T, "fog0", 140), -1e6f, 1e6f);
  const float fogr = pxt_clamp(pxt_num(L, T, "fogr", 900), 1, 1e6f);
  const float farh = pxt_clamp(pxt_num(L, T, "farh", 4), -1e4f, 1e4f);
  const int colw = (int)pxt_clamp(pxt_num(L, T, "colw", 1), 1, 8);
  const float step = pxt_clamp(pxt_num(L, T, "step", 1.028f), 1.02f, 1.5f);
  const float zfar = pxt_clamp(pxt_num(L, T, "zfar", 900), 1, 2000);
  const float grass = pxt_clamp(pxt_num(L, T, "grass", 60), 0, 2000);
  const int frame = (int)pxt_clamp(pxt_num(L, T, "frame", 0), 0, 1e6f);
  // beyond this the height is the cell's own, not blended from four: far off
  // the difference is under a pixel, and it is one read of PSRAM, not four
  const float bilinear = pxt_clamp(pxt_num(L, T, "bilinear", 5000), 0, 5000);
  unsigned long samples = 0;               // counted here, not through *work: a store through
                                           // a pointer every step, the compiler cannot keep it

  const float gx1 = gx0 + nx * cell, gy1 = gy0 + ny * cell;
  const float inv = 1.0f / cell;

  for (int col = 0; col < fbw; col += colw) {
    const float ra = atanf(((float)col + colw * 0.5f - (fbw - 1) * 0.5f) / cam.f);
    const float a = cam.yaw + ra;
    const float dx = cosf(a), dy = sinf(a), cr = cosf(ra);
    int yb = fbh;
    float z = 0.6f;
    for (int stepn = 0; stepn < PX_TERRAIN_MAX_STEPS && z < zfar; stepn++) {
      samples++;
      const float x = cam.x + dx * z, y = cam.y + dy * z;
      const float fi = (x - gx0) * inv, fj = (y - gy0) * inv;
      const int i = pxt_floori(fi), j = pxt_floori(fj);
      // The height first, and only what rises above the column is coloured:
      // most samples are hidden behind nearer ground, and the light, the
      // pattern and the haze are the dear part.
      const int inside = i >= 0 && j >= 0 && i < nx - 1 && j < ny - 1;
      const int idx = inside ? j * nx + i : 0;
      float h, fx = 0, fy = 0;
      if (inside && z < bilinear) {
        fx = fi - i; fy = fj - j;
        h = hbase + hscale * ((hgt[idx] * (1 - fx) + hgt[idx + 1] * fx) * (1 - fy) +
                              (hgt[idx + nx] * (1 - fx) + hgt[idx + nx + 1] * fx) * fy);
      } else if (inside) {
        h = hbase + hscale * hgt[idx];
      } else {
        // beyond the hole: woods rising towards the skyline
        float ox = gx0 - x; if (x - gx1 > ox) ox = x - gx1; if (ox < 0) ox = 0;
        float oy = gy0 - y; if (y - gy1 > oy) oy = y - gy1; if (oy < 0) oy = 0;
        const float u = pxt_clamp((ox > oy ? ox : oy) / 60.0f, 0, 1);
        const unsigned hx = (unsigned)pxt_floori(x / 7), hy = (unsigned)pxt_floori(y / 7);
        h = farh + 10 * u + 1.5f * (pxt_hmod(hx * 5u + hy * 3u, 7) / 6.0f);
      }
      const float depth = z * cr;
      const float syf = cam.hor + (cam.z - h) * cam.f / depth;
      if (syf < yb) {
        float r, g, b;
        int water = 0;
        if (inside) {
          int k = kind[idx];
          if (k >= nkinds) k = 0;
          const unsigned char *kd = &kinds[k * 6];
          const int pat = kd[3];
          if (pat == 4) {
            water = 1;
            r = kd[0]; g = kd[1]; b = kd[2];
          } else {
            // the sun on the slope, from the four neighbours' heights
            const int il = i > 0 ? idx - 1 : idx, ir = idx + 1;
            const int ju = j > 0 ? idx - nx : idx, jd = idx + nx;
            const float sx = (hgt[il] - hgt[ir]) * hscale / (2 * cell);
            const float sy = (hgt[ju] - hgt[jd]) * hscale / (2 * cell);
            const float d = (sx * sun[0] + sy * sun[1] + sun[2]) / sqrtf(sx * sx + sy * sy + 1);
            float l = 0.62f + 0.55f * (d > 0 ? d : 0);
            const float amt = kd[5] / 100.0f, per = kd[4] > 0 ? kd[4] : 1;
            if (pat == 1) l *= (pxt_hmod((unsigned)pxt_floori(x / per), 2) == 0) ? 1 + amt : 1 - amt * 0.86f;
            else if (pat == 2) l *= (pxt_hmod((unsigned)pxt_floori(x / per) + (unsigned)pxt_floori(y / per), 2) == 0) ? 1 + amt : 1 - amt * 0.8f;
            else if (pat == 3) l *= (1 - amt) + 2 * amt * (pxt_hmod((unsigned)i * 7u + (unsigned)j * 13u, 11) / 10.0f);
            if (light) l *= light[idx] / 128.0f;
            if (z < grass) l *= 0.93f + 0.14f * (pxt_hmod((unsigned)pxt_floori(x * 3) * 73u + (unsigned)pxt_floori(y * 3) * 151u, 17) / 16.0f);
            r = kd[0] * l; g = kd[1] * l; b = kd[2] * l;
          }
        } else {
          const unsigned hx = (unsigned)pxt_floori(x / 7), hy = (unsigned)pxt_floori(y / 7);
          const float l = 0.85f + 0.25f * (pxt_hmod(hx * 7u + hy * 13u, 5) / 4.0f);
          r = far[0] * l; g = far[1] * l; b = far[2] * l;
        }
        if (water) {                       // the sky in the water, more of it far away
          const float t = pxt_clamp(depth / 300, 0.1f, 0.5f);
          r = deep[0] + (sky[0] - deep[0]) * t;
          g = deep[1] + (sky[1] - deep[1]) * t;
          b = deep[2] + (sky[2] - deep[2]) * t;
          if (pxt_hmod((unsigned)i * 7u + (unsigned)j * 13u + (unsigned)frame, 37) == 0) { r = 200; g = 225; b = 250; }
        }
        float t = pxt_clamp((depth - fog0) / fogr, 0, 1);
        t = t * t;
        r += (haze[0] - r) * t; g += (haze[1] - g) * t; b += (haze[2] - b) * t;
        const int R = (int)pxt_clamp(r, 0, 255), Gc = (int)pxt_clamp(g, 0, 255), B = (int)pxt_clamp(b, 0, 255);
        int top = pxt_floori(syf);
        if (top < 0) top = 0;
        for (int yy = top; yy < yb; yy++) {
          for (int c = col; c < col + colw && c < fbw; c++) {
            unsigned char *p = &fb[(yy * fbw + c) * 3];
            p[0] = (unsigned char)R; p[1] = (unsigned char)Gc; p[2] = (unsigned char)B;
          }
        }
        yb = top;
        if (yb <= 0) break;
      }
      z = z * step + 0.04f;
    }
  }
  *work = samples;
  lua_settop(L, 1);
  return 0;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

#endif

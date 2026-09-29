// ============================================================
// px_field.h - Perlin noise, and a layer filled with a sum of waves
// ============================================================
// A plasma, clouds, a nebula, rings or rays over the whole screen, computed in
// C and laid into an 8-bit layer (px_layer.h) that px.show colours through a
// palette. In Lua the same is 8,192 evaluations a frame - over 100 ms on the
// panel; here it is one call.
//
//   px.noise(x, y [, z])        -> about -1..1: Ken Perlin's improved noise at
//                                  a point, smooth in all three; z is time
//   px.field(L, terms [, base]) -- L gets base (default 128) + 127 * the sum of
//                                  the terms at each pixel, clamped to 0..255.
//     A term is a table, its kind first:
//       {"sin",   fx, fy, phase, amp}          amp * sin(fx*x + fy*y + phase)
//       {"ring",  cx, cy, f, phase, amp}       amp * sin(f * distance + phase)
//       {"ray",   cx, cy, n, phase, amp}       amp * sin(n * angle + phase)
//       {"noise", scale, z, amp [, octaves]}   amp * fbm of noise(x*scale,
//                                              y*scale, z), octaves 1..6
//     Frequencies in radians a pixel, phases in radians, centres in pixels.
//     At most 8 terms.
//
// Integers only, like the other shared headers: noise in Q16 with its own
// permutation (a shuffle, not Perlin's table - any fixed one will do); sines
// from px_layer.h's table; distance by px_raster.h's integer square root;
// angle by an integer atan2 (octant and the approximation
// atan(t) ~ pi/4 t + 0.273 t (1 - t), within 0.004 rad). So the panel and
// luasim draw the same. px_raster.h and px_layer.h must be included first.
// ============================================================
#ifndef PX_FIELD_H
#define PX_FIELD_H

#include <stdint.h>

static const unsigned char pxn_perm[256] = {
  101,  76, 245, 191,  39,  78, 236, 121, 217, 250, 244,  12,  59,  96,  69, 205,
    145, 184, 130, 164,  55,  29,  88, 220,  56, 127, 166,  16, 105,  25, 196, 120,
    242, 115,  89, 185,  49,  90,   1,  71, 203, 235, 239, 241, 102, 224,  72,  45,
    131,  57, 186, 148,  13,  35, 137, 119, 179, 201,  98, 138, 171,  43,  63, 169,
    163,   6,  28,  91, 246, 152, 213, 216,  94, 197,  92, 252,  84,  40,  87,  82,
     97, 214,  22, 200, 225,  27,  21, 173,  52,   0, 212,  73,  20, 146,  19, 187,
    143, 227, 100, 103, 157, 168,  61, 237,  44,  81,   8,  54, 233, 255,  70, 150,
    190, 215, 161, 243, 156, 135, 226,   7, 194,  10,  99,  53, 230, 192,   9, 182,
    175,  83, 183, 165, 107,  66,  67, 122,  31, 247, 134,  68, 142, 154, 167,  26,
    253, 140, 204, 141, 155,  36, 109, 174,  24,   4,  38, 159, 136, 254, 104,  77,
    211, 181,  50,  58,  86,  64,  42,  15,  18, 207, 158,  30,  23,  46, 144,  34,
     41, 219,   3, 199, 222, 176, 125, 160, 221,  85,  62, 193, 133, 218, 106,   5,
    202, 147,  11, 251,  48, 112, 110,  51,  75, 189,  95, 234, 180, 177, 116, 232,
    139, 208, 231, 228, 240, 210, 249, 126,  80, 162,  65, 223,  32, 248, 124,  47,
     14, 198, 178, 206,  33, 172, 114, 238, 113, 117, 229, 108, 151, 149, 128,  79,
      2, 129,  17, 209, 123,  93, 153,  60, 111,  37, 118, 170,  74, 188, 132, 195
};
#define PXN_P(i) pxn_perm[(i) & 255]

// 6t^5 - 15t^4 + 10t^3, t in Q16
static int32_t pxn_fade(int32_t t) {
  const int64_t t2 = ((int64_t)t * t) >> 16, t3 = (t2 * t) >> 16;
  const int64_t inner = (((int64_t)t * (6 * (int64_t)t - 15 * 65536)) >> 16) + 10 * 65536;
  return (int32_t)((t3 * inner) >> 16);
}
static int32_t pxn_lerp(int32_t t, int32_t a, int32_t b) { return a + (int32_t)(((int64_t)t * (b - a)) >> 16); }
static int32_t pxn_grad(int hash, int32_t x, int32_t y, int32_t z) {
  const int h = hash & 15;
  const int32_t u = h < 8 ? x : y;
  const int32_t v = h < 4 ? y : (h == 12 || h == 14) ? x : z;
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// Improved noise at (x, y, z), Q16 in and out; about -65536..65536.
static int32_t pxn_noise3(int32_t x, int32_t y, int32_t z) {
  const int X = (x >> 16) & 255, Y = (y >> 16) & 255, Z = (z >> 16) & 255;
  const int32_t fx = x & 0xFFFF, fy = y & 0xFFFF, fz = z & 0xFFFF;
  const int32_t u = pxn_fade(fx), v = pxn_fade(fy), w = pxn_fade(fz);
  const int A = PXN_P(X) + Y, AA = PXN_P(A) + Z, AB = PXN_P(A + 1) + Z;
  const int B = PXN_P(X + 1) + Y, BA = PXN_P(B) + Z, BB = PXN_P(B + 1) + Z;
  const int32_t o = 65536;
  return pxn_lerp(w,
    pxn_lerp(v, pxn_lerp(u, pxn_grad(PXN_P(AA), fx, fy, fz),      pxn_grad(PXN_P(BA), fx - o, fy, fz)),
                pxn_lerp(u, pxn_grad(PXN_P(AB), fx, fy - o, fz),  pxn_grad(PXN_P(BB), fx - o, fy - o, fz))),
    pxn_lerp(v, pxn_lerp(u, pxn_grad(PXN_P(AA + 1), fx, fy, fz - o),     pxn_grad(PXN_P(BA + 1), fx - o, fy, fz - o)),
                pxn_lerp(u, pxn_grad(PXN_P(AB + 1), fx, fy - o, fz - o), pxn_grad(PXN_P(BB + 1), fx - o, fy - o, fz - o))));
}

// A Lua number to Q16 of units, clamped to +-32767 first (NaN to 0).
static int32_t pxn_q16(lua_Number v) {
  if (!(v == v)) return 0;
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return (int32_t)(v * 65536);
}

// px.noise(x, y [, z])
static int px_noise_lua(lua_State *L) {
  const int32_t n = pxn_noise3(pxn_q16(luaL_checknumber(L, 1)), pxn_q16(luaL_checknumber(L, 2)),
                               pxn_q16(luaL_optnumber(L, 3, 0)));
  lua_pushnumber(L, (lua_Number)n / 65536);
  return 1;
}

// atan2 in turns, Q16 (0..65535), y downwards as on the panel
static int32_t pxn_atan2(int32_t dy, int32_t dx) {
  const int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  if (ax == 0 && ay == 0) return 0;
  const int swap = ay > ax;
  const int64_t t = swap ? ((int64_t)ax << 16) / ay : ((int64_t)ay << 16) / ax;   // Q16, 0..1
  // atan(t) in turns: t/8 + 0.04345 t (1 - t); 0.04345 * 65536 = 2848
  int32_t a = (int32_t)(t / 8 + ((2848 * ((t * (65536 - t)) >> 16)) >> 16));
  if (swap) a = 16384 - a;            // atan(1/t) = pi/2 - atan(t)
  if (dx < 0) a = 32768 - a;
  if (dy < 0) a = 65536 - a;
  return a & 0xFFFF;
}

// radians -> Q16 turns (65536 / 2pi), clamped first
static int32_t pxn_turns(lua_Number r) {
  if (!(r == r)) return 0;
  if (r > 1000) r = 1000;
  if (r < -1000) r = -1000;
  return (int32_t)(r * (lua_Number)10430.378);
}

typedef struct {
  int kind;                 // 0 sin, 1 ring, 2 ray, 3 noise
  int32_t a, b, c, d;       // per kind, see px_field_lua
  int32_t amp;              // Q8, amp 1 = 256
  int oct;
} PxfTerm;

static int pxf_term_no;   // the term being read, for the error message
static lua_Number pxf_tnum(lua_State *L, int t, int i, lua_Number def) {
  lua_rawgeti(L, t, i);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) { v = lua_tonumber(L, -1); if (!(v == v)) v = def; }
  else if (!lua_isnil(L, -1)) { lua_pop(L, 1); return luaL_error(L, "px.field: term %d wants numbers after its kind", pxf_term_no); }
  lua_pop(L, 1);
  return v;
}
static int32_t pxf_amp(lua_Number a) {
  if (!(a == a)) return 0;
  if (a > 64) a = 64;
  if (a < -64) a = -64;
  return (int32_t)(a * 256);
}

// Scratch for the noise: its sum at every pixel (int32) and one octave's grid.
static char pxn_key;
static int32_t *pxn_scratch(lua_State *L, size_t bytes) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &pxn_key);
  int32_t *b = (int32_t *)lua_touserdata(L, -1);
  const size_t have = b ? lua_rawlen(L, -1) : 0;
  lua_pop(L, 1);
  if (b && have >= bytes) return b;
  b = (int32_t *)lua_newuserdatauv(L, bytes, 0);   // raises on no memory, as any allocation
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxn_key);
  return b;
}

// The grid step of a noise octave: about three samples a noise unit, 1 to 8
// pixels. Noise is smooth, so between the samples it is interpolated
// (bilinear, Q8): a whole-screen octave at scale 0.035 is 153 samples, not
// 8,192. Measured on the panel before this (2026-09-29): 27 ms an octave
// sampled at every pixel.
static int pxn_step(int32_t sc) {
  const int32_t g = sc > 0 ? 21845 / sc : 8;
  return g < 1 ? 1 : g > 8 ? 8 : g;
}

// What a call costs, in Lua instructions, by the panel's measurements
// (2026-09-29, 410 ns an instruction): a noise sample 8, a pixel of a sin
// term 1.1, of a ring or a ray 3.3 (11 ms a whole-screen term), an octave's
// interpolation 0.9 a pixel (3 octaves at scale 0.035: 20 ms).
// charge (may be NULL) is told before the work, so a call that would run past
// the frame's budget or deadline is refused before it starts.
typedef void (*PxfCharge)(lua_State *L, unsigned long instructions);

// px.field(L, terms [, base]). *work: the instructions charged.
static const char *const pxf_kinds[] = {"sin", "ring", "ray", "noise", NULL};
static int px_field_lua(lua_State *L, int w, int h, unsigned long *work, PxfCharge charge) {
  PxLayer *l = pxl_check(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  const lua_Number basen = luaL_optnumber(L, 3, 128);
  const int base = (int)(!(basen == basen) ? 128 : basen < -1024 ? -1024 : basen > 1024 ? 1024 : basen);
  *work = 0;
  if (l->w != w || l->h != h) return luaL_error(L, "px.field: the layer is not the canvas's size");
  if (w > 256) return luaL_error(L, "px.field: a canvas over 256 wide");   // colG holds x/g in a byte
  const int n = (int)lua_rawlen(L, 2);
  if (n > 8) return luaL_error(L, "px.field: at most 8 terms");
  PxfTerm T[8];
  unsigned long weight = 0;
  for (int i = 0; i < n; i++) {
    pxf_term_no = i + 1;
    lua_rawgeti(L, 2, i + 1);
    const int t = lua_gettop(L);
    if (!lua_istable(L, t)) return luaL_error(L, "px.field: term %d is not a table", i + 1);
    lua_rawgeti(L, t, 1);
    const char *k = lua_tostring(L, -1);
    int kind = -1;
    for (int j = 0; k && pxf_kinds[j]; j++) if (strcmp(k, pxf_kinds[j]) == 0) kind = j;
    lua_pop(L, 1);
    if (kind < 0) return luaL_error(L, "px.field: term %d's kind is sin, ring, ray or noise", i + 1);
    PxfTerm *q = &T[i];
    q->kind = kind; q->oct = 1;
    if (kind == 0) {          // sin: a, b = turns a pixel in x and y; c = phase
      q->a = pxn_turns(pxf_tnum(L, t, 2, 0)); q->b = pxn_turns(pxf_tnum(L, t, 3, 0));
      q->c = pxn_turns(pxf_tnum(L, t, 4, 0)); q->amp = pxf_amp(pxf_tnum(L, t, 5, 1));
      weight += 1;
    } else if (kind <= 2) {   // ring, ray: a, b = centre in 1/16 px; c = f or n; d = phase
      lua_Number cx = pxf_tnum(L, t, 2, (lua_Number)(w - 1) / 2), cy = pxf_tnum(L, t, 3, (lua_Number)(h - 1) / 2);
      cx = cx < -1024 ? -1024 : cx > 1024 ? 1024 : cx;
      cy = cy < -1024 ? -1024 : cy > 1024 ? 1024 : cy;
      q->a = (int32_t)(cx * 16); q->b = (int32_t)(cy * 16);
      if (kind == 1) q->c = pxn_turns(pxf_tnum(L, t, 4, 0.3));              // turns a pixel
      else {
        lua_Number nr = pxf_tnum(L, t, 4, 4);
        nr = nr < -64 ? -64 : nr > 64 ? 64 : nr;
        q->c = (int32_t)(nr * 256);                                            // rays, Q8
      }
      q->d = pxn_turns(pxf_tnum(L, t, 5, 0)); q->amp = pxf_amp(pxf_tnum(L, t, 6, 1));
      weight += kind == 1 ? 2 : 3;
    } else {                  // noise: a = scale Q16 (units a pixel); b = z Q16
      lua_Number sc = pxf_tnum(L, t, 2, 0.05);
      sc = sc < 0 ? -sc : sc;
      sc = sc > 16 ? 16 : sc;
      q->a = (int32_t)(sc * 65536); q->b = pxn_q16(pxf_tnum(L, t, 3, 0));
      q->amp = pxf_amp(pxf_tnum(L, t, 4, 1));
      lua_Number o = pxf_tnum(L, t, 5, 1);
      q->oct = o < 1 ? 1 : o > 6 ? 6 : (int)o;
      weight += 4 * (unsigned long)q->oct;
    }
    lua_pop(L, 1);
  }
  // The cost, then the charge, before any of the work.
  unsigned long cost = (unsigned long)w * h / 2;           // the loop and the store
  int noiseTerms = 0;
  for (int i = 0; i < n; i++) {
    if (T[i].kind == 0) cost += (unsigned long)w * h * 11 / 10;
    else if (T[i].kind <= 2) cost += (unsigned long)w * h * 33 / 10;
    else {
      noiseTerms++;
      int32_t sc = T[i].a;
      for (int o = 0; o < T[i].oct; o++, sc <<= 1) {
        const int g = pxn_step(sc);
        cost += (unsigned long)((w - 1) / g + 2) * (unsigned long)((h - 1) / g + 2) * 8;
        cost += (unsigned long)w * h * 9 / 10;           // its interpolation over every pixel
      }
    }
  }
  if (charge) charge(L, cost);
  *work = cost;

  // Every noise term, all its octaves, summed at every pixel first (Q15 * Q8,
  // the terms' amplitudes applied), each octave on its own grid.
  int32_t *nsum = 0;
  if (noiseTerms) {
    const int gwMax = (w - 1) + 2, ghMax = (h - 1) + 2;
    // the sum, one octave's grid, and two column tables (bytes), all in the heap:
    // the effect task's stack has no room to spare
    nsum = pxn_scratch(L, ((size_t)w * h + (size_t)gwMax * ghMax) * sizeof(int32_t) + 2 * (size_t)w);
    int32_t *grid = nsum + w * h;
    unsigned char *colG = (unsigned char *)(grid + gwMax * ghMax), *colF = colG + w;
    memset(nsum, 0, (size_t)w * h * sizeof(int32_t));
    for (int i = 0; i < n; i++) {
      const PxfTerm *q = &T[i];
      if (q->kind != 3) continue;
      int32_t amp = 32768, sc = q->a;
      uint32_t z = (uint32_t)q->b;
      for (int o = 0; o < q->oct; o++) {
        const int g = pxn_step(sc);
        const int gw = (w - 1) / g + 2, gh = (h - 1) / g + 2;
        const int32_t zz = (int32_t)(z & 0x7FFFFFFF);
        for (int gy = 0; gy < gh; gy++)
          for (int gx = 0; gx < gw; gx++) {
            // noise repeats every 256 units, so a coordinate is kept to 31 bits
            const int32_t nx = (int32_t)(((int64_t)gx * g * sc) & 0x7FFFFFFF);
            const int32_t ny = (int32_t)(((int64_t)gy * g * sc) & 0x7FFFFFFF);
            grid[gy * gw + gx] = (int32_t)(((int64_t)pxn_noise3(nx, ny, zz) * amp) >> 16);   // Q15
          }
        // where each column falls on the grid, once an octave, not a division a pixel
        for (int x = 0; x < w; x++) { colG[x] = (unsigned char)(x / g); colF[x] = (unsigned char)(((x % g) << 8) / g); }
        for (int y = 0; y < h; y++) {
          const int gy = y / g, fy = ((y - gy * g) << 8) / g;
          const int32_t *r0 = &grid[gy * gw], *r1 = r0 + gw;
          int32_t *acc = &nsum[y * w];
          for (int x = 0; x < w; x++) {
            const int gx = colG[x], fx = colF[x];
            const int32_t top = r0[gx] + (((r0[gx + 1] - r0[gx]) * fx) >> 8);
            const int32_t bot = r1[gx] + (((r1[gx + 1] - r1[gx]) * fx) >> 8);
            const int32_t v = top + (((bot - top) * fy) >> 8);
            acc[x] += v * q->amp >> 8;
          }
        }
        amp >>= 1; sc <<= 1; z += 0x9E3779u;                                   // each octave its own slice
      }
    }
  }

  for (int y = 0; y < h; y++) {
    unsigned char *out = &l->v[y * w];
    for (int x = 0; x < w; x++) {
      int32_t sum = nsum ? nsum[y * w + x] : 0;            // Q15 * Q8
      for (int i = 0; i < n; i++) {
        const PxfTerm *q = &T[i];
        int32_t s;                                         // Q15
        if (q->kind == 0) {
          // in unsigned 32 bits: the phase wraps by the turn, which is the point
          const uint32_t ph = (uint32_t)q->a * (uint32_t)x + (uint32_t)q->b * (uint32_t)y + (uint32_t)q->c - 16384u;   // sin = cos(phase - 1/4 turn)
          s = pxl_cosq(ph & 0xFFFF);
        } else if (q->kind == 1) {
          const int32_t dx = x * 16 - q->a, dy = y * 16 - q->b;                 // 1/16 px, within +-(1024+128)*16
          const int32_t d = (int32_t)pxr_isqrt((unsigned)(dx * dx) + (unsigned)(dy * dy));   // 1/16 px
          const uint32_t ph = (uint32_t)(((int64_t)q->c * d) >> 4) + (uint32_t)q->d - 16384u;
          s = pxl_cosq(ph & 0xFFFF);
        } else if (q->kind == 2) {
          const int32_t ang = pxn_atan2(y * 16 - q->b, x * 16 - q->a);        // Q16 turns
          const uint32_t ph = (uint32_t)(((int64_t)q->c * ang) >> 8) + (uint32_t)q->d - 16384u;
          s = pxl_cosq(ph & 0xFFFF);
        } else {
          continue;                                         // noise: summed above
        }
        sum += s * q->amp >> 8;
      }
      int v = base + (int)(((int64_t)sum * 127) >> 15);
      out[x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
  (void)weight;
  return 0;
}

#endif

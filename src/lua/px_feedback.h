// ============================================================
// px_feedback.h - the last frame, turned, zoomed and faded, under the new one
// ============================================================
// The trick behind MilkDrop and every "infinite tunnel": each frame the
// picture already on the canvas is resampled a little zoomed, turned and
// shifted, and dimmed, and the new drawing goes on top. Whatever is drawn is
// carried away - into a whirlpool, out of a tunnel, round a spiral - and one
// dot becomes a pattern that never repeats.
//
//   px.feedback{ zoom = 1.03, rot = 0.02, dx = 0, dy = 0, cx = 63.5, cy = 31.5,
//                decay = 0.05, edge = "black" }
//     zoom   > 1 pulls the picture toward you (it grows), < 1 sends it away
//            (clamped to 0.25 .. 20)
//     rot    radians a call, turning about (cx, cy); positive turns clockwise
//            on the panel (y grows downwards)
//     dx, dy pixels the picture moves
//     cx, cy the centre of the zoom and the turn (default the canvas's centre)
//     decay  0..1, how much darker it gets each call
//     edge   what comes in from outside: "black" (default), "clamp" (the
//            edge pixels stretched) or "wrap" (the other side: tiles)
//   Every field is optional; px.feedback{} leaves the picture as it is.
//
// Integers only, as px_raster.h and px_layer.h: the turn from px_layer.h's
// cosine table, the zoom by integer division, the sample bilinear in Q8. The
// last frame is copied into a 24 KB scratch userdata kept in the registry.
// Shared by the firmware (src/lua/lua_px.cpp) and luasim; px_raster.h and
// px_layer.h must be included first.
// ============================================================
#ifndef PX_FEEDBACK_H
#define PX_FEEDBACK_H

#include <string.h>
#include <stdint.h>

static char pxf_key;   // the registry key of the copy (include in one file only)

static unsigned char *pxf_copy(lua_State *L, size_t bytes) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &pxf_key);
  unsigned char *b = (unsigned char *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (b) return b;
  b = (unsigned char *)lua_newuserdatauv(L, bytes, 0);   // raises on no memory, as any allocation
  lua_rawsetp(L, LUA_REGISTRYINDEX, &pxf_key);
  return b;
}

// A field of the argument table, read raw (no metamethods), clamped; NaN and
// anything not a number give the default.
static lua_Number pxf_num(lua_State *L, const char *k, lua_Number def, lua_Number lo, lua_Number hi) {
  lua_pushstring(L, k);
  lua_rawget(L, 1);
  lua_Number v = def;
  if (lua_type(L, -1) == LUA_TNUMBER) {
    v = lua_tonumber(L, -1);
    if (!(v == v)) v = def;
  } else if (!lua_isnil(L, -1)) {
    return luaL_error(L, "px.feedback: %s wants a number", k);
  }
  lua_pop(L, 1);
  return v < lo ? lo : v > hi ? hi : v;
}

// px.feedback{...}. *work: the pixels resampled.
static const char *const pxf_edges[] = {"black", "clamp", "wrap", NULL};
static int px_feedback_lua(lua_State *L, unsigned char *fb, int w, int h, unsigned long *work) {
  luaL_checktype(L, 1, LUA_TTABLE);
  *work = 0;
  // zoom no smaller than 1/4: then a source coordinate stays within +-9000 px,
  // which keeps the per-pixel arithmetic in 32 bits (see below)
  const lua_Number zoom = pxf_num(L, "zoom", 1, (lua_Number)0.25, 20);
  const lua_Number rot = pxf_num(L, "rot", 0, -1000, 1000);
  const lua_Number dx = pxf_num(L, "dx", 0, -1024, 1024), dy = pxf_num(L, "dy", 0, -1024, 1024);
  const lua_Number cx = pxf_num(L, "cx", (lua_Number)(w - 1) / 2, -1024, 1024);
  const lua_Number cy = pxf_num(L, "cy", (lua_Number)(h - 1) / 2, -1024, 1024);
  const int decay = pxr_q8(pxf_num(L, "decay", 0, 0, 1));
  lua_pushliteral(L, "edge");
  lua_rawget(L, 1);
  int edge = 0;
  if (!lua_isnil(L, -1)) {
    const char *e = lua_tostring(L, -1);
    if (!e) return luaL_error(L, "px.feedback: edge is \"black\", \"clamp\" or \"wrap\"");
    for (edge = 0; pxf_edges[edge] && strcmp(pxf_edges[edge], e) != 0; edge++) {}
    if (!pxf_edges[edge]) return luaL_error(L, "px.feedback: edge is \"black\", \"clamp\" or \"wrap\"");
  }
  lua_pop(L, 1);
  if (edge == 2 && ((w & (w - 1)) || (h & (h - 1)))) edge = 1;   // wrap needs sizes of powers of two

  // The inverse map in Q16: a destination pixel (x, y) reads the last frame at
  // c + R(-rot) * (p - c - d) / zoom. The scaling by 65536 is exact; each value
  // is converted once.
  const long long Z = (long long)(zoom * 65536);                     // zoom, Q16
  const long long iz = (65536LL * 65536LL) / (Z > 0 ? Z : 1);         // 1/zoom, Q16
  const unsigned phase = (unsigned)((long long)(rot * (lua_Number)10430.378) & 0xFFFF);  // 65536 / 2pi, a turn in Q16
  const long long cs = (long long)pxl_cosq(phase), sn = (long long)pxl_cosq((phase + 49152u) & 0xFFFF);  // cos, sin: Q15
  const long long A = (cs * iz) >> 15, B = (sn * iz) >> 15;           // Q16
  const long long CX = (long long)(cx * 65536), CY = (long long)(cy * 65536);
  const long long DX = (long long)(dx * 65536), DY = (long long)(dy * 65536);

  unsigned char *src = pxf_copy(L, (size_t)w * h * 3);
  memcpy(src, fb, (size_t)w * h * 3);
  const int ia = 256 - decay;
  // Per pixel in 32 bits: the panel's core is 32-bit, and 64-bit arithmetic a
  // pixel cost 18.8 ms a call (2026-09-29). |A|, |B| <= 4 * 65536 (zoom >= 1/4),
  // and a source coordinate stays within +-(1024 + 1024 + 128 * 4) px, Q16 in 31 bits.
  const int32_t a32 = (int32_t)A, b32 = (int32_t)B;
  const int wmask = w - 1, hmask = h - 1, row = w * 3;
  for (int y = 0; y < h; y++) {
    const long long v = ((long long)y << 16) - CY - DY;
    const long long u = -CX - DX;                                      // x = 0
    // R(-rot) * (u, v) / zoom, with y downwards: [cos sin; -sin cos]
    int32_t sx = (int32_t)(CX + ((A * u + B * v) >> 16));
    int32_t sy = (int32_t)(CY + ((A * v - B * u) >> 16));
    unsigned char *p = &fb[y * row];
    for (int x = 0; x < w; x++, p += 3, sx += a32, sy -= b32) {
      const int ix = sx >> 16, iy = sy >> 16;
      const int fx = (sx >> 8) & 255, fy = (sy >> 8) & 255;
      const int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
      int c0, c1, c2;
      if (ix >= 0 && iy >= 0 && ix < wmask && iy < hmask) {
        // all four inside: no checks
        const unsigned char *q = &src[iy * row + ix * 3];
        const unsigned char *r = q + row;
        c0 = q[0] * w00 + q[3] * w10 + r[0] * w01 + r[3] * w11;
        c1 = q[1] * w00 + q[4] * w10 + r[1] * w01 + r[4] * w11;
        c2 = q[2] * w00 + q[5] * w10 + r[2] * w01 + r[5] * w11;
      } else {
        c0 = c1 = c2 = 0;
        for (int k = 0; k < 4; k++) {
          int qx = ix + (k & 1), qy = iy + (k >> 1);
          if (edge == 2) { qx &= wmask; qy &= hmask; }
          else if (edge == 1) {
            qx = qx < 0 ? 0 : qx >= w ? w - 1 : qx;
            qy = qy < 0 ? 0 : qy >= h ? h - 1 : qy;
          } else if (qx < 0 || qx >= w || qy < 0 || qy >= h) continue;
          const int wgt = k == 0 ? w00 : k == 1 ? w10 : k == 2 ? w01 : w11;   // Q16
          const unsigned char *q = &src[qy * row + qx * 3];
          c0 += q[0] * wgt; c1 += q[1] * wgt; c2 += q[2] * wgt;
        }
      }
      p[0] = (unsigned char)(((c0 >> 16) * ia) >> 8);
      p[1] = (unsigned char)(((c1 >> 16) * ia) >> 8);
      p[2] = (unsigned char)(((c2 >> 16) * ia) >> 8);
    }
  }
  *work = (unsigned long)w * h;
  return 0;
}

#endif

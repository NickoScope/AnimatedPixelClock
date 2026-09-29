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
  const lua_Number zoom = pxf_num(L, "zoom", 1, (lua_Number)0.05, 20);
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
  for (int y = 0; y < h; y++) {
    const long long v = ((long long)y << 16) - CY - DY;
    long long u = -CX - DX;                                            // x = 0
    // R(-rot) * (u, v) / zoom, with y downwards: [cos sin; -sin cos]
    long long sx = CX + ((A * u + B * v) >> 16);
    long long sy = CY + ((A * v - B * u) >> 16);
    unsigned char *p = &fb[y * w * 3];
    for (int x = 0; x < w; x++, p += 3, sx += A, sy -= B) {
      // the four neighbours and the fractions, Q8
      long long ix = sx >> 16, iy = sy >> 16;
      const int fx = (int)((sx >> 8) & 255), fy = (int)((sy >> 8) & 255);
      int c[3] = {0, 0, 0};
      for (int k = 0; k < 4; k++) {
        long long qx = ix + (k & 1), qy = iy + (k >> 1);
        if (edge == 2) { qx &= (w - 1); qy &= (h - 1); }
        else if (edge == 1) {
          qx = qx < 0 ? 0 : qx >= w ? w - 1 : qx;
          qy = qy < 0 ? 0 : qy >= h ? h - 1 : qy;
        } else if (qx < 0 || qx >= w || qy < 0 || qy >= h) continue;
        const int wgt = ((k & 1) ? fx : 256 - fx) * ((k >> 1) ? fy : 256 - fy);   // Q16
        const unsigned char *q = &src[(qy * w + qx) * 3];
        c[0] += q[0] * wgt; c[1] += q[1] * wgt; c[2] += q[2] * wgt;
      }
      p[0] = (unsigned char)(((c[0] >> 16) * ia) >> 8);
      p[1] = (unsigned char)(((c[1] >> 16) * ia) >> 8);
      p[2] = (unsigned char)(((c[2] >> 16) * ia) >> 8);
    }
  }
  *work = (unsigned long)w * h;
  return 0;
}

#endif

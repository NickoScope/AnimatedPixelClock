// ============================================================
// lua_px.h - the px.* raster API, drawing into a 128x64 RGB888 canvas
// ============================================================
// The same calls, with the same semantics, as tools/luasim/luasim.c: that
// file is the reference, and tools/luasim/fx_parity.py compares the two
// pixel for pixel. The canvas is a plain byte array rather than the panel,
// because the DMA panel has no readable framebuffer and px.get, px.blend and
// px.glow all read before they write.
//
// Host-portable: compiled into the firmware and into tools/luasim/fxhost.
// ============================================================
#ifndef LUA_PX_API_H
#define LUA_PX_API_H

#include <stdint.h>

struct lua_State;

#define LUA_PX_W 128
#define LUA_PX_H 64
#define LUA_PX_BYTES (LUA_PX_W * LUA_PX_H * 3)

// What px.t() and px.now() return during one draw(). Filled in by the host
// before each call; the bindings only read it.
struct LuaPxClock {
  double  phase;       // px.t(), [0,1)
  int     hour, min, sec;
  int     yday;        // 0-based day of the year
  int     utcMinutes;  // local time minus UTC; px.now().utc is in hours
  int     year;        // the year yday counts in, e.g. 2026
};

struct LuaPxCanvas {
  uint8_t   *rgb;      // LUA_PX_BYTES, row-major, R G B
  LuaPxClock clock;
  // px.button(): clicks of the knob or the remote's OK on an effect page since
  // the panel started. A script compares it with the last value it saw; the
  // count itself means nothing. Filled in by the host before each call.
  uint32_t   clicks;
};

// Set the global table `px` in L, bound to canvas. The canvas must outlive L.
void luaPxOpen(lua_State *L, LuaPxCanvas *canvas);

// Copy a canvas onto anything with drawPixelRGB888(x, y, r, g, b): the panel
// in the firmware, a byte array on the host. Every pixel is written, so the
// caller does not need to clear first.
template <class Display>
inline void luaPxBlit(const uint8_t *rgb, Display &d) {
  const uint8_t *p = rgb;
  for (int16_t y = 0; y < LUA_PX_H; y++)
    for (int16_t x = 0; x < LUA_PX_W; x++, p += 3)
      d.drawPixelRGB888(x, y, p[0], p[1], p[2]);
}

#endif  // LUA_PX_H

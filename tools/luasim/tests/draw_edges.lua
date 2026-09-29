-- px_draw.h at its edges, for fx_parity: anti-aliased lines far off the
-- canvas, zero-length, steep and flat, NaN ends; dots at fractions and off
-- the canvas; triangles degenerate, huge and backwards; models with no faces,
-- no edges, out-of-range indices refused; meshes in every mode, turned hard,
-- close to the eye, NaN options; in both px.mode modes.
-- Not an effect: nothing here is meant to look like anything.
local nan, inf = 0 / 0, 1 / 0
local tet = px.model{ v = { 1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, 1 }, f = { 1, 2, 3, 1, 2, 4, 1, 3, 4, 2, 3, 4 }, orient = true,
                      e = { 1, 2, 1, 3, 1, 4, 2, 3, 2, 4, 3, 4 } }
local pts = px.model{ v = { 0, 0, 0, 60, 60, 60, -60, 0, 30 } }
local wire = px.model{ v = { -1, 0, 0, 1, 0, 0, 0, 1, 0 }, e = { 1, 2, 2, 3, 3, 1 } }
local f = 0
function draw()
  f = f + 1
  px.clear(5, 5, 10)
  local add = f % 2 == 0
  px.mode(add and "add" or "set")
  px.aline(-500, -300, 900, 400, 255, 200, 100)
  px.aline(10.3, 10.7, 10.3, 10.7, 255, 255, 255)
  px.aline(20.25, 5, 21.75, 60, 100, 255, 100, 0.7)
  px.aline(nan, 3, 50, inf, 255, 0, 0)
  px.aline(1e9, -1e9, -1e9, 1e9, 0, 0, 255)
  for i = 0, 20 do px.dot(i * 6.3 - 3, 40 + math.sin(f + i) * 30, 255, 255, 255, (i % 5) / 4) end
  px.dot(nan, 5, 255, 0, 0); px.dot(-1e9, 1e9, 1, 2, 3)
  px.tri(0, 0, 10, 0, 20, 0, 255, 0, 0)
  px.tri(-1000, -1000, 2000, 0, 0, 2000, 30, 60, 90)
  px.tri(100, 10, 110, 30, 90, 30, 255, 255, 0)
  px.tri(90, 30, 110, 30, 100, 10, 0, 255, 255)
  local modes = { "wire", "solid", "both" }
  px.mesh(tet, { ax = f * 0.3, ay = f * 0.2, az = f * 1e3, scale = 18, x = 40, y = 32, mode = modes[f % 3 + 1], r = 200, g = 100, b = 255 })
  px.mesh(tet, { ax = nan, dist = 0.1, scale = 1e6, x = inf, mode = "solid" })
  px.mesh(pts, { scale = 30, mode = "both" })
  px.mesh(wire, { ay = f * 0.1, scale = 25, x = 100, y = 40, dist = 1.5, mode = "wire" })
end

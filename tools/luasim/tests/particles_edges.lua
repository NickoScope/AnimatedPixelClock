-- px_particles.h at its edges, for fx_parity: emits of 0 and past the
-- system's size, every edge mode, strong gravity and flow, NaN and huge
-- fields, palettes and colours, draw in both modes and sizes, clear.
-- Not an effect: nothing here is meant to look like anything.
local nan, inf = 0 / 0, 1 / 0
local P = px.particles(300, 7)
local Q = px.particles(1)
local pal = px.palette{ "cos", {0.5,0.5,0.5}, {0.5,0.5,0.5}, {1,1,1}, {0,0.33,0.67} }
local f = 0
function draw()
  f = f + 1
  px.fade(0.3)
  local c = f % 6
  if c == 0 then P:emit{ x = 64, y = 32, n = 80, speed = 60, speedj = 0.5, life = 1.5, lifej = 0.5, pal = pal, idx = f * 9, idxj = 90 } end
  if c == 1 then P:emit{ x = 0, y = 0, w = 128, h = 64, n = 500, speed = nan, angle = inf, spread = -1, life = 1e9, r = 400, g = -3, b = nan } end
  if c == 2 then P:emit{ n = 0 } end
  if c == 3 then P:emit{ x = 1e30, y = -1e30, n = 20, speed = 1e9 } end
  Q:emit{ x = 10, y = 10, n = 5, speed = 5 }
  local edges = { "kill", "wrap", "bounce" }
  P:step{ dt = (c == 4) and 1e9 or 1 / 15, gx = (c == 5) and 1e9 or 0, gy = 30, drag = (c == 2) and 50 or 0.3,
          flow = (c == 3) and 5000 or 60, flowscale = (c == 1) and 99 or 0.03, flowz = f * 0.1, edge = edges[f % 3 + 1] }
  Q:step()
  P:draw{ mode = (f % 2 == 0) and "add" or "set", bri = (c == 4) and nan or 1, fade = f % 3 ~= 0, size = (c == 0) and 2 or 1 }
  Q:draw()
  if f % 40 == 0 then P:clear() end
  px.text(1, 1, tostring(P:count()) .. " " .. tostring(Q:count()), 255, 255, 255)
end

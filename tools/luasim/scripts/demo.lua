-- demo.lua - exercises every px.* call, and is meant to look right at 128x64.
-- The contract: define draw(). The host calls it once per frame and supplies
-- the animation phase through px.t(), which is [0,1), never seconds.

local W, H = px.size()
local sin, cos, pi, floor = math.sin, math.cos, math.pi, math.floor

-- Starfield. Positions are fixed; only brightness breathes, because on a small
-- panel moving points read as noise and pulsing ones read as depth.
local stars = {}
for i = 1, 40 do
  stars[i] = { x = (i * 37) % W, y = (i * 53) % H, p = (i % 10) / 10 }
end

function draw()
  local t = px.t()
  px.clear(0, 0, 0)

  for _, s in ipairs(stars) do
    local b = floor(40 + 60 * (0.5 + 0.5 * sin(2 * pi * (t + s.p))))
    px.pixel(s.x, s.y, b, b, b + 20)
  end

  -- A horizon that swells, filled rather than outlined - the lesson the yacht
  -- radar taught: on a raster panel a wireframe reads as noise.
  local horizon = floor(H * 0.72 + 3 * sin(2 * pi * t))
  px.rect(0, horizon, W, H - horizon, 4, 22, 34, true)
  px.line(0, horizon, W - 1, horizon, 0, 120, 150)

  -- Sun, tracking the phase across the panel.
  local sx = floor(W * t)
  local sy = floor(horizon - 14 - 8 * sin(pi * t))
  px.circle(sx, sy, 6, 255, 150, 30, true)
  px.circle(sx, sy, 8, 90, 45, 10, false)

  -- Reflection: one dashed line per row, fading with depth.
  for y = horizon + 2, H - 1, 2 do
    local d = (y - horizon) / (H - horizon)
    local w = floor(2 + 6 * d)
    local b = floor(120 * (1 - d))
    px.rect(sx - w, y, w * 2, 1, b, floor(b * 0.6), 10, true)
  end

  -- Clock, right-aligned with the same trim rule the flight board uses.
  local n = px.now()
  local hhmm = string.format("%02d:%02d", n.hour, n.min)
  px.text(W - px.width(hhmm) - 2, 2, hhmm, 255, 255, 255)
  px.text(2, 2, "LUA ON THE PANEL", 255, 180, 0)
end

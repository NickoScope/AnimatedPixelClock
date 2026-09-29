-- px_layer.h at its edges, for fx_parity: palettes of both kinds, layers read
-- and written off the canvas, show in every mode with odd offsets, scroll with
-- and without wrap by small and huge steps, every mirror.
-- Not an effect: nothing here is meant to look like anything.
local W, H = px.size()
local sin, floor = math.sin, math.floor
local nan = 0 / 0

local grad = px.palette{ {0, 0, 0, 0}, {40, 255, 0, 0}, {40, 0, 255, 0}, {200, 20, 40, 255}, {255, 255, 255, 255} }
local one = px.palette{ {100, 10, 200, 30} }
local wild = px.palette{ {0, -50, 300, nan}, {255, 1e30, -1e30, 128}, {255, 1, 2, 3} }
local cosp = px.palette{ "cos", {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, {1, 1, 1}, {0, 0.33, 0.67} }
local cosw = px.palette{ "cos", {nan, 40, -40}, {2, 1e9, -3}, {17, -0.5, 3.3}, {-7, 0.25, nan} }
local pals = { grad, one, wild, cosp, cosw }

local A = px.layer()
local B = px.layer(300)
for y = 0, H - 1 do
  for x = 0, W - 1 do
    A:set(x, y, floor(128 + 60 * sin(x / 9) + 60 * sin(y / 7 + x / 23)))
  end
end
A:set(-1, 0, 5); A:set(W, 0, 5); A:set(0, H, 5); A:set(3, 3, -7); A:set(4, 4, 999)
local f = 0

function draw()
  f = f + 1
  local p = pals[f % #pals + 1]
  local r, g, b = px.pal(p, f * 7 - 300, (f % 5) / 4)
  px.clear(r // 4, g // 4, b // 4)
  px.show(A, p, f * 3 - 1000, 1)
  if f % 4 == 0 then px.show(A, cosp, -f, 0.5, "add") end
  if f % 4 == 1 then px.show(B, grad, f, 2, "max") end
  if f % 4 == 2 then B:fill(f % 3 == 0 and 0 or 77); px.show(B, one, 0, nan, "skip0") end
  px.circle(64 + floor(40 * sin(f / 9)), 32, 9, 255, 255, 255, true)
  px.capture(B)
  B:set(f % W, f % H, B:get(f % W, f % H) + 50)
  local c = f % 12
  if c == 0 then px.scroll(3, -2) elseif c == 1 then px.scroll(-5, 7, true)
  elseif c == 2 then px.scroll(W * 3 + 1, -H * 2 - 3, true) elseif c == 3 then px.scroll(0, 0)
  elseif c == 4 then px.scroll(-W, 0) elseif c == 5 then px.scroll(1, H - 1)
  elseif c == 6 then px.mirror("h") elseif c == 7 then px.mirror("v")
  elseif c == 8 then px.mirror("hv") elseif c == 9 then px.scroll(-1, -1, 1)
  end
  px.text(1, 1, tostring(A:get(3, 3)) .. " " .. tostring(A:get(4, 4)) .. " " .. tostring(A:get(-1, 0)), 255, 255, 0)
end

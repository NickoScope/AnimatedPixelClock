-- px_raster.h at its edges, for fx_parity: fade, blur, mode, glow and blend
-- with boxes off the canvas, alphas out of range, NaN, huge and tiny radii.
-- Not an effect: nothing here is meant to look like anything.
local W, H = px.size()
local sin, floor = math.sin, math.floor
local nan = 0 / 0
local f = 0

function draw()
  f = f + 1
  local t = px.t() * 6.2832
  -- paint something to work on
  if f % 30 == 1 then px.clear(20, 10, 40) end
  for i = 0, 11 do
    local x = floor(64 + 60 * sin(t * 3 + i))
    local y = floor(32 + 30 * sin(t * 2 + i * 1.7))
    px.circle(x, y, 2 + i % 5, 255, 120 + i * 10, 40 * (i % 4), i % 2 == 0)
  end
  -- add mode over everything put() draws
  local before = px.mode("add")
  px.line(0, f % H, W - 1, H - 1 - f % H, 90, 30, 200)
  px.rect(10 + f % 40, 5, 30, 20, 60, 60, 0, true)
  px.circle(64, 32, 20, 0, 80, 0)
  px.pixel(f % W, 10, 300, -5, 128)
  px.text(2, 50, "ADD", 100, 100, 100)
  px.mode(before)
  px.mode()
  -- glows at the edges of what glow accepts
  px.glow(64 + 40 * sin(t), 32, 10, 255, 200, 50)
  px.glow(-20, -20, 40, 50, 200, 255, 2.5)
  px.glow(130, 70, 300, 30, 30, 90, 0.3)
  px.glow(64, 32, 0.4, 255, 255, 255)
  px.glow(64, 32, 5000, 255, 255, 255)
  px.glow(nan, 32, 10, 255, 255, 255)
  px.glow(10.7, 20.3, 6.5, 400, -3, nan, 1)
  px.glow(100, 40, 8, 255, 0, 0, 0)
  -- blends with alphas and colours out of range
  for i = 0, 15 do
    px.blend(i * 8, 60, 255, 255, 255, i / 12 - 0.1)
    px.blend(i * 8 + 1, 61, 999, -50, nan, 0.5)
  end
  px.blend(-1, 0, 255, 255, 255, 1)
  px.blend(5, 5, 255, 255, 255, nan)
  -- fades: whole, toward a colour, boxes clipped, empty and backwards
  if f % 4 == 0 then px.fade(0.1) end
  if f % 4 == 1 then px.fade(0.25, 40, 20, 90, 0, 0, 64, 32) end
  if f % 4 == 2 then px.fade(0.5, 255, 255, 255, -30, 40, 50, 100) end
  if f % 4 == 3 then px.fade(2, 0, 0, 0, 200, 0, 10, 10) end
  px.fade(nan)
  px.fade(0.3, 0, 0, 0, 10, 10, 0, 5)
  px.fade(0.3, 0, 0, 0, 10, 10, -5, 5)
  -- blurs: whole, a box, the extremes
  if f % 3 == 0 then px.blur(0.4) end
  if f % 3 == 1 then px.blur(1, 96, 32, 64, 64) end
  if f % 3 == 2 then px.blur(0.05, -10, -10, 30, 30) end
  px.blur(-1)
  px.blur(nan)
end

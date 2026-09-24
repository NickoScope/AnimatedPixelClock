-- @upload-only
-- SPRITE TEST - px.grab and px.blit under every argument they clamp, for fx_parity.
--
-- Not a screen for the panel: fx_parity.py renders every script here through
-- luasim and fxhost and compares them pixel for pixel. Each frame draws a
-- figure, cuts it out, and stamps it across the canvas - straight, mirrored,
-- dimmed, brightened past white, tinted, half off every edge and wholly off
-- it - with a sprite built by string.char beside it. The refusals are not in
-- here: the sandbox has no pcall, and each of them - a string too short for a
-- sprite, a size that does not match its bytes, a coordinate that is not a
-- number - stops the frame with "bad argument".

PERIOD = 10
FPS = 15

local W, H = px.size()
local floor, sin = math.floor, math.sin

function draw()
  local t = px.t() * PERIOD
  local f = floor(t * FPS)

  -- The figure, on black: a body, a tail, an eye, with a black hole in it.
  px.clear(0, 0, 0)
  px.rect(4, 4, 9, 5, 250, 140, 20, true)
  px.line(3, 3, 1, 1 + f % 4, 255, 60, 40)
  px.line(3, 9, 1, 11 - f % 3, 255, 60, 40)
  px.pixel(11, 5, 240, 240, 250)
  px.pixel(8, 6, 0, 0, 0)
  local s, dx, dy = px.grab(0, 0, 16, 14)
  local none = px.grab(40, 40, 8, 8)               -- all black: nil
  local off = px.grab(-50, -50, 10, 10)            -- off the canvas: nil
  local edge = px.grab(-4, -4, 12, 12)             -- clipped at the corner

  -- The ground they are stamped on.
  for y = 0, H - 1, 4 do
    px.rect(0, y, W, 4, 20 + y, 40, 80 - y, true)
  end

  local x = floor(20 + 18 * sin(t * 0.9))
  px.blit(s, x, 4)
  px.blit(s, x, 20, true)
  px.blit(s, x + 20, 4, false, 0.4)
  px.blit(s, x + 20, 20, true, 3.5)                -- brightened past white
  px.blit(s, x + 40, 4, false, 1, 40, 120, 160, 0.5)
  px.blit(s, x + 40, 20, true, 0.8, 255, 255, 255, 2)  -- a held to 1
  px.blit(s, -6, 40)                               -- half off the left
  px.blit(s, W - 7, 40, true)                      -- half off the right
  px.blit(s, 50, -5)                               -- half off the top
  px.blit(s, 70, H - 6, false, 0 / 0)              -- NaN mul: black
  px.blit(s, 1e9, 1e9)                             -- far off: nothing
  px.blit(s, -1e9, 0 / 0)
  px.blit(s, 90.7, 40.2, false, 1, 0 / 0, 300, -5, 0.3)
  if edge then px.blit(edge, 100, 20) end

  -- Built by hand: 2 x 2, one transparent pixel.
  local hand = string.char(2, 2, 255, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255)
  px.blit(hand, 120, 2)
  px.blit(hand, 120, 6, true)

  local w = s and s:byte(1) or 0
  local h = s and s:byte(2) or 0
  px.text(2, H - 7, string.format("%d %d %d %d %s %s", w, h, dx or -1, dy or -1,
          tostring(none), tostring(off)), 200, 220, 240)
end

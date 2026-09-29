-- px_snapshot.h at its edges, for fx_parity: four slots, restore and mix of
-- an empty slot, mix at 0, 1, NaN and out of range, boxes off the canvas,
-- forget, and the old one-slot calls without arguments.
-- Not an effect: nothing here is meant to look like anything.
local W, H = px.size()
local sin, floor = math.sin, math.floor
local nan = 0 / 0
local f = 0

function draw()
  f = f + 1
  px.clear(10, 10, 30)
  for i = 0, 7 do
    px.circle(floor(64 + 50 * sin(f / 7 + i)), floor(32 + 25 * sin(f / 11 + i * 2)), 6, 255, 40 * i, 255 - 30 * i, true)
  end
  if f == 1 then px.save() end
  if f % 5 == 0 then px.save(2) end
  if f % 7 == 0 then px.save(3) end
  if f == 3 then px.save(4) end
  local r = px.restore(4)                      -- false until f == 3
  px.text(1, 1, r and "R" or "-", 255, 255, 0)
  px.mix(2, (f % 10) / 9)
  px.mix(3, nan)
  px.mix(3, 2, 10, 10, 50, 30)
  px.mix(1, 0.5, -20, -20, 60, 60)
  px.mix(1, 0.3, 100, 40, 100, 100)
  px.mix(1, 0.3, 10, 10, 0, 5)
  local m = px.mix(4, 0.25)
  px.text(10, 1, m and "M" or "-", 0, 255, 255)
  if f == 40 then px.forget(4) end
  if f == 60 then px.restore() end
end

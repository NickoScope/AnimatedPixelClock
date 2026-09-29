-- px_remap.h at its edges, for fx_parity: every kind of map with its
-- parameters at and past their limits (centres far off the canvas, a globe of
-- one pixel, a horizon above and below the screen, NaN and infinity); M:set
-- off the canvas and with values past 0..255; px.remap from an empty snapshot
-- slot, from a saved one, from a layer, with offsets huge and negative.
-- Not an effect: nothing here is meant to look like anything.
local nan, inf = 0 / 0, 1 / 0
local maps = {
  px.uvmap("tunnel"),
  px.uvmap("tunnel", { cx = -1e9, cy = 1e9, depth = 0 }),
  px.uvmap("tunnel", { cx = nan, depth = inf }),
  px.uvmap("polar", { scale = 64 }),
  px.uvmap("polar", { cx = 127, cy = 0, scale = -5 }),
  px.uvmap("sphere", { r = 1 }),
  px.uvmap("sphere", { cx = 0, cy = 63, r = 1e9 }),
  px.uvmap("plane"),
  px.uvmap("plane", { horizon = -100, height = 1e6, fov = 0 }),
  px.uvmap("plane", { horizon = 200, height = 0.001, fov = 1e9 }),
  px.uvmap("plane", { horizon = 63, fov = nan }),
  px.uvmap("swirl"),
  px.uvmap("swirl", { turn = -1e9, cx = 1024, cy = -1024 }),
  px.uvmap("swirl", { turn = 0.37 }),
  px.uvmap("blank"),
  px.uvmap(),
}
local M = px.uvmap("blank")
M:set(-1, 0, 1, 1); M:set(128, 5, 1, 1); M:set(3, 64, 1, 1); M:set(1e9, -1e9, 1, 1)
for x = 0, 127 do M:set(x, x % 64, x * 7 - 300, -x * 13, x * 3 - 100) end
local tex = px.layer()
px.field(tex, { { "sin", 0.3, 0.2, 0, 0.6 }, { "ring", 64, 32, 0.4, 0, 0.4 } })
local pal = px.palette{ { 0, 0, 0, 40 }, { 128, 255, 80, 0 }, { 255, 255, 255, 200 } }
local f = 0
local empty = px.remap(maps[1], 4, 0, 0)          -- slot 4 never saved: false, canvas untouched
function draw()
  f = f + 1
  px.clear(10, 5, 5)
  px.text(2, 2, empty and "FULL" or "EMPTY", 255, 255, 0)
  local m = maps[f % #maps + 1]
  if f % 3 == 0 then
    px.save(1)
    px.remap(m, 1, f * 99991, -f * 7)
  elseif f % 3 == 1 then
    px.remap(m, tex, math.mininteger + f, math.maxinteger - f, pal)
  else
    px.remap(M, tex, f, f, pal)
  end
end

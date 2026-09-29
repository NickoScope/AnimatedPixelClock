-- px_feedback.h at its edges, for fx_parity: every edge mode, zooms in and
-- out and at their clamps, turns both ways and huge, centres off the canvas,
-- NaN and missing fields, decay 0 and 1.
-- Not an effect: nothing here is meant to look like anything.
local sin, floor = math.sin, math.floor
local nan = 0 / 0
local f = 0
local CASES = {
  {}, { zoom = 1.08 }, { zoom = 0.9, edge = "clamp" }, { zoom = 1.02, rot = 0.1, edge = "wrap" },
  { rot = -0.3, decay = 0.1 }, { dx = 3.5, dy = -1.25, edge = "wrap" }, { cx = -50, cy = 200, zoom = 1.1 },
  { zoom = 100, rot = 1e6, decay = 2 }, { zoom = 0.001, dx = 1e9, edge = "clamp" },
  { zoom = nan, rot = nan, cx = nan, decay = nan }, { decay = 1 }, { zoom = 1.0001, rot = 6.2831853 },
}
function draw()
  f = f + 1
  if f % 12 == 1 then
    px.clear(0, 0, 0)
    for i = 0, 9 do px.circle(floor(64 + 40 * sin(i * 1.3)), floor(32 + 20 * sin(i * 2.1)), 4 + i % 3, 255, 25 * i, 200 - 20 * i, true) end
  end
  px.feedback(CASES[f % #CASES + 1])
  px.pixel(f % 128, 10, 255, 255, 255)
end

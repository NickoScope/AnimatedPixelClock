-- @upload-only
-- ============================================================
-- WARP - a tunnel, a planet, a neon road and a whirlpool
-- ============================================================
-- @name.en Warp
-- @name.ru Варп
-- @about.en Four scenes of 20 s by the clock, the same on every panel, flowing into each other: flying
-- @about.en down a plasma tunnel, a planet turning among stars, a neon road under a sunset, a whirlpool.
-- @about.ru Четыре сцены по 20 с по часам, одинаково на всех панелях, перетекающие друг в друга: полёт по
-- @about.ru плазменному туннелю, планета среди звёзд, неоновая дорога под закатом, водоворот.
-- @control.en knob press: Next scene now; it runs until the next change by the clock.
-- @control.ru knob press: Сразу следующая сцена; она идёт до ближайшей смены по часам.
-- @function.en 4 scenes, 20 s each, by the clock: the same on every panel
-- @function.en No clock
-- @function.en Needs firmware 2.7.7 or later
-- @function.ru 4 сцены по 20 с, по часам: одинаково на всех панелях
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.7 или новее
--
-- Four scenes, 20 s each, flowing into each other (px.mix); the button moves
-- to the next. Each is a flat picture seen through a map (px.uvmap) and slid
-- under it a frame at a time (px.remap) - the demoscene's oldest trick:
--
--   * tunnel: flying down a tube of shifting plasma, turning as it goes;
--   * planet: a globe of oceans and land turning among the stars;
--   * road: a neon grid rushing toward you under a sunset;
--   * whirlpool: plasma drawn round and down into a vortex.
--
-- The textures are 8-bit layers filled by px.field and coloured through
-- drifting palettes; the maps are computed once at load.
--
-- The scene is read off the wall clock, so two panels (or a panel and its
-- twin) show the same one at the same moment: px.t() is the phase of the
-- 80 s PERIOD aligned to the epoch, and its 20 s quarters are the scenes. A
-- press moves one scene on from the clock's (the presses are counted from
-- the moment the effect opened); the scene it brings runs until the clock's
-- next 20 s boundary, anything from 0 to 20 s. A change on the clock flows
-- in from the boundary itself, so every panel is at the same point of the
-- flow; a press flows in from the press.
--
-- Needs firmware 2.7.7 or later (px.uvmap, px.remap).
-- ============================================================
PERIOD = 80.0                 -- 4 scenes x 20 s: the scene is read off the clock
FPS = 15

local W, H = px.size()
local sin, floor, pi = math.sin, math.floor, math.pi
local HAS = rawget(px, "remap") ~= nil

local SCENES = { "tunnel", "planet", "road", "whirl" }
local cur, SCENE, XF = nil, 20, 1.2
local T, tprev = 0, nil
local xfAt = nil                      -- the clock's second (px.t() * PERIOD) the flow into this scene began
local LINES_SLOT, FADE_SLOT = 3, 2
-- px.button() counts from boot, not from here: the presses since the effect opened
local base = rawget(px, "button") and px.button() or 0
local seen = base

local MAPS, TEX, PAL = {}, {}, {}
if HAS then
  MAPS.tunnel = px.uvmap("tunnel", { depth = 20 })
  MAPS.planet = px.uvmap("sphere", { r = 27 })
  MAPS.road = px.uvmap("plane", { horizon = 30, height = 10, fov = 1.2 })
  MAPS.whirl = px.uvmap("swirl", { turn = 2.2 })
  TEX.tunnel = px.layer()
  TEX.planet = px.layer()
  px.field(TEX.planet, { { "noise", 0.05, 3.7, 1.3, 4 } })      -- the land, fixed
  TEX.road = px.layer()
  for y = 0, H - 1 do
    for x = 0, W - 1 do
      -- the grid: lines every 16 texels across and 8 along
      local on = (x % 16 == 0) or (y % 8 == 0)
      TEX.road:set(x, y, on and 255 or 20)
    end
  end
  TEX.whirl = px.layer()
  PAL.planet = px.palette{ { 0, 4, 18, 70 }, { 110, 20, 70, 150 }, { 126, 60, 140, 200 }, { 132, 210, 200, 130 },
                           { 150, 60, 150, 60 }, { 200, 30, 100, 40 }, { 235, 140, 120, 90 }, { 255, 250, 250, 250 } }
  PAL.road = px.palette{ { 0, 10, 0, 30 }, { 60, 30, 0, 60 }, { 200, 255, 40, 200 }, { 255, 120, 255, 255 } }
end
local A_, B_, C_, D_ = { 0.5, 0.5, 0.5 }, { 0.5, 0.5, 0.5 }, { 1, 1, 1 }, { 0, 0.33, 0.67 }
local SPEC = { "cos", A_, B_, C_, D_ }

-- stars for the planet, from a xorshift (the simulator and the panel agree)
local seed = 0x7A11B00B
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end
local SX, SY = {}, {}
for i = 1, 50 do SX[i], SY[i] = floor(rnd() * W), floor(rnd() * H) end

-- the tunnel's texture must tile (its u runs round the tube, its v on and on):
-- sines a whole number of times across the layer's 128 x 64 do
local K = 2 * pi
local TUN = { { "sin", K * 4 / W, K * 2 / H, 0, 0.5 }, { "sin", K * 6 / W, -K * 3 / H, 0, 0.4 },
              { "sin", K * 2 / W, K * 5 / H, 0, 0.3 } }
local WHI = { { "noise", 0.04, 0, 1.2, 3 }, { "ring", 63.5, 31.5, 0.25, 0, 0.35 } }

function draw()
  local t = px.t() * PERIOD
  local dt = 1 / FPS
  if tprev then
    dt = t - tprev
    if dt < 0 then dt = dt + PERIOD end
    if dt > 0.5 then dt = 0.5 end
  end
  tprev = t
  T = T + dt
  if T > 3600 then T = T - 3600 end   -- a 32-bit float T, held under an hour

  if not HAS then
    px.clear(0, 0, 0)
    px.text(2, 28, "NEEDS FIRMWARE 2.7.7", 120, 120, 120)
    return
  end

  -- the scene: the clock's 20 s quarter of the PERIOD, moved on by the presses
  local c = rawget(px, "button") and px.button() or 0
  local slot = floor(t / SCENE)                      -- 0..3, the same on every panel with NTP
  local want = (slot + c - base) % #SCENES + 1
  if not cur then
    cur = want                                       -- opened: nothing to flow in from
  elseif want ~= cur then
    cur = want
    LINES_SLOT, FADE_SLOT = FADE_SLOT, LINES_SLOT
    xfAt = c ~= seen and t or slot * SCENE
  end
  seen = c

  local s = SCENES[cur]
  local w1 = sin(T * 2 * pi / 31)
  for i = 1, 3 do D_[i] = (i - 1) * 0.33 + 0.25 * w1 end

  if s == "tunnel" then
    TUN[1][4], TUN[2][4], TUN[3][4] = T * 1.3, -T * 0.9, T * 0.5
    px.field(TEX.tunnel, TUN)
    px.remap(MAPS.tunnel, TEX.tunnel, floor(T * 18), floor(T * 55), px.palette(SPEC))
  elseif s == "planet" then
    px.remap(MAPS.planet, TEX.planet, floor(T * 9), 0, PAL.planet)
    px.mode("add")
    for i = 1, #SX do
      local k = 40 + floor(80 * (0.5 + 0.5 * sin(T * 1.3 + i)))
      px.pixel(SX[i], SY[i], k, k, k)
    end
    px.mode("set")
  elseif s == "road" then
    px.remap(MAPS.road, TEX.road, floor(64 + 30 * sin(T * 0.4)), floor(T * 90), PAL.road)
    -- the sky: violet into orange toward the horizon, and the sun
    for y = 0, 29 do
      local k = y / 29
      px.line(0, y, W - 1, y, floor(30 + 200 * k * k), floor(10 + 60 * k * k), floor(60 + 40 * k))
    end
    px.circle(64, 24, 12, 255, 200, 60, true)
    for y = 16, 29, 3 do px.line(50, y, 78, y, floor(30 + 200 * (y / 29) ^ 2), floor(10 + 60 * (y / 29) ^ 2), floor(60 + 40 * y / 29)) end
  else
    WHI[1][3], WHI[2][5] = T * 0.25, -T * 2.5
    px.field(TEX.whirl, WHI)
    px.remap(MAPS.whirl, TEX.whirl, 0, 0, px.palette(SPEC))
  end

  if xfAt then
    local u = ((t - xfAt) % PERIOD) / XF             -- clock seconds, across the wrap
    if u >= 1 then xfAt = nil else px.mix(FADE_SLOT, 1 - u * u * (3 - 2 * u)) end
  end
  px.save(LINES_SLOT)
end

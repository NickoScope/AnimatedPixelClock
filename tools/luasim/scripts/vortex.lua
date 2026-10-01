-- @upload-only
-- ============================================================
-- VORTEX - light poured into a whirlpool that never ends
-- ============================================================
-- @name.en Vortex
-- @name.ru Водоворот
-- @about.en Light poured into an endless whirlpool: a bright shape in the middle streams outward in
-- @about.en spirals and fades at the edges. Every 30 s by the clock, the same on every panel, the shape
-- @about.en changes: a ring, a figure of eight, a star, spokes, a spiral arm.
-- @about.ru Свет, закрученный в бесконечный водоворот: яркая фигура в центре уходит спиралями наружу и
-- @about.ru гаснет у краёв. Каждые 30 с по часам, одинаково на всех панелях, фигура меняется: кольцо,
-- @about.ru восьмёрка, звезда, спицы, спиральный рукав.
-- @control.en knob press: Next shape now; it holds until the next change by the clock.
-- @control.ru knob press: Сразу следующая фигура; она держится до ближайшей смены по часам.
-- @function.en 5 shapes, 30 s each, by the clock: the same on every panel
-- @function.en No clock
-- @function.en Needs firmware 2.7.5 or later
-- @function.ru 5 фигур по 30 с, по часам: одинаково на всех панелях
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.5 или новее
--
-- A few bright shapes are drawn at the middle each frame, and px.feedback
-- does the rest: every frame the whole picture is resampled a little larger,
-- a little turned and a little darker, so what was drawn streams outward in
-- spirals and fades at the edges - the MilkDrop trick. One shape becomes a
-- tunnel, a flower, a galaxy.
--
-- It does not repeat. The zoom, the twist and the drift of the centre each
-- follow their own slow wave (periods of 17, 29, 43 and 61 s, which do not
-- divide into each other), the colours come from Inigo Quilez's cosine
-- palette drifting the same way, and every 30 s the shape at the middle
-- changes: a ring of dots, a figure-of-eight, a star, spokes, a spiral arm.
-- The button changes the shape now.
--
-- The shape is read off the wall clock, so two panels (or a panel and its
-- twin) show the same one at the same moment: px.t() is the phase of the
-- 150 s PERIOD aligned to the epoch, and its 30 s fifths are the shapes. A
-- press moves one shape on from the clock's (the presses are counted from
-- the moment the effect opened); the shape it brings holds until the clock's
-- next 30 s boundary, anything from 0 to 30 s. The whirl and the colours
-- are each panel's own.
--
-- Needs firmware 2.7.5 or later for px.feedback (2.7.4 for px.palette);
-- without it, it fades trails in place.
-- ============================================================
PERIOD = 150.0                -- 5 shapes x 30 s: the shape is read off the clock
FPS = 15

local W, H = px.size()
local sin, cos, floor, pi = math.sin, math.cos, math.floor, math.pi
local CX, CY = (W - 1) / 2, (H - 1) / 2
local HAS = rawget(px, "feedback") ~= nil

local SHAPES = { "ring", "eight", "star", "spokes", "arm" }
local shape = 1
local SCENE = 30
local T, tprev = 0, nil
-- px.button() counts from boot, not from here: the presses since the effect opened
local base = rawget(px, "button") and px.button() or 0

-- the palette, reused: iq's cosine palette with its phases drifting
local A_, B_, C_, D_ = { 0.5, 0.5, 0.5 }, { 0.5, 0.5, 0.5 }, { 1, 1, 1 }, { 0, 0.33, 0.67 }
local SPEC = { "cos", A_, B_, C_, D_ }
local pal

local function colour(i)
  local r, g, b = px.pal(pal, i)
  return r, g, b
end

local function dot(x, y, i, bright)
  local r, g, b = colour(i)
  local xi, yi = floor(x + 0.5), floor(y + 0.5)
  px.pixel(xi, yi, floor(r * bright), floor(g * bright), floor(b * bright))
  px.pixel(xi + 1, yi, floor(r * bright * 0.5), floor(g * bright * 0.5), floor(b * bright * 0.5))
  px.pixel(xi, yi + 1, floor(r * bright * 0.5), floor(g * bright * 0.5), floor(b * bright * 0.5))
end

local function seg(x0, y0, x1, y1, i, bright)
  local r, g, b = colour(i)
  px.line(floor(x0 + 0.5), floor(y0 + 0.5), floor(x1 + 0.5), floor(y1 + 0.5),
          floor(r * bright), floor(g * bright), floor(b * bright))
end

-- the shape at the middle, turning; ox, oy its centre, R its size
local R = 1.7
local function emit(ox, oy, t)
  local s = SHAPES[shape]
  local hue = floor(t * 40)
  if s == "ring" then
    for k = 0, 5 do
      local a = t * 1.7 + k * pi / 3
      dot(ox + 6 * R * cos(a), oy + 6 * R * sin(a), hue + k * 40, 1)
    end
  elseif s == "eight" then
    for k = 0, 2 do
      local a = t * 2.3 + k * 2 * pi / 3
      dot(ox + 9 * R * sin(a), oy + 5 * R * sin(2 * a), hue + k * 80, 1)
    end
  elseif s == "star" then
    local n = 5
    for k = 0, n - 1 do
      local a0 = t * 0.9 + k * 2 * pi / n
      local a1 = t * 0.9 + (k + 2) * 2 * pi / n
      seg(ox + 7 * R * cos(a0), oy + 7 * R * sin(a0), ox + 7 * R * cos(a1), oy + 7 * R * sin(a1), hue + k * 50, 0.9)
    end
  elseif s == "spokes" then
    for k = 0, 3 do
      local a = t * 1.3 + k * pi / 2
      seg(ox + 2 * R * cos(a), oy + 2 * R * sin(a), ox + 9 * R * cos(a), oy + 9 * R * sin(a), hue + k * 64, 0.9)
    end
  else -- a spiral arm: two dots circling at different radii
    local a = t * 3.1
    dot(ox + 4 * R * cos(a), oy + 4 * R * sin(a), hue, 1)
    dot(ox + 10 * R * cos(-a * 0.7), oy + 10 * R * sin(-a * 0.7), hue + 128, 1)
  end
end

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
  -- a 32-bit float T outgrows a frame's step after days: held under an hour
  if T > 3600 then T = T - 3600 end

  -- the shape: the clock's 30 s fifth of the PERIOD, moved on by the presses
  local k = (rawget(px, "button") and px.button() or 0) - base
  local slot = floor(t / SCENE)                      -- 0..4, the same on every panel with NTP
  shape = (slot + k) % #SHAPES + 1

  local w1, w2, w3, w4 = sin(T * 2 * pi / 17), sin(T * 2 * pi / 29), sin(T * 2 * pi / 43), sin(T * 2 * pi / 61)
  for i = 1, 3 do
    D_[i] = (i - 1) * 0.33 + 0.25 * w3 + 0.1 * i * w4
    C_[i] = 1 + 0.3 * w2 * (i - 2)
  end
  pal = px.palette(SPEC)

  -- the whirl: the centre wanders a little, the zoom breathes, the twist
  -- changes its sense slowly
  local ox, oy = CX + 10 * w3, CY + 4 * w4
  if HAS then
    px.feedback{ zoom = 1.06 + 0.025 * w1, rot = 0.04 * w2, cx = ox, cy = oy, decay = 0.03 }
  else
    px.fade(0.25)
  end
  -- drawn over, not added: added, the colours of a turn pile up into white
  emit(ox, oy, T)
end

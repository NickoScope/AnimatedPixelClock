-- @upload-only
-- ============================================================
-- KALEIDOSCOPE - colour that never plays the same twice
-- ============================================================
-- A demoscene plasma folded into four mirrors. Nothing here is drawn a pixel
-- at a time in a frame; three native calls a frame do the picture:
--
--   * two fields of sines (lodev's plasma: along x, along y, along the
--     diagonal and in rings round a point) sit in two 8-bit layers. They do not
--     move. Colour moves through them: each layer is shown through the palette
--     with its own offset, stepping at its own speed and in its own direction,
--     and where the second is brighter it wins, channel by channel ("max":
--     added, the two wash out to white). The bands of the two run through
--     each other, and that is the motion (palette cycling, as on the 8-bit
--     machines).
--   * the palette is Inigo Quilez's cosine palette, a + b*cos(2pi(c*t + d)),
--     rebuilt every frame while c and d drift on slow periods that do not
--     divide into each other (37, 59 and 83 s), so the colours never come back
--     the same way.
--   * px.mirror("hv") folds the top-left quarter onto all four, which makes a
--     field of sines a kaleidoscope.
--
-- Every 40 s the light dips, a new mood (one of iq's palette families) and
-- two new fields come in, and it rises again. The next fields are computed a
-- few rows a frame during the scene before, so a change costs no frame.
-- The fields are seeded from the date and the minute: tonight's kaleidoscope
-- is not this morning's. The button changes the mood now.
--
-- Needs firmware 2.7.4 or later (px.layer, px.palette, px.show, px.mirror).
-- ============================================================
PERIOD = 600.0
FPS = 15

local W, H = px.size()
local QW, QH = W // 2, H // 2
local sin, sqrt, floor = math.sin, math.sqrt, math.floor

-- xorshift32: math.random is seeded differently in the simulator and on the
-- panel, and they must draw the same picture.
local now = px.now()
local seed = ((now.year * 366 + now.yday) * 1440 + now.hour * 60 + now.min) * 0x9E3779B1   -- hex wraps to a 32-bit integer; the decimal would be a float
seed = (seed ~ (seed >> 15)) | 1
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end
local function rr(a, b) return a + (b - a) * rnd() end

-- Palette families from iq's article (https://iquilezles.org/articles/palettes/).
local MOODS = {
  { a = { 0.5, 0.5, 0.5 }, b = { 0.5, 0.5, 0.5 }, c = { 1.0, 1.0, 1.0 }, d = { 0.00, 0.33, 0.67 } },
  { a = { 0.5, 0.5, 0.5 }, b = { 0.5, 0.5, 0.5 }, c = { 1.0, 1.0, 1.0 }, d = { 0.00, 0.10, 0.20 } },
  { a = { 0.5, 0.5, 0.5 }, b = { 0.5, 0.5, 0.5 }, c = { 1.0, 1.0, 0.5 }, d = { 0.80, 0.90, 0.30 } },
  { a = { 0.5, 0.5, 0.5 }, b = { 0.5, 0.5, 0.5 }, c = { 1.0, 0.7, 0.4 }, d = { 0.00, 0.15, 0.20 } },
  { a = { 0.5, 0.5, 0.5 }, b = { 0.5, 0.5, 0.5 }, c = { 2.0, 1.0, 0.0 }, d = { 0.50, 0.20, 0.25 } },
  { a = { 0.8, 0.5, 0.4 }, b = { 0.2, 0.4, 0.2 }, c = { 2.0, 1.0, 1.0 }, d = { 0.00, 0.25, 0.25 } },
}

-- A field: four sines with their own frequencies and phases, and a centre
-- for the rings. Built a few rows a frame into a layer.
local function new_field()
  return {
    fx = rr(0.06, 0.22), fy = rr(0.06, 0.22), fxy = rr(0.03, 0.15), fr = rr(0.12, 0.40),
    p1 = rr(0, 6.28), p2 = rr(0, 6.28), p3 = rr(0, 6.28), p4 = rr(0, 6.28),
    cx = rr(0, QW), cy = rr(0, QH), row = 0,
  }
end
local function build_rows(L, f, rows)
  local fx, fy, fxy, fr, p1, p2, p3, p4, cx, cy = f.fx, f.fy, f.fxy, f.fr, f.p1, f.p2, f.p3, f.p4, f.cx, f.cy
  for _ = 1, rows do
    local y = f.row
    if y >= QH then return true end
    local sy = sin(y * fy + p2)
    local dy2 = (y - cy) * (y - cy)
    for x = 0, QW - 1 do
      local dx = x - cx
      local v = sin(x * fx + p1) + sy + sin((x + y) * fxy + p3) + sin(sqrt(dx * dx + dy2) * fr + p4)
      L:set(x, y, floor((v + 4) * 31.9))
    end
    f.row = y + 1
  end
  return f.row >= QH
end

local LA, LB = px.layer(), px.layer()          -- on screen
local NA, NB = px.layer(), px.layer()          -- being built for the next scene
local fa, fb = new_field(), new_field()
build_rows(LA, fa, QH); build_rows(LB, fb, QH)
local na, nb = new_field(), new_field()

local mood = 1 + floor(rnd() * #MOODS)
local SCENE, DIP = 40, 1.6
local T, tprev, sceneAt = 0, nil, 0
local switching, swapped = false, false
local lastClicks = rawget(px, "button") and px.button() or 0

-- the palette spec, reused every frame
local A_, B_, C_, D_ = { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }
local SPEC = { "cos", A_, B_, C_, D_ }

local function start_switch()
  switching, swapped = true, false
  sceneAt = T
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

  local c = rawget(px, "button") and px.button() or 0
  if c ~= lastClicks then lastClicks = c; if not switching then start_switch() end end
  if not switching and T - sceneAt > SCENE then start_switch() end

  -- the next fields, a few rows a frame
  if not build_rows(NA, na, 3) then elseif not build_rows(NB, nb, 3) then end

  -- the light: down over DIP s, the new scene swapped in at the bottom, up again
  local bri = 1
  if switching then
    local k = (T - sceneAt) / DIP
    if k < 1 then
      bri = 1 - k
    else
      if not swapped and na.row >= QH and nb.row >= QH then
        LA, NA, LB, NB = NA, LA, NB, LB
        na, nb = new_field(), new_field()
        mood = mood % #MOODS + 1
        swapped = true
        sceneAt = T - DIP
      end
      if swapped then
        local u = (T - sceneAt - DIP) / DIP
        if u >= 1 then switching = false; sceneAt = T; u = 1 end
        bri = u
      else
        bri = 0
      end
    end
  end
  bri = bri * bri * (3 - 2 * bri)

  local m = MOODS[mood]
  local cs = 1 + 0.18 * sin(T * 6.2832 / 59)
  local dd = 0.12 * sin(T * 6.2832 / 37)
  for i = 1, 3 do
    -- a little under a and over b: the troughs clip to black, which an LED
    -- shows as depth and a screen shot as contrast
    A_[i], B_[i] = m.a[i] - 0.08, m.b[i] + 0.12
    C_[i] = m.c[i] * cs
    D_[i] = m.d[i] + dd + 0.05 * sin(T * 6.2832 / 83 + i)
  end
  local pal = px.palette(SPEC)

  px.show(LA, pal, floor(T * 23), bri * 0.9)
  px.show(LB, pal, floor(-T * 17), bri * 0.75, "max")
  px.mirror("hv")
end

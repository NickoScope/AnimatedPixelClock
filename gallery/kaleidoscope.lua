-- @upload-only
-- ============================================================
-- KALEIDOSCOPE - colour that never plays the same twice
-- ============================================================
-- @name.en Kaleidoscope
-- @name.ru Калейдоскоп
-- @about.en Colour plasma folded into four mirrors. Colours run through the pattern and never come back
-- @about.en the same. Every 40 s by the clock a new mood and a new pattern flow in over 3 s, at the same
-- @about.en moment and the same on every panel.
-- @about.ru Цветная плазма, сложенная в четыре зеркала. Цвета бегут по узору и не повторяются. Каждые 40
-- @about.ru с по часам за 3 с перетекает новое настроение и новый узор, на всех панелях в один и тот же
-- @about.ru момент и одинаково.
-- @control.en knob press: A new mood now (unless a change is already under way); it stays until the next change by the clock.
-- @control.ru knob press: Сразу новое настроение (если смена уже не идёт); оно держится до ближайшей смены по часам.
-- @function.en 6 colour moods
-- @function.en A new scene every 40 s by the clock
-- @function.en No clock
-- @function.en Needs firmware 2.7.4 or later
-- @function.ru 6 цветовых настроений
-- @function.ru Новая сцена каждые 40 с по часам
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.4 или новее
--
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
-- Every 40 s a new mood (one of iq's palette families) and two new fields
-- come in, and the old scene flows into the new one over 3 s (px.save and
-- px.mix, firmware 2.7.5; before it the light dips through black). The
-- next fields are computed a
-- few rows a frame during the scene before, so a change costs no frame.
--
-- It all runs on the wall clock, not on the time since the page opened. A
-- scene is numbered by the 40 s since 1970 (UTC, from px.now()), its fields
-- are seeded from that number and its mood is the number's, and the 40 s
-- turn and the colours' drift come from px.t() over an hour aligned to the
-- epoch: every panel with NTP shows the same scene, flowing in at the same
-- moment, and tonight's kaleidoscope is not this morning's. The button
-- changes the mood now: the scene after the one on screen, which the clock
-- then carries on from (px.button() counted from the opening). A press
-- during a change is ignored - the first 3 s of a scene by the clock, or
-- 3 s after the press before.
--
-- Needs firmware 2.7.4 or later (px.layer, px.palette, px.show, px.mirror),
-- and 2.7.5 for the flow between scenes.
-- ============================================================
PERIOD = 3600.0                       -- px.t(): the wall clock's hour, aligned to the epoch
FPS = 15

local W, H = px.size()
local QW, QH = W // 2, H // 2
local sin, sqrt, floor = math.sin, math.sqrt, math.floor

-- xorshift32: math.random is seeded differently in the simulator and on the
-- panel, and they must draw the same picture. Seeded from a scene's number,
-- so every panel draws that scene's fields.
local seed = 1
local function reseed(n)
  local s = n * 0x9E3779B1             -- hex wraps to a 32-bit integer; the decimal would be a float
  seed = (s ~ (s >> 15)) | 1
end
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
-- the two fields of scene n
local function fields_for(n)
  reseed(n)
  return new_field(), new_field()
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

-- The wall clock: a scene is 40 s, numbered from 1970 in UTC. px.now() gives
-- the hour since 1970 (an int32 until 2038) and px.t() the seconds in it.
local SCENE, XF = 40, 3.0             -- a scene, and the flow from one into the next
local SLOTS = 90                      -- scenes an hour: PERIOD / SCENE
local function hour_now()
  local n = px.now()
  local y = n.year
  local days = 365 * (y - 1970) + (y - 1969) // 4 - (y - 1901) // 100 + (y - 1601) // 400 + n.yday
  local e = days * 86400 + n.hour * 3600 + n.min * 60 + n.sec - floor(n.utc * 3600 + 0.5)
  return e // 3600, e % 3600
end

-- At load px.t() is not on PERIOD yet: the scene on screen from px.now().
local hour, sec0 = hour_now()
local G = hour * SLOTS + sec0 // SCENE -- the scene on screen
local LA, LB = px.layer(), px.layer()          -- on screen
local NA, NB = px.layer(), px.layer()          -- being built for the next scene
local fa, fb = fields_for(G)
build_rows(LA, fa, QH); build_rows(LB, fb, QH)
local nextG = G + 1                   -- the scene NA, NB are being built for
local na, nb = fields_for(nextG)

local mood = G % #MOODS + 1
local T, tprev = 0, nil               -- T: the clock's seconds in the hour
local fading, fadeAt = false, 0
local OA, OB, oldMood = nil, nil, mood -- the scene going out, while it flows away
local BTN = rawget(px, "button")
local lastClicks = BTN and BTN() or 0
local presses, pressAt = 0, nil       -- presses taken since the page opened; the clock at the last
-- px.mix (firmware 2.7.5) lets the old scene flow into the new; without it the
-- light dips through black instead.
local HAS_MIX = rawget(px, "mix") ~= nil

-- the palette spec, reused every frame
local A_, B_, C_, D_ = { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }
local SPEC = { "cos", A_, B_, C_, D_ }

local function palette_for(md)
  local m = MOODS[md]
  local cs = 1 + 0.18 * sin(T * 6.2832 / 59)
  local dd = 0.12 * sin(T * 6.2832 / 37)
  for i = 1, 3 do
    -- a little under a and over b: the troughs clip to black, which an LED
    -- shows as depth and a screen shot as contrast
    A_[i], B_[i] = m.a[i] - 0.08, m.b[i] + 0.12
    C_[i] = m.c[i] * cs
    D_[i] = m.d[i] + dd + 0.05 * sin(T * 6.2832 / 83 + i)
  end
  return px.palette(SPEC)
end

local function paint(L1, L2, md, bri)
  local pal = palette_for(md)
  px.show(L1, pal, floor(T * 23), bri * 0.9)
  px.show(L2, pal, floor(-T * 17), bri * 0.75, "max")
  px.mirror("hv")
end

function draw()
  -- T runs from 0 to 3600 with the wall clock's hour: the drifts' periods
  -- restart each hour, a seam no one sees. The hour itself is read again
  -- when the phase wraps, and whenever the clock jumps.
  local t = px.t() * PERIOD
  if not tprev or t < tprev or t - tprev > 2 then hour = hour_now() end
  tprev = t
  T = t
  local sl = floor(t / SCENE)
  local s = t - sl * SCENE            -- seconds into this scene, by the clock

  -- the button: the next scene now, unless a change is under way by the clock
  if pressAt and (t - pressAt) % PERIOD >= XF then pressAt = nil end
  local c = BTN and BTN() or 0
  if c ~= lastClicks then
    lastClicks = c
    if s >= XF and not pressAt then presses = presses + 1; pressAt = t end
  end
  local want = hour * SLOTS + sl + presses

  if not fading then
    if want ~= G then
      -- not the scene the spare pair is for (the clock jumped): build that one
      if want ~= nextG then nextG = want; na, nb = fields_for(want) end
      if na.row >= QH and nb.row >= QH then
        -- the new scene comes on screen, the old stays to flow out, and the
        -- spare pair is the old pair's once it has
        OA, OB, oldMood = LA, LB, mood
        LA, LB = NA, NB
        G = want
        mood = G % #MOODS + 1
        fading = true
        -- a change by the clock flows from the turn of the scene, so the
        -- panels flow together; one by the button, or one that waited for
        -- its fields, from now
        fadeAt = s < XF and t - s or t
      end
    end
    if not fading then
      -- the next fields, a few rows a frame, once the flow has given the pair back
      if not build_rows(NA, na, 3) then elseif not build_rows(NB, nb, 3) then end
      paint(LA, LB, mood, 1)
      return
    end
  end
  local u = ((t - fadeAt) % PERIOD) / XF
  if u >= 1 then
    -- the flow is over: the old pair becomes the spare, built afresh for the scene after
    fading = false
    NA, NB, OA, OB = OA, OB, nil, nil
    nextG = G + 1
    na, nb = fields_for(nextG)
    paint(LA, LB, mood, 1)
    return
  end
  u = u * u * (3 - 2 * u)
  if HAS_MIX then
    -- the old scene painted, kept, the new one painted, the old mixed over it
    paint(OA, OB, oldMood, 1)
    px.save(2)
    paint(LA, LB, mood, 1)
    px.mix(2, 1 - u)
  elseif u < 0.5 then
    paint(OA, OB, oldMood, 1 - 2 * u)            -- older firmware: down through black...
  else
    paint(LA, LB, mood, 2 * u - 1)               -- ...and up again
  end
end

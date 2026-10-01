-- @upload-only
-- ============================================================
-- REACTION - living patterns that grow, split and never settle
-- ============================================================
-- @name.en Reaction
-- @name.ru Реакция
-- @about.en Living chemical patterns: spots that divide like cells, coral, a maze, worms. Every minute
-- @about.en by the clock the pattern drifts slowly into the next, at the same moment on every panel, and
-- @about.en new drops fall every 30 s.
-- @about.ru Живые химические узоры: пятна делятся как клетки, растут кораллы, лабиринт, черви. Каждую
-- @about.ru минуту по часам узор медленно перетекает в следующий, на всех панелях в один и тот же момент,
-- @about.ru каждые 30 с падают новые капли.
-- @control.en knob press: Next pattern now (cells, coral, maze, worms) and fresh drops; it runs until the next change by the clock.
-- @control.ru knob press: Сразу следующий узор (клетки, кораллы, лабиринт, черви) и новые капли; он идёт до ближайшей смены по часам.
-- @function.en 4 patterns: 45 s each plus 15 s of change
-- @function.en No clock
-- @function.en Needs firmware 2.7.6 or later
-- @function.ru 4 узора: 45 с каждый и 15 с перехода
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.6 или новее
--
-- Two chemicals spread over the screen and react, in the Gray-Scott model
-- (Karl Sims's reaction-diffusion tutorial): where B meets enough A it makes
-- more B, and both are fed and drained at their own rates. From a few drops
-- grow spots that divide like cells, coral, a labyrinth, worms - which of
-- them depends on the feed f and the kill k.
--
-- It never settles because f and k do not stand still: they travel slowly
-- from one of those regimes to the next (a minute each), so spots stretch
-- into worms and worms knot into a maze, and every 30 s a few new drops fall.
-- The colours come from a gradient that drifts too. The button moves to the
-- next regime at once and drops new seeds.
--
-- The regime, the drops and the colours follow the wall clock, not the time
-- since the page opened: px.t() is the phase of a 4-minute cycle aligned to
-- the epoch, so two panels with NTP change regime at the same moment, and a
-- panel opened late joins the cycle where it is. The button adds one regime
-- to the clock's (px.button() counted from the opening), which then holds
-- until the next minute the clock turns. The field itself is a simulation
-- from a few drops: its pixels are each panel's own.
--
-- The model runs in the firmware (px.reaction): 8,192 cells, three steps a
-- frame (about 10 ms each on the panel), in integers so the panel and luasim grow the same patterns.
--
-- Needs firmware 2.7.6 or later (px.reaction; px.palette, px.show 2.7.4).
-- ============================================================
PERIOD = 240.0                       -- the 4 regimes of a minute, by the wall clock
FPS = 15

local W, H = px.size()
local sin, floor, pi = math.sin, math.floor, math.pi
local HAS = rawget(px, "reaction") ~= nil

-- f, k of the regimes (from the Gray-Scott parameter map; Karl Sims, Robert Munafo)
local REGIMES = {
  { f = 0.0367, k = 0.0649, name = "MITOSIS" },
  { f = 0.0545, k = 0.0620, name = "CORAL" },
  { f = 0.0290, k = 0.0570, name = "MAZE" },
  { f = 0.0780, k = 0.0610, name = "WORMS" },
}
local STAY, MOVE = 45, 15            -- seconds in a regime, and moving to the next
local SLOT = STAY + MOVE             -- a regime's minute; PERIOD holds #REGIMES of them
local DROP = 30                      -- seconds between new drops; divides PERIOD
local BTN = rawget(px, "button")
local base = BTN and BTN() or 0      -- the presses before the page opened
local lastClicks, dropAt = base, nil

local seed = 0x5EED1234
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end

local R = HAS and px.reaction{ f = REGIMES[1].f, k = REGIMES[1].k } or nil
local L = HAS and px.layer() or nil
local PARAMS = { f = 0, k = 0 }

local function drops(n)
  for _ = 1, n do R:seed(floor(rnd() * W), floor(rnd() * H), 2 + floor(rnd() * 3)) end
end
if HAS then drops(14) end

-- the colour: from the background through the chemical's body to its edge
-- B rarely passes 0.5 (index 128), so the colours sit low in the palette
local STOPS = { { 0, 0, 0, 0 }, { 30, 0, 0, 0 }, { 60, 0, 0, 0 }, { 95, 0, 0, 0 }, { 140, 0, 0, 0 } }
-- t: seconds into PERIOD; 80 and 120 divide it, so the drift has no seam
local function palette_at(t)
  local a, b = 0.5 + 0.5 * sin(t * 2 * pi / 80), 0.5 + 0.5 * sin(t * 2 * pi / 120 + 1)
  STOPS[1][2], STOPS[1][3], STOPS[1][4] = floor(4 + 10 * a), floor(6 + 8 * b), floor(18 + 20 * a)
  STOPS[2][2], STOPS[2][3], STOPS[2][4] = floor(20 + 60 * b), floor(40 + 80 * a), floor(120 + 100 * b)
  STOPS[3][2], STOPS[3][3], STOPS[3][4] = floor(200 * a + 30), floor(90 + 60 * b), floor(220 - 120 * a)
  STOPS[4][2], STOPS[4][3], STOPS[4][4] = floor(255 - 40 * b), floor(170 + 60 * a), floor(80 + 60 * b)
  STOPS[5][2], STOPS[5][3], STOPS[5][4] = 255, 245, 220
  return px.palette(STOPS)
end

function draw()
  local t = px.t() * PERIOD          -- seconds into the cycle, the same on every panel

  if not HAS then
    px.clear(0, 0, 0)
    px.text(2, 28, "NEEDS FIRMWARE 2.7.6", 120, 120, 120)
    return
  end

  -- the regime: the clock's minute, plus one for every press since the page
  -- opened; at the end of a minute b has become the next minute's a
  local c = BTN and BTN() or 0
  local slot = floor(t / SLOT)
  local regime = (slot + c - base) % #REGIMES + 1
  -- new drops: a press's, else the clock's every DROP s (not on the first
  -- frame: the page's own have just fallen)
  local d = floor(t / DROP)
  if c ~= lastClicks then lastClicks = c; drops(10)
  elseif dropAt and d ~= dropAt then drops(4) end
  dropAt = d

  -- f and k: held, then eased toward the next regime
  local a, b = REGIMES[regime], REGIMES[regime % #REGIMES + 1]
  local u = (t - slot * SLOT - STAY) / MOVE
  u = u < 0 and 0 or u > 1 and 1 or u
  u = u * u * (3 - 2 * u)
  PARAMS.f, PARAMS.k = a.f + (b.f - a.f) * u, a.k + (b.k - a.k) * u
  R:params(PARAMS)
  R:step(3)
  R:show(L)
  px.show(L, palette_at(t))
end

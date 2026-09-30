-- @upload-only
-- ============================================================
-- NEBULA - a cloud of gas and stars that never settles
-- ============================================================
-- @name.en Nebula
-- @name.ru Туманность
-- @about.en A cloud of gas that slowly boils, with a faint ripple running through it and stars twinkling
-- @about.en in front. Most of the sky stays dark and the clouds glow.
-- @about.ru Облако газа, которое медленно клубится, по нему бежит слабая рябь, впереди мерцают звёзды.
-- @about.ru Большая часть неба тёмная, облака светятся.
-- @control.en knob press: Next mood: violet and rose, teal and ice, ember, aurora.
-- @control.ru knob press: Следующее настроение: фиолетово-розовое, бирюзово-ледяное, угли, полярное сияние.
-- @function.en 4 colour moods
-- @function.en No clock
-- @function.en Needs firmware 2.7.5 or later
-- @function.ru 4 цветовых настроения
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.5 или новее
--
-- Perlin noise in three octaves, sliced along time, makes a nebula that boils
-- slowly; a faint ripple from a wandering centre runs through it; the field
-- sits low, so most of the sky stays dark and the clouds glow, through a
-- gradient from black to the gas's hot core. Stars twinkle in front, each by
-- the noise at its own place in time. The button changes the nebula's mood:
-- violet and rose, teal and ice, ember, aurora.
--
-- Every frame is one px.field call into a layer and one px.show through the
-- palette: 8,192 samples of fractal noise that would take a Lua loop over
-- 100 ms here take a few in C.
--
-- The time the gas boils along is the wall clock's, not the page's: px.t()
-- over PERIOD = 3600 is the second of the hour, the same on every panel with
-- NTP, so two panels show the same nebula at the same moment. Every motion
-- makes a whole number of turns in the hour - the noise repeats every 256
-- units, the ripple's phase every 2 pi - so the hour comes round without a
-- seam. The mood is the count of presses since the page opened, so one press
-- on each of two panels leaves them in the same mood.
--
-- Needs firmware 2.7.5 or later (px.field, px.noise; px.show and
-- px.palette are 2.7.4).
-- ============================================================
PERIOD = 3600.0     -- px.t() * PERIOD is the second of the hour on the wall clock
FPS = 15

local W, H = px.size()
local sin, floor, pi = math.sin, math.floor, math.pi
local HAS = rawget(px, "field") ~= nil

-- the moods: gradients from black through the gas's colours to its hot core;
-- the field sits low, so most of the sky is dark and the clouds glow
local MOODS = {
  { { 0, 0, 0, 0 }, { 60, 25, 0, 45 }, { 115, 100, 20, 140 }, { 175, 230, 90, 170 }, { 225, 255, 200, 230 }, { 255, 255, 255, 255 } },   -- violet and rose
  { { 0, 0, 0, 0 }, { 60, 0, 20, 45 }, { 115, 0, 90, 140 }, { 175, 40, 185, 210 }, { 225, 190, 255, 245 }, { 255, 255, 255, 255 } },    -- teal and ice
  { { 0, 0, 0, 0 }, { 60, 45, 6, 0 }, { 115, 160, 45, 10 }, { 175, 245, 130, 35 }, { 225, 255, 230, 160 }, { 255, 255, 255, 230 } },    -- ember
  { { 0, 0, 0, 0 }, { 60, 0, 40, 25 }, { 115, 25, 160, 90 }, { 175, 130, 255, 170 }, { 225, 225, 255, 235 }, { 255, 255, 255, 255 } },  -- aurora
}
local PALS = {}
for i, stops in ipairs(MOODS) do PALS[i] = px.palette(stops) end
-- the presses before the page opened are not this page's
local base = rawget(px, "button") and px.button() or 0
-- whole turns in the hour, so the hour wraps without a jump: the noise
-- repeats every 256 units, sin and the ripple's phase every 2 pi
local HOUR = 3600
local Z_RATE = 256 / HOUR                  -- the gas: one noise period an hour (was 0.07)
local STAR_RATE = 11 * 256 / HOUR          -- the twinkle: 11 noise periods (was 0.8)
local RIP_RATE = 516 * 2 * pi / HOUR       -- the ripple: 516 turns (was 0.9 rad/s)
local W2 = 54 * 2 * pi / HOUR              -- the ripple's centre: 54 and 37 swings
local W3 = 37 * 2 * pi / HOUR              -- (were 67 s and 97 s)

local L = HAS and px.layer() or nil
local NOISE = { "noise", 0.035, 0, 1.25, 3 }
local RING = { "ring", 64, 32, 0.12, 0, 0.22 }
local TERMS = { NOISE, RING }

-- stars: fixed places, from a xorshift so the simulator and the panel agree
local seed = 0x6B43A9B5
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end
local SX, SY = {}, {}
for i = 1, 70 do SX[i], SY[i] = floor(rnd() * W), floor(rnd() * H) end

function draw()
  local T = px.t() * PERIOD                 -- the second of the hour, 0..3600

  local c = rawget(px, "button") and px.button() or 0
  local mood = (c - base) % #MOODS + 1

  local w2, w3 = sin(T * W2), sin(T * W3)
  local pal = PALS[mood]

  if HAS then
    NOISE[3] = T * Z_RATE                                -- the gas boils along time
    RING[2], RING[3] = 64 + 40 * w2, 32 + 18 * w3        -- the ripple's centre wanders
    RING[5] = -((T * RIP_RATE) % (2 * pi))              -- px.field holds a phase to +-1000 rad
    px.field(L, TERMS, 62)
    px.show(L, pal)
  else
    px.clear(8, 4, 16)
  end

  -- the stars, over the gas, each twinkling by the noise at its place in time
  px.mode("add")
  for i = 1, #SX do
    local k = HAS and px.noise(i * 7.3, T * STAR_RATE) or 0.3 * sin(T * 1.7 + i)
    if k > 0 then
      local b = floor(60 + 400 * k)
      px.pixel(SX[i], SY[i], b, b, b)
    end
  end
  px.mode("set")
end

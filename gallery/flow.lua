-- @upload-only
-- ============================================================
-- FLOW - hundreds of particles on currents that never repeat
-- ============================================================
-- @name.en Flow
-- @name.ru Потоки
-- @about.en Hundreds of particles on currents that never repeat. Three scenes of 40 s each, by the
-- @about.en clock, the same on every panel: glowing currents, a fountain of sparks bouncing off the
-- @about.en walls, snow blown by the wind.
-- @about.ru Сотни частиц на течениях, которые не повторяются. Три сцены по 40 с по часам, одинаково на
-- @about.ru всех панелях: светящиеся течения, фонтан искр, отскакивающих от стен, снег на ветру.
-- @control.en knob press: Next scene now (currents, fountain, snow); it runs until the next change by the clock.
-- @control.ru knob press: Сразу следующая сцена (течения, фонтан, снег); она идёт до ближайшей смены по часам.
-- @function.en 3 scenes, 40 s each, by the clock: the same on every panel
-- @function.en No clock
-- @function.en Needs firmware 2.7.5 or later
-- @function.ru 3 сцены по 40 с, по часам: одинаково на всех панелях
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.5 или новее
--
-- A particle system in C (px.particles) does the moving and the drawing;
-- this script only says where they come from and what pulls them. Three
-- scenes, 40 s each, the button for the next:
--
--   * flow: seven hundred motes carried by a flow field - the curl of
--     Perlin noise, so they swirl along its contour lines and never pool -
--     leaving glowing trails (px.fade) in colours from a drifting palette;
--   * fountain: sparks thrown up from the middle of the floor, falling back
--     and bouncing off the walls;
--   * snow: flakes falling slowly, blown about by a soft wind.
--
-- The field itself drifts along time, so the currents change and the
-- pictures they draw never come back the same.
--
-- The scene is read off the wall clock, so two panels (or a panel and its
-- twin) show the same scene at the same moment: px.t() is the phase of the
-- 120 s PERIOD aligned to the epoch, and its 40 s thirds are the scenes. A
-- press moves one scene on from the clock's (the presses are counted from
-- the moment the effect opened); the scene it brings runs until the clock's
-- next 40 s boundary, anything from 0 to 40 s. The palette drifts on the
-- clock too (periods of 40 and 60 s), so the colours match as well; the
-- particles themselves are each panel's own.
--
-- Needs firmware 2.7.5 or later (px.particles; px.palette and px.fade are
-- 2.7.4).
-- ============================================================
PERIOD = 120.0                -- 3 scenes x 40 s: the scene is read off the clock
FPS = 15

local W, H = px.size()
local sin, floor, pi = math.sin, math.floor, math.pi
local HAS = rawget(px, "particles") ~= nil

local SCENES = { "flow", "fountain", "snow" }
local scene = 1
local SCENE = 40
local T, tprev = 0, nil
-- px.button() counts from boot, not from here: the presses since the effect opened
local base = rawget(px, "button") and px.button() or 0

local P = HAS and px.particles(1200, 20260929) or nil
local A_, B_, C_, D_ = { 0.5, 0.5, 0.5 }, { 0.5, 0.5, 0.5 }, { 1, 1, 1 }, { 0, 0.33, 0.67 }
local SPEC = { "cos", A_, B_, C_, D_ }

-- the emitters and the forces of each scene, reused tables
local EMIT_FLOW = { x = 0, y = 0, w = W, h = H, n = 12, speed = 4, life = 5, lifej = 0.5, idx = 0, idxj = 40 }
local STEP_FLOW = { dt = 1 / 15, flow = 110, flowscale = 0.022, flowz = 0, drag = 1.4, edge = "kill" }
local EMIT_FOUNT = { x = 64, y = 62, n = 10, speed = 70, speedj = 0.35, angle = -pi / 2, spread = 0.6, life = 3, lifej = 0.4, idx = 0, idxj = 60 }
local STEP_FOUNT = { dt = 1 / 15, gy = 48, drag = 0.1, edge = "bounce" }
local EMIT_SNOW = { x = -10, y = -2, w = W + 20, h = 0, n = 3, speed = 6, angle = pi / 2, spread = 0.8, life = 12, r = 220, g = 235, b = 255 }
local STEP_SNOW = { dt = 1 / 15, gy = 4, drag = 0.6, flow = 12, flowscale = 0.03, flowz = 0, edge = "kill" }
local DRAW_FLOW = { mode = "add", bri = 0.55, fade = true }
local DRAW_FOUNT = { mode = "add", bri = 1, fade = true }
local DRAW_SNOW = { mode = "set", bri = 1, fade = false }

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

  -- the scene: the clock's 40 s third of the PERIOD, moved on by the presses
  local k = (rawget(px, "button") and px.button() or 0) - base
  local slot = floor(t / SCENE)                      -- 0..2, the same on every panel with NTP
  scene = (slot + k) % #SCENES + 1

  if not HAS then
    px.clear(0, 0, 0)
    px.text(2, 28, "NEEDS FIRMWARE 2.7.5", 120, 120, 120)
    return
  end

  local w1, w2 = sin(t * 2 * pi / 40), sin(t * 2 * pi / 60)   -- whole times into PERIOD: no jump at the wrap
  for i = 1, 3 do D_[i] = (i - 1) * 0.33 + 0.2 * w1 + 0.05 * i * w2 end
  local pal = px.palette(SPEC)
  local s = SCENES[scene]

  if s == "flow" then
    px.fade(0.10)                                   -- the trails
    STEP_FLOW.dt, STEP_FLOW.flowz = dt, T * 0.05
    EMIT_FLOW.pal, EMIT_FLOW.idx = pal, floor(T * 20)
    if P:count() < 700 then P:emit(EMIT_FLOW) end
    P:step(STEP_FLOW)
    P:draw(DRAW_FLOW)
  elseif s == "fountain" then
    px.fade(0.35)
    STEP_FOUNT.dt = dt
    EMIT_FOUNT.pal, EMIT_FOUNT.idx = pal, floor(T * 40)
    EMIT_FOUNT.angle = -pi / 2 + 0.35 * w1
    P:emit(EMIT_FOUNT)
    P:step(STEP_FOUNT)
    P:draw(DRAW_FOUNT)
  else
    px.clear(2, 4, 14)
    STEP_SNOW.dt, STEP_SNOW.flowz = dt, T * 0.1
    if P:count() < 400 then P:emit(EMIT_SNOW) end
    P:step(STEP_SNOW)
    P:draw(DRAW_SNOW)
  end
end

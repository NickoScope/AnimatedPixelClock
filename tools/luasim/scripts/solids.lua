-- @upload-only
-- ============================================================
-- SOLIDS - the Platonic solids turning among slow stars
-- ============================================================
-- A tetrahedron, a cube, an octahedron and an icosahedron, one at a time,
-- turning on a spin that wanders; shown as a lit solid, as a wireframe with
-- anti-aliased edges, or as both; every 20 s the one on screen flows into the
-- next (px.mix). Behind them a field of stars drifts at fractions of a pixel a
-- frame, each star an anti-aliased dot, so it glides instead of stepping. The
-- button brings the next solid now.
--
-- The firmware turns, projects, sorts and draws each solid in one call
-- (px.model once, px.mesh a frame); the script only chooses where it points.
--
-- Needs firmware 2.7.6 or later (px.model, px.mesh, px.dot; px.mix 2.7.5).
-- ============================================================
PERIOD = 600.0
FPS = 15

local W, H = px.size()
local sin, cos, floor, pi, sqrt = math.sin, math.cos, math.floor, math.pi, math.sqrt
local HAS = rawget(px, "mesh") ~= nil

-- edges from faces: every side once
local function edges_of(f)
  local seen, e = {}, {}
  for i = 1, #f, 3 do
    for k = 0, 2 do
      local a, b = f[i + k], f[i + (k + 1) % 3]
      local key = a < b and a * 1000 + b or b * 1000 + a
      if not seen[key] then seen[key] = true; e[#e + 1] = a; e[#e + 1] = b end
    end
  end
  return e
end
local function scaled(v, s) local o = {} for i = 1, #v do o[i] = v[i] * s end return o end

local PHI = (1 + sqrt(5)) / 2
local SOLIDS = {}
if HAS then
  local tf = { 1, 2, 3, 1, 2, 4, 1, 3, 4, 2, 3, 4 }
  SOLIDS[1] = { name = "TETRA", mode = "solid", r = 255, g = 120, b = 60,
    m = px.model{ v = scaled({ 1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, 1 }, 0.66), f = tf, e = edges_of(tf), orient = true } }
  SOLIDS[2] = { name = "CUBE", mode = "both", r = 60, g = 180, b = 255,
    m = px.model{
      v = scaled({ -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1 }, 0.72),
      e = { 1, 2, 2, 3, 3, 4, 4, 1, 5, 6, 6, 7, 7, 8, 8, 5, 1, 5, 2, 6, 3, 7, 4, 8 },
      f = { 1, 3, 2, 1, 4, 3, 5, 6, 7, 5, 7, 8, 1, 2, 6, 1, 6, 5, 4, 8, 7, 4, 7, 3, 1, 5, 8, 1, 8, 4, 2, 3, 7, 2, 7, 6 },
      orient = true } }
  local of = { 5, 1, 3, 5, 3, 2, 5, 2, 4, 5, 4, 1, 6, 3, 1, 6, 2, 3, 6, 4, 2, 6, 1, 4 }
  SOLIDS[3] = { name = "OCTA", mode = "wire", r = 120, g = 255, b = 140,
    m = px.model{ v = { 1.2, 0, 0, -1.2, 0, 0, 0, 1.2, 0, 0, -1.2, 0, 0, 0, 1.2, 0, 0, -1.2 }, f = of, e = edges_of(of), orient = true } }
  local iv = { -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI, 0, 0, -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI,
               PHI, 0, -1, PHI, 0, 1, -PHI, 0, -1, -PHI, 0, 1 }
  local ifc = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 }
  for i = 1, #ifc do ifc[i] = ifc[i] + 1 end
  SOLIDS[4] = { name = "ICOSA", mode = "both", r = 230, g = 90, b = 255,
    m = px.model{ v = scaled(iv, 0.62), f = ifc, e = edges_of(ifc), orient = true } }
end

-- stars: fixed places and speeds, from a xorshift (the simulator and the panel agree)
local seed = 0x51A7B00B
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end
local SX, SY, SV, SB = {}, {}, {}, {}
for i = 1, 60 do SX[i], SY[i], SV[i], SB[i] = rnd() * W, rnd() * H, 0.6 + rnd() * 2.4, 60 + floor(rnd() * 150) end

local cur, SCENE, XF = 1, 20, 1.5
local T, tprev, sceneAt, xfAt = 0, nil, 0, nil
local LINES_SLOT, FADE_SLOT = 3, 2
local lastClicks = rawget(px, "button") and px.button() or 0
local OPTS = { ax = 0, ay = 0, az = 0, scale = 21, x = 63.5, y = 31.5, dist = 4, r = 255, g = 255, b = 255, mode = "wire" }

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
  if T > 3600 then T = T - 3600; sceneAt = sceneAt - 3600; if xfAt then xfAt = xfAt - 3600 end end

  if not HAS then
    px.clear(0, 0, 0)
    px.text(2, 28, "NEEDS FIRMWARE 2.7.6", 120, 120, 120)
    return
  end

  local c = rawget(px, "button") and px.button() or 0
  local change = c ~= lastClicks or T - sceneAt > SCENE
  if c ~= lastClicks then lastClicks = c end
  if change then
    cur = cur % #SOLIDS + 1
    sceneAt = T
    LINES_SLOT, FADE_SLOT = FADE_SLOT, LINES_SLOT      -- last frame flows out under the next solid
    xfAt = T
  end

  -- the stars, gliding left at fractions of a pixel a frame
  px.clear(0, 0, 4)
  px.mode("add")
  for i = 1, #SX do
    SX[i] = SX[i] - SV[i] * dt
    if SX[i] < -2 then SX[i] = W + 1; SY[i] = rnd() * H end
    px.dot(SX[i], SY[i], SB[i], SB[i], SB[i] + 20)
  end
  px.mode("set")

  if xfAt then
    local u = (T - xfAt) / XF
    if u >= 1 then xfAt = nil else px.mix(FADE_SLOT, 1 - u * u * (3 - 2 * u)) end
  end

  local s = SOLIDS[cur]
  OPTS.ax = T * (0.45 + 0.15 * sin(T * 0.11))
  OPTS.ay = T * (0.62 + 0.2 * sin(T * 0.07 + 1))
  OPTS.az = 0.4 * sin(T * 0.13)
  OPTS.scale = 21 + 3 * sin(T * 0.3)
  OPTS.x = 63.5 + 18 * sin(T * 0.21)
  OPTS.y = 31.5 + 5 * sin(T * 0.17 + 2)
  local hue = 0.5 + 0.5 * sin(T * 0.09)
  OPTS.r = floor(s.r * (0.7 + 0.3 * hue)); OPTS.g = floor(s.g * (1 - 0.3 * hue)); OPTS.b = s.b
  OPTS.mode = s.mode
  if s.mode == "wire" then px.mode("add") end
  px.mesh(s.m, OPTS)
  px.mode("set")
  px.save(LINES_SLOT)
end

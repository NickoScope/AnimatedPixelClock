-- @upload-only
-- ============================================================
-- SOLIDS - the Platonic solids turning among slow stars
-- ============================================================
-- @name.en Solids
-- @name.ru Многогранники
-- @about.en The Platonic solids (tetrahedron, cube, octahedron, icosahedron) turning among slowly
-- @about.en drifting stars, lit, as wireframes or both. Every 20 s by the clock, the same on every panel,
-- @about.en the solid on screen reshapes itself into the next.
-- @about.ru Платоновы тела (тетраэдр, куб, октаэдр, икосаэдр) вращаются среди медленно плывущих звёзд:
-- @about.ru освещённые, каркасом или и так и так. Каждые 20 с по часам, одинаково на всех панелях, фигура
-- @about.ru на экране перетекает в следующую.
-- @control.en knob press: Next solid now (if one is already reshaping, right after it); it holds until the next change by the clock.
-- @control.ru knob press: Сразу следующая фигура (если смена уже идёт, то сразу после неё); она держится до ближайшей смены по часам.
-- @function.en 4 solids, 20 s each, by the clock: the same on every panel
-- @function.en No clock
-- @function.en Needs firmware 2.7.6 or later
-- @function.ru 4 фигуры по 20 с, по часам: одинаково на всех панелях
-- @function.ru Часов нет
-- @function.ru Нужна прошивка 2.7.6 или новее
--
-- A tetrahedron, a cube, an octahedron and an icosahedron, one at a time,
-- turning on a spin that wanders; shown as a lit solid, as a wireframe with
-- anti-aliased edges, or as both. Every 20 s the one on screen reshapes itself
-- into the next: its surface swells and settles, point by point, until it is
-- the new solid, which then takes on its own edges (see "the morph"). Behind them a field of stars drifts at fractions of a pixel a
-- frame, each star an anti-aliased dot, so it glides instead of stepping. The
-- button brings the next solid now.
--
-- The solid is read off the wall clock, so two panels (or a panel and its
-- twin) show the same one at the same moment: px.t() is the phase of the
-- 80 s PERIOD aligned to the epoch, and its 20 s quarters are the solids. A
-- press moves one solid on from the clock's (the presses are counted from
-- the moment the effect opened); the solid it brings holds until the
-- clock's next 20 s boundary, anything from 0 to 20 s. The change itself is
-- timed in clock seconds: on the clock it starts at the boundary, so every
-- panel is at the same point of the morph, and a panel opened in the middle
-- of one joins it there. A press while a change is under way is kept, not
-- lost, and its change follows the one under way: a press that one panel
-- took and another dropped would leave the two a solid apart for good.
--
-- The firmware turns, projects, sorts and draws each solid in one call
-- (px.model once, px.mesh a frame); the script only chooses where it points.
--
-- Needs firmware 2.7.6 or later (px.model, px.mesh, px.dot; px.mix 2.7.5).
-- ============================================================
PERIOD = 80.0                 -- 4 solids x 20 s: the solid is read off the clock
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
  local tv = scaled({ 1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, 1 }, 0.66)
  SOLIDS[1] = { name = "TETRA", mode = "solid", r = 255, g = 120, b = 60, v = tv, f = tf,
    m = px.model{ v = tv, f = tf, e = edges_of(tf), orient = true } }
  local cv = scaled({ -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1 }, 0.72)
  local cf = { 1, 3, 2, 1, 4, 3, 5, 6, 7, 5, 7, 8, 1, 2, 6, 1, 6, 5, 4, 8, 7, 4, 7, 3, 1, 5, 8, 1, 8, 4, 2, 3, 7, 2, 7, 6 }
  SOLIDS[2] = { name = "CUBE", mode = "both", r = 60, g = 180, b = 255, v = cv, f = cf,
    m = px.model{ v = cv, e = { 1, 2, 2, 3, 3, 4, 4, 1, 5, 6, 6, 7, 7, 8, 8, 5, 1, 5, 2, 6, 3, 7, 4, 8 }, f = cf, orient = true } }
  local of = { 5, 1, 3, 5, 3, 2, 5, 2, 4, 5, 4, 1, 6, 3, 1, 6, 2, 3, 6, 4, 2, 6, 1, 4 }
  local ov = { 1.2, 0, 0, -1.2, 0, 0, 0, 1.2, 0, 0, -1.2, 0, 0, 0, 1.2, 0, 0, -1.2 }
  SOLIDS[3] = { name = "OCTA", mode = "wire", r = 120, g = 255, b = 140, v = ov, f = of,
    m = px.model{ v = ov, f = of, e = edges_of(of), orient = true } }
  local iv = { -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI, 0, 0, -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI,
               PHI, 0, -1, PHI, 0, 1, -PHI, 0, -1, -PHI, 0, 1 }
  local ifc = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 }
  for i = 1, #ifc do ifc[i] = ifc[i] + 1 end
  local iv2 = scaled(iv, 0.62)
  SOLIDS[4] = { name = "ICOSA", mode = "both", r = 230, g = 90, b = 255, v = iv2, f = ifc,
    m = px.model{ v = iv2, f = ifc, e = edges_of(ifc), orient = true } }
end

-- ---------------------------------------------------------------- the morph
-- One skin for all of them: a sphere of 162 points and 320 triangles (an
-- icosahedron divided twice). For each solid, every point of the skin is
-- carried out along its ray from the centre to the solid's surface. Changing
-- solid, each point slides from where the old one put it to where the new one
-- does, so a cube swells into an icosahedron and settles into a tetrahedron;
-- when it has arrived, the skin flows into the real solid with its edges.
local SKV, SKF = {}, {}
if HAS then
  local verts, faces, mid = {}, {}, {}
  local iv = { -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI, 0, 0, -1, PHI, 0, 1, PHI, 0, -1, -PHI, 0, 1, -PHI,
               PHI, 0, -1, PHI, 0, 1, -PHI, 0, -1, -PHI, 0, 1 }
  local ifc = { 1, 12, 6, 1, 6, 2, 1, 2, 8, 1, 8, 11, 1, 11, 12, 2, 6, 10, 6, 12, 5, 12, 11, 3, 11, 8, 7, 8, 2, 9,
                4, 10, 5, 4, 5, 3, 4, 3, 7, 4, 7, 9, 4, 9, 10, 5, 10, 6, 3, 5, 12, 7, 3, 11, 9, 7, 8, 10, 9, 2 }
  local function add(x, y, z)
    local l = sqrt(x * x + y * y + z * z)
    verts[#verts + 1] = { x / l, y / l, z / l }
    return #verts
  end
  for i = 1, #iv, 3 do add(iv[i], iv[i + 1], iv[i + 2]) end
  for i = 1, #ifc, 3 do faces[#faces + 1] = { ifc[i], ifc[i + 1], ifc[i + 2] } end
  local function midpoint(a, b)
    local key = a < b and a * 1000 + b or b * 1000 + a
    if mid[key] then return mid[key] end
    local p, q = verts[a], verts[b]
    local m = add(p[1] + q[1], p[2] + q[2], p[3] + q[3])
    mid[key] = m
    return m
  end
  for _ = 1, 2 do
    local nf = {}
    for _, f in ipairs(faces) do
      local a, b, c = f[1], f[2], f[3]
      local ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
      nf[#nf + 1] = { a, ab, ca }; nf[#nf + 1] = { b, bc, ab }; nf[#nf + 1] = { c, ca, bc }; nf[#nf + 1] = { ab, bc, ca }
    end
    faces = nf
  end
  for _, f in ipairs(faces) do SKF[#SKF + 1] = f[1]; SKF[#SKF + 1] = f[2]; SKF[#SKF + 1] = f[3] end
  -- each solid's planes (outward normal n, offset d), then each ray's distance
  for si, sd in ipairs(SOLIDS) do
    local planes, v, f = {}, sd.v, sd.f
    for i = 1, #f, 3 do
      local a, b, c = (f[i] - 1) * 3, (f[i + 1] - 1) * 3, (f[i + 2] - 1) * 3
      local ux, uy, uz = v[b + 1] - v[a + 1], v[b + 2] - v[a + 2], v[b + 3] - v[a + 3]
      local wx, wy, wz = v[c + 1] - v[a + 1], v[c + 2] - v[a + 2], v[c + 3] - v[a + 3]
      local nx, ny, nz = uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx
      local l = sqrt(nx * nx + ny * ny + nz * nz)
      nx, ny, nz = nx / l, ny / l, nz / l
      local d = nx * v[a + 1] + ny * v[a + 2] + nz * v[a + 3]
      if d < 0 then nx, ny, nz, d = -nx, -ny, -nz, -d end
      planes[#planes + 1] = { nx, ny, nz, d }
    end
    local out = {}
    for _, p in ipairs(verts) do
      local r = 1e9
      for _, pl in ipairs(planes) do
        local k = pl[1] * p[1] + pl[2] * p[2] + pl[3] * p[3]
        if k > 1e-6 and pl[4] / k < r then r = pl[4] / k end
      end
      out[#out + 1] = p[1] * r; out[#out + 1] = p[2] * r; out[#out + 1] = p[3] * r
    end
    -- the corners: 162 points would round them off, so the skin point nearest
    -- each vertex's direction is put on the vertex itself
    for i = 1, #v, 3 do
      local x, y, z = v[i], v[i + 1], v[i + 2]
      local l = sqrt(x * x + y * y + z * z)
      local best, bi = -2, 1
      for j, p in ipairs(verts) do
        local d = (p[1] * x + p[2] * y + p[3] * z) / l
        if d > best then best, bi = d, j end
      end
      out[bi * 3 - 2], out[bi * 3 - 1], out[bi * 3] = x, y, z
    end
    SKV[si] = out
  end
end
local MORPH, SETTLE = 2.4, 0.6        -- seconds the skin slides, and flows into the solid
local MV = {}                         -- the skin's points this frame
local SKIN = { v = MV, f = SKF, orient = true }
local SKINOPTS = { ax = 0, ay = 0, az = 0, scale = 21, x = 63.5, y = 31.5, dist = 4, r = 255, g = 255, b = 255, mode = "solid" }

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

local SCENE = 20
local T, tprev = 0, nil
-- the solid on screen (or the one it is changing into), the one it changes
-- from, and the clock's second (px.t() * PERIOD) the change began: nil when
-- none is under way. A change is the morph (MORPH) and then the settle (SETTLE).
local cur, from, changeAt = nil, nil, nil
local skinDrawn, saved = false, false   -- this panel drew the change's skin; saved its last frame
local FADE_SLOT = 2
-- px.button() counts from boot, not from here: the presses since the effect opened
local base = rawget(px, "button") and px.button() or 0
local seen = base
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
  if T > 3600 then T = T - 3600 end   -- a 32-bit float T, held under an hour

  if not HAS then
    px.clear(0, 0, 0)
    px.text(2, 28, "NEEDS FIRMWARE 2.7.6", 120, 120, 120)
    return
  end

  -- the solid: the clock's 20 s quarter of the PERIOD, moved on by the presses
  local c = rawget(px, "button") and px.button() or 0
  local slot = floor(t / SCENE)                      -- 0..3, the same on every panel with NTP
  local s = t - slot * SCENE                         -- clock seconds into the quarter
  local want = (slot + c - base) % #SOLIDS + 1
  local age = changeAt and (t - changeAt) % PERIOD   -- clock seconds into the change, across the wrap
  local over = nil                                   -- clock seconds since a change ended this frame
  if age and age >= MORPH + SETTLE then
    over = age - MORPH - SETTLE
    changeAt, age = nil, nil
  end
  if not cur then
    cur = want
    if s < MORPH + SETTLE then                       -- opened in the clock's change: join it there
      from, changeAt, age = (want - 2) % #SOLIDS + 1, t - s, s
    end
  elseif not changeAt and want ~= cur then
    -- due from the press, else from the boundary or from the end of the change
    -- that held it back, whichever came later: every panel at the same point
    local due = s
    if c ~= seen then due = 0 elseif over and over < s then due = over end
    from, cur = cur, want
    changeAt, age = (t - due) % PERIOD, due
    skinDrawn, saved = false, false
  end
  seen = c

  -- the stars, gliding left at fractions of a pixel a frame
  px.clear(0, 0, 4)
  px.mode("add")
  for i = 1, #SX do
    SX[i] = SX[i] - SV[i] * dt
    if SX[i] < -2 then SX[i] = W + 1; SY[i] = rnd() * H end
    px.dot(SX[i], SY[i], SB[i], SB[i], SB[i] + 20)
  end
  px.mode("set")

  if age and age >= MORPH and saved then            -- the settle: the skin's last frame flows out
    local u = (age - MORPH) / SETTLE
    px.mix(FADE_SLOT, 1 - u * u * (3 - 2 * u))
  end

  local sd = SOLIDS[cur]
  OPTS.ax = T * (0.45 + 0.15 * sin(T * 0.11))
  OPTS.ay = T * (0.62 + 0.2 * sin(T * 0.07 + 1))
  OPTS.az = 0.4 * sin(T * 0.13)
  OPTS.scale = 21 + 3 * sin(T * 0.3)
  OPTS.x = 63.5 + 18 * sin(T * 0.21)
  OPTS.y = 31.5 + 5 * sin(T * 0.17 + 2)
  local hue = 0.5 + 0.5 * sin(T * 0.09)
  local function colour(sd, o)
    o.r = floor(sd.r * (0.7 + 0.3 * hue)); o.g = floor(sd.g * (1 - 0.3 * hue)); o.b = sd.b
  end

  if age and (age < MORPH or (skinDrawn and not saved)) then
    -- the skin, sliding from the old solid's shape to the new one's
    local u = age / MORPH
    if u >= 1 then u = 1 end
    local e = u * u * (3 - 2 * u)
    local A, B = SKV[from], SKV[cur]
    for i = 1, #A do MV[i] = A[i] + (B[i] - A[i]) * e end
    local M = px.model(SKIN)
    for k, v in pairs(OPTS) do SKINOPTS[k] = v end
    local a, b = SOLIDS[from], SOLIDS[cur]
    SKINOPTS.r = floor((a.r + (b.r - a.r) * e) * (0.7 + 0.3 * hue))
    SKINOPTS.g = floor((a.g + (b.g - a.g) * e) * (1 - 0.3 * hue))
    SKINOPTS.b = floor(a.b + (b.b - a.b) * e)
    SKINOPTS.mode = "solid"
    px.mesh(M, SKINOPTS)
    skinDrawn = true
    if u >= 1 then
      -- arrived: this frame flows out under the real solid
      px.save(FADE_SLOT)
      saved = true
    end
    return
  end

  colour(sd, OPTS)
  OPTS.mode = sd.mode
  if sd.mode == "wire" then px.mode("add") end
  px.mesh(sd.m, OPTS)
  px.mode("set")
end

-- @upload-only
-- AQUARIUM - a planted tank with depth, and fish that notice you slowly.
--
-- Seven species: a goldfish and an angelfish, tiger barbs, guppies, a shoal of
-- neons that keeps formation, a corydoras working the sand and a snail on the
-- glass. A lamp over the tank, wood and stone on the bottom, three kinds of
-- plant, and an airstone in the corner.
--
-- **The tank has depth, and the fish swim through it.** Every fish carries a
-- `depth` from 0 at the back glass to 1 at the front, drifting on its own slow
-- cycle of twenty to forty seconds. Depth is not a label: it picks the body
-- from five precomputed sizes, sets how much water is hazing the colour, sets
-- how fast the fish appears to move, and decides what is drawn over what -
-- the whole shoal is sorted back to front every frame. A fish swimming toward
-- you grows, sharpens and speeds up, and passes in front of the ones behind.
--
-- **A turn is a turn, not a flip.** `face` crosses zero over about a third of
-- a second, and the body foreshortens as it goes, so a fish banks through the
-- turn and is briefly seen end-on.
--
-- The room's radar is the one ROOM RADAR draws - an Apollo MTR-1 over MQTT from
-- Home Assistant, bound into Lua as `presence`. Every reaction is damped in
-- SECONDS, not frames: about six to turn your way, a second and a half to
-- scatter from a fast movement, half a minute to settle, forty-five of an empty
-- room before the tank dims and sleeps. A still person's reading jitters by
-- centimetres, so anything answering frame by frame would twitch. The room
-- bends a fish's own route by 18% and never becomes the route, and there is
-- glass on all four sides - this is a tank, not a window on the sea.
--
-- WHERE THE BYTES WENT. The panel resolves far less colour than a file can
-- carry (effect_api: each extra BCM bit halves the refresh, and a CIE table
-- folds 256 inputs into 174 outputs), so none of the extra budget went into
-- colour. It went into things that move: depth, a shoal, a bottom dweller,
-- three kinds of plant, shadows on the sand and caustics walking across it.
--
-- A frame costs calls into C, not the work inside them - a filled rect is one
-- call and paints 128 pixels. About 1,200 a frame.
--
-- The canvas is never cleared between frames, so every layer is repainted in
-- order and nothing needs erasing.
--
-- WHAT STANDS STILL IS DRAWN ONCE (firmware 2.7.0, px.save/px.restore). The
-- water's gradient, the sand bed, the stones and the moss ball do not move;
-- they are drawn once, saved, and every later frame starts from that picture.
-- They depend only on how lit the tank is, and that changes over tens of
-- seconds, so the picture is made again when the light moves a step. On
-- firmware without the helpers the tank draws everything every frame, as
-- before. The branch stays in the moving layer: the light shafts pass behind
-- it.

PERIOD = 60.0
FPS = 15

local floor, sin, cos, sqrt = math.floor, math.sin, math.cos, math.sqrt
local max, abs, sort = math.max, math.abs, table.sort
local W, H = 128, 64

-- LUA_32BITS: lua_Integer is int32, so the textbook LCG overflows and collapses.
local seed = 0x1a2b3c4d
local function rnd()
  seed = seed ~ ((seed << 13) & 0x7fffffff)
  seed = seed ~ (seed >> 17)
  seed = seed ~ ((seed << 5) & 0x7fffffff)
  return (seed & 0x7fffffff) / 2147483647.0
end

-- ── water and light ─────────────────────────────────────────────────────────
local SURF = { 46, 140, 152 }
local MID  = { 16,  78, 104 }
local DEEP = {  6,  34,  56 }
local FLOOR_Y = 52

local function water(y, lift)
  local r, g, b
  if y < 20 then
    local k = y / 20
    r = SURF[1] + (MID[1] - SURF[1]) * k
    g = SURF[2] + (MID[2] - SURF[2]) * k
    b = SURF[3] + (MID[3] - SURF[3]) * k
  else
    local k = (y - 20) / 32
    if k > 1 then k = 1 end
    r = MID[1] + (DEEP[1] - MID[1]) * k
    g = MID[2] + (DEEP[2] - MID[2]) * k
    b = MID[3] + (DEEP[3] - MID[3]) * k
  end
  return r * lift, g * lift, b * lift
end

-- ── the species ─────────────────────────────────────────────────────────────
-- shape: 1 fusiform, 2 deep-bodied, 3 disc, 4 flat-bellied bottom dweller.
local SPECIES = {
  gold = {
    len = 15, tall = 10, shape = 2, speed = 0.62,
    back = { 236,  74,   0 }, mid = { 255, 150,  16 }, belly = { 255, 226, 150 },
    fin  = { 255, 176,  48 }, tail = "veil", sheen = { 255, 236, 190 },
  },
  angel = {
    len = 12, tall = 13, shape = 3, speed = 0.5,
    back = { 196, 200, 210 }, mid = { 240, 238, 228 }, belly = { 255, 250, 226 },
    fin  = { 255, 226, 120 }, tail = "fork", bars = { 0.18, 0.52, 0.86 },
    barcol = { 26, 28, 40 }, filament = true, sheen = { 255, 244, 190 },
  },
  -- The tiger barb: amber gold, four black bars, red fins.
  barb = {
    len = 11, tall = 7, shape = 1, speed = 1.0,
    back = { 236, 132,  16 }, mid = { 255, 190,  52 }, belly = { 255, 236, 176 },
    fin  = { 255,  62,  40 }, tail = "fork", bars = { 0.2, 0.46, 0.72, 0.95 },
    barcol = { 22, 20, 26 }, sheen = { 255, 224, 130 },
  },
  guppy = {
    len = 6, tall = 4, shape = 1, speed = 1.25,
    back = { 108, 110, 230 }, mid = { 90, 226, 236 }, belly = { 226, 250, 250 },
    fin  = { 255, 92, 180 }, tail = "veil",
  },
  neon = {
    len = 6, tall = 3, shape = 1, speed = 1.35, shoal = true,
    back = { 22, 56, 110 }, mid = { 40, 214, 255 }, belly = { 244, 44, 52 },
    fin  = { 180, 220, 245 }, tail = "fork",
  },
  -- The corydoras works the sand and never leaves it: flat underneath, humped
  -- over the shoulders, and it shuffles rather than cruises.
  cory = {
    len = 10, tall = 6, shape = 4, speed = 0.55, bottom = true,
    back = { 96, 104, 96 }, mid = { 168, 172, 150 }, belly = { 232, 230, 206 },
    fin  = { 206, 206, 182 }, tail = "fork", sheen = { 240, 238, 214 },
  },
}

local GUPPY_FIN = { { 255, 84, 186 }, { 255, 190, 40 }, { 90, 255, 150 } }

-- Five body sizes per species, built once. A fish further back is a SMALLER
-- fish, not a dimmer one - scaling only the colour is what makes a flat
-- picture, and it is the single thing that gives a 128x64 tank depth.
local NS = 5
local SCALE = { 0.62, 0.78, 0.94, 1.10, 1.26 }

local function profile(sp, s)
  local n = max(4, floor(sp.len * s))
  local p, tall = {}, sp.tall * s
  for i = 0, n - 1 do
    local u = i / (n - 1)
    local v
    if sp.shape == 3 then
      v = u < 0.3 and sqrt(u / 0.3) or (u < 0.62 and 1 or (1 - (u - 0.62) / 0.38) * 0.95)
    elseif sp.shape == 2 then
      v = u < 0.4 and sqrt(u / 0.4) or (1 - (u - 0.4) / 0.6) * (1 - 0.35 * (u - 0.4) / 0.6)
    elseif sp.shape == 4 then
      v = u < 0.22 and sqrt(u / 0.22) or (1 - (u - 0.22) / 0.78) * (1 - 0.35 * (u - 0.22) / 0.78)
    else
      v = u < 0.34 and sqrt(u / 0.34) or (1 - (u - 0.34) / 0.66) * (1 - 0.55 * (u - 0.34) / 0.66)
    end
    if v < 0 then v = 0 end
    p[i] = tall * 0.5 * v
  end
  return p, n
end

for _, sp in pairs(SPECIES) do
  sp.prof, sp.plen = {}, {}
  for i = 1, NS do sp.prof[i], sp.plen[i] = profile(sp, SCALE[i]) end
end

-- ── the inhabitants ─────────────────────────────────────────────────────────
local FISH = {}
local function spawn(name, n)
  local sp = SPECIES[name]
  for i = 1, n do
    FISH[#FISH + 1] = {
      sp = sp, name = name,
      x = 20 + rnd() * (W - 40), y = 16 + rnd() * 22,
      vx = (rnd() < 0.5 and -1 or 1) * 3, vy = 0,
      face = rnd() < 0.5 and -1 or 1,
      -- Its own journey through the tank: a phase and a period of 20-40 s.
      dep = rnd(), dph = rnd() * 6.28, dsp = 0.16 + rnd() * 0.16,
      ph = rnd() * 6.28, wob = 0.4 + rnd() * 0.9,
      wx = 32 + rnd() * (W - 64), wy = 16 + rnd() * 20,
      rx = 14 + rnd() * 22, tw = rnd() * 6.28,
      var = GUPPY_FIN[(i - 1) % 3 + 1],
      shuffle = rnd() * 6.28,
    }
  end
end

-- Halved on the owner's eye, 2026-09-22: eight neons and four guppies filled
-- the tank with specks and the big fish stopped being the subject. A shoal
-- reads as a shoal at four.
spawn("neon", 4)
spawn("guppy", 2)
spawn("barb", 4)
spawn("cory", 2)
spawn("angel", 1)
spawn("gold", 1)

local ORDER = {}
for i = 1, #FISH do ORDER[i] = FISH[i] end
local function byDepth(a, b) return a.dep < b.dep end
local sortIn = 0

-- ── the hardscape ───────────────────────────────────────────────────────────
local DUNE = {}
for x = 0, W - 1 do
  DUNE[x] = floor(FLOOR_Y + 1.7 * sin(x * 0.07) + 1.1 * sin(x * 0.19 + 2.0)
                  + 0.7 * sin(x * 0.41 + 1.0))
end

-- A branch, because a silhouette behind the leaves is most of what gives depth.
local WOOD = {}
do
  local bx, by = 88, DUNE[88]
  local ang, len = -1.15, 15
  for i = 1, 5 do
    local nx = bx + cos(ang) * len
    local ny = by + sin(ang) * len
    WOOD[#WOOD + 1] = { bx, by, nx, ny, max(1, 3 - i // 2) }
    if i == 2 then WOOD[#WOOD + 1] = { nx, ny, nx + 13, ny - 4, 1 } end
    bx, by, ang, len = nx, ny, ang + 0.42, len * 0.72
  end
end

-- The dune as RUNS of equal height, not 128 columns. The bed is a sum of three
-- slow sines, so neighbours share a height for four or five columns at a time:
-- drawing it column by column cost 256 calls a frame and dropped the panel
-- from 15 fps to 12. This is the same picture in about 50.
local RUNS = {}
do
  local x0 = 0
  for x = 1, W do
    if x == W or DUNE[x] ~= DUNE[x0] then
      RUNS[#RUNS + 1] = { x0, x - x0, DUNE[x0] }
      x0 = x
    end
  end
end

local STONE = {}
do
  local s = 0x5f3a91
  for i = 1, 7 do
    s = (s * 1103515 + 12345) & 0x7fffffff
    local x = 6 + s % (W - 12)
    STONE[i] = { x = x, y = DUNE[x] + 1, r = 1 + (s >> 11) % 3 }
  end
end

-- ── plants ──────────────────────────────────────────────────────────────────
-- Three kinds, because one kind repeated is a pattern and three are a tank.
local RIBBON, BUSH = {}, {}
for i = 1, 9 do
  RIBBON[i] = { x = 3 + (i - 1) * 14 + rnd() * 7, h = 20 + rnd() * 24,
                ph = rnd() * 6.28, sway = 0.7 + rnd() * 0.9,
                hue = rnd() < 0.4 and 1 or 0 }
end
for i = 1, 4 do
  BUSH[i] = { x = (i <= 2) and (6 + rnd() * 16) or (W - 22 + rnd() * 16),
              h = 9 + rnd() * 7, ph = rnd() * 6.28, n = 7 + floor(rnd() * 4) }
end
local MOSS_X, MOSS_R = 36, 5

-- ── the airstone ────────────────────────────────────────────────────────────
local STONE_X = 16
local BUB = {}
for i = 1, 22 do
  BUB[i] = { y = 54 + rnd() * 50, ox = (rnd() - 0.5) * 3,
             v = 9 + rnd() * 9, r = rnd() < 0.3 and 1 or 0, ph = rnd() * 6.28 }
end
-- Pearling: oxygen collecting on a leaf tip and letting go, at a tenth the
-- speed of the airstone. It is the detail that says the plants are alive.
local PEARL = {}
for i = 1, 8 do
  PEARL[i] = { rib = RIBBON[1 + floor(rnd() * 9)], t = rnd(),
               v = 0.06 + rnd() * 0.10, y = 0, x = 0, up = false }
end

-- ── the room ────────────────────────────────────────────────────────────────
local RAD = rawget(_G, "presence")
local RANGE = RAD and (RAD.scale() * 1000) or 4000

local focus, haveF, near, startle, awake = 0.5, 0, 0, 0, 1

local function lp(v, to, dt, tau)
  local k = dt / tau
  if k > 1 then k = 1 end
  return v + (to - v) * k
end

local function read_room(dt)
  local fx, fy, fastest = 0, 0, 0
  local n = RAD.count()
  local best = 1e9
  for i = 1, 3 do
    local x, y, v = RAD.target(i)
    if x then
      if v and abs(v) > fastest then fastest = abs(v) end
      if y < best then best, fx, fy = y, x, y end
    end
  end

  if n > 0 then
    local tx = 0.5 + (fx / RANGE)
    if tx < 0 then tx = 0 elseif tx > 1 then tx = 1 end
    local p = 1 - (fy / RANGE)
    if p < 0 then p = 0 elseif p > 1 then p = 1 end
    focus = lp(focus, tx, dt, 6.0)
    haveF = lp(haveF, 1, dt, 3.0)
    near = lp(near, p, dt, 8.0)
    awake = lp(awake, 1, dt, 12.0)
  else
    haveF = lp(haveF, 0, dt, 9.0)
    near = lp(near, 0, dt, 12.0)
    awake = lp(awake, 0, dt, 45.0)
  end

  -- 40 cm/s is a walk, not a fidget, and a still person's jitter sits below it.
  startle = fastest > 40 and lp(startle, 1, dt, 1.5) or lp(startle, 0, dt, 25.0)
end

local function story(dt, t)
  focus = 0.5 + 0.34 * sin(t * 0.21)
  haveF = 0.45 + 0.35 * sin(t * 0.07 + 1.0)
  near = 0.35 + 0.3 * sin(t * 0.05)
  awake = 1
  startle = max(0, startle - dt * 0.05)
end

-- ── drawing ─────────────────────────────────────────────────────────────────
local pixel, line, rect, circle, text = px.pixel, px.line, px.rect, px.circle, px.text

local function draw_water(t, lift)
  for i = 0, 31 do
    local y = i * 2
    local r, g, b = water(y, lift)
    rect(0, y, W, 2, floor(r), floor(g), floor(b), true)
  end
end

local function draw_lamp(t, lift)
  rect(0, 0, W, 1, floor(36 * lift), floor(38 * lift), floor(42 * lift), true)
  for i = 0, 7 do
    local x = 6 + i * 16
    local k = lift * (0.8 + 0.2 * sin(t * 0.6 + i))
    rect(x, 0, 10, 1, floor(220 * k), floor(232 * k), floor(200 * k), true)
  end
  rect(0, 1, W, 2, floor(90 * lift), floor(170 * lift), floor(170 * lift), true)
end

local function draw_shafts(t, lift)
  for s = 1, 4 do
    local base = 12 + (s - 1) * 33 + 9 * sin(t * 0.13 + s)
    local lean = 0.4 + 0.25 * sin(t * 0.11 + s * 2.1)
    for b = 0, 14 do                       -- to y 48: the sand bed starts below
      local y = 3 + b * 3
      local w = 3 + b * 0.6
      local x = base + y * lean - w * 0.5
      local k = (1 - b / 16) * 0.55 * lift * (0.6 + 0.4 * sin(t * 0.3 + s))
      local r, g, bl = water(y, lift)
      rect(floor(x), y, floor(w), 3,
           floor(r + 46 * k), floor(g + 72 * k), floor(bl + 58 * k), true)
    end
  end
end

local function draw_caustics(t, lift)
  -- Two sines of different wavelength crossing, so the net drifts and never
  -- repeats on the eye. Brightness dies with depth, because light does.
  for c = 0, 7 do                          -- the last row fell on the sand, which hid it
    local y0 = 4 + c * 6
    local fade = 1 - y0 / 56
    if fade > 0 then
      local amp = 1.5 + 1.4 * fade
      local px0, py0
      for i = 0, 8 do
        local x = i * 16
        local y = y0 + amp * sin(x * 0.11 + t * 0.9 + c * 0.7)
                     + amp * 0.6 * sin(x * 0.047 - t * 0.55 + c * 1.9)
        local yi = floor(y)
        if px0 then
          local k = fade * fade * lift * (0.55 + 0.45 * sin(t * 1.3 + c + i * 0.4))
          local wr, wg, wb = water(yi, lift)
          line(px0, py0, floor(x), yi,
               floor(wr + 110 * k), floor(wg + 140 * k), floor(wb + 110 * k))
        end
        px0, py0 = floor(x), yi
      end
    end
  end
end

local function draw_surface(t, lift)
  -- Nothing here may be darker than the water: near the surface a dark mark
  -- reads as a hole, not a shadow.
  for i = 0, 15 do
    local x = i * 8 + floor(5 * sin(t * 0.7 + i))
    local y = 3 + floor(1.4 * sin(t * 1.5 + i * 0.9))
    local k = lift * (0.55 + 0.45 * sin(t * 2.1 + i))
    local len = 3 + floor(3 * abs(sin(t * 0.9 + i * 1.7)))
    line(x, y, x + len, y, floor(90 + 130 * k), floor(180 + 60 * k), floor(190 + 50 * k))
  end
end

local function draw_wood(lift)
  for i = 1, #WOOD do
    local w = WOOD[i]
    local r, g, b = floor(52 * lift), floor(40 * lift), floor(30 * lift)
    for o = 0, w[5] - 1 do
      line(floor(w[1]), floor(w[2]) - o, floor(w[3]), floor(w[4]) - o, r, g, b)
    end
    line(floor(w[1]), floor(w[2]) - w[5], floor(w[3]), floor(w[4]) - w[5],
         floor(88 * lift), floor(70 * lift), floor(50 * lift))
  end
end

local function draw_sand_bed(lift)
  local cr, cg, cb = floor(122 * lift), floor(102 * lift), floor(70 * lift)
  local dr, dg, db = floor(58 * lift), floor(46 * lift), floor(32 * lift)
  for i = 1, #RUNS do
    local r = RUNS[i]
    local top = r[3]
    rect(r[1], top, r[2], 3, cr, cg, cb, true)
    if top + 3 <= H - 1 then
      rect(r[1], top + 3, r[2], H - top - 3, dr, dg, db, true)
    end
  end
  for i = 1, 7 do
    local st = STONE[i]
    circle(st.x, st.y, st.r, floor(92 * lift), floor(84 * lift), floor(72 * lift), true)
    pixel(st.x - st.r, st.y - st.r, floor(138 * lift), floor(126 * lift), floor(106 * lift))
  end
end

local function draw_sand(t, lift)
  -- Caustics land on the bottom too, and that is what sells a lit tank: the
  -- same wave that brightens the water walks bright patches across the sand.
  for i = 0, 15 do
    local x = i * 8 + floor(4 * sin(t * 0.6 + i * 0.9))
    local top = DUNE[x % W]
    local k = lift * (0.5 + 0.5 * sin(t * 1.1 + i * 1.3))
    line(x, top, x + 5, top, floor(150 * k), floor(128 * k), floor(92 * k))
    line(x + 1, top + 1, x + 4, top + 1, floor(110 * k), floor(94 * k), floor(68 * k))
  end
end

local function draw_plants(t, lift, calm)
  for i = 1, 9 do
    local k = RIBBON[i]
    local root = DUNE[floor(k.x) % W]
    local x, y = k.x, root
    local segs = floor(k.h / 2)
    for s = 1, segs do
      local f = s / segs
      local nx = k.x + k.sway * f * f * 8 * sin(t * (0.45 + 0.3 * calm) + k.ph + f * 2.2)
      local ny = root - s * 2
      local g = floor((46 + 82 * f) * lift)
      local r = floor(((k.hue > 0 and 40 or 8) + 22 * f) * lift)
      local b = floor((26 + 30 * f) * lift)
      line(floor(x), floor(y), floor(nx), floor(ny), r, g, b)
      -- One line of weed is a scratch; two are a plant.
      if f < 0.78 then
        line(floor(x) + 1, floor(y), floor(nx) + 1, floor(ny),
             floor(r * 0.68), floor(g * 0.7), floor(b * 0.68))
      end
      x, y = nx, ny
    end
  end
  for i = 1, 4 do
    local b = BUSH[i]
    local root = DUNE[floor(b.x) % W]
    for j = 1, b.n do
      local a = -1.9 + (j - 1) * (1.4 / b.n) + 0.22 * sin(t * 0.5 * calm + b.ph + j)
      local L = b.h * (0.6 + 0.4 * (j % 3) / 2)
      line(floor(b.x), root, floor(b.x + cos(a) * L), floor(root + sin(a) * L),
           floor(14 * lift), floor((70 + 5 * j) * lift), floor(34 * lift))
    end
  end
  -- The moss ball, which does nothing and belongs in every tank (its body is
  -- in the still picture).
  local mr = DUNE[MOSS_X] - MOSS_R + 1
  for j = 1, 7 do
    local a = j * 0.9 + 0.2 * sin(t * 0.4 + j)
    pixel(floor(MOSS_X + cos(a) * MOSS_R), floor(mr + sin(a) * MOSS_R),
          floor(26 * lift), floor(96 * lift), floor(44 * lift))
  end
end

local function draw_bubbles(t, dt, lift)
  local root = DUNE[STONE_X]
  circle(STONE_X, root, 2, floor(60 * lift), floor(66 * lift), floor(70 * lift), true)
  for i = 1, 22 do
    local b = BUB[i]
    b.y = b.y - b.v * dt * (0.6 + 0.4 * awake)
    if b.y < 5 then
      b.y = root - 1
      b.ox = (rnd() - 0.5) * 3
      b.v = 9 + rnd() * 9
    end
    -- The wobble widens as it rises, the way a real one does.
    local climb = (root - b.y) / root
    local x = floor(STONE_X + b.ox + (1.5 + 3.5 * climb) * sin(t * 2.2 + b.ph))
    local y = floor(b.y)
    local k = lift * (0.7 + 0.3 * sin(t * 3 + i))
    if b.r > 0 and climb > 0.3 then
      circle(x, y, 1, floor(170 * k), floor(225 * k), floor(230 * k), false)
      pixel(x, y - 1, floor(230 * k), floor(250 * k), floor(255 * k))
    else
      pixel(x, y, floor(190 * k), floor(232 * k), floor(240 * k))
    end
  end
  for i = 1, 8 do
    local p = PEARL[i]
    local r = p.rib
    local root2 = DUNE[floor(r.x) % W]
    if not p.up then
      p.t = p.t + dt * 0.25
      p.x = r.x + r.sway * 8 * sin(t * 0.5 + r.ph + 2.2)
      p.y = root2 - r.h
      if p.t >= 1 then p.t, p.up = 0, true end
    else
      p.y = p.y - p.v * dt * 26
      if p.y < 5 then p.up = false end
    end
    pixel(floor(p.x), floor(p.y),
          floor(200 * lift), floor(238 * lift), floor(244 * lift))
  end
end

-- One fish. Three vertical segments a column - back, flank, belly - which is
-- what makes a body look round rather than flat.
local function draw_fish(f, t, dt, lift, fx, pull, push)
  local sp = f.sp

  -- Its journey through the tank. The corydoras stays on the bottom, so it
  -- keeps a fixed near-middle depth; everything else drifts.
  if not sp.bottom then
    f.dph = f.dph + dt * f.dsp
    f.dep = 0.5 + 0.48 * sin(f.dph)
  else
    f.dep = 0.55
  end
  local si = 1 + floor(f.dep * (NS - 0.001))
  if si < 1 then si = 1 elseif si > NS then si = NS end
  local prof, len = sp.prof[si], sp.plen[si]
  local tallS = sp.tall * SCALE[si]

  f.tw = f.tw + dt * 0.35 * f.wob
  local wx = f.wx + f.rx * sin(f.tw)
  local wy = f.wy + 8 * sin(f.tw * 0.7 + 1.3)

  -- The room bends this fish's own route by a fraction. It never becomes the
  -- route: making the person the target dragged the whole tank after them.
  local tx = wx + (fx - wx) * pull * 0.18
  local ty = wy - 3 * pull
  if push > 0.02 then
    local away = (f.x < fx) and -1 or 1
    tx = wx + away * 22 * push
    ty = ty + 8 * push
  end

  if sp.shoal then
    -- Neons keep formation: each steers toward the shoal's own drift, which is
    -- what makes eight fish read as one animal.
    tx = tx + 9 * sin(t * 0.33)
    ty = ty + 5 * sin(t * 0.27)
  end

  if sp.bottom then
    -- The corydoras does not cruise. It shuffles: a few centimetres, a pause,
    -- a nose into the sand, and off again.
    f.shuffle = f.shuffle + dt * 0.8
    tx = f.x + (f.face * 26) * (sin(f.shuffle) > 0.35 and 1 or 0)
    ty = DUNE[floor(f.x) % W] - 3
  end

  -- Nearer is faster, as perspective makes it.
  local speed = (0.5 + 0.5 * awake) * (1 + 2.2 * push) * sp.speed * (0.72 + 0.4 * f.dep)
  f.vx = (f.vx + (tx - f.x) * 0.55 * dt * speed) * 0.94
  f.vy = (f.vy + (ty - f.y) * 0.5 * dt * speed) * 0.90
  local vmax = 24 * speed
  if f.vx > vmax then f.vx = vmax elseif f.vx < -vmax then f.vx = -vmax end
  if f.vy > vmax * 0.6 then f.vy = vmax * 0.6 elseif f.vy < -vmax * 0.6 then f.vy = -vmax * 0.6 end
  f.x = f.x + f.vx * dt
  f.y = f.y + f.vy * dt

  -- Glass on all four sides, with the margin being the fish's own half-length
  -- plus its tail, so no fin is clipped either.
  local half = floor(tallS / 2) + 2
  local side = floor(len / 2) + (sp.tail == "veil" and 7 or 5)
  if f.x < side then f.x, f.vx = side, abs(f.vx)
  elseif f.x > W - side then f.x, f.vx = W - side, -abs(f.vx) end
  local top = half + (sp.filament and 8 or 5)
  if f.y < top then f.y, f.vy = top, abs(f.vy) end
  local bed = DUNE[floor(f.x)] - half - (sp.filament and 7 or 1)
  if sp.bottom then bed = DUNE[floor(f.x)] - 2 end
  if f.y > bed then f.y, f.vy = bed, -abs(f.vy) * (sp.bottom and 0.2 or 1) end

  -- A turn is a turn, not a flip: face crosses zero over about a third of a
  -- second and the body foreshortens through it, so the fish banks and is
  -- briefly seen end-on.
  local want = f.vx >= 0 and 1 or -1
  local k = dt / 0.30
  f.face = f.face + (want - f.face) * (k > 1 and 1 or k)
  local dir = f.face >= 0 and 1 or -1
  local fore = abs(f.face)
  if fore < 0.22 then fore = 0.22 end

  f.ph = f.ph + dt * (4 + 9 * push + 5 * abs(f.vx) / 24)

  -- Distance is haze as well as size: the back of the tank is behind more
  -- water, and water is not clear.
  local haze = 0.58 + 0.42 * f.dep
  local br = lift * haze * (0.62 + 0.38 * (1 - f.y / 58))
  local fin = f.name == "guppy" and f.var or sp.fin
  local bk, md, bl = sp.back, sp.mid, sp.belly
  local fr, fg, fb = floor(fin[1] * br), floor(fin[2] * br), floor(fin[3] * br)

  local bx, by = floor(f.x), floor(f.y)
  local half_len = floor(len / 2)

  -- A shadow on the sand for the near half of the tank. Two calls, and most of
  -- what puts a fish IN the water rather than on the glass.
  if f.dep > 0.55 and not sp.bottom then
    local sy = DUNE[floor(f.x)] + 1
    local sw = floor(len * 0.4 * fore)
    if sy < H - 1 and sw > 1 then
      line(bx - sw, sy, bx + sw, sy, floor(34 * lift), floor(28 * lift), floor(20 * lift))
      line(bx - sw + 1, sy + 1, bx + sw - 1, sy + 1,
           floor(46 * lift), floor(38 * lift), floor(26 * lift))
    end
  end

  for c = 0, len - 1 do
    local hh = prof[c]
    if hh >= 0.5 then
      -- The wiggle grows toward the tail; a fish that waggles its face reads
      -- as wrong without being nameable.
      local wig = 0.9 * sin(f.ph - c * 0.55) * (c / len)
      local x = bx + floor(dir * (half_len - c) * fore)
      local yc = by + floor(wig)
      local h = floor(hh)
      local bar = false
      if sp.bars then
        local u = c / (len - 1)
        for j = 1, #sp.bars do
          if abs(u - sp.bars[j]) < 0.055 then bar = true end
        end
      end
      if bar then
        local bc = sp.barcol
        line(x, yc - h, x, yc + h, floor(bc[1] * br), floor(bc[2] * br), floor(bc[3] * br))
      elseif h >= 2 then
        local t1 = floor(h * 0.45)
        line(x, yc - h, x, yc - t1, floor(bk[1] * br), floor(bk[2] * br), floor(bk[3] * br))
        line(x, yc - t1, x, yc + t1, floor(md[1] * br), floor(md[2] * br), floor(md[3] * br))
        line(x, yc + t1, x, yc + h, floor(bl[1] * br), floor(bl[2] * br), floor(bl[3] * br))
      else
        line(x, yc - h, x, yc + h, floor(md[1] * br), floor(md[2] * br), floor(md[3] * br))
      end
    end
  end

  -- Where the lamp catches the back. Only worth it on the nearer half.
  if sp.sheen and f.dep > 0.4 then
    local sh = sp.sheen
    local sr, sg, sb = floor(sh[1] * br), floor(sh[2] * br), floor(sh[3] * br)
    for c = floor(len * 0.3), floor(len * 0.72) do
      local hh = prof[c]
      if hh >= 1.5 then
        local wig = 0.9 * sin(f.ph - c * 0.55) * (c / len)
        pixel(bx + floor(dir * (half_len - c) * fore),
              by + floor(wig) - floor(hh) + 1, sr, sg, sb)
      end
    end
  end

  -- The tail, from where the body actually ends this frame, wiggle included:
  -- detached, it reads as a flag being towed.
  local tb = floor(2.2 * sin(f.ph - 0.55 * (len - 1)))
  local tailx = bx - floor(dir * half_len * fore)
  local taily = by + floor(0.9 * sin(f.ph - (len - 1) * 0.55))
  if sp.tail == "veil" then
    local tl = f.name == "gold" and 6 or 3
    local grow = f.name == "gold" and 0.42 or 0.3
    for s = 1, tl do
      local spread = 0.6 + (tallS * grow) * s / tl
      local yb = taily + tb * s / tl
      local e = s == tl and 1.3 or 1.0
      local er, eg, eb = fr * e, fg * e, fb * e
      line(tailx - dir * s, floor(yb - spread), tailx - dir * s, floor(yb + spread),
           floor(er > 255 and 255 or er), floor(eg > 255 and 255 or eg),
           floor(eb > 255 and 255 or eb))
    end
  else
    for s = 0, 3 do
      local spread = (tallS * 0.3) * s / 3
      local yb = taily + tb * s / 3
      line(tailx - dir * s, floor(yb - spread), tailx - dir * s, floor(yb + spread), fr, fg, fb)
    end
  end

  -- Dorsal and anal. On the angelfish they are most of the animal, and the two
  -- filaments trailing off them are what make it a scalare.
  local wide = floor(len * (sp.shape == 3 and 0.45 or 0.34))
  local dh = floor(prof[wide])
  local dx = bx + floor(dir * (half_len - wide) * fore)
  if sp.shape == 3 then
    for s = 0, 4 do
      local rise = 5 - s
      line(dx - dir * s, by - dh - 1, dx - dir * (s + 1), by - dh - 1 - rise, fr, fg, fb)
      line(dx - dir * s, by + dh + 1, dx - dir * (s + 1),
           by + dh + 1 + floor(rise * 0.7), fr, fg, fb)
    end
    if sp.filament then
      line(dx - dir * 5, by - dh - 6, dx - dir * 9, by - dh - 9, fr, fg, fb)
      line(dx - dir * 5, by + dh + 4, dx - dir * 9, by + dh + 8, fr, fg, fb)
    end
  elseif tallS >= 5 then
    line(dx, by - dh - 1, dx - dir * 2, by - dh - 3, fr, fg, fb)
    if not sp.bottom then
      line(dx, by + dh + 1, dx - dir * 2, by + dh + 2, fr, fg, fb)
    else
      -- Pectorals held out like oars: it rests on them.
      line(dx, by + dh, dx - dir * 3, by + dh + 2, fr, fg, fb)
      line(dx - dir * 2, by + dh, dx - dir * 5, by + dh + 2, fr, fg, fb)
    end
  end

  -- A neon's stripe is the fish: the flank from eye to tail.
  if sp.shoal then
    line(bx + floor(dir * (half_len - 1) * fore), by - 1, tailx, by - 1,
         floor(60 * br), floor(220 * br), floor(250 * br))
  end

  -- An eye. Two pixels are the difference between a fish and a leaf.
  local ex = bx + floor(dir * (half_len - 1) * fore)
  local ey = by - (sp.shape == 3 and 2 or 1)
  pixel(ex, ey, floor(240 * br), floor(242 * br), floor(246 * br))
  if tallS >= 5 then pixel(ex + dir, ey, floor(18 * br), floor(20 * br), floor(26 * br)) end
  if sp.bottom then
    -- Barbels, and they are the whole animal.
    line(ex, ey + 2, ex + dir * 2, ey + 3, floor(210 * br), floor(206 * br), floor(180 * br))
    line(ex, ey + 2, ex + dir * 2, ey + 1, floor(210 * br), floor(206 * br), floor(180 * br))
  end
end

-- ── the snail ───────────────────────────────────────────────────────────────
-- It crosses the sand at about a pixel every two seconds and turns at the edge.
-- Six calls, and it is the slowest thing on the panel.
local snail = { x = 100, dir = -1 }

local function draw_snail(dt, lift)
  snail.x = snail.x + snail.dir * dt * 0.5 * (0.4 + 0.6 * awake)
  if snail.x < 6 then snail.dir = 1 elseif snail.x > W - 6 then snail.dir = -1 end
  local x = floor(snail.x)
  local y = DUNE[x % W] - 1
  circle(x, y - 1, 2, floor(150 * lift), floor(118 * lift), floor(62 * lift), true)
  pixel(x - 1, y - 2, floor(214 * lift), floor(180 * lift), floor(108 * lift))
  local fr, fg, fb = floor(198 * lift), floor(186 * lift), floor(160 * lift)
  line(x + snail.dir * 2, y, x + snail.dir * 4, y, fr, fg, fb)
  pixel(x + snail.dir * 4, y - 2, fr, fg, fb)
  pixel(x + snail.dir * 3, y - 2, fr, fg, fb)
end

-- ── the frame ───────────────────────────────────────────────────────────────
local tprev = nil
local still_lift = nil                     -- the light the saved picture was made at

function draw()
  local t = px.t() * PERIOD
  -- Real seconds between frames, from the only clock the firmware gives us.
  -- Every damping constant is in seconds, so the tank behaves the same at
  -- 15 fps and at 4.
  local dt
  if tprev then
    dt = t - tprev
    if dt < 0 then dt = dt + PERIOD end
    if dt > 0.5 then dt = 0.5 end
  else
    dt = 1 / FPS
  end
  tprev = t

  if RAD and RAD.state() == 1 then read_room(dt) else story(dt, t) end

  local lift = 0.45 + 0.55 * awake + 0.1 * startle
  if lift > 1.12 then lift = 1.12 end
  local calm = 1 - startle

  -- The still picture: made again only when the light has moved a step.
  local lq = floor(lift * 50 + 0.5) / 50
  if not (still_lift == lq and px.restore and px.restore()) then
    draw_water(t, lq)
    draw_sand_bed(lq)
    local mr = DUNE[MOSS_X] - MOSS_R + 1
    circle(MOSS_X, mr, MOSS_R, floor(16 * lq), floor(64 * lq), floor(30 * lq), true)
    if px.save then px.save(); still_lift = lq end
  end
  draw_shafts(t, lift)
  draw_caustics(t, lift)
  draw_lamp(t, lift)
  draw_surface(t, lift)
  draw_wood(lift)
  draw_sand(t, lift)
  draw_plants(t, lift, calm)
  draw_snail(dt, lift)

  -- Back to front. Depth moves over tens of seconds, so re-sorting every frame
  -- costs about ninety comparisons for an order that has not changed; twice a
  -- second is indistinguishable and the list is nearly sorted each time.
  sortIn = sortIn - dt
  if sortIn <= 0 then sort(ORDER, byDepth); sortIn = 0.5 end
  local fx = focus * W
  local pull = haveF * near * (1 - startle)
  for i = 1, #ORDER do draw_fish(ORDER[i], t, dt, lift, fx, pull, startle) end
  draw_bubbles(t, dt, lift)

  -- The time, down on the sand where white has something to sit on.
  local now = px.now()
  local s = string.format("%02d:%02d", now.hour, now.min)
  local tx, ty = W - px.width(s) - 3, H - 8
  text(tx + 1, ty + 1, s, 0, 0, 0)
  text(tx, ty, s, 240, 246, 250)
end

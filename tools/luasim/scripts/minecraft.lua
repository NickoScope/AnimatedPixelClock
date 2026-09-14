-- minecraft.lua - a blocky world with a full day/night cycle in one minute.
--
-- 128x64 divides into 32 x 16 blocks of 4 px. Everything is filled rects: on a
-- panel this coarse, outlines read as noise and solid colour reads as a world.
--
-- Made for an LED panel rather than a monitor: saturated blocks, a sky that is
-- black at night, stone that darkens with depth, and torchlight that lights
-- whole blocks instead of a soft glow. Dim greys all read as black on the
-- panel, so the picture is built from strong colour and real darkness.
--
-- Something is always moving. Steve patrols the left hills with a step cycle,
-- turning at each end and jumping now and then. A creeper paces the ridge and
-- once a minute hisses, flashes and blows up, and is back by dusk. A pig
-- wanders the far shore by day and a zombie by night, and a fish jumps out of
-- the lake every fifteen seconds.

local W, H = px.size()
local B    = 4                       -- block size, px
local COLS, ROWS = W // B, H // B    -- 32 x 16
local floor, sin, cos, pi, max, min, abs = math.floor, math.sin, math.cos, math.pi, math.max, math.min, math.abs

-- ---------------------------------------------------------------- palette
local GRASS    = { 55, 185,  30}
local GRASS_HI = {125, 240,  55}
local DIRT     = {150,  82,  28}
local STONE    = {118, 118, 128}
local STONE_D  = { 72,  72,  84}
local DIAMOND  = { 30, 240, 230}
local GOLD     = {255, 195,   0}
local LOG      = {125,  78,  28}
local LEAF     = { 18, 140,  24}
local LEAF_L   = { 70, 200,  40}
local WATER    = { 10,  85, 240}
local WATER_D  = {  4,  30, 150}
local SAND     = {245, 210, 110}
local CLOUD    = {255, 255, 255}
local CLOUD_2  = {205, 220, 245}
local CLOUD_W  = {255, 175, 130}     -- clouds lit by a low sun

local SKIN, HAIR, EYE = {235, 170, 115}, {80, 45, 10}, {30, 50, 230}
local SHIRT, PANTS    = {0, 200, 210}, {45, 55, 200}
local ZSKIN, ZSHIRT, ZPANTS = {90, 175, 60}, {0, 150, 170}, {90, 40, 170}
local CREEP, CREEP_D  = {80, 225, 70}, {30, 150, 35}
local PIG, PIG_D      = {255, 145, 170}, {215, 80, 115}
local BLACK           = {0, 0, 0}

-- A filled rect in colour c scaled by s, optionally pushed toward white by wt.
local function box(x, y, w, h, c, s, wt)
  local r, g, b = c[1] * s, c[2] * s, c[3] * s
  if wt then r, g, b = r + (255 - r) * wt, g + (255 - g) * wt, b + (255 - b) * wt end
  px.rect(x, y, w, h, min(255, floor(r)), min(255, floor(g)), min(255, floor(b)), true)
end

-- A block lit to s, warmed by wm of torchlight above what the sky gives it.
local function lit_box(x, y, w, h, c, s, wm)
  px.rect(x, y, w, h, min(255, floor(c[1] * s + 90 * wm)), min(255, floor(c[2] * s + 40 * wm)),
          min(255, floor(c[3] * s)), true)
end

local function mix(a, b, k)
  return {a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k, a[3] + (b[3] - a[3]) * k}
end

-- ---------------------------------------------------------------- terrain
-- Fixed, not generated per frame: the world should not shimmer.
local ground, kind = {}, {}
for x = 0, COLS - 1 do
  ground[x] = 10 + floor(1.6 * sin(x * 0.42) + 1.2 * sin(x * 0.17 + 1.3))
  kind[x] = "grass"
end
for x = 19, 24 do ground[x] = 12; kind[x] = "water" end
for _, x in ipairs({18, 25}) do kind[x] = "sand" end

local trees   = { {x = 4, h = 3}, {x = 9, h = 4}, {x = 29, h = 3} }
local torches = { {x = 14}, {x = 27} }
for _, t in ipairs(torches) do t.y = ground[t.x] - 1 end

-- Stone is textured once: scattered darker blocks, and ore only deep down.
local ORE = {}
for x = 0, COLS - 1 do
  ORE[x] = {}
  for y = ground[x] + 3, ROWS - 1 do
    local c = STONE
    if y >= 14 and (x * 7 + y * 13) % 13 == 0 then c = DIAMOND
    elseif y >= 13 and (x * 5 + y * 3) % 17 == 0 then c = GOLD
    elseif (x * 13 + y * 7) % 6 == 0 or (x * 5 + y * 11) % 9 == 0 then c = STONE_D end
    ORE[x][y] = c
  end
end

-- Torchlight falls off by a fifth a block, counted in whole blocks the way the
-- game counts it, so it lights blocks and never haloes the sky.
local TL = {}
for x = 0, COLS - 1 do
  TL[x] = {}
  for y = 0, ROWS - 1 do
    local l = 0
    for _, tq in ipairs(torches) do
      local d = abs(x - tq.x) + abs(y - tq.y)
      if d < 5 then l = max(l, 1 - d / 5) end
    end
    TL[x][y] = l
  end
end

local clouds = { {x = 2, y = 2, w = 5}, {x = 13, y = 1, w = 7}, {x = 24, y = 3, w = 4} }
local stars = {}
for i = 1, 34 do stars[i] = {x = (i * 41) % W, y = (i * 23) % 30} end

-- ---------------------------------------------------------------- sky
-- Three bands, top to horizon, on the block grid. Night is black, not navy.
local NIGHT = { {0, 0, 0},     {0, 0, 0},     {0, 0, 0} }
local DAWN  = { {60, 10, 110}, {230, 60, 30}, {255, 150, 20} }
local DAY   = { {0, 60, 225},  {10, 115, 250}, {60, 165, 255} }
local function sky_mix(a, b, k) return { mix(a[1], b[1], k), mix(a[2], b[2], k), mix(a[3], b[3], k) } end

-- The phase of the day: dawn near 0, noon at .25, dusk at .5, night from .64.
-- Returns the sky bands and the daylight, 0..1. Sunrise and sunset colours
-- are held for a few seconds and the blends between them are short: halfway
-- from blue to orange is a washed-out lilac, and it should not linger.
local function sky_at(t)
  if t < 0.05 then local k = t / 0.05; return sky_mix(NIGHT, DAWN, k), 0.3 * k
  elseif t < 0.10 then return DAWN, 0.3 + 0.15 * (t - 0.05) / 0.05
  elseif t < 0.14 then local k = (t - 0.10) / 0.04; return sky_mix(DAWN, DAY, k), 0.45 + 0.55 * k
  elseif t < 0.46 then return DAY, 1
  elseif t < 0.50 then local k = (t - 0.46) / 0.04; return sky_mix(DAY, DAWN, k), 1 - 0.55 * k
  elseif t < 0.57 then return DAWN, 0.45 - 0.15 * (t - 0.50) / 0.07
  elseif t < 0.64 then local k = (t - 0.57) / 0.07; return sky_mix(DAWN, NIGHT, k), 0.3 * (1 - k)
  else return NIGHT, 0 end
end

-- ---------------------------------------------------------------- figures
-- The ground under a figure: the highest of the columns it overlaps, so it
-- steps up onto a block rather than sinking into it.
local function surface(xp)
  local c = floor(xp / B)
  if c < 0 then c = 0 elseif c > COLS - 1 then c = COLS - 1 end
  return ground[c] * B
end
local function feet_y(x, w)
  return min(surface(x - 1), surface(x), surface(x + w - 1), surface(x + w))
end

-- Walk from x0 to x1 and back, pausing at each end and turning halfway
-- through the pause. Returns x, facing (+1 right), and whether it is walking.
local function patrol(secs, x0, x1, speed, pause)
  local walk = (x1 - x0) / speed
  local u = secs % (2 * walk + 2 * pause)
  if u < walk then return x0 + u * speed, 1, true
  elseif u < walk + pause then return x1, (u < walk + pause / 2) and 1 or -1, false
  elseif u < 2 * walk + pause then return x1 - (u - walk - pause) * speed, -1, true
  else return x0, (u < 2 * walk + 1.5 * pause) and -1 or 1, false end
end

-- Steve or a zombie, 4 x 8, side on. step: 0 standing, 1 and 3 mid-stride
-- with the arm forward or back, 2 legs passing.
local function person(x, y, dir, step, s, skin, shirt, pants, zombie)
  local front = dir > 0 and x + 3 or x
  local back  = dir > 0 and x or x + 3
  box(x, y, 4, 1, zombie and ZSKIN or HAIR, s)
  box(x, y + 1, 4, 3, skin, s)
  if not zombie then box(back, y + 1, 1, 1, HAIR, s) end
  box(dir > 0 and x + 2 or x + 1, y + 2, 1, 1, zombie and BLACK or EYE, s)
  box(x + 1, y + 4, 2, 2, shirt, s)
  if zombie then
    box(dir > 0 and x + 3 or x - 1, y + 4, 2, 1, skin, s)       -- arms out in front
  elseif step == 1 then
    box(front, y + 5, 1, 1, skin, s)
  elseif step == 3 then
    box(back, y + 5, 1, 1, skin, s)
  end
  if step == 1 or step == 3 then
    box(x + 1, y + 6, 2, 1, pants, s)
    box(x, y + 7, 1, 1, pants, s)
    box(x + 3, y + 7, 1, 1, pants, s)
  else
    box(x + 1, y + 6, 2, 2, pants, s)
  end
end

-- A creeper, 4 x 8, facing out of the screen as it always does.
local function creeper(x, y, step, s, wt)
  box(x, y, 4, 4, CREEP, s, wt)
  box(x + 1, y, 1, 1, CREEP_D, s, wt)
  box(x, y + 1, 1, 1, BLACK, 1)
  box(x + 3, y + 1, 1, 1, BLACK, 1)
  box(x + 1, y + 2, 2, 2, BLACK, 1)
  box(x + 1, y + 4, 2, 2, CREEP_D, s, wt)
  box(x, y + 6, 1, step == 1 and 1 or 2, CREEP, s, wt)
  box(x + 3, y + 6, 1, step == 1 and 2 or 1, CREEP, s, wt)
  box(x + 1, y + 7, 2, 1, CREEP_D, s, wt)
end

-- A pig, 8 x 5, side on.
local function pig(x, y, dir, step, s)
  local function at(o) return dir > 0 and x + o or x + 7 - o end
  box(dir > 0 and x or x + 3, y + 1, 5, 3, PIG, s)
  box(dir > 0 and x + 5 or x, y, 3, 3, PIG, s)
  box(at(7), y + 1, 1, 2, PIG_D, s)
  box(at(5), y + 1, 1, 1, BLACK, 1)
  box(at(step == 1 and 1 or 0), y + 4, 1, 1, PIG_D, s)
  box(at(step == 1 and 3 or 4), y + 4, 1, 1, PIG_D, s)
end

-- The creeper's minute: it hisses and flashes, goes off, and is back at dusk.
local HISS0, BOOM0, BOOM1, BACK = 0.31, 0.36, 0.39, 0.52
local CREEPER_X0, CREEPER_X1 = 46, 66

-- ---------------------------------------------------------------- draw
function draw()
  local t    = px.t()
  local secs = t * 60                         -- px.t() is the second hand on the panel
  local sky, day = sky_at(t)
  local lit  = 0.32 + 0.68 * day              -- sunlight on the world
  local rim  = lit + (1 - lit) * 0.45         -- the moon still catches the top edge
  local ms   = max(lit, 0.7)                  -- figures stay readable at night

  px.clear(floor(sky[1][1]), floor(sky[1][2]), floor(sky[1][3]))
  if day > 0 then
    -- the lower bands stop where the lowest ground and the lake begin
    px.rect(0, 20, W, 16, floor(sky[2][1]), floor(sky[2][2]), floor(sky[2][3]), true)
    px.rect(0, 36, W, 16, floor(sky[3][1]), floor(sky[3][2]), floor(sky[3][3]), true)
  end

  -- stars, only once the sky is dark enough to hold them
  if day < 0.3 then
    local a = (0.3 - day) / 0.3
    for i, st in ipairs(stars) do
      local v = floor(230 * a * (0.5 + 0.5 * sin(2 * pi * (t * 6 + i * 0.13))))
      px.pixel(st.x, st.y, v, v, v)
    end
  end

  -- sun and moon on the same arc, half a day apart
  local sp = (t - 0.02) / 0.56
  if sp >= 0 and sp <= 1 then
    local ax, ay = floor(4 + (W - 8) * sp), floor(H * 0.62 - (H * 0.52) * sin(pi * sp))
    px.circle(ax, ay, 7, 255, 120, 0, true)
    px.circle(ax, ay, 5, 255, 235, 50, true)
  end
  if t > 0.58 or t < 0.06 then
    local mp = ((t < 0.5 and (t + 0.5) or (t - 0.5)) - 0.02) / 0.56
    if mp >= 0 and mp <= 1 then
      local ax, ay = floor(4 + (W - 8) * mp), floor(H * 0.62 - (H * 0.52) * sin(pi * mp))
      px.circle(ax, ay, 4, 240, 240, 250, true)
      px.rect(ax - 2, ay - 1, 2, 2, 175, 175, 195, true)
      px.rect(ax + 1, ay + 1, 1, 1, 175, 175, 195, true)
    end
  end

  -- clouds, two runs of blocks each: white by day, warm at sunrise and
  -- sunset, gone at night rather than grey
  local cs = min(1, day * 3.4)
  if cs > 0.05 then
    local warm = 1 - max(0, min(1, (day - 0.3) / 0.7))
    local c1, c2 = mix(CLOUD, CLOUD_W, warm), mix(CLOUD_2, CLOUD_W, warm)
    for _, c in ipairs(clouds) do
      local cx = floor(c.x + t * COLS * 1.5) % (COLS + c.w) - c.w
      box(cx * B, c.y * B, c.w * B, B, c1, cs)
      box((cx + 1) * B, (c.y + 1) * B, (c.w - 2) * B, B, c2, cs)
    end
  end

  -- torchlight comes up as the sun goes down, and flickers
  local tk = 0
  if day < 0.6 then tk = (1 - day / 0.6) * (0.85 + 0.15 * sin(2 * pi * t * 23)) end

  for x = 0, COLS - 1 do
    local g, X, tl = ground[x], x * B, TL[x]
    if kind[x] == "water" then
      local wob = floor(1.5 + 1.5 * sin(2 * pi * (t * 8) + x))
      box(X, g * B + wob, B, 2 * B - wob, WATER, lit)
      box(X, (g + 2) * B, B, (ROWS - g - 2) * B, WATER_D, lit)
      px.rect(X, g * B + wob, B, 1, floor(150 * rim), floor(215 * rim), floor(255 * rim), true)
    else
      local sand = kind[x] == "sand"
      local s, wm = lit, 0
      local l = tl[g] * tk
      if l > s then wm, s = l - s, l end
      lit_box(X, g * B, B, B, sand and SAND or GRASS, s, wm)
      lit_box(X, g * B, B, 1, sand and SAND or GRASS_HI, max(s, rim), wm)
      s, wm = lit, 0
      l = tl[g + 1] * tk
      if l > s then wm, s = l - s, l end
      lit_box(X, (g + 1) * B, B, 2 * B, DIRT, s, wm)
      local ore = ORE[x]
      for y = g + 3, ROWS - 1 do
        local c = ore[y]
        s, wm = lit * max(0.18, 1 - (y - g - 2) * 0.24), 0
        if c == DIAMOND or c == GOLD then s = max(s, 0.75 * lit) end   -- ore catches the light
        l = tl[y] * tk * 0.8
        if l > s then wm, s = l - s, l end
        lit_box(X, y * B, B, B, c, s, wm)
      end
    end
  end

  for _, tr in ipairs(trees) do
    local ty = ground[tr.x] - tr.h
    box(tr.x * B, ty * B, B, tr.h * B, LOG, lit)
    box((tr.x - 1) * B, (ty - 1) * B, 3 * B, 2 * B, LEAF, lit)
    box(tr.x * B, (ty - 1) * B, B, B, LEAF_L, lit)
    box((tr.x - 2) * B, ty * B, B, B, LEAF, lit * 0.8)
    box((tr.x + 2) * B, ty * B, B, B, LEAF, lit * 0.8)
  end

  for _, tq in ipairs(torches) do
    local X, Y = tq.x * B, tq.y * B
    local f = 0.8 + 0.2 * sin(2 * pi * (t * 23 + tq.x))
    box(X + 1, Y + 1, 2, 3, {110, 72, 30}, max(lit, 0.6))
    px.rect(X + 1, Y, 2, 1, 255, floor(215 * f), floor(60 * f), true)
    if day < 0.6 then px.pixel(X + 1 + floor(secs * 7 + tq.x) % 2, Y - 1, 255, floor(140 * f), 0) end
  end

  -- the far shore: a pig by day, a zombie by night
  if t > 0.08 and t < 0.56 then
    local x, dir, moving = patrol(secs + 3, 101, 116, 4, 1.5)
    x = floor(x)
    pig(x, feet_y(x, 8) - 5, dir, moving and floor(secs * 5) % 2 or 0, ms)
  elseif t > 0.62 or t < 0.04 then
    local x, dir, moving = patrol(secs, 100, 122, 5, 1.2)
    x = floor(x)
    person(x, feet_y(x, 4) - 8, dir, moving and floor(secs * 5) % 4 or 0, ms, ZSKIN, ZSHIRT, ZPANTS, true)
  end

  -- the creeper on the ridge
  if t < HISS0 or t >= BACK then
    local x, _, moving = patrol(secs, CREEPER_X0, CREEPER_X1, 6, 1.0)
    x = floor(x)
    creeper(x, feet_y(x, 4) - 8, moving and floor(secs * 6) % 2 or 0, ms)
  elseif t < BOOM0 then
    local x = floor(patrol(HISS0 * 60, CREEPER_X0, CREEPER_X1, 6, 1.0))
    local flash = (floor(secs * 6) % 2 == 0) and 0.85 or 0
    local shake = (t > BOOM0 - 0.015) and (floor(secs * 20) % 2) or 0
    creeper(x + shake, feet_y(x, 4) - 8, 0, ms, flash)
  elseif t < BOOM1 then
    local x = floor(patrol(HISS0 * 60, CREEPER_X0, CREEPER_X1, 6, 1.0))
    local ex, ey = x + 2, feet_y(x, 4) - 4
    local u = (t - BOOM0) / (BOOM1 - BOOM0)
    if u < 0.3 then
      local r = floor(4 + 30 * u)
      px.circle(ex, ey, r + 2, 255, 150, 0, true)
      px.circle(ex, ey, r, 255, 255, 220, true)
    end
    for i = 0, 15 do                          -- debris: grass, dirt and stone
      local a = i * pi / 8 + 0.2
      local v = 26 + (i * 7) % 5 * 5
      local c = (i % 3 == 0) and GRASS or ((i % 3 == 1) and DIRT or STONE)
      box(floor(ex + cos(a) * v * u), floor(ey - sin(a) * v * u + 50 * u * u), 2, 2, c, ms,
          max(0, 0.8 - u * 2))
    end
  end

  -- Steve: patrols the left hills and jumps now and then; while the creeper
  -- hisses he turns to it and hops on the spot
  do
    local x, dir, moving = patrol(secs, 2, 30, 10, 1.4)
    x = floor(x)
    local step = moving and (floor(secs * 8) % 4) or 0
    local hop = 0
    local jp = (secs % 3.3) / 0.5
    if jp < 1 then hop = floor(7 * sin(pi * jp)) end
    if t >= HISS0 and t < BOOM1 then
      dir, step = 1, 0
      hop = floor(5 * abs(sin(pi * secs * 2.5)))
    end
    person(x, feet_y(x, 4) - 8 - hop, dir, step, ms, SKIN, SHIRT, PANTS, false)
  end

  -- a fish jumps out of the lake every fifteen seconds
  local fp = (secs % 15) / 1.4
  if fp < 1 then
    local fx = floor(80 + 12 * fp)
    local fy = floor(48 - 13 * sin(pi * fp))
    local up = fp < 0.5
    px.rect(fx, fy, 3, 1, 255, 120, 40, true)
    px.pixel(up and fx - 1 or fx + 3, up and fy + 1 or fy - 1, 255, 70, 20)
    if fp < 0.15 or fp > 0.85 then
      local sx = fp < 0.15 and 80 or 92
      px.pixel(sx - 2, 46, 190, 230, 255)
      px.pixel(sx + 3, 45, 190, 230, 255)
      px.pixel(sx, 44, 230, 245, 255)
    end
  end

  -- clock, top right: white with a black shadow, so it reads on sky and cloud
  local n = px.now()
  local hhmm = string.format("%02d:%02d", n.hour, n.min)
  local tx = W - px.width(hhmm) - 2
  px.text(tx + 1, 3, hhmm, 0, 0, 0)
  px.text(tx, 2, hhmm, 255, 255, 255)
end

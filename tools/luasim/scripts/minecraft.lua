-- minecraft.lua - a blocky world with a full day/night cycle in one minute.
--
-- 128x64 divides into 32 x 16 blocks of 4 px, which is the largest block size
-- that still leaves room for a skyline. Everything is filled rects: on a panel
-- this coarse, outlines read as noise and solid colour reads as a world.

local W, H = px.size()
local B    = 4                       -- block size, px
local COLS, ROWS = W // B, H // B    -- 32 x 16
local floor, sin, cos, pi, max, min = math.floor, math.sin, math.cos, math.pi, math.max, math.min

-- ---------------------------------------------------------------- palette
local GRASS_T = {124, 179, 66}   -- lit grass top
local GRASS_S = {94, 140, 50}    -- grass side, shaded
local DIRT    = {134, 96, 67}
local STONE   = {122, 122, 122}
local STONE_D = {90, 90, 90}
local ORE     = {180, 180, 190}
local LOG     = {102, 76, 46}
local LEAF    = {60, 120, 45}
local LEAF_L  = {80, 148, 58}
local WATER   = {50, 110, 220}
local SAND    = {214, 200, 148}
local TORCH   = {255, 190, 70}

local function blk(cx, cy, c, shade)
  if cy < 0 or cy >= ROWS then return end
  local s = shade or 1.0
  px.rect(cx * B, cy * B, B, B,
          floor(c[1] * s), floor(c[2] * s), floor(c[3] * s), true)
end

-- ---------------------------------------------------------------- terrain
-- Fixed, not generated per frame: the world should not shimmer.
local ground, kind = {}, {}
for x = 0, COLS - 1 do
  local h = 10 + floor(1.6 * sin(x * 0.42) + 1.2 * sin(x * 0.17 + 1.3))
  ground[x] = h
  kind[x] = "grass"
end
-- A lake, carved flat.
for x = 19, 24 do ground[x] = 12; kind[x] = "water" end
for _, x in ipairs({18, 25}) do kind[x] = "sand" end

local trees = { {x = 4, h = 3}, {x = 9, h = 4}, {x = 29, h = 3} }
local torches = { {x = 14, y = nil}, {x = 27, y = nil} }
for _, t in ipairs(torches) do t.y = ground[t.x] - 1 end

-- Clouds drift; each is a run of blocks at a fixed altitude.
local clouds = { {x = 2, y = 2, w = 5}, {x = 13, y = 1, w = 7}, {x = 24, y = 3, w = 4} }

-- ---------------------------------------------------------------- sky
local function mix(a, b, t)
  t = max(0, min(1, t))
  return {a[1] + (b[1]-a[1])*t, a[2] + (b[2]-a[2])*t, a[3] + (b[3]-a[3])*t}
end

local NIGHT = {8, 12, 34}
local DAWN  = {232, 120, 70}
local DAY   = {110, 168, 255}

local function sky_colour(t)
  -- t is the phase of the whole day. Dawn near 0, noon at .25, dusk at .5.
  if t < 0.08 then return mix(NIGHT, DAWN, t / 0.08)
  elseif t < 0.20 then return mix(DAWN, DAY, (t - 0.08) / 0.12)
  elseif t < 0.45 then return DAY
  elseif t < 0.55 then return mix(DAY, DAWN, (t - 0.45) / 0.10)
  elseif t < 0.65 then return mix(DAWN, NIGHT, (t - 0.55) / 0.10)
  else return NIGHT end
end

-- Stars sit at fixed points and only fade, which reads as depth; moving ones
-- would read as noise at this size.
local stars = {}
for i = 1, 34 do stars[i] = {x = (i * 41) % W, y = (i * 23) % 30} end

-- ---------------------------------------------------------------- draw
function draw()
  local t   = px.t()
  local sky = sky_colour(t)
  local day = (t > 0.10 and t < 0.55) and 1.0 or 0.0
  local dark = 1.0 - max(0, min(1, (sky[1] + sky[2] + sky[3]) / 420))  -- 0 day .. ~1 night

  px.clear(floor(sky[1]), floor(sky[2]), floor(sky[3]))

  -- stars, only once the sky is dark enough to hold them
  if dark > 0.35 then
    local a = (dark - 0.35) / 0.65
    for i, s in ipairs(stars) do
      local tw = 0.55 + 0.45 * sin(2*pi*(t*6 + i*0.13))
      local v = floor(210 * a * tw)
      px.pixel(s.x, s.y, v, v, floor(v * 1.05))
    end
  end

  -- sun and moon on the same arc, half a day apart
  local function body(phase, rad, c1, c2)
    if phase < 0 or phase > 1 then return end
    local ax = floor(4 + (W - 8) * phase)
    local ay = floor(H * 0.62 - (H * 0.52) * sin(pi * phase))
    px.circle(ax, ay, rad + 2, c2[1], c2[2], c2[3], true)
    px.circle(ax, ay, rad,     c1[1], c1[2], c1[3], true)
  end
  body((t - 0.02) / 0.56, 5, {255, 240, 150}, {255, 170, 60})          -- sun
  local mp = t < 0.5 and (t + 0.5) or (t - 0.5)
  if t > 0.58 or t < 0.06 then body((mp - 0.02) / 0.56, 4, {235,235,245}, {90,95,120}) end

  -- clouds, brighter by day
  local cs = 0.45 + 0.55 * (1 - dark)
  for _, c in ipairs(clouds) do
    local cx = floor(c.x + t * COLS * 1.5) % (COLS + c.w) - c.w
    for i = 0, c.w - 1 do
      blk(cx + i, c.y, {245, 248, 255}, cs)
      if i > 0 and i < c.w - 1 then blk(cx + i, c.y + 1, {225, 232, 245}, cs) end
    end
  end

  -- world lighting: blocks dim at night but never go black
  local lit = 0.42 + 0.58 * (1 - dark)

  for x = 0, COLS - 1 do
    local g = ground[x]
    if kind[x] == "water" then
      -- a surface that ripples by one pixel, not by a whole block
      local wob = floor(1.5 + 1.5 * sin(2*pi*(t*8) + x))
      px.rect(x*B, g*B + wob, B, (ROWS - g)*B - wob,
              floor(WATER[1]*lit), floor(WATER[2]*lit), floor(WATER[3]*lit), true)
      px.rect(x*B, g*B + wob, B, 1, floor(150*lit), floor(200*lit), 255, true)
    else
      local top = (kind[x] == "sand") and SAND or GRASS_T
      blk(x, g, top, lit)
      blk(x, g, kind[x] == "sand" and SAND or GRASS_S, lit * 0.82)
      blk(x, g, top, lit)                                  -- top face, redrawn lit
      px.rect(x*B, g*B, B, 1, floor(top[1]*lit*1.15), floor(top[2]*lit*1.15), floor(top[3]*lit*1.15), true)
      for y = g + 1, ROWS - 1 do
        local c = (y < g + 3) and DIRT or ((x * 7 + y * 13) % 17 == 0 and ORE or
                  (((x + y) % 5 == 0) and STONE_D or STONE))
        blk(x, y, c, lit)
      end
    end
  end

  -- trees
  for _, tr in ipairs(trees) do
    local base = ground[tr.x]
    for i = 1, tr.h do blk(tr.x, base - i, LOG, lit) end
    local ty = base - tr.h
    for dx = -1, 1 do for dy = 0, 1 do
      blk(tr.x + dx, ty - dy, (dx == 0 and dy == 1) and LEAF_L or LEAF, lit)
    end end
    blk(tr.x - 2, ty, LEAF, lit * 0.9); blk(tr.x + 2, ty, LEAF, lit * 0.9)
  end

  -- torches: dim by day, and they pool light on the ground at night
  for _, tq in ipairs(torches) do
    local flick = 0.75 + 0.25 * sin(2*pi*(t*23 + tq.x))
    local glow = dark * flick
    if glow > 0.05 then
      -- One additive lamp, not a stack of filled circles: px.circle overwrites,
      -- so a dim colour paints a dark blot instead of a faint light.
      px.glow(tq.x*B + 2, tq.y*B + 1, 11 + 2*flick, 255, 165, 55, 0.85 * glow)
    end
    px.rect(tq.x*B + 1, tq.y*B + 1, 2, 3, 92, 66, 40, true)
    px.rect(tq.x*B + 1, tq.y*B, 2, 2,
            floor(255*flick), floor(190*flick), floor(70*flick), true)
  end

  -- a creeper, pacing the flat ground left of the lake
  local cx = 11 + floor(6 * (0.5 + 0.5 * sin(2*pi*t)))
  local cy = ground[cx] - 2
  local bob = (floor(t * 60) % 2 == 0) and 0 or 1
  blk(cx, cy + 1, {70, 160, 72}, lit)          -- body
  blk(cx, cy, {84, 180, 84}, lit)              -- head
  px.rect(cx*B + 1, cy*B + 1, 1, 1, 12, 12, 12, true)
  px.rect(cx*B + 3, cy*B + 1, 1, 1, 12, 12, 12, true)
  px.rect(cx*B + 1, cy*B + 2, 3, 2, 12, 12, 12, true)
  px.rect(cx*B, (cy+2)*B, 2, 2 - bob, floor(60*lit), floor(140*lit), floor(62*lit), true)
  px.rect(cx*B + 2, (cy+2)*B, 2, 1 + bob, floor(60*lit), floor(140*lit), floor(62*lit), true)

  -- clock, top right, in the sky's contrast
  local n = px.now()
  local hhmm = string.format("%02d:%02d", n.hour, n.min)
  local tv = dark > 0.5 and 235 or 30
  px.text(W - px.width(hhmm) - 2, 2, hhmm, tv, tv, tv)
end

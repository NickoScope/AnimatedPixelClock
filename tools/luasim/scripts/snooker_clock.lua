-- snooker_clock.lua - a snooker table that plays frames by itself, with the
-- time in the corner like a game's HUD.
--
-- Geometry and play follow the WPBSA Official Rules of the Games of Snooker
-- and English Billiards (revised September 2024, identical in these parts to
-- the May 2022 edition). Where a rule is used, its number is next to it.

local W, H = px.size()
local floor, sqrt, min, max, abs = math.floor, math.sqrt, math.min, math.max, math.abs

-- ---------------------------------------------------------------- the table
-- Section 1 Rule 1(a): the playing area, within the cushion faces, is
-- 11 ft 8 1/2 in x 5 ft 10 in (3569 x 1778 mm). 116 x 58 px keeps that 2:1
-- to 0.4 % and leaves 3 px of rail along the long sides, 6 px at the ends.
-- Baulk is on the left, the Top Cushion on the right.
local OX, OY, TL, TW = 6, 3, 116, 58
local MM = 3569 / TL                          -- mm per px, 30.8
local CY = OY + TW / 2                        -- the centre longitudinal line
local BAULK_X = OX + 737 / MM                 -- 1(d): 29 in from the Bottom Cushion
local D_R = 292 / MM                          -- 1(e): the D, radius 11 1/2 in

-- ---------------------------------------------------------------- the balls
-- Section 1 Rule 2: fifteen reds, one each of yellow, green, brown, blue,
-- pink and black, and the white; 52.5 mm across, which is 1.7 px here - a
-- dot. They are drawn 1.93x real, 3.3 px across: the largest size at which
-- the pack still fits between the Pink and the Black the way Section 3
-- Rule 2(a) sets it up, apex red against the Pink, base row clear of the Black.
local BD = 3.3
local R = BD / 2

-- 1(f): the spots. Yellow is on the right-hand corner of the D seen from the
-- Baulk end, which with Baulk on the left is the lower corner on the panel.
local KINDS = {
  red    = {v = 1, col = {215,  20,  25}},
  yellow = {v = 2, col = {255, 215,   0}, spot = {BAULK_X, CY + D_R}},
  green  = {v = 3, col = {  0, 185,  85}, spot = {BAULK_X, CY - D_R}},
  brown  = {v = 4, col = {150,  72,  22}, spot = {BAULK_X, CY}},
  blue   = {v = 5, col = { 30,  85, 255}, spot = {OX + TL / 2, CY}},
  pink   = {v = 6, col = {255, 120, 175}, spot = {OX + TL * 3 / 4, CY}},
  black  = {v = 7, col = { 28,  28,  34}, spot = {OX + TL - 324 / MM, CY}},
  white  = {v = 0, col = {245, 245, 238}},
}
local COLOURS = {"yellow", "green", "brown", "blue", "pink", "black"}

local CLOTH      = {0, 118, 44}               -- the cloth a ball's edge is mixed with
local CLOTH_EDGE = {0, 84, 30}
local WOOD, WOOD_HI, WOOD_DK = {150, 82, 30}, {196, 122, 58}, {52, 22, 8}
local CUSHION    = {0, 72, 26}
local LINE       = {70, 170, 100}             -- baulk line, D and spots

-- ---------------------------------------------------------------- sprites
-- A ball is a sprite worked out once: coverage from a 3x3 supersample, a
-- sphere lit from the top left with a diffuse term and a tight specular
-- highlight, a soft shadow down and to the right, and the edge mixed with the
-- cloth. Drawing one is a dozen px.pixel calls and no maths.
local LX, LY, LZ = -0.45, -0.55, 0.70
do local n = sqrt(LX * LX + LY * LY + LZ * LZ); LX, LY, LZ = LX / n, LY / n, LZ / n end
local HX, HY, HZ = LX, LY, LZ + 1
do local n = sqrt(HX * HX + HY * HY + HZ * HZ); HX, HY, HZ = HX / n, HY / n, HZ / n end

local function coverage(px0, py0, cx, cy, rad)
  local hit = 0
  for sy = 0, 2 do
    for sx = 0, 2 do
      local dx, dy = px0 + (sx + 0.5) / 3 - cx, py0 + (sy + 0.5) / 3 - cy
      if dx * dx + dy * dy <= rad * rad then hit = hit + 1 end
    end
  end
  return hit / 9
end

-- scale 1 is a ball on the table; smaller ones are the drop into a pocket.
local function build_sprite(col, scale)
  local rad = R * scale
  local sp = {n = 0, dx = {}, dy = {}, r = {}, g = {}, b = {}}
  for y = 0, 4 do
    for x = 0, 4 do
      local bc = coverage(x, y, 2, 2, rad)
      local sc = scale == 1 and coverage(x, y, 2.6, 2.7, rad * 0.95) or 0
      if bc > 0.08 or sc > 0.25 then
        local shade = 1 - 0.6 * sc
        local r, g, b = CLOTH[1] * shade, CLOTH[2] * shade, CLOTH[3] * shade
        if bc > 0 then
          local nx, ny = (x + 0.5 - 2) / rad, (y + 0.5 - 2) / rad
          local rr = nx * nx + ny * ny
          if rr > 0.98 then local k = sqrt(0.98 / rr); nx, ny, rr = nx * k, ny * k, 0.98 end
          local nz = sqrt(1 - rr)
          local diff = max(0, nx * LX + ny * LY + nz * LZ)
          local s = max(0, nx * HX + ny * HY + nz * HZ)
          local s2 = s * s; local s4 = s2 * s2; local s8 = s4 * s4; local s16 = s8 * s8
          local spec = s16 * s8 * 0.95 * 255
          local k = 0.30 + 0.85 * diff
          local br = min(255, col[1] * k + spec)
          local bg = min(255, col[2] * k + spec)
          local bb = min(255, col[3] * k + spec)
          r, g, b = r + (br - r) * bc, g + (bg - g) * bc, b + (bb - b) * bc
        end
        local i = sp.n + 1
        sp.n = i
        sp.dx[i], sp.dy[i] = x - 2, y - 2
        sp.r[i], sp.g[i], sp.b[i] = floor(r), floor(g), floor(b)
      end
    end
  end
  return sp
end

local SPRITES = {}
for name, k in pairs(KINDS) do
  SPRITES[name] = { build_sprite(k.col, 1), build_sprite(k.col, 0.75), build_sprite(k.col, 0.5) }
end

local function draw_sprite(sp, x, y)
  local ox, oy = floor(x + 0.5), floor(y + 0.5)
  local dx, dy, r, g, b = sp.dx, sp.dy, sp.r, sp.g, sp.b
  for i = 1, sp.n do px.pixel(ox + dx[i], oy + dy[i], r[i], g[i], b[i]) end
end

-- ---------------------------------------------------------------- static draw list
-- Everything that never moves, as a list of calls built once.
local STATIC = {}
local function rect(x, y, w, h, c) STATIC[#STATIC + 1] = {1, x, y, w, h, c[1], c[2], c[3]} end
local function dot(x, y, c) STATIC[#STATIC + 1] = {2, x, y, c[1], c[2], c[3]} end
local function disc(x, y, r, c) STATIC[#STATIC + 1] = {3, x, y, r, c[1], c[2], c[3]} end

-- The frame starts from a clear to the middle of the cloth, so the list only
-- paints strips: a full-canvas rect per layer cost 50 000 pixel writes a frame.

-- rails: a dark outer edge, wood with a lit bevel, and the cushion nose
rect(0, 0, W, 1, WOOD_DK)
rect(0, H - 1, W, 1, WOOD_DK)
rect(0, 0, 1, H, WOOD_DK)
rect(W - 1, 0, 1, H, WOOD_DK)
rect(1, 1, W - 2, 1, WOOD_HI)
rect(1, H - 2, W - 2, 1, WOOD)
rect(1, 1, 1, H - 2, WOOD_HI)
rect(2, 2, OX - 3, H - 4, WOOD)
rect(OX + TL + 1, 1, W - OX - TL - 2, H - 2, WOOD)
rect(OX - 1, OY - 1, TL + 2, 1, CUSHION)
rect(OX - 1, OY + TW, TL + 2, 1, CUSHION)
rect(OX - 1, OY - 1, 1, TW + 2, CUSHION)
rect(OX + TL, OY - 1, 1, TW + 2, CUSHION)

-- cloth, a little darker towards the cushions: rings of 3 px at the ends and
-- 2 px along the sides, four steps from the edge to the middle
local VIG = 4
local function cloth_at(k)
  local f = k / VIG
  return {CLOTH_EDGE[1] + (CLOTH[1] - CLOTH_EDGE[1]) * f,
          floor(CLOTH_EDGE[2] + (CLOTH[2] + 8 - CLOTH_EDGE[2]) * f),
          floor(CLOTH_EDGE[3] + (CLOTH[3] + 3 - CLOTH_EDGE[3]) * f)}
end
local CLOTH_MID = cloth_at(VIG)
for k = 0, VIG - 1 do
  local c = cloth_at(k)
  local x, y, w, h = OX + k * 3, OY + k * 2, TL - k * 6, TW - k * 4
  rect(x, y, w, 2, c)
  rect(x, y + h - 2, w, 2, c)
  rect(x, y + 2, 3, h - 4, c)
  rect(x + w - 3, y + 2, 3, h - 4, c)
end

-- baulk line and the D
local bx = floor(BAULK_X)
rect(bx, OY, 1, TW, LINE)
for y = OY, OY + TW - 1 do
  for x = OX, bx - 1 do
    local d = sqrt((x + 0.5 - BAULK_X) ^ 2 + (y + 0.5 - CY) ^ 2)
    if abs(d - D_R) < 0.5 then dot(x, y, LINE) end
  end
end
for _, name in ipairs(COLOURS) do
  local s = KINDS[name].spot
  dot(floor(s[1]), floor(s[2]), LINE)
end

-- 1(g): six pockets, one at each corner and one in the middle of each long side
local POCKETS = {
  {OX, OY, 2.8}, {OX + TL / 2, OY - 0.5, 2.3}, {OX + TL, OY, 2.8},
  {OX, OY + TW, 2.8}, {OX + TL / 2, OY + TW + 0.5, 2.3}, {OX + TL, OY + TW, 2.8},
}
for _, p in ipairs(POCKETS) do
  disc(floor(p[1]), floor(p[2]), 3, WOOD_DK)
  disc(floor(p[1]), floor(p[2]), 2, {0, 0, 0})
end

local function draw_static()
  px.clear(CLOTH_MID[1], CLOTH_MID[2], CLOTH_MID[3])
  for i = 1, #STATIC do
    local o = STATIC[i]
    local k = o[1]
    if k == 1 then px.rect(o[2], o[3], o[4], o[5], o[6], o[7], o[8], true)
    elseif k == 2 then px.pixel(o[2], o[3], o[4], o[5], o[6])
    else px.circle(o[2], o[3], o[4], o[5], o[6], o[7], true) end
  end
end

-- ---------------------------------------------------------------- the rack
-- Section 3 Rule 2(a): reds in a tightly packed equilateral triangle, the
-- apex red on the centre line above the Pink Spot as close to it as it can be
-- without occupying it, the base parallel to the Top Cushion; the colours on
-- their spots; the cue-ball in hand.
local balls
local function rack()
  balls = {}
  local function add(kind, x, y)
    balls[#balls + 1] = {kind = kind, x = x, y = y, vx = 0, vy = 0, alive = true}
  end
  add("white", BAULK_X - 4, CY + D_R * 0.55)
  for _, k in ipairs(COLOURS) do add(k, KINDS[k].spot[1], KINDS[k].spot[2]) end
  local x0 = KINDS.pink.spot[1] + BD + 0.02
  local row_dx = BD * sqrt(3) / 2 + 0.01
  for row = 0, 4 do
    for i = 0, row do add("red", x0 + row * row_dx, CY + (i - row / 2) * (BD + 0.01)) end
  end
end
rack()

-- ---------------------------------------------------------------- draw
function draw()
  draw_static()
  for _, b in ipairs(balls) do
    if b.alive then draw_sprite(SPRITES[b.kind][1], b.x, b.y) end
  end
end

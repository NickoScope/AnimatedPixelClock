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

-- ---------------------------------------------------------------- physics
-- Units are px and seconds. A rolling ball slows at a constant rate; balls
-- collide elastically as equal masses, losing a little; cushions give back
-- less. Sub-steps keep any ball under a pixel of travel per step, so nothing
-- passes through another ball or a cushion.
local DECEL  = 34          -- px/s^2
local STOP   = 2.0         -- px/s: slower than this is at rest
local E_BALL = 0.94
local E_CUSH = 0.72
local VMAX   = 230         -- px/s, a power shot
local XMIN, XMAX = OX + R, OX + TL - R
local YMIN, YMAX = OY + R, OY + TW - R

-- What happened during the current stroke, for the rules.
local stroke = {first = nil, potted = {}, spin = 0}

local function pocket_check(b)
  for i = 1, 6 do
    local p = POCKETS[i]
    local dx, dy = b.x - p[1], b.y - p[2]
    if dx * dx + dy * dy < p[3] * p[3] then
      b.alive, b.drop, b.pxx, b.pyy = false, 0, p[1], p[2]
      b.vx, b.vy = 0, 0
      stroke.potted[#stroke.potted + 1] = b
      return true
    end
  end
  return false
end

local function cushion(b)
  if b.x < XMIN then
    b.x, b.vx, b.vy = 2 * XMIN - b.x, -b.vx * E_CUSH, b.vy * 0.96
  elseif b.x > XMAX then
    b.x, b.vx, b.vy = 2 * XMAX - b.x, -b.vx * E_CUSH, b.vy * 0.96
  end
  if b.y < YMIN then
    b.y, b.vy, b.vx = 2 * YMIN - b.y, -b.vy * E_CUSH, b.vx * 0.96
  elseif b.y > YMAX then
    b.y, b.vy, b.vx = 2 * YMAX - b.y, -b.vy * E_CUSH, b.vx * 0.96
  end
end

-- Contact between a and b along n, a moving into b. The first thing the white
-- touches is recorded, and that is when follow or draw takes hold: the cue
-- ball's own forward roll, or backspin, added to its path after the impact.
local function collide(a, b, nx, ny)
  local vn = (a.vx - b.vx) * nx + (a.vy - b.vy) * ny
  if vn <= 0 then return end
  local cue, other = nil, nil
  if a.kind == "white" then cue, other = a, b elseif b.kind == "white" then cue, other = b, a end
  local cvx, cvy = 0, 0
  if cue and not stroke.first then cvx, cvy = cue.vx, cue.vy end
  local jn = vn * (1 + E_BALL) / 2
  a.vx, a.vy = a.vx - jn * nx, a.vy - jn * ny
  b.vx, b.vy = b.vx + jn * nx, b.vy + jn * ny
  if cue and not stroke.first then
    stroke.first = other
    cue.vx, cue.vy = cue.vx + stroke.spin * cvx, cue.vy + stroke.spin * cvy
  end
end

local moving = {}
local function substep(h)
  local n, nm = #balls, 0
  for i = 1, n do
    local b = balls[i]
    b.mv = false
    if b.alive and (b.vx ~= 0 or b.vy ~= 0) then
      local s = sqrt(b.vx * b.vx + b.vy * b.vy)
      local ns = s - DECEL * h
      if ns <= STOP then
        b.vx, b.vy = 0, 0
      else
        local k = ns / s
        b.vx, b.vy = b.vx * k, b.vy * k
        b.x, b.y = b.x + b.vx * h, b.y + b.vy * h
        if not pocket_check(b) then
          cushion(b)
          b.mv = true
          nm = nm + 1
          moving[nm] = i
        end
      end
    end
  end
  -- Only pairs with a moving ball in them, each pair once.
  for m = 1, nm do
    local ia = moving[m]
    local a = balls[ia]
    for ib = 1, n do
      local b = balls[ib]
      if ib ~= ia and b.alive and not (b.mv and ib < ia) then
        local dx = b.x - a.x
        if dx < BD and dx > -BD then
          local dy = b.y - a.y
          if dy < BD and dy > -BD then
            local d2 = dx * dx + dy * dy
            if d2 < BD * BD and d2 > 1e-6 then
              local d = sqrt(d2)
              local nx, ny = dx / d, dy / d
              collide(a, b, nx, ny)
              local push = (BD - d) / 2
              a.x, a.y = a.x - nx * push, a.y - ny * push
              b.x, b.y = b.x + nx * push, b.y + ny * push
            end
          end
        end
      end
    end
  end
  return nm
end

-- Advance the table by dt. Returns true while anything is still moving or
-- dropping into a pocket.
local function simulate(dt)
  local vmax = 0
  for _, b in ipairs(balls) do
    if b.alive then
      local v = abs(b.vx) + abs(b.vy)
      if v > vmax then vmax = v end
    elseif b.drop then
      b.drop = b.drop + dt
      b.x = b.x + (b.pxx - b.x) * min(1, dt * 14)
      b.y = b.y + (b.pyy - b.y) * min(1, dt * 14)
      if b.drop > 0.3 then b.drop = nil end
    end
  end
  local busy = false
  if vmax > 0 then
    local nsub = min(14, floor(vmax * dt / 0.9) + 1)
    local h = dt / nsub
    for _ = 1, nsub do
      if substep(h) > 0 then busy = true end
    end
  end
  for _, b in ipairs(balls) do if b.drop then busy = true end end
  return busy
end

-- ---------------------------------------------------------------- the cue
-- The stick lies along the shot, behind the white: it swings round onto the
-- line, draws back, and strikes. Drawn as a shadow, the ash shaft and the dark
-- butt, with a chalked tip.
local SHAFT, BUTT, TIP, CUE_SHADOW = {225, 185, 120}, {95, 42, 18}, {120, 170, 255}, {0, 56, 20}

local function draw_cue(cx, cy, dx, dy, gap)
  local tx, ty = cx - dx * (R + gap), cy - dy * (R + gap)
  local mx, my = tx - dx * 30, ty - dy * 30
  local ex, ey = tx - dx * 48, ty - dy * 48
  local f = floor
  px.line(f(tx + 1.5), f(ty + 1.5), f(ex + 1.5), f(ey + 1.5), CUE_SHADOW[1], CUE_SHADOW[2], CUE_SHADOW[3])
  px.line(f(tx + 0.5), f(ty + 0.5), f(mx + 0.5), f(my + 0.5), SHAFT[1], SHAFT[2], SHAFT[3])
  px.line(f(mx + 0.5), f(my + 0.5), f(ex + 0.5), f(ey + 0.5), BUTT[1], BUTT[2], BUTT[3])
  px.pixel(f(tx + 0.5), f(ty + 0.5), TIP[1], TIP[2], TIP[3])
end

-- ---------------------------------------------------------------- time
-- Time comes from px.t(), a phase over 60 s, so the table moves at wall-clock
-- speed at any frame rate. A long gap (the page just opened, or luasim's
-- coarse frames) is capped: the game slows rather than jumps.
local last_t
local function frame_dt()
  local t = px.t()
  local dt = last_t and (t - last_t) or 0.05
  if dt < 0 then dt = dt + 1 end
  last_t = t
  return min(0.1, dt * 60)
end

-- ---------------------------------------------------------------- break-off demo
-- Milestone 2: the break and the physics. The game comes next.
local phase, ptime = "aim", 0
local shot = {dx = 1, dy = 0, speed = 0, spin = 0}
local aim_x, aim_y = 1, 0

local function plan_break()
  local c = balls[1]
  c.x, c.y = BAULK_X - 1.5, CY + D_R * 0.5
  local best
  for _, b in ipairs(balls) do
    if b.kind == "red" and (not best or b.x > best.x + 0.1 or (abs(b.x - best.x) <= 0.1 and b.y > best.y)) then best = b end
  end
  local tx, ty = best.x, best.y + BD * 0.9
  local dx, dy = tx - c.x, ty - c.y
  local d = sqrt(dx * dx + dy * dy)
  shot.dx, shot.dy, shot.speed, shot.spin = dx / d, dy / d, 205, 0
  aim_x, aim_y = shot.dx, shot.dy
end
plan_break()

local function cue_ball() return balls[1] end

-- ---------------------------------------------------------------- draw
function draw()
  local dt = frame_dt()
  ptime = ptime + dt
  local c = cue_ball()

  if phase == "aim" and ptime > 1.0 then
    phase, ptime = "draw", 0
  elseif phase == "draw" and ptime > 0.7 then
    phase, ptime = "strike", 0
  elseif phase == "strike" and ptime > 0.06 then
    stroke = {first = nil, potted = {}, spin = shot.spin}
    c.vx, c.vy = shot.dx * shot.speed, shot.dy * shot.speed
    phase, ptime = "roll", 0
  elseif phase == "roll" then
    if not simulate(dt) and ptime > 0.3 then phase, ptime = "rest", 0 end
  elseif phase == "rest" and ptime > 2.0 then
    rack(); plan_break()
    phase, ptime = "aim", 0
  end

  draw_static()
  for _, b in ipairs(balls) do
    if b.alive then
      draw_sprite(SPRITES[b.kind][1], b.x, b.y)
    elseif b.drop then
      draw_sprite(SPRITES[b.kind][b.drop < 0.12 and 2 or 3], b.x, b.y)
    end
  end

  if phase == "aim" or phase == "draw" or phase == "strike" then
    local gap = 2
    if phase == "draw" then
      local u = min(1, ptime / 0.5)
      gap = 2 + 8 * u * (2 - u)
    elseif phase == "strike" then
      gap = 10 * (1 - ptime / 0.06)
    end
    draw_cue(c.x, c.y, aim_x, aim_y, gap)
  elseif phase == "roll" and ptime < 0.12 then
    draw_cue(c.x - shot.dx * shot.speed * ptime, c.y - shot.dy * shot.speed * ptime, shot.dx, shot.dy, 0)
  end
end

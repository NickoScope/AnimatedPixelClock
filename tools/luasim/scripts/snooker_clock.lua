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

-- A middle pocket has a mouth: between its jaws there is no cushion, and a
-- ball that gets past the cushion line there is in. Without it a ball sent
-- at the pocket at an angle met the cushion before the pocket and bounced out.
local MID_X, MOUTH = OX + TL / 2, 2.4
-- The corners have jaws too: no cushion within CMOUTH of a corner along either
-- cushion. A pot sent into a corner at a shallow angle otherwise met the side
-- cushion a few pixels short of the pocket and came back out.
local CMOUTH = 3.4
local function near_end(x) return x < OX + CMOUTH or x > OX + TL - CMOUTH end
local function near_side(y) return y < OY + CMOUTH or y > OY + TW - CMOUTH end

local function capture(b, p)
  b.alive, b.drop, b.pxx, b.pyy = false, 0, p[1], p[2]
  b.vx, b.vy = 0, 0
  stroke.potted[#stroke.potted + 1] = b
end

local function pocket_check(b)
  if abs(b.x - MID_X) < MOUTH and (b.y < OY + 0.6 or b.y > OY + TW - 0.6) then
    capture(b, POCKETS[b.y < CY and 2 or 5])
    return true
  end
  if ((b.x < OX + 0.6 or b.x > OX + TL - 0.6) and near_side(b.y)) or
     ((b.y < OY + 0.6 or b.y > OY + TW - 0.6) and near_end(b.x)) then
    capture(b, POCKETS[(b.y < CY and 0 or 3) + (b.x < MID_X and 1 or 3)])
    return true
  end
  for i = 1, 6 do
    local p = POCKETS[i]
    local dx, dy = b.x - p[1], b.y - p[2]
    if dx * dx + dy * dy < p[3] * p[3] then
      capture(b, p)
      return true
    end
  end
  return false
end

local function cushion(b)
  local corner = near_side(b.y)
  if b.x < XMIN and not corner then
    b.x, b.vx, b.vy = 2 * XMIN - b.x, -b.vx * E_CUSH, b.vy * 0.96
  elseif b.x > XMAX and not corner then
    b.x, b.vx, b.vy = 2 * XMAX - b.x, -b.vx * E_CUSH, b.vy * 0.96
  end
  local jaws = abs(b.x - MID_X) < MOUTH or near_end(b.x)
  if b.y < YMIN and not jaws then
    b.y, b.vy, b.vx = 2 * YMIN - b.y, -b.vy * E_CUSH, b.vx * 0.96
  elseif b.y > YMAX and not jaws then
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
              -- Back both balls up to the moment they touched, so the
              -- contact normal is the true one and not one skewed by the
              -- overlap a sub-step allows; then finish the step.
              local wx, wy = b.vx - a.vx, b.vy - a.vy
              local ww, dw = wx * wx + wy * wy, dx * wx + dy * wy
              local s = 0
              if ww > 1e-6 and dw < 0 then
                local disc = dw * dw - ww * (d2 - BD * BD)
                if disc > 0 then s = max(-h, (-dw - sqrt(disc)) / ww) end
              end
              a.x, a.y = a.x + a.vx * s, a.y + a.vy * s
              b.x, b.y = b.x + b.vx * s, b.y + b.vy * s
              dx, dy = b.x - a.x, b.y - a.y
              local d = sqrt(dx * dx + dy * dy)
              if d > 1e-3 then
                local nx, ny = dx / d, dy / d
                collide(a, b, nx, ny)
                a.x, a.y = a.x - a.vx * s, a.y - a.vy * s
                b.x, b.y = b.x - b.vx * s, b.y - b.vy * s
                local push = (BD - d) / 2
                if push > 0 then
                  a.x, a.y = a.x - nx * push, a.y - ny * push
                  b.x, b.y = b.x + nx * push, b.y + ny * push
                end
              end
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
      local v = b.vx * b.vx + b.vy * b.vy
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
    -- two balls closing at 1.2 px a step each cannot pass through one another
    local nsub = min(12, floor(sqrt(vmax) * dt / 1.2) + 1)
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

-- ---------------------------------------------------------------- dice
-- xorshift32 on Lua's 32-bit integers. Seeded from the clock the page opened
-- at, so luasim with the same --start plays the same frames.
local seed = 1
local function rand()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end

-- ---------------------------------------------------------------- the frame
local byname = {}                 -- the white and the six colours
local function index_balls()
  for _, b in ipairs(balls) do if b.kind ~= "red" then byname[b.kind] = b end end
end

local game = {player = 1, score = {0, 0}, frames = {0, 0}, brk = 0, starter = 1,
              on = "red", in_hand = true, strokes = 0, over = false, msg = nil}

local function reds_left()
  local n = 0
  for _, b in ipairs(balls) do if b.kind == "red" and b.alive then n = n + 1 end end
  return n
end

-- The balls that may be hit first and potted: Section 2 Rule 11, Section 3
-- Rule 3(g)-(h). "colour" is the colour of choice after a red; a colour name
-- is its turn in the final sequence.
local function balls_on()
  local out = {}
  if game.on == "red" then
    for _, b in ipairs(balls) do if b.kind == "red" and b.alive then out[#out + 1] = b end end
  elseif game.on == "colour" then
    for _, k in ipairs(COLOURS) do if byname[k].alive then out[#out + 1] = byname[k] end end
  elseif byname[game.on] and byname[game.on].alive then
    out[1] = byname[game.on]
  end
  return out
end

local function spot_free(x, y, except)
  for _, b in ipairs(balls) do
    if b.alive and b ~= except then
      local dx, dy = b.x - x, b.y - y
      if dx * dx + dy * dy < (BD + 0.2) * (BD + 0.2) then return false end
    end
  end
  return true
end

-- Section 3 Rule 7(e)-(g), (i): on its own spot; if that is occupied, on the
-- highest value spot available; if all are, as near its own spot as it can
-- go towards the Top Cushion - never touching another ball.
local SPOTS_BY_VALUE = {"black", "pink", "blue", "brown", "green", "yellow"}
local function respot(b)
  local s = KINDS[b.kind].spot
  local x, y = s[1], s[2]
  if not spot_free(x, y, b) then
    local done = false
    for _, name in ipairs(SPOTS_BY_VALUE) do
      local s2 = KINDS[name].spot
      if spot_free(s2[1], s2[2], b) then x, y, done = s2[1], s2[2], true; break end
    end
    if not done then
      while x < XMAX and not spot_free(x, y, b) do x = x + 0.5 end
    end
  end
  b.x, b.y, b.vx, b.vy, b.alive, b.drop = x, y, 0, 0, true, nil
end

-- ---------------------------------------------------------------- the player
local function seg_clear(x1, y1, x2, y2, s1, s2)
  local dx, dy = x2 - x1, y2 - y1
  local l2 = dx * dx + dy * dy
  if l2 < 1e-6 then return true end
  local lim = BD * BD * 0.98
  for i = 1, #balls do
    local b = balls[i]
    if b.alive and b ~= s1 and b ~= s2 then
      local t = ((b.x - x1) * dx + (b.y - y1) * dy) / l2
      if t < 0 then t = 0 elseif t > 1 then t = 1 end
      local qx, qy = x1 + dx * t - b.x, y1 + dy * t - b.y
      if qx * qx + qy * qy < lim then return false end
    end
  end
  return true
end

-- One pot: target T into pocket k with the white at (cx, cy). Returns nil if
-- a ball is in the way, the cut is too fine, the pocket cannot be entered
-- from there, or the pot needs more pace than a cue can give.
local function eval_pot(cx, cy, T, k)
  local p = POCKETS[k]
  local ax, ay = p[1], p[2]
  if k == 2 then ay = OY + 0.6 elseif k == 5 then ay = OY + TW - 0.6 end   -- across the mouth
  if k ~= 2 and k ~= 5 then                                                    -- into the corner
    ax = ax + ((k == 1 or k == 4) and -0.8 or 0.8)
    ay = ay + ((k <= 3) and -0.8 or 0.8)
  end
  local ux, uy = ax - T.x, ay - T.y
  local dtp = sqrt(ux * ux + uy * uy)
  if dtp < 0.5 then return nil end
  ux, uy = ux / dtp, uy / dtp
  if k == 2 or k == 5 then
    if abs(uy) < 0.45 then return nil end              -- a middle pocket takes a steep ball
  else
    local ex = (k == 1 or k == 4) and -1 or 1
    local ey = (k <= 3) and -1 or 1
    if (ux * ex + uy * ey) * 0.7071 < 0.45 then return nil end
  end
  local gx, gy = T.x - ux * BD, T.y - uy * BD          -- where the white must be at contact
  if gx < XMIN or gx > XMAX or gy < YMIN or gy > YMAX then return nil end
  local vx, vy = gx - cx, gy - cy
  local dcg = sqrt(vx * vx + vy * vy)
  if dcg < 0.5 then return nil end
  vx, vy = vx / dcg, vy / dcg
  local cosc = vx * ux + vy * uy
  if cosc < 0.3 then return nil end
  local cue = byname.white
  if not seg_clear(cx, cy, gx, gy, cue, T) or not seg_clear(T.x, T.y, ax, ay, cue, T) then return nil end
  local diff = dcg / 60 + dtp / 80 + (1 - cosc) * 2.2
  local vo = sqrt(2 * DECEL * dtp) + 14
  local vc = vo / (cosc * (1 + E_BALL) / 2)
  local v0 = sqrt(vc * vc + 2 * DECEL * dcg)
  if v0 > VMAX then return nil end
  return {T = T, k = k, gx = gx, gy = gy, dx = vx, dy = vy, ux = ux, uy = uy, cosc = cosc,
          dcg = dcg, p = max(0.03, min(0.97, 1.05 - 0.45 * diff)), v0 = v0, cx = cx, cy = cy}
end

-- What would be on after potting c: the colours after a red, a red after a
-- colour while reds remain, and so on down the sequence.
local function next_targets(c)
  local out = {}
  if game.on == "red" then
    for _, k in ipairs(COLOURS) do out[#out + 1] = byname[k] end
  elseif game.on == "colour" then
    if reds_left() > 0 then
      for _, b in ipairs(balls) do if b.kind == "red" and b.alive then out[#out + 1] = b end end
    else
      out[1] = byname.yellow
    end
  else
    for i, k in ipairs(COLOURS) do
      if k == game.on and COLOURS[i + 1] then out[1] = byname[COLOURS[i + 1]] end
    end
  end
  return out
end

-- How good a next pot looks with the white at (qx, qy): a pocketable angle
-- at a comfortable distance. No obstruction check - it is a guess about a
-- position, and it has to be cheap.
local function position_quality(qx, qy, targets, except)
  local best = 0
  for _, t in ipairs(targets) do
    if t ~= except then
      local dxq, dyq = t.x - qx, t.y - qy
      local d = sqrt(dxq * dxq + dyq * dyq)
      if d > BD then
        for k = 1, 6 do
          local p = POCKETS[k]
          local ux, uy = p[1] - t.x, p[2] - t.y
          local dp = sqrt(ux * ux + uy * uy)
          local cosc = (dxq * ux + dyq * uy) / (d * dp)
          if cosc > 0.35 then
            local q = cosc * max(0.2, 1 - abs(d - 24) / 70) * max(0.3, 1 - dp / 150)
            if q > best then best = q end
          end
        end
      end
    end
  end
  return best
end

local function fold(v, lo, hi)
  for _ = 1, 3 do
    if v < lo then v = 2 * lo - v elseif v > hi then v = 2 * hi - v end
  end
  return max(lo, min(hi, v))
end

-- Where the white stops after pot c played at mult times the minimum pace
-- with spin (-draw .. +follow), and how good the next pot is from there.
local SPINS, PACES = {-0.6, 0, 0.5}, {1.0, 1.4}
local function eval_position(c, mult, spin)
  local v0 = min(VMAX, c.v0 * mult)
  local vcs = v0 * v0 - 2 * DECEL * c.dcg
  if vcs <= 0 then return nil end
  local vc = sqrt(vcs)
  local jn = vc * c.cosc * (1 + E_BALL) / 2
  local wx = vc * c.dx - jn * c.ux + spin * vc * c.dx
  local wy = vc * c.dy - jn * c.uy + spin * vc * c.dy
  local ws = sqrt(wx * wx + wy * wy)
  local qx, qy = c.gx, c.gy
  if ws > 1 then
    local dist = ws * ws / (2 * DECEL) * 0.8
    qx, qy = c.gx + wx / ws * dist, c.gy + wy / ws * dist
    -- running into a pocket on the way is an in-off
    for k = 1, 6 do
      local p = POCKETS[k]
      local ex, ey = qx - c.gx, qy - c.gy
      local t = ((p[1] - c.gx) * ex + (p[2] - c.gy) * ey) / (ex * ex + ey * ey)
      if t > 0 and t < 1 then
        local ox, oy = c.gx + ex * t - p[1], c.gy + ey * t - p[2]
        if ox * ox + oy * oy < 12 then return nil end
      end
    end
    qx, qy = fold(qx, XMIN, XMAX), fold(qy, YMIN, YMAX)
  end
  local q = position_quality(qx, qy, next_targets(c), c.T)
  return {v0 = v0, spin = spin, qx = qx, qy = qy, q = q}
end

-- Where the white can go from in hand: Section 3 Rule 5, on or within the D.
local function d_positions()
  local out = {}
  local cands = {
    {BAULK_X - 0.3, CY}, {BAULK_X - 0.3, CY + D_R * 0.5}, {BAULK_X - 0.3, CY - D_R * 0.5},
    {BAULK_X - D_R * 0.6, CY + D_R * 0.6}, {BAULK_X - D_R * 0.6, CY - D_R * 0.6}, {BAULK_X - D_R * 0.8, CY},
  }
  for _, p in ipairs(cands) do if spot_free(p[1], p[2], byname.white) then out[#out + 1] = p end end
  if #out == 0 then out[1] = {BAULK_X - D_R * 0.9, CY} end
  return out
end

-- The thinking is spread over frames: a dozen candidate pots a frame, then
-- two positional options a frame for the best few. A shot takes about a
-- second to choose, which is about what it looks like it should take.
local plan
local function start_plan()
  local positions
  local cue = byname.white
  if game.in_hand then positions = d_positions() else positions = {{cue.x, cue.y}} end
  local jobs = {}
  for pi = 1, #positions do
    for _, t in ipairs(balls_on()) do
      for k = 1, 6 do jobs[#jobs + 1] = {pi, t, k} end
    end
  end
  plan = {positions = positions, jobs = jobs, j = 1, cands = {}, stage = 1, k = 1, o = 1, best = nil}
end

local function step_plan()
  if plan.stage == 1 then
    local stop = min(#plan.jobs, plan.j + 11)
    for j = plan.j, stop do
      local job = plan.jobs[j]
      local pos = plan.positions[job[1]]
      local c = eval_pot(pos[1], pos[2], job[2], job[3])
      if c then plan.cands[#plan.cands + 1] = c end
    end
    plan.j = stop + 1
    if plan.j > #plan.jobs then
      table.sort(plan.cands, function(a, b) return a.p > b.p end)
      plan.stage = 2
    end
  elseif plan.stage == 2 then
    local c = plan.cands[plan.k]
    if not c or plan.k > 3 then plan.stage = 3; return end
    for _ = 1, 2 do
      local oi = plan.o
      local mult = PACES[(oi - 1) // #SPINS + 1]
      local spin = SPINS[(oi - 1) % #SPINS + 1]
      local o = eval_position(c, mult, spin)
      if o then
        local weight = (game.on == "red") and 1 or (1 + 0.05 * KINDS[c.T.kind].v)
        local score = c.p * (1 - 0.15 * (mult - 1)) * weight * (0.45 + 0.55 * o.q)
        if not plan.best or score > plan.best.score then plan.best = {c = c, o = o, score = score} end
      end
      plan.o = oi + 1
      if plan.o > #SPINS * #PACES then plan.o = 1; plan.k = plan.k + 1; break end
    end
  end
end

-- A safety: a thin contact on a ball on, hard enough to send the white up the
-- table and back towards Baulk, far from anything easy.
local function plan_safety(cx, cy)
  local cue = byname.white
  local best
  for _, t in ipairs(balls_on()) do
    local dxt, dyt = t.x - cx, t.y - cy
    local d = sqrt(dxt * dxt + dyt * dyt)
    if d > BD then
      for side = -1, 1, 2 do
        local ax, ay = t.x - dyt / d * side * BD * 0.85, t.y + dxt / d * side * BD * 0.85
        if seg_clear(cx, cy, ax, ay, cue, t) then
          local score = t.x + rand() * 6
          if not best or score > best.score then best = {score = score, ax = ax, ay = ay, t = t, d = d} end
        end
      end
    end
  end
  if not best then
    local t = balls_on()[1]
    if not t then return nil end
    local dxt, dyt = t.x - cx, t.y - cy
    best = {ax = t.x, ay = t.y, t = t, d = sqrt(dxt * dxt + dyt * dyt)}
  end
  local dx, dy = best.ax - cx, best.ay - cy
  local l = sqrt(dx * dx + dy * dy)
  local run = best.d + 2 * (XMAX - best.t.x) + 30
  return {dx = dx / l, dy = dy / l, speed = min(VMAX * 0.85, sqrt(2 * DECEL * run)), spin = 0, target = best.t}
end

-- The break-off: from the D near the Yellow, a thin contact on the end red of
-- the back row, so the white comes back off the Top and side cushions.
local function plan_break()
  local c = byname.white
  c.x, c.y, c.alive = BAULK_X - 1.5, CY + D_R * (0.35 + 0.3 * rand()), true
  local best
  for _, b in ipairs(balls) do
    if b.kind == "red" and (not best or b.x > best.x + 0.1 or (abs(b.x - best.x) <= 0.1 and b.y > best.y)) then best = b end
  end
  local tx, ty = best.x, best.y + BD * (0.84 + 0.1 * rand())
  local dx, dy = tx - c.x, ty - c.y
  local d = sqrt(dx * dx + dy * dy)
  return {dx = dx / d, dy = dy / d, speed = 195 + 20 * rand(), spin = 0, target = best}
end

-- Aim is never perfect: an angle error that grows with the difficulty of the
-- pot, and now and then a real miss.
local function execute(shot, difficulty)
  local sigma = 0.003 + 0.007 * difficulty
  if rand() < 0.05 then sigma = sigma + 0.03 end
  local err = (rand() + rand() - 1) * sigma
  local dx, dy = shot.dx - shot.dy * err, shot.dy + shot.dx * err
  local l = sqrt(dx * dx + dy * dy)
  shot.dx, shot.dy = dx / l, dy / l
  shot.speed = min(VMAX, shot.speed * (0.96 + 0.08 * rand()))
  return shot
end

local function choose_shot()
  local b = plan.best
  local cue = byname.white
  local shot, pos
  if b and (b.c.p > 0.38 or rand() < b.c.p) and rand() > 0.04 then
    local c = b.c
    pos = {c.cx, c.cy}
    shot = execute({dx = c.dx, dy = c.dy, speed = b.o.v0, spin = b.o.spin, target = c.T}, 1.05 - c.p)
  else
    if game.in_hand then pos = {BAULK_X - 0.3, CY} else pos = {cue.x, cue.y} end
    shot = plan_safety(pos[1], pos[2]) or {dx = 1, dy = 0, speed = 80, spin = 0}
    shot = execute(shot, 0.3)
    if game.brk == 0 then game.msg, game.msg_t = "SAFETY", 1.2 end
  end
  if game.in_hand then
    cue.x, cue.y, cue.vx, cue.vy, cue.alive, cue.drop = pos[1], pos[2], 0, 0, true, nil
    game.in_hand = false
  end
  return shot
end

local function new_frame()
  rack()
  index_balls()
  game.score = {0, 0}
  game.brk, game.on, game.in_hand, game.strokes, game.over = 0, "red", false, 0, false
  game.player = game.starter
  game.starter = 3 - game.starter                  -- Section 3 Rule 3(b): the break alternates
end

-- After the balls stop: score, fouls and penalties (Section 3 Rules 3, 7,
-- 10 and 11), re-spotting, and whose turn it is next.
local function resolve(shot)
  local on, first = game.on, stroke.first
  local target_kind = shot.target and shot.target.kind
  local on_value = (on == "red") and 1 or (on == "colour") and KINDS[target_kind or "black"].v or KINDS[on].v
  local function is_on(b)
    if on == "red" then return b.kind == "red" end
    if on == "colour" then return b.kind ~= "red" and b.kind ~= "white" and b.kind == target_kind end
    return b.kind == on
  end
  local foul, penalty, points, cue_in = false, 0, 0, false
  if not first then
    foul, penalty = true, max(4, on_value)                                   -- 11(a)(vi)
  elseif not is_on(first) then
    foul, penalty = true, max(4, on_value, KINDS[first.kind].v)              -- 11(b)(iv)
  end
  for _, b in ipairs(stroke.potted) do
    if b.kind == "white" then
      foul, cue_in, penalty = true, true, max(penalty, 4, on_value)          -- 11(a)(vii)
    elseif is_on(b) then
      points = points + KINDS[b.kind].v
    else
      foul, penalty = true, max(penalty, 4, on_value, KINDS[b.kind].v)       -- 11(b)(iii)
    end
  end

  -- Colours go back on their spots unless potted in turn in the final
  -- sequence; the highest value first (7(f)).
  local sequence = on ~= "red" and on ~= "colour"
  local to_spot = {}
  for _, b in ipairs(stroke.potted) do
    if b.kind ~= "red" and b.kind ~= "white" and not (sequence and not foul and b.kind == on) then
      to_spot[#to_spot + 1] = b
    end
  end
  table.sort(to_spot, function(a, b) return KINDS[a.kind].v > KINDS[b.kind].v end)
  for _, b in ipairs(to_spot) do respot(b) end
  if cue_in then game.in_hand = true end                                     -- S2 Rule 9(a)(ii)

  local final_black = (on == "black") and (points > 0 or foul)               -- S3 Rule 4(a)
  game.strokes = game.strokes + 1
  if foul then
    game.score[3 - game.player] = game.score[3 - game.player] + penalty
    game.msg, game.msg_t = "FOUL " .. penalty, 1.6
    game.brk, game.player = 0, 3 - game.player
    if reds_left() > 0 then game.on = "red"
    elseif not sequence then game.on = "yellow" end
  elseif points > 0 then
    game.score[game.player] = game.score[game.player] + points
    game.brk = game.brk + points
    if on == "red" then game.on = "colour"
    elseif on == "colour" then game.on = reds_left() > 0 and "red" or "yellow"
    else
      local nxt
      for i, k in ipairs(COLOURS) do if k == on then nxt = COLOURS[i + 1] end end
      game.on = nxt
    end
  else
    game.brk, game.player = 0, 3 - game.player
    if reds_left() > 0 then game.on = "red"
    elseif not sequence then game.on = "yellow" end
  end

  -- The end: the final Black, a lost cause conceded (S2 Rule 1(a)), or a
  -- frame that has simply gone on too long for a wall.
  -- Points still on the table for the player at it: a red and a black for
  -- every red, the black after the last red if a colour is on, then 27 for
  -- the six colours - or only what is left of the sequence.
  local remaining
  if game.on == "red" then
    remaining = reds_left() * 8 + 27
  elseif game.on == "colour" then
    remaining = reds_left() * 8 + 7 + 27
  else
    remaining = 0
    local counting = false
    for _, k in ipairs(COLOURS) do
      if k == game.on then counting = true end
      if counting then remaining = remaining + KINDS[k].v end
    end
  end
  local diff = abs(game.score[1] - game.score[2])
  if (final_black or game.on == nil) and game.score[1] == game.score[2] then
    respot(byname.black)                                                     -- S3 Rule 4(b)
    game.on, game.in_hand, game.player = "black", true, (rand() < 0.5) and 1 or 2
  elseif final_black or game.on == nil or diff > remaining + 7 or game.strokes > 160 then
    game.over = true
    local w = game.score[1] > game.score[2] and 1 or 2
    game.frames[w] = game.frames[w] + 1
    game.msg, game.msg_t = "FRAME", 4.0
  end
end

-- ---------------------------------------------------------------- the flow
local phase, ptime = "start", 0
local shot = {dx = 1, dy = 0, speed = 0, spin = 0}
local aim_x, aim_y = 1, 0
local from_x, from_y = 1, 0

local function begin_aim(s)
  shot = s
  from_x, from_y = aim_x, aim_y
  phase, ptime = "aim", 0
end

-- ---------------------------------------------------------------- the HUD
-- As in a game: the time top right, the score top left, each on its own dark
-- plate so it reads over rail, cloth and balls alike, and drawn last so no
-- ball ever covers it. Both plates sit between the pockets, which stay in view.

-- Seven-segment digits, 4 x 7, as rects worked out once.
local SEGMENTS = {[0] = "abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg"}
local SEG_RECT = {a = {0, 0, 4, 1}, b = {3, 0, 1, 4}, c = {3, 3, 1, 4}, d = {0, 6, 4, 1},
                  e = {0, 3, 1, 4}, f = {0, 0, 1, 4}, g = {0, 3, 4, 1}}
local DIGIT = {}
for d = 0, 9 do
  local list = {}
  for s in SEGMENTS[d]:gmatch(".") do list[#list + 1] = SEG_RECT[s] end
  DIGIT[d] = list
end

local function seg_digit(x, y, d, r, g, b)
  local list = DIGIT[d]
  for i = 1, #list do
    local s = list[i]
    px.rect(x + s[1], y + s[2], s[3], s[4], r, g, b, true)
  end
end

local PLATE_EDGE = {120, 92, 30}
local function plate(x, y, w, h)
  px.rect(x, y, w, h, PLATE_EDGE[1], PLATE_EDGE[2], PLATE_EDGE[3], true)
  px.rect(x + 1, y + 1, w - 2, h - 2, 0, 0, 0, true)
end

-- HH:MM, white, with the colon blinking amber on the second. The plate ends
-- short of the top-right pocket.
local CLOCK_X = 92
local function draw_clock(t)
  local n = px.now()
  plate(CLOCK_X, 0, 25, 11)
  local x, y = CLOCK_X + 2, 2
  seg_digit(x, y, n.hour // 10, 255, 255, 255)
  seg_digit(x + 5, y, n.hour % 10, 255, 255, 255)
  if (t * 60) % 1 < 0.5 then
    px.rect(x + 10, y + 1, 1, 2, 255, 170, 0, true)
    px.rect(x + 10, y + 4, 1, 2, 255, 170, 0, true)
  end
  seg_digit(x + 12, y, n.min // 10, 255, 255, 255)
  seg_digit(x + 17, y, n.min % 10, 255, 255, 255)
end

-- A small ball in the colour of the ball on; after a red it cycles through
-- the colours, any of which may be played.
local function ball_dot(x, y, kind)
  local c = KINDS[kind].col
  px.rect(x, y + 1, 3, 1, c[1], c[2], c[3], true)
  px.rect(x + 1, y, 1, 3, c[1], c[2], c[3], true)
  px.pixel(x + 1, y + 1, min(255, c[1] + 90), min(255, c[2] + 90), min(255, c[3] + 90))
end

-- Both scores, the player at the table in yellow; then the break in amber,
-- or for a moment the foul that just happened.
local function draw_score(t)
  local s1, s2 = tostring(game.score[1]), tostring(game.score[2])
  local extra, er, eg, eb
  if game.msg and game.msg ~= "FRAME" then
    extra, er, eg, eb = game.msg, 255, 80, 40
  elseif game.brk > 0 then
    extra, er, eg, eb = "(" .. game.brk .. ")", 255, 170, 0
  end
  local w1, w2 = px.width(s1), px.width(s2)
  local w = 7 + w1 + 3 + w2 + (extra and (3 + px.width(extra)) or 0) + 1
  plate(9, 0, w, 9)
  if game.on then
    ball_dot(11, 3, game.on == "colour" and COLOURS[floor(t * 120) % 6 + 1] or game.on)
  end
  local x = 16
  local hot, cold = {255, 215, 0}, {185, 185, 185}
  local c1 = game.player == 1 and hot or cold
  local c2 = game.player == 2 and hot or cold
  px.text(x, 0, s1, c1[1], c1[2], c1[3])
  x = x + w1 + 3
  px.text(x, 0, s2, c2[1], c2[2], c2[3])
  if extra then px.text(x + w2 + 3, 0, extra, er, eg, eb) end
end

-- Between frames: the score of the frame just played and the frames won.
local function draw_frame_over()
  local w = 58
  local x, y = (W - w) // 2, 21
  plate(x, y, w, 21)
  local l1 = "FRAME"
  local l2 = game.score[1] .. " - " .. game.score[2]
  local l3 = "FRAMES " .. game.frames[1] .. "-" .. game.frames[2]
  px.text(x + (w - px.width(l1)) // 2, y, l1, 255, 170, 0)
  px.text(x + (w - px.width(l2)) // 2, y + 6, l2, 255, 255, 255)
  px.text(x + (w - px.width(l3)) // 2, y + 12, l3, 185, 185, 185)
end

-- ---------------------------------------------------------------- draw
function draw()
  local dt = frame_dt()
  ptime = ptime + dt
  if game.msg_t then
    game.msg_t = game.msg_t - dt
    if game.msg_t <= 0 then game.msg, game.msg_t = nil, nil end
  end

  if phase == "start" then
    local n = px.now()
    seed = (n.hour * 60 + n.min) * 2654435 + n.yday * 97 + 12345
    if seed == 0 then seed = 1 end
    new_frame()
    local s = plan_break()
    aim_x, aim_y = s.dx, s.dy
    begin_aim(s)
  elseif phase == "think" then
    if not plan then start_plan() end
    step_plan()
    if plan.stage == 3 and ptime > 0.6 then
      local s = choose_shot()
      plan = nil
      if not byname.white.alive then byname.white.alive = true end
      begin_aim(s)
    end
  elseif phase == "aim" then
    local k = min(1, ptime / 0.45)
    local ax, ay = from_x + (shot.dx - from_x) * k, from_y + (shot.dy - from_y) * k
    local l = sqrt(ax * ax + ay * ay)
    if l > 0.01 then aim_x, aim_y = ax / l, ay / l else aim_x, aim_y = shot.dx, shot.dy end
    if ptime > 0.7 then aim_x, aim_y, phase, ptime = shot.dx, shot.dy, "draw", 0 end
  elseif phase == "draw" and ptime > 0.65 then
    phase, ptime = "strike", 0
  elseif phase == "strike" and ptime > 0.06 then
    stroke = {first = nil, potted = {}, spin = shot.spin}
    local c = byname.white
    c.vx, c.vy = shot.dx * shot.speed, shot.dy * shot.speed
    phase, ptime = "roll", 0
  elseif phase == "roll" then
    if not simulate(dt) and ptime > 0.25 then
      resolve(shot)
      phase, ptime = "pause", 0
    end
  elseif phase == "pause" and ptime > 0.5 then
    if game.over then phase, ptime = "over", 0 else phase, ptime = "think", 0 end
  elseif phase == "over" and ptime > 4.0 then
    new_frame()
    local s = plan_break()
    begin_aim(s)
  end

  draw_static()
  for _, b in ipairs(balls) do
    if b.alive then
      draw_sprite(SPRITES[b.kind][1], b.x, b.y)
    elseif b.drop then
      draw_sprite(SPRITES[b.kind][b.drop < 0.12 and 2 or 3], b.x, b.y)
    end
  end

  local c = byname.white
  if c.alive and (phase == "aim" or phase == "draw" or phase == "strike") then
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

  -- the HUD, last of all, so nothing is ever drawn over it
  local t = px.t()
  if phase == "over" then draw_frame_over() end
  draw_score(t)
  draw_clock(t)
end

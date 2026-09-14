-- football_clock.lua - a football match that plays itself: Atletico Madrid
-- against Real Madrid, told apart by their kit colours only, with a
-- broadcast score bug and the real time in the corner.
--
-- The pitch follows the IFAB Laws of the Game 2026/27, Law 1 (The Field of
-- Play); the halves, Law 7.

local W, H = px.size()
local floor, sqrt, min, max, abs, sin, cos, pi =
  math.floor, math.sqrt, math.min, math.max, math.abs, math.sin, math.cos, math.pi

-- ---------------------------------------------------------------- the view
-- Top-down, with the pitch's width foreshortened the way a broadcast camera
-- up in the stand sees it: 1.10 px a metre along the touchline, 0.75 across.
-- True perspective was the alternative. At 64 px tall it shrinks the far
-- touchline's players to a pixel and bends every line into jaggies. This
-- keeps lines straight, turns circles into ellipses, as on television, and
-- gives every player the same size.
--
-- Law 1: for international matches the touchline is 100-110 m and the goal
-- line 64-75 m. 105 x 68 m sits inside both.
local PL, PW = 105, 68
local GX0, GX1 = 6, 121            -- goal lines, px
local GY0, GY1 = 10, 61            -- touchlines, px
local SX = (GX1 - GX0) / PL        -- 1.095 px per m
local SY = (GY1 - GY0) / PW        -- 0.750 px per m
local function sx(x) return GX0 + x * SX end
local function sy(y) return GY0 + y * SY end

-- ---------------------------------------------------------------- dice
-- xorshift32 on Lua's 32-bit integers. The crowd is drawn from a fixed seed;
-- the match reseeds from the clock the page opened at, so luasim with the
-- same --start plays the same match.
local seed = 12345
local function rand()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end

-- ---------------------------------------------------------------- colours
local GRASS_A, GRASS_B = {20, 112, 32}, {32, 138, 44}   -- mown stripes
local RUNOFF   = {16, 92, 26}
local LINE     = {225, 235, 225}
local NET, NET_DK = {205, 210, 205}, {95, 105, 95}

-- ---------------------------------------------------------------- static draw list
-- Built once. The frame starts with a clear to one of the stripes, so only
-- the other stripes and the strips around the pitch are painted.
local STATIC = {}
local function rect(x, y, w, h, c) STATIC[#STATIC + 1] = {1, x, y, w, h, c[1], c[2], c[3]} end
local function dot(x, y, c) STATIC[#STATIC + 1] = {2, x, y, c[1], c[2], c[3]} end

-- The stands along the top: a crowd of short runs of colour in rows of seats,
-- the home end red and white, the away end white and violet.
local CROWD_HOME = {{205, 30, 40}, {235, 235, 235}, {40, 60, 170}, {150, 25, 30}}
local CROWD_AWAY = {{235, 235, 235}, {110, 60, 170}, {220, 190, 60}, {180, 180, 190}}
local CROWD_ANY  = {{60, 45, 40}, {120, 90, 70}, {80, 80, 90}}
rect(0, 0, W, 7, {18, 16, 22})
local CROWD = {}                   -- kept apart: the crowd jumps when a goal goes in
for row = 0, 2 do
  local x = 0
  while x < W do
    local w = 2 + floor(rand() * 3)
    local pal = rand() < 0.3 and CROWD_ANY or (x < W / 2 and CROWD_HOME or CROWD_AWAY)
    local c = pal[1 + floor(rand() * #pal)]
    local k = 0.55 + 0.35 * rand()
    CROWD[#CROWD + 1] = {x, 1 + row * 2, w, floor(c[1] * k), floor(c[2] * k), floor(c[3] * k)}
    x = x + w + (rand() < 0.35 and 1 or 0)
  end
end
-- advertising boards: plain coloured panels, no names, dimmer than the pitch
local BOARDS = {{0, 50, 115}, {140, 22, 30}, {160, 160, 160}, {16, 16, 16}, {165, 118, 0}, {0, 92, 56}}
for i = 0, 7 do
  rect(i * 16, 7, 16, 2, BOARDS[i % #BOARDS + 1])
  rect(i * 16, 63, 16, 1, BOARDS[(i + 2) % #BOARDS + 1])
end

-- grass: the run-off round the pitch, then every other mown stripe
rect(0, 9, W, 1, RUNOFF)
rect(0, GY1 + 1, W, 63 - GY1 - 1, RUNOFF)
rect(0, GY0, GX0, GY1 - GY0 + 1, RUNOFF)
rect(GX1 + 1, GY0, W - GX1 - 1, GY1 - GY0 + 1, RUNOFF)
for i = 1, 11, 2 do
  local x0, x1 = floor(sx(i * PL / 12) + 0.5), floor(sx((i + 1) * PL / 12) + 0.5)
  rect(x0, GY0, x1 - x0, GY1 - GY0 + 1, GRASS_B)
end

-- Law 1 markings. Lines are at most 12 cm wide, a tenth of a pixel here;
-- they are drawn one pixel wide.
local function hline(x0, x1, y)
  local a, b = floor(sx(x0) + 0.5), floor(sx(x1) + 0.5)
  rect(a, floor(sy(y) + 0.5), b - a + 1, 1, LINE)
end
local function vline(x, y0, y1)
  local a, b = floor(sy(y0) + 0.5), floor(sy(y1) + 0.5)
  rect(floor(sx(x) + 0.5), a, 1, b - a + 1, LINE)
end
hline(0, PL, 0); hline(0, PL, PW)                  -- touchlines
vline(0, 0, PW); vline(PL, 0, PW)                  -- goal lines
vline(PL / 2, 0, PW)                               -- halfway line

local marked = {}
local function mark(x, y)
  local mx, my = floor(sx(x) + 0.5), floor(sy(y) + 0.5)
  local key = mx * 100 + my
  if not marked[key] then marked[key] = true; dot(mx, my, LINE) end
end
local function arc(cx, cy, r, keep)
  for i = 0, 119 do
    local a = i * 2 * pi / 120
    local x, y = cx + r * cos(a), cy + r * sin(a)
    if not keep or keep(x, y) then mark(x, y) end
  end
end
arc(PL / 2, PW / 2, 9.15)                          -- centre circle, radius 9.15 m
mark(PL / 2, PW / 2)                               -- centre mark
local HALF_GOAL = 7.32 / 2                         -- 7.32 m between the posts
for side = 0, 1 do
  local function X(d) return side == 0 and d or PL - d end
  local ga = HALF_GOAL + 5.5                       -- goal area: 5.5 m from each post
  hline(min(X(0), X(5.5)), max(X(0), X(5.5)), PW / 2 - ga)
  hline(min(X(0), X(5.5)), max(X(0), X(5.5)), PW / 2 + ga)
  vline(X(5.5), PW / 2 - ga, PW / 2 + ga)          -- ... and 5.5 m into the field
  local pa = HALF_GOAL + 16.5                      -- penalty area: 16.5 m from each post
  hline(min(X(0), X(16.5)), max(X(0), X(16.5)), PW / 2 - pa)
  hline(min(X(0), X(16.5)), max(X(0), X(16.5)), PW / 2 + pa)
  vline(X(16.5), PW / 2 - pa, PW / 2 + pa)         -- ... and 16.5 m into the field
  mark(X(11), PW / 2)                              -- penalty mark, 11 m from the goal
  arc(X(11), PW / 2, 9.15, function(x)             -- the arc of 9.15 m outside the area
    return (side == 0 and x > 16.6) or (side == 1 and x < PL - 16.6)
  end)
end

-- Goals behind the goal lines, posts 7.32 m apart, with a net.
local GOAL_Y0, GOAL_Y1 = floor(sy(PW / 2 - HALF_GOAL) + 0.5), floor(sy(PW / 2 + HALF_GOAL) + 0.5)
for side = 0, 1 do
  local gx = side == 0 and GX0 - 3 or GX1 + 1
  rect(gx, GOAL_Y0, 3, GOAL_Y1 - GOAL_Y0 + 1, NET_DK)
  for y = GOAL_Y0, GOAL_Y1 do
    for x = gx, gx + 2 do
      if (x + y) % 2 == 0 then dot(x, y, NET) end
    end
  end
  local post = side == 0 and GX0 - 1 or GX1 + 1
  dot(post, GOAL_Y0, LINE); dot(post, GOAL_Y1, LINE)
end
for _, c in ipairs({{GX0, GY0}, {GX1, GY0}, {GX0, GY1}, {GX1, GY1}}) do
  dot(c[1], c[2] - 1, {250, 220, 0})               -- corner flags
end

local function draw_static(crowd_jump)
  px.clear(GRASS_A[1], GRASS_A[2], GRASS_A[3])
  for i = 1, #STATIC do
    local o = STATIC[i]
    if o[1] == 1 then px.rect(o[2], o[3], o[4], o[5], o[6], o[7], o[8], true)
    else px.pixel(o[2], o[3], o[4], o[5], o[6]) end
  end
  for i = 1, #CROWD do
    local c = CROWD[i]
    local up = (crowd_jump and (i + crowd_jump) % 3 == 0) and 1 or 0
    px.rect(c[1], c[2] - up, c[3], 1, c[4], c[5], c[6], true)
  end
end

-- ---------------------------------------------------------------- players
-- 3 x 4 px: a head, two rows of shirt, a row of shorts that doubles as the
-- running legs, and a shadow on the grass. Kits by colour only: Atletico in
-- red and white stripes with blue shorts; Real Madrid all white, with a dark
-- navy edge so a player still reads where he crosses a white line.
local SKINS = {{235, 185, 145}, {196, 138, 98}, {128, 86, 56}}
local SHADOW = {8, 60, 16}
local KITS = {
  atm    = {rows = {{"R", "W", "R"}, {"R", "W", "R"}}, legs = "B"},
  rma    = {rows = {{"W", "W", "W"}, {"W", "W", "N"}}, legs = "W", edge = "N"},
  atm_gk = {rows = {{"Y", "Y", "Y"}, {"Y", "Y", "Y"}}, legs = "K"},
  rma_gk = {rows = {{"M", "M", "M"}, {"M", "M", "M"}}, legs = "K"},
}
local PAL = {
  R = {220, 28, 40}, W = {250, 250, 250}, B = {40, 75, 210}, N = {36, 36, 110},
  Y = {250, 220, 0}, M = {225, 40, 170}, K = {30, 30, 30},
}

local function build_player(kit, skin, step)
  local sp = {n = 0, dx = {}, dy = {}, r = {}, g = {}, b = {}}
  local function put(dx, dy, c)
    local i = sp.n + 1
    sp.n, sp.dx[i], sp.dy[i], sp.r[i], sp.g[i], sp.b[i] = i, dx, dy, c[1], c[2], c[3]
  end
  put(2, 4, SHADOW); put(3, 3, SHADOW)
  put(1, 0, skin)
  for r = 1, 2 do
    for c = 1, 3 do put(c - 1, r, PAL[kit.rows[r][c]]) end
  end
  if kit.edge then put(3, 1, PAL[kit.edge]) end
  local legs = PAL[kit.legs]
  if step == 0 then put(0, 3, legs); put(2, 3, legs) else put(1, 3, legs) end
  return sp
end

-- SPR[kit][skin][step + 1], built once
local SPR = {}
for _, name in ipairs({"atm", "rma", "atm_gk", "rma_gk"}) do
  SPR[name] = {}
  for s = 1, 3 do
    SPR[name][s] = {build_player(KITS[name], SKINS[s], 0), build_player(KITS[name], SKINS[s], 1)}
  end
end

local function draw_sprite(sp, x, y)
  local dx, dy, r, g, b = sp.dx, sp.dy, sp.r, sp.g, sp.b
  for i = 1, sp.n do px.pixel(x + dx[i], y + dy[i], r[i], g[i], b[i]) end
end

-- ---------------------------------------------------------------- formations
-- For a team attacking towards +x from its goal at x = 0, in metres.
local F442 = {{4, 34}, {18, 8}, {16, 26}, {16, 42}, {18, 60}, {34, 10}, {31, 27}, {31, 41}, {34, 58}, {46, 29}, {46, 39}}
local F433 = {{4, 34}, {18, 8}, {16, 26}, {16, 42}, {18, 60}, {30, 20}, {28, 34}, {30, 48}, {44, 12}, {47, 34}, {44, 56}}

-- ---------------------------------------------------------------- time
-- Play runs at real speed, from px.t(); a long gap is capped at 0.1 s so the
-- match slows rather than jumps. The match clock runs 20 times faster: a
-- match minute is three real seconds, a half 2 min 15 s, and a whole match
-- with half-time and full-time fits in about five minutes on the wall.
local RATIO = 20
local last_t
local function frame_dt()
  local t = px.t()
  local dt = last_t and (t - last_t) or 0.05
  if dt < 0 then dt = dt + 1 end
  last_t = t
  return min(0.1, dt * 60)
end

-- ---------------------------------------------------------------- the match
local G, ROLL, BOUNCE = 9.8, 3.0, 0.45     -- gravity, rolling deceleration m/s^2, bounce
local players, ball = {}, {}
local match = {score = {0, 0}, half = 1, clock = 0, phase = "setup", timer = 0,
               dir = {1, -1}, kickoff = 1, first_kickoff = 1, possession = nil,
               last_team = 1, restart = nil, scorer = nil, jump = nil, msg = nil}

local function new_players()
  players = {}
  for team = 1, 2 do
    local form = team == 1 and F442 or F433
    for role = 1, 11 do
      players[#players + 1] = {
        team = team, role = role, x = 0, y = 0, vx = 0, vy = 0,
        kit = (team == 1 and "atm" or "rma") .. (role == 1 and "_gk" or ""),
        skin = (role * 7 + team * 3) % 3 + 1, stride = rand(), form = form[role],
        tx = 0, ty = 0, speed = 0, stun = 0, lock = 0, decide = 0,
      }
    end
  end
end

-- The formation spot for p, shifted with the ball: up the pitch when his team
-- has it, back when it does not, and across towards the ball's side.
local function shape_target(p, press_back)
  local dir = match.dir[p.team]
  local bx = dir > 0 and ball.x or PL - ball.x
  local by = dir > 0 and ball.y or PW - ball.y
  local f = p.form
  local ux, uy
  if p.role == 1 then
    ux = 2.5 + max(0, min(8, (bx - 20) * 0.08))
    uy = PW / 2 + (by - PW / 2) * 0.15
  else
    local have = match.possession == p.team
    ux = f[1] * 0.85 + (bx - 40) * 0.5 + (have and 12 or -6)
    uy = f[2] + (by - PW / 2) * 0.3
    if press_back then ux = min(ux, PL / 2 - 2) end
    ux = max(6, min(PL - 4, ux))
    uy = max(2, min(PW - 2, uy))
  end
  if dir > 0 then return ux, uy else return PL - ux, PW - uy end
end

local function place_for_kickoff(team_to_kick)
  for _, p in ipairs(players) do
    local x, y = shape_target(p, true)
    p.x, p.y, p.vx, p.vy = x, y, 0, 0
  end
  ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz, ball.owner = PL / 2, PW / 2, 0, 0, 0, 0, nil
  -- the kicking team's two most advanced outfield players stand at the centre
  local best, second
  for _, p in ipairs(players) do
    if p.team == team_to_kick and p.role > 1 then
      local d = abs(p.x - PL / 2) + abs(p.y - PW / 2)
      if not best or d < best.d then second = best; best = {p = p, d = d}
      elseif not second or d < second.d then second = {p = p, d = d} end
    end
  end
  local dir = match.dir[team_to_kick]
  best.p.x, best.p.y = PL / 2 - dir * 0.6, PW / 2
  second.p.x, second.p.y = PL / 2 - dir * 1.5, PW / 2 + 3
  match.restart = {kind = "kickoff", team = team_to_kick, taker = best.p, mate = second.p, x = PL / 2, y = PW / 2}
  match.possession = team_to_kick
end

local function dist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return sqrt(dx * dx + dy * dy)
end

-- ---------------------------------------------------------------- the ball
local function kick(p, vx, vy, vz)
  ball.owner = nil
  ball.vx, ball.vy, ball.vz = vx, vy, vz
  ball.z = max(ball.z, vz > 0 and 0.1 or 0)
  ball.last, ball.decided = p, nil
  match.last_team = p.team
  p.lock = 0.35
end

-- Pace for a ground ball to arrive at d metres still rolling at `arrive`.
local function ground_pace(d, arrive)
  return min(26, sqrt(2 * ROLL * d + arrive * arrive))
end

-- Nobody hits it perfectly: a little off in angle and pace, more on a long
-- ball, and `care` (1 normally) scales it for a hurried clearance.
local function pass_to(p, tx, ty, lofted, care)
  local dx, dy = tx - ball.x, ty - ball.y
  local d = max(0.5, sqrt(dx * dx + dy * dy))
  local err = (rand() + rand() - 1) * (0.05 + d * 0.002) * (care or 1)
  dx, dy = dx - dy * err, dy + dx * err
  local pace = 0.92 + rand() * 0.16
  if lofted then
    local v = min(24, 7 + d * 0.45)
    local tflight = d / v
    kick(p, dx / d * v * pace, dy / d * v * pace, G * tflight / 2)
  else
    local v = ground_pace(d, 4) * pace
    kick(p, dx / d * v, dy / d * v, 0)
  end
end

local function ball_physics(dt)
  if ball.owner then
    local o = ball.owner
    local s = sqrt(o.vx * o.vx + o.vy * o.vy)
    local fx, fy = 0, 0
    if s > 0.3 then fx, fy = o.vx / s, o.vy / s else fx = match.dir[o.team] end
    ball.x, ball.y, ball.z = o.x + fx * 0.8, o.y + fy * 0.8, 0
    ball.vx, ball.vy, ball.vz = o.vx, o.vy, 0
    return
  end
  if ball.z > 0 or ball.vz > 0 then
    ball.vz = ball.vz - G * dt
    ball.z = ball.z + ball.vz * dt
    if ball.z <= 0 then
      ball.z = 0
      if ball.vz < -2 then
        ball.vz = -ball.vz * BOUNCE
        ball.vx, ball.vy = ball.vx * 0.75, ball.vy * 0.75
      else
        ball.vz = 0
      end
    end
  end
  ball.x, ball.y = ball.x + ball.vx * dt, ball.y + ball.vy * dt
  if ball.z == 0 then
    local s = sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
    if s > 0 then
      local ns = max(0, s - ROLL * dt)
      ball.vx, ball.vy = ball.vx * ns / s, ball.vy * ns / s
    end
  end
end

-- ---------------------------------------------------------------- reading the play
local SPRINT, RUN, JOG = 7.2, 5.4, 3.0

local function dist2(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return dx * dx + dy * dy
end

-- The goal line a team attacks.
local function goal_of(team) return match.dir[team] > 0 and PL or 0 end

local function pressure(team, x, y, r)
  local n, r2 = 0, r * r
  for _, q in ipairs(players) do
    if q.team ~= team and dist2(q.x, q.y, x, y) < r2 then n = n + 1 end
  end
  return n
end

-- How far the nearest opponent is from getting a foot to a ball played from
-- (x0, y0) to (x1, y1) at pace v, allowing for how far he can run while it
-- travels. Negative means it gets cut out.
local function lane_margin(team, x0, y0, x1, y1, v)
  local dx, dy = x1 - x0, y1 - y0
  local l2 = dx * dx + dy * dy
  if l2 < 0.01 then return 10 end
  local len, worst = sqrt(l2), 10
  for _, q in ipairs(players) do
    if q.team ~= team then
      local t = ((q.x - x0) * dx + (q.y - y0) * dy) / l2
      if t > 0 and t < 1.05 then
        local gap = dist(x0 + dx * t, y0 + dy * t, q.x, q.y)
        local m = gap - (0.9 + t * len / max(4, v) * 5.5)
        if m < worst then worst = m end
      end
    end
  end
  return worst
end

local function steer(p, tx, ty, speed, dt)
  local dx, dy = tx - p.x, ty - p.y
  local d = sqrt(dx * dx + dy * dy)
  local wx, wy = 0, 0
  if d > 0.15 then
    local s = speed * min(1, d / 2.5)
    wx, wy = dx / d * s, dy / d * s
  end
  local k = min(1, dt * 5)
  p.vx, p.vy = p.vx + (wx - p.vx) * k, p.vy + (wy - p.vy) * k
  p.x, p.y = p.x + p.vx * dt, p.y + p.vy * dt
  p.x, p.y = max(-3, min(PL + 3, p.x)), max(-2, min(PW + 2, p.y))
  local s = abs(p.vx) + abs(p.vy)
  if s > 0.6 then p.stride = p.stride + dt * s * 0.9 end
end

-- ---------------------------------------------------------------- the keeper
local function keeper_target(p)
  local gx = match.dir[p.team] > 0 and 0 or PL
  local dirx = match.dir[p.team]
  local dx, dy = ball.x - gx, ball.y - PW / 2
  local d = max(1, sqrt(dx * dx + dy * dy))
  local out = min(5, 1.2 + d * 0.06)
  local tx, ty = gx + dx / d * out, PW / 2 + dy / d * out
  if ball.shot and not ball.owner and ball.vx * dirx < -3 then
    local t = (gx + dirx - ball.x) / ball.vx
    if t > 0 then ty = ball.y + ball.vy * t end
    tx = gx + dirx
  end
  ty = max(PW / 2 - HALF_GOAL - 2.5, min(PW / 2 + HALF_GOAL + 2.5, ty))
  return tx, ty
end

-- A shot reaching the keeper's line is decided once: caught, parried, or past.
local function keeper_save()
  if not ball.shot or ball.owner or ball.decided then return end
  for _, k in ipairs(players) do
    if k.role == 1 then
      local own = match.dir[k.team] > 0 and 0 or PL
      local toward = (own == 0 and ball.vx < 0) or (own == PL and ball.vx > 0)
      if toward and abs(ball.x - k.x) < 1.3 then
        ball.decided = true
        local dy = abs(ball.y - k.y)
        local speed = sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
        k.lock = 0.8
        if dy < 3.6 and ball.z < 2.7 then
          -- he stops most of what comes at him, and far less of what is
          -- struck hard or placed inside a post: about seven in ten on target
          local placed = abs(ball.y - PW / 2) > HALF_GOAL - 1.2 and 0.3 or 0
          local chance = 0.85 - dy / 3.6 * 0.55 - max(0, speed - 16) * 0.04 - placed
          if rand() < chance then
            match.msg, match.msg_t = "SAVE", 1.4
            ball.last, match.last_team = k, k.team
            if speed < 22 and ball.z < 1.8 and rand() < 0.55 then
              ball.owner, ball.shot = k, nil
              match.possession = k.team
              k.decide = 1.4
            else
              ball.vx = -ball.vx * 0.3
              ball.vy = ball.vy * 0.4 + (rand() - 0.5) * 14
              ball.vz = 2 + rand() * 3
              ball.shot = nil
              k.lock = 0.5
            end
          end
        end
        return
      end
    end
  end
end

-- ---------------------------------------------------------------- the player on the ball
local function carrier_decide(p)
  local team, dir = p.team, match.dir[p.team]
  local gx = goal_of(team)
  local ux = dir > 0 and p.x or PL - p.x
  local dgoal = dist(p.x, p.y, gx, PW / 2)
  local press = pressure(team, p.x, p.y, 4)

  if p.role > 1 and dgoal < 32 and abs(p.y - PW / 2) < 20 then
    local chance = (32 - dgoal) / 19 * (press > 1 and 0.7 or 1)
    if rand() < chance * 1.2 then
      local aim = PW / 2 + (rand() * 2 - 1) * (HALF_GOAL + 2.2)
      local dx, dy = gx - p.x, aim - p.y
      local d = sqrt(dx * dx + dy * dy)
      local v = 19 + rand() * 9
      kick(p, dx / d * v, dy / d * v, rand() < 0.5 and (1 + rand() * 5) or 0.3)
      ball.shot = true
      return
    end
  end

  if p.role > 1 and ux > 76 and (p.y < 15 or p.y > PW - 15) and rand() < 0.45 then
    local tx = dir > 0 and PL - 7 - rand() * 7 or 7 + rand() * 7
    pass_to(p, tx, PW / 2 + (rand() * 2 - 1) * 10, true, 1.5)
    ball.cross = true
    return
  end

  -- under pressure deep in his own half, a defender just clears it
  if p.role >= 2 and p.role <= 5 and ux < 30 and press > 0 and rand() < 0.7 then
    pass_to(p, p.x + dir * (35 + rand() * 20), p.y + (rand() - 0.5) * 70, true, 3)
    return
  end

  local best, bs
  for _, q in ipairs(players) do
    if q.team == team and q ~= p then
      local lx, ly = q.x + q.vx * 0.5 + (q.role >= 9 and dir * 4 or 0), q.y + q.vy * 0.5
      local d = dist(p.x, p.y, lx, ly)
      if d > 5 and d < 48 then
        local margin = lane_margin(team, p.x, p.y, lx, ly, ground_pace(d, 4))
        local qux = dir > 0 and lx or PL - lx
        local open = 10
        for _, o in ipairs(players) do
          if o.team ~= team then open = min(open, dist(o.x, o.y, lx, ly)) end
        end
        local score = (qux - ux) * 0.14 + min(margin, 4) * 0.35 + min(open, 8) * 0.12
                      - abs(d - 16) * 0.02 + rand() * 0.6
        if q.role == 1 then score = score - 2 end
        if margin < 0 then score = score - 2.5 end
        if not bs or score > bs then best, bs = {q = q, x = lx, y = ly, margin = margin, d = d}, score end
      end
    end
  end
  local want = p.role == 1 or press > 1 or (press > 0 and rand() < 0.6) or rand() < 0.25
  if best and want and (bs > 0.6 or p.role == 1) then
    pass_to(p, best.x, best.y, best.margin < 0 and best.d > 16)
    ball.target = best.q
    return
  end
  p.decide = (ux > 68 and 0.35 or 0.7) + rand() * 0.6   -- carry it on a while
end

-- ---------------------------------------------------------------- restarts
local function set_restart(kind, team, x, y)
  local taker, mate
  if kind == "goalkick" then
    for _, p in ipairs(players) do if p.team == team and p.role == 1 then taker = p end end
  else
    local bd, bd2
    for _, p in ipairs(players) do
      if p.team == team and p.role > 1 then
        local d = dist(p.x, p.y, x, y)
        if not bd or d < bd then mate, bd2 = taker, bd; taker, bd = p, d
        elseif not bd2 or d < bd2 then mate, bd2 = p, d end
      end
    end
  end
  match.restart = {kind = kind, team = team, x = x, y = y, taker = taker, mate = mate}
  match.phase, match.timer, match.possession = "restart", 0, team
  ball.owner, ball.shot, ball.cross, ball.target = nil, nil, nil, nil
  ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz = x, y, 0, 0, 0, 0
  if kind == "corner" then match.msg, match.msg_t = "CORNER", 1.6
  elseif kind == "goalkick" then match.msg, match.msg_t = "GOAL KICK", 1.4 end
end

local function set_kickoff(team) set_restart("kickoff", team, PL / 2, PW / 2) end

-- Where each player stands while a restart is set up.
local function restart_spot(p, r)
  local dir = match.dir[p.team]
  if p == r.taker then
    if r.kind == "throw" then return r.x, r.y < PW / 2 and r.y - 0.8 or r.y + 0.8 end
    if r.kind == "corner" then
      return r.x + (r.x < PL / 2 and -0.8 or 0.8), r.y + (r.y < PW / 2 and -0.8 or 0.8)
    end
    return r.x - dir * 0.7, r.y
  end
  if r.kind == "kickoff" and p == r.mate then return PL / 2 - dir * 1.5, PW / 2 + 3 end
  if r.kind == "corner" and p.role > 1 then
    local gx = r.x < PL / 2 and 0 or PL
    local inward = gx == 0 and 1 or -1
    if p.team == r.team and p.role >= 6 then
      return gx + inward * (7 + (p.role * 37) % 9), PW / 2 + ((p.role * 13) % 11 - 5) * 1.6
    elseif p.team ~= r.team and p.role <= 8 then
      return gx + inward * (4 + (p.role * 29) % 8), PW / 2 + ((p.role * 17) % 11 - 5) * 1.5
    end
  end
  if p.role == 1 then return keeper_target(p) end
  return shape_target(p, r.kind == "kickoff")
end

local function take_restart(r)
  local p = r.taker
  if r.kind == "kickoff" then
    pass_to(p, r.mate.x, r.mate.y, false)
    ball.target = r.mate
  elseif r.kind == "throw" then
    local best, bd
    for _, q in ipairs(players) do
      if q.team == p.team and q ~= p and q.role > 1 then
        local d = dist(q.x, q.y, r.x, r.y) + rand() * 6
        if d > 5 and d < 26 and (not bd or d < bd) then best, bd = q, d end
      end
    end
    best = best or r.mate
    local d = max(1, dist(best.x, best.y, r.x, r.y))
    local v = min(13, 4 + d * 0.55)
    kick(p, (best.x - r.x) / d * v, (best.y - r.y) / d * v, G * (d / v) / 2)
    ball.target = best
  elseif r.kind == "corner" then
    local gx = r.x < PL / 2 and 0 or PL
    local inward = gx == 0 and 1 or -1
    pass_to(p, gx + inward * (6 + rand() * 7), PW / 2 + (rand() - 0.5) * 12, true)
    ball.cross = true
  else
    local best, bs
    for _, q in ipairs(players) do
      if q.team == p.team and q.role > 1 then
        local qux = match.dir[q.team] > 0 and q.x or PL - q.x
        local open = 10
        for _, o in ipairs(players) do
          if o.team ~= q.team then open = min(open, dist(o.x, o.y, q.x, q.y)) end
        end
        local s = open - abs(qux - 45) * 0.1 + rand() * 3
        if not bs or s > bs then best, bs = q, s end
      end
    end
    pass_to(p, best.x, best.y, true)
    ball.target = best
  end
  match.phase = "play"
end

local function restart_update(dt)
  local r = match.restart
  match.timer = match.timer + dt
  local ready = match.timer > (r.kind == "kickoff" and 2.5 or 1.4)
  for _, p in ipairs(players) do
    local tx, ty = restart_spot(p, r)
    steer(p, tx, ty, (p == r.taker) and RUN or RUN * 0.8, dt)
    if p == r.taker and dist(p.x, p.y, tx, ty) > 1.2 then ready = false end
    if r.kind == "kickoff" then
      local ux = match.dir[p.team] > 0 and p.x or PL - p.x
      if ux > PL / 2 + 0.5 and match.timer < 12 then ready = false end
    end
  end
  if ready then take_restart(r) end
end

-- ---------------------------------------------------------------- goals and the ball going out
local function goal(team)
  match.score[team] = match.score[team] + 1
  match.phase, match.timer = "goal", 0
  match.scorer = (ball.last and ball.last.team == team) and ball.last or nil
  match.kickoff = 3 - team
  ball.x = ball.x < PL / 2 and -1.6 or PL + 1.6
  ball.y = max(PW / 2 - HALF_GOAL + 0.3, min(PW / 2 + HALF_GOAL - 0.3, ball.y))
  ball.vx, ball.vy, ball.vz, ball.z, ball.owner = 0, 0, 0, 0, nil
  match.msg, match.msg_t = "GOAL", 5
end

local function check_out()
  local x, y = ball.x, ball.y
  if y < 0 or y > PW then
    set_restart("throw", 3 - match.last_team, max(1, min(PL - 1, x)), y < 0 and 0 or PW)
  elseif x < 0 or x > PL then
    local end_dir = x < 0 and -1 or 1
    local attacking = (match.dir[1] == end_dir) and 1 or 2
    if abs(y - PW / 2) < HALF_GOAL and ball.z < 2.44 then
      goal(attacking)
    elseif match.last_team == attacking then
      set_restart("goalkick", 3 - attacking, x < 0 and 5.5 or PL - 5.5, PW / 2 + (y < PW / 2 and -4 or 4))
    else
      set_restart("corner", attacking, x < 0 and 0.4 or PL - 0.4, y < PW / 2 and 0.4 or PW - 0.4)
    end
  end
end

-- ---------------------------------------------------------------- open play
local function try_control(dt)
  if ball.owner then
    local o = ball.owner
    for _, q in ipairs(players) do
      if q.team ~= o.team and q.stun <= 0 and dist2(q.x, q.y, o.x, o.y) < 1.7 and rand() < dt * 2.4 then
        if rand() < 0.45 then
          local ax, ay = o.x - q.x, o.y - q.y
          local l = max(0.1, sqrt(ax * ax + ay * ay))
          local v = 4 + rand() * 5
          kick(o, ax / l * v + (rand() - 0.5) * 3, ay / l * v + (rand() - 0.5) * 3, 0)
          o.lock, o.stun = 0.6, 0.4
          ball.last, match.last_team = q, q.team
        else
          q.stun = 0.8
        end
        return
      end
    end
    return
  end
  if ball.z > 2.3 then return end
  local best, bd
  for _, p in ipairs(players) do
    if p.lock <= 0 and p.stun <= 0 then
      local reach = p.role == 1 and 1.7 or 1.05
      local d = dist2(p.x, p.y, ball.x, ball.y)
      if d < reach * reach and (not bd or d < bd) then best, bd = p, d end
    end
  end
  if not best then return end
  local speed = sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
  if ball.cross and ball.z > 0.9 and best.role > 1 then
    local crosser = ball.last and ball.last.team
    ball.cross = nil
    if best.team == crosser then
      local gx = goal_of(best.team)
      local aim = PW / 2 + (rand() * 2 - 1) * (HALF_GOAL + 0.6)
      local dx, dy = gx - best.x, aim - best.y
      local d = max(0.5, sqrt(dx * dx + dy * dy))
      local v = 11 + rand() * 5
      kick(best, dx / d * v, dy / d * v, 0.6)
      ball.shot = true
    else
      local away = -match.dir[3 - best.team] * -1
      kick(best, match.dir[best.team] * (10 + rand() * 6), (rand() - 0.5) * 12, 4 + rand() * 3)
    end
    return
  end
  if ball.shot and best.role == 1 then return end        -- keeper_save decides a shot
  if ball.shot and rand() < 0.5 then
    -- a block: the shot comes off him at an angle, and may still go in or out
    local a = (rand() - 0.5) * 1.6
    ball.vx, ball.vy = (ball.vx - ball.vy * a) * 0.6, (ball.vy + ball.vx * a) * 0.6
    ball.vz = ball.vz + 1 + rand() * 2
    ball.last, match.last_team, best.lock = best, best.team, 0.5
    return
  end
  local chance = (speed < 9 or best.role == 1) and 1 or (best == ball.target and 0.85 or 0.45)
  if rand() < chance then
    ball.owner, ball.target, ball.shot, ball.cross = best, nil, nil, nil
    match.possession, match.last_team, ball.last = best.team, best.team, best
    best.decide = best.role == 1 and 1.2 or (0.5 + rand() * 0.6)
  else
    best.lock = 0.3
  end
end

local function play_update(dt)
  local owner = ball.owner
  if owner then match.possession = owner.team end
  local chase = {}
  local ax_, ay_ = ball.x + ball.vx * 0.35, ball.y + ball.vy * 0.35
  for team = 1, 2 do
    if not owner or owner.team ~= team then
      local best, bd
      for _, p in ipairs(players) do
        if p.team == team and p.role > 1 and p.stun <= 0 then
          local d = dist2(p.x, p.y, ax_, ay_)
          if not bd or d < bd then best, bd = p, d end
        end
      end
      chase[team] = best
    end
  end
  for _, p in ipairs(players) do
    p.lock, p.stun = max(0, p.lock - dt), max(0, p.stun - dt)
    local tx, ty, sp
    if p == owner then
      local dir = match.dir[p.team]
      if p.role == 1 then
        tx, ty, sp = p.x, p.y, JOG
      else
        local ay = 0
        local near, nd
        for _, q in ipairs(players) do
          if q.team ~= p.team then
            local d = dist2(q.x, q.y, p.x, p.y)
            if not nd or d < nd then near, nd = q, d end
          end
        end
        if near and nd < 49 then ay = (p.y > near.y) and 0.8 or -0.8 end
        if p.y < 6 then ay = 0.6 elseif p.y > PW - 6 then ay = -0.6 end
        tx, ty, sp = p.x + dir * 6, p.y + ay * 6, RUN * 0.85
      end
      p.decide = p.decide - dt
      if p.decide <= 0 then
        p.decide = 0.35 + rand() * 0.5
        carrier_decide(p)
      end
    elseif p.role == 1 then
      tx, ty = keeper_target(p)
      sp = ball.shot and 3.8 or RUN              -- a shot is on him before he is across
    elseif chase[p.team] == p then
      tx, ty, sp = ax_, ay_, SPRINT
    elseif ball.target == p then
      tx, ty, sp = ball.x + ball.vx * 0.6, ball.y + ball.vy * 0.6, RUN
    else
      tx, ty = shape_target(p, false)
      sp = JOG + ((match.possession == p.team) and 1.2 or 2.0)
      if match.possession == p.team then
        tx = tx + match.dir[p.team] * ((p.role >= 9) and 10 or ((p.role >= 6) and 4 or 0))
      end
    end
    if p.stun > 0 then sp = sp * 0.35 end
    steer(p, tx, ty, sp, dt)
  end
end

-- ---------------------------------------------------------------- the whistle
local function walk_all(dt, kickoff_spots)
  for _, p in ipairs(players) do
    local tx, ty = shape_target(p, true)
    steer(p, tx, ty, JOG, dt)
  end
end

local function goal_update(dt)
  match.timer = match.timer + dt
  local s = match.scorer
  local gx = ball.x < PL / 2 and 0 or PL
  for _, p in ipairs(players) do
    local tx, ty, sp
    if p == s then
      tx, ty, sp = gx + (gx == 0 and 1.5 or -1.5), (s.y < PW / 2) and 1 or PW - 1, SPRINT
    elseif s and p.team == s.team and p.role > 1 then
      tx = s.x + (gx == 0 and 1 or -1) * (1 + (p.role % 4))
      ty, sp = s.y + ((p.role % 5) - 2) * 1.2, RUN
    else
      tx, ty = shape_target(p, true)
      sp = JOG
    end
    steer(p, tx, ty, sp, dt)
  end
  if match.timer > 5 then set_kickoff(match.kickoff) end
end

-- ---------------------------------------------------------------- draw
local order = {}
local function draw_players()
  if #order ~= #players then
    for i = 1, #players do order[i] = players[i] end
  end
  for i = 2, #order do                          -- nearly sorted already: cheap
    local v, j = order[i], i - 1
    while j >= 1 and order[j].y > v.y do order[j + 1] = order[j]; j = j - 1 end
    order[j + 1] = v
  end
  for i = 1, #order do
    local p = order[i]
    local step = (abs(p.vx) + abs(p.vy) > 0.6) and (floor(p.stride * 2) % 2 + 1) or 1
    draw_sprite(SPR[p.kit][p.skin][step], floor(sx(p.x) + 0.5) - 1, floor(sy(p.y) + 0.5) - 3)
  end
end

local function draw_ball()
  local bx, by = floor(sx(ball.x) + 0.5), floor(sy(ball.y) + 0.5)
  if ball.z > 0.25 then px.pixel(bx + 1, by, SHADOW[1], SHADOW[2], SHADOW[3]) end
  px.pixel(bx, by - floor(ball.z * 0.8 + 0.5), 255, 255, 255)
end

function draw()
  local dt = frame_dt()
  if match.phase == "setup" then
    local n = px.now()
    seed = (n.hour * 60 + n.min) * 2654435 + n.yday * 97 + 777
    if seed == 0 then seed = 1 end
    new_players()
    ball = {x = PL / 2, y = PW / 2, z = 0, vx = 0, vy = 0, vz = 0}
    place_for_kickoff(match.first_kickoff)
    set_kickoff(match.first_kickoff)
  end
  local ph = match.phase
  if ph == "play" or ph == "restart" or ph == "goal" then
    match.clock = match.clock + dt * RATIO
  end
  if match.msg_t then
    match.msg_t = match.msg_t - dt
    if match.msg_t <= 0 then match.msg, match.msg_t = nil, nil end
  end
  if ph == "play" then
    play_update(dt)
    ball_physics(dt)
    keeper_save()
    try_control(dt)
    check_out()
    if match.phase == "play" then
      if match.half == 1 and match.clock >= 45 * 60 then
        match.phase, match.timer, match.clock = "halftime", 0, 45 * 60
        match.msg, match.msg_t = nil, nil
      elseif match.half == 2 and match.clock >= 90 * 60 then
        match.phase, match.timer, match.clock = "fulltime", 0, 90 * 60
        match.msg, match.msg_t = nil, nil
      end
    end
  elseif ph == "restart" then
    restart_update(dt)
  elseif ph == "goal" then
    goal_update(dt)
  elseif ph == "halftime" then
    match.timer = match.timer + dt
    walk_all(dt)
    if match.timer > 5 then
      match.half, match.dir = 2, {-1, 1}
      set_kickoff(3 - match.first_kickoff)
    end
  elseif ph == "fulltime" then
    match.timer = match.timer + dt
    walk_all(dt)
    if match.timer > 8 then
      match.score, match.half, match.clock, match.dir = {0, 0}, 1, 0, {1, -1}
      match.first_kickoff = 3 - match.first_kickoff
      set_kickoff(match.first_kickoff)
    end
  end

  draw_static(match.phase == "goal" and floor(match.timer * 6) or nil)
  draw_players()
  draw_ball()
end

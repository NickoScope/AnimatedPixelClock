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

-- ---------------------------------------------------------------- draw (milestone 1: the still)
function draw()
  draw_static(nil)
  for i, p in ipairs(F442) do
    local sp = SPR[i == 1 and "atm_gk" or "atm"][i % 3 + 1][i % 2 + 1]
    draw_sprite(sp, floor(sx(p[1]) + 0.5) - 1, floor(sy(p[2]) + 0.5) - 3)
  end
  for i, p in ipairs(F433) do
    local sp = SPR[i == 1 and "rma_gk" or "rma"][(i + 1) % 3 + 1][i % 2 + 1]
    draw_sprite(sp, floor(sx(PL - p[1]) + 0.5) - 1, floor(sy(PW - p[2]) + 0.5) - 3)
  end
  px.pixel(floor(sx(PL / 2) + 0.5), floor(sy(PW / 2) + 0.5), 255, 255, 255)
end

-- @upload-only
-- GOLF CLOCK - eighteen holes of stroke play in two minutes, a new round every run.
--
-- Two players, ГЕНА and НИКОША, play a full round on a par-72 course: the
-- introduction, eighteen holes of about six seconds each, then the result. The
-- score is always at the top; the real time sits at the bottom right.
--
-- A round is made at the start of each two-minute period from the clock, so
-- every run plays a different game. Realistic, not random: each player gets a
-- handicap (6 to 16), and each hole's score against par comes from an amateur's
-- spread for that handicap - mostly pars and bogeys, some birdies, the odd
-- double, an eagle or a hole-in-one rarely. The shots are then laid out to add
-- up to that score: tee shot, approach, a chip or a bunker, one to three putts,
-- sometimes a ball in the water with its penalty stroke. The player farther
-- from the hole plays next, and the player with the lower score on the last
-- hole has the honour on the tee, as in the Rules of Golf.
--
-- Each hole is a small broadcast (the owner, 2026-09-24): the hole card; a
-- close-up of one shot, the player face-on with a real swing - a drive, a chip
-- or a bunker splash; the plan with every other shot; the last putt full
-- screen and the stand applauding whoever took fewer strokes. About one round
-- in ten has a hole-in-one on a par 3, with fireworks.
--
-- Everything on screen is a function of px.t(): nothing accumulates between
-- frames, so a slow frame never puts the game behind the clock. On the panel:
-- 15 fps, about 10 ms a frame.
--
-- PHOTOS: a private copy for the owner's panel sets PHOTOS before this line
-- (portraits in the introduction and the result); the public one draws the two
-- players as pixel golfers instead. See tools/luasim/scripts/private/.

PERIOD = 120
FPS = 15

local W, H = px.size()
local floor, sqrt, min, max, abs = math.floor, math.sqrt, math.min, math.max, math.abs

local INTRO, HOLE_S, HOLES = 6.0, 5.8, 18
-- Inside a hole: the card over the plan, the close-up of one shot full screen,
-- the plan with every other shot, then the last putt full screen and the
-- applause (the owner, 2026-09-24).
local CARD_END, SCENE_END, MAP_END, ROLL_END = 0.5, 1.8, 3.8, 5.0
local RESULT_AT = INTRO + HOLE_S * HOLES          -- 110.4 s; the result to 120

local NAMES = {"ГЕНА", "НИКОША"}
local COL = { {110, 170, 255}, {110, 235, 110} }    -- Гена blue, Никоша green
local PARS = {4, 4, 3, 5, 4, 4, 3, 4, 5, 4, 3, 4, 5, 4, 4, 3, 5, 4}   -- 36 + 36 = 72

-- ---------------------------------------------------------------- drawing
local function R(x, y, w, h, r, g, b) px.rect(floor(x), floor(y), floor(w), floor(h), r, g, b, true) end
local function C(x, y, rad, r, g, b) px.circle(floor(x), floor(y), floor(rad), r, g, b, true) end
local function P(x, y, r, g, b) px.pixel(floor(x), floor(y), r, g, b) end
local function T(x, y, s, c, font) px.text(floor(x), floor(y), s, c[1], c[2], c[3], font or "small") end
local function TW(s, font) return px.width(s, font or "small") end

-- ---------------------------------------------------------------- random
-- xorshift32 on Lua's integers; math.random is seeded differently in the
-- simulator and on the panel.
local seed = 1
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end
local function pick(weights)
  local total = 0
  for _, w in ipairs(weights) do total = total + w end
  local x, acc = rnd() * total, 0
  for i, w in ipairs(weights) do acc = acc + w; if x < acc then return i end end
  return #weights
end

-- ---------------------------------------------------------------- the course
-- One hole on screen: tee on the left, green on the right, the fairway a
-- curve between them. Screen space for play is y 8..56 (the score above, the
-- time and messages below).
local function make_hole(par)
  local len = (par == 3) and (140 + floor(rnd() * 70))
           or (par == 4) and (340 + floor(rnd() * 120))
           or (490 + floor(rnd() * 90))
  local tx, ty = 10, 32 + floor((rnd() - 0.5) * 16)
  local gx, gy = (par == 3) and 104 or 112, 32 + floor((rnd() - 0.5) * 20)
  local bend = floor((rnd() - 0.5) * 30)
  local h = {
    par = par, len = len, tx = tx, ty = ty, gx = gx, gy = gy,
    cx = (tx + gx) / 2, cy = (ty + gy) / 2 + bend,
    water = rnd() < 0.35, bunkers = {}, trees = {},
  }
  -- bunkers beside the green, and one by the landing zone on long holes
  for i = 1, 1 + floor(rnd() * 3) do
    local a = rnd() * 6.283
    h.bunkers[i] = {gx + math.cos(a) * 10, gy + math.sin(a) * 9, 3 + floor(rnd() * 2)}
  end
  if par >= 4 then
    local f = 0.55 + rnd() * 0.15
    local x, y = (1 - f) ^ 2 * tx + 2 * (1 - f) * f * h.cx + f * f * gx,
                 (1 - f) ^ 2 * ty + 2 * (1 - f) * f * h.cy + f * f * gy
    h.bunkers[#h.bunkers + 1] = {x, y + (rnd() < 0.5 and -9 or 9), 3}
  end
  if h.water then
    local f = (par == 3) and 0.55 or (0.35 + rnd() * 0.3)
    h.wf = f
    h.wx = (1 - f) ^ 2 * tx + 2 * (1 - f) * f * h.cx + f * f * gx
    h.wy = (1 - f) ^ 2 * ty + 2 * (1 - f) * f * h.cy + f * f * gy   -- across the fairway
  end
  for i = 1, 14 do
    local x = 4 + floor(rnd() * 120)
    h.trees[i] = {x, (rnd() < 0.5) and (9 + floor(rnd() * 5)) or (50 + floor(rnd() * 5)), 2 + floor(rnd() * 2)}
  end
  return h
end

-- the point at fraction f of the way from tee to hole, and the lateral offset
local function along(h, f, off)
  f = max(0, min(1, f))
  local x = (1 - f) ^ 2 * h.tx + 2 * (1 - f) * f * h.cx + f * f * h.gx
  local y = (1 - f) ^ 2 * h.ty + 2 * (1 - f) * f * h.cy + f * f * h.gy
  -- the normal to the curve, for missing left and right
  local dx = 2 * (1 - f) * (h.cx - h.tx) + 2 * f * (h.gx - h.cx)
  local dy = 2 * (1 - f) * (h.cy - h.ty) + 2 * f * (h.gy - h.cy)
  local n = sqrt(dx * dx + dy * dy); if n < 1e-6 then n = 1 end
  return x - dy / n * (off or 0), y + dx / n * (off or 0)
end

-- ---------------------------------------------------------------- a score
-- A hole's score against par for an amateur of handicap hcp: weights for
-- eagle, birdie, par, bogey, double, triple. Our model of club golf, not a
-- published table: about hcp/18 + 0.1 over par a hole on average.
local function hole_diff(hcp, par)
  local w = {
    (par == 5) and 0.012 or 0.002,
    max(0.03, 0.17 - 0.008 * hcp),
    max(0.25, 0.56 - 0.017 * hcp),
    0.27 + 0.006 * hcp,
    0.04 + 0.009 * hcp,
    0.008 + 0.003 * hcp,
  }
  return pick(w) - 3        -- -2 eagle .. +3 triple
end

-- The shots that add up to a score: a list of {f, off, kind} positions after
-- each stroke, f the fraction of the hole covered, off the miss sideways.
local function lay_shots(h, strokes)
  local shots, pen = {}, false
  if strokes == 1 then                                   -- a hole-in-one
    shots[1] = {f = 1, off = 0, kind = "ace"}
    return shots, false
  end
  local putts = (strokes == 2) and 1 or pick({0.25, 0.62, 0.13})
  local full = strokes - putts
  if full < 1 then full, putts = 1, strokes - 1 end
  -- a ball in the water costs a stroke: only on a hole with water, and only
  -- when the score has strokes to spare
  if h.water and full >= h.par and rnd() < 0.5 then pen = true end
  local pos = 0
  for i = 1, full do
    local kind, off
    if i == full then
      -- the last full shot finds the green, or a bunker beside it on a bad hole
      pos = 0.93 + rnd() * 0.05
      off = (rnd() - 0.5) * 6
      kind = "green"
    elseif i == full - 1 and full >= 3 then
      -- a green missed in regulation: this one stops short or beside it, and
      -- the next is a chip (or a splash out of a bunker)
      pos = 0.82 + rnd() * 0.07
      off = (rnd() - 0.5) * 10
      kind = "rough"
    else
      local left = full - i + 1
      pos = pos + (0.93 - pos) / left * (0.8 + rnd() * 0.4)
      off = (rnd() - 0.5) * 12
      kind = (abs(off) > 4) and "rough" or "fairway"
      if pen and i == 1 then kind = "water"; pos = h.wf or pos; off = 0 end
    end
    shots[#shots + 1] = {f = pos, off = off, kind = kind}
  end
  if pen then
    -- the drop after the water is the same place, played again: the penalty
    -- stroke is the extra count, not a shot
    table.insert(shots, 2, {f = shots[1].f - 0.04, off = 4, kind = "drop"})
    table.remove(shots, #shots)                            -- keep the total
    shots[#shots].kind = "green"; shots[#shots].f = 0.95
  end
  for i = 1, putts do
    local left = 1 - pos
    if i == putts then pos = 1 else pos = pos + left * (0.6 + rnd() * 0.3) end
    shots[#shots + 1] = {f = pos, off = (i == putts) and 0 or (rnd() - 0.5) * 2, kind = "putt"}
  end
  return shots, pen
end

local WORD = {[-3] = "АЛЬБАТРОС!", [-2] = "ИГЛ!", [-1] = "БЕРДИ!", [0] = "ПАР",
              [1] = "БОГИ", [2] = "ДАБЛ-БОГИ", [3] = "ТРИПЛ"}

-- ---------------------------------------------------------------- a round
local round, round_id = nil, -1

local function make_round(id)
  seed = id * 69069 + 12345              -- wraps in Lua's 32-bit integers, as it should
  if seed == 0 then seed = 1 end
  for _ = 1, 8 do rnd() end
  local g = {hcp = {6 + floor(rnd() * 11), 6 + floor(rnd() * 11)}, holes = {},
             total = {0, 0}, topar = {0, 0}, birdies = {0, 0}}
  local honour = 1 + floor(rnd() * 2)
  -- A hole-in-one in about one round of ten (the owner's number): on one of
  -- the par 3s, by one of the two. No other way to an ace.
  local ace
  if rnd() < 0.1 or FORCE_ACE then       -- FORCE_ACE: a test hook, never set on the panel
    local threes = {}
    for n = 1, HOLES do if PARS[n] == 3 then threes[#threes + 1] = n end end
    ace = {hole = threes[1 + floor(rnd() * #threes)], pl = 1 + floor(rnd() * 2)}
  end
  g.ace = ace
  for n = 1, HOLES do
    local h = make_hole(PARS[n])
    h.n = n
    h.p = {}
    for pl = 1, 2 do
      local d = hole_diff(g.hcp[pl], h.par)
      if h.par == 3 and d <= -2 then d = -1 end                         -- -2 on a par 3 is an ace: only as below
      if ace and ace.hole == n and ace.pl == pl then d = -2 end
      local strokes = h.par + d
      local shots, pen = lay_shots(h, strokes)
      h.p[pl] = {strokes = strokes, diff = d, shots = shots, pen = pen}
    end
    -- the order of play: the honour tees off first; then whoever is farther
    -- from the hole plays, until both are in
    local order, at, done = {}, {0, 0}, {false, false}
    local cur = {0, 0}           -- how far each ball has come (fraction)
    local first = honour
    while not (done[1] and done[2]) do
      local pl
      if at[1] == 0 and at[2] == 0 then pl = first
      elseif at[1] == 0 then pl = 1
      elseif at[2] == 0 then pl = 2
      elseif done[1] then pl = 2
      elseif done[2] then pl = 1
      else pl = (cur[1] <= cur[2]) and 1 or 2 end
      at[pl] = at[pl] + 1
      local s = h.p[pl].shots[at[pl]]
      order[#order + 1] = {pl = pl, i = at[pl], from = cur[pl], s = s}
      cur[pl] = s.f
      if at[pl] >= #h.p[pl].shots then done[pl] = true end
    end
    h.order = order
    -- The close-up of the hole (the owner: real insets, not only the plan):
    -- the ace when there is one; otherwise a drive on odd holes and, on even
    -- ones, a chip - out of a bunker four times in ten - when anyone plays one.
    h.bunker = rnd() < 0.4
    h.dist = 0
    if ace and ace.hole == n then
      h.scene = {kind = "ace", pl = ace.pl}
    else
      local chip
      if n % 2 == 0 then
        for _, ev in ipairs(order) do
          if not chip and ev.from >= 0.7 and ev.s.kind ~= "putt" and ev.from < 0.97 then chip = ev end
        end
      end
      if chip then
        h.scene = {kind = h.bunker and "bunker" or "chip", pl = chip.pl}
      else
        h.scene = {kind = "drive", pl = order[1].pl}
        h.dist = floor(h.len * min(1, order[1].s.f))
      end
    end
    h.final = order[#order]
    -- honour on the next tee: the lower score here; a tie keeps it
    if h.p[1].strokes < h.p[2].strokes then honour = 1
    elseif h.p[2].strokes < h.p[1].strokes then honour = 2 end
    g.holes[n] = h
    for pl = 1, 2 do
      g.total[pl] = g.total[pl] + h.p[pl].strokes
      g.topar[pl] = g.topar[pl] + h.p[pl].diff
      if h.p[pl].diff <= -1 then g.birdies[pl] = g.birdies[pl] + 1 end
    end
  end
  return g
end

local function topar_s(d)
  if d == 0 then return "E" elseif d > 0 then return "+" .. d else return "-" .. (-d) end
end

-- ---------------------------------------------------------------- players
-- A pixel golfer in the player's colours: cap, face, shirt; or the portrait
-- when a private copy supplied PHOTOS.
local function avatar(x, y, pl)
  if PHOTOS and PHOTOS[pl] then
    local p = PHOTOS[pl]
    if not p.rgb then                      -- decoded once, not every frame
      p.rgb = {}
      for i = 1, #p.data, 2 do p.rgb[#p.rgb + 1] = tonumber(p.data:sub(i, i + 1), 16) end
    end
    local rgb, w, k = p.rgb, p.w, 1
    for yy = 0, p.h - 1 do
      for xx = 0, w - 1 do
        px.pixel(x + xx, y + yy, rgb[k], rgb[k + 1], rgb[k + 2])
        k = k + 3
      end
    end
    return p.w, p.h
  end
  local c = COL[pl]
  local skin = {235, 180, 140}
  local cap = (pl == 1) and {40, 60, 150} or {40, 140, 90}
  R(x + 6, y + 16, 12, 12, c[1] // 2, c[2] // 2, c[3] // 2)        -- shirt
  R(x + 7, y + 17, 10, 11, c[1], c[2], c[3])
  C(x + 12, y + 10, 5, skin[1], skin[2], skin[3])                    -- face
  R(x + 7, y + 3, 11, 4, cap[1], cap[2], cap[3])                    -- cap
  R(x + (pl == 1 and 16 or 5), y + 6, 4, 1, cap[1], cap[2], cap[3])  -- peak
  P(x + 10, y + 10, 40, 30, 30); P(x + 14, y + 10, 40, 30, 30)       -- eyes
  R(x + 11, y + 13, 3, 1, 150, 70, 60)                               -- smile
  if pl == 2 then R(x + 8, y + 13, 9, 3, 90, 60, 40) end             -- Никоша's beard
  R(x + 19, y + 12, 1, 16, 200, 200, 200)                            -- the club
  R(x + 18, y + 27, 3, 1, 220, 220, 220)
  return 24, 30
end

-- ---------------------------------------------------------------- the clock
local function draw_clock()
  local n = px.now()
  local s = string.format("%02d:%02d", n.hour, n.min)
  local w = TW(s)
  R(W - w - 3, H - 8, w + 3, 8, 0, 0, 0)
  T(W - w - 1, H - 7, s, {230, 230, 230})
end

-- ---------------------------------------------------------------- the hole
local function draw_course(h)
  px.clear(18, 60, 22)                                   -- rough
  for _, t in ipairs(h.trees) do
    C(t[1], t[2], t[3], 10, 38, 14)
    P(t[1] - 1, t[2] - 1, 30, 80, 30)
  end
  for i = 0, 16 do                                       -- the fairway
    local x, y = along(h, i / 16, 0)
    C(x, y, (h.par == 3) and 4 or 6, 60, 150, 55)
  end
  if h.water then C(h.wx, h.wy, 6, 30, 90, 200); C(h.wx + 4, h.wy + 1, 4, 40, 110, 220) end
  for _, b in ipairs(h.bunkers) do C(b[1], b[2], b[3], 215, 195, 130) end
  C(h.gx, h.gy, 8, 90, 205, 80)                          -- the green
  C(h.gx, h.gy, 6, 110, 220, 95)
  R(h.tx - 3, h.ty - 3, 6, 6, 70, 170, 70)                -- the tee box
  -- the flag
  px.line(floor(h.gx), floor(h.gy), floor(h.gx), floor(h.gy) - 8, 230, 230, 230)
  R(h.gx + 1, h.gy - 8, 4, 3, 240, 40, 40)
  P(h.gx, h.gy, 0, 0, 0)
end

-- The plan plays every shot but the hole's last one between SCENE_END and
-- MAP_END; the last one is the full-screen putt.
local function ball_state(h, ht)
  local pos = {{f = 0, off = 0}, {f = 0, off = 0}}
  local flying, holed, lastmsg = nil, {false, false}, nil
  local n = #h.order - 1
  local sl = (MAP_END - SCENE_END - 0.05) / max(1, n)
  for k = 1, n do
    local ev = h.order[k]
    local t0 = SCENE_END + (k - 1) * sl
    if ht < t0 then break end
    local u = min(1, (ht - t0) / (sl * 0.75))
    local s = ev.s
    if u < 1 then flying = {pl = ev.pl, from = pos[ev.pl], to = s, u = u} end
    pos[ev.pl] = {f = s.f, off = s.off, kind = s.kind}
    if u >= 1 and s.kind == "water" then lastmsg = {pl = ev.pl, text = "В ВОДЕ", until_t = t0 + sl * 2.5} end
    if u >= 1 and ev.i == #h.p[ev.pl].shots then
      holed[ev.pl] = true
      local d = h.p[ev.pl].diff
      lastmsg = {pl = ev.pl, text = (h.p[ev.pl].strokes == 1) and "ЭЙС!" or WORD[d] or ("+" .. d),
                 until_t = t0 + sl * 3}
    end
  end
  return pos, flying, holed, lastmsg
end

local function draw_ball(h, f, off, pl, height)
  local x, y = along(h, f, off)
  if height and height > 0 then
    P(x, y, 10, 30, 10)                                   -- the shadow
    y = y - height
  end
  local c = COL[pl]
  P(x - 1, y, c[1], c[2], c[3]); P(x + 1, y, c[1], c[2], c[3])
  P(x, y - 1, c[1], c[2], c[3]); P(x, y + 1, c[1], c[2], c[3])
  P(x, y, 255, 255, 255)
end

-- the score line: name and to-par for each, the hole in the middle
local function draw_score(g, upto, n, h)
  R(0, 0, W, 8, 0, 0, 0)
  local s1 = NAMES[1] .. " " .. topar_s(upto[1])
  local s2 = topar_s(upto[2]) .. " " .. NAMES[2]
  T(1, 1, s1, COL[1])
  T(W - TW(s2) - 1, 1, s2, COL[2])
  local mid = n and (n .. "/18 П" .. h.par) or ""
  T((W - TW(mid)) // 2, 1, mid, {255, 200, 60})
end

-- ---------------------------------------------------------------- close-ups
-- A golfer seen face-on, the target to the right: feet at (x, fy), the arms
-- at angle a from the shoulder (0 hanging down, negative the backswing,
-- positive the follow-through), the club along them.
local SHIRT = { {40, 70, 170}, {70, 165, 70} }
local CAP = { {30, 40, 110}, {35, 120, 75} }
local SKIN = {235, 180, 140}

local function golfer(x, fy, pl, a, club, putter)
  local sh, cp = SHIRT[pl], CAP[pl]
  px.line(floor(x - 2), floor(fy - 8), floor(x - 3), floor(fy - 1), 45, 45, 60)     -- legs
  px.line(floor(x + 2), floor(fy - 8), floor(x + 3), floor(fy - 1), 45, 45, 60)
  R(x - 4, fy - 1, 3, 1, 240, 240, 240); R(x + 2, fy - 1, 3, 1, 240, 240, 240)      -- shoes
  R(x - 3, fy - 15, 7, 7, sh[1], sh[2], sh[3])                                         -- shirt
  R(x - 3, fy - 9, 7, 1, 30, 30, 40)                                                   -- belt
  C(x, fy - 18, 3, SKIN[1], SKIN[2], SKIN[3])                                          -- head
  R(x - 3, fy - 22, 7, 2, cp[1], cp[2], cp[3])                                         -- cap
  R(x + 2, fy - 20, 3, 1, cp[1], cp[2], cp[3])
  if pl == 2 then R(x - 2, fy - 16, 5, 2, 90, 60, 40) end                              -- the beard
  local sx, sy = x, fy - 14
  local arm = 7
  local hx, hy = sx + math.sin(a) * arm, sy + math.cos(a) * arm
  px.line(floor(sx), floor(sy), floor(hx), floor(hy), SKIN[1], SKIN[2], SKIN[3])
  px.line(floor(sx + 1), floor(sy), floor(hx + 1), floor(hy), SKIN[1] - 30, SKIN[2] - 30, SKIN[3] - 30)
  local ca = a * (putter and 1 or 1.2)                                                 -- the wrists hinge
  local cx, cy = hx + math.sin(ca) * club, hy + math.cos(ca) * club
  px.line(floor(hx), floor(hy), floor(cx), floor(cy), 190, 190, 200)
  R(cx - 1, cy, 3, 1, putter and 170 or 120, putter and 170 or 120, putter and 180 or 130)
end

-- the arms' angle through a swing: u 0..1; impact at IMPACT
local IMPACT = 0.55
local function swing(u, back, through)
  local e = function(x) return x * x * (3 - 2 * x) end
  if u < 0.12 then return 0.05 end
  if u < 0.45 then return 0.05 + (back - 0.05) * e((u - 0.12) / 0.33) end
  if u < IMPACT then return back + (0 - back) * e((u - 0.45) / (IMPACT - 0.45)) end
  if u < 0.8 then return through * e((u - IMPACT) / (0.8 - IMPACT)) end
  return through
end

local function label(s, pl)
  local w = TW(s, "5x7")
  R(0, 0, w + 3, 9, 0, 0, 0)
  T(1, 1, s, COL[pl], "5x7")
end

local function sky(y1, top, bot)
  for i = 0, 5 do
    local k = i / 5
    R(0, floor(y1 * i / 6), W, floor(y1 / 6) + 1,
      floor(top[1] + (bot[1] - top[1]) * k), floor(top[2] + (bot[2] - top[2]) * k), floor(top[3] + (bot[3] - top[3]) * k))
  end
end

local function treeline(y)
  for i = 0, 16 do
    local x = i * 8 + (i * 37) % 5
    C(x, y + (i * 13) % 3, 4 + (i * 7) % 3, 20, 70 + (i * 11) % 20, 30)
  end
end

-- The drive (or the tee shot of the ace): the swing, then the ball away into
-- the sky with its trail, and the distance.
local function scene_drive(h, sc, u)
  sky(36, {70, 130, 230}, {170, 210, 250})
  treeline(34)
  for i = 0, 6 do                                        -- mown stripes
    local c = (i % 2 == 0) and {70, 165, 60} or {80, 180, 70}
    R(0, 38 + i * 4, W, 4, c[1], c[2], c[3])
  end
  R(28, 55, 16, 3, 95, 190, 85)                          -- the tee box
  local pl = sc.pl
  local a = swing(u, -2.5, 2.4)
  golfer(34, 56, pl, a, 9, false)
  local v = (u - IMPACT) / (1 - IMPACT)
  if v < 0 then
    P(38, 55, 255, 255, 255)                             -- teed up
  else
    for k = 4, 0, -1 do                                  -- the ball and its trail
      local vv = max(0, v - k * 0.05)
      local bx = 38 + vv * 110
      local by = 55 - math.sin(min(1, vv) * 2.2) * 44 - vv * 6
      local c = 255 - k * 45
      P(bx, by, c, c, c)
    end
  end
  local what = (sc.kind == "ace") and "ПАР 3" or "ДРАЙВ"
  label(NAMES[pl] .. ": " .. what, pl)
  if v > 0.35 and sc.kind ~= "ace" then
    local d = h.dist .. " ЯРД"
    T(W - TW(d, "5x7") - 2, 1, d, {255, 230, 120}, "5x7")
  end
end

-- The chip, or the splash out of a bunker: a short swing, the ball up and down
-- onto the green and rolling towards the flag.
local function scene_chip(h, sc, u)
  sky(22, {90, 150, 235}, {180, 215, 250})
  treeline(20)
  R(0, 24, W, 40, 35, 105, 40)                           -- rough
  R(62, 36, 66, 28, 90, 200, 80)                         -- the green
  C(62, 50, 14, 90, 200, 80)
  local fx, fy2 = 108, 46
  px.line(fx, fy2, fx, fy2 - 18, 235, 235, 235)
  R(fx + 1, fy2 - 18, 6, 4, 240, 40, 40)
  R(fx - 2, fy2, 5, 2, 10, 10, 10)                       -- the cup
  local bunker = sc.kind == "bunker"
  if bunker then C(34, 57, 12, 220, 200, 140); R(18, 57, 32, 7, 220, 200, 140) end
  local pl = sc.pl
  golfer(30, 58, pl, swing(u, -1.3, 1.1), 8, false)
  local v = (u - IMPACT) / (1 - IMPACT)
  local bx, by
  if v < 0 then bx, by = 34, 57
  elseif v < 0.6 then
    local w = v / 0.6
    bx, by = 34 + w * 58, 57 - math.sin(w * 3.1416) * 26 - w * 11
  else
    local w = min(1, (v - 0.6) / 0.4)
    bx, by = 92 + (1 - (1 - w) * (1 - w)) * 12, 46
  end
  if bunker and v >= 0 and v < 0.35 then                 -- the sand
    for k = 0, 14 do
      local ang = -0.3 - (k % 7) * 0.35
      local r = v * 60 * (0.5 + (k % 3) * 0.25)
      P(34 + math.cos(ang) * r, 56 + math.sin(ang) * r * 0.8, 230, 210, 150)
    end
  end
  P(bx, by, 255, 255, 255); P(bx + 1, by, 200, 200, 200)
  label(NAMES[pl] .. (bunker and ": ИЗ БУНКЕРА" or ": ЧИП"), pl)
end

-- The last putt of the hole, full screen, then the applause for whoever took
-- fewer strokes here (both, politely, on a halved hole).
local function crowd(clap, y)
  for i = 0, 25 do
    local x = 3 + i * 5
    local yy = y + (i % 2) * 3
    local c = {(i * 71) % 200 + 40, (i * 37) % 180 + 50, (i * 53) % 200 + 40}
    R(x - 2, yy + 3, 4, 4, c[1], c[2], c[3])            -- shoulders
    C(x, yy + 1, 2, 225, 180, 140)                       -- head
    if clap > 0 and ((floor(clap * 8) + i) % 2 == 0) then
      P(x - 2, yy - 2, 255, 220, 180); P(x + 2, yy - 2, 255, 220, 180)
      P(x, yy - 3, 255, 255, 255)
    end
  end
end

local function scene_putt(h, u)
  local ev = h.final
  local pl = ev.pl
  R(0, 0, W, 16, 25, 25, 35)                             -- the stand
  local win = (h.p[1].strokes < h.p[2].strokes) and 1 or (h.p[2].strokes < h.p[1].strokes) and 2 or 0
  local roll = min(1, u / 0.6)
  local after = max(0, (u - 0.6) / 0.4)
  crowd(after > 0 and (win > 0 and after or after * 0.4) or 0, 3)
  R(0, 16, W, 1, 240, 240, 240)                          -- the rope
  for i = 0, 5 do                                        -- the green, receding
    local g = 150 + i * 12
    R(0, 17 + i * 8, W, 8, 60 + i * 6, g, 55 + i * 5)
  end
  local cx, cy = 100, 38
  px.line(cx, cy, cx, cy - 18, 235, 235, 235)
  R(cx + 1, cy - 18, 7, 4, 240, 40, 40)
  R(cx - 3, cy, 7, 2, 10, 10, 10)                        -- the cup
  golfer(24, 54, pl, swing(min(1, u / 0.35), -0.35, 0.35), 7, true)
  if roll < 1 then
    local w = 1 - (1 - roll) * (1 - roll)                -- slowing down
    local bx = 28 + (cx - 28) * w
    local by = 53 + (cy - 53) * w + math.sin(w * 3.1416) * 3
    if u > 0.12 then P(bx, by, 255, 255, 255); P(bx, by + 1, 150, 150, 150) end
  end
  if after > 0 then
    local s, c
    if win > 0 then
      s, c = "БРАВО, " .. NAMES[win] .. "!", COL[win]
      for k = 0, 20 do                                   -- confetti
        local x = (k * 29 + floor(after * 40) * 3) % W
        local y = 18 + (k * 17 + floor(after * 60)) % 44
        local cc = ({{255, 80, 80}, {255, 220, 60}, {80, 200, 255}, {255, 255, 255}})[k % 4 + 1]
        P(x, y, cc[1], cc[2], cc[3])
      end
    else
      s, c = "ЛУНКА ПОПОЛАМ", {255, 215, 90}
    end
    local w = TW(s, "5x7")
    R((W - w) // 2 - 2, 44, w + 4, 18, 0, 0, 0)
    T((W - w) // 2, 45, s, c, "5x7")
    local d = win > 0 and h.p[win].diff or h.p[1].diff
    local wd = (win > 0 and h.p[win].strokes or h.p[1].strokes) .. " - " .. (WORD[d] or ("+" .. d))
    T((W - TW(wd, "5x7")) // 2, 54, wd, {230, 230, 230}, "5x7")
  else
    label(NAMES[pl] .. ": ПАТТ", pl)
  end
end

-- The ace: fireworks over the green and the whole stand on its feet.
local function scene_ace(h, u)
  local pl = h.scene.pl
  R(0, 0, W, H, 8, 10, 30)
  crowd(u, 40)
  for b = 0, 4 do                                        -- the fireworks
    local t0 = b * 0.18
    local v = (u - t0) / 0.5
    if v > 0 and v < 1 then
      local bx, by = 16 + b * 24, 10 + (b * 7) % 14
      local c = ({{255, 90, 60}, {255, 220, 60}, {90, 200, 255}, {140, 255, 120}, {255, 120, 255}})[b + 1]
      for k = 0, 11 do
        local ang = k * 0.5236
        P(bx + math.cos(ang) * v * 12, by + math.sin(ang) * v * 9,
          floor(c[1] * (1 - v * 0.6)), floor(c[2] * (1 - v * 0.6)), floor(c[3] * (1 - v * 0.6)))
      end
    end
  end
  local s = "HOLE IN ONE!"
  if (u * 4) % 1 < 0.75 then T((W - TW(s, "5x7")) // 2, 20, s, {255, 215, 60}, "5x7") end
  local n = NAMES[pl] .. " - ЛУНКА " .. h.n
  T((W - TW(n, "5x7")) // 2, 30, n, COL[pl], "5x7")
end

-- ---------------------------------------------------------------- screens
local function draw_intro(g, t)
  px.clear(8, 30, 12)
  local title = "ГОЛЬФ - 18 ЛУНОК"
  T((W - TW(title)) // 2, 1, title, {255, 215, 90})
  local aw, ah = avatar(8, 12, 1)
  local bw, bh = avatar(W - 8 - 24, 12, 2)
  T(36, 16, NAMES[1], COL[1])
  T(36, 24, "HCP " .. g.hcp[1], {200, 200, 200})
  local n2 = NAMES[2]
  T(W - 36 - TW(n2), 34, n2, COL[2])
  local h2 = "HCP " .. g.hcp[2]
  T(W - 36 - TW(h2), 42, h2, {200, 200, 200})
  if (t * 2) % 1 < 0.7 then T((W - TW("VS")) // 2, 27, "VS", {255, 90, 60}) end
  local left = floor(INTRO - t + 0.99)
  local s = "СТАРТ " .. left
  T((W - TW(s)) // 2, 50, s, {230, 230, 230})
end

local function draw_result(g, t)
  px.clear(10, 16, 40)
  local title = "ИТОГ - ПАР 72"
  T((W - TW(title)) // 2, 1, title, {255, 215, 90})
  avatar(4, 10, 1)
  avatar(W - 28, 10, 2)
  for pl = 1, 2 do
    local x = (pl == 1) and 30 or (W - 30)
    local lines = {NAMES[pl], tostring(g.total[pl]), topar_s(g.topar[pl])}
    for i, s in ipairs(lines) do
      local w = TW(s)
      local c = (i == 1) and COL[pl] or {220, 220, 220}
      T((pl == 1) and x or (x - w), 10 + (i - 1) * 8, s, c)
    end
  end
  -- birdies or better, one line across: 5x7, whose Д reads as Д
  local b = "БЕРДИ " .. g.birdies[1] .. " : " .. g.birdies[2]
  T((W - TW(b, "5x7")) // 2, 35, b, {220, 220, 220}, "5x7")
  local win
  if g.total[1] < g.total[2] then win = "ПОБЕДИЛ " .. NAMES[1] .. "!"
  elseif g.total[2] < g.total[1] then win = "ПОБЕДИЛ " .. NAMES[2] .. "!"
  else win = "НИЧЬЯ!" end
  local c = (g.total[1] < g.total[2]) and COL[1] or (g.total[2] < g.total[1]) and COL[2] or {255, 215, 90}
  if (t * 2) % 1 < 0.8 then T((W - TW(win, "5x7")) // 2, 48, win, c, "5x7") end
end

-- ---------------------------------------------------------------- draw
function draw()
  local n = px.now()
  -- the round of this two-minute period: periods start on even minutes
  local id = ((n.year % 100) * 400 + n.yday) * 720 + (n.hour * 60 + n.min) // 2
  if id ~= round_id then round, round_id = make_round(id), id end
  local g = round
  local t = px.t() * PERIOD

  if t < INTRO then
    draw_intro(g, t)
  elseif t >= RESULT_AT then
    draw_result(g, t - RESULT_AT)
  else
    local hn = floor((t - INTRO) / HOLE_S) + 1
    local ht = (t - INTRO) - (hn - 1) * HOLE_S
    local h = g.holes[hn]
    if ht >= CARD_END and ht < SCENE_END then
      local u = (ht - CARD_END) / (SCENE_END - CARD_END)
      local k = h.scene.kind
      if k == "chip" or k == "bunker" then scene_chip(h, h.scene, u) else scene_drive(h, h.scene, u) end
      return
    end
    if ht >= MAP_END then
      local u = (ht - MAP_END) / (HOLE_S - MAP_END)
      if h.scene.kind == "ace" then scene_ace(h, u) else scene_putt(h, u) end
      return
    end
    draw_course(h)
    local pos, flying, holed, msg = ball_state(h, ht)
    for pl = 1, 2 do
      if not holed[pl] and not (flying and flying.pl == pl) then
        draw_ball(h, pos[pl].f, pos[pl].off, pl, 0)
      end
    end
    if flying then
      local u, a, b = flying.u, flying.from, flying.to
      local f = (a.f or 0) + ((b.f or 0) - (a.f or 0)) * u
      local off = (a.off or 0) + ((b.off or 0) - (a.off or 0)) * u
      local arc = (b.kind == "putt") and 0 or (4 * u * (1 - u)) * min(12, 30 * ((b.f or 0) - (a.f or 0)) + 3)
      draw_ball(h, f, off, flying.pl, arc)
    end
    -- the score so far: holes finished, and this one once a ball is in
    local upto = {0, 0}
    for k = 1, hn - 1 do
      upto[1] = upto[1] + g.holes[k].p[1].diff
      upto[2] = upto[2] + g.holes[k].p[2].diff
    end
    for pl = 1, 2 do if holed[pl] then upto[pl] = upto[pl] + h.p[pl].diff end end
    draw_score(g, upto, hn, h)
    if ht < CARD_END then
      local s = "ЛУНКА " .. hn .. " - " .. h.len .. " ЯРДОВ"
      R(0, H - 8, TW(s) + 3, 8, 0, 0, 0)
      T(1, H - 7, s, {255, 215, 90})
    elseif msg and ht < msg.until_t then
      -- 5x7, not the small font: its Д reads as А at this size
      local s = NAMES[msg.pl] .. ": " .. msg.text
      R(0, H - 9, TW(s, "5x7") + 2, 9, 0, 0, 0)
      T(1, H - 8, s, COL[msg.pl], "5x7")
    end
    draw_clock()
  end
end

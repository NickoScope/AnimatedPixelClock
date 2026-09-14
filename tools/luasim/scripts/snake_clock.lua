-- snake_clock.lua - HH:MM where each digit is a snake.
--
-- On the minute the four snakes crawl away downward and four new ones crawl in
-- from the top and lay themselves out as the next time.
--
-- The model is a rope, not falling blocks. A digit's cells are ordered into a
-- walk; the snake's body is that whole walk, and the animation slides the body
-- along a route. Dissolve routes the walk into a column heading off the bottom;
-- assembly routes a column from above into the walk. At rest the body sits
-- exactly on the digit, which is why the still frame is a clean clock and not
-- an approximation of one.
--
-- On black, with the world clock's bold face one row taller: 6 x 12 cells of
-- 4 px, so the digits are 24 x 48 px and HH:MM spans the panel. Nothing lights
-- the background - no grid, no glow round the heads.

local W, H = px.size()
local B = 4
local COLS, ROWS = W // B, H // B          -- 32 x 16
local floor, min, max, abs, cos, pi = math.floor, math.min, math.max, math.abs, math.cos, math.pi

local GLYPH = {
  ["0"]={".####.","######","##..##","##..##","##..##","##..##","##..##","##..##","##..##","##..##","######",".####."},
  ["1"]={"..##..",".###..","####..","..##..","..##..","..##..","..##..","..##..","..##..","..##..","######","######"},
  ["2"]={".####.","######","##..##","....##","....##","...###","..###.",".###..","###...","##....","######","######"},
  ["3"]={".####.","######","##..##","....##","....##","..###.","..####","....##","....##","##..##","######",".####."},
  ["4"]={"...###","..####",".##.##","##..##","##..##","##..##","######","######","....##","....##","....##","....##"},
  ["5"]={"######","######","##....","##....","#####.","######","....##","....##","....##","##..##","######",".####."},
  ["6"]={".####.","######","##....","##....","#####.","######","##..##","##..##","##..##","##..##","######",".####."},
  ["7"]={"######","######","....##","....##","...##.","...##.","..##..","..##..","..##..","..##..","..##..","..##.."},
  ["8"]={".####.","######","##..##","##..##","##..##",".####.",".####.","##..##","##..##","##..##","######",".####."},
  ["9"]={".####.","######","##..##","##..##","##..##","######",".#####","....##","....##","....##","######",".####."},
}

local GW, DIGH = 6, 12
local TOP = 2                                -- rows 2..13
local XS = {1, 8, 18, 25}                    -- left column of each digit

-- The colon: two 2 x 2 dots, as on the world clock.
local COLON_CELLS = {}
for _, r in ipairs({3, 4, 7, 8}) do
  COLON_CELLS[#COLON_CELLS + 1] = {c = 15, r = TOP + r}
  COLON_CELLS[#COLON_CELLS + 1] = {c = 16, r = TOP + r}
end

-- One hue per digit position, so a snake keeps its identity across the change.
local SNAKE = { {0, 230, 120}, {255, 170, 0}, {60, 130, 255}, {255, 50, 170} }
local WHITE = {245, 245, 255}

-- ---------------------------------------------------------------- the walk
-- Nearest-neighbour from the top-left cell. Through a two-cell stroke it
-- zigzags, which at rest is invisible - the body covers the digit exactly -
-- and in motion reads as a coiled snake uncoiling.
local function walk_of(ch, x0)
  local g = GLYPH[ch]
  local cells = {}
  for r = 1, DIGH do
    local row = g[r]
    for c = 1, GW do
      if row:sub(c, c) == "#" then cells[#cells + 1] = {c = x0 + c - 1, r = TOP + r - 1} end
    end
  end
  local path, used = {cells[1]}, {true}      -- topmost-leftmost, so it enters from above
  for _ = 2, #cells do
    local best, bd = nil, 1e9
    local last = path[#path]
    for i, c in ipairs(cells) do
      if not used[i] then
        local d = abs(c.c - last.c) + abs(c.r - last.r)
        if d < bd then bd, best = d, i end
      end
    end
    used[best] = true; path[#path + 1] = cells[best]
  end
  return path
end

-- Route: a column joined to the walk, or the walk joined to a column, each
-- exactly long enough that the whole body is off the panel at the far end and
-- no longer: a longer runway is seconds of empty screen.
-- Body = route[s + 1 .. s + N], so sliding s animates the whole rope.
local function route_in(path)
  local head = path[1]
  local run = head.r + #path                 -- body just above row 0 at s = 0
  local r = {}
  for k = run, 1, -1 do r[#r + 1] = {c = head.c, r = head.r - k} end
  for _, c in ipairs(path) do r[#r + 1] = c end
  return r, run
end
local function route_out(path)
  local tail = path[#path]
  local run = ROWS - tail.r + #path - 1      -- body just below the last row at s = run
  local r = {}
  for _, c in ipairs(path) do r[#r + 1] = c end
  for k = 1, run do r[#r + 1] = {c = tail.c, r = tail.r + k} end
  return r, run
end

-- ---------------------------------------------------------------- state
-- A digit's walk and both its routes depend only on the digit and where it
-- stands, so each of the forty is worked out the first time it is needed and
-- kept. Rebuilt every minute, the bold digits cost 160 000 instructions in the
-- one frame the minute turned; rebuilt every frame, the routes cost more than
-- the drawing.
local cache = {}
local function snake_for(ch, x0)
  local key = ch .. x0
  local sn = cache[key]
  if not sn then
    local path = walk_of(ch, x0)
    local rin, run_in = route_in(path)
    local rout, run_out = route_out(path)
    sn = {n = #path, rin = rin, run_in = run_in, rout = rout, run_out = run_out}
    cache[key] = sn
  end
  return sn
end

local shown
local cur_s, new_s = {}, {}
local function set_time(cur, nxt)
  if shown == cur then return end
  shown = cur
  local D = {1, 2, 4, 5}
  for i = 1, 4 do
    cur_s[i] = snake_for(cur:sub(D[i], D[i]), XS[i])
    new_s[i] = snake_for(nxt:sub(D[i], D[i]), XS[i])
  end
end

-- ---------------------------------------------------------------- drawing
-- A segment: its colour scaled by s and pushed toward white by w, over a
-- half-bright edge, all inside its own 4 px.
local function cell(c, r, col, s, w)
  if r < 0 or r >= ROWS or c < 0 or c >= COLS then return end
  local R, G, Bl = col[1] * s, col[2] * s, col[3] * s
  if w > 0 then R, G, Bl = R + (255 - R) * w, G + (255 - G) * w, Bl + (255 - Bl) * w end
  R, G, Bl = min(255, floor(R)), min(255, floor(G)), min(255, floor(Bl))
  local x, y = c * B, r * B
  px.rect(x, y, B, B, R // 2, G // 2, Bl // 2, true)
  px.rect(x, y, B - 1, B - 1, R, G, Bl, true)
end

-- The body brightens from tail to head, which is what makes a line of blocks
-- read as a creature, and the head carries a white eye.
--
-- A pulse also runs head-to-tail the whole time, not only during a change. A
-- clock that is only alive for ten seconds a minute is a still image with an
-- interruption; the pulse is what makes the other fifty seconds worth looking
-- at. It whitens the segment it passes rather than lighting anything around.
local function body(route, s, n, col, pulse, blink)
  for i = 0, n - 1 do
    local p = route[s + i + 1]
    if p then
      local f = i / max(1, n - 1)                 -- 0 tail .. 1 head
      local w = 0
      if pulse then
        -- one crest travelling head-to-tail, narrow enough to read as a beat
        local d = abs(((1 - f) - pulse) % 1.0)
        d = min(d, 1 - d)
        w = 0.45 * max(0, 1 - d * 7)
      end
      cell(p.c, p.r, col, 0.7 + 0.3 * f, w)
    end
  end
  local hp = route[s + n]
  if hp and hp.r >= 0 and hp.r < ROWS and not blink then
    px.rect(hp.c * B + 1, hp.r * B + 1, 1, 1, 255, 255, 255, true)   -- an eye
  end
end

local DIS0, DIS1, ASM1 = 0.50, 0.72, 0.99

function draw()
  local t = px.t()
  local n = px.now()
  local cur = string.format("%02d:%02d", n.hour, n.min)
  local nxt = string.format("%02d:%02d", (n.min == 59) and (n.hour + 1) % 24 or n.hour,
                            (n.min + 1) % 60)
  set_time(cur, nxt)
  px.clear(0, 0, 0)

  -- The colon stays, and stays bright: it is punctuation, not a snake. It
  -- breathes once a second (px.t() is the second hand on the panel).
  local beat = 0.8 + 0.2 * abs(cos(pi * t * 60))
  for _, c in ipairs(COLON_CELLS) do cell(c.c, c.r, WHITE, beat, 0) end

  for i = 1, 4 do
    local col = SNAKE[i]
    if t < DIS0 then
      local sn = cur_s[i]
      -- Each snake pulses at its own rate and blinks on its own schedule, so
      -- four of them never look like one animation drawn four times.
      local pulse = (t * (7 + i * 1.7)) % 1.0
      local bl = ((t * 60 + i * 11) % 17) < 0.6
      body(sn.rout, 0, sn.n, col, pulse, bl)

    elseif t < DIS1 then
      -- crawling away: the rope slides forward into its exit column
      local k = (t - DIS0) / (DIS1 - DIS0)
      local sn = cur_s[i]
      local lead = (i - 1) * 0.06                 -- the four leave in turn, not as one
      local kk = max(0, min(1, (k - lead) / (1 - lead)))
      body(sn.rout, floor(kk * sn.run_out), sn.n, col, (t * 9) % 1.0, false)

    else
      -- crawling in: a column from above unrolls onto the new digit
      local k = (t - DIS1) / (ASM1 - DIS1)
      local sn = new_s[i]
      local lead = (i - 1) * 0.07
      local kk = max(0, min(1, (k - lead) / (1 - lead)))
      body(sn.rin, floor(kk * sn.run_in), sn.n, col, (t * 9) % 1.0, false)
    end
  end
end

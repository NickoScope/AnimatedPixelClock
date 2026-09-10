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

local W, H = px.size()
local B = 4
local COLS, ROWS = W // B, H // B
local floor, min, max, abs, sin, pi = math.floor, math.min, math.max, math.abs, math.sin, math.pi

local GLYPH = {
  ["0"]={"01110","10001","10011","10101","11001","10001","01110"},
  ["1"]={"00100","01100","00100","00100","00100","00100","01110"},
  ["2"]={"01110","10001","00001","00010","00100","01000","11111"},
  ["3"]={"11110","00001","00001","01110","00001","00001","11110"},
  ["4"]={"00010","00110","01010","10010","11111","00010","00010"},
  ["5"]={"11111","10000","11110","00001","00001","10001","01110"},
  ["6"]={"00110","01000","10000","11110","10001","10001","01110"},
  ["7"]={"11111","00001","00010","00100","01000","01000","01000"},
  ["8"]={"01110","10001","10001","01110","10001","10001","01110"},
  ["9"]={"01110","10001","10001","01111","00001","00010","01100"},
}

local TOP, DIGH = 5, 7
local XS = {3, 9, 18, 24}
local COLON = 15
local RUNWAY = ROWS + 6          -- long enough to be fully off-screen at the ends

-- One hue per digit position, so a snake keeps its identity across the change.
local SNAKE = { {0,225,150}, {255,180,40}, {90,150,255}, {230,80,180} }

-- ---------------------------------------------------------------- the walk
-- Nearest-neighbour from the top-left cell. On these glyphs almost every step
-- lands on a neighbour; the rare jump is one cell wide and reads as the snake
-- crossing its own stroke rather than as a break.
local function walk_of(ch, x0)
  local g = GLYPH[ch]
  local cells = {}
  for r = 1, DIGH do
    local row = g[r]
    for c = 1, 5 do
      if row:sub(c, c) == "1" then cells[#cells+1] = {c = x0 + c - 1, r = TOP + r - 1} end
    end
  end
  local path, used = {}, {}
  local cur = 1                                  -- topmost-leftmost, so it enters from above
  path[1] = cells[1]; used[1] = true
  for _ = 2, #cells do
    local best, bd = nil, 1e9
    for i, c in ipairs(cells) do
      if not used[i] then
        local d = abs(c.c - path[#path].c) + abs(c.r - path[#path].r)
        if d < bd then bd, best = d, i end
      end
    end
    used[best] = true; path[#path+1] = cells[best]
  end
  return path
end

-- Route: a column of length RUNWAY joined to the walk, or the walk joined to a
-- column. Body = route[s .. s+N-1], so sliding s animates the whole rope.
local function route_in(path)
  local head = path[1]
  local r = {}
  for k = RUNWAY, 1, -1 do r[#r+1] = {c = head.c, r = head.r - k} end
  for _, c in ipairs(path) do r[#r+1] = c end
  return r, RUNWAY
end
local function route_out(path)
  local tail = path[#path]
  local r = {}
  for _, c in ipairs(path) do r[#r+1] = c end
  for k = 1, RUNWAY do r[#r+1] = {c = tail.c, r = tail.r + k} end
  return r
end

-- ---------------------------------------------------------------- state
local shown
local walks_cur, walks_new = {}, {}
local function set_time(cur, nxt)
  if shown == cur then return end
  shown = cur
  local D = {1, 2, 4, 5}
  for i = 1, 4 do
    walks_cur[i] = walk_of(cur:sub(D[i], D[i]), XS[i])
    walks_new[i] = walk_of(nxt:sub(D[i], D[i]), XS[i])
  end
end

-- ---------------------------------------------------------------- drawing
local function cell(c, r, col, s)
  if r < 0 or r >= ROWS or c < 0 or c >= COLS then return end
  s = s or 1.0
  px.rect(c*B, r*B, B, B, floor(col[1]*s*0.55), floor(col[2]*s*0.55), floor(col[3]*s*0.55), true)
  px.rect(c*B, r*B, B-1, B-1, floor(col[1]*s), floor(col[2]*s), floor(col[3]*s), true)
end

-- The body is drawn from tail to head so the head sits on top, and it brightens
-- along its length: that is what makes a line of blocks read as a creature.
local function body(route, s, n, col, headglow)
  for i = 0, n - 1 do
    local p = route[s + i + 1]
    if p then
      local f = i / max(1, n - 1)                 -- 0 tail .. 1 head
      cell(p.c, p.r, col, 0.55 + 0.45 * f)
    end
  end
  local hp = route[s + n]
  if hp and headglow and hp.r >= -1 and hp.r < ROWS then
    px.glow(hp.c*B + 2, hp.r*B + 2, 7, col[1], col[2], col[3], 0.5)
    px.rect(hp.c*B + 1, hp.r*B + 1, 1, 1, 255, 255, 255, true)   -- an eye
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

  px.clear(5, 7, 12)
  for r = 0, ROWS - 1 do px.rect(0, r*B, W, 1, 10, 14, 22, true) end

  -- The colon stays: it is punctuation, not a snake. It dims while they travel.
  local held = (t < DIS0) and 1.0 or 0.30
  local beat = 0.55 + 0.45 * abs(sin(pi * n.sec))
  for _, r in ipairs({TOP+1, TOP+2, TOP+4, TOP+5}) do
    cell(COLON, r, {225, 232, 245}, beat * held)
  end

  for i = 1, 4 do
    local col = SNAKE[i]
    if t < DIS0 then
      local p = walks_cur[i]
      body(route_out(p), 0, #p, col, false)

    elseif t < DIS1 then
      -- crawling away: the rope slides forward into its exit column
      local k = (t - DIS0) / (DIS1 - DIS0)
      local p = walks_cur[i]
      local lead = (i - 1) * 0.06                 -- the four leave in turn, not as one
      local kk = max(0, min(1, (k - lead) / (1 - lead)))
      local r = route_out(p)
      body(r, floor(kk * (#r - #p)), #p, col, kk > 0)

    else
      -- crawling in: a column from above unrolls onto the new digit
      local k = (t - DIS1) / (ASM1 - DIS1)
      local p = walks_new[i]
      local lead = (i - 1) * 0.07
      local kk = max(0, min(1, (k - lead) / (1 - lead)))
      local r, rest = route_in(p)
      body(r, floor(kk * rest), #p, col, kk < 1)
    end
  end
end

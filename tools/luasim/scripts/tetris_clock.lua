-- tetris_clock.lua - HH:MM that clears itself like completed lines and is
-- rebuilt by falling tetrominoes when the minute turns.
--
-- The whole panel is the well: 32 x 16 cells of 4 px. Digits are 5 x 7 cells,
-- which is the largest that leaves room above for pieces to fall through.

local W, H = px.size()
local B = 4
local COLS, ROWS = W // B, H // B          -- 32 x 16
local floor, ceil, min, max, abs, sin, pi = math.floor, math.ceil, math.min, math.max, math.abs, math.sin, math.pi

-- ---------------------------------------------------------------- digits
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

local TOP   = 5                              -- top row of the digit block
local DIGH  = 7
local XS    = {3, 9, 18, 24}                 -- left column of each digit
local COLON = 15

-- Tetromino colours, in the order pieces are handed out.
local PIECE_COLS = {
  {0,240,240}, {240,240,0}, {160,60,230}, {60,220,80},
  {235,60,60}, {60,120,240}, {245,150,40},
}

-- Cells the time occupies, as a set keyed "c,r".
local function cells_for(hhmm)
  local out = {}
  local DPOS = {1, 2, 4, 5}          -- "HH:MM" - the colon is position 3
  for i = 1, 4 do
    local g = GLYPH[hhmm:sub(DPOS[i], DPOS[i])]
    local x0 = XS[i]
    for r = 1, DIGH do
      local row = g[r]
      for c = 1, 5 do
        if row:sub(c, c) == "1" then out[#out+1] = {c = x0 + c - 1, r = TOP + r - 1} end
      end
    end
  end
  return out
end

-- Group cells into pieces of up to four, preferring neighbours, so what falls
-- looks like tetrominoes rather than confetti. Bottom rows first: a well fills
-- from the floor up, and the eye knows it.
-- The colon is not a tetromino and does not fall: it is punctuation, and it
-- stays put while the digits come and go.
local COLON_CELLS = {{c = COLON, r = TOP+1}, {c = COLON, r = TOP+2},
                     {c = COLON, r = TOP+4}, {c = COLON, r = TOP+5}}

local function make_pieces(cells)
  local occupied, bykey = {}, {}
  for _, c in ipairs(cells) do bykey[c.c .. "," .. c.r] = c end
  table.sort(cells, function(a, b)
    if a.r ~= b.r then return a.r > b.r end
    return a.c < b.c
  end)
  local pieces = {}
  for _, seed in ipairs(cells) do
    local k = seed.c .. "," .. seed.r
    if not occupied[k] then
      local grp = {seed}; occupied[k] = true
      -- grow greedily through neighbours
      local guard = 0
      while #grp < 4 and guard < 24 do
        guard = guard + 1
        local grew = false
        for _, g in ipairs(grp) do
          for _, d in ipairs({{1,0},{-1,0},{0,1},{0,-1}}) do
            local nk = (g.c+d[1]) .. "," .. (g.r+d[2])
            if bykey[nk] and not occupied[nk] and #grp < 4 then
              occupied[nk] = true; grp[#grp+1] = bykey[nk]; grew = true
            end
          end
        end
        if not grew then break end
      end
      -- Stepping by 3 through seven colours means neighbours never repeat and
      -- the whole palette is used before any colour comes round again.
      local ci = (#pieces * 3) % #PIECE_COLS + 1
      pieces[#pieces+1] = {cells = grp, col = PIECE_COLS[ci]}
    end
  end
  return pieces
end

-- ---------------------------------------------------------------- state
local shown, nextt = nil, nil
local pieces_cur, pieces_new = nil, nil

local function set_time(cur, nxt)
  if shown ~= cur then
    shown, nextt = cur, nxt
    pieces_cur = make_pieces(cells_for(cur))
    pieces_new = make_pieces(cells_for(nxt))
  end
end

-- ---------------------------------------------------------------- drawing
local function cell(c, r, col, bright)
  if r < 0 or r >= ROWS or c < 0 or c >= COLS then return end
  local s = bright or 1.0
  local x, y = c * B, r * B
  px.rect(x, y, B, B, floor(col[1]*s*0.62), floor(col[2]*s*0.62), floor(col[3]*s*0.62), true)
  px.rect(x, y, B-1, B-1, floor(col[1]*s), floor(col[2]*s), floor(col[3]*s), true)
  px.rect(x, y, B-1, 1, min(255,floor(col[1]*s*1.5)), min(255,floor(col[2]*s*1.5)),
          min(255,floor(col[3]*s*1.5)), true)     -- lit top edge
end

local function well()
  px.clear(6, 8, 14)
  for r = 0, ROWS - 1 do                      -- faint grid, so the well reads
    px.rect(0, r*B, W, 1, 12, 16, 26, true)
  end
  px.rect(0, 0, 1, H, 22, 28, 44, true)
  px.rect(W-1, 0, 1, H, 22, 28, 44, true)
end

-- ---------------------------------------------------------------- phases
-- 0.00-0.62  steady
-- 0.62-0.80  line clear, bottom row of the block upwards
-- 0.80-1.00  the next time falls into place
local CLR0, CLR1, ASM0 = 0.62, 0.80, 0.80

function draw()
  local t = px.t()
  local n = px.now()
  local cur = string.format("%02d:%02d", n.hour, n.min)
  local nxt = string.format("%02d:%02d", (n.min == 59) and (n.hour + 1) % 24 or n.hour,
                            (n.min + 1) % 60)
  set_time(cur, nxt)
  well()

  -- Colon: always there, breathing once a second so the clock reads as running.
  local beat = 0.55 + 0.45 * math.abs(sin(pi * n.sec))
  for _, c in ipairs(COLON_CELLS) do cell(c.c, c.r, {225, 232, 245}, beat) end

  if t < CLR0 then
    -- A gloss sweeps across the stack the whole time. A clock that only moves
    -- for ten seconds a minute is a still image with an interruption, and the
    -- other fifty seconds are most of what anyone actually sees.
    -- Two glints half a period apart. One alone spends a quarter of its cycle
    -- off the left edge, and those frames are dead - measured, not assumed.
    local SPAN = 36
    local s1 = ((t * 3.0) % 1.0) * SPAN - 2
    local s2 = (((t * 3.0) + 0.5) % 1.0) * SPAN - 2
    for _, p in ipairs(pieces_cur) do
      for _, c in ipairs(p.cells) do cell(c.c, c.r, p.col) end
    end
    -- Blended toward white, not multiplied: these colours already sit near 255,
    -- so scaling them up clamps and nothing moves. Measured that the hard way.
    for _, p in ipairs(pieces_cur) do
      for _, c in ipairs(p.cells) do
        local k = c.c + c.r * 0.35
        local a = 0.5 * max(max(0, 1 - abs(k - s1) / 3.0),
                            max(0, 1 - abs(k - s2) / 3.0))
        if a > 0.01 then
          for yy = 0, B-2 do for xx = 0, B-2 do
            px.blend(c.c*B + xx, c.r*B + yy, 255, 255, 255, a)
          end end
        end
      end
    end
    -- Every few seconds one piece remembers it is a tetromino and settles.
    local who = floor(t * 14) % max(1, #pieces_cur) + 1
    local ph = (t * 14) % 1.0
    if ph < 0.18 and pieces_cur[who] then
      local q = pieces_cur[who]
      local lift = (ph < 0.09) and 1 or 0
      for _, c in ipairs(q.cells) do cell(c.c, c.r - lift, q.col, 1.15) end
    end

  elseif t < CLR1 then
    -- Seven rows go in sequence from the bottom. The row being cleared flares
    -- white first, which is the whole pleasure of a line clear.
    local k = (t - CLR0) / (CLR1 - CLR0)
    local cleared = floor(k * DIGH)                 -- how many rows are gone
    local frac = k * DIGH - cleared                 -- progress within this row
    local row_going = TOP + DIGH - 1 - cleared
    for _, p in ipairs(pieces_cur) do
      for _, c in ipairs(p.cells) do
        if c.r < row_going then
          -- everything above a cleared row drops by that many rows
          cell(c.c, c.r + cleared, p.col, 0.92)
        elseif c.r == row_going then
          local flare = 1.0 - frac
          if frac < 0.55 then
            cell(c.c, c.r + cleared, {255, 255, 255}, 0.35 + 0.65 * flare)
          end
        end
      end
    end
    if frac < 0.5 then                              -- flash across the full width
      local a = (0.5 - frac) * 0.5
      px.rect(0, (row_going + cleared) * B, W, B,
              floor(255*a), floor(255*a), floor(255*a), true)
    end

  else
    -- Assemble. Each piece gets a slot in the order make_pieces produced
    -- (bottom-up), falls from above the well, and eases into its cells.
    local k = (t - ASM0) / (1.0 - ASM0)
    local np = #pieces_new
    for i, p in ipairs(pieces_new) do
      local start = (i - 1) / np * 0.72
      local u = (k - start) / 0.28
      if u > 0 then
        local landed = u >= 1
        u = min(1, u)
        local ease = 1 - (1 - u) * (1 - u)          -- fast in, settles at the end
        local top_r = math.huge
        for _, c in ipairs(p.cells) do top_r = min(top_r, c.r) end
        local drop = floor((1 - ease) * (top_r + 5))
        local squash = (landed and (k - start - 0.28) < 0.05) and 1.35 or 1.0
        for _, c in ipairs(p.cells) do
          cell(c.c, c.r - drop, p.col, landed and 1.0 or 0.85)
        end
        if landed and squash > 1 then                -- a flash on the lock
          for _, c in ipairs(p.cells) do
            px.glow(c.c*B + 2, (c.r)*B + 2, 6, p.col[1], p.col[2], p.col[3], 0.35)
          end
        end
      end
    end
  end
end

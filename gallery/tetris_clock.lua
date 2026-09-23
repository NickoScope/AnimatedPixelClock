-- @upload-only
-- TETRIS CLOCK - HH:MM that clears like completed lines and is rebuilt by falling tetrominoes.
-- tetris_clock.lua - HH:MM that clears itself like completed lines and is
-- rebuilt by falling tetrominoes when the minute turns.
--
-- The whole panel is the well: 32 x 16 cells of 4 px, on black. The digits
-- are the world clock's bold face, one row taller: 6 x 12 cells, so 24 x 48 px
-- - three quarters of the panel's height - and HH:MM uses every column:
-- margin, digit, gap, digit, gap, colon, gap, digit, gap, digit, margin.
--
-- Nothing lights the background. Every lit pixel is a block of a digit, the
-- colon, or a piece on its way down: no well grid, no glow, no flash band.
-- A block is its colour with a half-bright edge, so a digit reads solid from
-- across the room and still shows the tetrominoes it is made of.

local W, H = px.size()
local B = 4
local COLS, ROWS = W // B, H // B          -- 32 x 16
local floor, min, max, abs, cos, pi = math.floor, math.min, math.max, math.abs, math.cos, math.pi

-- ---------------------------------------------------------------- digits
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
local TOP   = 2                              -- rows 2..13: 8 px above, 8 below
local XS    = {1, 8, 18, 25}                 -- left column of each digit

-- The colon is two 2 x 2 dots, as on the world clock. It is not a tetromino
-- and does not fall: it is punctuation, and it stays put while digits change.
local COLON_CELLS = {}
for _, r in ipairs({3, 4, 7, 8}) do
  COLON_CELLS[#COLON_CELLS + 1] = {c = 15, r = TOP + r}
  COLON_CELLS[#COLON_CELLS + 1] = {c = 16, r = TOP + r}
end

-- Tetromino colours at full saturation, in the order pieces are handed out.
local PIECE_COLS = {
  {0, 235, 255}, {255, 215, 0}, {175, 60, 255}, {40, 230, 60},
  {255, 40, 40}, {40, 110, 255}, {255, 140, 0},
}
local WHITE = {245, 245, 255}

-- Cells the time occupies.
local function cells_for(hhmm)
  local out = {}
  local DPOS = {1, 2, 4, 5}          -- "HH:MM" - the colon is position 3
  for i = 1, 4 do
    local g = GLYPH[hhmm:sub(DPOS[i], DPOS[i])]
    for r = 1, DIGH do
      local row = g[r]
      for c = 1, GW do
        if row:sub(c, c) == "#" then out[#out + 1] = {c = XS[i] + c - 1, r = TOP + r - 1} end
      end
    end
  end
  return out
end

-- Group cells into pieces of up to four, preferring neighbours, so what falls
-- looks like tetrominoes rather than confetti. Bottom rows first: a well fills
-- from the floor up, and the eye knows it.
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
            local nk = (g.c + d[1]) .. "," .. (g.r + d[2])
            if bykey[nk] and not occupied[nk] and #grp < 4 then
              occupied[nk] = true; grp[#grp + 1] = bykey[nk]; grew = true
            end
          end
        end
        if not grew then break end
      end
      -- Stepping by 3 through seven colours means neighbours never repeat and
      -- the whole palette is used before any colour comes round again.
      local ci = (#pieces * 3) % #PIECE_COLS + 1
      pieces[#pieces + 1] = {cells = grp, col = PIECE_COLS[ci]}
    end
  end
  return pieces
end

-- ---------------------------------------------------------------- state
-- The minute that arrives is the one that was falling into place, so its
-- pieces carry over instead of being worked out again in the frame the minute
-- turns.
local shown, next_time
local pieces_cur, pieces_new = nil, nil

local function set_time(cur, nxt)
  if shown ~= cur then
    pieces_cur = (cur == next_time) and pieces_new or make_pieces(cells_for(cur))
    pieces_new = make_pieces(cells_for(nxt))
    shown, next_time = cur, nxt
  end
end

-- ---------------------------------------------------------------- drawing
-- One block: its colour scaled by s and pushed toward white by w, over a
-- half-bright edge. Light is mixed here, in the colour, rather than blended
-- onto the canvas: it never spills past the block, and it costs one rect.
local function cell(c, r, col, s, w)
  if r < 0 or r >= ROWS or c < 0 or c >= COLS then return end
  s = s or 1.0
  local R, G, Bl = col[1] * s, col[2] * s, col[3] * s
  if w and w > 0 then R, G, Bl = R + (255 - R) * w, G + (255 - G) * w, Bl + (255 - Bl) * w end
  R, G, Bl = min(255, floor(R)), min(255, floor(G)), min(255, floor(Bl))
  local x, y = c * B, r * B
  px.rect(x, y, B, B, R // 2, G // 2, Bl // 2, true)
  px.rect(x, y, B - 1, B - 1, R, G, Bl, true)
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
  px.clear(0, 0, 0)

  -- Colon: always there, breathing once a second so the clock reads as running.
  -- px.t() is the second hand on the panel, so t * 60 is seconds with a fraction.
  local beat = 0.8 + 0.2 * abs(cos(pi * t * 60))
  for _, c in ipairs(COLON_CELLS) do cell(c.c, c.r, WHITE, beat) end

  if t < CLR0 then
    -- A gloss sweeps across the stack the whole time. A clock that only moves
    -- for ten seconds a minute is a still image with an interruption, and the
    -- other fifty seconds are most of what anyone actually sees.
    -- Two glints half a period apart. One alone spends a quarter of its cycle
    -- off the left edge, and those frames are dead - measured, not assumed.
    local SPAN = COLS + 8
    local s1 = ((t * 3.0) % 1.0) * SPAN - 4
    local s2 = (((t * 3.0) + 0.5) % 1.0) * SPAN - 4
    -- Every few seconds one piece remembers it is a tetromino and hops.
    local who = floor(t * 14) % max(1, #pieces_cur) + 1
    local hop = ((t * 14) % 1.0) < 0.09 and who or nil
    for i, p in ipairs(pieces_cur) do
      local lift = (i == hop) and 1 or 0
      for _, c in ipairs(p.cells) do
        local k = c.c + c.r * 0.35
        -- toward white, not brighter: these colours already sit at 255
        local a = 0.45 * max(max(0, 1 - abs(k - s1) / 3.0), max(0, 1 - abs(k - s2) / 3.0))
        cell(c.c, c.r - lift, p.col, 1.0, a)
      end
    end

  elseif t < CLR1 then
    -- Twelve rows go in sequence from the bottom. The row being cleared turns
    -- white first, which is the whole pleasure of a line clear, then is gone.
    local k = (t - CLR0) / (CLR1 - CLR0)
    local cleared = floor(k * DIGH)                 -- how many rows are gone
    local frac = k * DIGH - cleared                 -- progress within this row
    local row_going = TOP + DIGH - 1 - cleared
    for _, p in ipairs(pieces_cur) do
      for _, c in ipairs(p.cells) do
        if c.r < row_going then
          -- everything above a cleared row drops by that many rows
          cell(c.c, c.r + cleared, p.col)
        elseif c.r == row_going and frac < 0.55 then
          cell(c.c, c.r + cleared, p.col, 1.0, min(1, frac * 5))
        end
      end
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
        -- a white flash on the lock, inside the piece's own blocks
        local flash = (landed and (k - start - 0.28) < 0.05) and 0.7 or 0
        for _, c in ipairs(p.cells) do cell(c.c, c.r - drop, p.col, 1.0, flash) end
      end
    end
  end
end

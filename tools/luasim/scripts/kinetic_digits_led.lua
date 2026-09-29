-- @upload-only
-- KINETIC DIGITS LED - a flip board of seven-segment digits made of the panel's LEDs.
--
-- The panel pretends to be a mechanical flip-digit display: every digit is
-- seven segments built out of LEDs, and a segment that changes does not
-- switch, it flips - its lit face narrows to its edge, a glint passes, and the
-- dark face comes round (or, in the "line" style, the segment grows out of its
-- centre). Changes run across the board as a wave, column after column. In
-- the other mode every one of the 8,192 LEDs is a flip dot of its own.
--
-- RASTER TO SEGMENTS. A picture - a clock, rings, plasma, Conway's Life, a
-- turning cube, or text in a seven-segment font - lies on a work canvas of
-- 12 x 18 units a digit, and is averaged by area into two images:
--   V, 2 x 2 samples a digit: f=(2x,2y) b=(2x+1,2y) e=(2x,2y+1) c=(2x+1,2y+1)
--   H, 1 x 3 samples a digit: a=(x,3y) g=(x,3y+1) d=(x,3y+2)
-- A sample brighter than the threshold (110, or darker when inverted) turns
-- its segment on: seven bits a digit, a=0x40 ... g=0x01. The picture is
-- never drawn: each source is a function evaluated at the sample points only,
-- which is what makes it affordable at 410 ns a Lua instruction.
--
-- The V/H sampling is Ksawery Kirklewski's idea, from his Flipdigits Player.
-- This code was written from scratch from the owner's prototype (a browser
-- emulation); no code from Owen McAteer's FlipDigits (GPL-3.0) is used. The
-- seven-segment font is the prototype's; it was not checked against
-- edouard-gv/seven-dots (Apache-2.0).
--
-- THE BOARD. Digit cells of 4x7 (32 x 9 digits), 5x9 (25x7), 6x11 (21x5),
-- 8x12 (16x5), 8x16 (16x4), 11x21 (11x3) or 16x32 (8x2) LEDs; one dark LED
-- right of and below every digit; strokes 2 LEDs wide from a cell 10 wide up,
-- else 1; dark corners from a glyph 5 wide up; italic leans the upper half by
-- one LED. The board updates 12 times a second (fewer on the small cells,
-- whose samples cost more); a flip takes 160 ms, eased in and out, starting
-- x*8 + y*4 + 0..10 ms after the change. Everything runs on time, not on
-- frames: a slow frame shows a flip later in its course, never slower.
--
-- It runs its own program of scenes (SHOW below), a change of grid going out
-- as a wave and coming back in. The knob's click or the remote's OK on this
-- page (px.button, firmware 2.7.3+) picks a scene, counted over 0.45 s as
-- OCEANARIUM counts them: one press the next scene, two the one before, and a
-- chosen scene stays until three presses hand the board back to the program.
-- The scene's name shows for a second on the dark board before it fills.

PERIOD = 600.0
FPS = 15

local floor, sin, cos, sqrt, abs = math.floor, math.sin, math.cos, math.sqrt, math.abs
local max, min = math.max, math.min
local pi = math.pi
local W, H = 128, 64
local rect, pixel, clear = px.rect, px.pixel, px.clear

-- ── the program ─────────────────────────────────────────────────────────────
-- mode: "digits" or "dots"; size: the cell; src: text, clock, rings, plasma,
-- life, cube; pal: flip, src, rainbow, amber, white; style: plate or line.
local MARQUEE = "NICE COTE D'AZUR - ARRIVALS - 12:45 LANDED"
local SHOW = {
  { mode = "digits", size = "8x12", src = "text", pal = "flip", style = "plate", secs = 30 },
  { mode = "digits", size = "8x12", src = "plasma", pal = "src", style = "plate", secs = 22 },
  { mode = "digits", size = "6x11", src = "rings", pal = "rainbow", style = "plate", secs = 20 },
  { mode = "digits", size = "11x21", src = "clock", pal = "amber", style = "line", secs = 20 },
  { mode = "digits", size = "5x9", src = "life", pal = "flip", style = "plate", secs = 22 },
  { mode = "digits", size = "16x32", src = "cube", pal = "white", style = "plate", secs = 18 },
  { mode = "dots", src = "plasma", pal = "src", style = "plate", secs = 22 },
  { mode = "digits", size = "8x16", src = "text", pal = "flip", style = "line", ital = true, secs = 26 },
  { mode = "dots", src = "rings", pal = "rainbow", style = "plate", secs = 18 },
  { mode = "digits", size = "4x7", src = "plasma", pal = "src", style = "plate", secs = 18 },
}
local THR, INV = 110, false
local DFPS = 12                     -- board updates a second
local FLIP = 0.160                  -- seconds a flip takes
local WAVE = 0.008                  -- seconds of delay a column (half that a row)

-- LUA_32BITS: xorshift, as the other screens do.
local seed = 0x6b1dd1
local function rnd()
  seed = seed ~ ((seed << 13) & 0x7fffffff)
  seed = seed ~ (seed >> 17)
  seed = seed ~ ((seed << 5) & 0x7fffffff)
  return (seed & 0x7fffffff) / 2147483647.0
end

-- ── colour ──────────────────────────────────────────────────────────────────
-- A hue table, so a pixel of plasma is a lookup and not an HSL conversion.
local HUE_R, HUE_G, HUE_B = {}, {}, {}
do
  local function f(p, q, t)
    t = t % 1
    if t < 1 / 6 then return p + (q - p) * 6 * t end
    if t < 1 / 2 then return q end
    if t < 2 / 3 then return p + (q - p) * (2 / 3 - t) * 6 end
    return p
  end
  local l, s = 0.5, 0.9
  local q = l < 0.5 and l * (1 + s) or l + s - l * s
  local p = 2 * l - q
  for h = 0, 359 do
    local hh = h / 360
    HUE_R[h], HUE_G[h], HUE_B[h] = f(p, q, hh + 1 / 3) * 255, f(p, q, hh) * 255, f(p, q, hh - 1 / 3) * 255
  end
end
-- and the brightness of each hue, as the threshold sees it
local HUE_L = {}
for h = 0, 359 do HUE_L[h] = HUE_R[h] * 0.299 + HUE_G[h] * 0.587 + HUE_B[h] * 0.114 end
-- a hue's face in the "source colour" palette: normalised to its brightest
-- channel, so it depends on the hue only and is a lookup, not arithmetic
local HFACE = {}
for h = 0, 359 do
  local m = max(HUE_R[h], HUE_G[h], HUE_B[h], 1)
  HFACE[h] = (floor(HUE_R[h] / m * 255) << 16) | (floor(HUE_G[h] / m * 255) << 8) | floor(HUE_B[h] / m * 255)
end
local function hue(h)
  local i = floor(h) % 360
  return HUE_R[i], HUE_G[i], HUE_B[i]
end
local FACE = { flip = { 228, 255, 40 }, amber = { 255, 150, 20 }, white = { 255, 255, 255 } }

-- ── the seven-segment font (the prototype's) ────────────────────────────────
local BIT = { a = 0x40, b = 0x20, c = 0x10, d = 0x08, e = 0x04, f = 0x02, g = 0x01 }
local SEGS = { "a", "b", "c", "d", "e", "f", "g" }
local FONT = {
  [" "] = 0, ["0"] = 0x7E, ["1"] = 0x30, ["2"] = 0x6D, ["3"] = 0x79, ["4"] = 0x33, ["5"] = 0x5B,
  ["6"] = 0x5F, ["7"] = 0x70, ["8"] = 0x7F, ["9"] = 0x7B,
  A = 0x77, B = 0x1F, C = 0x4E, D = 0x3D, E = 0x4F, F = 0x47, G = 0x5E, H = 0x37, I = 0x06, J = 0x3C,
  K = 0x57, L = 0x0E, M = 0x76, N = 0x15, O = 0x1D, P = 0x67, Q = 0x73, R = 0x05, S = 0x5B, T = 0x0F,
  U = 0x3E, V = 0x1C, W = 0x2A, X = 0x37, Y = 0x3B, Z = 0x6D,
  ["А"] = 0x77, ["Б"] = 0x5F, ["В"] = 0x7F, ["Г"] = 0x46, ["Е"] = 0x4F, ["Ё"] = 0x4F, ["З"] = 0x79,
  ["К"] = 0x57, ["М"] = 0x76, ["Н"] = 0x37, ["О"] = 0x7E, ["П"] = 0x76, ["Р"] = 0x67, ["С"] = 0x4E,
  ["Т"] = 0x0F, ["У"] = 0x3B, ["Х"] = 0x37, ["Ч"] = 0x33, ["Ь"] = 0x1F, ["Э"] = 0x79, ["Ы"] = 0x1F,
  ["-"] = 0x01, ["—"] = 0x01, ["_"] = 0x08, ["="] = 0x09, ["°"] = 0x63, ["'"] = 0x02, ['"'] = 0x22,
  ["?"] = 0x65, ["."] = 0x08, [","] = 0x08, [":"] = 0x09, ["!"] = 0x30,
}
-- The text as a list of characters. The sandbox has no utf8 library, so the
-- two- and three-byte sequences are cut by hand.
local function chars(s)
  local out, i = {}, 1
  while i <= #s do
    local c = s:byte(i)
    local n = c < 0x80 and 1 or (c < 0xE0 and 2 or (c < 0xF0 and 3 or 4))
    local ch = s:sub(i, i + n - 1)
    if n == 1 then ch = ch:upper() end
    out[#out + 1] = ch
    i = i + n
  end
  return out
end
local MARQ = chars(MARQUEE)

-- ── time ────────────────────────────────────────────────────────────────────
local T, tprev = 0, nil
local function ease(p) return p < 0.5 and 2 * p * p or 1 - ((-2 * p + 2) ^ 2) / 2 end

-- ── the board ───────────────────────────────────────────────────────────────
-- G holds the grid; the segment state is flat arrays indexed by
-- (digit * 7 + segment), the dot state by LED.
local G = nil
local ON, FROM, ST, COL = {}, {}, {}, {}       -- target, start amount, start time, delay, colour
local RX, RY, RW, RH = {}, {}, {}, {}                 -- a segment's rect (and its italic second rect)
local RX2, RY2, RW2, RH2 = {}, {}, {}, {}
local HORIZ = {}
local SXs, SYs = {}, {}                              -- a segment's sample point on the work canvas
-- the coarse grid the smooth sources are evaluated on (see "updating the board")
local SP, NXn, NYn = 12, 0, 0       -- grid spacing in work units, nodes across and down
local GA, GXf, GYf = {}, {}, {}     -- per digit sample: top-left node, and the weights
local CXI, CXF = {}, {}             -- per LED column in the dot mode
local CH, CHN = {}, 0               -- a pass's changes: i to turn on, -(i+1) to turn off
local function field_grid(Wc, Hc)
  NXn, NYn = floor(Wc / SP) + 2, floor(Hc / SP) + 2
end
local ACTIVE, NACT = {}, 0                            -- the segments or dots in flight
local INACT = {}
local scene, sceneAt, going = 1, 0, false
local DOTS = false

local function segrects(s, cw, ch, ital)
  local w, h = cw - 1, ch - 1
  local t = cw >= 10 and 2 or 1
  local mid = floor((h - t) / 2)
  local corner = w >= 5 and t or 0
  local x, y, rw, rh
  if s == "a" then x, y, rw, rh = corner, 0, w - 2 * corner, t
  elseif s == "g" then x, y, rw, rh = corner, mid, w - 2 * corner, t
  elseif s == "d" then x, y, rw, rh = corner, h - t, w - 2 * corner, t
  elseif s == "f" then x, y, rw, rh = 0, t, t, mid - t
  elseif s == "b" then x, y, rw, rh = w - t, t, t, mid - t
  elseif s == "e" then x, y, rw, rh = 0, mid + t, t, h - t - mid - t
  else x, y, rw, rh = w - t, mid + t, t, h - t - mid - t end
  if not ital then return x, y, rw, rh end
  -- Italic: the rows above the middle of the glyph lean one LED right.
  local yh = (h - 1) / 2
  local top = min(rh, max(0, floor(yh - y) + ((yh - y) % 1 > 0 and 1 or 0)))
  if y + rh - 1 < yh then return x + 1, y, rw, rh end
  if y >= yh then return x, y, rw, rh end
  return x + 1, y, rw, top, x, y + top, rw, rh - top
end

-- Flips are put on a timetable: a bucket every 1/60 s holds what starts (a
-- segment) or what happens (a dot) in it, so a frame touches only what is
-- due. Before, every frame walked every flip in progress, the ones the wave
-- had not reached yet included - a hundred thousand instructions a frame
-- when a dot board changed thousands of dots at once.
local BK, BKlast = {}, 0
local BKN = {}                      -- entries in each bucket; buckets are reused, not made
local POOL, NPOOL = {}, 0
local RATE = 60
local function schedule(time, e)
  local k = floor(time * RATE)
  -- A flip whose moment has already gone by (a commit spread over frames
  -- starts from its first frame) goes in the next bucket: a bucket behind
  -- the timetable is never read, and that flip would never happen.
  if k <= BKlast then k = BKlast + 1 end
  local b = BK[k]
  if not b then
    if NPOOL > 0 then b = POOL[NPOOL]; POOL[NPOOL] = nil; NPOOL = NPOOL - 1 else b = {} end
    BK[k] = b
    BKN[k] = 0
  end
  local n = BKN[k] + 1
  BKN[k] = n
  b[n] = e
end
local function reset_active()
  for i = 1, NACT do ACTIVE[i] = nil end
  NACT = 0
  for i = 0, #INACT do INACT[i] = false end
  for k in pairs(BK) do BK[k] = nil; BKN[k] = nil end
  BKlast = floor(T * RATE)
end

local function build(sc)
  clear(0, 0, 0)
  reset_active()
  -- The state tables are made once and kept: a new set of 8,192-entry
  -- tables a scene left megabytes to the collector (3.4 MB of the effect's
  -- 4 MB at the peak, measured). Nothing needs resetting between scenes -
  -- the wave out has left every segment dark - only entries never used yet
  -- are filled in.
  local need = sc.mode == "dots" and W * H or 32 * 9 * 7
  for i = 0, need - 1 do
    if ON[i] == nil then ON[i] = 0; FROM[i] = 0; ST[i] = 0; COL[i] = 0; INACT[i] = false
    else ON[i] = 0; FROM[i] = 0 end
  end
  DOTS = sc.mode == "dots"
  if DOTS then
    G = { cols = W, rows = H, n = W * H, phase = "idle", P = 0 }
    SP = 8                                    -- 4 LEDs
    field_grid(W * 2, H * 2)
    for x = 0, W - 1 do
      local wx = (x * 2 + 1) / SP
      CXI[x] = floor(wx)
      CXF[x] = wx - CXI[x]
    end
    CHN = 0
    return
  end
  local cw, ch = sc.size:match("(%d+)x(%d+)")
  cw, ch = tonumber(cw), tonumber(ch)
  local cols, rows = floor(W / cw), floor(H / ch)
  G = { cw = cw, ch = ch, cols = cols, rows = rows, n = cols * rows,
        ox = floor((W - cols * cw) / 2), oy = floor((H - rows * ch) / 2) }
  G.P, G.phase = 0, "idle"
  CHN = 0
  SP = 12                                     -- a digit's width
  field_grid(cols * 12, rows * 18)
  for d = 0, cols * rows - 1 do
    local dx, dy = d % cols, floor(d / cols)
    local bx, by = G.ox + dx * cw, G.oy + dy * ch
    for k = 1, 7 do
      local s = SEGS[k]
      local i = d * 7 + k - 1
      local x, y, rw, rh, x2, y2, rw2, rh2 = segrects(s, cw, ch, sc.ital)
      RX[i], RY[i], RW[i], RH[i] = bx + x, by + y, rw, rh
      RX2[i] = x2 and bx + x2 or nil
      RY2[i], RW2[i], RH2[i] = y2 and by + y2, rw2, rh2
      HORIZ[i] = (s == "a" or s == "g" or s == "d")
      ON[i], FROM[i], ST[i], COL[i] = 0, 0, 0, 0
      -- where the segment is sampled: V is 2 x 2 a digit, H is 1 x 3
      if HORIZ[i] then
        local row = (s == "a" and 0 or (s == "g" and 1 or 2))
        SXs[i], SYs[i] = dx * 12 + 6, dy * 18 + row * 6 + 3
      else
        local sx = (s == "b" or s == "c") and 1 or 0
        local sy = (s == "e" or s == "c") and 1 or 0
        SXs[i], SYs[i] = dx * 12 + sx * 6 + 3, dy * 18 + sy * 9 + 4.5
      end
      -- and where that point falls in the field's grid
      local gx, gy = SXs[i] / SP, SYs[i] / SP
      local cx, cy = floor(gx), floor(gy)
      GA[i], GXf[i], GYf[i] = cy * NXn + cx, gx - cx, gy - cy
    end
  end
end

-- ── sources ─────────────────────────────────────────────────────────────────
-- Each is sampled at a point of the work canvas (12 x 18 units a digit, or
-- 2 units an LED in the dot mode) and returns r, g, b.
local SRC = {}
local unit = 12

function SRC.plasma(x, y, t)
  local u, v = x / unit, y / unit
  local f = sin(u * 0.9 + t) + sin(v * 1.1 - t * 0.7) + sin((u + v) * 0.6 + t * 0.5)
  local du, dv = u - 6 - 3 * sin(t * 0.3), v - 3
  f = f + sin(sqrt(du * du + dv * dv) * 1.2 - t * 1.3)
  local band = sin(f * 1.9) * 0.5 + 0.5
  local r, g, b = hue(f * 45 + t * 25)
  return r * band, g * band, b * band
end

local ringC = { 0, 0 }
function SRC.rings(x, y, t)
  local dx, dy = x - ringC[1], y - ringC[2]
  local r = sqrt(dx * dx + dy * dy)
  local v = sin(r * 1.9 / unit - t * 3.2) * 0.5 + 0.5
  local rr, g, b = hue(r / unit * 18 + t * 40)
  return rr * v, g * v, b * v
end

-- The clock: HH:MM (or HH:MM:SS on a wide board) in the 5x7 font, scaled up to
-- fill the canvas, blue to white to orange across it.
local GLYPH = {}
local clockStr, clockScale, clockX0, clockY0 = "", 1, 0, 0
local function capture_glyphs()
  -- Drawn once with px.text's classic 5x7 font and read back: the font the
  -- panel already has, not a copy of it.
  for _, ch in ipairs({ "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":" }) do
    clear(0, 0, 0)
    px.text(0, 0, ch, 255, 255, 255, "5x7")
    local rows = {}
    for yy = 0, 7 do
      local row = {}
      for xx = 0, 4 do
        local r = px.get(xx, yy)
        row[xx] = r > 0
      end
      rows[yy] = row
    end
    GLYPH[ch] = rows
  end
  clear(0, 0, 0)
end
local cW, cH = 192, 90
local function clock_setup(Wc, Hc)
  local now = px.now()
  local p2 = function(n) return string.format("%02d", n) end
  clockStr = (Wc / Hc < 1.6) and (p2(now.hour) .. ":" .. p2(now.min))
                                or (p2(now.hour) .. ":" .. p2(now.min) .. ":" .. p2(now.sec))
  local n = #clockStr
  clockScale = min(Hc * 0.8 / 8, Wc * 0.94 / (n * 6))
  clockX0 = (Wc - n * 6 * clockScale) / 2
  clockY0 = (Hc - 8 * clockScale) / 2
  cW, cH = Wc, Hc
end
function SRC.clock(x, y, t)
  local gx, gy = (x - clockX0) / clockScale, (y - clockY0) / clockScale
  if gx < 0 or gy < 0 or gy >= 8 then return 0, 0, 0 end
  local ci = floor(gx / 6)
  local ch = clockStr:sub(ci + 1, ci + 1)
  local gl = GLYPH[ch]
  local cx = floor(gx - ci * 6)
  if not gl or cx > 4 or not gl[floor(gy)][cx] then return 0, 0, 0 end
  local k = x / cW
  if k < 0.5 then
    local m = k / 0.5
    return 90 + 165 * m, 200 + 55 * m, 255
  end
  local m = (k - 0.5) / 0.5
  return 255, 255 - 85 * m, 255 - 195 * m
end

-- Life: a grid of 4 x 6 cells a digit, stepped at the board's rate; a sample
-- is the average over the cells under it, by area, as the canvas would be.
local life, lifeW, lifeH, lifeHue, lifeStale, lifePop = nil, 0, 0, 0, 0, -1
local lifeNext, lifeRow, lifeCount = {}, 0, 0
local LIFE_ROWS = 3                 -- rows of the next generation a frame
local function life_begin(Wl, Hl)
  lifeW, lifeH, life, lifeNext = Wl, Hl, {}, {}
  lifeHue = rnd() * 360
  for i = 0, Wl * Hl - 1 do life[i] = rnd() < 0.3 and 1 or 0 end
  lifeStale, lifePop, lifeRow, lifeCount = 0, -1, 0, 0
end
-- The next generation is worked out a few rows a frame into a second buffer
-- and swapped in when it is whole: a whole step in one frame cost up to half
-- a million instructions on the small cells.
local function life_chunk(Wl, Hl)
  if not life or lifeW ~= Wl or lifeH ~= Hl then life_begin(Wl, Hl); return end
  local n = lifeNext
  local yEnd = min(Hl, lifeRow + LIFE_ROWS)
  for y = lifeRow, yEnd - 1 do
    local ym, yp = ((y - 1) % Hl) * Wl, ((y + 1) % Hl) * Wl
    local y0 = y * Wl
    for x = 0, Wl - 1 do
      local xm, xp = (x - 1) % Wl, (x + 1) % Wl
      local k = (life[ym + xm] > 0 and 1 or 0) + (life[ym + x] > 0 and 1 or 0) + (life[ym + xp] > 0 and 1 or 0)
              + (life[y0 + xm] > 0 and 1 or 0) + (life[y0 + xp] > 0 and 1 or 0)
              + (life[yp + xm] > 0 and 1 or 0) + (life[yp + x] > 0 and 1 or 0) + (life[yp + xp] > 0 and 1 or 0)
      local a = life[y0 + x]
      local v = 0
      if a > 0 and (k == 2 or k == 3) then v = min(40, a + 1) elseif a == 0 and k == 3 then v = 1 end
      n[y0 + x] = v
      if v > 0 then lifeCount = lifeCount + 1 end
    end
  end
  lifeRow = yEnd
  if lifeRow < Hl then return end
  life, lifeNext = n, life
  local pop = lifeCount
  lifeRow, lifeCount = 0, 0
  lifeStale = abs(pop - lifePop) <= 2 and lifeStale + 1 or 0
  lifePop = pop
  if lifeStale > 40 or pop < Wl * Hl * 0.03 then life_begin(Wl, Hl) end
end
-- The life cells in a rectangle of cells: their colour, and the share of
-- them alive as the brightness. (By luma a blue cell would never pass the
-- threshold at all: a board of blue life went dark in the simulator.)
local function life_avg(x0, y0, x1, y1)
  local r, g, b, n, alive = 0, 0, 0, 0, 0
  for y = y0, y1 - 1 do
    for x = x0, x1 - 1 do
      local a = life[y * lifeW + x]
      if a and a > 0 then
        local hr, hg, hb = hue(lifeHue + a * 6)
        r, g, b, alive = r + hr, g + hg, b + hb, alive + 1
      end
      n = n + 1
    end
  end
  if alive == 0 then return 0, 0, 0, 0 end
  return r / alive, g / alive, b / alive, alive / n * 255
end

-- The cube: its twelve edges stamped into the sample grids, thick.
local cubeEdges = {}
local function cube_setup(Wc, Hc, t)
  local R = min(Wc, Hc) * 0.34
  local ax, ay = t * 0.7, t * 0.9
  local V = {}
  for i = 0, 7 do
    local x = (i & 1) > 0 and 1 or -1
    local y = (i & 2) > 0 and 1 or -1
    local z = (i & 4) > 0 and 1 or -1
    local y2, z2 = y * cos(ax) - z * sin(ax), y * sin(ax) + z * cos(ax)
    local x3, z3 = x * cos(ay) + z2 * sin(ay), -x * sin(ay) + z2 * cos(ay)
    local p = 3.2 / (3.2 + z3)
    V[i] = { Wc / 2 + x3 * R * p, Hc / 2 + y2 * R * p }
  end
  local E = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
              { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } }
  for i = 1, 12 do
    local a, b = V[E[i][1]], V[E[i][2]]
    local r, g, bb = hue(i * 30 + t * 30)
    cubeEdges[i] = { a[1], a[2], b[1], b[2], r, g, bb }
  end
end
function SRC.cube(x, y, t)
  local lw = max(3, unit * 1.1) * 0.5
  local best, br, bg, bb = lw * lw, 0, 0, 0
  local hit = false
  for i = 1, 12 do
    local e = cubeEdges[i]
    local ex, ey = e[3] - e[1], e[4] - e[2]
    local L = ex * ex + ey * ey
    local k = L > 0 and ((x - e[1]) * ex + (y - e[2]) * ey) / L or 0
    if k < 0 then k = 0 elseif k > 1 then k = 1 end
    local dx, dy = x - (e[1] + ex * k), y - (e[2] + ey * k)
    local d = dx * dx + dy * dy
    if d <= best then best, br, bg, bb, hit = d, e[5], e[6], e[7], true end
  end
  if hit then return br, bg, bb end
  return 0, 0, 0
end

-- Text on the board: the clock on the top row, the marquee along the middle
-- at 2.5 characters a second, an underline along the bottom. The bits are
-- the font's; in the digits mode they go to the segments directly.
-- The clock line, made once a pass: px.now() makes a table and
-- string.format a string, and made once a digit they were most of the
-- garbage the collector had to clear.
local textClock = ""
local function text_clock(cols)
  local now = px.now()
  textClock = cols >= 8 and string.format("%02d-%02d-%02d", now.hour, now.min, now.sec)
                         or string.format("%02d%02d", now.hour, now.min)
end
local function text_bits(x, y, cols, rows, t)
  local mid = floor((rows - 1) / 2)
  if y == mid then
    local off = floor(t * 2.5) % (#MARQ + cols)
    local i = off + x - cols + 1
    local ch = MARQ[i]
    return ch and (FONT[ch] or 0) or 0, 255, 255, 255
  end
  if y == 0 and rows > 2 then
    local s = textClock
    local s0 = floor((cols - #s) / 2)
    local j = x - s0 + 1
    if j < 1 or j > #s then return 0, 0, 0, 0 end
    return FONT[s:sub(j, j)] or 0, 111, 211, 255
  end
  if y == rows - 1 and rows > 2 then return 0x08, 255, 154, 60 end
  return 0, 0, 0, 0
end

-- ── what a lit face shows ───────────────────────────────────────────────────
local function face(pal, r, g, b, col, cols, t)
  if pal == "src" then
    if INV then r, g, b = 255 - r, 255 - g, 255 - b end
    local m = max(r, g, b, 1)
    return floor(r / m * 255), floor(g / m * 255), floor(b / m * 255)
  elseif pal == "rainbow" then
    local hr, hg, hb = hue(col / cols * 300 + t * 30)
    return floor(hr), floor(hg), floor(hb)
  end
  local c = FACE[pal] or FACE.flip
  return c[1], c[2], c[3]
end
local function pack(r, g, b) return (r << 16) | (g << 8) | b end

-- A change: the segment or dot i now wants `want`, starting after `delay`.
local function retarget(i, want, delay, base)
  base = base or T
  local from, to = FROM[i], ON[i]
  if from ~= to then
    local p = (T - ST[i]) / FLIP
    if p < 0 then p = 0 elseif p > 1 then p = 1 end
    from = from + (to - from) * ease(p)
  end
  local start = base + delay
  FROM[i], ON[i], ST[i] = from, want, start
  if DOTS then
    -- A dot is one LED: its flip is two moments, the edge and the new face.
    schedule(start + FLIP * 0.5, -(i + 1))
    schedule(start + FLIP, i)
  elseif not INACT[i] then
    INACT[i] = true
    schedule(start, i)
  end
end

local function lum(r, g, b) return r * 0.299 + g * 0.587 + b * 0.114 end
local function lit(l) return (l > THR) ~= INV end

-- ── updating the board ──────────────────────────────────────────────────────
-- A pass works out what the whole board should show, a budget of work a
-- frame, WITHOUT showing any of it; when the pass is complete every change is
-- started at once, as one wave, the way the prototype's 12 Hz frame does. An
-- earlier version flipped each part as soon as it was sampled, and the owner
-- saw it (2026-09-29): the dot screens were redrawn band by band, top to
-- bottom, instead of changing.
--
-- Plasma and rings are smooth fields: they are evaluated on a coarse grid of
-- nodes and interpolated at each sample, and only the cheap last step - the
-- bands and the hue - is done per sample. In the dot mode that is per LED, a
-- row at a time, with the weights worked out once.
local BUDGET = 30000                -- Lua instructions a frame for the update (digits)
local BUDGET_DOTS = 55000           -- the dot board's frames draw little, so it gets more
local COST = { text = 50, clock = 70, life = 170, cube = 300, node = 60, fieldsample = 45, dotrow = 4500 }
local TEXTB, TEXTR, TEXTG, TEXTB2 = {}, {}, {}, {}
local lastSlot = -1
local NODE = {}                     -- the field on the coarse grid
local ROWV = {}
local plasmaC = 6

local FIELD = {}
function FIELD.plasma(x, y, t)
  local u, v = x / unit, y / unit
  local du, dv = u - plasmaC, v - 3
  return sin(u * 0.9 + t) + sin(v * 1.1 - t * 0.7) + sin((u + v) * 0.6 + t * 0.5)
         + sin(sqrt(du * du + dv * dv) * 1.2 - t * 1.3)
end
function FIELD.rings(x, y, t)
  local dx, dy = x - ringC[1], y - ringC[2]
  return sqrt(dx * dx + dy * dy)
end
-- the last step: a field value to a hue index and a brightness 0..1
local function shade(src, v, t)
  if src == "plasma" then
    return floor(v * 45 + t * 25) % 360, sin(v * 1.9) * 0.5 + 0.5
  end
  return floor(v / unit * 18 + t * 40) % 360, sin(v * 1.9 / unit - t * 3.2) * 0.5 + 0.5
end

local function pass_setup(sc, t)
  local src = sc.src
  CHN = 0
  unit = 12
  plasmaC = 6 + 3 * sin(t * 0.3)
  local Wc, Hc
  if DOTS then Wc, Hc = W * 2, H * 2 else Wc, Hc = G.cols * 12, G.rows * 18 end
  if src == "rings" then
    ringC[1], ringC[2] = Wc / 2 + sin(t * 0.4) * Wc * 0.18, Hc / 2 + cos(t * 0.3) * Hc * 0.18
  end
  if DOTS then return end
  local cols, rows = G.cols, G.rows
  if src == "clock" then clock_setup(Wc, Hc)
  elseif src == "cube" then cube_setup(Wc, Hc, t)
  elseif src == "text" then
    text_clock(cols)
    for d = 0, G.n - 1 do
      TEXTB[d], TEXTR[d], TEXTG[d], TEXTB2[d] = text_bits(d % cols, floor(d / cols), cols, rows, t)
    end
  end
end

local SEGBIT = { [0] = 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 }

-- One sample's result into the pass: its colour now, its state at the commit.
local function result(i, want, pal, r, g, b, col, cols, t, constFace)
  if want == 1 then COL[i] = constFace or pack(face(pal, r, g, b, col, cols, t)) end
  if ON[i] ~= want then CHN = CHN + 1; CH[CHN] = want == 1 and i or -(i + 1) end
end

-- The changes start from one moment (the pass's commit), though starting
-- them is spread over a few frames when there are thousands: the wave is
-- the same either way, because each flip's time is the commit's plus its
-- own delay.
local COMMIT_PER_FRAME = 700
local commitAt, commitK = 0, 1
local function commit()
  if commitK == 1 then commitAt = T end
  local last = min(CHN, commitK + COMMIT_PER_FRAME - 1)
  for k = commitK, last do
    local e = CH[k]
    local i = e < 0 and -e - 1 or e
    local delay
    if DOTS then delay = (i % W) / 8 * WAVE + floor(i / W) / 16 * WAVE
    else local d = floor(i / 7); delay = (d % G.cols) * WAVE + floor(d / G.cols) * WAVE * 0.5 end
    retarget(i, e < 0 and 0 or 1, delay + rnd() * 0.010, commitAt)
    CH[k] = nil
  end
  if last >= CHN then CHN, commitK = 0, 1; return true end
  commitK = last + 1
  return false
end

local function sample_chunk(sc, t)
  local src = sc.src
  local isField = FIELD[src] ~= nil
  local pal = sc.pal
  local constFace = FACE[pal] and pack(FACE[pal][1], FACE[pal][2], FACE[pal][3])
  if src == "life" and not DOTS then life_chunk(G.cols * 4, G.rows * 6) end
  local spent = 0
  local BUDGET = DOTS and BUDGET_DOTS or BUDGET
  while spent < BUDGET do
    local ph = G.phase
    if ph == "idle" then
      local slot = floor(t * DFPS)
      if slot == lastSlot then return end
      lastSlot = slot
      pass_setup(sc, t)
      G.phase, G.P = isField and "nodes" or "samples", 0
    elseif ph == "nodes" then
      -- the field on the grid
      local f = FIELD[src]
      local k, nN = G.P, NXn * NYn
      while k < nN and spent < BUDGET do
        NODE[k] = f((k % NXn) * SP, floor(k / NXn) * SP, t)
        k = k + 1
        spent = spent + COST.node
      end
      G.P = k
      if k >= nN then G.phase, G.P = "samples", 0 end
    elseif ph == "samples" then
      if DOTS then
        -- a row of LEDs at a time: the row of the grid, then each LED along it
        local y = G.P
        while y < H and spent < BUDGET do
          local wy = (y * 2 + 1) / SP
          local j = floor(wy)
          local fy = wy - j
          local r0, r1 = j * NXn, (j + 1) * NXn
          for c = 0, NXn - 1 do ROWV[c] = NODE[r0 + c] + (NODE[r1 + c] - NODE[r0 + c]) * fy end
          local base = y * W
          local plasma = src == "plasma"
          local k1 = plasma and 45 or 18 / unit
          local k0 = plasma and t * 25 or t * 40
          local b1 = plasma and 1.9 or 1.9 / unit
          local b0 = plasma and 0 or -t * 3.2
          local srcFace = pal == "src"
          for x = 0, W - 1 do
            local c = CXI[x]
            local a = ROWV[c]
            local v = a + (ROWV[c + 1] - a) * CXF[x]
            local h = floor(v * k1 + k0) % 360
            local band = sin(v * b1 + b0) * 0.5 + 0.5
            local want = ((HUE_L[h] * band) > THR) ~= INV and 1 or 0
            local i = base + x
            if want == 1 then
              if constFace then COL[i] = constFace
              elseif srcFace and not INV then COL[i] = HFACE[h]
              else COL[i] = pack(face(pal, HUE_R[h] * band, HUE_G[h] * band, HUE_B[h] * band, x, W, t)) end
            end
            if ON[i] ~= want then CHN = CHN + 1; CH[CHN] = want == 1 and i or -(i + 1) end
          end
          y = y + 1
          spent = spent + COST.dotrow
        end
        G.P = y
        if y >= H then G.phase = "commit" end
      else
        local n = G.n * 7
        local i = G.P
        local per = isField and COST.fieldsample or (COST[src] or 80)
        local f = SRC[src]
        local cols = G.cols
        while i < n and spent < BUDGET do
          local d = floor(i / 7)
          local r, g, b, l
          if isField then
            local a, fx, fy = GA[i], GXf[i], GYf[i]
            local top = NODE[a] + (NODE[a + 1] - NODE[a]) * fx
            local bot = NODE[a + NXn] + (NODE[a + NXn + 1] - NODE[a + NXn]) * fx
            local h, band = shade(src, top + (bot - top) * fy, t)
            l = HUE_L[h] * band
            if pal == "src" and not INV then
              -- straight to the face: it depends on the hue only
              local want = (l > THR) and 1 or 0
              if want == 1 then COL[i] = HFACE[h] end
              if ON[i] ~= want then CHN = CHN + 1; CH[CHN] = want == 1 and i or -(i + 1) end
              goto next
            end
            r, g, b = HUE_R[h] * band, HUE_G[h] * band, HUE_B[h] * band
          elseif src == "text" then
            if (TEXTB[d] & SEGBIT[i % 7]) > 0 then r, g, b = TEXTR[d], TEXTG[d], TEXTB2[d] else r, g, b = 0, 0, 0 end
          elseif src == "life" then
            if life then
              -- the segment's rectangle of life cells: 4 x 6 cells a digit
              local k7, dx, dy = i % 7, d % cols, floor(d / cols)
              if k7 == 0 or k7 == 3 or k7 == 6 then
                local row = k7 == 0 and 0 or (k7 == 6 and 1 or 2)
                r, g, b, l = life_avg(dx * 4, dy * 6 + row * 2, dx * 4 + 4, dy * 6 + row * 2 + 2)
              else
                local sx = (k7 == 1 or k7 == 2) and 2 or 0
                local sy = (k7 == 2 or k7 == 4) and 3 or 0
                r, g, b, l = life_avg(dx * 4 + sx, dy * 6 + sy, dx * 4 + sx + 2, dy * 6 + sy + 3)
              end
            else r, g, b, l = 0, 0, 0, 0 end
          else
            r, g, b = f(SXs[i], SYs[i], t)
          end
          do
            local want = lit(l or lum(r, g, b)) and 1 or 0
            result(i, want, pal, r, g, b, d % cols, cols, t, constFace)
          end
          ::next::
          i = i + 1
          spent = spent + per
        end
        G.P = i
        if i >= n then G.phase = "commit" end
      end
    else
      if commit() then G.phase = "idle" end
      return
    end
  end
end

-- ── drawing what is in flight ───────────────────────────────────────────────
-- The canvas is never cleared: a segment is drawn only while it flips, and
-- its last drawing stays until it flips again.
local function draw_seg(i, r, g, b)
  rect(RX[i], RY[i], RW[i], RH[i], r, g, b, true)
  if RX2[i] then rect(RX2[i], RY2[i], RW2[i], RH2[i], r, g, b, true) end
end

local function run_timetable(style)
  local now = floor(T * RATE)
  for k = BKlast + 1, now do
    local b = BK[k]
    if b then
      local bn = BKN[k]
      BK[k], BKN[k] = nil, nil
      for j = 1, bn do
        local e = b[j]
        if DOTS then
          local i = e < 0 and -e - 1 or e
          local st = ST[i]
          local c = COL[i]
          local cr, cg, cb = (c >> 16) & 255, (c >> 8) & 255, c & 255
          if e < 0 then
            -- the edge: only if this is still the flip it was set for
            if abs(T - (st + FLIP * 0.5)) < 0.06 and FROM[i] ~= ON[i] then
              if style == "line" then
                pixel(i % W, floor(i / W), floor(cr * 0.5), floor(cg * 0.5), floor(cb * 0.5))
              else
                pixel(i % W, floor(i / W), 89 + floor(cr * 0.15), 89 + floor(cg * 0.15), 89 + floor(cb * 0.15))
              end
            end
          elseif T >= st + FLIP - 1 / RATE then
            if ON[i] == 1 then pixel(i % W, floor(i / W), cr, cg, cb)
            else pixel(i % W, floor(i / W), 0, 0, 0) end
            FROM[i] = ON[i]
          end
        else
          NACT = NACT + 1
          ACTIVE[NACT] = e
        end
      end
      if NPOOL < 16 and bn <= 256 then NPOOL = NPOOL + 1; POOL[NPOOL] = b end
    end
  end
  BKlast = now
end

local function draw_active(style)
  run_timetable(style)
  if DOTS then return end
  local n, keep = NACT, 0
  for j = 1, n do
    local i = ACTIVE[j]
    local p = (T - ST[i]) / FLIP
    local done = p >= 1
    local waiting = p <= 0                 -- the wave has not reached it: it shows what it showed
    if p < 0 then p = 0 elseif p > 1 then p = 1 end
    local e = ease(p)
    local from, to = FROM[i], ON[i]
    local c = COL[i]
    local cr, cg, cb = (c >> 16) & 255, (c >> 8) & 255, c & 255
    if waiting then
      -- nothing to draw yet
    elseif style == "line" then
      local amt = from + (to - from) * e
      if DOTS then
        local k = amt > 0.02 and amt or 0
        pixel(i % W, floor(i / W), floor(cr * k), floor(cg * k), floor(cb * k))
      else
        draw_seg(i, 0, 0, 0)
        if amt > 0.02 then
          -- grown from the centre along the segment's length
          if HORIZ[i] then
            local L = RW[i]
            local l = max(1, floor(L * amt + 0.5))
            rect(RX[i] + floor((L - l) / 2), RY[i], l, RH[i], cr, cg, cb, true)
          else
            local x, y, w, h = RX[i], RY[i], RW[i], RH[i]
            if RX2[i] then h = h + RH2[i] end
            local l = max(1, floor(h * amt + 0.5))
            local top = y + floor((h - l) / 2)
            for yy = top, top + l - 1 do
              local xx = (RX2[i] and yy >= RY2[i]) and RX2[i] or x
              rect(xx, yy, w, 1, cr, cg, cb, true)
            end
          end
        end
      end
    else
      -- the plate: the lit face narrows to its edge, a glint, the other face
      local lvl, glint
      local fr = from >= 0.5 and 1 or 0
      if fr == to then lvl, glint = to, 0
      else
        local shows = (to == 1) and (e > 0.5) or (to == 0 and e < 0.5)
        lvl = shows and abs(cos(pi * e)) or 0
        glint = max(0, 1 - abs(e - 0.5) * 6) * 0.35
      end
      local rr = min(255, floor(cr * lvl + 255 * glint))
      local gg = min(255, floor(cg * lvl + 255 * glint))
      local bb = min(255, floor(cb * lvl + 255 * glint))
      if DOTS then pixel(i % W, floor(i / W), rr, gg, bb)
      else draw_seg(i, rr, gg, bb) end
    end
    if done then
      FROM[i] = to
      INACT[i] = false
    else
      keep = keep + 1
      ACTIVE[keep] = i
    end
  end
  for j = keep + 1, n do ACTIVE[j] = nil end
  NACT = keep
end

-- ── the program ─────────────────────────────────────────────────────────────
local nextScene = 2
local hold = false                  -- a scene picked by hand stays
local titleUntil = -1
local function scene_name(sc)
  local s = (sc.mode == "dots" and "DOTS" or sc.size) .. " " .. sc.src:upper()
  return s
end
local function start_scene(k, title)
  scene = (k - 1) % #SHOW + 1
  sceneAt = T
  going = false
  build(SHOW[scene])
  life = nil
  lastSlot = -1
  if title then
    -- the name on the dark board for a second, in the classic 5x7
    local s = title
    local w = px.width(s, "5x7")
    px.text(floor((W - w) / 2), 28, s, 228, 255, 40, "5x7")
    titleUntil = T + 1.0
  end
end

-- the board goes out as a wave; when it is dark the chosen scene comes in
local function go_to(k)
  if going then nextScene = k; return end
  going = T
  nextScene = k
  for i = 0, (DOTS and G.n or G.n * 7) - 1 do
    if ON[i] == 1 then
      local x, y
      if DOTS then x, y = (i % W) / 8, floor(i / W) / 16
      else local d = floor(i / 7); x, y = d % G.cols, floor(d / G.cols) * 0.5 end
      retarget(i, 0, x * WAVE + y * WAVE + rnd() * 0.010)
    end
  end
end

-- The effect's button (px.button, firmware 2.7.3+), presses grouped over
-- 0.45 s - what worked with the remote on OCEANARIUM: the remote holds the
-- switch 250 ms after its last frame, so its clicks come ~0.4 s apart.
local BTN = { last = nil, n = 0, at = 0, title = false }
local MULTI = 0.45
local function buttons()
  local btn = rawget(px, "button")
  if not btn then return end
  local n = btn()
  if BTN.last and n ~= BTN.last then BTN.n, BTN.at = BTN.n + (n - BTN.last), T end
  BTN.last = n
  if BTN.n > 0 and T - BTN.at > MULTI then
    local k = BTN.n
    BTN.n = 0
    if k == 1 then hold = true; BTN.title = true; go_to(scene + 1)
    elseif k == 2 then hold = true; BTN.title = true; go_to(scene - 1)
    else hold = false; BTN.title = "AUTO"; go_to(scene + 1) end
  end
end

capture_glyphs()
start_scene(1)

function draw()
  local t = px.t() * PERIOD
  local dt
  if tprev then
    dt = t - tprev
    if dt < 0 then dt = dt + PERIOD end
    if dt > 0.5 then dt = 0.5 end
  else
    dt = 1 / FPS
  end
  tprev = t
  T = T + dt

  buttons()
  local sc = SHOW[scene]
  if not going and not hold and T - sceneAt > sc.secs and T > titleUntil then go_to(scene + 1) end
  if going then
    if T - going > FLIP + 0.5 then
      local title = BTN.title
      BTN.title = false
      local k = (nextScene - 1) % #SHOW + 1
      if title == true then title = scene_name(SHOW[k]) end
      start_scene(k, title or nil)
      sc = SHOW[scene]
    end
  elseif T < titleUntil then
    -- the name is showing; the board waits
  elseif titleUntil > 0 then
    titleUntil = -1
    clear(0, 0, 0)
  else
    sample_chunk(sc, T)
  end
  draw_active(sc.style)
end

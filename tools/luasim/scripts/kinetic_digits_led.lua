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
--
-- A change of scene flows (firmware 2.7.5+, px.mix): the new board starts at
-- once and flips in from dark while the old picture dissolves over it in two
-- seconds (half a second when picked by hand, so its name reads clearly). The new scene draws into its own canvas, kept in snapshot slot 3,
-- because a board redraws only what flips and needs its last frame intact;
-- the old picture waits in slot 2. On older firmware the board goes out as a
-- wave first, as before.

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
  { mode = "digits", size = "5x9", src = "cube", pal = "white", style = "plate", secs = 18 },
  { mode = "dots", src = "plasma", pal = "src", style = "plate", secs = 22 },
  { mode = "digits", size = "8x16", src = "text", pal = "flip", style = "line", ital = true, secs = 26 },
  { mode = "dots", src = "rings", pal = "rainbow", style = "plate", secs = 18 },
  { mode = "dots", src = "cube", pal = "src", style = "plate", secs = 25 },
  { mode = "dots", src = "ball", pal = "src", style = "plate", secs = 25 },
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
local DT = 1 / 15                   -- this frame's seconds
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
local dots_order                    -- defined with the dot board (below)
-- per segment: its sample column and row as integer keys (x/3 and 2y), its
-- distance from the plasma's centre, its cell in the cube's raster, and its
-- first life cell
local KX, KY, RQ, RAS, LIFEA = {}, {}, {}, {}, {}
local KXL, KYL, RQMAX = {}, {}, 0
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
    dots_order()
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
  G.roll = 0
  RQMAX = 0
  -- the distinct sample columns (x/3) and rows (2y) of this grid
  for k in pairs(KXL) do KXL[k] = nil end
  for k in pairs(KYL) do KYL[k] = nil end
  for dx = 0, cols - 1 do for _, o in ipairs({ 1, 2, 3 }) do KXL[#KXL + 1] = dx * 4 + o end end
  for dy = 0, rows - 1 do for _, o in ipairs({ 6, 9, 18, 27, 30 }) do KYL[#KYL + 1] = dy * 36 + o end end
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
      KX[i], KY[i] = floor(SXs[i] / 3 + 0.5), floor(SYs[i] * 2 + 0.5)
      local du, dv = SXs[i] / 12 - 6, SYs[i] / 12 - 3
      RQ[i] = floor(sqrt(du * du + dv * dv) * 20)
      if RQ[i] > RQMAX then RQMAX = RQ[i] end
      if HORIZ[i] then
        local row = (s == "a" and 0 or (s == "g" and 1 or 2))
        RAS[i] = -((dy * 3 + row) * cols + dx + 1)
        LIFEA[i] = (dy * 6 + row * 2) * (cols * 2) + dx * 2
      else
        local sx = (s == "b" or s == "c") and 1 or 0
        local sy = (s == "e" or s == "c") and 1 or 0
        RAS[i] = (dy * 2 + sy) * (cols * 2) + dx * 2 + sx
        LIFEA[i] = (dy * 6 + sy * 3) * (cols * 2) + dx * 2 + sx
      end
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
local LIFE_ROWS = 7                 -- rows of the next generation a frame: 2.5 generations a second
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
local EDGES = true
local DRAWN = 0                     -- flips the frame drew (see sample_chunk)
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
    -- A dot is one LED: its flip is two moments, the edge and the new face -
    -- or, when a pass changes more dots than a frame can draw twice, the new
    -- face alone (an edge one LED wide is not seen in a crowd of thousands).
    if EDGES then schedule(start + FLIP * 0.5, -(i + 1)) end
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
local BUDGET_DOTS = 45000           -- less what the frame spent drawing flips
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
  if commitK == 1 then commitAt = T; EDGES = not DOTS or CHN <= 1200 end
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
  if last >= CHN then CHN, commitK = 0, 1; EDGES = true; return true end
  commitK = last + 1
  return false
end

-- ── the dot board, continuously ─────────────────────────────────────────────
-- The owner (2026-09-29): the dot plasma changed in jerks - a whole board a
-- few times a second - where the seven-segment board flows. 8,192 LEDs
-- cannot all be worked out every frame in Lua, so every frame works out an
-- eighth of them, every eighth LED of every row with the start shifted row
-- at random row by row and moving on frame by frame - scattered over the
-- whole board, so no pattern shows - and flips
-- what changed at once: the board moves every frame, and each LED is looked
-- at about twice a second. An LED costs a few table lookups: the plasma's
-- three plane waves are tables by column, row and diagonal, its radial wave
-- a table by distance (from a fixed centre in this mode, so the distance of
-- each LED is a table too), and the threshold and the face a table by the
-- field's value - all but the distances made once a frame.
local SLICES = 8
local CHN_LAST = 0                  -- how many dots the last frame changed
local ORQ = {}                      -- each LED's distance from the plasma's centre, in 1/20 units
local slice = 0
local TA, TB, TC, TR = {}, {}, {}, {}
local DX2, DY2 = {}, {}
local LITQ, FACEQ = {}, {}
local QN = 200                      -- steps of the field's value
local ROWOFF = {}                   -- each row's own starting column, fixed
function dots_order()
  if ORQ[0] then return end
  for y = 0, H - 1 do ROWOFF[y] = floor(rnd() * SLICES) % SLICES end
  for i = 0, W * H - 1 do
    local du, dv = ((i % W) * 2 + 1) / 12 - 6, (floor(i / W) * 2 + 1) / 12 - 3
    ORQ[i] = floor(sqrt(du * du + dv * dv) * 20)
  end
end

-- ── the cube in flip dots ───────────────────────────────────────────────────
-- The owner (2026-09-29): a cube "переливающийся как плазма и точечный", whose
-- speed, direction of turning and size change, which travels the screen and
-- bounces off its edges by the laws of physics, all smoothly.
--
-- A body in two dimensions with a spin: it flies at a speed that drifts
-- between targets, turns about an axis and at a rate that drift too, and
-- breathes between sizes. At a wall the part of its velocity into the wall
-- is turned back (restitution 0.97), and friction at the contact point
-- trades the velocity along the wall for spin about the screen's axis and
-- back - the contact point's own velocity is v + w x r, and the impulse that
-- slows it is shared between the body's travel and its turning. A cube sent
-- along a wall comes off it spinning.
--
-- Its twelve edges are drawn straight into the dots, an LED thick, nearer
-- edges brighter; the colour of every dot is the plasma's field where it is,
-- and a quarter of the lit dots is painted again every frame so the colour
-- runs along the edges. Moving, nearly every dot of an edge changes every
-- frame, too many to flip one by one (a flip is a timetable entry and a
-- pixel): a new dot lights at once, a dot left behind glows at a third for
-- a frame before it goes dark - the trail of the board's own afterglow.
local dots_cube, dots_ball
do
local function rr(a, b) return a + (b - a) * rnd() end
local function lp(v, to, dt, tau)
  local k = dt / tau
  if k > 1 then k = 1 end
  return v + (to - v) * k
end
local function new_body(x, y, vx, vy)
  return { mark = {}, litn = {}, litp = {}, fade = {}, np = 0, nf = 0, frame = 0,
           x = x, y = y, vx = vx, vy = vy, R = 16, Rt = 16, sp = 16, spt = 16,
           ax = 0.3, ay = 0.6, az = 0, wx = 0.3, wy = 0.6, wz = 0.1, wtx = 0.3, wty = 0.6, wtz = 0.1,
           nextW = 0, nextR = 0, nextS = 0, V = {}, Z = {}, lf = {}, lc = {} }
end
local CUBE, BALL = new_body(64, 32, 14, 9), new_body(50, 30, -12, 11)
local function plasma_hue(x, y, i)
  local v = TA[x] + TB[y] + TC[x + y] + TR[ORQ[i]]
  return floor(v * 45 + T * 30) % 360
end
local CUBE_E = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
                 { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } }
-- the ball's wires: five meridians and three parallels, 16 points round each
local SPH = {}
do
  local N = 16
  for m = 0, 4 do
    local ph = m * pi / 5
    for k = 0, N do
      local th = k * 2 * pi / N
      SPH[#SPH + 1] = { sin(th) * cos(ph), cos(th), sin(th) * sin(ph), k > 0 }
    end
  end
  for _, yv in ipairs({ -0.62, 0, 0.62 }) do
    local rr0 = sqrt(1 - yv * yv)
    for k = 0, N do
      local th = k * 2 * pi / N
      SPH[#SPH + 1] = { rr0 * cos(th), yv, rr0 * sin(th), k > 0 }
    end
  end
end
-- the lasers: four emitters on the ball (a tetrahedron), turning with it
local EMIT = { { 0.577, 0.577, 0.577 }, { -0.577, -0.577, 0.577 }, { -0.577, 0.577, -0.577 }, { 0.577, -0.577, -0.577 } }

-- What drifts: the spin (axis, sense, rate), the size, the speed; then one
-- step of travel and turning.
local function drift(c, dt)
  if T >= c.nextW then
    local ux, uy, uz = rr(-1, 1), rr(-1, 1), rr(-1, 1)
    local m = sqrt(ux * ux + uy * uy + uz * uz) + 1e-3
    local rate = rr(0.25, 1.6) * (rnd() < 0.5 and -1 or 1)
    c.wtx, c.wty, c.wtz = ux / m * rate, uy / m * rate, uz / m * rate
    c.nextW = T + rr(5, 8)
  end
  if T >= c.nextR then c.Rt = rr(10, 24); c.nextR = T + rr(6, 10) end
  if T >= c.nextS then c.spt = rr(8, 26); c.nextS = T + rr(7, 11) end
  c.wx, c.wy, c.wz = lp(c.wx, c.wtx, dt, 4), lp(c.wy, c.wty, dt, 4), lp(c.wz, c.wtz, dt, 4)
  c.R = lp(c.R, c.Rt, dt, 3)
  c.sp = lp(c.sp, c.spt, dt, 3)
  local v = sqrt(c.vx * c.vx + c.vy * c.vy) + 1e-3
  local k = lp(v, c.sp, dt, 2) / v
  c.vx, c.vy = c.vx * k, c.vy * k
  c.ax, c.ay, c.az = c.ax + c.wx * dt, c.ay + c.wy * dt, c.az + c.wz * dt
  c.x, c.y = c.x + c.vx * dt, c.y + c.vy * dt
  c.cx, c.sx, c.cy, c.sy, c.cz, c.sz = cos(c.ax), sin(c.ax), cos(c.ay), sin(c.ay), cos(c.az), sin(c.az)
end
-- a point of the body, turned (x, then y, then z): screen offset and depth
local function turn(c, x, y, z)
  local y1, z1 = y * c.cx - z * c.sx, y * c.sx + z * c.cx
  local x2, z2 = x * c.cy + z1 * c.sy, -x * c.sy + z1 * c.cy
  return x2 * c.cz - y1 * c.sz, x2 * c.sz + y1 * c.cz, z2
end
-- The walls: back out, bounce, and friction at the contact point (its own
-- velocity is v + w x r) trading travel along the wall for spin. Returns
-- how far the body was moved back in.
local function walls(c, minX, maxX, minY, maxY, r)
  local e, f = 0.97, 0.35
  local px_, py_ = 0, 0
  if minX < 0 then
    px_ = -minX
    if c.vx < 0 then
      c.vx = -c.vx * e
      local u = c.vy - c.wz * r
      c.vy, c.wz = c.vy - f * u * 0.5, c.wz + f * u / r * 0.5
      c.wtz = c.wz
    end
  elseif maxX > W - 1 then
    px_ = (W - 1) - maxX
    if c.vx > 0 then
      c.vx = -c.vx * e
      local u = c.vy + c.wz * r
      c.vy, c.wz = c.vy - f * u * 0.5, c.wz - f * u / r * 0.5
      c.wtz = c.wz
    end
  end
  if minY < 0 then
    py_ = -minY
    if c.vy < 0 then
      c.vy = -c.vy * e
      local u = c.vx + c.wz * r
      c.vx, c.wz = c.vx - f * u * 0.5, c.wz - f * u / r * 0.5
      c.wtz = c.wz
    end
  elseif maxY > H - 1 then
    py_ = (H - 1) - maxY
    if c.vy > 0 then
      c.vy = -c.vy * e
      local u = c.vx - c.wz * r
      c.vx, c.wz = c.vx - f * u * 0.5, c.wz + f * u / r * 0.5
      c.wtz = c.wz
    end
  end
  c.x, c.y = c.x + px_, c.y + py_
  return px_, py_
end

local function plasma_tables(c, t, rings)
  c.frame = c.frame + 1
  if c.frame % 2 == 1 then
    for x = 0, W - 1 do TA[x] = sin((x * 2 + 1) / 12 * 0.9 + t) end
    for y = 0, H - 1 do TB[y] = sin((y * 2 + 1) / 12 * 1.1 - t * 0.7) end
    for sxy = 0, W + H - 2 do TC[sxy] = sin((sxy * 2 + 2) / 12 * 0.6 + t * 0.5) end
    if rings then for k = 0, 440 do TR[k] = sin(k / 20 * 1.2 - t * 1.3) end end
  end
end

-- One wire from (x0, y0) to (x1, y1) into the dots, brightness k: a dot new
-- this frame is lit at once, an old one painted again now and then.
local function wire(c, x0, y0, x1, y1, k, nn)
  local fr, MARKF, LITN = c.frame, c.mark, c.litn
  local dx, dy = x1 - x0, y1 - y0
  local steps = floor(max(abs(dx), abs(dy))) + 1
  for s = 0, steps do
    local xx = floor(x0 + dx * s / steps + 0.5)
    local yy = floor(y0 + dy * s / steps + 0.5)
    if xx >= 0 and xx < W and yy >= 0 and yy < H then
      local i = yy * W + xx
      local m = MARKF[i]
      if m ~= fr then
        local was = m == fr - 1
        MARKF[i] = fr
        nn = nn + 1
        LITN[nn] = i
        if not was or (s + fr) % 4 == 0 then
          local col = HFACE[plasma_hue(xx, yy, i)]
          pixel(xx, yy, min(255, floor(((col >> 16) & 255) * k)), min(255, floor(((col >> 8) & 255) * k)),
                min(255, floor((col & 255) * k)))
        end
      end
    end
  end
  return nn
end

-- The ball's wire: one plasma colour a segment (they are short), kept per
-- dot for the afterglow; stride 2 leaves the far side dotted.
local function wire2(c, x0, y0, x1, y1, k, nn, stride)
  local fr, MARKF, LITN, LF, LC = c.frame, c.mark, c.litn, c.lf, c.lc
  local dx, dy = x1 - x0, y1 - y0
  local steps = floor(max(abs(dx), abs(dy))) + 1
  local mx, my = floor((x0 + x1) * 0.5 + 0.5), floor((y0 + y1) * 0.5 + 0.5)
  if mx < 0 then mx = 0 elseif mx >= W then mx = W - 1 end
  if my < 0 then my = 0 elseif my >= H then my = H - 1 end
  local col = HFACE[floor((TA[mx] + TB[my] + TC[mx + my]) * 45 + T * 30) % 360]
  local r = min(255, floor(((col >> 16) & 255) * k))
  local g = min(255, floor(((col >> 8) & 255) * k))
  local b = min(255, floor((col & 255) * k))
  local ix, iy = dx / steps * stride, dy / steps * stride
  local fx, fy = x0 + 0.5, y0 + 0.5
  for s = 0, steps, stride do
    local xx, yy = fx // 1, fy // 1
    fx, fy = fx + ix, fy + iy
    if xx >= 0 and xx < W and yy >= 0 and yy < H then
      local i = yy * W + xx
      local m = MARKF[i]
      if m ~= fr then
        local was = m == fr - 1
        MARKF[i], LF[i], LC[i] = fr, fr, col
        nn = nn + 1
        LITN[nn] = i
        if not was or (s + fr) % 4 == 0 then pixel(xx, yy, r, g, b) end
      end
    end
  end
  return nn
end

-- A laser beam: its own colour, not the plasma; remembered with the frame
-- it was lit so the afterglow dims the beam's colour, not the field's.
local function laser_px(c, xx, yy, col, k, nn, repaint)
  if xx < 0 or xx >= W or yy < 0 or yy >= H then return nn end
  local i = yy * W + xx
  local fr, MARKF = c.frame, c.mark
  local m = MARKF[i]
  if m == fr then return nn end
  local was = m == fr - 1 and c.lf[i] == fr - 1
  MARKF[i] = fr
  c.lf[i], c.lc[i] = fr, col
  nn = nn + 1
  c.litn[nn] = i
  if not was or repaint then
    pixel(xx, yy, min(255, floor(((col >> 16) & 255) * k)), min(255, floor(((col >> 8) & 255) * k)),
          min(255, floor((col & 255) * k)))
  end
  return nn
end
local function beam(c, x0, y0, dx, dy, col, nn)
  local t = 1e9
  if dx > 0.01 then t = (W - 1 - x0) / dx elseif dx < -0.01 then t = -x0 / dx end
  if dy > 0.01 then t = min(t, (H - 1 - y0) / dy) elseif dy < -0.01 then t = min(t, -y0 / dy) end
  if t <= 0 or t > 400 then return nn end
  local steps = floor(t) + 1
  local fr, MARKF, LITN, LF, LC = c.frame, c.mark, c.litn, c.lf, c.lc
  local R, G, B = (col >> 16) & 255, (col >> 8) & 255, col & 255
  local band, pr, pg, pb = -1, 0, 0, 0
  local fx, fy = x0 + 0.5, y0 + 0.5
  for s = 0, steps do
    local xx, yy = fx // 1, fy // 1
    fx, fy = fx + dx, fy + dy
    if xx >= 0 and xx < W and yy >= 0 and yy < H then
      local i = yy * W + xx
      local m = MARKF[i]
      if m ~= fr then
        local was = m == fr - 1 and LF[i] == fr - 1
        MARKF[i], LF[i], LC[i] = fr, fr, col
        nn = nn + 1
        LITN[nn] = i
        if not was or (s + fr) % 4 == 0 then
          -- dimmer as it goes, in four steps along the beam
          local bd = (s * 4) // (steps + 1)
          if bd ~= band then
            band = bd
            local k = 1.0 - 0.14 * bd
            pr, pg, pb = floor(R * k), floor(G * k), floor(B * k)
          end
          pixel(xx, yy, pr, pg, pb)
        end
      end
    end
  end
  -- where it strikes the wall: a small flare, whiter
  local ex, ey = floor(x0 + dx * t + 0.5), floor(y0 + dy * t + 0.5)
  local wc = col | 0x606060
  nn = laser_px(c, ex, ey, wc, 1.2, nn, true)
  nn = laser_px(c, ex + 1, ey, col, 0.8, nn, true)
  nn = laser_px(c, ex - 1, ey, col, 0.8, nn, true)
  nn = laser_px(c, ex, ey + 1, col, 0.8, nn, true)
  nn = laser_px(c, ex, ey - 1, col, 0.8, nn, true)
  return nn
end

-- The afterglow: last frame's dots left behind glow at a third, then go.
local function afterglow(c, nn)
  local fr, MARKF, LITN, LITP, FADE = c.frame, c.mark, c.litn, c.litp, c.fade
  for j = 1, c.nf do
    local i = FADE[j]
    if MARKF[i] ~= fr then pixel(i % W, floor(i / W), 0, 0, 0) end
    FADE[j] = nil
  end
  local nf = 0
  for j = 1, c.np do
    local i = LITP[j]
    if MARKF[i] ~= fr then
      local col = c.lf[i] == fr - 1 and c.lc[i] or HFACE[plasma_hue(i % W, floor(i / W), i)]
      pixel(i % W, floor(i / W), ((col >> 16) & 255) // 3, ((col >> 8) & 255) // 3, (col & 255) // 3)
      nf = nf + 1
      FADE[nf] = i
    end
  end
  c.nf = nf
  c.litn, c.litp = LITP, LITN
  c.np = nn
end

local function depth_k(z)
  -- nearer is brighter: z runs about -1.7 (near) .. 1.7 (far)
  local k = 1.0 - 0.28 * z
  if k > 1.25 then return 1.25 elseif k < 0.35 then return 0.35 end
  return k
end

function dots_cube(sc, t)
  local c = CUBE
  plasma_tables(c, t * 0.5, true)
  drift(c, DT)
  local V, Z = c.V, c.Z
  local minX, maxX, minY, maxY = 1e9, -1e9, 1e9, -1e9
  for i = 0, 7 do
    local x3, y3, z2 = turn(c, (i & 1) > 0 and 1 or -1, (i & 2) > 0 and 1 or -1, (i & 4) > 0 and 1 or -1)
    local p = 3.2 / (3.2 + z2)
    local X, Y = c.x + x3 * c.R * 0.62 * p, c.y + y3 * c.R * 0.62 * p
    V[i * 2], V[i * 2 + 1], Z[i] = X, Y, z2
    minX, maxX, minY, maxY = min(minX, X), max(maxX, X), min(minY, Y), max(maxY, Y)
  end
  local r = max(1, (maxX - minX + maxY - minY) / 4)
  local sx, sy = walls(c, minX, maxX, minY, maxY, r)
  local nn = 0
  for e = 1, 12 do
    local a, b = CUBE_E[e][1], CUBE_E[e][2]
    nn = wire(c, V[a * 2] + sx, V[a * 2 + 1] + sy, V[b * 2] + sx, V[b * 2 + 1] + sy, depth_k(Z[a] + Z[b]), nn)
  end
  afterglow(c, nn)
end

-- The ball: a sphere of wires, turning, with the same flight and the same
-- walls - a circle against them, its radius the contact's lever.
function dots_ball(sc, t)
  local c = BALL
  plasma_tables(c, t * 0.5, false)
  drift(c, DT)
  local S = c.R * 0.75
  local sx, sy = walls(c, c.x - S, c.x + S, c.y - S, c.y + S, S)
  -- the turn as one matrix (x, then y, then z), once a frame
  local cx, sx_, cy, sy_, cz, sz = c.cx, c.sx, c.cy, c.sy, c.cz, c.sz
  local a11, a12, a13 = cy * cz, sx_ * sy_ * cz - cx * sz, cx * sy_ * cz + sx_ * sz
  local a21, a22, a23 = cy * sz, sx_ * sy_ * sz + cx * cz, cx * sy_ * sz - sx_ * cz
  local a31, a32, a33 = -sy_, sx_ * cy, cx * cy
  local X0, Y0 = c.x, c.y
  local nn = 0
  -- the lasers first, so the ball sits over them where they leave it
  local hue0 = floor(T * 40)
  for b = 1, 4 do
    local q = EMIT[b]
    local x3 = a11 * q[1] + a12 * q[2] + a13 * q[3]
    local y3 = a21 * q[1] + a22 * q[2] + a23 * q[3]
    local z3 = a31 * q[1] + a32 * q[2] + a33 * q[3]
    local l = sqrt(x3 * x3 + y3 * y3)
    if l > 0.25 then
      local dx, dy = x3 / l, y3 / l
      local col = HFACE[(hue0 + b * 90) % 360]
      local x0, y0
      if z3 < 0 then          -- facing us: from the emitter itself, with a spark
        x0, y0 = X0 + x3 * S, Y0 + y3 * S
        nn = laser_px(c, floor(x0 + 0.5), floor(y0 + 0.5), col | 0x808080, 1.3, nn, true)
      else                    -- behind: from the rim
        x0, y0 = X0 + dx * S, Y0 + dy * S
      end
      nn = beam(c, x0, y0, dx, dy, col, nn)
    end
  end
  local px0, py0, pz0
  for j = 1, #SPH do
    local q = SPH[j]
    local x, y, z = q[1], q[2], q[3]
    local z2 = a31 * x + a32 * y + a33 * z
    local p = S * 3.2 / (3.2 + z2 * 0.8)
    local X, Y = X0 + (a11 * x + a12 * y + a13 * z) * p, Y0 + (a21 * x + a22 * y + a23 * z) * p
    if q[4] then
      local k = 1.0 - 0.476 * (pz0 + z2)
      if k > 1.25 then k = 1.25 elseif k < 0.35 then k = 0.35 end
      nn = wire2(c, px0, py0, X, Y, k, nn, (pz0 + z2) > 0.3 and 2 or 1)
    end
    px0, py0, pz0 = X, Y, z2
  end
  afterglow(c, nn)
end
end

local function dots_continuous(sc, t)
  -- An LED is looked at every eighth frame, so the picture moves at half
  -- speed here: a contour then moves a pixel or two between looks, and the
  -- edge flows instead of fraying into the sampling pattern.
  t = t * 0.5
  local src = sc.src
  local pal = sc.pal
  local constFace = FACE[pal] and pack(FACE[pal][1], FACE[pal][2], FACE[pal][3])
  local lo, hi, qk
  if src == "plasma" then
    -- the field is -4..4
    for x = 0, W - 1 do TA[x] = sin((x * 2 + 1) / 12 * 0.9 + t) end
    for y = 0, H - 1 do TB[y] = sin((y * 2 + 1) / 12 * 1.1 - t * 0.7) end
    for sxy = 0, W + H - 2 do TC[sxy] = sin((sxy * 2 + 2) / 12 * 0.6 + t * 0.5) end
    for k = 0, 440 do TR[k] = sin(k / 20 * 1.2 - t * 1.3) end      -- distance in 1/20 units
    for y = 0, H - 1 do DY2[y] = 0 end
    lo, hi = -4, 4
  else
    -- rings: the distance, in work units, from the moving centre
    local cx, cy = 128 + sin(t * 0.4) * 46, 64 + cos(t * 0.3) * 23
    for x = 0, W - 1 do local dx = x * 2 + 1 - cx; DX2[x] = dx * dx end
    for y = 0, H - 1 do local dy = y * 2 + 1 - cy; DY2[y] = dy * dy end
    lo, hi = 0, 300
  end
  qk = QN / (hi - lo)
  for q = 0, QN do
    local v = lo + q / qk
    local h, band = shade(src, v, t)
    local lit1 = ((HUE_L[h] * band) > THR) ~= INV
    LITQ[q] = lit1 and 1 or 0
    FACEQ[q] = constFace or (pal == "src" and HFACE[h])
  end
  local sl = slice
  slice = (slice + 1) % SLICES
  local plasma = src == "plasma"
  local rainbow = pal == "rainbow"
  EDGES = CHN_LAST < 150
  local changed = 0
  for y = 0, H - 1 do
    local base = y * W
    local by = TB[y]
    local dy2 = DY2[y]
    for x = (ROWOFF[y] + sl) % SLICES, W - 1, SLICES do
      local i = base + x
      local v
      if plasma then v = TA[x] + by + TC[x + y] + TR[ORQ[i]]
      else v = sqrt(DX2[x] + dy2) end
      local q = floor((v - lo) * qk)
      if q < 0 then q = 0 elseif q > QN then q = QN end
      local want = LITQ[q]
      if want == 1 then
        if rainbow then COL[i] = pack(face(pal, 0, 0, 0, x, W, t)) else COL[i] = FACEQ[q] or COL[i] end
      end
      if ON[i] ~= want then
        changed = changed + 1
        retarget(i, want, x / 8 * WAVE + y / 16 * WAVE + rnd() * 0.010, T)
      end
    end
  end
  CHN_LAST = changed
  EDGES = true
end

-- ── the digit board, continuously ───────────────────────────────────────────
-- Every source flows (the owner, 2026-09-29: "все должны течь плавно"): each
-- frame works out as much of the board as a budget allows - all of it on the
-- bigger cells - and flips what changed at once, rather than a whole board
-- a few times a second. The plasma is tables made once a frame (its plane
-- waves by sample column and row, the diagonal one by the sum formula, the
-- radial one by each segment's distance from a fixed centre), the cube is
-- drawn straight into the V and H samples, and the clock and the text are
-- the seven-segment font itself.
local FTA, FSA, FCA, FTB, FSB, FCB = {}, {}, {}, {}, {}, {}
local RASV, RASH = {}, {}
local RAINB = {}
local DIGIT_BUDGET = 26000
-- per sample, with the loop, measured on the host (fxhost --exact)
local DCOST = { plasma = 40, rings = 38, text = 22, clock = 22, life = 55, cube = 40 }
local DAYS = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" }
local MDAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
local clockRows = { "", "", "" }
local function clock_lines(cols)
  local now = px.now()
  local y, yd = now.year or 2026, now.yday or 0
  local leap = (y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0
  local m, d = 1, yd + 1
  while true do
    local md = MDAYS[m] + ((m == 2 and leap) and 1 or 0)
    if d <= md or m == 12 then break end
    d = d - md
    m = m + 1
  end
  -- the day of the week (Sakamoto): 0 is Sunday
  local tt = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 }
  local yy = m < 3 and y - 1 or y
  local dow = (yy + yy // 4 - yy // 100 + yy // 400 + tt[m] + d) % 7
  clockRows[1] = string.format("%02d-%02d-%02d", d, m, y % 100)
  clockRows[2] = cols >= 8 and string.format("%02d-%02d-%02d", now.hour, now.min, now.sec)
                            or string.format("%02d%02d", now.hour, now.min)
  local day = DAYS[dow + 1]
  if #day > cols then day = day:sub(1, cols) end
  clockRows[3] = day
end
local function clock_bits(x, y, cols, rows)
  -- one row: the time; two: date and time; three: date, time, the day
  local line
  if rows == 1 then line = clockRows[2]
  elseif rows == 2 then line = clockRows[y + 1]
  else
    local top = floor((rows - 3) / 2)
    local k = y - top
    if k < 0 or k > 2 then return 0, 0, 0, 0 end
    line = clockRows[k + 1]
    if k ~= 1 then
      local s0 = floor((cols - #line) / 2)
      local j = x - s0 + 1
      if j < 1 or j > #line then return 0, 0, 0, 0 end
      return FONT[line:sub(j, j)] or 0, 111, 211, 255
    end
  end
  local s0 = floor((cols - #line) / 2)
  local j = x - s0 + 1
  if j < 1 or j > #line then return 0, 0, 0, 0 end
  return FONT[line:sub(j, j)] or 0, 255, 200, 120
end

local function cube_raster(cols, rows)
  local vW, hW = cols * 2, cols
  for k = 0, vW * rows * 2 - 1 do RASV[k] = 0 end
  for k = 0, hW * rows * 3 - 1 do RASH[k] = 0 end
  local lw2 = (max(3, 12 * 1.1) * 0.55) ^ 2
  for e = 1, 12 do
    local E = cubeEdges[e]
    local x0, y0, x1, y1 = E[1], E[2], E[3], E[4]
    local c = pack(floor(E[5]), floor(E[6]), floor(E[7]))
    local L = sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2)
    local steps = max(1, floor(L / 4.5))
    for k = 0, steps do
      local px_, py_ = x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps
      local vx0, vy0 = floor((px_ - 3) / 6), floor((py_ - 4.5) / 9)
      for vy = vy0, vy0 + 1 do
        if vy >= 0 and vy < rows * 2 then
          for vx = vx0, vx0 + 1 do
            if vx >= 0 and vx < vW then
              local dx, dy = vx * 6 + 3 - px_, vy * 9 + 4.5 - py_
              if dx * dx + dy * dy < lw2 then RASV[vy * vW + vx] = c end
            end
          end
        end
      end
      local hx0, hy0 = floor((px_ - 6) / 12), floor((py_ - 3) / 6)
      for hy = hy0, hy0 + 1 do
        if hy >= 0 and hy < rows * 3 then
          for hx = hx0, hx0 + 1 do
            if hx >= 0 and hx < hW then
              local dx, dy = hx * 12 + 6 - px_, hy * 6 + 3 - py_
              if dx * dx + dy * dy < lw2 then RASH[hy * hW + hx] = c end
            end
          end
        end
      end
    end
  end
end

local function digits_frame(sc, t)
  local src, pal = sc.src, sc.pal
  local cols, rows = G.cols, G.rows
  local n = G.n * 7
  local constFace = FACE[pal] and pack(FACE[pal][1], FACE[pal][2], FACE[pal][3])
  local srcFace = pal == "src" and not INV
  local lo, qk, cx, cy
  unit = 12
  -- what this frame's samples need, made once
  if src == "plasma" then
    for j = 1, #KXL do
      local k = KXL[j]
      local u = k * 3 / 12
      FTA[k], FSA[k], FCA[k] = sin(u * 0.9 + t), sin(u * 0.6), cos(u * 0.6)
    end
    for j = 1, #KYL do
      local k = KYL[j]
      local v = k / 2 / 12
      FTB[k], FSB[k], FCB[k] = sin(v * 1.1 - t * 0.7), sin(v * 0.6 + t * 0.5), cos(v * 0.6 + t * 0.5)
    end
    for q = 0, RQMAX do TR[q] = sin(q / 20 * 1.2 - t * 1.3) end
    lo, qk = -4, QN / 8
  elseif src == "rings" then
    local Wc, Hc = cols * 12, rows * 18
    cx, cy = Wc / 2 + sin(t * 0.4) * Wc * 0.18, Hc / 2 + cos(t * 0.3) * Hc * 0.18
    lo, qk = 0, QN / sqrt(Wc * Wc + Hc * Hc)
  end
  if lo then
    for q = 0, QN do
      local h, band = shade(src, lo + q / qk, t)
      LITQ[q] = (((HUE_L[h] * band) > THR) ~= INV) and 1 or 0
      FACEQ[q] = constFace or HFACE[h]
    end
  end
  if pal == "rainbow" then
    for c = 0, cols - 1 do local r, g, b = hue(c / cols * 300 + t * 30); RAINB[c] = pack(floor(r), floor(g), floor(b)) end
  end
  if src == "text" or src == "clock" then
    if src == "text" then text_clock(cols) else clock_lines(cols) end
    for d = 0, G.n - 1 do
      local dx, dy = d % cols, floor(d / cols)
      if src == "text" then TEXTB[d], TEXTR[d], TEXTG[d], TEXTB2[d] = text_bits(dx, dy, cols, rows, t)
      else TEXTB[d], TEXTR[d], TEXTG[d], TEXTB2[d] = clock_bits(dx, dy, cols, rows) end
    end
  elseif src == "life" then
    life_chunk(cols * 2, rows * 6)
  elseif src == "cube" then
    -- drawn every other frame: it turns less than a radian a second
    G.cubeTick = not G.cubeTick
    if G.cubeTick or not RASV[0] then
      cube_setup(cols * 12, rows * 18, t)
      cube_raster(cols, rows)
    end
  end
  -- as many segments as the budget buys, round and round the board
  local budget = DIGIT_BUDGET - DRAWN * 90
  if budget < 6000 then budget = 6000 end
  local K = min(n, floor(budget / (DCOST[src] or 40)))
  local i = G.roll
  local mid = floor((rows - 1) / 2)
  for _ = 1, K do
    local want, c
    if src == "plasma" then
      local kx, ky = KX[i], KY[i]
      local v = FTA[kx] + FTB[ky] + FSA[kx] * FCB[ky] + FCA[kx] * FSB[ky] + TR[RQ[i]]
      local q = floor((v - lo) * qk)
      if q < 0 then q = 0 elseif q > QN then q = QN end
      want, c = LITQ[q], FACEQ[q]
    elseif src == "rings" then
      local dx, dy = SXs[i] - cx, SYs[i] - cy
      local q = floor(sqrt(dx * dx + dy * dy) * qk)
      if q > QN then q = QN end
      want, c = LITQ[q], FACEQ[q]
    elseif src == "text" or src == "clock" then
      local d = floor(i / 7)
      if (TEXTB[d] & SEGBIT[i % 7]) > 0 then
        want = 1
        c = constFace or (srcFace and pack(face("src", TEXTR[d], TEXTG[d], TEXTB2[d], 0, 1, t)))
      else want = 0 end
    elseif src == "life" then
      local r, g, b, l
      if life then
        local a, Wl = LIFEA[i], lifeW
        local n1, n2, n3, n4
        if HORIZ[i] then n1, n2, n3, n4 = life[a], life[a + 1], life[a + Wl], life[a + Wl + 1]
        else n1, n2, n3, n4 = life[a], life[a + Wl], life[a + 2 * Wl], 0 end
        local alive = (n1 > 0 and 1 or 0) + (n2 > 0 and 1 or 0) + (n3 > 0 and 1 or 0) + (n4 > 0 and 1 or 0)
        local of = HORIZ[i] and 4 or 3
        -- a third of the cells under it alive lights a segment: by the board's
        -- own threshold (43%) Life came out as sparse sparks
        want = (alive / of > 0.3) and 1 or 0
        if want == 1 then
          local age = max(n1, n2, n3, n4)
          c = constFace or HFACE[floor(lifeHue + age * 6) % 360]
        end
      else want = 0 end
    else -- cube
      local r = RAS[i]
      local v = r >= 0 and RASV[r] or RASH[-r - 1]
      if v > 0 then want, c = 1, constFace or v else want = 0 end
    end
    if want == 1 then
      if pal == "rainbow" then c = RAINB[floor(i / 7) % cols] end
      COL[i] = c or COL[i]
    end
    if ON[i] ~= want then
      local d = floor(i / 7)
      local dx, dy = d % cols, floor(d / cols)
      local delay
      if src == "text" and dy == mid then
        delay = (cols - 1 - dx) * WAVE * 0.5          -- the marquee's wave runs its own way
      else
        delay = dx * WAVE + dy * WAVE * 0.5
      end
      retarget(i, want, delay + rnd() * 0.010, T)
    end
    i = i + 1
    if i >= n then i = 0 end
  end
  G.roll = i
end

-- What the frame already spent drawing flips comes off the update's budget:
-- measured on the panel (2026-09-29), a dot board changing thousands of
-- dots ran at 8.6 fps, the cost being the pixel calls more than the Lua. With
-- this the board's refresh slows down while a big change is flipping, and
-- the frame rate holds.
local function sample_chunk(sc, t)
  local src = sc.src
  local isField = FIELD[src] ~= nil
  local pal = sc.pal
  local constFace = FACE[pal] and pack(FACE[pal][1], FACE[pal][2], FACE[pal][3])
  if src == "life" and not DOTS then life_chunk(G.cols * 4, G.rows * 6) end
  local spent = 0
  local BUDGET = (DOTS and BUDGET_DOTS or BUDGET) - DRAWN * (DOTS and 70 or 90)
  if BUDGET < 6000 then BUDGET = 6000 end
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

local EVN = 0                       -- events handled this frame
local function run_timetable(style)
  local now = floor(T * RATE)
  EVN = 0
  for k = BKlast + 1, now do
    local b = BK[k]
    if b then
      local bn = BKN[k]
      EVN = EVN + bn
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
  if DOTS then DRAWN = EVN; return end
  DRAWN = NACT
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
local BTN = { last = nil, n = 0, at = 0, title = false }
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

-- The flow from one scene into the next (px.mix, 2.7.5+): the picture on
-- screen is put aside in slot 2, the new scene starts at once, and for XF
-- seconds each frame it draws on its own canvas (slot 3) with the old
-- picture mixed over it, less and less.
-- (one table: the main chunk is at Lua's 200 locals)
-- A change by hand flows in half a second: the scene's name has to read
-- clearly in its one second on the dark board.
local FL = { mix = rawget(px, "mix") ~= nil, secs = 2.0, auto = 2.0, hand = 0.5, at = nil, old = 2, new = 3 }

-- the board goes out as a wave; when it is dark the chosen scene comes in.
-- With px.mix it does not wait: the scene flows in over the old picture.
local function go_to(k)
  if FL.mix then
    local title = BTN.title
    BTN.title = false
    local kk = (k - 1) % #SHOW + 1
    if title == true then title = scene_name(SHOW[kk]) end
    px.save(FL.old)
    start_scene(kk, title or nil)
    px.save(FL.new)
    FL.at = T
    FL.secs = title and FL.hand or FL.auto
    return
  end
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
  DT = dt

  buttons()
  local sc = SHOW[scene]
  if not going and not hold and T - sceneAt > sc.secs and T > titleUntil then go_to(scene + 1) end
  local flowing = FL.at ~= nil
  if flowing then px.restore(FL.new) end
  FL.frame(SHOW[scene])
  if flowing then
    local u = (T - FL.at) / FL.secs
    if u >= 1 then
      FL.at = nil
      px.forget(FL.old)
      px.forget(FL.new)
    else
      px.save(FL.new)
      px.mix(FL.old, 1 - ease(u))
    end
  end
end

FL.frame = function(sc)
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
    draw_active(sc.style)
    if DOTS and (sc.src == "plasma" or sc.src == "rings") then dots_continuous(sc, T)
    elseif DOTS and sc.src == "cube" then dots_cube(sc, T)
    elseif DOTS and sc.src == "ball" then dots_ball(sc, T)
    elseif not DOTS then digits_frame(sc, T)
    else sample_chunk(sc, T) end
    return
  end
  draw_active(sc.style)
end

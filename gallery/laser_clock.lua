-- @upload-only
-- ============================================================
-- LASER CLOCK - a laser on the pavement writes the time on the wall
-- ============================================================
-- @name.en Laser clock
-- @name.ru Лазерные часы
-- @about.en Night, a brick wall. A small laser projector on the ground writes text on the wall with its
-- @about.en beam, then keeps retracing it, so the lines shimmer. Every 5 s by the clock the next line: the time, the
-- @about.en day of the week, the date, the outside temperature (when the panel has weather), КАННЫ.
-- @about.ru Ночь, кирпичная стена. Маленький лазерный проектор на земле пишет лучом текст на стене, а
-- @about.ru потом всё время обводит его заново, и линии мерцают. Каждые 5 с по часам следующая надпись: время,
-- @about.ru день недели, дата, температура на улице (если у панели есть погода), КАННЫ.
-- @control.en knob press: Changes the laser colour: each line its own colour, then green, red, blue, violet, then a colour for every character, and round again.
-- @control.ru knob press: Меняет цвет лазера: у каждой надписи свой цвет, затем зелёный, красный, синий, фиолетовый, затем свой цвет у каждого знака, и снова по кругу.
-- @function.en Time, day of the week, date
-- @function.en Outside temperature from the panel's weather
-- @function.en The colon blinks
-- @function.ru Время, день недели, дата
-- @function.ru Температура на улице из погоды панели
-- @function.ru Двоеточие мигает
--
-- Night, the back wall of a house. A small laser projector stands on the
-- ground in the middle, and its beam - a thin line in the haze - carries one
-- bright dot over the bricks. The dot writes the time: stroke by stroke, with
-- the beam going dark as it jumps between strokes, sparks where it burns, and
-- what it has drawn glowing on behind it. Once the time is written the
-- projector does what a real one does: it runs over the whole figure again and
-- again, fast, so the lines stay lit and shimmer as the scan comes round, and
-- the beam in the air becomes a flickering fan.
--
-- Every 5 s the wall says the next thing (the last text dissolving into the
-- bricks as the laser writes the next, with px.mix on firmware 2.7.5): the time, the day of the week, the
-- date, the temperature outside and КАННЫ - each in its own colour of the
-- RGB laser, the temperature's going from ice blue to red with the reading.
-- The temperature is skipped while the panel has no weather (off in the
-- portal, or not fetched yet). While the time is up, a digit that changes
-- goes dark and is written again. The colon blinks. The button changes the
-- laser: each screen its colour, then green, red, blue, violet, then a colour
-- a character.
--
-- The 5 s are the wall clock's, not the time since the page opened: px.t()
-- over PERIOD 100 (twenty turns, a whole number of rounds of 4 screens or 5),
-- aligned to the epoch by the firmware, says whose turn it is, so two panels
-- write the same line at the same moment - unless only one of them has the
-- weather. The laser's colour is the presses since the page opened
-- (px.button() from its value then), so quick presses are never lost.
--
-- The digits are true arcs and strokes; the letters come from the panel's
-- own 5x7 system font, Latin and Cyrillic, read at load and joined into
-- strokes. The weather is the weather clock's (px.weather).
--
-- The glow: lines are drawn in px.mode("add"), the wall around the writing
-- softened with px.blur, and the hot core drawn again over it. The wall and
-- the projector are drawn once and px.restore()d each frame.
--
-- Needs firmware 2.7.4 or later for the glow (px.mode, px.blur), the
-- temperature (px.weather); without them it is flatter and skips the
-- temperature.
-- ============================================================
PERIOD = 100.0                        -- twenty 5 s turns of the wall clock
FPS = 15

local W, H = px.size()
local floor, sqrt, exp, sin, min, max, abs = math.floor, math.sqrt, math.exp, math.sin, math.min, math.max, math.abs
local HAS_MODE, HAS_BLUR = rawget(px, "mode") ~= nil, rawget(px, "blur") ~= nil
-- px.mix (firmware 2.7.5): the text going out dissolves on the wall while the
-- laser writes the next; without it, it goes out in a quarter of a second
local HAS_MIX = rawget(px, "mix") ~= nil
local XF_S = 1.5

-- xorshift32: the simulator and the panel must draw the same picture
local seed = 0x2545F491
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7fffffff) / 2147483648.0
end

-- ---------------------------------------------------------------- the scene
local AX, AY = 64, 57                 -- the projector's aperture
local GROUND = 59                     -- the pavement from here down

local function scene()
  px.clear(0, 0, 0)
  -- bricks: 8 x 4 with a mortar line, every other course offset by half
  for y = 0, GROUND - 1 do
    local course = y // 4
    local off = (course % 2) * 4
    for x = 0, W - 1 do
      local mortar = (y % 4 == 3) or ((x + off) % 8 == 7)
      if mortar then
        px.pixel(x, y, 7, 6, 7)
      else
        local b = ((x + off) // 8) * 7 + course * 13
        local v = 13 + ((b * 0x9E3779B1) >> 7) % 6
        px.pixel(x, y, v + 3, v - 2, v - 3)
      end
    end
  end
  -- the pavement, a kerb line
  for y = GROUND, H - 1 do
    for x = 0, W - 1 do
      local v = 6 + ((x * 7 + y * 13) % 5 == 0 and 3 or 0)
      px.pixel(x, y, v, v, v + 1)
    end
  end
  px.line(0, GROUND, W - 1, GROUND, 16, 15, 16)
  -- the projector: a small dark box on the ground, a lens on top
  px.rect(58, 58, 13, 5, 22, 22, 26, true)
  px.rect(58, 58, 13, 5, 40, 40, 46)
  px.rect(62, 56, 5, 2, 30, 30, 34, true)
  px.pixel(59, 62, 10, 10, 10); px.pixel(69, 62, 10, 10, 10)
  px.pixel(60, 60, 60, 12, 12)         -- a power light
end
scene()
px.save()

-- ---------------------------------------------------------------- the font
-- Digits drawn the way a neon sign or a laser show letters them: true arcs
-- and straight strokes on a 5 x 9 box, every bowl an ellipse, a slight lean
-- to the right. Each glyph is a list of strokes; each stroke a polyline the
-- laser follows without lifting.
local SLANT = 0.2                     -- x moves this much right per unit up

local function arc(p, cx, cy, rx, ry, a0, a1)
  -- angles in degrees, counter-clockwise from the right, y downwards on screen
  local n = max(2, floor(abs(a1 - a0) / 11))
  for i = 0, n do
    local a = math.rad(a0 + (a1 - a0) * i / n)
    p[#p + 1] = cx + rx * math.cos(a); p[#p + 1] = cy - ry * math.sin(a)
  end
  return p
end
local function pt(p, x, y) p[#p + 1] = x; p[#p + 1] = y; return p end

local GLYPH = {
  ["0"] = { arc({}, 2.5, 4.5, 2.5, 4.5, 90, 450) },
  ["1"] = { pt(pt(pt({}, 1.0, 2.0), 3.2, 0), 3.2, 9) },
  ["2"] = { pt(pt(arc({}, 2.5, 2.5, 2.5, 2.5, 165, -38), 0, 9), 5, 9) },
  ["3"] = { arc(arc({}, 2.5, 2.2, 2.3, 2.2, 160, -90), 2.5, 6.55, 2.5, 2.45, 90, -160) },
  ["4"] = { pt(pt(pt(pt({}, 3.7, 9), 3.7, 0), 0, 6.3), 5.1, 6.3) },
  ["5"] = { arc(pt(pt(pt({}, 4.7, 0), 0.9, 0), 0.4, 4.3), 2.5, 6.4, 2.5, 2.6, 150, -155) },
  ["6"] = { arc(arc({}, 2.5, 4.5, 2.5, 4.5, 48, 205), 2.5, 6.4, 2.5, 2.6, 205, 565) },
  ["7"] = { pt(pt(pt({}, 0, 0), 5, 0), 1.7, 9) },
  ["8"] = { arc(arc({}, 2.5, 2.2, 2.15, 2.2, -90, 270), 2.5, 6.6, 2.5, 2.4, 90, -270) },
  ["9"] = { arc(arc({}, 2.5, 2.6, 2.5, 2.6, 25, -335), 2.5, 4.5, 2.5, 4.5, 25, -130) },
  [":"] = { pt(pt({}, 0.9, 2.7), 0.9, 2.95), pt(pt({}, 0.4, 6.4), 0.4, 6.65) },
  ["."] = { pt(pt({}, 0.4, 8.7), 0.4, 9) },
  ["\194\176"] = { arc({}, 1.2, 1.2, 1.1, 1.1, 90, 450) },   -- the degree sign
  ["+"] = { pt(pt({}, 0, 4.5), 4, 4.5), pt(pt({}, 2, 2.5), 2, 6.5) },
  ["-"] = { pt(pt({}, 0, 4.5), 3.4, 4.5) },
}
local ADV = { [":"] = 2.3, ["."] = 2.1, ["\194\176"] = 3.1, ["+"] = 5.3, ["-"] = 4.4, [" "] = 3.2 }
local DIGIT_ADV, LETTER_ADV = 6.6, 7.1

-- Letters come from the panel's own 5x7 system font, Latin and Cyrillic: each
-- is drawn once at load, read back a dot at a time, and its dots joined into
-- strokes - across and down always, diagonally where no square corner is
-- there to take the turn - then straight runs merged, so the laser draws
-- lines, not dots. Rows 0..6 of the font map onto the digits' 0..9.
local LETTERS = "\208\144\208\145\208\146\208\147\208\148\208\149\208\129\208\150\208\151\208\152\208\153\208\154\208\155\208\156\208\157\208\158\208\159\208\160\208\161\208\162\208\163\208\164\208\165\208\166\208\167\208\168\208\169\208\170\208\171\208\172\208\173\208\174\208\175ABCDEFGHIJKLMNOPQRSTUVWXYZ'"
local UTF8 = "[%z\1-\127\194-\244][\128-\191]*"

local function capture_letter(ch)
  px.clear(0, 0, 0)
  px.text(0, 0, ch, 255, 255, 255, "5x7")
  local on = {}
  for r = 0, 7 do
    for c = 0, 4 do
      local v = px.get(c, r)
      if v > 0 then on[r * 8 + c] = true end
    end
  end
  local function lit(c, r) return c >= 0 and c <= 4 and r >= 0 and r <= 7 and on[r * 8 + c] end
  -- the edges
  local adj, used = {}, {}
  local function edge(a, b)
    adj[a] = adj[a] or {}; adj[b] = adj[b] or {}
    adj[a][#adj[a] + 1] = b; adj[b][#adj[b] + 1] = a
  end
  for r = 0, 7 do
    for c = 0, 4 do
      if lit(c, r) then
        local k = r * 8 + c
        adj[k] = adj[k] or {}
        if lit(c + 1, r) then edge(k, k + 1) end
        if lit(c, r + 1) then edge(k, k + 8) end
        if lit(c + 1, r + 1) and not lit(c + 1, r) and not lit(c, r + 1) then edge(k, k + 9) end
        if lit(c - 1, r + 1) and not lit(c - 1, r) and not lit(c, r + 1) then edge(k, k + 7) end
      end
    end
  end
  local function key(a, b) return a < b and a * 64 + b or b * 64 + a end
  local function free(a)
    local n = 0
    for _, b in ipairs(adj[a]) do if not used[key(a, b)] then n = n + 1 end end
    return n
  end
  local strokes = {}
  local function walk(start)
    local p = { (start % 8) * 1.25, (start // 8) * 1.5 }
    local cur, dir = start, nil
    while true do
      local nxt
      for _, b in ipairs(adj[cur]) do
        if not used[key(cur, b)] then
          if not nxt or (b - cur) == dir then nxt = b end
        end
      end
      if not nxt then break end
      used[key(cur, nxt)] = true
      local d = nxt - cur
      if d == dir and #p >= 4 then p[#p - 1], p[#p] = nil, nil end   -- straight on: one line
      p[#p + 1] = (nxt % 8) * 1.25; p[#p + 1] = (nxt // 8) * 1.5
      cur, dir = nxt, d
    end
    if #p == 2 then p[3], p[4] = p[1], p[2] + 0.3 end               -- a lone dot
    strokes[#strokes + 1] = p
  end
  -- ends first (odd degree), then whatever is left, in reading order
  for pass = 1, 2 do
    for k = 0, 63 do
      if adj[k] then
        while free(k) > 0 and (pass == 2 or free(k) % 2 == 1) do walk(k) end
        if pass == 2 and #adj[k] == 0 and not used[k * 64 + k] then used[k * 64 + k] = true; walk(k) end
      end
    end
  end
  return strokes
end
for ch in LETTERS:gmatch(UTF8) do GLYPH[ch] = capture_letter(ch); ADV[ch] = LETTER_ADV end
-- Ы: in 5x7 its bowl touches its bar, and the dots join into a knot. Drawn by
-- hand on the same grid instead (column * 1.25, row * 1.5).
do
  local function g(t) local p = {} for i = 1, #t, 2 do p[#p + 1] = t[i] * 1.25; p[#p + 1] = t[i + 1] * 1.5 end return p end
  GLYPH["Ы"] = { g{ 0, 0, 0, 6, 1.8, 6, 2.6, 5.2, 2.6, 3.8, 1.8, 3, 0, 3 }, g{ 4.2, 0, 4.2, 6 } }
end
for d = 0, 9 do ADV[tostring(d)] = DIGIT_ADV end

-- A text laid out on the wall: its characters, where each starts (in units),
-- and the scale and origin that fit it, centred, no taller than the clock.
local function layout(text)
  local chars, xs, w = {}, {}, 0
  for ch in text:gmatch(UTF8) do
    if GLYPH[ch] or ch == " " then
      chars[#chars + 1] = ch; xs[#xs + 1] = w
      w = w + (ADV[ch] or LETTER_ADV)
    end
  end
  w = w - 0.9                                           -- no gap after the last
  local span = w + 9 * SLANT
  local sc = min(4.2, 120 / span)
  return chars, xs, sc, (W - span * sc) / 2, 25 - 4.5 * sc
end

-- ---------------------------------------------------------------- segments
-- Every character's strokes cut into segments of at most 3.5 px. Each keeps
-- when the dot last passed it; its brightness falls from then.
local SX0, SY0, SX1, SY1, LIT, NEWSTROKE, SLOTOF, LEN = {}, {}, {}, {}, {}, {}, {}, {}
local IX0, IY0, IX1, IY1, K = {}, {}, {}, {}, {}   -- the ends rounded once; this frame's brightness
local NSEG = 0
local GHOST = {}                      -- segments of what went: fading, never lit again
local shown, shownKey = {}, ""
local BLINK = {}                      -- the places that are the clock's colon

-- A new text on the wall. With the same screen and the same length only the
-- characters that differ go dark and are rewritten; otherwise all of it.
local function build(text, screenKey, noGhosts)
  local chars, xs, sc, ox, oy = layout(text)
  local same = screenKey == shownKey and #chars == #shown
  local changed = {}
  for s = 1, #chars do changed[s] = not same or chars[s] ~= shown[s] end
  local oldLit = {}
  for i = 1, NSEG do
    local s = SLOTOF[i]
    if not same or changed[s] then
      if not noGhosts then GHOST[#GHOST + 1] = { SX0[i], SY0[i], SX1[i], SY1[i], LIT[i], s } end
    else
      oldLit[#oldLit + 1] = LIT[i]
    end
  end
  local n, k = 0, 0
  for s = 1, #chars do
    BLINK[s] = chars[s] == ":"
    for _, stroke in ipairs(GLYPH[chars[s]] or {}) do
      local p = stroke
      for i = 1, #p // 2 - 1 do
        local x0 = ox + (xs[s] + p[2 * i - 1] + (9 - p[2 * i]) * SLANT) * sc
        local y0 = oy + p[2 * i] * sc
        local x1 = ox + (xs[s] + p[2 * i + 1] + (9 - p[2 * i + 2]) * SLANT) * sc
        local y1 = oy + p[2 * i + 2] * sc
        local len = sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2)
        local parts = max(1, floor(len / 3.5 + 0.999))
        for j = 0, parts - 1 do
          n = n + 1
          SX0[n] = x0 + (x1 - x0) * j / parts; SY0[n] = y0 + (y1 - y0) * j / parts
          SX1[n] = x0 + (x1 - x0) * (j + 1) / parts; SY1[n] = y0 + (y1 - y0) * (j + 1) / parts
          LEN[n] = max(0.2, len / parts)
          IX0[n], IY0[n] = floor(SX0[n] + 0.5), floor(SY0[n] + 0.5)
          IX1[n], IY1[n] = floor(SX1[n] + 0.5), floor(SY1[n] + 0.5)
          NEWSTROKE[n] = (i == 1 and j == 0)
          SLOTOF[n] = s
          if changed[s] then LIT[n] = -1e9 else k = k + 1; LIT[n] = oldLit[k] or -1e9 end
        end
      end
    end
  end
  for i = n + 1, NSEG do
    SX0[i], SY0[i], SX1[i], SY1[i], LIT[i], NEWSTROKE[i], SLOTOF[i], LEN[i] = nil
    IX0[i], IY0[i], IX1[i], IY1[i], K[i] = nil
  end
  NSEG = n
  for s = #chars + 1, #shown do shown[s], BLINK[s] = nil, nil end
  for s = 1, #chars do shown[s] = chars[s] end
  shownKey = screenKey
  local pathLen = 0
  for i = 1, n do if changed[SLOTOF[i]] then pathLen = pathLen + LEN[i] end end
  return changed, pathLen
end

-- ---------------------------------------------------------------- colours
-- An RGB laser: each screen its own colour; the temperature's goes from ice
-- blue to red with the reading. The button cycles: each screen its colour,
-- then green, red, blue, violet, then a colour a character.
local LASERS = {
  { 40, 255, 70 },   -- 520 nm green, the one every show uses
  { 255, 30, 40 },   -- red
  { 60, 90, 255 },   -- blue
  { 190, 60, 255 },  -- violet
}
local RAINBOW = { { 255, 40, 60 }, { 255, 170, 30 }, { 255, 255, 120 }, { 40, 255, 90 }, { 60, 200, 255 }, { 170, 80, 255 } }
local SCREEN_COL = { time = { 40, 255, 70 }, day = { 40, 210, 255 }, date = { 255, 185, 40 },
                     cannes = { 255, 120, 40 }, temp = { 255, 255, 255 } }
local colour = 0                      -- 0: each screen its own
local screenCol = SCREEN_COL.time
local function temp_colour(t)
  local stops = { { -10, 90, 110, 255 }, { 5, 40, 220, 255 }, { 15, 60, 255, 120 }, { 24, 255, 210, 40 }, { 32, 255, 50, 30 } }
  if t <= stops[1][1] then return { stops[1][2], stops[1][3], stops[1][4] } end
  for i = 2, #stops do
    local a, b = stops[i - 1], stops[i]
    if t <= b[1] then
      local f = (t - a[1]) / (b[1] - a[1])
      return { floor(a[2] + (b[2] - a[2]) * f), floor(a[3] + (b[3] - a[3]) * f), floor(a[4] + (b[4] - a[4]) * f) }
    end
  end
  return { stops[#stops][2], stops[#stops][3], stops[#stops][4] }
end
local function col_of(slot)
  if colour == 0 then return screenCol end
  if colour > #LASERS then return RAINBOW[(slot - 1) % #RAINBOW + 1] end
  return LASERS[colour]
end

-- ---------------------------------------------------------------- the laser
local T, tprev = 0, nil
local mode = "write"                  -- "write" the changed places, then "scan" everything
local writing = {}                    -- which places are being written
local seg, u = 1, 0                   -- where the dot is: segment and how far along it
local dotX, dotY, beamOn = AX, AY - 20, false
local FAN = {}                        -- dot positions this frame, for the beam fan
local SPARK = {}                      -- embers where it writes
local WRITE_V, SCAN_V, JUMP_V = 46, 1500, 4000
local WV = WRITE_V                    -- this writing's speed: a whole new text is written in ~1.4 s
local TAU_SCAN, TAU_WRITE = 0.9, 12   -- how long a line glows once passed (writing: the rest waits)
local clickBase = rawget(px, "button") and px.button() or 0   -- the presses before the page opened
local lastClicks = clickBase
local lastMin = -1
local SCREEN_S = 5
local xfAt = nil                      -- when the last screen's text began to dissolve
-- Each frame's wall and text go into one snapshot slot (a 24 KB copy); at a
-- change of screen that slot becomes the one that fades, and the other
-- takes the new frames. No text is drawn twice for it.
local LINES_SLOT, FADE_SLOT = 3, 2
local screen, screenTurn = 0, nil     -- the screen on the wall, and the clock's turn it was written in

local function first_seg()
  for i = 1, NSEG do if mode == "scan" or writing[SLOTOF[i]] then return i end end
  return nil
end
local function next_seg(i)
  for k = 1, NSEG do
    local j = (i + k - 1) % NSEG + 1
    if mode == "scan" or writing[SLOTOF[j]] then
      if mode == "write" and j <= i then return nil end   -- written through: done
      return j
    end
  end
  return nil
end

local function start_writing(changed, pathLen)
  WV = max(WRITE_V, (pathLen or 0) / 1.4)
  writing = changed
  mode = "write"
  seg = first_seg()
  u = 0
  if not seg then mode = "scan"; seg = first_seg() or 1 end
end

-- Move the dot `dist` px along the program. A segment that begins a stroke is
-- reached by a jump with the beam off; every segment passed is lit now.
local function travel(dist, colonOn)
  local fanStep = mode == "scan" and 14 or 1e9
  local sinceFan = 0
  local jumpBudget = dist * JUMP_V / (mode == "scan" and SCAN_V or WV)
  local guard = 0
  while dist > 0 and seg do
    guard = guard + 1
    if guard > 4000 then break end
    if u == 0 and NEWSTROKE[seg] then
      -- a jump to the stroke's start: fast, dark
      local dx, dy = SX0[seg] - dotX, SY0[seg] - dotY
      local d = sqrt(dx * dx + dy * dy)
      jumpBudget = jumpBudget - d
      dotX, dotY = SX0[seg], SY0[seg]
      if jumpBudget < 0 then beamOn = false; return end
    end
    local s = SLOTOF[seg]
    local dark = BLINK[s] and not colonOn
    local rest = LEN[seg] * (1 - u)
    if dist >= rest then
      dist = dist - rest
      if not dark then LIT[seg] = T end
      dotX, dotY = SX1[seg], SY1[seg]
      u = 0
      local nx = next_seg(seg)
      if not nx then
        mode = "scan"; writing = {}
        seg = first_seg()
      else
        seg = nx
      end
    else
      u = u + dist / LEN[seg]
      dotX = SX0[seg] + (SX1[seg] - SX0[seg]) * u
      dotY = SY0[seg] + (SY1[seg] - SY0[seg]) * u
      if not dark then LIT[seg] = T end
      dist = 0
    end
    beamOn = not dark
    sinceFan = sinceFan + rest
    if sinceFan >= fanStep and beamOn and #FAN < 10 then
      FAN[#FAN + 1] = dotX; FAN[#FAN + 1] = dotY; sinceFan = 0
    end
  end
end

-- ---------------------------------------------------------------- drawing
local HOT = { 0, 0, 0 }
local function line_col(x0, y0, x1, y1, c, k)
  if k < 0.02 then return end
  px.line(floor(x0 + 0.5), floor(y0 + 0.5), floor(x1 + 0.5), floor(y1 + 0.5),
          min(255, floor(c[1] * k)), min(255, floor(c[2] * k)), min(255, floor(c[3] * k)))
end

local function draw_lines(core)
  local tau = mode == "write" and TAU_WRITE or TAU_SCAN
  local cut = core and 0.25 or 0.02                -- the core only where it is bright
  local rainbow = colour > #LASERS
  local c = col_of(1)
  -- the core: the colour run toward white, as a hot line looks
  local cr, cg, cb = c[1], c[2], c[3]
  if core then cr, cg, cb = cr * 0.8 + 70, cg * 0.8 + 70, cb * 0.8 + 70 end
  for i = 1, NSEG do
    local k
    if core then
      k = K[i]
    else
      local age = T - LIT[i]
      k = age < 6 and exp(-age / tau) or 0
      if age < 0.12 then k = k * 1.35 end        -- just passed: the scan's bright edge
      K[i] = k
    end
    if k >= cut then
      if rainbow then
        c = col_of(SLOTOF[i]); cr, cg, cb = c[1], c[2], c[3]
        if core then cr, cg, cb = cr * 0.8 + 70, cg * 0.8 + 70, cb * 0.8 + 70 end
      end
      if core and k > 1 then k = 1 end
      px.line(IX0[i], IY0[i], IX1[i], IY1[i], min(255, floor(cr * k)), min(255, floor(cg * k)), min(255, floor(cb * k)))
    end
  end
  for gi = #GHOST, 1, -1 do
    local g = GHOST[gi]
    local age = T - g[5]
    if age > 1.5 then table.remove(GHOST, gi)
    elseif not core then
      line_col(g[1], g[2], g[3], g[4], col_of(g[6]), exp(-age / 0.25) * 0.8)
    end
  end
end

-- ---------------------------------------------------------------- screens
-- Each 5 s the wall says the next thing: the time, the day, the date, the
-- temperature outside, Cannes. The temperature is skipped while the panel
-- has no weather.
local DAYS = { "\208\146\208\158\208\161\208\154\208\160\208\149\208\161\208\149\208\157\208\172\208\149", "\208\159\208\158\208\157\208\149\208\148\208\149\208\155\208\172\208\157\208\152\208\154", "\208\146\208\162\208\158\208\160\208\157\208\152\208\154",
               "\208\161\208\160\208\149\208\148\208\144", "\208\167\208\149\208\162\208\146\208\149\208\160\208\147", "\208\159\208\175\208\162\208\157\208\152\208\166\208\144", "\208\161\208\163\208\145\208\145\208\158\208\162\208\144" }
local function ymd(now)
  local y, d = now.year, now.yday + 1
  local leap = (y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0
  local ML = { 31, leap and 29 or 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
  local m = 1
  while m < 12 and d > ML[m] do d = d - ML[m]; m = m + 1 end
  return y, m, d
end
local function weekday(y, m, d)            -- Sakamoto: 0 is Sunday
  local t = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 }
  if m < 3 then y = y - 1 end
  return (y + y // 4 - y // 100 + y // 400 + t[m] + d) % 7
end
local SCREENS = {
  { key = "time", make = function(now) return string.format("%02d:%02d", now.hour, now.min), SCREEN_COL.time end },
  { key = "day", make = function(now)
      local y, m, d = ymd(now)
      return DAYS[weekday(y, m, d) + 1], SCREEN_COL.day
    end },
  { key = "date", make = function(now)
      local y, m, d = ymd(now)
      return string.format("%02d.%02d.%04d", d, m, y), SCREEN_COL.date
    end },
  { key = "temp", make = function()
      local w = rawget(px, "weather") and px.weather()
      if not w then return nil end
      local t = w.fahrenheit and w.temp * 9 / 5 + 32 or w.temp
      local n = floor(t + 0.5)
      return (n > 0 and "+" or "") .. n .. "\194\176" .. (w.fahrenheit and "F" or "C"), temp_colour(w.temp)
    end },
  { key = "cannes", make = function() return "КАННЫ", SCREEN_COL.cannes end },
}
local TEMP = 4                              -- SCREENS' temperature, the one that can be missing
local WEATHER = rawget(px, "weather")       -- px.weather (2.7.4), or nil

function draw()
  local t = px.t() * PERIOD
  local dt = 1 / FPS
  if tprev then
    dt = t - tprev
    if dt < 0 then dt = dt + PERIOD end
    if dt > 0.5 then dt = 0.5 end
  end
  tprev = t
  T = T + dt

  local c = rawget(px, "button") and px.button() or 0
  if c ~= lastClicks then lastClicks = c; colour = (c - clickBase) % (#LASERS + 2) end

  local now = px.now()
  local turn = t // SCREEN_S                -- a float's floor, no call: this runs every frame
  if turn ~= screenTurn then
    screenTurn = turn
    -- the clock's turn among the screens that have something to say: all of
    -- them, the temperature only while there is weather
    -- (a float with a whole value: it indexes SCREENS as the integer does)
    local pick
    if WEATHER and WEATHER() then pick = turn % #SCREENS + 1
    else pick = turn % (#SCREENS - 1) + 1; if pick >= TEMP then pick = pick + 1 end end
    -- (the weather cannot go between the two asks: the panel copies it in before a frame)
    local text, col = SCREENS[pick].make(now)
    if pick ~= screen then
      screen = pick
      local flow = HAS_MIX and NSEG > 0
      if flow then
        -- last frame's text, kept as it glowed, dissolves under the new one
        LINES_SLOT, FADE_SLOT = FADE_SLOT, LINES_SLOT
        xfAt = T
      end
      screenCol = col
      lastMin = now.min
      start_writing(build(text, SCREENS[screen].key, flow))
    end
  elseif SCREENS[screen].key == "time" and now.min ~= lastMin then
    lastMin = now.min
    start_writing(build(SCREENS[screen].make(now), "time"))
  end
  local colonOn = SCREENS[screen].key ~= "time" or now.sec % 2 == 0

  -- the dot's travel this frame
  for i = #FAN, 1, -1 do FAN[i] = nil end
  travel((mode == "scan" and SCAN_V or WV) * dt, colonOn)

  -- embers where it writes
  if mode == "write" and beamOn then
    for _ = 1, 2 do
      SPARK[#SPARK + 1] = { dotX, dotY, (rnd() - 0.5) * 18, (rnd() - 0.8) * 14, T }
    end
  end

  px.restore()
  if xfAt then
    -- the last screen's text, fading into the bricks as the new one is written
    local u = (T - xfAt) / XF_S
    if u >= 1 then xfAt = nil
    else px.mix(FADE_SLOT, 1 - u * u * (3 - 2 * u)) end
  end

  -- the lines on the wall, their halo, and the hot core over it. Drawn over,
  -- not added: the segments share their ends, and added they would bead.
  draw_lines(false)
  if HAS_BLUR then px.blur(0.55, 0, 0, W, GROUND) end
  draw_lines(true)
  if HAS_MIX then px.save(LINES_SLOT) end
  if HAS_MODE then px.mode("add") end

  -- the beam in the haze, from the aperture to the dot (a fan while scanning)
  local lc = col_of(SLOTOF[seg or 1] or 1)
  if beamOn then
    local k = mode == "write" and 0.30 or 0.12
    k = k * (0.85 + 0.3 * rnd())
    line_col(AX, AY, dotX, dotY, lc, k)
    for i = 1, #FAN, 2 do line_col(AX, AY, FAN[i], FAN[i + 1], lc, 0.07) end
    -- motes in the beam: dust catching the light
    for _ = 1, 3 do
      local f = rnd()
      local mx, my = AX + (dotX - AX) * f, AY + (dotY - AY) * f
      px.pixel(floor(mx + 0.5), floor(my + 0.5), floor(lc[1] * 0.5), floor(lc[2] * 0.5), floor(lc[3] * 0.5))
    end
    -- the dot, and the light it throws on the bricks around it
    local x, y = floor(dotX + 0.5), floor(dotY + 0.5)
    px.pixel(x, y, 255, 255, 255)
    if mode == "write" then
      px.glow(dotX, dotY, 5, lc[1], lc[2], lc[3], 0.5)
    end
  end

  -- sparks: a short fall and out
  for i = #SPARK, 1, -1 do
    local s = SPARK[i]
    local age = T - s[5]
    if age > 0.5 then table.remove(SPARK, i)
    else
      local x = s[1] + s[3] * age
      local y = s[2] + s[4] * age + 30 * age * age
      local k = 1 - age / 0.5
      px.pixel(floor(x + 0.5), floor(y + 0.5), floor(255 * k), floor((150 + lc[2] * 0.4) * k), floor(60 * k))
    end
  end

  -- the lens glows with whatever it is sending
  local lk = beamOn and 1 or 0.3
  px.pixel(AX, AY, floor(lc[1] * lk), floor(lc[2] * lk), floor(lc[3] * lk))
  px.pixel(AX - 1, AY, floor(lc[1] * lk * 0.3), floor(lc[2] * lk * 0.3), floor(lc[3] * lk * 0.3))
  px.pixel(AX + 1, AY, floor(lc[1] * lk * 0.3), floor(lc[2] * lk * 0.3), floor(lc[3] * lk * 0.3))
  if HAS_MODE then px.mode("set") end
end

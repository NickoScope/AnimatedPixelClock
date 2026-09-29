-- @upload-only
-- ============================================================
-- LASER CLOCK - a laser on the pavement writes the time on the wall
-- ============================================================
-- Night, the back wall of a house. A small laser projector stands on the
-- ground in the middle, and its beam - a thin line in the haze - carries one
-- bright dot over the bricks. The dot writes the time: stroke by stroke, with
-- the beam going dark as it jumps between strokes, sparks where it burns, and
-- what it has drawn glowing on behind it. Once the time is written the
-- projector does what a real one does: it runs over the whole figure again and
-- again, fast, so the lines stay lit and shimmer as the scan comes round, and
-- the beam in the air becomes a flickering fan.
--
-- Each minute the digits that change go dark, and the laser writes the new
-- ones while the rest dim a little - it can only be in one place at a time.
-- The colon is two dots it touches every other second. The button changes
-- the laser: green, red, blue, violet, and one colour a digit.
--
-- The glow: lines are drawn in px.mode("add"), the wall around the writing
-- softened with px.blur, and the hot core drawn again over it. The wall and
-- the projector are drawn once and px.restore()d each frame.
--
-- Needs firmware 2.7.4 or later (px.mode, px.blur); nothing is drawn wrong
-- without them, only flatter.
-- ============================================================
PERIOD = 600.0
FPS = 15

local W, H = px.size()
local floor, sqrt, exp, sin, min, max, abs = math.floor, math.sqrt, math.exp, math.sin, math.min, math.max, math.abs
local HAS_MODE, HAS_BLUR = rawget(px, "mode") ~= nil, rawget(px, "blur") ~= nil

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
  local n = max(2, floor(abs(a1 - a0) / 7))
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
}

-- where each of the five places sits, and the size of a unit
local SLOT_X = { 4, 31, 60, 69, 96 }
local TOP, UX, UY = 6, 4.1, 4.2

-- ---------------------------------------------------------------- segments
-- Every place's strokes cut into segments of at most 2 px. Each keeps when
-- the dot last passed it; its brightness falls from then.
local SX0, SY0, SX1, SY1, LIT, NEWSTROKE, SLOTOF, LEN = {}, {}, {}, {}, {}, {}, {}, {}
local NSEG = 0
local GHOST = {}                      -- segments of digits that changed: fading, never lit again
local shown = { "", "", "", "", "" }

local function build(chars)
  local oldLit = {}
  for i = 1, NSEG do
    local s = SLOTOF[i]
    if chars[s] ~= shown[s] then
      GHOST[#GHOST + 1] = { SX0[i], SY0[i], SX1[i], SY1[i], LIT[i], s }
    else
      oldLit[#oldLit + 1] = LIT[i]
    end
  end
  local n, k = 0, 0
  for s = 1, 5 do
    local g = GLYPH[chars[s]]
    for _, stroke in ipairs(g) do
      local p = stroke
      for i = 1, #p // 2 - 1 do
        local x0 = SLOT_X[s] + (p[2 * i - 1] + (9 - p[2 * i]) * SLANT) * UX
        local y0 = TOP + p[2 * i] * UY
        local x1 = SLOT_X[s] + (p[2 * i + 1] + (9 - p[2 * i + 2]) * SLANT) * UX
        local y1 = TOP + p[2 * i + 2] * UY
        local len = sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2)
        local parts = max(1, floor(len / 2 + 0.999))
        for j = 0, parts - 1 do
          n = n + 1
          SX0[n] = x0 + (x1 - x0) * j / parts; SY0[n] = y0 + (y1 - y0) * j / parts
          SX1[n] = x0 + (x1 - x0) * (j + 1) / parts; SY1[n] = y0 + (y1 - y0) * (j + 1) / parts
          LEN[n] = len / parts
          NEWSTROKE[n] = (i == 1 and j == 0)
          SLOTOF[n] = s
          if chars[s] ~= shown[s] then LIT[n] = -1e9 else k = k + 1; LIT[n] = oldLit[k] or -1e9 end
        end
      end
    end
  end
  for i = n + 1, NSEG do SX0[i], SY0[i], SX1[i], SY1[i], LIT[i], NEWSTROKE[i], SLOTOF[i], LEN[i] = nil end
  NSEG = n
  local changed = {}
  for s = 1, 5 do changed[s] = chars[s] ~= shown[s]; shown[s] = chars[s] end
  return changed
end

-- ---------------------------------------------------------------- colours
local LASERS = {
  { 40, 255, 70 },   -- 520 nm green, the one every show uses
  { 255, 30, 40 },   -- red
  { 60, 90, 255 },   -- blue
  { 190, 60, 255 },  -- violet
}
local RAINBOW = { { 255, 40, 60 }, { 255, 170, 30 }, { 255, 255, 255 }, { 40, 255, 90 }, { 60, 140, 255 } }
local colour = 1
local function col_of(slot)
  if colour > #LASERS then return RAINBOW[slot] end
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
local TAU_SCAN, TAU_WRITE = 0.9, 12   -- how long a line glows once passed (writing: the rest waits)
local lastClicks = rawget(px, "button") and px.button() or 0
local lastMin = -1

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

local function clock_chars(now)
  local h, m = now.hour, now.min
  return { tostring(h // 10), tostring(h % 10), ":", tostring(m // 10), tostring(m % 10) }
end

local function start_writing(changed)
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
  local jumpBudget = dist * JUMP_V / (mode == "scan" and SCAN_V or WRITE_V)
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
    local dark = (s == 3 and not colonOn)
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
  for i = 1, NSEG do
    local age = T - LIT[i]
    if age < 6 then
      local k = exp(-age / tau)
      if age < 0.12 then k = k * 1.35 end        -- just passed: the scan's bright edge
      local c = col_of(SLOTOF[i])
      if core then
        -- the core: the colour run toward white, as a hot line looks
        local r, g, b = c[1] * 0.8 + 70, c[2] * 0.8 + 70, c[3] * 0.8 + 70
        HOT[1], HOT[2], HOT[3] = r, g, b
        line_col(SX0[i], SY0[i], SX1[i], SY1[i], HOT, min(1, k))
      else
        line_col(SX0[i], SY0[i], SX1[i], SY1[i], c, k)
      end
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
  if c ~= lastClicks then lastClicks = c; colour = colour % (#LASERS + 1) + 1 end

  local now = px.now()
  if now.min ~= lastMin then
    lastMin = now.min
    start_writing(build(clock_chars(now)))
  end
  local colonOn = now.sec % 2 == 0

  -- the dot's travel this frame
  for i = #FAN, 1, -1 do FAN[i] = nil end
  travel((mode == "scan" and SCAN_V or WRITE_V) * dt, colonOn)

  -- embers where it writes
  if mode == "write" and beamOn then
    for _ = 1, 2 do
      SPARK[#SPARK + 1] = { dotX, dotY, (rnd() - 0.5) * 18, (rnd() - 0.8) * 14, T }
    end
  end

  px.restore()

  -- the lines on the wall, their halo, and the hot core over it. Drawn over,
  -- not added: the segments share their ends, and added they would bead.
  draw_lines(false)
  if HAS_BLUR then px.blur(0.55, 0, 0, W, GROUND) end
  draw_lines(true)
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

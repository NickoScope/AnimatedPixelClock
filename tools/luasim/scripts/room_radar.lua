-- room_radar.lua - who is in the room, as a 24 GHz radar sees it.
--
-- The HLK-LD2450 reports up to three people, each as X and Y in millimetres
-- plus a speed, ten times a second, inside a fan 6 m deep and 60 degrees either
-- side. This draws exactly that fan up from the middle of the bottom edge, so
-- the picture is the sensor's own view of the room: 10 px to the metre.
--
-- Two sources of people. `presence`, bound by src/presence when the panel has
-- a real radar behind it, is preferred; fake_targets() invents them when it is
-- absent or has no live feed, so the screen works with no Home Assistant and
-- luasim keeps running this file unchanged. The scripted trails read the
-- script's paths; the live ones read the binding's own ring of past positions.
--
-- The fan is about 3 900 pixels and every one is repainted each frame, so the
-- per-pixel work is kept to a table read and one px.pixel call. The grid is
-- one flat array of integers, one per pixel, not a table per pixel: the
-- firmware's Lua heap has to share PSRAM with the HUB75 driver.

local W, H = px.size()
local floor, min, max, abs = math.floor, math.min, math.max, math.abs
local sin, cos, sqrt, atan, exp, pi =
  math.sin, math.cos, math.sqrt, math.atan, math.exp, math.pi
local deg, rad = math.deg, math.rad
local fmt = string.format

-- src/presence binds `presence` before this chunk runs - see "the panel's own
-- radar" below. It is read here as well, for the scale set in the portal.
local RAD = rawget(_G, "presence")

local STORY  = 24.0            -- seconds the scripted scene lasts; px.t() spans it
PERIOD = STORY                 -- a global: the panel's px.t() spans PERIOD seconds (default 60)
local CX, CY = 63.5, 63.0      -- the sensor
-- How deep the fan reaches. 6 m is the sensor's own range and what the scripted
-- story was drawn for; the panel offers 2, 4 or 6 m and defaults to 4, because
-- at 6 m a living room leaves the top two thirds of the fan empty (docs/16 in
-- the knowledge base). The fan stays 60 px deep, so the rest follows from it.
local RANGE, HALF = RAD and RAD.scale() or 6.0, 60
local PPM    = 60 / RANGE      -- pixels per metre
local RINGS  = floor(RANGE)    -- a ring every metre

-- The beam is decoration: the LD2450 does not sweep. It is what makes a fan of
-- dots read as a radar, so it stays, and it stays dim.
local SWEEP_T = 4.0            -- there and back; STORY holds a whole number of them
local SWEEP_A = 58             -- degrees it reaches either side
local PERSIST = 0.45           -- seconds for the phosphor to fade to 1/e

local function in_fan(x, y)
  return y > 0 and x * x + y * y <= RANGE * RANGE and abs(deg(atan(x, y))) <= HALF
end

local function to_px(x, y) return CX + x * PPM, CY - y * PPM end

-- ---------------------------------------------------------------- the grid
-- For every pixel: how lit its ring or spoke is (v, 1..255), and at which
-- phase of the swing the beam crosses it (0..255). Both are fixed, so both are
-- computed once.
--
-- A pixel's colour depends on nothing else but the frame, so the pixels are
-- kept sorted by phase, in one array: each phase's beam is worked out once a
-- frame, not once a pixel. Within a phase the plain pixels (v = 1, most of the
-- fan) come first and share one colour; the ring and spoke pixels follow.
-- A plain pixel is the integer y * 128 + x; a ring or spoke pixel packs its v
-- above that, (v * 64 + y) * 128 + x.
local NPH = 0                  -- phases that occur
local C1, C2 = {}, {}          -- per phase: where in the swing the beam crosses it, out and back
local FAN = {}                 -- every pixel of the fan
local PLAIN_END, LINES_END = {}, {}  -- per phase: its last plain pixel, and its last pixel
local LR, LG, LB = {}, {}, {}  -- per v: the colour before the beam and the fade
do
  local SPOKES = {-60, -30, 0, 30, 60}
  local SC, SS, EDGE = {}, {}, {}
  for i, a in ipairs(SPOKES) do
    local ar = rad(a)
    SC[i], SS[i], EDGE[i] = cos(ar), sin(ar), abs(a) == HALF
  end
  local n = 0
  for y = 0, H - 1 do
    for x = 0, W - 1 do
      local mx, my = (x + 0.5 - CX) / PPM, (CY - (y + 0.5)) / PPM
      local r = sqrt(mx * mx + my * my)
      local ang = (my > 0 and r <= RANGE + 0.08) and deg(atan(mx, my))
      if ang and abs(ang) <= HALF + 0.8 then
        local lit = 0
        for k = 1, RINGS do                            -- a ring every metre
          local d = abs(r - k) * PPM
          if d < 1 then lit = max(lit, (1 - d) * ((k % 3 == 0) and 1.0 or 0.5)) end
        end
        for i = 1, 5 do                                -- dotted spokes, solid edges
          local d = abs(mx * SC[i] - my * SS[i]) * PPM
          local edge = EDGE[i]
          if d < 1 and (edge or floor(r * PPM) % 3 == 0) then
            lit = max(lit, (1 - d) * (edge and 0.75 or 0.4))
          end
        end
        local v  = 1 + floor(lit * 254)
        local ph = floor((max(-1, min(1, ang / SWEEP_A)) + 1) / 2 * 255 + 0.5)
        n = n + 1
        FAN[n] = ((ph * 256 + v) * H + y) * W + x     -- phase on top, to sort by
      end
    end
  end
  -- Sorted in C, then the phase is taken off each pixel again.
  table.sort(FAN)
  local last = -1
  for i = 1, n do
    local k = FAN[i]
    local p, v = k >> 21, (k >> 13) & 255
    if p ~= last then
      last, NPH = p, NPH + 1
      local q = p / 255 * 2 - 1
      C1[NPH], C2[NPH] = (q + 1) / 4, (3 - q) / 4
      PLAIN_END[NPH] = i - 1
    end
    if v == 1 then
      PLAIN_END[NPH], FAN[i] = i, k & 8191
    else
      FAN[i] = k & 2097151
    end
    LINES_END[NPH] = i
  end
  for v = 1, 255 do
    local lit = (v - 1) / 254
    LR[v], LG[v], LB[v] = 5 + lit * 30, 14 + lit * 140, 11 + lit * 105
  end
end

-- The beam swings at constant speed, out and back, so it crosses a pixel at two
-- points of each swing; time since the later one is the phosphor's age. A sine
-- swing was tried first: it dwells at the edges and burns a bright band there.
--
-- Colours are floored but not clamped to 255: px.pixel clamps every channel
-- itself, and only green can pass 255 (a bright ring right under the beam).
local function background(S, g)
  local pixel, FAN, PLAIN_END, LINES_END = px.pixel, FAN, PLAIN_END, LINES_END
  local C1, C2, LR, LG, LB = C1, C2, LR, LG, LB
  local r1, g1, b1 = LR[1], LG[1], LB[1]
  local u = (S / SWEEP_T) % 1
  local first = 1
  for i = 1, NPH do
    local since, back = (u - C1[i]) % 1, (u - C2[i]) % 1
    if back < since then since = back end    -- min(since, back), as math.min picks
    since = since * SWEEP_T
    local beam = 0.5 * exp(-since / PERSIST) + 0.7 * exp(-since / 0.05)
    local br, bgr, bb = beam * 12, beam * 95, beam * 70
    local cr, cg, cb = (r1 + br) * g // 1, (g1 + bgr) * g // 1, (b1 + bb) * g // 1
    local last = PLAIN_END[i]
    for j = first, last do
      local k = FAN[j]
      pixel(k & 127, k >> 7, cr, cg, cb)
    end
    first = LINES_END[i]
    for j = last + 1, first do
      local k = FAN[j]
      local v = k >> 13
      pixel(k & 127, (k >> 7) & 63,
            (LR[v] + br) * g // 1, (LG[v] + bgr) * g // 1, (LB[v] + bb) * g // 1)
    end
    first = first + 1
  end
end

-- ---------------------------------------------------------------- the people
-- Waypoints {seconds, x metres, y metres}. Absent outside the first and last.
-- A repeated position is someone sitting down.
local PEOPLE = {
  { col = { 40, 235, 255}, seed = 0.0, path = {
      {1.0, 3.8, 2.2}, {4.5, 1.2, 3.0}, {7.5, -0.9, 2.4}, {9.5, -0.9, 1.4},
      {16.5, -0.9, 1.4}, {19.5, -2.4, 1.8}, {21.5, -4.2, 2.3} } },
  { col = {255, 175, 40}, seed = 1.7, path = {
      {5.5, 0.6, 6.6}, {9.0, 1.6, 4.2}, {14.5, 1.6, 4.2}, {17.5, 0.3, 2.6},
      {20.0, 1.4, 1.6}, {22.5, 3.6, 1.9} } },
  { col = {245, 80, 210}, seed = 3.1, path = {
      {11.5, -5.4, 3.6}, {18.5, 5.2, 3.0} } },
}

local function pos(person, s)
  local p = person.path
  s = s % STORY
  if s < p[1][1] or s > p[#p][1] then return nil end
  for i = 1, #p - 1 do
    local a, b = p[i], p[i + 1]
    if s <= b[1] then
      local f = (s - a[1]) / (b[1] - a[1])
      -- The radar's own wobble: centimetres, slow enough not to read as walking.
      local j = person.seed
      return a[2] + (b[2] - a[2]) * f + 0.05 * sin(s * 1.3 + j),
             a[3] + (b[3] - a[3]) * f + 0.05 * sin(s * 0.9 + 2 * j)
    end
  end
end

-- What the LD2450 hands over: up to three {x, y, speed} in mm and cm/s, in
-- slots. On the panel this is the one function to replace.
local function fake_targets(s)
  local out = {}
  for i, person in ipairs(PEOPLE) do
    local x, y = pos(person, s)
    if x and in_fan(x, y) then
      local xp, yp = pos(person, s - 0.1)
      local speed = xp and sqrt((x - xp) ^ 2 + (y - yp) ^ 2) * 1000 or 0
      out[#out + 1] = {x = floor(x * 1000), y = floor(y * 1000), speed = floor(speed), slot = i}
    end
  end
  return out
end

-- #fake_targets(s) without building the list, which is all the empty-room fade
-- needs. On the panel it becomes the count in the radar's recent reports.
local function count_targets(s)
  local n = 0
  for _, person in ipairs(PEOPLE) do
    local x, y = pos(person, s)
    if x and in_fan(x, y) then n = n + 1 end
  end
  return n
end

-- ---------------------------------------------------- the panel's own radar
-- src/presence binds `presence` before this chunk runs: what the MTR-1 last
-- reported, smoothed and aged. It is absent under luasim and fxhost, and in
-- any build without PRESENCE_ENABLED, and it answers DEMO while no feed has
-- arrived - in each of those the scripted story above runs instead.
--
--   state()      0 the story, 1 live targets, 2 the feed stopped and says so
--   target(i)    x mm, y mm, speed cm/s, or nil for an empty slot. The binding
--                has taken abs(mm/s) / 10 already, which is what > 12 tests.
--   trail(i, k)  where slot i was k steps of 0.1 s ago, out of its ring
--   count(k)     targets it held k steps ago, for the empty-room fade
--   scale()      metres the fan covers; read at the top of this file
local DEMO, LOST = 0, 2

-- Refilled in place, never rebuilt: draw() asks the binding for targets 19
-- times a frame, and this heap is PSRAM shared with the HUB75 driver. A live
-- frame allocates nothing, where fake_targets() builds a table per person.
local SLOT = {{x = 0, y = 0, speed = 0, slot = 1},
              {x = 0, y = 0, speed = 0, slot = 2},
              {x = 0, y = 0, speed = 0, slot = 3}}
local LIVE = {}

local function live_targets()
  local n = 0
  for i = 1, 3 do
    local x, y, v = RAD.target(i)
    if x and in_fan(x / 1000, y / 1000) then
      local t = SLOT[i]
      t.x, t.y, t.speed = x, y, v
      n = n + 1
      LIVE[n] = t
    end
  end
  for i = #LIVE, n + 1, -1 do LIVE[i] = nil end
  return LIVE
end

-- ---------------------------------------------------------------- the words
local DIG = {
  ["0"]={".####.","######","##..##","##..##","##..##","##..##","##..##","##..##","##..##","######",".####."},
  ["1"]={"..##..",".###..","####..","..##..","..##..","..##..","..##..","..##..","..##..","######","######"},
  ["2"]={".####.","######","##..##","....##","...###","..###.",".###..","###...","##....","######","######"},
  ["3"]={".####.","######","##..##","....##","..###.","..####","....##","....##","##..##","######",".####."},
}

local function big(x, y, s, r, g, b)
  local gl = DIG[s]
  if not gl then return end
  for row = 1, #gl do
    for col = 1, 6 do
      if gl[row]:sub(col, col) == "#" then px.pixel(x + col - 1, y + row - 1, r, g, b) end
    end
  end
end

local function dim(c, k) return floor(c[1] * k), floor(c[2] * k), floor(c[3] * k) end

local TEAL = {0, 150, 115}

-- The range labels on the fan's right edge never move, so they are placed once.
local RANGE_LABELS = {}
for _, k in ipairs(RANGE >= 6 and {2, 4, 6} or RANGE >= 4 and {2, 4} or {1, 2}) do
  local label = k .. "M"
  RANGE_LABELS[#RANGE_LABELS + 1] = {
    label, min(W - px.width(label), floor(CX + k * PPM * sin(rad(HALF)) + 4)),
    floor(CY - k * PPM * cos(rad(HALF)) - 5) }
end

local VISIBLE = {}             -- per trail step, for the target being drawn

-- ---------------------------------------------------------------- draw
function draw()
  local S = px.t() * STORY
  local state = RAD and RAD.state() or DEMO
  local live = state ~= DEMO
  local targets = live and live_targets() or fake_targets(S)

  -- An empty room fades down after a moment: the sleep idea, drawn.
  local seen = #targets > 0 and 1 or 0
  for k = 1, 4 do
    local was = live and RAD.count(k * 3) or count_targets(S - k * 0.3)
    if was > 0 then seen = seen + 1 end
  end
  local g = 0.3 + 0.7 * seen / 5

  px.clear(0, 0, 0)
  background(S, g)

  for _, l in ipairs(RANGE_LABELS) do
    px.text(l[2], l[3], l[1], dim(TEAL, 0.8 * g))
  end

  local near
  for _, t in ipairs(targets) do
    local person = PEOPLE[t.slot]
    local col = person.col
    local mx, my = t.x / 1000, t.y / 1000
    local bx, by = to_px(mx, my)
    local ix, iy = floor(bx), floor(by)
    local d = sqrt(mx * mx + my * my)
    near = near and min(near, d) or d

    -- trail, sampled at the radar's own 10 Hz
    for k = 1, 14 do
      local x, y
      if live then
        local tx, ty = RAD.trail(t.slot, k)
        if tx then x, y = tx / 1000, ty / 1000 end
      else
        x, y = pos(person, S - k * 0.1)
      end
      local there = x ~= nil and in_fan(x, y)
      VISIBLE[k] = there
      if there then
        local tx, ty = to_px(x, y)
        px.blend(floor(tx), floor(ty), col[1], col[2], col[3], 0.75 * (1 - k / 15))
      end
    end

    -- just walked in: a burst of light where the radar first saw them
    local age = 1.0
    for k = 1, 10 do
      if not VISIBLE[k] then age = (k - 1) * 0.1; break end
    end
    if age < 1.0 then px.glow(bx, by, 3 + 14 * age, col[1], col[2], col[3], 0.8 * (1 - age)) end

    if t.speed > 12 then
      px.glow(bx, by, 5, col[1], col[2], col[3], 0.9)
    else
      -- sitting still: a slow ring, so a person who does not move is not
      -- mistaken for a dead pixel
      local f = (S / 1.6 + t.slot * 0.3) % 1
      local rr = 2 + 6 * f
      for a = 0, 35 do
        px.blend(floor(bx + rr * cos(a * pi / 18)), floor(by + rr * sin(a * pi / 18)),
                 col[1], col[2], col[3], 0.7 * (1 - f))
      end
      px.glow(bx, by, 4, col[1], col[2], col[3], 0.6)
    end
    px.rect(ix - 1, iy - 1, 2, 2, (col[1] + 255) // 2, (col[2] + 255) // 2, (col[3] + 255) // 2, true)

    local label = fmt("%.1fM", d)
    px.text(min(W - px.width(label) - 1, ix + 4), max(0, iy - 9), label, dim(col, 0.85))
  end

  px.text(1, 1, "ROOM RADAR", dim(TEAL, g))
  local n = #targets
  local white = floor(255 * g)
  -- Picopixel's glyphs start two rows below y, so 56 is the last row that fits.
  big(2, 51, tostring(n), white, white, white)
  if n == 0 then
    -- An empty room and a radar that stopped reporting draw the same picture,
    -- so the second one is named rather than left to look like the first.
    px.text(10, 50, state == LOST and "NO FEED" or "EMPTY", dim(TEAL, g))
  else
    px.text(10, 50, "IN ROOM", dim(TEAL, g))
    px.text(10, 56, fmt("NEAR %.1fM", near), dim(TEAL, g))
  end

  local now = px.now()
  local hhmm = fmt("%02d:%02d", now.hour, now.min)
  px.text(W - px.width(hhmm) - 1, 56, hhmm, white, white, white)
  px.text(W - px.width("LD2450") - 1, 50, "LD2450", dim(TEAL, 0.7 * g))
end

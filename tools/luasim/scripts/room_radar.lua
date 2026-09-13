-- room_radar.lua - who is in the room, as a 24 GHz radar sees it.
--
-- The HLK-LD2450 reports up to three people, each as X and Y in millimetres
-- plus a speed, ten times a second, inside a fan 6 m deep and 60 degrees either
-- side. This draws exactly that fan up from the middle of the bottom edge, so
-- the picture is the sensor's own view of the room: 10 px to the metre.
--
-- The people are scripted. fake_targets() returns what the radar would, a list
-- of {x, y, speed} in mm and cm/s, and on the panel it is the one function to
-- replace. The trails read the script's paths directly; on the panel they need
-- a short history per target slot instead.
--
-- Memory is two 128-byte strings per row for the precomputed grid, about 17 KB,
-- not a table per pixel: the firmware's Lua heap has to share PSRAM with the
-- HUB75 driver.

local W, H = px.size()
local floor, min, max, abs = math.floor, math.min, math.max, math.abs
local sin, cos, sqrt, atan, exp, pi =
  math.sin, math.cos, math.sqrt, math.atan, math.exp, math.pi
local deg, rad = math.deg, math.rad
local fmt, byte, char = string.format, string.byte, string.char

local STORY  = 24.0            -- seconds the scripted scene lasts; px.t() spans it
local CX, CY = 63.5, 63.0      -- the sensor
local PPM    = 10              -- pixels per metre
local RANGE, HALF = 6.0, 60    -- metres, and degrees either side

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
-- For every pixel: how lit its ring or spoke is, and at which phase of the
-- swing the beam crosses it. Both are fixed, so both are computed once.
local GRID, PHASE, X0, X1 = {}, {}, {}, {}
do
  local SPOKES = {-60, -30, 0, 30, 60}
  for y = 0, H - 1 do
    local g, p, first, last = {}, {}, nil, nil
    for x = 0, W - 1 do
      local mx, my = (x + 0.5 - CX) / PPM, (CY - (y + 0.5)) / PPM
      local r, ang = sqrt(mx * mx + my * my), deg(atan(mx, my))
      local v, ph = 0, 0
      if my > 0 and r <= RANGE + 0.08 and abs(ang) <= HALF + 0.8 then
        local lit = 0
        for k = 1, 6 do                                -- a ring every metre
          local d = abs(r - k) * PPM
          if d < 1 then lit = max(lit, (1 - d) * ((k % 3 == 0) and 1.0 or 0.5)) end
        end
        for _, a in ipairs(SPOKES) do                  -- dotted spokes, solid edges
          local ar = rad(a)
          local d = abs(mx * cos(ar) - my * sin(ar)) * PPM
          local edge = abs(a) == HALF
          if d < 1 and (edge or floor(r * PPM) % 3 == 0) then
            lit = max(lit, (1 - d) * (edge and 0.75 or 0.4))
          end
        end
        v  = 1 + floor(lit * 254)
        ph = floor((max(-1, min(1, ang / SWEEP_A)) + 1) / 2 * 255 + 0.5)
        first = first or x; last = x
      end
      g[#g + 1], p[#p + 1] = char(v), char(ph)
    end
    GRID[y], PHASE[y], X0[y], X1[y] = table.concat(g), table.concat(p), first, last
  end
end

-- The beam swings at constant speed, out and back, so it crosses a pixel at two
-- points of each swing; time since the later one is the phosphor's age. A sine
-- swing was tried first: it dwells at the edges and burns a bright band there.
local function background(S, g)
  local u = (S / SWEEP_T) % 1
  for y = 0, H - 1 do
    local x0 = X0[y]
    if x0 then
      local grid, phase = GRID[y], PHASE[y]
      for x = x0, X1[y] do
        local v = byte(grid, x + 1)
        if v > 0 then
          local q = byte(phase, x + 1) / 255 * 2 - 1
          local since = min((u - (q + 1) / 4) % 1, (u - (3 - q) / 4) % 1) * SWEEP_T
          local beam = 0.5 * exp(-since / PERSIST) + 0.7 * exp(-since / 0.05)
          local lit = (v - 1) / 254
          px.pixel(x, y, min(255, floor((5 + lit * 30 + beam * 12) * g)),
                         min(255, floor((14 + lit * 140 + beam * 95) * g)),
                         min(255, floor((11 + lit * 105 + beam * 70) * g)))
        end
      end
    end
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

local function visible(person, s)
  local x, y = pos(person, s)
  return x ~= nil and in_fan(x, y)
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

-- ---------------------------------------------------------------- draw
function draw()
  local S = px.t() * STORY
  local targets = fake_targets(S)

  -- An empty room fades down after a moment: the sleep idea, drawn.
  local seen = 0
  for k = 0, 4 do
    if #fake_targets(S - k * 0.3) > 0 then seen = seen + 1 end
  end
  local g = 0.3 + 0.7 * seen / 5

  px.clear(0, 0, 0)
  background(S, g)

  local TEAL = {0, 150, 115}
  for _, k in ipairs({2, 4, 6}) do
    local label = k .. "M"
    local lx = min(W - px.width(label), floor(CX + k * PPM * sin(rad(HALF)) + 4))
    local ly = floor(CY - k * PPM * cos(rad(HALF)) - 5)
    px.text(lx, ly, label, dim(TEAL, 0.8 * g))
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
      local x, y = pos(person, S - k * 0.1)
      if x and in_fan(x, y) then
        local tx, ty = to_px(x, y)
        px.blend(floor(tx), floor(ty), col[1], col[2], col[3], 0.75 * (1 - k / 15))
      end
    end

    -- just walked in: a burst of light where the radar first saw them
    local age = 1.0
    for k = 1, 10 do
      if not visible(person, S - k * 0.1) then age = (k - 1) * 0.1; break end
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
    px.text(10, 50, "EMPTY", dim(TEAL, g))
  else
    px.text(10, 50, "IN ROOM", dim(TEAL, g))
    px.text(10, 56, fmt("NEAR %.1fM", near), dim(TEAL, g))
  end

  local now = px.now()
  local hhmm = fmt("%02d:%02d", now.hour, now.min)
  px.text(W - px.width(hhmm) - 1, 56, hhmm, white, white, white)
  px.text(W - px.width("LD2450") - 1, 50, "LD2450", dim(TEAL, 0.7 * g))
end

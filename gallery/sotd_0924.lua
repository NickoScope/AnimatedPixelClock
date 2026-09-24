-- @upload-only
-- @by claude
-- SOTD 0924 - Screen of the Day for 24 September 2026
-- sotd_0924.lua - Screen of the Day for 24 September 2026, "Картинка дня": a lake in golden autumn, lit by the real sun.
--
-- The scene follows the sun as it stands over LAT/LON at the panel's own date
-- and time: a blue night with stars and the moon, a pink dawn with mist on the
-- water, a clear blue day, an orange sunset. The sun rises on the left (east)
-- and sets on the right (west) at its true height; the sky, hills, trees and
-- water take their light from it. Birches drop their leaves into the wind, a
-- flock flies south by day, a fisherman drifts in his boat and lights a lantern
-- at night. Title top left, today's date in Russian top right, the next sunrise
-- or sunset bottom left, the time bottom right.
--
-- Everything is redrawn every frame from filled rects, short lines and a
-- handful of circles, with one glow; the palette is rebuilt once a minute.

PERIOD = 60
FPS = 20

-- Where the panel stands. Moscow; change to your own place.
local LAT, LON = 55.75, 37.62

local W, H = px.size()
local sin, cos, asin, acos, pi, floor = math.sin, math.cos, math.asin, math.acos, math.pi, math.floor
local rad = pi / 180
local HORIZON = 40

local function mix(a, b, f) return floor(a + (b - a) * f + 0.5) end
local function mix3(a, b, f) return { mix(a[1], b[1], f), mix(a[2], b[2], f), mix(a[3], b[3], f) } end
local function clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end
local function scale(c, k) return floor(c[1] * k), floor(c[2] * k), floor(c[3] * k) end

---------------------------------------------------------------- the sun

-- Solar elevation (degrees), hour angle (degrees) and the clock times of
-- sunrise, solar noon and sunset (hours), from NOAA's short formulas.
local function sun(n)
  local N = n.yday + 1
  local decl = 23.44 * sin(2 * pi * (284 + N) / 365) * rad
  local B = 2 * pi * (N - 81) / 364
  local eot = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B)   -- minutes
  local shift = (4 * LON - 60 * n.utc + eot) / 60                -- solar - clock, hours
  local clock = n.hour + n.min / 60 + n.sec / 3600
  local ha = 15 * (clock + shift - 12)
  local phi = LAT * rad
  local elev = asin(sin(phi) * sin(decl) + cos(phi) * cos(decl) * cos(ha * rad)) / rad
  local c0 = (sin(-0.833 * rad) - sin(phi) * sin(decl)) / (cos(phi) * cos(decl))
  local h0 = acos(clamp(c0, -1, 1)) / rad
  local noon = 12 - shift
  return elev, ha, h0, noon - h0 / 15, noon, noon + h0 / 15, clock
end

-- Light by solar elevation: sky top / middle / horizon, far and near hills,
-- and a brightness for everything lit (trees, boat).
local KEYS = {
  { -18, {  2,  2, 10 }, {  4,  4, 18 }, {  10,  10,  30 }, {  12, 10, 25 }, {   5,  4, 12 }, 0.18 },
  {  -8, {  8, 10, 40 }, { 25, 25, 80 }, {  70,  50, 110 }, {  30, 25, 60 }, {  12, 10, 30 }, 0.30 },
  {  -2, { 15, 12, 50 }, { 80, 30, 90 }, { 230, 110,  70 }, {  90, 35, 80 }, {  40, 16, 45 }, 0.55 },
  {   2, { 20, 20, 70 }, {120, 70,120 }, { 255, 150,  60 }, {  95, 40, 80 }, {  45, 18, 45 }, 0.70 },
  {   8, { 40, 70,150 }, {120,130,190 }, { 255, 190, 120 }, { 110, 90,130 }, {  80, 55, 50 }, 0.90 },
  {  20, { 30,100,210 }, { 80,150,230 }, { 170, 210, 240 }, { 110,135,175 }, { 120, 90, 40 }, 1.00 },
}

local function light(elev)
  if elev <= KEYS[1][1] then local k = KEYS[1]; return k[2], k[3], k[4], k[5], k[6], k[7] end
  for i = 1, #KEYS - 1 do
    local a, b = KEYS[i], KEYS[i + 1]
    if elev <= b[1] then
      local f = (elev - a[1]) / (b[1] - a[1])
      return mix3(a[2], b[2], f), mix3(a[3], b[3], f), mix3(a[4], b[4], f),
             mix3(a[5], b[5], f), mix3(a[6], b[6], f), a[7] + (b[7] - a[7]) * f
    end
  end
  local k = KEYS[#KEYS]; return k[2], k[3], k[4], k[5], k[6], k[7]
end

---------------------------------------------------------------- fixed shapes

local far, near = {}, {}
for x = 0, W - 1 do
  far[x]  = floor(HORIZON - 4 - 3 * sin(x * 0.06 + 1.0) - 2 * sin(x * 0.17))
  near[x] = floor(HORIZON - 1 - 2 * sin(x * 0.045 + 3.0) - 1.5 * sin(x * 0.23 + 0.5))
  if near[x] > HORIZON then near[x] = HORIZON end
end

local stars = {}
for i = 1, 22 do
  stars[i] = { x = (i * 47 + 11) % W, y = 8 + (i * 29) % 22, p = (i * 7 % 10) / 10 }
end

local birches = { { x = 6, top = 20 }, { x = 15, top = 16 }, { x = 25, top = 22 } }
local crown = {
  { -3, 0, 5, 230, 140, 20 }, { 3, 2, 4, 250, 190, 40 }, { 0, -3, 4, 210, 90, 20 },
  { -2, 5, 3, 250, 170, 30 }, { 3, 6, 3, 200, 70, 20 },
}
local pines = { { 108, 22 }, { 116, 17 }, { 123, 24 } }

local leafColours = { { 255, 170, 30 }, { 240, 90, 20 }, { 255, 210, 60 }, { 200, 50, 20 } }
local leaves = {}
for i = 1, 12 do
  local b = birches[(i % #birches) + 1]
  leaves[i] = {
    x0 = b.x + (i * 5 % 9) - 4, y0 = b.top + (i * 3 % 7),
    land = HORIZON + 3 + (i * 7 % 18),
    drift = 30 + (i * 13 % 50),
    p = (i * 0.37) % 1,
    c = leafColours[(i % #leafColours) + 1],
  }
end

local mist = {}
for i = 1, 7 do
  mist[i] = { y = HORIZON - 2 + (i * 3 % 8), len = 18 + (i * 11 % 22), p = (i * 0.29) % 1 }
end

---------------------------------------------------------------- text

local MONTHS = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля",
                 "августа", "сентября", "октября", "ноября", "декабря" }
local DAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }

local function dateText(n)
  local leap = (n.year % 4 == 0 and n.year % 100 ~= 0) or n.year % 400 == 0
  local d = n.yday + 1
  for m = 1, 12 do
    local len = DAYS[m] + ((m == 2 and leap) and 1 or 0)
    if d <= len then return d .. " " .. MONTHS[m] end
    d = d - len
  end
  return ""
end

local function hm(h)
  local m = floor(h * 60 + 0.5) % 1440
  return string.format("%d:%02d", m // 60, m % 60)
end

local function shadowText(x, y, s, r, g, b)
  px.text(x + 1, y + 1, s, 0, 0, 0)
  px.text(x, y, s, r, g, b)
end

---------------------------------------------------------------- the minute's palette

local S = { key = -1 }

local function rebuild(n)
  local elev, ha, h0, rise, noon, set, clock = sun(n)
  local top, mid, hor, farC, nearC, L = light(elev)
  S.elev, S.L, S.farC, S.nearC = elev, L, farC, nearC

  S.sky = {}
  for y = 0, HORIZON - 1 do
    local f = y / (HORIZON - 1)
    S.sky[y] = f < 0.5 and mix3(top, mid, f / 0.5) or mix3(mid, hor, (f - 0.5) / 0.5)
  end
  S.water = {}
  for y = HORIZON, H - 1 do
    local d = (y - HORIZON) / (H - 1 - HORIZON)
    local s = S.sky[HORIZON - 1 - floor(d * 20)]
    local k = 0.60 - 0.40 * d
    S.water[y] = { floor(s[1] * k), floor(s[2] * k), floor(s[3] * k + 12 * d) }
  end

  -- The sun: east on the left, west on the right, at its true height.
  local span = h0 + 12
  S.sunX = floor(64 + clamp(ha / span, -1.1, 1.1) * 40)
  S.sunY = floor(HORIZON - 1 - elev * 0.9)
  S.sunUp = elev > -1.5
  S.sunC = elev <= 0 and { 255, 120, 40 }
        or elev < 10 and mix3({ 255, 120, 40 }, { 255, 200, 110 }, elev / 10)
        or mix3({ 255, 200, 110 }, { 255, 245, 210 }, clamp((elev - 10) / 15, 0, 1))

  S.starK = clamp((-4 - elev) / 10, 0, 1)
  S.mistK = (n.hour < 12 and elev > -6 and elev < 12) and clamp(1 - math.abs(elev - 3) / 9, 0, 1) or 0
  S.pine = mix3({ 4, 6, 8 }, { 25, 70, 45 }, L)
  S.boat = mix3({ 20, 10, 15 }, { 110, 60, 30 }, clamp((L - 0.6) / 0.4, 0, 1))

  if clock >= rise and clock < set and clock < noon then
    S.caption = "ВОСХОД " .. hm(rise)
  elseif clock >= noon and clock < set then
    S.caption = "ЗАКАТ " .. hm(set)
  else
    S.caption = "ВОСХОД " .. hm(rise)
  end
  S.date = dateText(n)
end

---------------------------------------------------------------- draw

function draw()
  local t = px.t()
  local tau = 2 * pi * t
  local n = px.now()
  local key = n.yday * 1440 + n.hour * 60 + n.min
  if key ~= S.key then S.key = key; rebuild(n) end
  local L = S.L

  -- Sky, stars, moon.
  for y = 0, HORIZON - 1 do
    local c = S.sky[y]
    px.rect(0, y, W, 1, c[1], c[2], c[3], true)
  end
  if S.starK > 0 then
    for _, s in ipairs(stars) do
      local b = floor(S.starK * (60 + 90 * (0.5 + 0.5 * sin(2 * pi * (t * 4 + s.p)))))
      px.pixel(s.x, s.y, b, b, b + floor(30 * S.starK))
    end
  end
  if S.starK > 0.3 then
    local c = S.sky[12]
    px.circle(96, 14, 5, mix(c[1], 235, S.starK), mix(c[2], 230, S.starK), mix(c[3], 200, S.starK), true)
    px.circle(98, 12, 5, c[1], c[2], c[3], true)
  end

  -- Sun, behind the hills when it is low.
  local sx, sy = S.sunX, S.sunY
  if S.sunUp then
    local c = S.sunC
    px.glow(sx, sy, 13, c[1], floor(c[2] * 0.8), floor(c[3] * 0.6), 0.6)
    px.circle(sx, sy, 6, c[1], c[2], c[3], true)
    px.circle(sx, sy, 4, 255, mix(c[2], 255, 0.5), mix(c[3], 255, 0.5), true)
  end

  -- A flock heading south, by day only.
  if S.elev > -4 then
    local fx = floor(W + 20 - ((t * 2) % 1) * (W + 60))
    local bc = L > 0.8 and 40 or 25
    for i = 0, 6 do
      local side = (i % 2 == 0) and 1 or -1
      local rank = floor((i + 1) / 2)
      local bx, by = fx + rank * 5, 14 + side * rank * 2
      local up = (sin(tau * 30 + i) > 0) and -1 or 1
      px.line(bx - 2, by + up, bx, by, bc, bc - 10, bc)
      px.line(bx, by, bx + 2, by + up, bc, bc - 10, bc)
    end
  end

  -- Hills.
  local fc, nc = S.farC, S.nearC
  for x = 0, W - 1 do
    px.line(x, far[x], x, HORIZON - 1, fc[1], fc[2], fc[3])
    px.line(x, near[x], x, HORIZON - 1, nc[1], nc[2], nc[3])
  end

  -- Lake, and the sun's path on it.
  for y = HORIZON, H - 1 do
    local c = S.water[y]
    px.rect(0, y, W, 1, c[1], c[2], c[3], true)
  end
  if S.sunUp then
    local c = S.sunC
    local base = S.elev < 10 and 9 or 6
    for y = HORIZON + 1, H - 1, 2 do
      local d = (y - HORIZON) / (H - HORIZON)
      local w = floor((base - 4 * d) * (0.55 + 0.45 * sin(tau * 6 + y * 0.9)))
      if w > 0 then
        local off = floor(2 * sin(tau * 3 + y * 0.5))
        px.rect(sx - w + off, y, w * 2, 1, c[1], mix(c[2], 80, d), mix(c[3], 40, d), true)
      end
    end
  end
  local hc = S.sky[HORIZON - 1]
  px.line(0, HORIZON, W - 1, HORIZON, hc[1], hc[2], hc[3])

  -- Morning mist, drifting over the water.
  if S.mistK > 0 then
    local m = S.mistK
    for _, s in ipairs(mist) do
      local x = floor(((t * 1.5 + s.p) % 1) * (W + s.len)) - s.len
      local c = S.water[math.max(s.y, HORIZON)]
      px.rect(x, s.y, s.len, 1, mix(c[1], 230, 0.45 * m), mix(c[2], 225, 0.45 * m), mix(c[3], 235, 0.45 * m), true)
    end
  end

  -- Shores: birches left, pines right, with reflections.
  do local r, g, b = scale({ 60, 35, 25 }, L); px.rect(0, HORIZON - 2, 34, 3, r, g, b, true) end
  for _, b in ipairs(birches) do
    local tr, tg, tb = scale({ 225, 220, 205 }, L)
    px.line(b.x, b.top + 4, b.x, HORIZON, tr, tg, tb)
    px.pixel(b.x, b.top + 9, 30, 20, 20)
    px.pixel(b.x, b.top + 14, 30, 20, 20)
    px.line(b.x, HORIZON + 1, b.x, HORIZON + 6, scale({ 110, 100, 100 }, L))
    local sway = floor(1.2 * sin(tau * 2 + b.x))
    for _, c in ipairs(crown) do
      local r, g, bl = scale({ c[4], c[5], c[6] }, L)
      px.circle(b.x + c[1] + sway, b.top + c[2], c[3], r, g, bl, true)
    end
  end
  local pc = S.pine
  for _, p in ipairs(pines) do
    local x, top = p[1], p[2]
    for y = top, HORIZON do
      local w = floor((y - top) * 0.28) + ((y - top) % 4 == 0 and 1 or 0)
      px.rect(x - w, y, w * 2 + 1, 1, pc[1], pc[2], pc[3], true)
    end
    px.line(x, HORIZON + 1, x, HORIZON + 5, floor(pc[1] * 0.7), floor(pc[2] * 0.7), floor(pc[3] * 0.7) + 10)
  end

  -- Fisherman in a boat, drifting; a lantern after dark.
  local bx, by = floor(84 + 8 * sin(tau)), 52
  local bc = S.boat
  px.rect(bx - 7, by, 15, 2, bc[1], bc[2], bc[3], true)
  px.rect(bx - 5, by + 2, 11, 1, bc[1], bc[2], bc[3], true)
  do local r, g, b = scale({ 40, 60, 120 }, math.max(L, 0.2)); px.rect(bx - 1, by - 4, 2, 4, r, g, b, true) end
  px.pixel(bx - 1, by - 5, scale({ 230, 180, 140 }, math.max(L, 0.2)))
  px.line(bx + 1, by - 3, bx + 10, by - 9, scale({ 90, 70, 60 }, math.max(L, 0.5)))
  px.line(bx + 10, by - 8, bx + 10, by, scale({ 140, 130, 130 }, math.max(L, 0.4)))
  px.rect(bx - 6, by + 4, 13, 1, floor(bc[1] * 0.6), floor(bc[2] * 0.6), floor(bc[3] * 0.6) + 15, true)
  if L < 0.5 then
    local flick = 0.8 + 0.2 * sin(tau * 40)
    px.glow(bx - 5, by - 2, 4, 255, 180, 60, flick)
    px.pixel(bx - 5, by - 2, 255, 220, 120)
  end

  -- Falling leaves.
  for _, l in ipairs(leaves) do
    local p = (t * 3 + l.p) % 1
    local x, y
    if p < 0.75 then
      local f = p / 0.75
      x = l.x0 + l.drift * f + 3 * sin(2 * pi * (f * 3 + l.p))
      y = l.y0 + (l.land - l.y0) * f
    else
      x = l.x0 + l.drift + (p - 0.75) * 12
      y = l.land
    end
    x, y = floor(x), floor(y)
    local r, g, b = scale(l.c, math.max(L, 0.35))
    px.pixel(x, y, r, g, b)
    if sin(2 * pi * (p * 5 + l.p)) > 0 then px.pixel(x + 1, y, floor(r * 0.6), floor(g * 0.6), floor(b * 0.6))
    else px.pixel(x, y + 1, floor(r * 0.6), floor(g * 0.6), floor(b * 0.6)) end
  end

  -- Text.
  shadowText(2, 1, "КАРТИНКА ДНЯ", 255, 215, 120)
  shadowText(W - px.width(S.date) - 2, 1, S.date, 255, 170, 190)
  shadowText(2, H - 7, S.caption, 255, 200, 140)
  local hhmm = string.format("%02d:%02d", n.hour, n.min)
  shadowText(W - px.width(hhmm) - 2, H - 7, hhmm, 210, 200, 230)
end

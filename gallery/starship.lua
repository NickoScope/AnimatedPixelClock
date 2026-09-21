-- @upload-only
-- STARSHIP - one flight a minute, and the minute is the clock.
--
-- PERIOD is 60, so px.t() is the second hand: the count, the launch, staging,
-- the catch, two orbits and the splashdown all happen against the real seconds
-- of the real minute. At :00 the engines light. The clock is not drawn beside
-- the picture - it IS the picture.
--
-- Nothing here fades the screen. The fireworks effect showed what that costs on
-- this panel: 5,760 px.blend calls a frame and 119 ms of wall clock. This one
-- clears and redraws, and keeps under about 400 px.* calls a frame, because the
-- crossing from Lua into C is what a frame is actually spent on.
--
-- The randomness is xorshift32 and not the textbook LCG: this Lua is built with
-- LUA_32BITS, lua_Integer is int32, and `seed * 1103515245` overflows into a
-- generator that repeats two values. See effect_api, "numbers".

PERIOD = 60.0
FPS = 20

local W, H = 128, 64
local floor, ceil, sin, cos, sqrt, abs = math.floor, math.ceil, math.sin, math.cos, math.sqrt, math.abs
local min, max, pi = math.min, math.max, math.pi

-- --------------------------------------------------------------- random
local seed = 0x5EED1234
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7FFFFFFF) / 2147483648.0
end

-- --------------------------------------------------------------- the flight
-- Every phase is a slice of the minute. The boundaries are the story, so they
-- are named once and read from everywhere.
local T_COUNT   = 0.00   -- T-10 on the pad, engines chilled
local T_IGNITE  = 0.155  -- ignition, hold-down, the pad floods with light
local T_LIFT    = 0.185  -- release
local T_ASCENT  = 0.230  -- through the sky, it goes black and the stars come
local T_STAGE   = 0.375  -- hot staging: the ship lights while still attached
local T_SPLIT   = 0.405  -- they part, the booster flips
local T_CATCH   = 0.560  -- the booster is back at the tower
local T_ORBIT   = 0.600  -- Earth's limb below, two laps
local T_ENTRY   = 0.830  -- retrograde burn and the plasma
local T_FLIP    = 0.930  -- the belly flop and the flip
local T_SPLASH  = 0.965  -- the Gulf

local function phase(t)
  if t < T_IGNITE then return "count",  (t - T_COUNT)  / (T_IGNITE - T_COUNT)
  elseif t < T_LIFT   then return "ignite", (t - T_IGNITE) / (T_LIFT  - T_IGNITE)
  elseif t < T_ASCENT then return "lift",   (t - T_LIFT)   / (T_ASCENT - T_LIFT)
  elseif t < T_STAGE  then return "ascent", (t - T_ASCENT) / (T_STAGE - T_ASCENT)
  elseif t < T_SPLIT  then return "stage",  (t - T_STAGE)  / (T_SPLIT - T_STAGE)
  elseif t < T_CATCH  then return "split",  (t - T_SPLIT)  / (T_CATCH - T_SPLIT)
  elseif t < T_ORBIT  then return "catch",  (t - T_CATCH)  / (T_ORBIT - T_CATCH)
  elseif t < T_ENTRY  then return "orbit",  (t - T_ORBIT)  / (T_ENTRY - T_ORBIT)
  elseif t < T_FLIP   then return "entry",  (t - T_ENTRY)  / (T_FLIP - T_ENTRY)
  elseif t < T_SPLASH then return "flip",   (t - T_FLIP)   / (T_SPLASH - T_FLIP)
  else                     return "splash", (t - T_SPLASH) / (1.0 - T_SPLASH) end
end

-- --------------------------------------------------------------- the sky
-- Forty stars, placed once at load and never moved: they are the frame of
-- reference everything else moves against.
local SX, SY, SB = {}, {}, {}
for i = 1, 40 do
  SX[i] = floor(rnd() * W)
  SY[i] = floor(rnd() * 42)
  SB[i] = 70 + floor(rnd() * 150)
end

-- How black the sky is: ground level is a pale dawn, orbit is nothing at all.
local function skyAt(alt)              -- alt 0..1
  local k = 1.0 - min(1.0, alt)
  return floor(6 + 26 * k * k), floor(10 + 34 * k * k), floor(22 + 60 * k * k)
end

local function stars(alt, twinkle)
  if alt < 0.18 then return end
  local f = min(1.0, (alt - 0.18) / 0.35)
  for i = 1, 40 do
    local b = SB[i] * f
    -- A star does not blink on and off; it breathes. One in three is doing it
    -- at any moment, on its own phase.
    if (i + twinkle) % 3 == 0 then b = b * 0.55 end
    local v = floor(b)
    px.pixel(SX[i], SY[i], v, v, floor(v * 1.05))
  end
end

-- --------------------------------------------------------------- the pad
local PAD_Y   = 57
local TOWER_X = 86

local function ground(shake)
  px.rect(0, PAD_Y, W, H - PAD_Y, 14, 12, 10, true)
  px.rect(0, PAD_Y, W, 1, 30, 26, 20, true)
  -- the flame trench under the mount
  px.rect(52, PAD_Y, 22, 2, 8, 7, 6, true)
  for x = 0, W - 1, 7 do
    px.pixel(x, PAD_Y + 3 + ((x + shake) % 2), 22, 19, 15)
  end
end

-- Mechazilla: a lattice tower with the two arms. Drawn once a frame and cheap -
-- eight lines - because it is the thing the booster has to come back to.
local function tower(armOpen, lit)
  local r, g, b = 60, 62, 68
  if lit > 0 then
    r = floor(r + (255 - r) * lit * 0.5)
    g = floor(g + (200 - g) * lit * 0.5)
    b = floor(b + (120 - b) * lit * 0.5)
  end
  px.line(TOWER_X, 16, TOWER_X, PAD_Y - 1, r, g, b)
  px.line(TOWER_X + 5, 16, TOWER_X + 5, PAD_Y - 1, r, g, b)
  for y = 18, PAD_Y - 2, 5 do
    px.line(TOWER_X, y, TOWER_X + 5, y - 2, floor(r * 0.7), floor(g * 0.7), floor(b * 0.7))
  end
  px.line(TOWER_X, 14, TOWER_X + 5, 14, r, g, b)
  -- the arms: open while it waits, closing on the catch
  local reach = floor(3 + 9 * armOpen)
  px.line(TOWER_X - 1, 24, TOWER_X - reach, 24, r, g, b)
  px.line(TOWER_X - 1, 27, TOWER_X - reach, 27, r, g, b)
end

-- --------------------------------------------------------------- the stack
-- Starship is a bullet on a barrel: the ship's nose is a cone, the booster is a
-- longer cylinder with a flare of engines. Drawn from its base so every phase
-- can just say where the base is.
local function booster(x, base, len, roll)
  local w = 5
  px.rect(x - w // 2, base - len, w, len, 168, 172, 178, true)
  px.rect(x - w // 2, base - len, 1, len, 96, 100, 106, true)      -- the shaded side
  px.rect(x - w // 2, base - 1, w, 1, 60, 62, 66, true)            -- the engine skirt
  -- grid fins, and they are not symmetric once it is coming down
  local fy = base - len + 2
  px.line(x - w // 2 - 1, fy, x - w // 2 - 1, fy + 2, 120, 124, 130)
  px.line(x + w // 2, fy + (roll > 0 and 1 or 0), x + w // 2, fy + 2, 120, 124, 130)
end

local function ship(x, base, tilt, glow)
  local h = 11
  local r, g, b = 196, 200, 206
  if glow > 0 then                       -- the heat shield, seen from behind
    r = floor(r + (255 - r) * glow)
    g = floor(g - g * 0.45 * glow)
    b = floor(b - b * 0.7 * glow)
  end
  local dx = floor(tilt * 3)
  px.rect(x - 2, base - h + 3, 5, h - 3, r, g, b, true)
  px.rect(x - 2, base - h + 3, 1, h - 3, floor(r * 0.55), floor(g * 0.55), floor(b * 0.55), true)
  -- the nose: three rows narrowing
  px.rect(x - 2 + dx, base - h + 2, 5, 1, r, g, b, true)
  px.rect(x - 1 + dx, base - h + 1, 3, 1, r, g, b, true)
  px.pixel(x + dx, base - h, r, g, b)
  -- forward flaps
  px.line(x - 3, base - h + 4, x - 3, base - h + 6, 130, 134, 140)
  px.line(x + 3, base - h + 4, x + 3, base - h + 6, 130, 134, 140)
end

-- --------------------------------------------------------------- the fire
-- One plume routine for every engine in the effect. Length and spread say what
-- it is: a hold-down flood, a launch column, a relight, a landing burn.
local function plume(x, y, len, spread, power, sideways)
  local n = floor(10 + 22 * power)
  for i = 1, n do
    local d = rnd()
    local dist = d * len
    local wob = (rnd() - 0.5) * spread * (0.3 + d)
    local px_, py_
    if sideways ~= 0 then
      px_ = x + dist * sideways
      py_ = y + wob
    else
      px_ = x + wob
      py_ = y + dist
    end
    -- white at the throat, orange at the edge, dark at the tip
    local heat = 1.0 - d
    local r = floor(255 * min(1.0, heat * 1.6))
    local g = floor(210 * heat * heat + 40 * heat)
    local b = floor(150 * heat * heat * heat)
    px.blend(floor(px_), floor(py_), r, g, b, 0.35 + 0.6 * heat)
  end
end

-- --------------------------------------------------------------- the Earth
-- The limb from low orbit: a curve, a hairline of atmosphere on top of it, the
-- terminator sliding across, and the night side lit from below by cities. It is
-- drawn a column at a time - one rect each, not a pixel each - because a
-- per-pixel globe is 2,500 crossings into C and this panel cannot pay that
-- twice a second.
local CX, CY, CR = 64, 132, 82

-- Continents, placed once. Blobs rather than noise: a dither reads as static,
-- and what makes a globe look like Earth is large shapes with coastlines.
local LAND = {
  {  8, 2, 26, 9}, { 30, 6, 14, 6}, { 52, 1, 30, 7}, { 70, 6, 18, 8},
  { 96, 3, 24, 8}, { 18,12, 20, 5}, { 60,11, 22, 6}, {100,10, 16, 5},
}
-- Cities: only seen once a column has crossed into night.
local CITYX, CITYY = {}, {}
for i = 1, 26 do CITYX[i] = floor(rnd() * W); CITYY[i] = 2 + floor(rnd() * 14) end

local function limbTop(x)
  local dx = x - CX
  local inside = CR * CR - dx * dx
  if inside <= 0 then return nil end
  return floor(CY - sqrt(inside))
end

local function earth(rot)
  -- lit(x): 1 in full day, 0 deep in night, with the terminator walking
  local function lit(x)
    local v = 0.5 + 0.5 * cos((x / W) * 1.7 * pi + rot)
    return v
  end

  for x = 0, W - 1 do
    local ty = limbTop(x)
    if ty and ty < H then
      local L = lit(x)
      local day = max(0.0, (L - 0.42) / 0.58)        -- 0 at the terminator
      -- the ocean, one rect for the whole column
      px.rect(x, ty, 1, H - ty,
              floor(6 + 16 * day), floor(22 + 58 * day), floor(48 + 120 * day), true)
      -- the atmosphere, brightest right on the edge and always a little lit
      local a = 0.30 + 0.70 * L
      px.blend(x, ty - 1, floor(90 * a), floor(170 * a), floor(255 * a), 0.85)
      px.blend(x, ty - 2, floor(50 * a), floor(110 * a), floor(210 * a), 0.45)
      px.blend(x, ty - 3, floor(20 * a), floor(60 * a), floor(140 * a), 0.22)
    end
  end

  -- land, clipped to the globe and shaded by the same terminator
  for i = 1, #LAND do
    local b = LAND[i]
    for x = b[1], b[1] + b[3] - 1 do
      if x >= 0 and x < W then
        local ty = limbTop(x)
        if ty then
          local L = lit(x)
          local day = max(0.0, (L - 0.42) / 0.58)
          local y0 = ty + b[2]
          local hh = min(b[4], H - y0)
          if hh > 0 and day > 0.04 then
            px.rect(x, y0, 1, hh,
                    floor(24 + 74 * day), floor(40 + 76 * day), floor(26 + 44 * day), true)
          end
        end
      end
    end
  end

  -- and the cities, which are the thing that makes it read as Earth at night
  for i = 1, 26 do
    local x = CITYX[i]
    local ty = limbTop(x)
    if ty then
      local L = lit(x)
      if L < 0.40 then
        local k = (0.40 - L) / 0.40
        px.blend(x, ty + CITYY[i], 255, 190, 110, 0.25 + 0.55 * k)
      end
    end
  end
end

-- --------------------------------------------------------------- the Gulf
local function gulf(splash)
  for y = 46, H - 1 do
    local d = (y - 46) / (H - 46)
    px.rect(0, y, W, 1, floor(8 + 10 * d), floor(30 + 26 * d), floor(58 + 40 * d), true)
  end
  -- swell
  for x = 0, W - 1, 2 do
    local y = 48 + floor(2 * sin(x * 0.18 + splash * 6))
    px.blend(x, y, 120, 170, 210, 0.35)
  end
end

-- --------------------------------------------------------------- numerals
-- The count, big, in the middle of the sky. Built from px.rect on a 3x5 grid so
-- it reads at four pixels a cell from across a room.
local DIG = {
  [0] = {7,5,5,5,7}, [1] = {2,6,2,2,7}, [2] = {7,1,7,4,7}, [3] = {7,1,3,1,7},
  [4] = {5,5,7,1,1}, [5] = {7,4,7,1,7}, [6] = {7,4,7,5,7}, [7] = {7,1,1,1,1},
  [8] = {7,5,7,5,7}, [9] = {7,5,7,1,7},
}

local function bigDigit(n, x, y, cell, r, g, b)
  local rows = DIG[n]
  if not rows then return end
  for ry = 1, 5 do
    local bits = rows[ry]
    for rx = 1, 3 do
      if bits % (2 ^ (4 - rx)) >= (2 ^ (3 - rx)) then
        px.rect(x + (rx - 1) * cell, y + (ry - 1) * cell, cell, cell, r, g, b, true)
      end
    end
  end
end

-- --------------------------------------------------------------- draw
local twinkle = 0

function draw()
  twinkle = twinkle + 1
  local t = px.t()
  local ph, u = phase(t)
  local now = px.now()

  -- Altitude drives the sky, and the sky drives everything that is not the
  -- vehicle. One number, derived from the phase, keeps them in step.
  local alt = 0
  if ph == "lift"   then alt = 0.05 * u
  elseif ph == "ascent" then alt = 0.05 + 0.55 * u
  elseif ph == "stage" or ph == "split" then alt = 0.62 + 0.25 * u
  elseif ph == "catch" then alt = 0.9 - 0.85 * u    -- down through the sky to the pad
  elseif ph == "orbit" or ph == "entry" then alt = 1.0
  elseif ph == "flip" then alt = 0.45
  elseif ph == "splash" then alt = 0.12 end

  local sr, sg, sb = skyAt(alt)
  px.clear(sr, sg, sb)
  stars(alt, twinkle)

  -- ---------------------------------------------------------- on the pad
  if ph == "count" or ph == "ignite" or ph == "lift" then
    local lit = (ph == "ignite") and (0.4 + 0.6 * u) or ((ph == "lift") and 1.0 or 0)
    local shake = (ph == "ignite" or ph == "lift") and twinkle or 0
    ground(shake)
    tower(1.0, lit)

    local rise = 0
    if ph == "lift" then rise = u * u * 26 end
    local base = PAD_Y - floor(rise)
    booster(64, base, 20, 0)
    ship(64, base - 20, 0, 0)

    if ph == "ignite" then
      -- hold-down: the flood spreads sideways in the trench before it lifts
      plume(64, base, 5 + 6 * u, 18 + 12 * u, 0.7 + 0.3 * u, 0)
      plume(64, base + 2, 10 * u, 6, 0.5 * u, -1)
      plume(64, base + 2, 10 * u, 6, 0.5 * u, 1)
    elseif ph == "lift" then
      plume(64, base, 16 + 14 * u, 9, 1.0, 0)
    end

    if ph == "count" then
      local secs = ceil((1.0 - u) * 10)
      if secs < 1 then secs = 1 end
      if secs > 9 then
        bigDigit(1, 46, 16, 3, 230, 90, 60)
        bigDigit(0, 60, 16, 3, 230, 90, 60)
      else
        bigDigit(secs, 55, 16, 3, 230, 90, 60)
      end
    else
      px.text(46, 16, "LIFTOFF", 255, 190, 90)
    end

  -- ---------------------------------------------------------- climbing
  elseif ph == "ascent" then
    -- the ground falls away, then is gone
    if u < 0.4 then
      local gy = PAD_Y + floor(u * 60)
      if gy < H then px.rect(0, gy, W, H - gy, 14, 12, 10, true) end
    end
    local x = 64 + floor(u * 8)          -- the downrange lean
    local base = 44 - floor(u * 6)
    booster(x, base, 20, 0)
    ship(x, base - 20, u * 0.4, 0)
    plume(x, base, 16 + 6 * sin(twinkle * 0.7), 8, 1.0, 0)

  -- ---------------------------------------------------------- hot staging
  elseif ph == "stage" then
    local x, base = 74, 38
    booster(x, base, 20, 0)
    ship(x, base - 20, 0.4, 0)
    plume(x, base, 12, 6, 0.8, 0)
    -- the ship's engines lighting between the two: this is the moment
    plume(x, base - 20, 6 + 10 * u, 5, u, 0)
    px.blend(x, base - 20, 255, 240, 200, u)

  -- ---------------------------------------------------------- they part
  elseif ph == "split" then
    local sx = 78 + floor(u * 16)
    local sy = 30 - floor(u * 14)
    ship(sx, sy, 0.5, 0)
    plume(sx, sy, 10, 4, 0.7, 0)
    -- the booster flips and comes back: it falls, and it is pointing the wrong
    -- way round for most of the way down
    local bx = 74 - floor(u * 12)
    local by = 40 + floor(u * u * 22)
    booster(bx, by, 20, u > 0.3 and 1 or 0)
    if u > 0.55 then plume(bx, by - 20, 7, 5, 0.6, 0) end     -- boostback, upward
    if u > 0.2 and u < 0.5 then
      for i = 1, 6 do                                        -- the flip's cold gas
        px.blend(bx + floor((rnd() - 0.5) * 10), by - 18 + floor(rnd() * 4),
                 200, 220, 255, 0.35)
      end
    end

  -- ---------------------------------------------------------- the catch
  elseif ph == "catch" then
    ground(0)
    local closing = min(1.0, u * 1.4)
    tower(1.0 - 0.75 * closing, 0.5 * (1 - u))
    local by = 22 + floor(u * u * 22)
    local bx = TOWER_X - 8
    booster(bx, by, 20, 0)
    plume(bx, by, 9 - 6 * u, 5, 1.0 - 0.5 * u, 0)
    if u > 0.85 then
      px.text(40, 12, "CAUGHT", 120, 255, 150)
    end

  -- ---------------------------------------------------------- orbit
  elseif ph == "orbit" then
    earth(u * 2.4)
    -- two laps: the ship crosses the frame twice, and the trail behind it is
    -- drawn explicitly because nothing fades here
    local lap = u * 2.0
    local a = (lap % 1.0) * 2.0 * pi
    local ox = 64 + floor(52 * cos(a - pi / 2))
    local oy = 26 + floor(14 * sin(a - pi / 2))
    -- the track it came along. Drawn explicitly: nothing fades on this screen,
    -- so a trail is a thing you draw, not a thing you leave behind.
    for k = 1, 14 do
      local aa = a - pi / 2 - k * 0.05
      local tx = 64 + floor(52 * cos(aa))
      local ty = 26 + floor(14 * sin(aa))
      px.blend(tx, ty, 190, 215, 255, 0.8 * (1 - k / 15))
    end
    ship(ox, oy + 5, 0, 0)
    px.text(2, 2, string.format("%02d:%02d", now.hour, now.min), 150, 170, 200)
    if lap >= 1.0 then px.text(92, 2, "ORBIT 2", 120, 160, 210) end

  -- ---------------------------------------------------------- re-entry
  elseif ph == "entry" then
    earth(2.4 + u * 0.6)
    local ex = 20 + floor(u * 70)
    local ey = 14 + floor(u * u * 22)
    -- the plasma runs back from the windward side, and it is not symmetric
    for i = 1, 26 do
      local d = rnd()
      local px_ = ex - d * (12 + 20 * u)
      local py_ = ey - d * (3 + 6 * u) + (rnd() - 0.5) * 3
      local heat = 1.0 - d
      px.blend(floor(px_), floor(py_),
               floor(255 * heat), floor(120 * heat * heat), floor(200 * heat * heat * heat),
               0.3 + 0.6 * heat)
    end
    ship(ex, ey + 5, -0.6, 0.4 + 0.6 * u)
    px.text(2, 2, "ENTRY", 255, 140, 120)

  -- ---------------------------------------------------------- flip and burn
  elseif ph == "flip" then
    gulf(0)
    local fy = 12 + floor(u * 30)
    local tilt = (u < 0.55) and -0.9 or (-0.9 + 1.8 * ((u - 0.55) / 0.45))
    ship(64, fy, tilt, 0.25 * (1 - u))
    if u > 0.5 then
      plume(64, fy, 8 + 8 * (u - 0.5), 5, (u - 0.5) * 2, 0)
    end

  -- ---------------------------------------------------------- the Gulf
  else
    gulf(u)
    local sy = 44 + floor(u * 6)
    if u < 0.35 then
      ship(64, sy, 0, 0)
      plume(64, sy, 10 - 20 * u, 6, 1.0 - 2 * u, 0)
    else
      -- the splash, then rings going out across the water
      local k = (u - 0.35) / 0.65
      for i = 1, 3 do
        local rr = floor(3 + k * 34 + i * 5)
        if rr < 70 then
          px.circle(64, 50, rr, floor(160 * (1 - k)), floor(200 * (1 - k)), floor(230 * (1 - k)), false)
        end
      end
      px.text(36, 20, "SPLASHDOWN", 190, 220, 255)
    end
  end

  -- The time, always, low and quiet - except in orbit where it moves aside.
  if ph ~= "orbit" and ph ~= "entry" then
    local s = string.format("%02d:%02d", now.hour, now.min)
    px.text(W - px.width(s) - 2, 2, s, 150, 160, 180)
  end
end

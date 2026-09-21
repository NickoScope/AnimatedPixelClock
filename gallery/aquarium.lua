-- @upload-only
-- AQUARIUM - a lit freshwater tank, and fish that notice you slowly.
--
-- Five species, each with its own silhouette and its own shading rather than a
-- recoloured lozenge: a goldfish, an angelfish, striped barbs, guppies and a
-- shoal of neons. Light falls from a lamp over the glass, plants sway, and an
-- airstone works in the corner.
--
-- The room's radar is the one ROOM RADAR draws - an Apollo MTR-1 over MQTT from
-- Home Assistant, bound into Lua as `presence`. **Every reaction is damped in
-- seconds, not frames.** Walk in and the fish take the better part of ten
-- seconds to drift your way; move fast and they scatter in under two, then take
-- half a minute to forgive you. An empty room does not switch anything off -
-- the tank slowly goes to sleep. A still person's reading jitters by
-- centimetres, so anything answering the radar frame by frame would twitch.
--
-- What a frame costs here is the number of calls into C, not the work inside
-- them: a filled rect is one call and paints 128 pixels. About 750 a frame,
-- most of it the fish, which is why this holds 15 fps.
--
-- The canvas is never cleared between frames, so every layer is repainted in
-- order and nothing needs erasing.

PERIOD = 60.0
FPS = 15

local floor, sin, sqrt = math.floor, math.sin, math.sqrt
local max, abs = math.max, math.abs
local W, H = 128, 64

-- LUA_32BITS: lua_Integer is int32, so the textbook LCG overflows and collapses.
local seed = 0x1a2b3c4d
local function rnd()
  seed = seed ~ ((seed << 13) & 0x7fffffff)
  seed = seed ~ (seed >> 17)
  seed = seed ~ ((seed << 5) & 0x7fffffff)
  return (seed & 0x7fffffff) / 2147483647.0
end

-- ── water and light ─────────────────────────────────────────────────────────
-- Lit from above, so the gradient is bright under the lamp and loses it with
-- depth. Freshwater green-blue, not reef blue.
local SURF = { 46, 140, 152 }
local MID  = { 16,  78, 104 }
local DEEP = {  6,  34,  56 }
local FLOOR_Y = 53

local function water(y, lift)
  local r, g, b
  if y < 20 then
    local k = y / 20
    r = SURF[1] + (MID[1] - SURF[1]) * k
    g = SURF[2] + (MID[2] - SURF[2]) * k
    b = SURF[3] + (MID[3] - SURF[3]) * k
  else
    local k = (y - 20) / 33
    if k > 1 then k = 1 end
    r = MID[1] + (DEEP[1] - MID[1]) * k
    g = MID[2] + (DEEP[2] - MID[2]) * k
    b = MID[3] + (DEEP[3] - MID[3]) * k
  end
  return r * lift, g * lift, b * lift
end

-- ── the species ─────────────────────────────────────────────────────────────
-- shape: 1 fusiform (barb, neon, guppy), 2 deep-bodied (goldfish),
--        3 disc (angelfish - taller than it is long, which is the whole look).
-- back / mid / belly are the vertical shading; bars are vertical stripes as
-- fractions along the body.
local SPECIES = {
  gold = {
    len = 16, tall = 11, shape = 2, speed = 0.62,
    back = { 236,  74,   0 }, mid = { 255, 150,  16 }, belly = { 255, 226, 150 },
    fin  = { 255, 176,  48 }, tail = "veil", sheen = { 255, 236, 190 },
  },
  angel = {
    len = 12, tall = 13, shape = 3, speed = 0.5,
    back = { 196, 200, 210 }, mid = { 240, 238, 228 }, belly = { 255, 250, 226 },
    fin  = { 255, 226, 120 }, tail = "fork", bars = { 0.18, 0.52, 0.86 },
    barcol = { 26, 28, 40 }, filament = true, sheen = { 255, 244, 190 },
  },
  -- The tiger barb: amber gold, four black bars, red fins. Pale gold made it
  -- a minnow; this is the fish the name means.
  barb = {
    len = 11, tall = 7, shape = 1, speed = 1.0,
    back = { 236, 132,  16 }, mid = { 255, 190,  52 }, belly = { 255, 236, 176 },
    fin  = { 255,  62,  40 }, tail = "fork", bars = { 0.2, 0.46, 0.72, 0.95 },
    barcol = { 22, 20, 26 }, sheen = { 255, 224, 130 },
  },
  guppy = {
    len = 6, tall = 4, shape = 1, speed = 1.25,
    back = { 108, 110, 230 }, mid = { 90, 226, 236 }, belly = { 226, 250, 250 },
    fin  = { 255, 92, 180 }, tail = "veil",
  },
  neon = {
    len = 6, tall = 3, shape = 1, speed = 1.35,
    back = { 22, 56, 110 }, mid = { 40, 214, 255 }, belly = { 244, 44, 52 },
    fin  = { 180, 220, 245 }, tail = "fork",
  },
}

-- Guppies are the one kind that are all different; that is what a tank of them
-- looks like. Each gets its own tail.
local GUPPY_FIN = { { 255, 84, 186 }, { 255, 190, 40 }, { 90, 255, 150 } }

-- Half-height along the body, nose (0) to wrist (len-1). Built once at load.
local function profile(sp)
  local p, n = {}, sp.len
  for i = 0, n - 1 do
    local u = i / (n - 1)
    local v
    if sp.shape == 3 then
      -- A disc: rises almost at once, holds, then falls to a short wrist.
      v = u < 0.3 and sqrt(u / 0.3) or (u < 0.62 and 1 or (1 - (u - 0.62) / 0.38) * 0.95)
    elseif sp.shape == 2 then
      v = u < 0.4 and sqrt(u / 0.4) or (1 - (u - 0.4) / 0.6) * (1 - 0.35 * (u - 0.4) / 0.6)
    else
      v = u < 0.34 and sqrt(u / 0.34) or (1 - (u - 0.34) / 0.66) * (1 - 0.55 * (u - 0.34) / 0.66)
    end
    if v < 0 then v = 0 end
    p[i] = sp.tall * 0.5 * v
  end
  return p
end
for _, sp in pairs(SPECIES) do sp.prof = profile(sp) end

-- ── the inhabitants ─────────────────────────────────────────────────────────
local FISH = {}
local function spawn(name, n)
  local sp = SPECIES[name]
  for i = 1, n do
    FISH[#FISH + 1] = {
      sp = sp, name = name,
      x = 20 + rnd() * (W - 40), y = 16 + rnd() * 22,
      vx = (rnd() < 0.5 and -1 or 1) * 3, vy = 0,
      ph = rnd() * 6.28, wob = 0.4 + rnd() * 0.9,
      -- A beat of its own, well inside the glass: centre, reach, and phase.
      wx = 32 + rnd() * (W - 64), wy = 16 + rnd() * 20,
      rx = 16 + rnd() * 20, tw = rnd() * 6.28,
      var = GUPPY_FIN[(i - 1) % 3 + 1],
    }
  end
end
-- Drawn in this order, so the big two swim in front of the small fry.
spawn("neon", 6)
spawn("guppy", 3)
spawn("barb", 3)
spawn("angel", 1)
spawn("gold", 1)

-- ── plants ──────────────────────────────────────────────────────────────────
local KELP = {}
for i = 1, 7 do
  KELP[i] = { x = 4 + (i - 1) * 19 + rnd() * 8, h = 18 + rnd() * 20,
              ph = rnd() * 6.28, sway = 0.7 + rnd() * 0.9,
              hue = rnd() < 0.4 and 1 or 0 }
end

-- ── the airstone, in the corner ─────────────────────────────────────────────
-- Bubbles come off the sand and nowhere else. One starting in open water is
-- the tell of a drawn tank.
local STONE_X = 16
local BUB = {}
for i = 1, 18 do
  BUB[i] = { y = 54 + rnd() * 40, ox = (rnd() - 0.5) * 3,
             v = 9 + rnd() * 9, r = rnd() < 0.3 and 1 or 0, ph = rnd() * 6.28 }
end

-- ── the room ────────────────────────────────────────────────────────────────
local RAD = rawget(_G, "presence")
local RANGE = RAD and (RAD.scale() * 1000) or 4000

local focus, haveF, near, startle, awake = 0.5, 0, 0, 0, 1

local function lp(v, to, dt, tau)
  local k = dt / tau
  if k > 1 then k = 1 end
  return v + (to - v) * k
end

local function read_room(dt)
  local fx, fy, fastest = 0, 0, 0
  local n = RAD.count()
  local best = 1e9
  for i = 1, 3 do
    local x, y, v = RAD.target(i)
    if x then
      if v and abs(v) > fastest then fastest = abs(v) end
      if y < best then best, fx, fy = y, x, y end
    end
  end

  if n > 0 then
    -- x is lateral about the sensor, y the distance out from it, both in mm.
    local tx = 0.5 + (fx / RANGE)
    if tx < 0 then tx = 0 elseif tx > 1 then tx = 1 end
    local p = 1 - (fy / RANGE)
    if p < 0 then p = 0 elseif p > 1 then p = 1 end
    focus = lp(focus, tx, dt, 6.0)
    haveF = lp(haveF, 1, dt, 3.0)
    near = lp(near, p, dt, 8.0)
    awake = lp(awake, 1, dt, 12.0)
  else
    haveF = lp(haveF, 0, dt, 9.0)
    near = lp(near, 0, dt, 12.0)
    awake = lp(awake, 0, dt, 45.0)
  end

  -- The one fast edge, and even it is not instant: about a second and a half to
  -- take fright, half a minute to settle. 40 cm/s is a walk, not a fidget, and
  -- a still person's jitter sits well below it.
  startle = fastest > 40 and lp(startle, 1, dt, 1.5) or lp(startle, 0, dt, 25.0)
end

local function story(dt, t)
  focus = 0.5 + 0.34 * sin(t * 0.21)
  haveF = 0.45 + 0.35 * sin(t * 0.07 + 1.0)
  near = 0.35 + 0.3 * sin(t * 0.05)
  awake = 1
  startle = max(0, startle - dt * 0.05)
end

-- ── drawing ─────────────────────────────────────────────────────────────────
local pixel, line, rect, circle, text = px.pixel, px.line, px.rect, px.circle, px.text

local function draw_water(t, lift)
  for i = 0, 31 do
    local y = i * 2
    local r, g, b = water(y, lift)
    rect(0, y, W, 2, floor(r), floor(g), floor(b), true)
  end
end

local function draw_lamp(t, lift)
  -- The lamp over the glass, and the hot band right under it.
  rect(0, 0, W, 1, floor(36 * lift), floor(38 * lift), floor(42 * lift), true)
  for i = 0, 7 do
    local x = 6 + i * 16
    local k = lift * (0.8 + 0.2 * sin(t * 0.6 + i))
    rect(x, 0, 10, 1, floor(220 * k), floor(232 * k), floor(200 * k), true)
  end
  rect(0, 1, W, 2, floor(90 * lift), floor(170 * lift), floor(170 * lift), true)
end

local function draw_shafts(t, lift)
  -- Light falling from the lamp, leaning as the surface moves. Sixteen
  -- widening bands a shaft rather than a per-pixel gradient.
  for s = 1, 4 do
    local base = 12 + (s - 1) * 33 + 9 * sin(t * 0.13 + s)
    local lean = 0.4 + 0.25 * sin(t * 0.11 + s * 2.1)
    for b = 0, 15 do
      local y = 3 + b * 3
      local w = 3 + b * 0.6
      local x = base + y * lean - w * 0.5
      local k = (1 - b / 16) * 0.55 * lift * (0.6 + 0.4 * sin(t * 0.3 + s))
      local r, g, bl = water(y, lift)
      rect(floor(x), y, floor(w), 3,
           floor(r + 46 * k), floor(g + 72 * k), floor(bl + 58 * k), true)
    end
  end
end

local function draw_caustics(t, lift)
  -- Two sines of different wavelength crossing, so the net drifts and never
  -- repeats on the eye. Brightness dies with depth, because light does.
  for c = 0, 12 do
    local y0 = 4 + c * 4
    local fade = 1 - y0 / 56
    if fade > 0 then
      local amp = 1.5 + 1.4 * fade
      local px0, py0
      for i = 0, 10 do
        local x = i * 12.8
        local y = y0 + amp * sin(x * 0.11 + t * 0.9 + c * 0.7)
                     + amp * 0.6 * sin(x * 0.047 - t * 0.55 + c * 1.9)
        local yi = floor(y)
        if px0 then
          local k = fade * fade * lift * (0.55 + 0.45 * sin(t * 1.3 + c + i * 0.4))
          local wr, wg, wb = water(yi, lift)
          line(px0, py0, floor(x), yi,
               floor(wr + 110 * k), floor(wg + 140 * k), floor(wb + 110 * k))
        end
        px0, py0 = floor(x), yi
      end
    end
  end
end

local function draw_surface(t, lift)
  -- Bright dashes sliding along the waterline. Nothing here may be darker than
  -- the water: near the surface a dark mark reads as a hole, not a shadow.
  for i = 0, 15 do
    local x = i * 8 + floor(5 * sin(t * 0.7 + i))
    local y = 3 + floor(1.4 * sin(t * 1.5 + i * 0.9))
    local k = lift * (0.55 + 0.45 * sin(t * 2.1 + i))
    local len = 3 + floor(3 * abs(sin(t * 0.9 + i * 1.7)))
    line(x, y, x + len, y, floor(90 + 130 * k), floor(180 + 60 * k), floor(190 + 50 * k))
  end
end

-- The sand is not a bar of brown: its top edge dips and rises, it pales toward
-- the light, and stones sit on it. A ruled horizon is what made an early
-- version read as a graphic rather than a tank.
local DUNE = {}
for x = 0, W - 1 do
  DUNE[x] = floor(FLOOR_Y + 1.7 * sin(x * 0.07) + 1.1 * sin(x * 0.19 + 2.0)
                  + 0.7 * sin(x * 0.41 + 1.0))
end

local STONE = {}
do
  local s = 0x5f3a91
  for i = 1, 5 do
    s = (s * 1103515 + 12345) & 0x7fffffff
    local x = 10 + s % (W - 20)
    STONE[i] = { x = x, y = DUNE[x] + 1, r = 1 + (s >> 11) % 2 }
  end
end

local function draw_sand(t, lift)
  for x = 0, W - 1 do
    local top = DUNE[x]
    line(x, top, x, top + 2, floor(122 * lift), floor(102 * lift), floor(70 * lift))
    if top + 3 <= H - 1 then
      line(x, top + 3, x, H - 1, floor(58 * lift), floor(46 * lift), floor(32 * lift))
    end
  end
  for i = 1, 5 do
    local st = STONE[i]
    circle(st.x, st.y, st.r, floor(92 * lift), floor(84 * lift), floor(72 * lift), true)
    pixel(st.x - st.r, st.y - st.r, floor(132 * lift), floor(120 * lift), floor(102 * lift))
  end
  local s = 0x2ab517
  for i = 1, 16 do
    s = (s * 1103515 + 12345) & 0x7fffffff
    local x = s % W
    local y = DUNE[x] + 1 + (s >> 9) % 5
    if y <= H - 1 then
      local k = lift * (0.6 + 0.4 * sin(t * 0.8 + i))
      pixel(x, y, floor(150 * k), floor(128 * k), floor(96 * k))
    end
  end
end

local function draw_kelp(t, lift, calm)
  for i = 1, 7 do
    local k = KELP[i]
    local root = DUNE[floor(k.x) % W]
    local x, y = k.x, root
    local segs = floor(k.h / 2)
    for s = 1, segs do
      -- The sway grows toward the tip and slows right down when the tank is
      -- asleep. Fronds that thrash in an empty room look like a screensaver.
      local f = s / segs
      local nx = k.x + k.sway * f * f * 8 * sin(t * (0.45 + 0.3 * calm) + k.ph + f * 2.2)
      local ny = root - s * 2
      local g = floor((46 + 82 * f) * lift)
      local r = floor(((k.hue > 0 and 40 or 8) + 22 * f) * lift)
      local b = floor((26 + 30 * f) * lift)
      line(floor(x), floor(y), floor(nx), floor(ny), r, g, b)
      -- One line of weed is a scratch; two are a plant.
      if f < 0.78 then
        line(floor(x) + 1, floor(y), floor(nx) + 1, floor(ny),
             floor(r * 0.68), floor(g * 0.7), floor(b * 0.68))
      end
      x, y = nx, ny
    end
  end
end

local function draw_bubbles(t, dt, lift)
  -- Off the airstone, wobbling as they climb, gone at the surface.
  local root = DUNE[STONE_X]
  circle(STONE_X, root, 2, floor(60 * lift), floor(66 * lift), floor(70 * lift), true)
  for i = 1, 18 do
    local b = BUB[i]
    b.y = b.y - b.v * dt * (0.6 + 0.4 * awake)
    if b.y < 5 then
      b.y = root - 1
      b.ox = (rnd() - 0.5) * 3
      b.v = 9 + rnd() * 9
    end
    -- The wobble widens as it rises, the way a real one does.
    local climb = (root - b.y) / root
    local x = floor(STONE_X + b.ox + (1.5 + 3.5 * climb) * sin(t * 2.2 + b.ph))
    local y = floor(b.y)
    local k = lift * (0.7 + 0.3 * sin(t * 3 + i))
    if b.r > 0 and climb > 0.3 then
      circle(x, y, 1, floor(170 * k), floor(225 * k), floor(230 * k), false)
      pixel(x, y - 1, floor(230 * k), floor(250 * k), floor(255 * k))
    else
      pixel(x, y, floor(190 * k), floor(232 * k), floor(240 * k))
    end
  end
end

-- One fish. Three vertical segments a column - back, flank, belly - which is
-- what makes a body look round instead of flat, and what the palette is for.
local function draw_fish(f, t, dt, lift, fx, pull, push)
  local sp = f.sp
  local prof, len = sp.prof, sp.len

  f.tw = f.tw + dt * 0.35 * f.wob
  local wx = f.wx + f.rx * sin(f.tw)
  local wy = f.wy + 8 * sin(f.tw * 0.7 + 1.3)

  -- **The room nudges a heading; it never tows a fish.** An earlier version
  -- made the person's position the target and the whole tank slid after them
  -- like iron filings - which is a window on the sea, not an aquarium. The
  -- fish lives its own path; presence bends it by a fraction and that is all.
  local tx = wx + (fx - wx) * pull * 0.18
  local ty = wy - 3 * pull
  if push > 0.02 then
    -- Startled, it turns away and puts on speed - inside its own tank, still
    -- swimming its own route, just the other way along it.
    local away = (f.x < fx) and -1 or 1
    tx = wx + away * 22 * push
    ty = ty + 8 * push
  end

  local speed = (0.5 + 0.5 * awake) * (1 + 2.2 * push) * sp.speed
  f.vx = (f.vx + (tx - f.x) * 0.55 * dt * speed) * 0.94
  f.vy = (f.vy + (ty - f.y) * 0.5 * dt * speed) * 0.90
  local vmax = 24 * speed
  if f.vx > vmax then f.vx = vmax elseif f.vx < -vmax then f.vx = -vmax end
  if f.vy > vmax * 0.6 then f.vy = vmax * 0.6 elseif f.vy < -vmax * 0.6 then f.vy = -vmax * 0.6 end
  f.x = f.x + f.vx * dt
  f.y = f.y + f.vy * dt

  -- The glass. All four walls, and the fish turns at them rather than leaving:
  -- this is a tank, and nothing in it is ever off screen. The margin is the
  -- fish's own half-length plus its tail, so no fin is ever clipped either.
  local half = floor(sp.tall / 2) + 2
  local side = floor(len / 2) + (sp.tail == "veil" and 7 or 5)
  if f.x < side then f.x, f.vx = side, abs(f.vx)
  elseif f.x > W - side then f.x, f.vx = W - side, -abs(f.vx) end
  local top = half + (sp.filament and 8 or 5)
  if f.y < top then f.y, f.vy = top, abs(f.vy) end
  local bed = DUNE[floor(f.x)] - half - (sp.filament and 7 or 1)
  if f.y > bed then f.y, f.vy = bed, -abs(f.vy) end

  local dir = f.vx >= 0 and 1 or -1
  f.ph = f.ph + dt * (4 + 9 * push + 5 * abs(f.vx) / 24)

  -- The fish keep their colour. Depth and the lamp still touch them, but only
  -- a little: a decorative tank's fish are the brightest thing in it, and
  -- multiplying them by the water's own dimming made them go grey with it.
  local br = (0.78 + 0.22 * lift) * (0.88 + 0.12 * (1 - f.y / 58))
  if br > 1.12 then br = 1.12 end
  local fin = f.name == "guppy" and f.var or sp.fin
  local bk, md, bl = sp.back, sp.mid, sp.belly
  local fr, fg, fb = floor(fin[1] * br), floor(fin[2] * br), floor(fin[3] * br)

  local bx, by = floor(f.x), floor(f.y)
  for c = 0, len - 1 do
    local hh = prof[c]
    if hh >= 0.5 then
      -- The wiggle grows toward the tail; a fish that waggles its face reads as
      -- wrong without being nameable.
      local wig = 0.9 * sin(f.ph - c * 0.55) * (c / len)
      local x = bx + dir * (floor(len / 2) - c)
      local yc = by + floor(wig)
      local h = floor(hh)
      local bar = false
      if sp.bars then
        local u = c / (len - 1)
        for j = 1, #sp.bars do
          if abs(u - sp.bars[j]) < 0.055 then bar = true end
        end
      end
      if bar then
        local bc = sp.barcol
        line(x, yc - h, x, yc + h, floor(bc[1] * br), floor(bc[2] * br), floor(bc[3] * br))
      elseif h >= 2 then
        local t1 = floor(h * 0.45)
        line(x, yc - h, x, yc - t1, floor(bk[1] * br), floor(bk[2] * br), floor(bk[3] * br))
        line(x, yc - t1, x, yc + t1, floor(md[1] * br), floor(md[2] * br), floor(md[3] * br))
        line(x, yc + t1, x, yc + h, floor(bl[1] * br), floor(bl[2] * br), floor(bl[3] * br))
      else
        line(x, yc - h, x, yc + h, floor(md[1] * br), floor(md[2] * br), floor(md[3] * br))
      end
    end
  end

  -- Where the lamp catches the back. One line, and it is what turns a flat
  -- shape into something wet.
  if sp.sheen then
    local sh = sp.sheen
    local sr, sg, sb = floor(sh[1] * br), floor(sh[2] * br), floor(sh[3] * br)
    local c0, c1 = floor(len * 0.3), floor(len * 0.72)
    for c = c0, c1 do
      local hh = prof[c]
      if hh >= 1.5 then
        local wig = 0.9 * sin(f.ph - c * 0.55) * (c / len)
        pixel(bx + dir * (floor(len / 2) - c), by + floor(wig) - floor(hh) + 1, sr, sg, sb)
      end
    end
  end

  -- The tail, starting where the body actually ends this frame, wiggle
  -- included: detached, it reads as a flag being towed.
  local tb = floor(2.2 * sin(f.ph - 0.55 * (len - 1)))
  local tailx = bx - dir * floor(len / 2)
  local taily = by + floor(0.9 * sin(f.ph - (len - 1) * 0.55))
  if sp.tail == "veil" then
    -- A goldfish's tail is longer than it is deep, and it trails.
    local tl = f.name == "gold" and 6 or 3
    local grow = f.name == "gold" and 0.42 or 0.3
    for s = 1, tl do
      local spread = 0.6 + (sp.tall * grow) * s / tl
      local yb = taily + tb * s / tl
      -- The trailing edge catches the lamp; a flat wedge reads as cardboard.
      local e = s == tl and 1.35 or 1.0
      line(tailx - dir * s, floor(yb - spread), tailx - dir * s, floor(yb + spread),
           floor(fr * e > 255 and 255 or fr * e),
           floor(fg * e > 255 and 255 or fg * e),
           floor(fb * e > 255 and 255 or fb * e))
    end
  else
    for s = 0, 3 do
      local spread = (sp.tall * 0.3) * s / 3
      local yb = taily + tb * s / 3
      line(tailx - dir * s, floor(yb - spread), tailx - dir * s, floor(yb + spread), fr, fg, fb)
    end
  end

  -- Dorsal and anal fins. On the angelfish they are most of the animal, and the
  -- two filaments trailing off them are what makes it a scalare.
  local wide = floor(len * (sp.shape == 3 and 0.45 or 0.34))
  local dh = floor(prof[wide])
  local dx = bx + dir * (floor(len / 2) - wide)
  if sp.shape == 3 then
    -- Swept back from the shoulder, tallest behind the widest point, so the
    -- outline is a triangle rather than a fringe on a box.
    for s = 0, 4 do
      local rise = 5 - s
      line(dx - dir * s, by - dh - 1, dx - dir * (s + 1), by - dh - 1 - rise, fr, fg, fb)
      line(dx - dir * s, by + dh + 1, dx - dir * (s + 1), by + dh + 1 + floor(rise * 0.7), fr, fg, fb)
    end
    if sp.filament then
      -- The two trailing threads. Without them it is a disc; with them it is
      -- a scalare.
      line(dx - dir * 5, by - dh - 6, dx - dir * 9, by - dh - 9, fr, fg, fb)
      line(dx - dir * 5, by + dh + 4, dx - dir * 9, by + dh + 8, fr, fg, fb)
    end
  elseif sp.tall >= 6 then
    line(dx, by - dh - 1, dx - dir * 2, by - dh - 3, fr, fg, fb)
    line(dx, by + dh + 1, dx - dir * 2, by + dh + 2, fr, fg, fb)
  end

  -- A neon's stripe is the fish: it runs the flank from the eye to the tail,
  -- over the body already drawn.
  if f.name == "neon" then
    line(bx + dir * (floor(len / 2) - 1), by - 1, tailx, by - 1,
         floor(60 * br), floor(220 * br), floor(250 * br))
  end

  -- An eye. Two pixels are the difference between a fish and a leaf.
  local ex = bx + dir * (floor(len / 2) - 1)
  local ey = by - (sp.shape == 3 and 2 or 1)
  pixel(ex, ey, floor(240 * br), floor(242 * br), floor(246 * br))
  if sp.tall >= 5 then pixel(ex + dir, ey, floor(18 * br), floor(20 * br), floor(26 * br)) end
end

-- ── the frame ───────────────────────────────────────────────────────────────
local tprev = nil

function draw()
  local t = px.t() * PERIOD
  -- Real seconds between frames, from the only clock the firmware gives us.
  -- Every damping constant above is in seconds, so the tank behaves the same at
  -- 15 fps and at 4.
  local dt
  if tprev then
    dt = t - tprev
    if dt < 0 then dt = dt + PERIOD end
    if dt > 0.5 then dt = 0.5 end
  else
    dt = 1 / FPS
  end
  tprev = t

  if RAD and RAD.state() == 1 then read_room(dt) else story(dt, t) end

  -- An empty room dims the lamp rather than switching it off.
  local lift = 0.45 + 0.55 * awake + 0.1 * startle
  if lift > 1.12 then lift = 1.12 end

  draw_water(t, lift)
  draw_shafts(t, lift)
  draw_caustics(t, lift)
  draw_lamp(t, lift)
  draw_surface(t, lift)
  draw_sand(t, lift)
  draw_kelp(t, lift, 1 - startle)

  local fx = focus * W
  local pull = haveF * near * (1 - startle)
  for i = 1, #FISH do draw_fish(FISH[i], t, dt, lift, fx, pull, startle) end
  draw_bubbles(t, dt, lift)

  -- The time, down on the sand where white has something to sit on. Over the
  -- water with a four-copy outline it closed every counter in the digits.
  local now = px.now()
  local s = string.format("%02d:%02d", now.hour, now.min)
  local tx, ty = W - px.width(s) - 3, H - 8
  text(tx + 1, ty + 1, s, 0, 0, 0)
  text(tx, ty, s, 240, 246, 250)
end

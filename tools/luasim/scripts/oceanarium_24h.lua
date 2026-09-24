-- @upload-only
-- OCEANARIUM 24H - the same tank with its day run in five minutes, to watch the light change.
DEMO_DAY = 300
-- OCEANARIUM - a window into a great public aquarium, a hundred-odd kinds of
-- sea life living their own lives, lit by the time of day.
--
-- The screen is the glass of one big tank, tens of metres deep. What lives in
-- it is not a scene that plays: every animal is its own agent, arriving and
-- leaving on its own, looking for food, keeping to its own part of the tank -
-- the reef, the open water, the sand, the surface - and minding the others. A
-- shark cruising through makes the small fish scatter and the bait ball open
-- round it. The big ones come out of the far blue as shadows first, and take
-- their colour as they come nearer, because distance under water is haze.
--
-- **Depth.** Everything has a depth, from the far haze (0) to the glass (1).
-- Depth sets its size, how much water tints it, how fast it seems to move,
-- where the sand is under it, and the order things are drawn in.
--
-- **Light.** The tank follows the day at the panel's clock: blue noon with
-- sun shafts and caustics walking on the sand, a warm violet dusk, a dark
-- night of moonlight, glowing jellies and plankton sparking where a fish
-- darts. At night the tank lights (the backlight) come on when somebody comes
-- into the room and go out when it has been empty for a minute; a click of the
-- knob or the remote's OK on this page switches them by hand (px.button,
-- firmware 2.7.3), until the next sunrise or sunset.
--
-- **The room.** The Apollo MTR-1 radar that ROOM RADAR draws, bound into Lua
-- as `presence`. The curious ones - a grouper, a Napoleon wrasse, a batfish,
-- a turtle, the puffer, the octopus - come to the glass where you stand and
-- look at you. A fast movement scatters the small fish into the reef, puffs
-- up the pufferfish and pulls the garden eels into the sand. Without the
-- radar the tank lives on its own.
--
-- **How it is drawn (firmware 2.7.1+).** Each animal is drawn once per pose
-- - its size, its turn, its beat of the tail - at full light, cut out with
-- px.grab and from then on stamped with px.blit: mirrored when it swims the
-- other way, dimmed by the light at its depth, and hazed toward the water
-- colour by the distance. The reef is cut out the same way when the tank
-- opens, so relighting it for the time of day is a few stamps, not a redraw.
-- Poses are cut as the animals first need them, a few a frame, and forgotten
-- when nothing has worn them for a while.

PERIOD = 600.0
FPS = 15

local floor, sin, cos, sqrt = math.floor, math.sin, math.cos, math.sqrt
local abs, max, min, exp, log = math.abs, math.max, math.min, math.exp, math.log
local atan = math.atan
local pi = math.pi
local TAU = 2 * pi
local W, H = 128, 64
local pixel, line, rect, circle, text = px.pixel, px.line, px.rect, px.circle, px.text
local blit, grab = px.blit, px.grab
local READY = blit ~= nil and grab ~= nil and px.save ~= nil

-- LUA_32BITS: lua_Integer is int32, so the textbook LCG overflows and collapses.
local seed = 0x5eaf00d
local function rnd()
  seed = seed ~ ((seed << 13) & 0x7fffffff)
  seed = seed ~ (seed >> 17)
  seed = seed ~ ((seed << 5) & 0x7fffffff)
  return (seed & 0x7fffffff) / 2147483647.0
end
local function rr(a, b) return a + (b - a) * rnd() end
local function pick(t) return t[1 + floor(rnd() * #t) % #t] end

local function clamp(v, a, b) if v < a then return a elseif v > b then return b end return v end
local function mix(a, b, k) return a + (b - a) * k end
local function smooth(a, b, v)
  local k = clamp((v - a) / (b - a), 0, 1)
  return k * k * (3 - 2 * k)
end
local function lp(v, to, dt, tau)
  local k = dt / tau
  if k > 1 then k = 1 end
  return v + (to - v) * k
end
-- A colour {r,g,b} scaled, as integers the px calls take.
local function C(c, k)
  k = k or 1
  local r, g, b = c[1] * k, c[2] * k, c[3] * k
  return floor(r > 255 and 255 or r), floor(g > 255 and 255 or g), floor(b > 255 and 255 or b)
end
local function cmix(a, b, k)
  return { a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k, a[3] + (b[3] - a[3]) * k }
end
-- Pure black is transparent to px.grab, so anything meant to be black in an
-- animal is this near-black instead.
local INK = { 14, 14, 18 }

-- A hash of two integers to 0..1, for spots and mottling that stay put on a
-- body however often the body is redrawn.
local function hash2(x, y)
  local h = (x * 374761393 + y * 668265263) & 0x7fffffff
  h = ((h ~ (h >> 13)) * 1274126177) & 0x7fffffff
  return (h ~ (h >> 16)) / 2147483647.0
end

-- ── the tank ────────────────────────────────────────────────────────────────
-- Depth z: 0 is the far haze, 1 the glass. The sand rises toward the back,
-- which is most of what makes a flat screen read as a deep tank.
local function scale_at(z) return 0.26 + 0.74 * z end
local function floor_y(z) return 43 + 19 * z end

-- ── light ───────────────────────────────────────────────────────────────────
-- The day at the panel's clock. The sun is a simple model, our choice and not
-- an ephemeris: a day that is 8.5 h at midwinter and 15.5 h at midsummer (at
-- about 48 degrees north it is 8.2-16.2 h), centred on 13:15, which is roughly
-- solar noon in Central European summer time.
local LIGHT = {
  day = 1, warm = 0, night = 0, lamp = 0, lampWant = 0,
  top = { 30, 150, 200 }, deep = { 4, 40, 86 }, lum = 1,
}

local DAY_TOP, DAY_DEEP = { 34, 150, 196 }, { 5, 42, 86 }
local WARM_TOP, WARM_DEEP = { 112, 84, 132 }, { 14, 22, 62 }
local NIGHT_TOP, NIGHT_DEEP = { 16, 38, 80 }, { 4, 12, 30 }
local LAMP_TOP, LAMP_DEEP = { 60, 168, 222 }, { 10, 62, 112 }

-- The tank's clock. Normally the panel's; in the demo (two quick clicks, or
-- DEMO_DAY set by a copy of the script) the light runs a whole day in five
-- minutes while the animals keep their own pace. Two clicks again stop the
-- run where it is; three bring the tank back to the time it really is. The
-- time shown is the tank's.
local CLK = { rate = 1, off = 0 }            -- off: hours the tank is ahead of the panel
CLK.demo = 24 * 3600 / (rawget(_G, "DEMO_DAY") or 300)
if rawget(_G, "DEMO_DAY") then CLK.rate = CLK.demo end
local function clock_h()
  local now = px.now()
  local h = now.hour + now.min / 60 + (now.sec or 0) / 3600
  return (h + CLK.off) % 24, now
end
local function clock_run(dt)
  if CLK.rate ~= 1 then CLK.off = (CLK.off + dt * (CLK.rate - 1) / 3600) % 24 end
end

local function sun_now()
  local h, now = clock_h()
  local yday = now.yday or 172
  local dl = 12 + 3.5 * cos(TAU * (yday - 172) / 365)
  local x = (h - 13.25) / (dl / 2)
  if x > 2 then x = 2 elseif x < -2 then x = -2 end
  return cos(x * pi / 2), now            -- 1 at noon, 0 at sunrise and sunset, -1 deep night
end

local function light_update(dt, first)
  local e = sun_now()
  local day = smooth(-0.10, 0.28, e)
  local warm = exp(-((e - 0.05) / 0.16) ^ 2)          -- dawn and dusk
  local L = LIGHT
  if first then L.day, L.warm = day, warm
  else
    local tau = CLK.rate > 1 and 0.8 or 8
    L.day = lp(L.day, day, dt, tau)
    L.warm = lp(L.warm, warm, dt, tau)
  end
  L.lamp = first and L.lampWant or lp(L.lamp, L.lampWant, dt, 2.5)
  local top = cmix(NIGHT_TOP, DAY_TOP, L.day)
  local deep = cmix(NIGHT_DEEP, DAY_DEEP, L.day)
  top = cmix(top, WARM_TOP, L.warm * 0.55)
  deep = cmix(deep, WARM_DEEP, L.warm * 0.3)
  -- The tank's own lights only add: at noon they change nothing.
  local k = L.lamp * (1 - 0.55 * L.day)
  for i = 1, 3 do
    top[i] = mix(top[i], max(top[i], LAMP_TOP[i]), k)
    deep[i] = mix(deep[i], max(deep[i], LAMP_DEEP[i]), k)
  end
  L.top, L.deep = top, deep
  L.night = (1 - L.day) * (1 - k)
  L.lum = max(0.32, max(L.day * (1 - 0.25 * L.warm), k * 0.95))
end

-- The water's colour and the light at each row of the screen, as tables
-- remade only when the light has moved a step (light_tables).
local WR, WG, WB, LUMY = {}, {}, {}, {}
local lightKey = nil
local function light_tables()
  local tp, dp = LIGHT.top, LIGHT.deep
  local key = ((floor(tp[1] / 3) * 86 + floor(tp[2] / 3)) * 86 + floor(tp[3] / 3)) * 86 + floor(dp[3] / 3)
              + floor(LIGHT.lum * 20) * 54700816
  if key == lightKey then return false end
  lightKey = key
  for y = 0, 63 do
    local k = (y / 63) ^ 0.8
    WR[y] = tp[1] + (dp[1] - tp[1]) * k
    WG[y] = tp[2] + (dp[2] - tp[2]) * k
    WB[y] = tp[3] + (dp[3] - tp[3]) * k
    LUMY[y] = LIGHT.lum * (1.08 - 0.42 * y / 63)
  end
  return true
end
local function water(y)
  y = floor(y)
  if y < 0 then y = 0 elseif y > 63 then y = 63 end
  return WR[y], WG[y], WB[y]
end
-- How lit something is at a row: light falls off with depth.
local function lum_at(y)
  y = floor(y)
  if y < 0 then y = 0 elseif y > 63 then y = 63 end
  return LUMY[y]
end

-- ── the room ────────────────────────────────────────────────────────────────
local RAD = rawget(_G, "presence")
local RANGE = RAD and (RAD.scale() * 1000) or 4000
local ROOM = { focus = 0.5, haveF = 0, near = 0, startle = 0, awake = 1, fast = 0 }

local function read_room(dt)
  local R = ROOM
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
    local tx = clamp(0.5 + fx / RANGE, 0, 1)
    local p = clamp(1 - fy / RANGE, 0, 1)
    R.focus = lp(R.focus, tx, dt, 5.0)
    R.haveF = lp(R.haveF, 1, dt, 3.0)
    R.near = lp(R.near, p, dt, 6.0)
    R.awake = lp(R.awake, 1, dt, 8.0)
  else
    R.haveF = lp(R.haveF, 0, dt, 9.0)
    R.near = lp(R.near, 0, dt, 12.0)
    R.awake = lp(R.awake, 0, dt, 60.0)
  end
  -- 40 cm/s is a walk, not a fidget: a still person's jitter sits below it.
  R.fast = fastest
  R.startle = fastest > 40 and lp(R.startle, 1, dt, 1.2) or lp(R.startle, 0, dt, 20.0)
end

-- Without the radar the tank is left to itself: nobody at the glass, lights
-- by the clock.
local function no_room(dt)
  local R = ROOM
  R.haveF = lp(R.haveF, 0, dt, 5)
  R.near = lp(R.near, 0, dt, 5)
  R.startle = lp(R.startle, 0, dt, 10)
  R.awake = 1
end

-- The knob's click or the remote's OK on this page (px.button, firmware
-- 2.7.3+), counted over 0.35 s:
--   one    the tank lights on or off, by hand, until the next sunrise or
--          sunset; then the tank's own rule again (at night on while somebody
--          is in the room)
--   two    the demo: a day in five minutes; two again stop it where it is
--   three  back to the time it really is
-- One click waits out the window before it acts: it cannot know sooner that
-- no second one is coming.
-- hand: nil automatic, true/false by hand; handDay: whether it was day when
-- the hand chose; last: the count last seen; n, at: clicks in the window.
local IN = { hand = nil, handDay = nil, last = nil, n = 0, at = 0, said = nil, till = -1 }
local MULTI = 0.35                          -- the owner's choice, 2026-09-24

local function press(n, T)
  local s
  if n == 1 then
    if IN.hand == nil then IN.hand = LIGHT.lampWant < 0.5 else IN.hand = not IN.hand end
    IN.handDay = LIGHT.day > 0.5
    s = IN.hand and "ПОДСВЕТКА ВКЛ" or "ПОДСВЕТКА ВЫКЛ"
  elseif n == 2 then
    if CLK.rate > 1 then CLK.rate = 1; s = "ДЕМО ВЫКЛ"
    else CLK.rate = CLK.demo; s = "ДЕМО: СУТКИ ЗА 5 МИН" end
  else
    CLK.rate, CLK.off, IN.hand = 1, 0, nil
    s = "ТЕКУЩЕЕ ВРЕМЯ"
  end
  IN.said, IN.till = s, T + 2.5
end

local function lamp_update(T)
  local btn = rawget(px, "button")
  if btn then
    local n = btn()
    if IN.last and n ~= IN.last then IN.n, IN.at = IN.n + (n - IN.last), T end
    IN.last = n
  end
  if IN.n > 0 and T - IN.at > MULTI then
    press(IN.n, T)
    IN.n = 0
  end
  local isDay = LIGHT.day > 0.5
  if IN.hand ~= nil and IN.handDay ~= isDay then IN.hand = nil end
  if IN.hand ~= nil then LIGHT.lampWant = IN.hand and 1 or 0
  else
    local dark = LIGHT.day < 0.35
    if RAD and CLK.rate == 1 and CLK.off == 0 then
      LIGHT.lampWant = (dark and ROOM.awake > 0.5) and 1 or 0
    else
      -- the demo, or no radar to ask: the tank's evening hours, as a public
      -- aquarium keeps them
      local h = floor(clock_h())
      LIGHT.lampWant = (dark and h >= 17 and h < 23) and 1 or 0
    end
  end
end

-- ── painting a pose ─────────────────────────────────────────────────────────
-- Every painter draws one animal facing right, at full light, centred on
-- (CX, CY) on a black canvas; px.grab cuts it out. Nothing here runs per
-- frame - only when a pose is first needed - so these can afford to work a
-- pixel at a time, which is what shading and patterns need.
local CX, CY = 64, 32

-- A run of pixels in a column is one call: colours are quantised to 3 steps
-- so neighbouring pixels of one shade merge.
-- A big pose is cut in vertical slices over several frames (cut_wanted);
-- CLIP0..CLIP1 are the columns of the slice being painted, and the per-pixel
-- loops skip what is outside it.
local CLIP0, CLIP1 = 0, 127
local runX, runY0, runY1, runR, runG, runB = 0, 0, -1, 0, 0, 0
local function run_flush()
  if runY1 >= runY0 then line(runX, runY0, runX, runY1, runR, runG, runB) end
  runY1 = runY0 - 1
end
local function run_px(x, y, r, g, b)
  if x < CLIP0 or x > CLIP1 then return end
  r = r < 0 and 0 or (r > 255 and 255 or r)
  g = g < 0 and 0 or (g > 255 and 255 or g)
  b = b < 0 and 0 or (b > 255 and 255 or b)
  r, g, b = r - r % 3, g - g % 3, b - b % 3
  r, g, b = floor(r), floor(g), floor(b)
  if r + g + b < 12 then r, g, b = 14, 14, 18 end   -- never transparent by accident
  if x == runX and y == runY1 + 1 and r == runR and g == runG and b == runB then
    runY1 = y
    return
  end
  run_flush()
  runX, runY0, runY1, runR, runG, runB = x, y, y, r, g, b
end

local function P(x, y, c, k)
  local r, g, b = C(c, k)
  if r + g + b < 12 then r, g, b = INK[1], INK[2], INK[3] end
  pixel(floor(x), floor(y), r, g, b)
end
local function L2(x0, y0, x1, y1, c, k)
  local r, g, b = C(c, k)
  if r + g + b < 12 then r, g, b = INK[1], INK[2], INK[3] end
  line(floor(x0 + 0.5), floor(y0 + 0.5), floor(x1 + 0.5), floor(y1 + 0.5), r, g, b)
end

-- Body outlines. Each is where the body is fullest (pk), how blunt the head
-- is (nb, lower is blunter), how thick the tail stalk is (ts), how the rear
-- tapers (tp), and the back and the belly each as a share of the half-height.
local SHAPE = {
  fusi  = { pk = 0.32, nb = 0.55, ts = 0.16, tp = 1.1, at = 1.0, ab = 1.0 },
  slim  = { pk = 0.30, nb = 0.70, ts = 0.18, tp = 1.2, at = 1.0, ab = 0.9 },
  deep  = { pk = 0.38, nb = 0.50, ts = 0.14, tp = 0.9, at = 1.05, ab = 1.0 },
  disc  = { pk = 0.44, nb = 0.42, ts = 0.12, tp = 0.75, at = 1.0, ab = 1.0 },
  long  = { pk = 0.30, nb = 0.85, ts = 0.30, tp = 1.4, at = 1.0, ab = 1.0 },
  needle = { pk = 0.40, nb = 1.00, ts = 0.45, tp = 2.0, at = 1.0, ab = 1.0 },
  globe = { pk = 0.48, nb = 0.40, ts = 0.22, tp = 0.55, at = 1.0, ab = 1.0 },
  box   = { pk = 0.18, nb = 0.30, ts = 0.34, tp = 0.40, at = 1.0, ab = 1.0 },
  shark = { pk = 0.34, nb = 0.95, ts = 0.14, tp = 1.25, at = 0.95, ab = 0.78 },
  stout = { pk = 0.36, nb = 0.42, ts = 0.22, tp = 0.95, at = 1.0, ab = 1.1 },
  mola  = { pk = 0.46, nb = 0.45, ts = 0.62, tp = 0.45, at = 1.0, ab = 1.0 },
  flatb = { pk = 0.28, nb = 0.60, ts = 0.18, tp = 1.0, at = 1.0, ab = 0.55 },  -- flat-bellied
}

local function profile(u, S)
  if u < S.pk then
    return (u / S.pk) ^ S.nb
  end
  local w = (u - S.pk) / (1 - S.pk)
  local c = cos(w * pi / 2)
  if c < 0 then c = 0 end
  return S.ts + (1 - S.ts) * c ^ S.tp
end

-- The colour of the body at u (0 the snout, 1 the tail stalk) and v (-1 the
-- back, +1 the belly), with its pattern, before the light is put on it. As
-- three numbers: a table a pixel would be garbage the collector then has to
-- clear in the middle of some later frame.
local function body_rgb(sp, u, v, cx, cy, n, hm)
  local a, b, k
  if v < 0 then a, b, k = sp.back, sp.mid, clamp((v + 1) / 0.72, 0, 1)
  else a, b, k = sp.mid, sp.belly, clamp((v - 0.12) / 0.6, 0, 1) end
  local r, g, bl = a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k, a[3] + (b[3] - a[3]) * k
  local pat = sp.pat
  if not pat then return r, g, bl end
  for i = 1, #pat do
    local p = pat[i]
    local kind = p.k
    local c = nil
    if kind == "vbar" then
      if (not p.top or v < (p.top_v or -0.1)) and (not p.lo or v > p.lo) then
        for j = 1, #p.u do
          local d = abs(u - p.u[j])
          if d < p.w then c = p.c; break
          elseif p.edge and d < p.w + (p.ew or 0.035) then c = p.edge; break end
        end
      end
    elseif kind == "hstripe" then
      if abs(v - p.v) < p.w and u >= (p.u0 or 0) and u <= (p.u1 or 1) then c = p.c end
    elseif kind == "lines" then
      if v > (p.v0 or -0.9) and v < (p.v1 or 0.7) and u > (p.u0 or 0.08) then
        local ph = (v - (p.v0 or -0.9)) * (p.n or 4)
        if ph - floor(ph) < (p.duty or 0.4) then c = p.c end
      end
    elseif kind == "split" then
      if u > p.u + (p.lean or 0) * v then c = p.c end
    elseif kind == "front" then
      if u < p.u + (p.lean or 0) * v then c = p.c end
    elseif kind == "mask" then
      if u > p.u0 and u < p.u1 and (not p.v0 or (v > p.v0 and v < p.v1)) then c = p.c end
    elseif kind == "blotch" then
      local du, dv = (u - p.u) / p.ru, (v - p.v) / p.rv
      local d = du * du + dv * dv
      if d < 1 then c = p.c
      elseif p.ring and d < 1.8 then c = p.ring end
    elseif kind == "spots" then
      local gs = p.g or 3
      local gx, gy = floor(cx / gs), floor((cy + 64) / gs)
      if hash2(gx + (p.seed or 0), gy) < (p.d or 0.4) and (cx % gs) == (gy % 2) and ((cy + 64) % gs) == 0 then
        if not p.top or v < 0.3 then c = p.c end
      end
    elseif kind == "dots" then                -- a regular net of dots, as a whale shark wears
      local gs = p.g or 3
      local ox = (floor((cy + 64) / gs) % 2) * floor(gs / 2)
      if ((cx + ox) % gs) == 0 and ((cy + 64) % gs) == 0 and v < (p.lo or 0.35) then c = p.c end
    elseif kind == "mottle" then
      local gs = p.g or 2
      if hash2(floor(cx / gs) + 31, floor((cy + 64) / gs) + 17) < (p.d or 0.45) then
        local q, m = p.c, p.a or 0.6
        r, g, bl = r + (q[1] - r) * m, g + (q[2] - g) * m, bl + (q[3] - bl) * m
      end
    elseif kind == "rims" then                -- scales with a lit rim, as the French angel's
      if ((cx + floor((cy + 64) / 2)) % 2) == 0 and ((cy + 64) % 2) == 0 and u > (p.u0 or 0.15) then c = p.c end
    elseif kind == "grad" then
      local t = clamp((u - p.u0) / (p.u1 - p.u0), 0, 1)
      local q = p.c
      r, g, bl = r + (q[1] - r) * t, g + (q[2] - g) * t, bl + (q[3] - bl) * t
    elseif kind == "belly" then
      if v > (p.v or 0.35) then c = p.c end
    elseif kind == "diag" then                -- stripes that lean, sweetlips and lionfish
      local st = (u * n + v * hm * (p.lean or 0.6)) / (p.p or 4)
      if st - floor(st) < (p.duty or 0.45) then c = p.c end
    end
    if c then r, g, bl = c[1], c[2], c[3] end
  end
  return r, g, bl
end

-- Fins: the height at t (0..1 along the fin) as a share of the fin's height.
local function fin_shape(kind, t)
  if kind == "tri" then return t < 0.25 and t / 0.25 or 1 - (t - 0.25) / 0.75 * 0.85
  elseif kind == "round" then return sin(t * pi) ^ 0.7
  elseif kind == "low" then return 0.45 * sin(t * pi) ^ 0.4
  elseif kind == "shark" then return t < 0.55 and t / 0.55 or (1 - t) / 0.45
  elseif kind == "sail" then return t < 0.2 and t / 0.2 or 1 - (t - 0.2) / 0.8 * 0.95
  elseif kind == "rear" then return t < 0.6 and 0.4 + t or 1 - (t - 0.6) * 2.2
  end
  return sin(t * pi)
end

-- One fish, or shark: sp as in the species table, lp its length in pixels,
-- fore how side-on it is (1 side-on, 0.25 nearly end-on), ph the beat.
local function paint_fish(sp, lp, fore, ph, var)
  local S = SHAPE[sp.shape or "fusi"]
  local tl = sp.tl or 0.22
  local n = max(2, floor(lp * (1 - tl) + 0.5))
  local hm = max(1, lp * sp.h)                      -- the body's full height
  local amp = (sp.wig or 1) * min(2.2, hm * 0.14 + lp * 0.02)
  local inflated = var == 1
  if inflated then S = SHAPE.globe; hm = hm * 1.7; n = max(2, floor(n * 0.85)) end

  local x0 = CX + floor(n / 2)                        -- the snout's column when side-on
  local cols = {}
  local fx = function(c) return CX + floor((n / 2 - c) * fore + 0.5) end

  -- The body, tail end first, so the head is drawn over it in a turn.
  for c = n - 1, 0, -1 do
    local u = n > 1 and c / (n - 1) or 0.5
    local pr = profile(u, S)
    if sp.hump and u < 0.32 then pr = pr * (1 + sp.hump * sin(clamp(u / 0.32, 0, 1) * pi)) end
    local ht = pr * hm * 0.5 * S.at
    local hb = pr * hm * 0.5 * S.ab
    local wig = amp * sin(ph - u * 2.6) * u * u
    local yc = CY + wig
    local x = fx(c)
    cols[c] = { x = x, yc = yc, ht = ht, hb = hb }
    if ht + hb >= 0.4 and x >= CLIP0 and x <= CLIP1 then
      local y0 = floor(yc - ht + 0.5)
      local y1 = floor(yc + hb + 0.5)
      if y1 < y0 then y1 = y0 end
      for py = y0, y1 do
        local d = py - yc
        local v = d < 0 and (ht > 0.3 and d / ht or -0.5) or (hb > 0.3 and d / hb or 0.5)
        v = clamp(v, -1, 1)
        local br, bg, bb = body_rgb(sp, u, v, c, py - CY, n, hm)
        -- Lit from above, darker at the rims, a sheen along the upper flank.
        local vv = (v + 0.5) * 3.3
        vv = 1 - vv * vv
        local s = 1.0 - 0.10 * v + (vv > 0 and 0.22 * vv * (sp.sheen or 1) or 0)
        if abs(v) > 0.8 and hm > 3 then s = s * 0.78 end
        s = s * (1 - 0.08 * u)
        run_px(x, py, br * s, bg * s, bb * s)
      end
      run_flush()
    end
  end

  local fin = sp.fin or sp.mid
  -- Dorsal and anal fins ride on the body's edge.
  local function fins(F, up)
    if not F then return end
    local fc = F.c or fin
    local h = F.h * hm * 0.5
    if h < 0.6 then return end
    local c0, c1 = floor(F.a * (n - 1)), floor(F.b * (n - 1))
    for c = c0, c1 do
      local col = cols[c]
      if col then
        local t = c1 > c0 and (c - c0) / (c1 - c0) or 0.5
        local fh = h * fin_shape(F.k or "round", t)
        if F.k == "spiny" then fh = (c % 2 == 0) and h * (0.7 + 0.3 * sin(t * pi)) or 0 end
        if fh >= 0.5 then
          local ya = up and (col.yc - col.ht) or (col.yc + col.hb)
          local yb = up and (ya - fh) or (ya + fh)
          local k = (c % 2 == 0) and 1 or 0.86
          if F.k == "spiny" then
            local steps = max(1, floor(fh))
            for s = 0, steps do
              local yy = up and ya - s or ya + s
              P(col.x, yy, (s % 3 == 2) and (F.c2 or { 245, 240, 230 }) or fc, 1)
            end
          else
            L2(col.x, ya, col.x, yb, fc, k)
          end
        end
      end
    end
    -- The trailing filament of a Moorish idol or a bannerfish.
    if F.fil then
      local col = cols[floor((F.a + (F.b - F.a) * 0.2) * (n - 1))]
      if col then
        local ya = col.yc - col.ht - h
        local tx = col.x - floor(F.fil * lp * fore)
        local ty = ya + F.fil * lp * 0.25 + amp * sin(ph - 2) * 0.6
        L2(col.x, ya, (col.x + tx) / 2, ya - 1, fc, 0.95)
        L2((col.x + tx) / 2, ya - 1, tx, ty, fc, 0.85)
      end
    end
  end
  fins(sp.dorsal, true)
  if sp.dorsal2 then fins(sp.dorsal2, true) end
  fins(sp.anal, false)

  -- The tail, from where the body ends this beat.
  local last = cols[n - 1]
  local tk = sp.tail or "fork"
  if last and tk ~= "none" then
    local tcol = sp.tailc or fin
    local lt = max(1, floor(lp * tl * fore + 0.5))
    local base = max(0.5, (last.ht + last.hb) * 0.5)
    local th = max(base + 0.5, hm * (sp.th or 0.42))
    local flex = amp * 1.1 * sin(ph - 2.9)
    local xe = last.x
    local thick = 1 + floor(hm / 14)
    for s = 1, lt do
      local f = s / lt
      local x = xe - s
      local yb = last.yc + flex * f
      local spread, fill
      if tk == "fork" then
        spread = base + (th - base) * f; fill = f < 0.45
      elseif tk == "lunate" then
        spread = base + (th - base) * f ^ 0.6; fill = f < 0.25
      elseif tk == "round" then
        spread = (base + (th - base) * sqrt(f)) * (f > 0.75 and (1 - (f - 0.75) * 1.6) or 1); fill = true
      elseif tk == "trunc" then
        spread = base + (th - base) * f; fill = true
      elseif tk == "veil" then
        spread = base + (th - base) * f * (1 + 0.15 * sin(ph * 2 + f * 5)); fill = true
      elseif tk == "shark" then
        spread = base + (th - base) * f; fill = f < 0.3
      elseif tk == "point" then
        spread = base * (1 - f); fill = true
      else
        spread = base; fill = true
      end
      local k = 1 - 0.18 * f
      if tk == "shark" then
        -- The upper lobe is the long one; the lower is half its length.
        L2(x, yb - spread * 1.25, x, yb - spread * 1.25 + thick, tcol, k)
        if f < 0.55 then L2(x, yb + spread * 0.8, x, yb + spread * 0.8 - thick, tcol, k) end
        if fill then L2(x, yb - spread * 1.25, x, yb + spread * 0.8, tcol, k * 0.9) end
      elseif fill then
        L2(x, yb - spread, x, yb + spread, tcol, k)
      else
        L2(x, yb - spread, x, yb - spread + thick, tcol, k)
        L2(x, yb + spread, x, yb + spread - thick, tcol, k)
      end
    end
    if sp.lyre then                         -- the long streamers of a lyretail
      L2(xe - lt, last.yc + flex - th, xe - lt - lp * 0.12, last.yc + flex - th - 1, tcol, 0.9)
      L2(xe - lt, last.yc + flex + th, xe - lt - lp * 0.12, last.yc + flex + th + 1, tcol, 0.9)
    end
  end

  -- Pectoral fin: beating twice for every beat of the tail.
  local pc = cols[floor(0.28 * (n - 1))]
  if pc and hm >= 5 then
    if sp.fans then                         -- the lionfish's great striped fans
      for r = 0, 4 do
        local a = 0.5 + r * 0.28 + 0.12 * sin(ph * 0.5 + r)
        local len = hm * (0.75 + 0.1 * r)
        local x1 = pc.x - cos(a) * len * fore
        local y1 = pc.yc + sin(a) * len
        local steps = max(2, floor(len))
        for s = 0, steps do
          local t = s / steps
          P(pc.x + (x1 - pc.x) * t, pc.yc + (y1 - pc.yc) * t,
            (floor(s / 2) % 2 == 0) and sp.fin or { 240, 232, 220 }, 1)
        end
      end
    else
      local a = 0.55 + 0.35 * sin(ph * 2)
      local len = max(1.5, hm * (sp.pect or 0.32))
      L2(pc.x - 1, pc.yc + pc.hb * 0.2, pc.x - 1 - cos(a) * len * fore, pc.yc + pc.hb * 0.2 + sin(a) * len,
         sp.pectc or fin, 1.1)
    end
  end

  -- Shark gills; the hammer; the saw; the horn; the barbels; the snout.
  local head = cols[0]
  local hx = head and head.x or x0
  local hy = head and head.yc or CY
  if sp.gills and hm >= 6 then
    for i = 0, 3 do
      local col = cols[floor((0.2 + i * 0.025) * (n - 1))]
      if col then L2(col.x, col.yc - col.ht * 0.3, col.x, col.yc + col.hb * 0.25, sp.back, 0.55) end
    end
  end
  if sp.hammer then
    local col = cols[1] or head
    L2(col.x, hy - hm * 0.62, col.x, hy + hm * 0.2, sp.back, 0.9)
    L2(col.x + 1, hy - hm * 0.55, col.x + 1, hy + hm * 0.12, sp.back, 1.05)
  end
  if sp.saw then
    local sl = lp * sp.saw * fore
    L2(hx + 1, hy, hx + sl, hy, sp.back, 1)
    for s = 2, floor(sl), 2 do
      P(hx + s, hy - 1, { 230, 226, 210 }); P(hx + s, hy + 1, { 230, 226, 210 })
    end
  end
  if sp.horn then
    L2(hx - 1, hy - hm * 0.28, hx + lp * sp.horn * fore, hy - hm * 0.45, sp.back, 0.9)
  end
  if sp.cowhorns then
    L2(hx - 1, hy - hm * 0.35, hx + 1 + lp * 0.08 * fore, hy - hm * 0.52, sp.back, 1)
  end
  if sp.barbels then
    L2(hx - 1, hy + hm * 0.2, hx + 1, hy + hm * 0.45, { 238, 230, 200 }, 1)
  end
  if sp.snout then
    local sl = max(1, lp * sp.snout * fore)
    L2(hx + 1, hy + hm * 0.05, hx + sl, hy + hm * 0.05, sp.snoutc or sp.mid, 1)
  end
  if inflated then                            -- a puffer blown up: spines all round
    local r = hm * 0.5
    for a = 0, 11 do
      local an = a * TAU / 12
      local cxp = CX + (n / 2 - (n - 1) * 0.48) * fore
      P(cxp + cos(an) * (r + 1) * fore, CY + sin(an) * (r + 1), sp.back, 0.8)
    end
  end

  -- The eye; and, nearly end-on, the other eye too, which is what makes a
  -- fish look at you.
  local ec = cols[floor((sp.eu or 0.13) * (n - 1))]
  if ec and lp >= 5 then
    local ey = floor(ec.yc - ec.ht * (sp.ev or 0.28) + 0.5)
    local iris = sp.eye or { 232, 228, 196 }
    if hm >= 10 then
      P(ec.x, ey, iris); P(ec.x + 1, ey, INK); P(ec.x, ey + 1, INK); P(ec.x + 1, ey + 1, INK)
    elseif hm >= 5 then
      P(ec.x, ey, iris); P(ec.x + 1, ey, INK)
    else
      P(ec.x, ey, INK)
    end
    if fore < 0.5 and hm >= 5 then
      P(ec.x, ey + floor(hm * 0.3), INK)
      P(ec.x + 1, ey + floor(hm * 0.3), iris)
    end
  end
end

-- A filled, shaded ellipse: the lit top and the dark underside of a round
-- thing. c is the colour, under the belly colour below the middle.
local function blob(cx, cy, rx, ry, c, under, pat)
  if rx < 0.5 or ry < 0.5 then P(cx, cy, c); return end
  local y0, y1 = floor(cy - ry + 0.5), floor(cy + ry + 0.5)
  for y = y0, y1 do
    local dy = (y - cy) / ry
    if dy < -1 then dy = -1 elseif dy > 1 then dy = 1 end
    local hw = rx * sqrt(1 - dy * dy)
    local xa, xb = floor(cx - hw + 0.5), floor(cx + hw + 0.5)
    if xa < CLIP0 then xa = CLIP0 end
    if xb > CLIP1 then xb = CLIP1 end
    local r0, g0, b0 = c[1], c[2], c[3]
    if under and dy > 0.15 then
      local k = clamp((dy - 0.15) / 0.5, 0, 1)
      r0, g0, b0 = r0 + (under[1] - r0) * k, g0 + (under[2] - g0) * k, b0 + (under[3] - b0) * k
    end
    local dd = (dy + 0.45) / 0.25
    local s0 = 1.08 - 0.22 * dy + 0.16 * exp(-dd * dd)
    for x = xa, xb do
      local r, g, b = r0, g0, b0
      if pat then
        local cc = pat(x, y, dy, (x - cx) / rx)
        if cc then r, g, b = cc[1], cc[2], cc[3] end
      end
      local s = s0
      if abs(x - cx) > hw - 0.8 then s = s * 0.8 end
      run_px(x, y, r * s, g * s, b * s)
    end
    run_flush()
  end
end

-- A tapered stroke along a list of points, for arms, flippers and tails.
local function stroke(pts, w0, w1, c, k)
  local n = #pts
  for i = 1, n - 1 do
    local a, b = pts[i], pts[i + 1]
    local w = w0 + (w1 - w0) * (i - 1) / max(1, n - 2)
    if w >= 1.5 then
      local r = floor(w / 2)
      for o = -r, r do
        L2(a[1], a[2] + o, b[1], b[2] + o, c, (k or 1) * (o == -r and 1.1 or (o == r and 0.8 or 1)))
      end
    else
      L2(a[1], a[2], b[1], b[2], c, k)
    end
  end
end

-- ── rays ────────────────────────────────────────────────────────────────────
-- A manta or an eagle ray gliding across, seen from a little below: the far
-- wing above the body, the near one below it showing its pale underside, both
-- beating together.
local function paint_ray(sp, lp, fore, ph)
  local bl = lp * 0.42
  -- Seen from the side and a little below, the span is foreshortened to a
  -- third: what reads is a glider's silhouette, wings swept back.
  local span = lp * (sp.span or 0.55) * 0.38
  local f = sin(ph)
  local front, rear = CX + bl * 0.5 * fore, CX - bl * 0.5 * fore
  local function wing(tipy, c, edge, spots)
    local tipx = CX - bl * 0.55 * fore
    local y0, y1 = CY, tipy
    local step = y1 < y0 and -1 or 1
    for y = floor(y0), floor(y1), step do
      local t = (y - y0) / (y1 - y0)
      local le = front + (tipx - front) * t ^ 0.8 + bl * 0.12 * sin(t * pi) * fore
      local te = rear + (tipx - rear) * t ^ 1.6
      if le < te then le, te = te, le end
      for x = max(CLIP0, floor(te)), min(CLIP1, floor(le)) do
        local cc = c
        if edge and (x >= floor(le) - 0 or t > 0.85) then cc = edge end
        if spots and hash2(x * 3, y * 5) < 0.12 and ((x + y) % 2 == 0) then cc = spots end
        local s = 1.05 - 0.25 * t
        run_px(x, y, cc[1] * s, cc[2] * s, cc[3] * s)
      end
      run_flush()
    end
  end
  wing(CY - span * (0.55 + 0.4 * f), sp.back, sp.edge, sp.spots)
  -- the tail, thin and long
  local tl = lp * (sp.tailL or 0.5)
  local pts = {}
  for i = 0, 8 do
    local t = i / 8
    pts[#pts + 1] = { rear - tl * t * fore, CY + 1 + sin(ph - t * 3) * 1.2 * t }
  end
  stroke(pts, 1, 1, sp.back, 0.8)
  blob(CX, CY, bl * 0.5 * fore, max(1, bl * 0.16), sp.back, sp.belly)
  wing(CY + span * (0.42 - 0.38 * f), sp.belly, sp.edge2 or sp.back, nil)
  if sp.manta then
    -- the cephalic fins, rolled like horns
    L2(front, CY - bl * 0.08, front + bl * 0.16 * fore, CY - bl * 0.02, sp.back, 1)
    L2(front, CY + bl * 0.06, front + bl * 0.16 * fore, CY + bl * 0.1, sp.back, 0.8)
    if sp.patch then blob(CX + bl * 0.12 * fore, CY - span * 0.25, bl * 0.12 * fore, span * 0.12, sp.patch) end
  else
    blob(front + bl * 0.06 * fore, CY + 0.5, bl * 0.1 * fore, max(1, bl * 0.08), sp.back)
  end
  P(front - bl * 0.05 * fore, CY - 1, INK)
end

-- A ray lying on or gliding over the sand, and the flounder: a flat disc seen
-- from the front at a low angle, the edge rippling.
local function paint_flat(sp, lp, fore, ph)
  local rx, ry = lp * 0.32 * fore, max(1, lp * 0.09)
  local cy = CY
  for x = max(CLIP0, floor(CX - rx)), min(CLIP1, floor(CX + rx)) do
    local u = (x - CX) / rx
    local e = sqrt(max(0, 1 - u * u))
    local rip = 0.8 * sin(ph * 2 + u * 6)
    local top = cy - ry * e - (abs(u) > 0.5 and rip * 0.5 or 0)
    local bot = cy + ry * 0.4 * e
    for y = floor(top), floor(bot) do
      local cc = sp.back
      local v = (y - top) / max(1, bot - top)
      if sp.rings and hash2(x, y) < 0.08 then cc = sp.rings end
      if sp.spots2 and hash2(x * 7, y * 3) < 0.1 then cc = sp.spots2 end
      local s = 1.05 - 0.3 * v
      local r, g, b = cc[1], cc[2], cc[3]
      if v > 0.75 and sp.belly then
        r, g, b = (r + sp.belly[1]) * 0.5, (g + sp.belly[2]) * 0.5, (b + sp.belly[3]) * 0.5
      end
      run_px(x, y, r * s, g * s, b * s)
    end
    run_flush()
  end
  if not sp.flounder then
    local tl = lp * (sp.tailL or 0.45)
    L2(CX - rx, cy, CX - rx - tl * fore, cy + 1 + sin(ph) * 0.6, sp.tailc or sp.back, 0.85)
  end
  P(CX + rx * 0.45, cy - ry * 0.9, sp.eye or INK)
  P(CX + rx * 0.55, cy - ry * 0.7, sp.eye or INK)
end

-- ── jellies ─────────────────────────────────────────────────────────────────
local function paint_jelly(sp, lp, fore, ph)
  local R = lp * 0.5
  local c = 0.5 - 0.5 * cos(ph)                       -- 0 relaxed, 1 contracted
  local tlen = lp * (sp.tent or 1.2)
  if tlen + R > 58 then tlen = 58 - R end
  local yr = floor(CY + R * 0.4 - tlen * 0.45)          -- the rim
  local bw = R * (1 - 0.22 * c)
  local bh = R * ((sp.flat and 0.35 or 0.66) + 0.2 * c)
  if sp.comb then
    -- A comb jelly: no bell, a clear egg with eight rows of beating combs
    -- that break the light into colours running down them.
    local ry, rx = lp * 0.5, lp * 0.3
    for y = floor(CY - ry), floor(CY + ry) do
      local dy = (y - CY) / ry
      local hw = rx * sqrt(max(0, 1 - dy * dy))
      for x = floor(CX - hw), floor(CX + hw) do
        run_px(x, y, 60, 80, 96)
      end
      run_flush()
    end
    for r = 0, 3 do
      local xo = (r - 1.5) / 1.5 * rx * 0.75
      for y = floor(CY - ry * 0.85), floor(CY + ry * 0.85) do
        local hue = (y * 0.35 + ph * 2 + r) % 3
        local col = hue < 1 and { 255, 70, 140 } or (hue < 2 and { 70, 255, 150 } or { 90, 140, 255 })
        local dy = (y - CY) / ry
        P(CX + xo * sqrt(max(0, 1 - dy * dy)), y, col)
      end
    end
    return
  end
  -- tentacles first, behind the bell
  local nt = sp.nt or 8
  for i = 0, nt - 1 do
    local x0 = CX - bw * 0.9 + (2 * bw * 0.9) * (nt > 1 and i / (nt - 1) or 0.5)
    local len = tlen * (0.6 + 0.4 * hash2(i, 3))
    local steps = floor(len)
    for s = 0, steps, (sp.dense and 1 or 1) do
      local t = s / max(1, steps)
      local x = x0 + sin(ph * 0.7 + t * 5 + i * 1.3) * (1 + 2.5 * t) - c * (x0 - CX) * 0.15
      P(x, yr + s, sp.tc or sp.rim, 0.95 - 0.55 * t)
    end
  end
  -- oral arms: thicker, ruffled
  local na = sp.arms or 4
  for i = 0, na - 1 do
    local x0 = CX + (i - (na - 1) / 2) * max(1, bw * 0.2)
    local len = tlen * (sp.armL or 0.55)
    local pts = {}
    for s = 0, 6 do
      local t = s / 6
      pts[#pts + 1] = { x0 + sin(ph * 0.6 + t * 4 + i) * (1 + 2 * t), yr + len * t }
    end
    stroke(pts, max(1, R * 0.28), 1, sp.ac or sp.rim, 0.9)
    if sp.armtip then P(pts[7][1], pts[7][2], sp.armtip) end
  end
  -- the bell: a clear dome, its rim brighter than its middle
  for y = floor(yr - bh), yr do
    local t = (yr - y) / max(1, bh)
    local hw = bw * sqrt(max(0, 1 - t * t))
    for x = floor(CX - hw), floor(CX + hw) do
      local edge = abs(x - CX) > hw - 1.2 or y == floor(yr - bh)
      local cc = edge and sp.rim or sp.bell
      if sp.stripes and floor((x - CX) / max(1, bw * 0.25) + 10) % 2 == 0 and not edge then cc = sp.stripes end
      if sp.dome and t > 0.45 and abs(x - CX) < hw * 0.55 then cc = sp.dome end
      run_px(x, y, cc[1], cc[2], cc[3])
    end
    run_flush()
  end
  if sp.gonads and R >= 4 then                -- a moon jelly's four rings
    for i = 0, 3 do
      local gx = CX + (i - 1.5) * bw * 0.32
      local gy = yr - bh * 0.45
      circle(floor(gx), floor(gy), max(1, floor(R * 0.14)), C(sp.gonads))
    end
  end
end

-- ── turtles ─────────────────────────────────────────────────────────────────
local function paint_turtle(sp, lp, fore, ph)
  local sl, sh = lp * 0.56 * fore, lp * 0.19
  local a = sin(ph)
  local sx, sy = CX - lp * 0.04 * fore, CY
  -- the far front flipper, behind the shell
  local fl = lp * 0.42
  local ang = -0.2 - 0.9 * a
  stroke({ { sx + sl * 0.35, sy - 1 }, { sx + sl * 0.35 - cos(ang) * fl * 0.5 * fore, sy - 2 + sin(ang) * fl * 0.5 },
           { sx + sl * 0.35 - cos(ang) * fl * fore, sy - 2 + sin(ang) * fl * 0.8 } }, 2, 1, sp.skin, 0.7)
  -- rear flippers
  L2(sx - sl * 0.45, sy + sh * 0.3, sx - sl * 0.72, sy + sh * 0.5 + a, sp.skin, 0.85)
  -- the shell: a dome over a pale plastron
  blob(sx, sy - sh * 0.1, sl * 0.5, sh * 0.62, sp.shell, sp.plastron, function(x, y, dy, dx)
    if dy < 0.25 then
      if sp.scute and (abs((dx * 3 + 10) % 1 - 0.5) < 0.12 or abs(dy + 0.5) < 0.07) then return sp.scute end
      if sp.streak and hash2(x * 5, floor(y / 2)) < 0.3 then return sp.streak end
    end
    return nil
  end)
  -- head and neck
  local hx, hy = sx + sl * 0.5 + lp * 0.07 * fore, sy + sh * 0.12
  blob(hx, hy, lp * 0.085 * fore + 0.5, lp * 0.06 + 0.3, sp.skin, sp.plastron)
  if lp >= 12 then
    P(hx + lp * 0.03 * fore, hy - 1, INK)
    for i = 0, 2 do P(hx - lp * 0.05 * fore + i * 2 * fore, hy - lp * 0.03, sp.scute or sp.shell, 0.8) end
  end
  -- the near front flipper, over everything: the stroke that drives it
  local ang2 = 0.3 - 1.1 * a
  stroke({ { sx + sl * 0.3, sy + sh * 0.35 }, { sx + sl * 0.3 - cos(ang2) * fl * 0.5 * fore, sy + sh * 0.35 + sin(ang2) * fl * 0.5 },
           { sx + sl * 0.3 - cos(ang2) * fl * fore, sy + sh * 0.35 + sin(ang2) * fl * 0.85 } }, max(1, lp * 0.07), 1, sp.skin, 1)
end

-- ── seahorses and the leafy seadragon ───────────────────────────────────────
local SH_PTS = { { 0.03, -0.40 }, { -0.05, -0.30 }, { 0.00, -0.18 }, { 0.06, -0.04 },
                 { 0.05, 0.08 }, { -0.01, 0.18 }, { -0.04, 0.27 }, { -0.01, 0.34 } }
local SH_W = { 0.07, 0.045, 0.085, 0.10, 0.085, 0.05, 0.035, 0.02 }
local function paint_seahorse(sp, lp, fore, ph)
  local function at(i) return CX + SH_PTS[i][1] * lp * fore, CY + SH_PTS[i][2] * lp end
  -- the dorsal fin, a blur of beating rays on the back
  local fx, fy = at(4)
  for r = 0, 3 do
    local len = lp * 0.08 * (0.6 + 0.4 * sin(ph * 3 + r))
    L2(fx - lp * 0.09 * fore, fy + r - 1, fx - lp * 0.09 * fore - len * fore, fy + r - 2, sp.fin or sp.body, 0.9)
  end
  for i = 1, #SH_PTS - 1 do
    local x0, y0 = at(i)
    local x1, y1 = at(i + 1)
    local steps = max(1, floor(abs(y1 - y0) + abs(x1 - x0)))
    for s = 0, steps do
      local t = s / steps
      local x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
      local w = (SH_W[i] + (SH_W[i + 1] - SH_W[i]) * t) * lp * fore
      local ring = (floor(y) % 3 == 0) and (sp.ring or sp.body) or sp.body
      L2(x - w, y, x + w, y, ring, 1)
      if w > 1 then P(x + w, y, sp.belly or ring, 1.1) end
    end
  end
  -- the tail's curl, forward under the belly
  local tx, ty = at(#SH_PTS)
  for s = 0, 10 do
    local a = s / 10 * 4.4
    local r = lp * (0.055 - 0.004 * s)
    P(tx + (sin(a) * r + r * 0.2) * fore, ty + (1 - cos(a)) * r, sp.body, 0.9)
  end
  -- the head: a snout like a pipe, a crown, an eye
  local hx, hy = at(1)
  L2(hx, hy + 1, hx + lp * 0.16 * fore, hy + 1 + lp * 0.02, sp.body, 1)
  P(hx + lp * 0.16 * fore, hy + 1 + lp * 0.02, sp.body, 1.2)
  L2(hx - 1, hy - lp * 0.05, hx, hy - lp * 0.09, sp.body, 1.1)
  if lp >= 10 then P(hx + 1, hy, INK) end
  if sp.leafy then
    -- The seadragon: leaves off every joint, drifting like weed.
    for i = 2, #SH_PTS do
      local x, y = at(i)
      for side = -1, 1, 2 do
        local la = side * (0.9 + 0.3 * sin(ph + i))
        local len = lp * 0.12
        local ex, ey = x - sin(la) * len * fore, y + side * len * 0.4 - len * 0.5
        L2(x, y, ex, ey, sp.leaf, 1)
        P(ex, ey, sp.leaf2 or sp.leaf, 1.1)
        P(ex - 1, ey + 1, sp.leaf2 or sp.leaf, 1)
      end
    end
  end
end

-- ── octopus, cuttlefish, squid, nautilus ────────────────────────────────────
local function paint_octopus(sp, lp, fore, ph, var)
  local tex = function(x, y) if hash2(x * 3, y * 7) < 0.18 then return sp.spot end end
  if var == 1 then
    -- jetting: mantle first, arms streaming behind it
    for i = 0, 7 do
      local pts = {}
      for s = 0, 6 do
        local t = s / 6
        pts[#pts + 1] = { CX - lp * 0.05 * fore - lp * 0.5 * t * fore,
                          CY + (i - 3.5) * 0.35 * (1 - t * 0.6) + sin(ph + t * 4 + i) * 1.5 * t }
      end
      stroke(pts, max(1, lp * 0.06), 1, sp.body, 0.85 + 0.03 * i)
    end
    blob(CX + lp * 0.14 * fore, CY - 0.5, lp * 0.2 * fore, lp * 0.11, sp.body, sp.under, tex)
    P(CX - lp * 0.02 * fore, CY + 1, sp.eye or { 230, 200, 120 })
    return
  end
  -- crawling: the arms out on the sand, curling at the tips
  for i = 0, 7 do
    local a0 = pi * (0.05 + 0.9 * i / 7)
    local pts = {}
    for s = 0, 8 do
      local t = s / 8
      local a = a0 + 0.6 * t * t * sin(ph + i * 1.7)
      local r = lp * 0.42 * t
      pts[#pts + 1] = { CX + cos(a) * r * fore * 1.1, CY + sin(a) * r * 0.55 + t * 2 }
    end
    stroke(pts, max(1, lp * 0.07), 1, (i % 2 == 0) and sp.body or sp.under, 0.9)
  end
  blob(CX - lp * 0.06 * fore, CY - lp * 0.2, lp * 0.15 * fore, lp * 0.19, sp.body, sp.under, tex)
  local ex = CX + lp * 0.05 * fore
  if lp >= 10 then
    P(ex, CY - lp * 0.07, sp.eye or { 230, 200, 120 }); P(ex + 1, CY - lp * 0.07, INK)
  end
end

local function paint_cuttle(sp, lp, fore, ph)
  local bl, bh = lp * 0.36 * fore, lp * 0.14
  -- arms, bunched to a point in front
  for i = 0, 4 do
    L2(CX + bl, CY + (i - 2) * 0.4, CX + bl + lp * 0.2 * fore, CY + (i - 2) * 0.15 + sin(ph + i) * 0.4, sp.arms or sp.body, 0.9)
  end
  -- the body, with clouds of dark passing down it
  blob(CX, CY, bl, bh, sp.body, sp.under, function(x, y, dy, dx)
    local band = sin((dx * 5 + ph * 1.2)) + 0.4 * sin(dy * 6)
    if sp.zebra and band > 0.55 and dy < 0.3 then return sp.zebra end
    if sp.chrom and hash2(x * 11 + floor(ph * 2), y * 13) < 0.1 then return sp.chrom end
    return nil
  end)
  -- the fin skirt, rippling: the wave runs along it
  for x = floor(CX - bl), floor(CX + bl) do
    local u = (x - CX) / bl
    local e = sqrt(max(0, 1 - u * u))
    local rip = sin(ph * 3 - u * 8)
    P(x, CY - bh * e - (rip > 0 and 1 or 0), sp.skirt or sp.body, 0.9 + 0.2 * rip)
    P(x, CY + bh * e + (rip > 0 and 1 or 0), sp.skirt or sp.body, 0.8 + 0.2 * rip)
  end
  if lp >= 8 then P(CX + bl * 0.55, CY - bh * 0.25, INK); P(CX + bl * 0.55 + 1, CY - bh * 0.25, sp.eye or { 220, 210, 150 }) end
end

local function paint_nautilus(sp, lp, fore, ph)
  local r = lp * 0.36
  -- tentacles out the front
  for i = 0, 5 do
    L2(CX + r * 0.6 * fore, CY + r * 0.2 + i * 0.4, CX + r * 0.6 * fore + lp * 0.22 * fore,
       CY + r * 0.3 + i * 0.8 + sin(ph + i) * 0.7, sp.skin, 0.85)
  end
  blob(CX, CY, r * fore, r, sp.shell, sp.under, function(x, y, dy, dx)
    local a = atan(dy, dx)
    if sin(a * 3 + (1 - dx) * 4) > 0.45 and dy < 0.5 then return sp.stripe end
    return nil
  end)
  circle(floor(CX + r * 0.35 * fore), floor(CY + r * 0.1), max(1, floor(r * 0.28)), C(sp.under, 0.9))
  if lp >= 10 then P(CX + r * 0.7 * fore, CY - 1, INK) end
end

-- ── crustaceans ─────────────────────────────────────────────────────────────
local function legs(x0, x1, y, n, len, c, ph, fore)
  for i = 0, n - 1 do
    local x = x0 + (x1 - x0) * (n > 1 and i / (n - 1) or 0.5)
    local lift = (i % 2 == 0) and sin(ph) or -sin(ph)
    local kx = x + (i - (n - 1) / 2) * 0.5 * fore
    L2(x, y, kx + fore, y + len * 0.5 - max(0, lift) * 0.8, c, 0.9)
    L2(kx + fore, y + len * 0.5 - max(0, lift) * 0.8, kx + (i < n / 2 and -1 or 1) * fore, y + len, c, 0.8)
  end
end

local function paint_crust(sp, lp, fore, ph)
  local k = sp.crust
  if k == "crab" then
    local bw, bh = lp * 0.3 * fore, lp * 0.14
    legs(CX - bw * 0.9, CX + bw * 0.9, CY + bh * 0.3, 4, lp * 0.28, sp.legs or sp.body, ph, fore)
    blob(CX, CY, bw, bh, sp.body, sp.under)
    -- claws raised, opening and shutting
    for side = -1, 1, 2 do
      local cx = CX + side * bw * 0.75
      local cy = CY - bh * 0.9
      blob(cx, cy - lp * 0.06, lp * 0.07 * fore + 0.5, lp * 0.06 + 0.5, sp.claw or sp.body)
      local o = 0.5 + 0.5 * sin(ph * 0.5)
      L2(cx + fore, cy - lp * 0.12, cx + 2 * fore, cy - lp * 0.12 - 1 - o, sp.claw or sp.body, 1.1)
    end
    P(CX - 1, CY - bh - 1, INK); P(CX + 1, CY - bh - 1, INK)
  elseif k == "lobster" or k == "shrimp" or k == "mantis" then
    local bl = lp * (k == "shrimp" and 0.5 or 0.62) * fore
    local bh = lp * (k == "shrimp" and 0.08 or 0.1)
    -- antennae first, long and sweeping
    local al = lp * (k == "mantis" and 0.2 or (k == "shrimp" and 1.0 or 1.1))
    for i = 0, 1 do
      local sw = 0.3 * sin(ph * 0.5 + i * 2)
      local pts = {}
      for s = 0, 6 do
        local t = s / 6
        pts[#pts + 1] = { CX + bl * 0.5 + al * t * cos(-0.5 - i * 0.4 + sw) * fore,
                          CY - bh * 0.5 + al * t * sin(-0.5 - i * 0.4 + sw) * 0.8 + t * t * al * 0.3 }
      end
      stroke(pts, 1, 1, sp.ant or sp.body, 0.95)
    end
    legs(CX - bl * 0.1, CX + bl * 0.35, CY + bh * 0.6, 5, lp * 0.12, sp.legs or sp.body, ph, fore)
    -- the abdomen in segments, then the tail fan
    local segs = 6
    for i = 0, segs - 1 do
      local t = i / segs
      local x = CX + bl * 0.5 - bl * t
      local hh = bh * (1 - 0.35 * t) * (k == "shrimp" and (1 - 0.8 * t * (1 - t)) or 1)
      local arch = (k == "shrimp") and -sin(t * pi) * bh * 0.8 or 0
      local c = (i % 2 == 0) and sp.body or (sp.band or sp.body)
      L2(x, CY - hh + arch, x, CY + hh * 0.7 + arch, c, 1.05 - 0.15 * t)
      L2(x - 1 * fore, CY - hh + arch, x - 1 * fore, CY + hh * 0.7 + arch, c, 0.95 - 0.15 * t)
      if sp.stripe and k == "shrimp" then P(x, CY - hh + arch, sp.stripe) end
    end
    local fx = CX + bl * 0.5 - bl
    L2(fx, CY - bh * 0.2, fx - lp * 0.09 * fore, CY - bh * 1.1, sp.fan or sp.body, 1)
    L2(fx, CY, fx - lp * 0.1 * fore, CY + bh * 0.9, sp.fan or sp.body, 1)
    if sp.claws then
      blob(CX + bl * 0.7, CY - bh * 0.3, lp * 0.1 * fore + 0.5, lp * 0.05 + 0.5, sp.claw or sp.body)
    end
    if sp.spots and lp >= 12 then
      for i = 0, 5 do P(CX + bl * 0.5 - i * bl / 6, CY - bh * 0.5 + (i % 2), sp.spots) end
    end
    if lp >= 8 then P(CX + bl * 0.5 + 1, CY - bh, sp.eyec or INK) end
  end
end

-- ── the bottom's slow ones ──────────────────────────────────────────────────
local function paint_static(sp, lp, fore, ph)
  local k = sp.still
  if k == "star" then
    local R = lp * 0.5
    for a = 0, 4 do
      local an = -pi / 2 + a * TAU / 5 + (sp.twist or 0)
      local pts = {}
      for s = 0, 5 do
        local t = s / 5
        pts[#pts + 1] = { CX + cos(an) * R * t, CY + sin(an) * R * t * 0.45 }
      end
      stroke(pts, max(1, R * (sp.fat or 0.35)), 1, sp.body, 1)
    end
    blob(CX, CY, R * (sp.fat or 0.35) * 0.9, R * (sp.fat or 0.35) * 0.45, sp.body)
    if sp.bumps and R >= 5 then
      for a = 0, 4 do
        local an = -pi / 2 + a * TAU / 5
        P(CX + cos(an) * R * 0.5, CY + sin(an) * R * 0.22, sp.bumps)
      end
    end
  elseif k == "urchin" then
    local r = lp * 0.22
    for a = 0, 13 do
      local an = a * TAU / 14 + 0.1 * sin(ph + a)
      if sin(an) < 0.4 then
        L2(CX, CY, CX + cos(an) * lp * 0.5, CY + sin(an) * lp * 0.45, sp.spine or sp.body, 0.9)
      end
    end
    blob(CX, CY, r, r * 0.8, sp.body)
    if sp.eyes then P(CX, CY - r * 0.2, sp.eyes) end
  elseif k == "cucumber" then
    blob(CX, CY, lp * 0.5, lp * 0.13, sp.body, sp.under, function(x, y)
      if hash2(x * 5, y * 3) < 0.2 then return sp.spot end
    end)
  elseif k == "feather" then
    -- A feather star: arms like ferns, waving.
    for a = 0, 9 do
      local an = -pi * (0.1 + 0.8 * a / 9) + 0.15 * sin(ph + a * 0.7)
      local len = lp * 0.5
      local ex, ey = CX + cos(an) * len, CY + sin(an) * len
      L2(CX, CY, ex, ey, sp.body, 1)
      for s = 2, floor(len), 2 do
        local t = s / len
        local px_, py_ = CX + cos(an) * s, CY + sin(an) * s
        P(px_ + 1, py_ - 1, sp.pinn or sp.body, 0.9 - 0.3 * t)
      end
    end
    blob(CX, CY + 1, 1.5, 1, sp.body)
  end
end

-- ── the moray, looking out of its hole ──────────────────────────────────────
-- var is how far out it is (0..3); ph opens and shuts its mouth, which is how
-- it breathes - not a threat, though it reads as one.
local function paint_moray(sp, lp, fore, ph, var)
  local out = (var + 1) / 4
  local len = lp * 0.55 * out
  local th = max(1, lp * 0.07)
  local pts = {}
  local x0 = CX - lp * 0.25
  for s = 0, 8 do
    local t = s / 8
    pts[#pts + 1] = { x0 + len * t, CY + sin(ph * 0.3 + t * 3) * lp * 0.05 * t - t * lp * 0.06 }
  end
  for i = 1, #pts - 1 do
    local a, b = pts[i], pts[i + 1]
    for o = -floor(th), floor(th) do
      local v = o / max(1, th)
      local c = sp.body
      if sp.net and hash2(floor(a[1]) * 3, floor(a[2] + o) * 5) < 0.35 then c = sp.net end
      L2(a[1], a[2] + o, b[1], b[2] + o, c, 1.05 - 0.2 * v)
    end
  end
  local hx, hy = pts[#pts][1], pts[#pts][2]
  local jaw = 0.5 + 0.5 * sin(ph)
  blob(hx + th * 0.8, hy - 0.3, th * 1.3, th * 0.95, sp.body)
  L2(hx + th * 0.5, hy + th * 0.2, hx + th * 2.1, hy + th * 0.1 + jaw * th * 0.6, sp.body, 0.9)
  L2(hx + th * 0.6, hy + th * 0.25 + jaw * 0.5, hx + th * 1.9, hy + th * 0.2 + jaw * th * 0.5, { 90, 20, 30 }, 1)
  P(hx + th * 1.2, hy - th * 0.5, sp.eye or { 230, 220, 140 })
end

-- ── who lives here ──────────────────────────────────────────────────────────
-- L is the length in pixels at the glass; everything further back is smaller
-- (scale_at). zone is the part of the tank it keeps to; act when it is about
-- (d day, n night, a always); w how often it turns up; pred how high it is in
-- the food chain (the small flee from anything two steps above them).
local SPECIES = {}
local BY_NAME = {}
do
local function def(t)
  t.pred = t.pred or 0; t.act = t.act or "a"; t.w = t.w or 1; t.v = t.v or 1
  SPECIES[#SPECIES + 1] = t; t.id = #SPECIES; return t
end
local function fish(t) t.kind = t.kind or "fish"; t.act = t.act or "d"; t.pred = t.pred or 0; return def(t) end

-- colours
local WHITE, CREAM, BLK = { 240, 238, 228 }, { 250, 236, 196 }, INK
local YEL, GOLD, ORG, RED = { 255, 214, 20 }, { 250, 176, 30 }, { 255, 110, 20 }, { 226, 40, 40 }
local BLUE, ROYAL, CYAN, TEAL = { 40, 110, 240 }, { 30, 70, 220 }, { 60, 210, 240 }, { 30, 160, 150 }
local SILV, GREY, DGREY = { 196, 208, 222 }, { 128, 136, 148 }, { 70, 78, 90 }
local PINK, PURP, GREEN = { 255, 120, 170 }, { 150, 70, 210 }, { 70, 200, 90 }
local BROWN, SAND = { 130, 90, 56 }, { 200, 176, 130 }

-- The reef
fish{ name = "clownfish", L = 8, h = 0.42, shape = "deep", back = ORG, mid = { 255, 124, 26 }, belly = { 255, 150, 60 },
  pat = { { k = "vbar", u = { 0.17, 0.52, 0.9 }, w = 0.055, c = WHITE, edge = BLK } }, fin = ORG, tailc = ORG,
  dorsal = { a = 0.28, b = 0.85, h = 0.35, k = "round", c = { 255, 120, 30 } }, tail = "round", zone = "anem", v = 5, w = 3 }
fish{ name = "blue tang", L = 12, h = 0.55, shape = "disc", back = ROYAL, mid = BLUE, belly = { 90, 150, 250 },
  pat = { { k = "blotch", u = 0.5, v = -0.25, ru = 0.3, rv = 0.3, c = { 16, 20, 60 } }, { k = "blotch", u = 0.62, v = -0.35, ru = 0.12, rv = 0.14, c = BLUE } },
  fin = { 20, 40, 150 }, tailc = YEL, tail = "trunc", dorsal = { a = 0.2, b = 0.95, h = 0.25, k = "low", c = { 20, 30, 100 } },
  anal = { a = 0.45, b = 0.95, h = 0.2, k = "low", c = { 20, 30, 100 } }, zone = "reef", v = 8, w = 3, school = { 2, 4 } }
fish{ name = "yellow tang", L = 11, h = 0.7, shape = "disc", back = YEL, mid = YEL, belly = { 255, 230, 90 }, fin = YEL, tail = "trunc",
  dorsal = { a = 0.2, b = 0.95, h = 0.5, k = "round" }, anal = { a = 0.45, b = 0.95, h = 0.45, k = "round" }, snout = 0.05, zone = "reef", v = 8, w = 3, school = { 2, 5 } }
fish{ name = "Moorish idol", L = 12, h = 0.8, shape = "disc", back = CREAM, mid = WHITE, belly = CREAM,
  pat = { { k = "vbar", u = { 0.2 }, w = 0.1, c = BLK }, { k = "vbar", u = { 0.66 }, w = 0.12, c = BLK }, { k = "split", u = 0.8, c = YEL } },
  fin = WHITE, tailc = BLK, tail = "trunc", snout = 0.12, snoutc = ORG,
  dorsal = { a = 0.25, b = 0.6, h = 0.9, k = "sail", fil = 0.5, c = WHITE }, anal = { a = 0.45, b = 0.8, h = 0.6, k = "round", c = BLK }, zone = "reef", v = 7, w = 2 }
fish{ name = "copperband butterflyfish", L = 10, h = 0.7, shape = "disc", back = WHITE, mid = WHITE, belly = CREAM,
  pat = { { k = "vbar", u = { 0.18, 0.42, 0.68 }, w = 0.07, c = { 255, 120, 20 }, edge = BLK, ew = 0.02 }, { k = "blotch", u = 0.85, v = -0.55, ru = 0.07, rv = 0.14, c = BLK, ring = WHITE } },
  snout = 0.16, snoutc = WHITE, fin = { 240, 220, 170 }, tail = "trunc", dorsal = { a = 0.3, b = 0.95, h = 0.4, k = "round" }, zone = "reef", v = 6, w = 2 }
fish{ name = "raccoon butterflyfish", L = 10, h = 0.68, shape = "disc", back = YEL, mid = YEL, belly = { 255, 236, 120 },
  pat = { { k = "mask", u0 = 0.08, u1 = 0.2, v0 = -0.9, v1 = 0.35, c = BLK }, { k = "diag", c = GOLD, lean = 0.8, p = 3 } },
  snout = 0.05, fin = YEL, tail = "trunc", dorsal = { a = 0.3, b = 0.95, h = 0.4, k = "round", c = { 250, 200, 0 } }, zone = "reef", v = 6, w = 2, school = { 2, 2 } }
fish{ name = "emperor angelfish", L = 15, h = 0.62, shape = "deep", back = ROYAL, mid = { 40, 70, 210 }, belly = BLUE,
  pat = { { k = "lines", c = YEL, n = 6, duty = 0.45, v0 = -0.9, v1 = 0.8 }, { k = "mask", u0 = 0.1, u1 = 0.2, c = BLK }, { k = "front", u = 0.08, c = { 220, 220, 250 } } },
  fin = BLUE, tailc = YEL, tail = "round", dorsal = { a = 0.3, b = 0.95, h = 0.4, k = "round", c = BLUE }, anal = { a = 0.5, b = 0.95, h = 0.35, k = "round", c = BLUE }, zone = "reef", v = 6, w = 2 }
fish{ name = "queen angelfish", L = 15, h = 0.7, shape = "deep", back = { 60, 140, 200 }, mid = { 120, 200, 120 }, belly = YEL,
  pat = { { k = "blotch", u = 0.12, v = -0.8, ru = 0.07, rv = 0.15, c = ROYAL, ring = CYAN } },
  fin = { 60, 120, 230 }, tailc = YEL, tail = "round", dorsal = { a = 0.3, b = 0.95, h = 0.6, k = "rear", c = { 50, 120, 220 }, fil = 0.25 },
  anal = { a = 0.5, b = 0.95, h = 0.5, k = "rear", c = { 50, 120, 220 } }, zone = "reef", v = 6, w = 2 }
fish{ name = "French angelfish", L = 17, h = 0.75, shape = "disc", back = { 30, 30, 34 }, mid = { 34, 34, 40 }, belly = { 40, 40, 46 },
  pat = { { k = "rims", c = { 240, 200, 40 } } }, fin = { 36, 36, 40 }, tail = "round", eye = YEL,
  dorsal = { a = 0.3, b = 0.9, h = 0.6, k = "rear", fil = 0.3 }, anal = { a = 0.5, b = 0.9, h = 0.5, k = "rear" }, zone = "reef", v = 5, w = 1, curious = true, school = { 2, 2 } }
fish{ name = "regal angelfish", L = 11, h = 0.6, shape = "deep", back = ORG, mid = { 255, 160, 40 }, belly = YEL,
  pat = { { k = "vbar", u = { 0.25, 0.45, 0.65, 0.85 }, w = 0.045, c = WHITE, edge = { 40, 60, 160 } } },
  fin = { 250, 200, 60 }, tail = "round", dorsal = { a = 0.3, b = 0.9, h = 0.4, k = "round", c = { 80, 90, 200 } }, zone = "reef", v = 6, w = 1 }
fish{ name = "flame angelfish", L = 8, h = 0.52, shape = "deep", back = RED, mid = { 255, 70, 30 }, belly = ORG,
  pat = { { k = "vbar", u = { 0.35, 0.5, 0.65 }, w = 0.02, c = BLK, top = true, top_v = 0.1 } }, fin = { 200, 30, 40 }, tail = "round",
  dorsal = { a = 0.3, b = 0.9, h = 0.35, k = "round", c = { 60, 60, 200 } }, zone = "reef", v = 6, w = 2 }
fish{ name = "mandarinfish", L = 6, h = 0.34, shape = "fusi", back = ORG, mid = { 60, 150, 220 }, belly = ORG,
  pat = { { k = "diag", c = { 20, 170, 150 }, lean = 1.5, p = 2 } }, fin = { 255, 90, 30 }, tail = "round",
  dorsal = { a = 0.3, b = 0.8, h = 0.8, k = "round", c = { 255, 110, 20 } }, zone = "floorreef", v = 2.5, w = 2 }
fish{ name = "lionfish", L = 14, h = 0.4, shape = "fusi", back = { 190, 60, 40 }, mid = { 220, 120, 100 }, belly = { 250, 210, 190 },
  pat = { { k = "vbar", u = { 0.15, 0.3, 0.45, 0.6, 0.75, 0.9 }, w = 0.05, c = WHITE } }, fin = { 200, 70, 50 }, fans = true,
  dorsal = { a = 0.2, b = 0.7, h = 1.3, k = "spiny", c = { 200, 60, 40 }, c2 = WHITE }, tail = "round", zone = "reef", v = 3, w = 2, pred = 1 }
fish{ name = "porcupinefish", L = 13, h = 0.5, shape = "globe", back = { 150, 130, 90 }, mid = { 200, 180, 130 }, belly = CREAM,
  pat = { { k = "spots", c = { 40, 36, 30 }, d = 0.6, g = 3, top = true } }, fin = { 200, 190, 150 }, tail = "round", eye = { 120, 220, 160 },
  zone = "reef", v = 3, w = 2, curious = true, puffer = true }
fish{ name = "yellow boxfish", L = 9, h = 0.55, shape = "box", back = YEL, mid = YEL, belly = { 255, 230, 110 },
  pat = { { k = "spots", c = BLK, d = 0.35, g = 3 } }, fin = YEL, tail = "round", zone = "reef", v = 3, w = 2 }
fish{ name = "longhorn cowfish", L = 11, h = 0.5, shape = "box", back = GOLD, mid = YEL, belly = CREAM,
  pat = { { k = "spots", c = { 70, 110, 210 }, d = 0.3, g = 3 } }, cowhorns = true, fin = YEL, tail = "round", tl = 0.28, zone = "reef", v = 2.5, w = 1 }
fish{ name = "Picasso triggerfish", L = 12, h = 0.5, shape = "fusi", back = { 200, 180, 140 }, mid = { 236, 226, 200 }, belly = WHITE,
  pat = { { k = "diag", c = { 60, 50, 70 }, lean = -0.8, p = 3, duty = 0.3 }, { k = "mask", u0 = 0.18, u1 = 0.24, v0 = -0.4, v1 = 0.4, c = { 60, 120, 220 } }, { k = "split", u = 0.86, c = BLK } },
  fin = { 220, 210, 190 }, tail = "trunc", dorsal = { a = 0.55, b = 0.9, h = 0.35, k = "round" }, anal = { a = 0.55, b = 0.9, h = 0.35, k = "round" }, zone = "reef", v = 6, w = 2 }
fish{ name = "clown triggerfish", L = 15, h = 0.5, shape = "fusi", back = { 26, 26, 30 }, mid = { 30, 30, 34 }, belly = { 34, 34, 40 },
  pat = { { k = "spots", c = WHITE, d = 0.7, g = 3, seed = 5 }, { k = "blotch", u = 0.35, v = -0.55, ru = 0.12, rv = 0.25, c = YEL } },
  snoutc = ORG, snout = 0.03, fin = { 40, 40, 44 }, tail = "trunc", zone = "reef", v = 6, w = 1 }
fish{ name = "parrotfish", L = 20, h = 0.38, shape = "fusi", back = { 30, 170, 130 }, mid = { 60, 200, 170 }, belly = { 150, 220, 230 },
  pat = { { k = "mottle", c = { 240, 120, 200 }, d = 0.3, a = 0.5 } }, fin = { 240, 120, 200 }, tail = "lunate", eye = YEL,
  dorsal = { a = 0.2, b = 0.85, h = 0.2, k = "low" }, zone = "reef", v = 7, w = 2 }
fish{ name = "Napoleon wrasse", L = 38, h = 0.4, shape = "stout", back = { 30, 110, 120 }, mid = { 50, 150, 150 }, belly = { 120, 200, 190 },
  pat = { { k = "diag", c = { 30, 90, 110 }, lean = 0.2, p = 3, duty = 0.25 } }, hump = 0.45, fin = { 40, 130, 140 }, tail = "round", eye = YEL,
  dorsal = { a = 0.3, b = 0.9, h = 0.25, k = "low" }, anal = { a = 0.6, b = 0.9, h = 0.25, k = "low" }, zone = "open", v = 5, w = 1, curious = true, big = true, pred = 1 }
fish{ name = "cleaner wrasse", L = 6, h = 0.24, shape = "slim", back = { 90, 150, 250 }, mid = { 120, 190, 250 }, belly = WHITE,
  pat = { { k = "hstripe", v = -0.05, w = 0.22, c = BLK } }, fin = { 120, 180, 250 }, tail = "round", zone = "reef", v = 5, w = 2 }
fish{ name = "bird wrasse", L = 13, h = 0.28, shape = "slim", back = { 40, 160, 120 }, mid = { 70, 190, 150 }, belly = { 140, 220, 190 },
  snout = 0.1, snoutc = { 60, 170, 140 }, fin = { 60, 170, 200 }, tail = "lunate", zone = "reef", v = 8, w = 1 }
fish{ name = "anthias", L = 6, h = 0.34, shape = "fusi", back = { 255, 110, 90 }, mid = { 255, 140, 110 }, belly = { 255, 180, 140 },
  fin = { 255, 90, 160 }, tail = "fork", lyre = true, zone = "reef", v = 7, w = 3, school = { 6, 10 } }
fish{ name = "blue-green chromis", L = 5, h = 0.38, shape = "fusi", back = { 60, 200, 180 }, mid = { 100, 230, 220 }, belly = { 170, 240, 240 },
  fin = { 90, 220, 220 }, tail = "fork", zone = "reef", v = 7, w = 3, school = { 6, 12 } }
fish{ name = "damselfish", L = 6, h = 0.45, shape = "deep", back = ROYAL, mid = BLUE, belly = { 110, 150, 250 }, fin = BLUE, tailc = YEL, tail = "fork", zone = "reef", v = 7, w = 2 }
fish{ name = "Banggai cardinalfish", L = 7, h = 0.55, shape = "deep", back = SILV, mid = { 220, 226, 230 }, belly = WHITE,
  pat = { { k = "vbar", u = { 0.15, 0.4 }, w = 0.05, c = BLK }, { k = "spots", c = WHITE, d = 0.3, g = 2 } }, fin = BLK, tail = "fork", lyre = true,
  dorsal = { a = 0.3, b = 0.5, h = 0.7, k = "tri", c = BLK }, anal = { a = 0.5, b = 0.8, h = 0.6, k = "tri", c = BLK }, zone = "reef", v = 3, w = 1, school = { 3, 5 } }
fish{ name = "royal gramma", L = 6, h = 0.34, shape = "fusi", back = PURP, mid = { 170, 80, 230 }, belly = PINK,
  pat = { { k = "split", u = 0.5, lean = 0.1, c = YEL } }, fin = YEL, tail = "round", zone = "reef", v = 4, w = 2 }
fish{ name = "firefish", L = 6, h = 0.24, shape = "slim", back = CREAM, mid = { 255, 240, 200 }, belly = WHITE,
  pat = { { k = "grad", u0 = 0.45, u1 = 0.95, c = { 255, 60, 60 } } }, fin = RED, tail = "round",
  dorsal = { a = 0.12, b = 0.3, h = 1.4, k = "tri", c = { 250, 200, 150 } }, zone = "floorreef", v = 3, w = 2 }
fish{ name = "yellow watchman goby", L = 6, h = 0.24, shape = "slim", back = YEL, mid = YEL, belly = { 255, 230, 120 },
  pat = { { k = "spots", c = { 90, 170, 240 }, d = 0.3, g = 2 } }, fin = YEL, tail = "round", zone = "floor", v = 2, w = 1 }
fish{ name = "unicornfish", L = 22, h = 0.38, shape = "fusi", back = { 90, 100, 110 }, mid = { 120, 130, 140 }, belly = { 170, 176, 180 },
  horn = 0.12, fin = { 110, 120, 140 }, tail = "lunate", lyre = true, zone = "open", v = 8, w = 1, school = { 2, 4 } }
fish{ name = "Achilles tang", L = 11, h = 0.52, shape = "disc", back = { 30, 30, 34 }, mid = { 36, 36, 40 }, belly = { 40, 40, 46 },
  pat = { { k = "blotch", u = 0.82, v = 0.1, ru = 0.1, rv = 0.35, c = ORG } }, fin = { 40, 40, 50 }, tailc = ORG, tail = "lunate",
  dorsal = { a = 0.2, b = 0.95, h = 0.25, k = "low" }, zone = "reef", v = 8, w = 1 }
fish{ name = "powder blue tang", L = 11, h = 0.55, shape = "disc", back = { 90, 170, 240 }, mid = { 110, 190, 250 }, belly = { 150, 210, 250 },
  pat = { { k = "front", u = 0.14, c = BLK } }, fin = YEL, tailc = { 220, 240, 250 }, tail = "lunate",
  dorsal = { a = 0.2, b = 0.95, h = 0.3, k = "low", c = YEL }, zone = "reef", v = 8, w = 2 }
fish{ name = "sailfin tang", L = 13, h = 0.6, shape = "disc", back = { 200, 170, 90 }, mid = { 220, 200, 130 }, belly = CREAM,
  pat = { { k = "vbar", u = { 0.2, 0.35, 0.5, 0.65, 0.8 }, w = 0.05, c = { 110, 70, 40 } } }, fin = { 120, 80, 50 }, tail = "trunc",
  dorsal = { a = 0.2, b = 0.95, h = 1.0, k = "round", c = { 150, 110, 70 } }, anal = { a = 0.45, b = 0.95, h = 0.8, k = "round", c = { 150, 110, 70 } }, zone = "reef", v = 6, w = 1 }
fish{ name = "harlequin sweetlips", L = 18, h = 0.4, shape = "deep", back = WHITE, mid = { 240, 236, 220 }, belly = WHITE,
  pat = { { k = "spots", c = BLK, d = 0.55, g = 3 }, { k = "grad", u0 = 0.5, u1 = 1.2, c = YEL } }, fin = { 250, 220, 120 }, tail = "trunc", zone = "reef", v = 5, w = 1, curious = true }
fish{ name = "oriental sweetlips", L = 16, h = 0.38, shape = "deep", back = WHITE, mid = { 245, 240, 220 }, belly = YEL,
  pat = { { k = "lines", c = { 30, 30, 40 }, n = 5, duty = 0.4 } }, fin = YEL, tail = "trunc", zone = "reef", v = 5, w = 1, school = { 2, 4 } }
fish{ name = "batfish", L = 18, h = 1.1, shape = "disc", back = { 180, 180, 170 }, mid = { 200, 200, 190 }, belly = { 220, 218, 205 },
  pat = { { k = "vbar", u = { 0.18 }, w = 0.06, c = { 40, 40, 44 } }, { k = "vbar", u = { 0.4 }, w = 0.05, c = { 90, 90, 90 } } }, fin = { 170, 166, 150 }, tail = "trunc",
  dorsal = { a = 0.25, b = 0.9, h = 0.7, k = "round" }, anal = { a = 0.35, b = 0.9, h = 0.7, k = "round" }, zone = "open", v = 4, w = 1, curious = true, school = { 2, 5 } }
fish{ name = "giant grouper", L = 44, h = 0.42, shape = "stout", back = { 90, 80, 60 }, mid = { 120, 110, 80 }, belly = { 170, 160, 120 },
  pat = { { k = "mottle", c = { 50, 46, 36 }, d = 0.45, g = 3, a = 0.7 }, { k = "spots", c = { 200, 190, 120 }, d = 0.25, g = 3 } }, fin = { 100, 90, 70 }, tail = "round",
  dorsal = { a = 0.3, b = 0.9, h = 0.25, k = "low" }, anal = { a = 0.6, b = 0.9, h = 0.25, k = "round" }, zone = "open", v = 3.5, w = 1, curious = true, big = true, pred = 2 }
fish{ name = "coral trout", L = 20, h = 0.34, shape = "fusi", back = { 210, 50, 50 }, mid = { 240, 80, 70 }, belly = { 250, 140, 120 },
  pat = { { k = "spots", c = { 80, 180, 255 }, d = 0.4, g = 2 } }, fin = { 220, 60, 60 }, tail = "trunc", zone = "reef", v = 7, w = 1, pred = 1 }
fish{ name = "red snapper", L = 20, h = 0.38, shape = "fusi", back = { 220, 70, 70 }, mid = { 240, 110, 100 }, belly = { 250, 190, 180 }, fin = { 230, 80, 80 },
  tail = "fork", dorsal = { a = 0.25, b = 0.85, h = 0.3, k = "low" }, zone = "open", v = 8, w = 1, school = { 3, 6 }, pred = 1 }
fish{ name = "bluestripe snapper", L = 12, h = 0.34, shape = "fusi", back = YEL, mid = YEL, belly = { 255, 240, 200 },
  pat = { { k = "lines", c = { 60, 150, 255 }, n = 4, duty = 0.25, v0 = -0.8, v1 = 0.3 } }, fin = YEL, tail = "fork", zone = "open", v = 9, w = 3, school = { 8, 14 } }
fish{ name = "fusilier", L = 11, h = 0.3, shape = "slim", back = { 40, 110, 230 }, mid = { 90, 160, 240 }, belly = { 210, 230, 250 },
  pat = { { k = "hstripe", v = -0.55, w = 0.25, c = YEL, u0 = 0.2 } }, fin = { 80, 140, 240 }, tailc = YEL, tail = "fork", zone = "open", v = 12, w = 3, school = { 10, 16 } }
fish{ name = "sergeant major", L = 9, h = 0.52, shape = "deep", back = YEL, mid = { 230, 236, 210 }, belly = WHITE,
  pat = { { k = "vbar", u = { 0.25, 0.42, 0.59, 0.76 }, w = 0.04, c = BLK } }, fin = { 220, 220, 200 }, tail = "fork", zone = "reef", v = 7, w = 2, school = { 3, 6 } }
fish{ name = "flame hawkfish", L = 7, h = 0.36, shape = "fusi", back = RED, mid = { 255, 50, 40 }, belly = { 255, 90, 80 },
  pat = { { k = "hstripe", v = -0.2, w = 0.1, c = BLK, u0 = 0.4 } }, fin = RED, tail = "round", zone = "floorreef", v = 3, w = 1 }
fish{ name = "soldierfish", L = 10, h = 0.42, shape = "deep", back = { 220, 40, 40 }, mid = { 240, 70, 60 }, belly = { 250, 130, 110 },
  fin = { 240, 60, 50 }, tail = "fork", eye = { 250, 240, 240 }, zone = "reef", v = 5, w = 2, act = "n", school = { 3, 6 } }
fish{ name = "squirrelfish", L = 11, h = 0.36, shape = "fusi", back = { 220, 50, 50 }, mid = { 240, 90, 80 }, belly = WHITE,
  pat = { { k = "lines", c = WHITE, n = 5, duty = 0.3, v0 = -0.7, v1 = 0.6 } }, fin = { 250, 200, 60 }, tail = "fork", zone = "reef", v = 5, w = 2, act = "n" }
fish{ name = "frogfish", L = 12, h = 0.6, shape = "globe", back = YEL, mid = { 250, 200, 60 }, belly = { 255, 220, 120 },
  pat = { { k = "mottle", c = ORG, d = 0.4, g = 2 } }, fin = GOLD, tail = "round", zone = "floorreef", v = 0.8, w = 1, pred = 1 }
fish{ name = "scorpionfish", L = 14, h = 0.34, shape = "stout", back = { 130, 80, 70 }, mid = { 170, 110, 90 }, belly = { 200, 160, 140 },
  pat = { { k = "mottle", c = { 80, 50, 40 }, d = 0.5, g = 2, a = 0.8 }, { k = "mottle", c = { 220, 180, 150 }, d = 0.2, g = 2 } }, fin = { 150, 90, 80 }, tail = "round",
  dorsal = { a = 0.15, b = 0.8, h = 0.7, k = "spiny", c = { 150, 90, 80 }, c2 = { 200, 150, 120 } }, zone = "floor", v = 0.8, w = 1, pred = 1 }
fish{ name = "peacock flounder", kind = "flat", flounder = true, L = 16, back = { 150, 140, 110 }, belly = SAND, rings = { 90, 170, 255 }, zone = "floor", v = 3, w = 1 }
fish{ name = "goatfish", L = 14, h = 0.28, shape = "flatb", back = { 250, 220, 170 }, mid = { 250, 236, 210 }, belly = WHITE,
  pat = { { k = "hstripe", v = -0.1, w = 0.14, c = YEL } }, barbels = true, fin = YEL, tail = "fork", zone = "floor", v = 5, w = 2, school = { 2, 5 } }
fish{ name = "lined surgeonfish", L = 12, h = 0.5, shape = "disc", back = ORG, mid = { 255, 160, 60 }, belly = { 220, 220, 250 },
  pat = { { k = "lines", c = { 40, 90, 230 }, n = 6, duty = 0.4, v0 = -0.9, v1 = 0.4 } }, fin = { 60, 90, 220 }, tail = "lunate", zone = "reef", v = 8, w = 1 }
fish{ name = "foxface rabbitfish", L = 12, h = 0.5, shape = "deep", back = YEL, mid = YEL, belly = { 255, 236, 140 },
  pat = { { k = "mask", u0 = 0.0, u1 = 0.22, v0 = -0.9, v1 = 0.2, c = { 40, 30, 30 } }, { k = "blotch", u = 0.14, v = 0.5, ru = 0.1, rv = 0.35, c = WHITE } },
  fin = YEL, tail = "trunc", dorsal = { a = 0.2, b = 0.9, h = 0.4, k = "spiny", c = YEL, c2 = GOLD }, zone = "reef", v = 5, w = 1 }
fish{ name = "trumpetfish", L = 22, h = 0.09, shape = "needle", back = { 200, 160, 60 }, mid = { 230, 200, 100 }, belly = CREAM,
  snout = 0.12, fin = { 200, 150, 60 }, tail = "round", tl = 0.08, zone = "reef", v = 3, w = 1, pred = 1 }
fish{ name = "cornetfish", L = 30, h = 0.05, shape = "needle", back = { 150, 170, 150 }, mid = { 190, 200, 190 }, belly = WHITE,
  pat = { { k = "spots", c = { 90, 160, 230 }, d = 0.25, g = 3 } }, snout = 0.14, fin = { 170, 180, 170 }, tail = "point", tl = 0.25, zone = "open", v = 5, w = 1, pred = 1 }
fish{ name = "needlefish", L = 20, h = 0.07, shape = "needle", back = { 60, 140, 110 }, mid = SILV, belly = WHITE, snout = 0.14, fin = SILV, tail = "fork", tl = 0.1, zone = "surface", v = 14, w = 1, pred = 1 }
fish{ name = "hogfish", L = 18, h = 0.42, shape = "deep", back = { 230, 170, 150 }, mid = { 240, 200, 180 }, belly = WHITE,
  pat = { { k = "front", u = 0.2, lean = -0.2, c = { 150, 60, 60 } } }, snout = 0.06, fin = { 220, 150, 140 }, tail = "lunate",
  dorsal = { a = 0.15, b = 0.3, h = 0.8, k = "spiny", c = { 200, 100, 100 } }, zone = "floor", v = 5, w = 1 }
fish{ name = "harlequin tuskfish", L = 16, h = 0.4, shape = "deep", back = { 240, 150, 50 }, mid = { 240, 230, 220 }, belly = WHITE,
  pat = { { k = "vbar", u = { 0.2, 0.35, 0.5, 0.65, 0.8 }, w = 0.05, c = ORG, edge = { 60, 110, 220 }, ew = 0.02 } }, fin = { 60, 120, 230 }, tail = "round", zone = "reef", v = 6, w = 1 }
fish{ name = "bannerfish", L = 11, h = 0.75, shape = "disc", back = WHITE, mid = WHITE, belly = CREAM,
  pat = { { k = "vbar", u = { 0.25 }, w = 0.1, c = BLK }, { k = "vbar", u = { 0.62 }, w = 0.12, c = BLK }, { k = "split", u = 0.82, c = YEL } }, fin = YEL, tail = "trunc",
  snout = 0.06, dorsal = { a = 0.25, b = 0.7, h = 0.9, k = "sail", fil = 0.6, c = WHITE }, zone = "reef", v = 6, w = 2, school = { 3, 6 } }
fish{ name = "pipefish", L = 12, h = 0.05, shape = "needle", back = { 220, 140, 60 }, mid = { 240, 180, 80 }, belly = CREAM,
  pat = { { k = "vbar", u = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8 }, w = 0.03, c = { 150, 60, 40 } } }, snout = 0.08, fin = RED, tail = "round", tl = 0.1, zone = "floorreef", v = 1.2, w = 1 }

-- The open water: the big ones, the fast ones, the shoals
local function shark(t)
  t.shape = t.shape or "shark"; t.tail = t.tail or "shark"; t.gills = true; t.pred = t.pred or 3; t.act = t.act or "a"
  t.dorsal = t.dorsal or { a = 0.36, b = 0.52, h = 0.95, k = "shark" }
  t.dorsal2 = t.dorsal2 or { a = 0.74, b = 0.8, h = 0.35, k = "shark" }
  t.anal = t.anal or { a = 0.76, b = 0.82, h = 0.3, k = "shark" }
  t.pect = t.pect or 0.9; t.tl = t.tl or 0.24; t.th = t.th or 0.55; t.wig = t.wig or 1.4
  t.big = true; t.zone = t.zone or "open"
  return fish(t)
end
shark{ name = "whale shark", L = 150, h = 0.2, back = { 60, 80, 100 }, mid = { 80, 100, 120 }, belly = { 220, 226, 230 },
  pat = { { k = "dots", c = { 220, 230, 240 }, g = 4 }, { k = "vbar", u = { 0.3, 0.4, 0.5, 0.6 }, w = 0.006, c = { 200, 210, 220 }, top = true } },
  fin = { 60, 80, 100 }, v = 6, w = 0.25, pred = 0, zmax = 0.52, rare = true, act = "d", tail = "lunate" }
shark{ name = "great hammerhead", L = 80, h = 0.16, back = { 110, 116, 120 }, mid = { 140, 146, 150 }, belly = { 230, 230, 226 }, hammer = true,
  fin = { 110, 116, 120 }, v = 11, w = 0.5, dorsal = { a = 0.32, b = 0.46, h = 1.4, k = "shark" }, zmax = 0.85, rare = true }
shark{ name = "blacktip reef shark", L = 46, h = 0.16, back = { 150, 146, 130 }, mid = { 170, 166, 150 }, belly = { 236, 234, 226 },
  pat = { { k = "blotch", u = 0.44, v = -1.2, ru = 0.05, rv = 0.3, c = BLK } }, fin = { 150, 146, 130 }, v = 13, w = 1.2 }
shark{ name = "whitetip reef shark", L = 50, h = 0.14, back = { 120, 124, 126 }, mid = { 150, 154, 156 }, belly = { 226, 226, 222 },
  fin = { 120, 124, 126 }, v = 10, w = 1, act = "n", dorsal = { a = 0.34, b = 0.5, h = 0.9, k = "shark", c = { 210, 210, 210 } } }
shark{ name = "grey reef shark", L = 56, h = 0.17, back = { 100, 110, 120 }, mid = { 130, 140, 150 }, belly = { 230, 232, 232 }, fin = { 100, 110, 120 }, v = 14, w = 1 }
shark{ name = "sand tiger shark", L = 70, h = 0.18, back = { 150, 130, 100 }, mid = { 170, 150, 120 }, belly = { 225, 220, 205 },
  pat = { { k = "spots", c = { 110, 80, 60 }, d = 0.3, g = 4, top = true } }, fin = { 150, 130, 100 }, v = 8, w = 0.7, zmax = 0.9,
  dorsal = { a = 0.42, b = 0.55, h = 0.8, k = "shark" }, dorsal2 = { a = 0.62, b = 0.7, h = 0.7, k = "shark" } }
shark{ name = "zebra shark", L = 52, h = 0.14, back = { 200, 180, 120 }, mid = { 215, 200, 150 }, belly = { 235, 225, 190 },
  pat = { { k = "spots", c = { 60, 50, 40 }, d = 0.6, g = 3 } }, fin = { 190, 170, 120 }, tail = "veil", tl = 0.4, th = 0.3, v = 6, w = 0.8, pred = 2, zone = "floor",
  dorsal = { a = 0.4, b = 0.52, h = 0.6, k = "shark" } }
shark{ name = "nurse shark", L = 56, h = 0.14, shape = "flatb", back = { 150, 120, 80 }, mid = { 170, 140, 100 }, belly = { 210, 190, 150 },
  barbels = true, fin = { 150, 120, 80 }, tail = "veil", tl = 0.3, th = 0.3, v = 5, w = 0.8, act = "n", pred = 2, zone = "floor",
  dorsal = { a = 0.55, b = 0.65, h = 0.6, k = "round" }, dorsal2 = { a = 0.7, b = 0.78, h = 0.5, k = "round" } }
shark{ name = "epaulette shark", L = 24, h = 0.12, shape = "flatb", back = { 200, 176, 130 }, mid = { 215, 195, 150 }, belly = CREAM,
  pat = { { k = "spots", c = { 90, 70, 50 }, d = 0.4, g = 2 }, { k = "blotch", u = 0.3, v = -0.3, ru = 0.05, rv = 0.35, c = BLK, ring = WHITE } },
  fin = { 190, 170, 130 }, tail = "veil", tl = 0.35, th = 0.25, v = 3, w = 1, act = "n", pred = 1, zone = "floor", gills = false }
shark{ name = "sawfish", L = 90, h = 0.13, shape = "flatb", back = { 150, 140, 110 }, mid = { 170, 160, 130 }, belly = { 225, 220, 205 }, saw = 0.22,
  fin = { 150, 140, 110 }, v = 5, w = 0.25, rare = true, pred = 2, zone = "floor", zmax = 0.75 }
shark{ name = "shovelnose guitarfish", L = 36, h = 0.12, shape = "flatb", back = { 190, 170, 130 }, mid = { 205, 185, 145 }, belly = CREAM,
  fin = { 190, 170, 130 }, v = 4, w = 0.7, pred = 1, zone = "floor", gills = false }
def{ name = "manta ray", kind = "ray", manta = true, L = 70, span = 0.75, back = { 30, 34, 44 }, belly = { 236, 238, 240 }, edge2 = { 40, 44, 56 },
  patch = { 200, 206, 214 }, tailL = 0.25, zone = "open", v = 7, w = 0.6, act = "a", pred = 0, big = true, zmax = 0.8, rare = true }
def{ name = "spotted eagle ray", kind = "ray", L = 40, span = 0.6, back = { 40, 44, 60 }, belly = WHITE, spots = WHITE, tailL = 1.1,
  zone = "open", v = 8, w = 1, act = "d", pred = 0, big = true, school = { 1, 3 } }
def{ name = "southern stingray", kind = "flat", L = 34, back = { 120, 110, 90 }, belly = CREAM, tailL = 0.6, zone = "floor", v = 3, w = 1, act = "a", pred = 0 }
def{ name = "blue-spotted ribbontail ray", kind = "flat", L = 18, back = { 180, 160, 100 }, spots2 = { 60, 150, 255 }, belly = CREAM, tailL = 0.5,
  tailc = { 60, 140, 240 }, zone = "floor", v = 3, w = 1, act = "a", pred = 0 }
fish{ name = "ocean sunfish", L = 60, h = 0.9, shape = "mola", back = { 130, 140, 150 }, mid = { 170, 176, 180 }, belly = { 210, 212, 214 },
  pat = { { k = "mottle", c = { 220, 222, 224 }, d = 0.3, g = 3 } }, fin = { 140, 150, 160 }, tail = "round", tl = 0.08, th = 0.35,
  dorsal = { a = 0.72, b = 0.85, h = 1.4, k = "tri" }, anal = { a = 0.72, b = 0.85, h = 1.3, k = "tri" }, zone = "open", v = 3, w = 0.25,
  big = true, rare = true, curious = true, zmax = 0.7, wig = 0.2 }
fish{ name = "bluefin tuna", L = 34, h = 0.26, shape = "fusi", back = { 20, 40, 110 }, mid = { 150, 170, 200 }, belly = SILV,
  fin = { 60, 70, 100 }, tailc = { 40, 50, 90 }, tail = "lunate", th = 0.6, dorsal = { a = 0.3, b = 0.4, h = 0.5, k = "tri", c = YEL },
  dorsal2 = { a = 0.6, b = 0.64, h = 0.5, k = "tri" }, pat = { { k = "vbar", u = { 0.75, 0.8, 0.85 }, w = 0.012, c = YEL, top = true } },
  zone = "open", v = 22, w = 1, act = "d", school = { 4, 7 }, pred = 2, big = true }
fish{ name = "giant trevally", L = 34, h = 0.36, shape = "deep", back = { 90, 110, 130 }, mid = { 170, 180, 190 }, belly = SILV,
  fin = { 110, 120, 130 }, tail = "lunate", th = 0.55, zone = "open", v = 16, w = 1, act = "a", pred = 2, school = { 1, 4 }, big = true }
fish{ name = "great barracuda", L = 40, h = 0.15, shape = "long", back = { 90, 110, 120 }, mid = { 190, 200, 210 }, belly = WHITE,
  pat = { { k = "vbar", u = { 0.35, 0.45, 0.55, 0.65, 0.75 }, w = 0.02, c = { 70, 80, 90 }, top = true } },
  fin = { 120, 130, 140 }, tail = "fork", dorsal = { a = 0.3, b = 0.36, h = 0.9, k = "tri" }, dorsal2 = { a = 0.72, b = 0.78, h = 0.6, k = "tri" },
  zone = "open", v = 10, w = 1, act = "a", pred = 2, big = true }
fish{ name = "chevron barracuda", L = 22, h = 0.15, shape = "long", back = { 110, 130, 140 }, mid = { 200, 210, 220 }, belly = WHITE,
  pat = { { k = "diag", c = { 90, 100, 110 }, lean = 1.2, p = 3, duty = 0.2 } }, fin = { 150, 160, 170 }, tail = "fork",
  zone = "open", v = 11, w = 1, act = "a", pred = 2, school = { 6, 10 } }
fish{ name = "yellowtail amberjack", L = 30, h = 0.28, shape = "fusi", back = { 80, 110, 140 }, mid = { 190, 200, 200 }, belly = WHITE,
  pat = { { k = "hstripe", v = -0.1, w = 0.1, c = YEL } }, fin = YEL, tail = "fork", zone = "open", v = 16, w = 1, pred = 2, school = { 3, 6 }, big = true }
fish{ name = "mahi-mahi", L = 30, h = 0.3, shape = "stout", back = { 30, 150, 130 }, mid = { 120, 200, 110 }, belly = YEL,
  pat = { { k = "spots", c = { 40, 110, 210 }, d = 0.3, g = 2 } }, fin = { 40, 120, 200 }, tail = "fork",
  dorsal = { a = 0.05, b = 0.95, h = 0.35, k = "low", c = { 40, 100, 200 } }, zone = "surface", v = 16, w = 0.6, pred = 2, big = true }
fish{ name = "king mackerel", L = 26, h = 0.18, shape = "long", back = { 60, 90, 130 }, mid = SILV, belly = WHITE,
  fin = { 90, 110, 140 }, tail = "lunate", zone = "open", v = 18, w = 0.8, pred = 2, school = { 2, 4 } }
def{ name = "sardines", kind = "ball", L = 3, n = 44, c = { 176, 200, 226 }, zone = "open", v = 6, w = 1.6, act = "d", pred = 0 }
def{ name = "anchovies", kind = "river", L = 2, n = 36, c = { 160, 190, 210 }, zone = "open", v = 10, w = 1.2, act = "a", pred = 0 }
def{ name = "green sea turtle", kind = "turtle", L = 40, shell = { 110, 100, 60 }, scute = { 70, 60, 40 }, plastron = CREAM, skin = { 120, 110, 80 },
  zone = "open", v = 4, w = 1, act = "a", curious = true, big = true }
def{ name = "hawksbill turtle", kind = "turtle", L = 32, shell = { 170, 110, 50 }, streak = { 90, 50, 30 }, scute = { 110, 70, 30 }, plastron = CREAM, skin = { 140, 110, 80 },
  zone = "open", v = 4, w = 0.8, act = "a", curious = true, big = true }
def{ name = "loggerhead turtle", kind = "turtle", L = 44, shell = { 150, 80, 40 }, scute = { 100, 50, 30 }, plastron = { 230, 200, 150 }, skin = { 170, 120, 80 },
  zone = "open", v = 3.5, w = 0.5, act = "a", curious = true, big = true, rare = true }

-- Jellies, and the rest that is not a fish
local function jelly(t) t.kind = "jelly"; t.zone = t.zone or "open"; t.act = t.act or "a"; t.pred = 0; t.glow = true; return def(t) end
jelly{ name = "moon jellyfish", L = 12, bell = { 150, 160, 200 }, rim = { 210, 216, 240 }, gonads = { 220, 120, 200 }, nt = 14, tent = 0.6,
  ac = { 200, 200, 230 }, arms = 4, armL = 0.6, v = 1.5, w = 2, school = { 2, 5 } }
jelly{ name = "Pacific sea nettle", L = 12, bell = { 200, 120, 50 }, rim = { 250, 170, 80 }, stripes = { 170, 80, 40 }, tc = { 180, 60, 40 }, nt = 10, tent = 2.2,
  ac = { 250, 220, 180 }, arms = 4, armL = 1.1, v = 1.5, w = 1.5 }
jelly{ name = "lion's mane jellyfish", L = 18, bell = { 200, 90, 50 }, rim = { 250, 150, 80 }, tc = { 230, 120, 70 }, nt = 22, tent = 2.4,
  ac = { 250, 180, 120 }, arms = 5, armL = 0.9, v = 1.2, w = 0.6, rare = true }
jelly{ name = "purple-striped jelly", L = 11, bell = { 200, 190, 220 }, rim = { 236, 226, 250 }, stripes = { 140, 60, 170 }, tc = { 130, 60, 150 }, nt = 8, tent = 1.8,
  ac = { 220, 180, 230 }, arms = 4, armL = 1.0, v = 1.4, w = 1 }
jelly{ name = "fried egg jellyfish", L = 13, flat = true, bell = { 240, 230, 190 }, rim = { 250, 244, 220 }, dome = { 255, 190, 40 }, nt = 0, tent = 0.6,
  ac = { 230, 210, 160 }, armtip = { 170, 90, 220 }, arms = 5, armL = 0.9, v = 1.2, w = 0.8 }
jelly{ name = "comb jelly", L = 8, comb = true, v = 1, w = 1, school = { 1, 3 } }
def{ name = "common octopus", kind = "octopus", L = 26, body = { 170, 90, 70 }, under = { 210, 150, 120 }, spot = { 230, 190, 160 },
  zone = "floor", v = 3, w = 1, act = "n", curious = true, pred = 1 }
def{ name = "blue-ringed octopus", kind = "octopus", L = 12, body = { 210, 170, 80 }, under = { 230, 200, 120 }, spot = { 40, 110, 255 },
  zone = "floor", v = 2, w = 0.4, act = "n", rare = true }
def{ name = "broadclub cuttlefish", kind = "cuttle", L = 24, body = { 190, 160, 140 }, under = { 230, 210, 190 }, zebra = { 70, 50, 50 }, skirt = { 220, 210, 200 },
  zone = "reef", v = 3, w = 1, act = "a", curious = true, pred = 1 }
def{ name = "flamboyant cuttlefish", kind = "cuttle", L = 12, body = { 160, 70, 60 }, under = { 250, 200, 80 }, zebra = { 90, 30, 40 }, skirt = { 250, 220, 60 },
  arms = { 255, 90, 160 }, zone = "floor", v = 1.5, w = 0.5, act = "d", rare = true }
def{ name = "reef squid", kind = "cuttle", L = 16, body = { 200, 180, 200 }, under = { 230, 220, 230 }, chrom = { 150, 80, 120 }, skirt = { 220, 210, 230 },
  zone = "open", v = 6, w = 1, act = "n", school = { 3, 5 }, pred = 1 }
def{ name = "chambered nautilus", kind = "nautilus", L = 14, shell = { 240, 226, 200 }, stripe = { 160, 80, 40 }, under = { 250, 240, 225 }, skin = { 220, 180, 150 },
  zone = "open", v = 1.5, w = 0.5, act = "n", rare = true }
def{ name = "yellow seahorse", kind = "seahorse", L = 12, body = { 250, 200, 40 }, ring = { 230, 170, 30 }, belly = { 255, 230, 120 }, fin = { 255, 240, 180 },
  zone = "grass", v = 0.8, w = 1.5, act = "d" }
def{ name = "big-bellied seahorse", kind = "seahorse", L = 16, body = { 230, 120, 60 }, ring = { 200, 90, 40 }, belly = { 250, 200, 150 }, fin = { 250, 220, 190 },
  zone = "grass", v = 0.8, w = 1, act = "d" }
def{ name = "leafy seadragon", kind = "seahorse", leafy = true, L = 22, body = { 220, 180, 80 }, ring = { 180, 150, 60 }, belly = { 240, 210, 120 },
  leaf = { 150, 170, 60 }, leaf2 = { 220, 200, 100 }, fin = { 230, 220, 180 }, zone = "grass", v = 0.9, w = 0.6, act = "d", rare = true }
local function crust(t) t.kind = "crust"; t.zone = t.zone or "floor"; t.pred = 0; t.act = t.act or "n"; return def(t) end
crust{ name = "spiny lobster", crust = "lobster", L = 20, body = { 200, 110, 50 }, band = { 230, 170, 90 }, ant = { 220, 150, 80 }, legs = { 230, 180, 110 },
  spots = WHITE, fan = { 210, 130, 60 }, v = 2, w = 1.2 }
crust{ name = "European lobster", crust = "lobster", claws = true, L = 18, body = { 40, 60, 120 }, band = { 60, 80, 150 }, ant = { 200, 80, 60 },
  legs = { 70, 90, 160 }, claw = { 50, 70, 140 }, fan = { 50, 70, 140 }, v = 1.8, w = 0.8 }
crust{ name = "red rock crab", crust = "crab", L = 12, body = { 220, 60, 40 }, under = { 250, 170, 120 }, legs = { 230, 90, 60 }, claw = { 240, 80, 50 }, v = 3, w = 1.2, act = "a" }
crust{ name = "hermit crab", crust = "crab", L = 9, body = { 200, 150, 110 }, under = { 230, 200, 160 }, legs = { 230, 60, 40 }, claw = { 240, 70, 50 }, v = 1.5, w = 1, act = "a" }
crust{ name = "cleaner shrimp", crust = "shrimp", L = 8, body = { 240, 190, 180 }, band = { 220, 50, 50 }, stripe = WHITE, ant = WHITE, legs = { 250, 220, 220 }, v = 2.5, w = 1.2, act = "a", zone = "floorreef" }
crust{ name = "peacock mantis shrimp", crust = "mantis", L = 12, body = { 40, 170, 110 }, band = { 60, 200, 160 }, ant = { 250, 140, 40 },
  legs = { 240, 60, 60 }, fan = { 60, 120, 240 }, eyec = { 250, 90, 180 }, v = 3, w = 0.8, act = "d" }
local function still(t) t.kind = "static"; t.zone = t.zone or "floor"; t.pred = 0; t.act = "a"; t.v = t.v or 0.1; return def(t) end
still{ name = "red sea star", still = "star", L = 11, body = { 220, 40, 40 }, bumps = WHITE, w = 1 }
still{ name = "blue linckia star", still = "star", L = 13, body = { 40, 90, 230 }, fat = 0.2, w = 1 }
still{ name = "cushion star", still = "star", L = 10, body = { 250, 140, 40 }, fat = 0.55, bumps = { 250, 230, 160 }, w = 0.8 }
still{ name = "chocolate chip star", still = "star", L = 11, body = { 230, 200, 150 }, bumps = { 60, 40, 30 }, fat = 0.42, w = 0.8 }
still{ name = "long-spined urchin", still = "urchin", L = 12, body = { 40, 30, 50 }, spine = { 60, 50, 70 }, eyes = { 60, 140, 250 }, w = 1 }
still{ name = "pencil urchin", still = "urchin", L = 9, body = { 150, 50, 50 }, spine = { 200, 90, 70 }, w = 0.8 }
still{ name = "sea cucumber", still = "cucumber", L = 16, body = { 70, 50, 40 }, under = { 110, 90, 70 }, spot = { 230, 200, 60 }, w = 0.8 }
still{ name = "feather star", still = "feather", L = 12, body = { 250, 200, 40 }, pinn = { 250, 120, 40 }, w = 1, zone = "reeftop" }
still{ name = "crimson feather star", still = "feather", L = 11, body = { 230, 40, 60 }, pinn = { 250, 140, 160 }, w = 0.8, zone = "reeftop" }
def{ name = "green moray", kind = "moray", L = 34, body = { 100, 150, 60 }, eye = { 250, 240, 120 }, zone = "hole", v = 0, w = 0, act = "a", pred = 1 }
def{ name = "honeycomb moray", kind = "moray", L = 30, body = { 60, 44, 30 }, net = { 230, 210, 140 }, eye = { 250, 240, 200 }, zone = "hole", v = 0, w = 0, act = "a", pred = 1 }
def{ name = "garden eels", kind = "eels", L = 10, c = { 230, 226, 210 }, spots = { 60, 50, 50 }, zone = "floor", v = 0, w = 0, act = "d" }

for i = 1, #SPECIES do BY_NAME[SPECIES[i].name] = SPECIES[i] end
end

-- ── poses: drawn once, cut out, stamped ever after ──────────────────────────
local PAINT = {
  fish = paint_fish, ray = paint_ray, flat = paint_flat, jelly = paint_jelly,
  turtle = paint_turtle, seahorse = paint_seahorse, octopus = paint_octopus,
  cuttle = paint_cuttle, nautilus = paint_nautilus, crust = paint_crust,
  static = paint_static, moray = paint_moray,
}
-- Beats of the tail (or the bell, the flipper, the legs) a kind is cut in.
local NPH = { fish = 4, ray = 6, flat = 4, jelly = 6, turtle = 6, seahorse = 4, octopus = 6,
              cuttle = 6, nautilus = 4, crust = 4, static = 1, moray = 4 }
local TURN = { 1.0, 0.62, 0.3 }
-- Sizes go up in steps of 18%: a fish swimming nearer moves through them.
local STEP = 1.18
local LOGSTEP = log(STEP)
local function size_idx(lp) return max(0, floor(log(max(lp, 2) / 2) / LOGSTEP + 0.5)) end
local function size_len(i) return 2 * STEP ^ i end

local POSE = {}
local poseBytes, poseCount = 0, 0
local POSE_BUDGET = 1100 * 1024           -- the effect's heap is 4 MB, and cutting leaves garbage
local WANT = {}                            -- key -> request, filled while moving, cut before drawing
local frameNo = 0

local function pose_key(sp, si, ti, var, pn)
  return sp.id * 1000000 + si * 10000 + ti * 1000 + var * 100 + pn
end

local function cut(sp, si, ti, var, pn)
  px.clear(0, 0, 0)
  local nph = NPH[sp.kind] or 1
  local ph = pn * TAU / nph
  if sp.still == "urchin" or sp.still == "feather" then ph = pn * TAU / 4 end
  local lp = size_len(si)
  PAINT[sp.kind](sp, lp, TURN[ti], ph, var)
  -- Cut from a frame around the animal, not the whole canvas: grab is
  -- charged by the pixels it looks at.
  local fw, fh = min(W, floor(lp * 2.2) + 14), min(H, floor(lp * 1.9) + 14)
  local fx, fy = max(0, CX - floor(fw / 2)), max(0, CY - floor(fh / 2))
  local s, dx, dy = grab(fx, fy, fw, fh)
  local key = pose_key(sp, si, ti, var, pn)
  if s then
    POSE[key] = { s, CX - fx - dx, CY - fy - dy, s:byte(1), frameNo }
    poseBytes = poseBytes + #s + 64
  else
    POSE[key] = false
  end
  poseCount = poseCount + 1
end

-- What cutting a pose costs, in Lua instructions, as measured on the host
-- (fxhost, one cut a frame, 2026-09-24): a fixed part for clearing and
-- cutting, and a part that grows with the area the animal covers.
local CUT_K = { fish = 115, jelly = 80, nautilus = 52, cuttle = 29, octopus = 24, turtle = 20,
                ray = 17, flat = 13, crust = 21, static = 27, seahorse = 11, moray = 8 }
local function cut_cost(sp, si)
  local lp = size_len(si)
  local a = lp * lp * (CUT_K[sp.kind] or 30)
  if sp.kind == "fish" then a = a * (sp.h or 0.4) * 1.6 end
  return 3500 + a
end

local function want(sp, si, ti, var, pn, urgent)
  local key = pose_key(sp, si, ti, var, pn)
  local p = POSE[key]
  if p then p[5] = frameNo; return p end
  if p == nil then
    local w = WANT[key]
    if not w then WANT[key] = { sp, si, ti, var, pn, urgent and 2 or 1 }
    elseif urgent then w[6] = 2 end
  end
  return nil
end

-- A pose that is close enough while the right one waits to be cut: another
-- beat, side-on, or a size or two either way.
local function nearest(sp, si, ti, var, pn)
  local nph = NPH[sp.kind] or 1
  for d = 1, nph - 1 do
    local p = POSE[pose_key(sp, si, ti, var, (pn + d) % nph)]
    if p then p[5] = frameNo; return p end
  end
  for ds = 1, 3 do
    for s = -1, 1, 2 do
      local s2 = si + ds * s
      if s2 >= 0 then
        for t2 = ti, 1, -1 do
          for p2 = 0, nph - 1 do
            local p = POSE[pose_key(sp, s2, t2, var, p2)]
            if p then p[5] = frameNo; return p end
          end
        end
      end
    end
  end
  if var > 0 then return nearest(sp, si, ti, 0, pn) end
  return nil
end

-- A pose that costs more than a slice is painted a slice a frame: each slice
-- on a black canvas, cut out, and when all are in, stamped side by side and
-- cut once more whole. The animal wears a near pose meanwhile.
local SLICE = 16000                       -- instructions a frame for cutting
local function cut_slice(w)
  local sp, si = w[1], w[2]
  local lp = size_len(si)
  local k = w.k
  local lo, hi = floor(CX - lp * 0.75), floor(CX + lp * 0.75)
  local i = w.i
  local a = i == 1 and 0 or floor(lo + (hi - lo) * (i - 1) / k)
  local b = i == k and W - 1 or floor(lo + (hi - lo) * i / k) - 1
  CLIP0, CLIP1 = a, b
  px.clear(0, 0, 0)
  local nph = NPH[sp.kind] or 1
  PAINT[sp.kind](sp, lp, TURN[w[3]], w[5] * TAU / nph, w[4])
  CLIP0, CLIP1 = 0, W - 1
  local s, dx, dy = grab(a, 0, b - a + 1, H)
  if s then w.pieces[#w.pieces + 1] = { s, a + dx, dy } end
  w.i = i + 1
  if w.i <= k then return false end
  px.clear(0, 0, 0)
  for j = 1, #w.pieces do
    local pc = w.pieces[j]
    blit(pc[1], pc[2], pc[3])
  end
  local s2, dx2, dy2 = grab(0, 0, W, H)
  local key = pose_key(sp, si, w[3], w[4], w[5])
  if s2 then
    POSE[key] = { s2, CX - dx2, CY - dy2, s2:byte(1), frameNo }
    poseBytes = poseBytes + #s2 + 64
  else
    POSE[key] = false
  end
  poseCount = poseCount + 1
  return true
end

local function cut_wanted(budget)
  -- A big one a slice a frame; then the urgent (an animal with no pose at
  -- all is not on screen), then the rest, while the frame's budget lasts.
  local spent = 0
  for key, w in pairs(WANT) do
    if w.k then
      if cut_slice(w) then WANT[key] = nil end
      spent = SLICE
      break
    end
  end
  for pass = 2, 1, -1 do
    for key, w in pairs(WANT) do
      if w[6] == pass and not w.k then
        local cost = cut_cost(w[1], w[2])
        if cost > SLICE and budget <= SLICE then
          if spent == 0 then
            w.k, w.i, w.pieces = floor(cost / SLICE) + 1, 1, {}
            if cut_slice(w) then WANT[key] = nil end
            spent = SLICE
          end
        elseif spent + cost <= budget or spent == 0 then
          cut(w[1], w[2], w[3], w[4], w[5])
          WANT[key] = nil
          spent = spent + cost
        end
        if spent >= budget then return end
      end
    end
  end
end

local function forget_old()
  if poseBytes < POSE_BUDGET then return end
  local keep = frameNo - 15 * 20
  for pass = 1, 2 do
    for key, p in pairs(POSE) do
      if p and p[5] < keep then
        poseBytes = poseBytes - #p[1] - 64
        POSE[key] = nil
      elseif p == false then
        POSE[key] = nil
      end
    end
    if poseBytes < POSE_BUDGET * 0.8 then return end
    keep = frameNo - 15 * 4
  end
end

-- ── the reef ────────────────────────────────────────────────────────────────
-- Rock, coral and weed, cut out when the tank opens, at full light. They sit
-- on the sand at their own depth and are drawn in depth order with the
-- animals, so a fish can pass behind a rock and in front of the next.
local DECOR, ANEMONE, GRASS = {}, {}, {}
local HOLE = nil                           -- where the moray lives
local build_reef
do
local BASE = 60                            -- decor is painted standing on this row

local ALGAE = { 120, 150, 70 }
local function paint_rock(w, h, col, sd, hole)
  local n = 3 + floor(w / 10)
  local crev, lit, under = cmix(col, { 30, 30, 40 }, 0.55), cmix(col, { 200, 190, 170 }, 0.35), cmix(col, { 20, 20, 30 }, 0.5)
  for i = 0, n - 1 do
    local t = n > 1 and i / (n - 1) or 0.5
    local bx = CX - w / 2 + w * t + (hash2(sd, i) - 0.5) * 4
    local rh = h * (0.55 + 0.45 * sin(pi * (0.15 + 0.7 * t))) * (0.8 + 0.3 * hash2(i, sd))
    local rw = w / n * 0.9 + 3
    blob(bx, BASE - rh / 2, rw, rh / 2, col, under, function(x, y, dy)
      local m = hash2(floor(x / 2) + sd, floor(y / 2))
      if m < 0.18 then return crev end
      if m > 0.9 and dy < 0 then return lit end
      if m > 0.8 and dy < -0.3 then return ALGAE end    -- algae on the lit top
      return nil
    end)
  end
  if hole then
    local hx, hy = CX + hole[1], BASE - hole[2]
    blob(hx, hy, hole[3], hole[3] * 0.7, { 8, 10, 14 })
  end
end

local function paint_brain(r, col)
  local groove = cmix(col, { 60, 40, 30 }, 0.45)
  blob(CX, BASE - r * 0.55, r, r * 0.6, col, cmix(col, { 40, 30, 20 }, 0.5), function(x, y, dy, dx)
    if sin(x * 1.3 + sin(y * 0.9) * 2.2) > 0.55 then return groove end
    return nil
  end)
end

local function branch(x, y, a, len, depth, col, tip)
  local x1, y1 = x + cos(a) * len, y + sin(a) * len
  L2(x, y, x1, y1, col, 1.0 - depth * 0.05)
  if depth >= 3 or len < 2 then P(x1, y1, tip); return end
  branch(x1, y1, a - 0.45, len * 0.72, depth + 1, col, tip)
  branch(x1, y1, a + 0.4, len * 0.7, depth + 1, col, tip)
end

local function paint_stag(h, col, tip)
  for i = -1, 1 do
    branch(CX + i * h * 0.25, BASE, -pi / 2 + i * 0.35, h * 0.38, 0, col, tip)
  end
end

local function paint_table(w, col)
  L2(CX, BASE, CX, BASE - w * 0.35, cmix(col, { 60, 50, 40 }, 0.4), 1)
  L2(CX + 1, BASE, CX + 1, BASE - w * 0.35, cmix(col, { 60, 50, 40 }, 0.4), 0.8)
  blob(CX, BASE - w * 0.4, w / 2, max(1.5, w * 0.07), col, cmix(col, { 40, 40, 40 }, 0.5))
end

local function paint_fan(h, col, ph)
  local sway = 0.08 * sin(ph)
  local function fb(x, y, a, len, d)
    local x1, y1 = x + cos(a + sway * d) * len, y + sin(a + sway * d) * len
    L2(x, y, x1, y1, col, 1.05 - d * 0.08)
    if d >= 4 or len < 2 then return end
    fb(x1, y1, a - 0.35, len * 0.75, d + 1)
    fb(x1, y1, a + 0.35, len * 0.75, d + 1)
  end
  fb(CX, BASE, -pi / 2, h * 0.34, 0)
end

local function paint_sponge(n, h, col)
  for i = 0, n - 1 do
    local x = CX + (i - (n - 1) / 2) * 3.2
    local hh = h * (0.6 + 0.4 * hash2(i, 7))
    for o = 0, 1 do L2(x + o, BASE, x + o, BASE - hh, col, 1.05 - o * 0.25) end
    L2(x - 1, BASE - hh, x + 2, BASE - hh, cmix(col, { 255, 255, 255 }, 0.25), 1)
    P(x, BASE - hh + 1, { 30, 16, 30 }); P(x + 1, BASE - hh + 1, { 30, 16, 30 })
  end
end

local function paint_anemone(r, col, tip, ph)
  blob(CX, BASE - r * 0.25, r * 0.7, r * 0.3, cmix(col, { 90, 60, 40 }, 0.5))
  for i = 0, 17 do
    local a = -pi * (0.08 + 0.84 * i / 17)
    local len = r * (0.8 + 0.3 * hash2(i, 3))
    local x0, y0 = CX + cos(a) * r * 0.5, BASE - r * 0.45
    local sw = 0.35 * sin(ph + i * 0.4)
    local pts = {}
    for s = 0, 4 do
      local t = s / 4
      pts[#pts + 1] = { x0 + cos(a + sw * t) * len * t, y0 + sin(a + sw * t) * len * t }
    end
    stroke(pts, 1, 1, col, 1)
    P(pts[5][1], pts[5][2], tip)
  end
end

local function paint_grass(h, n, col, ph)
  for i = 0, n - 1 do
    local x0 = CX + (i - (n - 1) / 2) * 2
    local hh = h * (0.6 + 0.4 * hash2(i, 11))
    local pts = {}
    for s = 0, 5 do
      local t = s / 5
      pts[#pts + 1] = { x0 + sin(ph + i * 0.9) * 3 * t * t, BASE - hh * t }
    end
    stroke(pts, 1, 1, (i % 3 == 0) and cmix(col, { 200, 220, 120 }, 0.3) or col, 1.05)
  end
end

local function paint_kelp(h, col, ph)
  local pts = {}
  for s = 0, 12 do
    local t = s / 12
    local x = CX + sin(ph + t * 2.5) * 4 * t
    pts[#pts + 1] = { x, BASE - h * t }
    if s % 2 == 1 and s < 12 then
      local side = (s % 4 == 1) and 1 or -1
      L2(x, BASE - h * t, x + side * (4 + 2 * sin(ph + s)), BASE - h * t - 3, cmix(col, { 220, 200, 80 }, 0.25), 1)
    end
  end
  stroke(pts, 2, 1, col, 1)
end

local function paint_clam(w)
  blob(CX, BASE - w * 0.2, w / 2, w * 0.2, { 150, 140, 120 }, { 90, 80, 70 })
  for x = floor(CX - w / 2 + 1), floor(CX + w / 2 - 1) do
    local c = (floor(x / 2) % 2 == 0) and { 40, 120, 230 } or { 60, 200, 190 }
    P(x, BASE - w * 0.38 + sin(x * 0.8) * 0.8, c)
  end
end

local function paint_soft(h, col)
  L2(CX, BASE, CX, BASE - h * 0.6, col, 0.9); L2(CX + 1, BASE, CX + 1, BASE - h * 0.6, col, 0.8)
  local polyp = cmix(col, { 240, 240, 200 }, 0.35)
  blob(CX, BASE - h * 0.7, h * 0.5, h * 0.25, col, cmix(col, { 60, 60, 40 }, 0.4), function(x, y)
    if hash2(x, y) < 0.3 then return polyp end
  end)
end

-- A decor piece: its frames (1 for rock, 6 for things that sway), cut now.
local function decor(x, z, nph, paint, tag)
  local d = { decor = true, x = x, z = z, y = floor_y(z), frames = {}, nph = nph, ph = rnd() * TAU,
              phs = 0.6 + rnd() * 0.4, tag = tag }
  for i = 0, nph - 1 do
    px.clear(0, 0, 0)
    paint(i * TAU / nph)
    local s, dx, dy = grab(0, 0, W, H)
    if s then d.frames[i + 1] = { s, CX - dx, BASE - dy, s:byte(1), 0 } end
  end
  DECOR[#DECOR + 1] = d
  return d
end


function build_reef()
  local ROCK = { 104, 96, 90 }
  -- the far rocks, small and hazed, and the reef on both sides
  decor(12, 0.18, 1, function() paint_rock(18, 8, ROCK, 1) end)
  decor(70, 0.12, 1, function() paint_rock(26, 7, ROCK, 2) end)
  decor(116, 0.22, 1, function() paint_rock(20, 9, ROCK, 3) end)
  decor(46, 0.30, 1, function() paint_brain(5, { 190, 170, 110 }) end)
  decor(20, 0.48, 1, function() paint_rock(34, 16, { 110, 100, 96 }, 4) end, "reef")
  decor(10, 0.52, 1, function() paint_fan(26, { 200, 70, 150 }, 0) end)
  decor(30, 0.44, 1, function() paint_table(18, { 180, 150, 110 }) end)
  decor(27, 0.58, 1, function() paint_brain(7, { 210, 190, 100 }) end)
  decor(36, 0.62, 1, function() paint_sponge(4, 12, { 170, 70, 200 }) end)
  local arch = decor(104, 0.5, 1, function() paint_rock(40, 22, { 100, 92, 88 }, 5, { 6, 8, 3 }) end, "reef")
  HOLE = { x = 104 + 6, y = floor_y(0.5) - 8, z = 0.505 }
  decor(88, 0.56, 1, function() paint_stag(20, { 220, 180, 140 }, { 250, 230, 200 }) end)
  decor(94, 0.64, 1, function() paint_sponge(3, 10, { 250, 150, 40 }) end)
  decor(120, 0.66, 1, function() paint_soft(12, { 150, 170, 110 }) end)
  decor(60, 0.40, 1, function() paint_clam(8) end)
  -- the near ones, swaying
  ANEMONE[1] = decor(14, 0.84, 4, function(ph) paint_anemone(7, { 230, 170, 150 }, { 255, 230, 220 }, ph) end, "anem")
  ANEMONE[2] = decor(84, 0.74, 4, function(ph) paint_anemone(6, { 150, 210, 120 }, { 250, 120, 200 }, ph) end, "anem")
  GRASS[1] = decor(62, 0.9, 4, function(ph) paint_grass(12, 7, { 70, 150, 70 }, ph) end, "grass")
  GRASS[2] = decor(44, 0.78, 4, function(ph) paint_grass(9, 6, { 90, 160, 60 }, ph) end, "grass")
  decor(122, 0.8, 4, function(ph) paint_kelp(44, { 150, 130, 50 }, ph) end, "kelp")
  decor(4, 0.95, 4, function(ph) paint_kelp(52, { 130, 120, 40 }, ph) end, "kelp")
  decor(112, 0.93, 4, function(ph) paint_fan(22, { 240, 120, 40 }, ph) end)
  decor(30, 0.97, 1, function() paint_rock(22, 7, { 132, 112, 96 }, 6) end)
  table.sort(DECOR, function(a, b) return a.z < b.z end)
end

end

-- ── the still picture: water, the far wall of blue, the sand ────────────────
local FLOOR_TOP = floor_y(0)
local function draw_still()
  for y = 0, FLOOR_TOP + 1, 2 do
    local r, g, b = water(y)
    rect(0, y, W, 2, floor(r), floor(g), floor(b), true)
  end
  -- far off, the tank's back: a ridge you can just make out
  for x = 0, W - 1, 2 do
    local hgt = 4 + 3 * sin(x * 0.07) + 2 * sin(x * 0.19 + 1) + 1.5 * sin(x * 0.41)
    local y0 = floor(FLOOR_TOP - hgt)
    local r, g, b = water(y0)
    rect(x, y0, 2, FLOOR_TOP - y0 + 1, floor(r * 0.8), floor(g * 0.82), floor(b * 0.86), true)
  end
  -- the sand, hazier the further it is
  for y = FLOOR_TOP, H - 1 do
    local z = (y - FLOOR_TOP) / (H - 1 - FLOOR_TOP)
    local hz = (1 - z) ^ 1.4 * 0.8
    local wr, wg, wb = water(y)
    local l = lum_at(y) * (0.75 + 0.25 * z)
    local r = mix(198 * l, wr * 0.9, hz)
    local g = mix(176 * l, wg * 0.9, hz)
    local b = mix(128 * l, wb * 0.9, hz)
    rect(0, y, W, 1, floor(r), floor(g), floor(b), true)
    -- ripples in the sand, finer with distance
    if y % 2 == 0 then
      local k = 0.82
      for x0 = (y * 7) % 11, W - 1, 11 + floor(z * 6) do
        line(x0, y, x0 + 3 + floor(z * 3), y, floor(r * k), floor(g * k), floor(b * k))
      end
    end
  end
end

-- ── the living ──────────────────────────────────────────────────────────────
local LIFE = {}                            -- everything that swims, crawls or drifts
local T = 0                                -- seconds the tank has been open
local REEF_X = { { 2, 44 }, { 82, 126 } }
local COOL = {}                            -- a rare one's next allowed visit
local NOW = {}                             -- how many of each kind are in (members too)

local function body_len(sp, z) return sp.L * scale_at(z) end
local function half_h(sp, z)
  local k = sp.kind
  local r = (k == "fish") and (sp.h or 0.4) * 0.5 or (k == "turtle" and 0.22 or (k == "ray" and 0.15 or 0.2))
  return body_len(sp, z) * r
end

-- Where a creature of this zone goes next.
local function new_target(c)
  local sp = c.sp
  local zone = sp.zone
  local zmax = sp.zmax or 0.95
  if zone == "reef" or zone == "floorreef" then
    local r = REEF_X[1 + floor(rnd() * 2) % 2]
    c.tz = rr(0.32, min(zmax, 0.92))
    c.tx = rr(r[1], r[2])
    local fy = floor_y(c.tz) - half_h(sp, c.tz)
    c.ty = zone == "floorreef" and fy - rr(1, 5) or fy - rr(5, 22)
  elseif zone == "floor" or zone == "hole" then
    c.tz = clamp(c.z + rr(-0.12, 0.12), 0.22, min(zmax, 0.95))
    c.tx = clamp(c.x + rr(-40, 40), 6, W - 6)
    c.ty = floor_y(c.tz) - half_h(sp, c.tz) - (sp.kind == "fish" and 1 or 0)
  elseif zone == "surface" then
    c.tz = rr(0.3, 0.9); c.tx = rr(0, W); c.ty = rr(4, 13)
  elseif zone == "anem" and c.home then
    local a = c.home
    c.tz = a.z + rr(-0.03, 0.03); c.tx = a.x + rr(-6, 6); c.ty = a.y - rr(5, 11)
  elseif zone == "grass" and c.home then
    local g = c.home
    c.tz = g.z - rr(0.0, 0.04); c.tx = g.x + rr(-7, 7); c.ty = g.y - rr(5, 14)
  else
    c.tz = rr(0.06, zmax)
    c.tx = rr(8, W - 8)
    c.ty = rr(8, max(10, floor_y(c.tz) - 8 - half_h(sp, c.tz)))
  end
  -- The room bends the route a little and never becomes it.
  if ROOM.haveF > 0.2 and zone ~= "floor" and zone ~= "hole" then
    c.tx = c.tx + (ROOM.focus * W - c.tx) * 0.15 * ROOM.haveF * ROOM.near
  end
  c.retarget = rr(4, 11) * (sp.big and 1.5 or 1)
end

local function make(sp, x, y, z)
  local c = { sp = sp, x = x, y = y, z = z, vx = 0, vy = 0, vz = 0, face = rnd() < 0.5 and -1 or 1,
              ph = rnd() * TAU, age = 0, mode = "in", retarget = 0, flee = 0, infl = 0, look = 0,
              var = 0, dir = 1, tint = 0, pause = 0, seen = false, flip = rnd() < 0.5,
              life = sp.big and rr(22, 45) or rr(35, 110) }
  c.tx, c.ty, c.tz = x, y, z
  LIFE[#LIFE + 1] = c
  NOW[sp] = (NOW[sp] or 0) + 1
  return c
end

-- A shoal: a leader nobody sees, and the members holding station on it.
local function make_school(sp, n, x, y, z)
  local G = make(sp, x, y, z)
  G.leader, G.members, G.scatter = true, {}, 0
  local R = sp.L * (0.9 + 0.22 * n)
  for i = 1, n do
    local m = make(sp, x, y, z)
    m.group = G
    m.ox, m.oy, m.oz = rr(-R, R), rr(-R, R) * 0.45, rr(-0.06, 0.06)
    m.ph = rnd() * TAU
    G.members[#G.members + 1] = m
  end
  return G
end

-- The sardines: a ball of silver turning on itself.
local function make_ball(sp, x, y, z, river)
  local b = make(sp, x, y, z)
  b.ball, b.river, b.pts, b.panic = true, river, {}, 0
  for i = 1, sp.n do
    b.pts[i] = { a = rnd() * TAU, r = 0.35 + 0.65 * sqrt(rnd()), h = rr(-0.5, 0.5),
                 w = (0.7 + rnd() * 0.6) * (rnd() < 0.85 and 1 or -1), s = rnd() }
  end
  b.life = river and 60 or rr(60, 120)
  return b
end

local function entry(sp)
  local z, x, y
  local zmax = sp.zmax or 0.95
  if (sp.big or sp.kind == "jelly") and rnd() < 0.65 then
    -- out of the far blue: a shadow first, taking colour as it comes
    z = 0.03
    x = rr(24, W - 24)
    y = rr(12, floor_y(z) - 6)
  else
    z = rr(0.18, zmax)
    local len = body_len(sp, z)
    x = rnd() < 0.5 and (-len * 0.6 - 3) or (W + len * 0.6 + 3)
    if sp.zone == "floor" then y = floor_y(z) - half_h(sp, z)
    elseif sp.zone == "surface" then y = rr(4, 12)
    elseif sp.kind == "jelly" then y = rr(10, 40)
    else y = rr(10, max(12, floor_y(z) - 10)) end
  end
  return x, y, z
end

local function count_of(sp) return NOW[sp] or 0 end

local ANEM_USED, GRASS_USED = {}, {}
local function spawn(sp)
  local x, y, z = entry(sp)
  if sp.kind == "ball" then return make_ball(sp, x < 0 and -10 or W + 10, rr(14, 30), rr(0.35, 0.8), false) end
  if sp.kind == "river" then
    -- anchovies cross the tank in a stream and are gone
    local b = make_ball(sp, x < 0 and -40 or W + 40, rr(10, 34), rr(0.25, 0.85), true)
    b.tx, b.ty, b.retarget, b.mode = x < 0 and W + 120 or -120, b.y, 1e9, "cross"
    return b
  end
  if sp.zone == "anem" then
    local a = pick(ANEMONE)
    if (ANEM_USED[a] or 0) >= 2 then return nil end
    ANEM_USED[a] = (ANEM_USED[a] or 0) + 1
    local c = make(sp, a.x + rr(-5, 5), a.y - rr(4, 10), a.z)
    c.home, c.life, c.mode = a, 1e9, "roam"
    return c
  end
  if sp.zone == "grass" then
    local g = pick(GRASS)
    if (GRASS_USED[g] or 0) >= 1 then return nil end
    GRASS_USED[g] = 1
    local c = make(sp, g.x + rr(-5, 5), g.y - rr(5, 12), g.z - 0.02)
    c.home, c.life = g, rr(90, 200)
    return c
  end
  if sp.school and rnd() < 0.85 then
    local n = min(8, floor(rr(sp.school[1], sp.school[2] + 0.99)))
    return make_school(sp, n, x, y, z)
  end
  return make(sp, x, y, z)
end

-- How many are in the tank, counted the way the eye counts: a shoal is one
-- thing, a big shark is a lot of the screen.
local function crowd()
  local n, bigs = 0, 0
  for i = 1, #LIFE do
    local c = LIFE[i]
    if not c.group and c.sp.kind ~= "static" and c.sp.kind ~= "moray" and c.sp.kind ~= "eels" then
      n = n + (c.leader and 2 or (c.ball and 2 or 1))
      if c.sp.big then bigs = bigs + 1 end
    end
  end
  return n, bigs
end

local SPAWNABLE = {}
for i = 1, #SPECIES do
  local sp = SPECIES[i]
  if sp.w > 0 and sp.kind ~= "static" and sp.kind ~= "moray" and sp.kind ~= "eels" then SPAWNABLE[#SPAWNABLE + 1] = sp end
end

local function choose()
  local day = LIGHT.day
  local _, bigs = crowd()
  local total, weights = 0, {}
  for i = 1, #SPAWNABLE do
    local sp = SPAWNABLE[i]
    local w = sp.w
    if sp.act == "d" then w = w * (0.12 + 0.88 * day)
    elseif sp.act == "n" then w = w * (0.12 + 0.88 * (1 - day)) end
    if sp.big and bigs >= 2 then w = 0 end
    if sp.rare and (COOL[sp] or 0) > T then w = 0 end
    if count_of(sp) >= (sp.school and 1 or 2) then w = 0 end
    if sp.zone == "anem" and sp.w > 0 then w = w * 2 end
    weights[i] = w
    total = total + w
  end
  local r = rnd() * total
  for i = 1, #SPAWNABLE do
    r = r - weights[i]
    if r <= 0 and weights[i] > 0 then return SPAWNABLE[i] end
  end
  return nil
end

local spawnIn = 0
local function populate(dt)
  spawnIn = spawnIn - dt
  if spawnIn > 0 then return end
  spawnIn = rr(0.8, 2.4)
  local n, bigs = crowd()
  local want = floor(13 + 7 * LIGHT.day)
  if n >= want then return end
  local sp = choose()
  if not sp then return end
  if sp.rare then COOL[sp] = T + rr(240, 480) end
  spawn(sp)
end

-- ── moving ──────────────────────────────────────────────────────────────────
local PRED = {}

local function threat_to(c)
  local best, bd = nil, 1e9
  local sp = c.sp
  for i = 1, #PRED do
    local p = PRED[i]
    if p ~= c and p.sp.pred >= sp.pred + 2 and abs(p.z - c.z) < 0.28 then
      local reach = body_len(p.sp, p.z) * 0.7 + 12
      local dx, dy = c.x - p.x, (c.y - p.y) * 1.6
      local d = dx * dx + dy * dy
      if d < reach * reach and d < bd then best, bd = p, d end
    end
  end
  return best
end

local function flee_from(c, p, boost)
  local dx, dy = c.x - p.x, c.y - p.y
  local d = sqrt(dx * dx + dy * dy) + 0.1
  c.tx = c.x + dx / d * 40
  c.ty = clamp(c.y + dy / d * 18, 6, floor_y(c.z) - 3)
  c.flee = boost or 1
  c.retarget = 2.5
  if c.sp.zone == "reef" or c.sp.zone == "floorreef" then
    local r = c.x < 64 and REEF_X[1] or REEF_X[2]
    c.tx = clamp(c.tx, r[1], r[2])
    c.ty = floor_y(c.z) - rr(3, 8)
  end
end

local function steer(c, dt, vmax, tau)
  local dx, dy = c.tx - c.x, c.ty - c.y
  local d = sqrt(dx * dx + dy * dy) + 0.001
  local want = vmax * min(1, d / (vmax * 1.2 + 2))
  c.vx = lp(c.vx, dx / d * want, dt, tau)
  c.vy = lp(c.vy, dy / d * want * 0.7, dt, tau)
  c.x = c.x + c.vx * dt
  c.y = c.y + c.vy * dt
  local vz = (c.tz - c.z)
  local vzm = 0.05 * (c.sp.v / 8 + 0.3) * (1 + c.flee)
  c.z = c.z + clamp(vz, -vzm, vzm) * dt
  return d
end

local function turn(c, dt, want)
  local tau = 0.25 + c.sp.L / 180
  c.face = lp(c.face, want, dt, tau)
  c.dir = c.face >= 0 and 1 or -1
end

local startleWas = 0
local function move(c, dt)
  local sp = c.sp
  local kind = sp.kind
  c.age = c.age + dt
  c.retarget = c.retarget - dt
  if c.flee > 0 then c.flee = max(0, c.flee - dt * 0.45) end
  if c.look > 0 then c.look = c.look - dt end

  if kind == "static" then return end
  if kind == "moray" then
    -- More out at night; back in when something big or somebody quick goes by.
    local want = 0.35 + 0.55 * (1 - LIGHT.day)
    if ROOM.startle > 0.4 or c.flee > 0.2 then want = 0 end
    c.out = lp(c.out or 0, want, dt, want > (c.out or 0) and 6 or 0.8)
    c.var = floor(clamp(c.out, 0, 0.999) * 4)
    c.ph = c.ph + dt * 1.2
    return
  end

  -- time to go?
  if c.mode ~= "out" and c.age > c.life and not c.group then
    c.mode = "out"
    if (sp.big or kind == "jelly") and rnd() < 0.5 then
      c.tz = 0.02; c.tx = c.x; c.ty = c.y
    else
      local len = body_len(sp, c.z)
      c.tx = c.x < 64 and -len - 10 or W + len + 10
      c.ty = c.y
    end
    c.retarget = 1e9
  elseif c.mode == "in" and c.age > 3 then
    c.mode = "roam"
  end

  -- the room
  if ROOM.startle > 0.55 and startleWas <= 0.55 and not sp.big then
    if sp.puffer then c.infl = 6 end
    if sp.L <= 14 and sp.pred < 2 then
      flee_from(c, { x = ROOM.focus * W, y = 30 }, 1.2)
      c.tz = max(0.2, c.z - 0.15)
    end
    if kind == "octopus" then c.jet = 3; c.tint = 1; c.tx = rr(10, W - 10); c.ty = rr(20, 34) end
  end
  if c.infl > 0 then c.infl = c.infl - dt end
  if sp.curious and c.mode == "roam" and c.look <= 0 and ROOM.haveF > 0.6 and ROOM.near > 0.25
     and ROOM.startle < 0.3 and rnd() < dt * 0.06 then
    c.look = rr(6, 12)
    c.tx = clamp(ROOM.focus * W + rr(-14, 14), 12, W - 12)
    c.tz = min(sp.zmax or 0.95, 0.92)
    c.ty = rr(20, min(36, floor_y(c.tz) - half_h(sp, c.tz) - 2))
    c.retarget = c.look
  end

  -- a predator near?
  if sp.pred < 2 and not sp.big and kind ~= "jelly" and c.mode ~= "out" then
    local p = threat_to(c.group or c)
    if p then
      if c.group then c.group.scatter = 1; flee_from(c.group, p) else flee_from(c, p) end
    end
  end

  if c.retarget <= 0 and c.mode ~= "out" and not c.group then new_target(c) end

  local sc = scale_at(c.z)
  local boost = 1 + 1.6 * c.flee
  local vmax = sp.v * sc * boost
  local tau = 0.35 + sp.L / 70

  if c.ball then
    -- the shoal ball drifts, and opens where a hunter pushes into it
    local d = steer(c, dt, vmax, 1.5)
    if d < 4 and c.retarget > 0 and c.mode ~= "out" then c.retarget = 0 end
    local p = threat_to(c)
    c.panic = lp(c.panic, p and 1 or 0, dt, p and 0.4 or 3)
    c.threat = p
    c.ph = c.ph + dt
    return
  end

  if c.group then
    local G = c.group
    local s = scale_at(G.z) * (1 + 1.8 * (G.scatter or 0))
    c.tx = G.x + c.ox * s * G.dir + sin(T * 0.7 + c.ph) * 2
    c.ty = G.y + c.oy * s + sin(T * 0.5 + c.ph * 1.3) * 1.5
    c.tz = clamp(G.z + c.oz, 0.03, 0.97)
    local d = steer(c, dt, vmax * 1.5 + 4 * sc, 0.4)
    turn(c, dt, (abs(c.vx) > 0.8 * sc) and (c.vx > 0 and 1 or -1) or c.face)
    c.mode = G.mode
  elseif c.leader then
    local d = steer(c, dt, vmax, tau)
    if d < 5 and c.mode ~= "out" then c.retarget = 0 end
    c.scatter = max(0, (c.scatter or 0) - dt * 0.35)
    c.dir = c.vx >= 0 and 1 or -1
  elseif kind == "jelly" then
    -- a jelly swims by squeezing its bell: up on the stroke, sinking between
    c.ph = c.ph + dt * TAU * 0.45
    local pulse = max(0, sin(c.ph)) * 3.5 * sc
    c.vy = lp(c.vy, -pulse + 1.2 * sc, dt, 0.4)
    c.vx = lp(c.vx, (c.tx - c.x) * 0.02 + sin(T * 0.03) * 0.8, dt, 2)
    c.x, c.y = c.x + c.vx * dt, c.y + c.vy * dt
    c.z = c.z + clamp(c.tz - c.z, -0.03, 0.03) * dt
    if c.y < 6 then c.vy = abs(c.vy) end
    if c.y > floor_y(c.z) - 16 then c.y = floor_y(c.z) - 16 end
    return
  elseif kind == "seahorse" then
    local d = steer(c, dt, vmax, 1.2)
    c.y = c.y + sin(T * 0.8 + c.ph) * 0.05
    if abs(c.vx) > 0.3 then turn(c, dt, c.vx > 0 and 1 or -1) end
    c.ph = c.ph + dt * TAU * 1.2
    if d < 2 then c.retarget = min(c.retarget, rr(1, 5)) end
    return
  elseif kind == "crust" then
    c.pause = c.pause - dt
    if c.pause <= 0 then
      local d = steer(c, dt, vmax, 0.5)
      c.ph = c.ph + dt * TAU * 1.5
      if d < 2 then c.pause = rr(1, 6); c.retarget = 0 end
      if ROOM.startle > 0.5 then c.pause = 2 end
    else
      c.vx, c.vy = 0, 0
    end
    c.y = floor_y(c.z) - half_h(sp, c.z)
    if abs(c.vx) > 0.2 then turn(c, dt, c.vx > 0 and 1 or -1) end
    return
  elseif kind == "octopus" then
    c.jet = max(0, (c.jet or 0) - dt)
    c.tint = max(0, c.tint - dt * 0.5)
    if c.jet > 0 then
      c.var = 1
      steer(c, dt, 18 * sc, 0.3)
      c.ph = c.ph + dt * TAU * 1.5
    else
      c.var = 0
      local fy = floor_y(c.z) - half_h(sp, c.z)
      c.ty = fy
      c.pause = c.pause - dt
      if c.pause <= 0 then
        local d = steer(c, dt, vmax * 0.7, 0.8)
        c.ph = c.ph + dt * TAU * 0.4
        if d < 2 then c.pause = rr(2, 8); c.retarget = 0 end
      end
      c.y = lp(c.y, fy, dt, 0.8)
    end
    if abs(c.vx) > 0.3 then turn(c, dt, c.vx > 0 and 1 or -1) end
    return
  else
    local d = steer(c, dt, vmax, tau)
    if d < 3 and c.mode ~= "out" then c.retarget = min(c.retarget, rr(0.5, 3)) end
  end

  -- keep off the sand, out of the surface
  if sp.zone ~= "floor" then
    local fy = floor_y(c.z) - half_h(sp, c.z) - 1
    if c.y > fy then c.y = fy; c.vy = min(0, c.vy) end
  else
    c.y = lp(c.y, floor_y(c.z) - half_h(sp, c.z) - 1, dt, 0.5)
  end
  if c.y < 4 + half_h(sp, c.z) then c.y = 4 + half_h(sp, c.z) end

  -- facing: the way it swims, or at you
  local want = c.face
  if c.look > 0 and abs(c.x - c.tx) < 8 then
    want = 0
  elseif abs(c.vx) > 0.6 * sc then
    want = c.vx > 0 and 1 or -1
  end
  turn(c, dt, want)

  -- the beat: faster when it hurries, slower the bigger it is
  local speed01 = clamp(sqrt(c.vx * c.vx + c.vy * c.vy) / max(0.1, sp.v * sc), 0, 2)
  local hz
  if kind == "ray" then hz = 0.22 + 0.1 * speed01
  elseif kind == "turtle" then hz = 0.3 + 0.15 * speed01
  elseif kind == "cuttle" or kind == "nautilus" then hz = 0.5
  else hz = (0.7 + 1.5 * speed01) * (10 / sp.L) ^ 0.35 end
  c.ph = c.ph + dt * TAU * hz
end

-- Gone for good: out of the sides, or back into the far blue.
local function gone(c)
  if c.mode ~= "out" then return false end
  if c.z < 0.035 then return true end
  local len = body_len(c.sp, c.z)
  return c.x < -len - 8 or c.x > W + len + 8
end

-- ── light in the water ──────────────────────────────────────────────────────
local DT = 1 / 15

local function draw_surface()
  local k = max(LIGHT.day * (1 - 0.5 * LIGHT.warm), LIGHT.lamp * 0.6)
  if k < 0.05 then return end
  local r0, g0, b0 = water(1)
  for i = 0, 15 do
    local x = i * 8 + floor(5 * sin(T * 0.6 + i))
    local y = 1 + floor(1.3 * sin(T * 1.3 + i * 0.9) + 0.5)
    local a = k * (0.5 + 0.5 * sin(T * 1.9 + i * 1.3))
    local len = 3 + floor(4 * abs(sin(T * 0.8 + i * 1.7)))
    line(x, y, x + len, y, floor(r0 + (230 - r0) * a), floor(g0 + (250 - g0) * a), floor(b0 + (255 - b0) * a))
  end
end

local function shaft_set(xs, lean, width, k, tint)
  local t1, t2, t3 = tint[1], tint[2], tint[3]
  for s = 1, #xs do
    local base = xs[s]
    local ks = k * (0.6 + 0.4 * sin(T * 0.3 + s * 1.7))
    for b = 0, 10 do
      local y = b * 4
      local w = width + b * 0.65
      local kk = (1 - b / 12) * ks
      local r, g, bl = WR[y] + t1 * kk, WG[y] + t2 * kk, WB[y] + t3 * kk
      rect(floor(base + y * lean - w * 0.5), y, floor(w), 4, floor(r > 255 and 255 or r),
           floor(g > 255 and 255 or g), floor(bl > 255 and 255 or bl), true)
    end
  end
end

local SUN_TINT, LAMP_TINT, MOON_TINT = { 46, 70, 56 }, { 40, 64, 80 }, { 18, 24, 34 }
local function draw_shafts()
  local d = LIGHT.day * (1 - 0.4 * LIGHT.warm)
  if d > 0.05 then
    local xs = {}
    for s = 1, 4 do xs[s] = 8 + (s - 1) * 34 + 10 * sin(T * 0.11 + s) end
    shaft_set(xs, 0.35 + 0.2 * sin(T * 0.07), 3, d * 0.6, SUN_TINT)
  end
  local l = LIGHT.lamp * (1 - LIGHT.day)
  if l > 0.05 then
    shaft_set({ 22, 64, 106 }, 0.04 * sin(T * 0.2), 5, l * 0.55, LAMP_TINT)
  end
  local m = LIGHT.night * (1 - LIGHT.lamp)
  if m > 0.2 then
    shaft_set({ 40 + 6 * sin(T * 0.05), 92 }, 0.3, 4, m * 0.7, MOON_TINT)
  end
end

local function draw_caustics()
  local k = max(LIGHT.day * (1 - 0.5 * LIGHT.warm), LIGHT.lamp * 0.7)
  if k < 0.08 then return end
  for c = 0, 3 do
    local y0 = FLOOR_TOP + 2 + c * 5
    local z = (y0 - FLOOR_TOP) / (H - 1 - FLOOR_TOP)
    local amp = 0.6 + 0.9 * z
    local x0, yA
    for i = 0, 8 do
      local x = i * 16
      local y = y0 + amp * sin(x * 0.12 + T * 0.8 + c * 0.9) + amp * 0.6 * sin(x * 0.05 - T * 0.5 + c * 2)
      local yi = floor(y)
      if x0 then
        local kk = k * (0.35 + 0.35 * z) * (0.5 + 0.5 * sin(T * 1.2 + c + i * 0.5))
        local l = lum_at(yi)
        line(x0, yA, x, yi, floor(min(255, 198 * l + 70 * kk)), floor(min(255, 176 * l + 80 * kk)), floor(min(255, 128 * l + 70 * kk)))
      end
      x0, yA = x, yi
    end
  end
end

-- Marine snow: the specks every public tank has drifting in its light.
local SNOW = {}
for i = 1, 24 do SNOW[i] = { x = rnd() * W, y = rnd() * H, z = 0.1 + 0.9 * rnd(), p = rnd() * TAU } end
local function draw_snow(front)
  local l = LIGHT.lum
  for i = 1, #SNOW do
    local s = SNOW[i]
    if (s.z > 0.8) == front then
      s.y = s.y + DT * (0.8 + 1.6 * s.z)
      s.x = s.x + DT * (sin(T * 0.13 + s.p) * 0.6 + 0.4)
      if s.y > floor_y(s.z) then s.y = -1; s.x = rnd() * W end
      if s.x > W then s.x = 0 elseif s.x < 0 then s.x = W - 1 end
      local yi = floor(s.y)
      if yi >= 0 then
        local r, g, b = WR[yi], WG[yi], WB[yi]
        local k = (0.18 + 0.4 * s.z) * l
        local cr, cg, cb = floor(r + (200 - r) * k), floor(g + (215 - g) * k), floor(b + (220 - b) * k)
        local xi = floor(s.x)
        pixel(xi, yi, cr, cg, cb)
        if s.z > 0.93 then pixel(xi + 1, yi, cr, cg, cb) end
      end
    end
  end
end

-- At night what darts leaves a trail of sparks: the plankton lighting up.
local SPARK = {}
local function spark_add(x, y)
  if #SPARK < 40 then SPARK[#SPARK + 1] = { x = x + rr(-1, 1), y = y + rr(-1, 1), t = 0.8 } end
end
local function draw_sparks()
  local n = LIGHT.night
  local i = 1
  while i <= #SPARK do
    local s = SPARK[i]
    s.t = s.t - DT
    if s.t <= 0 then
      SPARK[i] = SPARK[#SPARK]; SPARK[#SPARK] = nil
    else
      local k = s.t / 0.8 * n
      pixel(floor(s.x), floor(s.y), floor(40 + 60 * k), floor(90 + 150 * k), floor(120 + 135 * k))
      i = i + 1
    end
  end
end

-- ── drawing the living ──────────────────────────────────────────────────────
local function light_for(c)
  local y = c.y
  local mul = lum_at(y)
  local hz = clamp((1 - c.z) ^ 1.45 * 0.97, 0, 0.97)
  local wr, wg, wb = water(y)
  local tr, tg, tb = wr * 0.62, wg * 0.66, wb * 0.74          -- the far ones are shadows
  local sp = c.sp
  if sp.glow and LIGHT.night > 0.3 then
    local g = LIGHT.night * (1 - LIGHT.lamp)
    mul = max(mul, 0.5 + 0.3 * g * (0.6 + 0.4 * sin(c.ph)))
    tr, tg, tb = mix(tr, 40, g * 0.8), mix(tg, 160, g * 0.8), mix(tb, 255, g * 0.8)
    hz = max(hz * 0.6, 0.22 * g)
  end
  if sp.kind == "octopus" then
    if c.tint > 0 then
      tr, tg, tb = mix(tr, 230, c.tint), mix(tg, 40, c.tint), mix(tb, 30, c.tint)
      hz = max(hz, 0.45 * c.tint)
    elseif c.var == 0 and c.pause > 0 then
      tr, tg, tb = 180 * mul, 160 * mul, 120 * mul        -- the colour of the sand it sits on
      hz = max(hz, 0.4)
    end
  end
  return mul, tr, tg, tb, hz
end

local function pose_for(c)
  local sp = c.sp
  local kind = sp.kind
  local lp = body_len(sp, c.z)
  if lp < 2.2 then return nil end
  -- The size with a margin, so an animal hovering between two sizes does not
  -- flick between them (and have both cut).
  local si = c.si
  if not si or abs(lp - (c.silp or 0)) > (c.silp or 0) * 0.06 then
    local f = log(max(lp, 2) / 2) / LOGSTEP
    if not si or abs(f - si) > 0.8 then si = max(0, floor(f + 0.5)); c.si = si end
    c.silp = lp
  end
  local ti = 1
  if kind ~= "jelly" and kind ~= "seahorse" and kind ~= "static" and kind ~= "moray" and kind ~= "nautilus" then
    local fore = abs(c.face)
    if fore < 0.45 then ti = 3 elseif fore < 0.8 then ti = 2 end
  end
  local nph = NPH[kind] or 1
  local pn = floor((c.ph % TAU) / TAU * nph) % nph
  if ti > 1 then pn = 0 end                 -- a turn is over before the tail beats
  if kind == "static" then
    pn = 0
    if sp.still == "urchin" or sp.still == "feather" then pn = floor(T * 0.6 + c.ph) % 4 end
  end
  local var = c.var or 0
  if sp.puffer then var = c.infl > 0 and 1 or 0 end
  -- The same pose as last frame is most frames: no key, no lookup.
  local code = ((si * 4 + ti) * 8 + var) * 16 + pn
  if code == c.code and c.exact then
    local p = c.pose
    if frameNo - p[5] > 30 then p[5] = frameNo end
    return p
  end
  local p = want(sp, si, ti, var, pn, not c.seen)
  c.code, c.exact = code, p ~= nil
  if not p then p = nearest(sp, si, ti, var, pn) end
  return p
end

local function draw_dot(c)
  local mul, tr, tg, tb, hz = light_for(c)
  pixel(floor(c.x), floor(c.y), floor(tr * 0.9), floor(tg * 0.9), floor(tb * 0.9))
end

local function draw_creature(c)
  local p = c.pose
  if not p then
    if body_len(c.sp, c.z) < 2.2 then draw_dot(c) end
    return
  end
  c.seen = true
  local flip = (c.sp.kind == "jelly" or c.sp.kind == "static") and c.flip or c.dir < 0
  local x, y = floor(c.x), floor(c.y)
  local X = flip and (x - (p[4] - 1 - p[2])) or (x - p[2])
  local mul, tr, tg, tb, hz = light_for(c)
  blit(p[1], X, y - p[3], flip, mul, tr, tg, tb, hz)
  -- night sparks off whatever hurries
  if LIGHT.night > 0.4 and LIGHT.lamp < 0.5 then
    local v = abs(c.vx) + abs(c.vy)
    if v > 6 * scale_at(c.z) and rnd() < DT * v * 0.35 then
      spark_add(x - c.dir * body_len(c.sp, c.z) * 0.45, y)
    end
  end
end

local function draw_decor(d)
  local fr = d.frames[d.nph > 1 and (floor(d.ph) % d.nph + 1) or 1]
  if not fr then return end
  d.ph = d.ph + DT * d.phs * (d.nph > 1 and 1.4 or 0)
  local mul = lum_at(d.y) * 0.95
  local hz = clamp((1 - d.z) ^ 1.45 * 0.97, 0, 0.97)
  local wr, wg, wb = water(d.y)
  blit(fr[1], floor(d.x) - fr[2], d.y - fr[3], false, mul, wr * 0.62, wg * 0.66, wb * 0.74, hz)
end

local function draw_ball(b)
  local sp = b.sp
  local sc = scale_at(b.z)
  local mul, tr, tg, tb, hz = light_for(b)
  local c = sp.c
  local R = (9 + 6 * b.panic) * sc
  local th = b.threat
  local dir = b.vx >= 0 and 1 or -1
  for i = 1, #b.pts do
    local q = b.pts[i]
    local x, y, bright
    if b.river then
      q.s = (q.s + DT * 0.02) % 1
      x = b.x - dir * q.s * 80 * sc
      y = b.y + sin(q.s * 9 + T * 1.3) * 4 * sc + q.h * 7 * sc
      bright = 0.8 + 0.4 * sin(T * 3 + i)
    else
      q.a = q.a + DT * q.w * (0.9 + 1.8 * b.panic)
      x = b.x + cos(q.a) * q.r * R
      y = b.y + q.h * R * 0.7 + sin(q.a) * q.r * R * 0.33
      bright = 0.65 + 0.6 * abs(cos(q.a))
      if th then
        local dx, dy = x - th.x, y - th.y
        local d = sqrt(dx * dx + dy * dy) + 0.1
        local hole = body_len(th.sp, th.z) * 0.35
        if d < hole then x, y = x + dx / d * (hole - d), y + dy / d * (hole - d) end
      end
    end
    local k = mul * bright * (1 - hz)
    local r = c[1] * k + tr * hz
    local g = c[2] * k + tg * hz
    local bb = c[3] * k + tb * hz
    local xi, yi = floor(x), floor(y)
    pixel(xi, yi, floor(min(255, r)), floor(min(255, g)), floor(min(255, bb)))
    if sc > 0.72 then pixel(xi - dir, yi, floor(r * 0.7), floor(g * 0.7), floor(bb * 0.7)) end
  end
end

-- The garden eels: a colony standing out of the sand, heads into the current,
-- gone into their burrows when something startles them.
local EELS = { eels = true, x0 = 50, x1 = 76, z = 0.44, out = 1, n = 8 }
local function draw_eels(e)
  local sp = BY_NAME["garden eels"]
  local want = 1
  if ROOM.startle > 0.3 then want = 0 end
  for i = 1, #PRED do
    local p = PRED[i]
    if abs(p.x - (e.x0 + e.x1) / 2) < 30 and abs(p.z - e.z) < 0.3 then want = 0 end
  end
  if LIGHT.day < 0.3 then want = 0.15 end
  e.out = lp(e.out, want, DT, want < e.out and 0.3 or 5)
  local sc = scale_at(e.z)
  local by = floor(floor_y(e.z))
  local mul = lum_at(by)
  local hz = clamp((1 - e.z) ^ 1.45 * 0.97, 0, 0.97)
  local wr, wg, wb = water(by)
  local c = sp.c
  local function col(k)
    return floor(c[1] * mul * k * (1 - hz) + wr * 0.62 * hz), floor(c[2] * mul * k * (1 - hz) + wg * 0.66 * hz),
           floor(c[3] * mul * k * (1 - hz) + wb * 0.74 * hz)
  end
  for i = 0, e.n - 1 do
    local bx = e.x0 + (e.x1 - e.x0) * i / (e.n - 1) + (hash2(i, 5) - 0.5) * 3
    local h = e.out * (6 + 4 * hash2(i, 9)) * sc * (0.9 + 0.1 * sin(T * 0.7 + i))
    if h >= 1 then
      local bend = (1.5 + sin(T * 0.4 + i * 0.6)) * sc
      local r, g, b = col(0.9)
      local x1, y1 = bx + bend * 0.5, by - h
      line(floor(bx), by, floor(x1), floor(y1), r, g, b)
      local hr, hg, hb = col(1.1)
      pixel(floor(x1 + bend * 0.5), floor(y1), hr, hg, hb)
    end
  end
end

-- What the clicks did, for a moment at the top of the glass.
local function draw_caption()
  local said = IN.said
  if not said or T > IN.till then return end
  -- 5x7: in the small font Д reads as А
  local x = floor((W - px.width(said, "5x7")) / 2)
  text(x + 1, 4, said, 8, 12, 18, "5x7")
  text(x, 3, said, 250, 236, 170, "5x7")
end

local function draw_clock()
  local h = clock_h()
  local s = string.format("%02d:%02d", floor(h), floor((h % 1) * 60))
  local tx, ty = W - px.width(s) - 3, H - 8
  text(tx + 1, ty + 1, s, 8, 12, 18)
  local l = max(0.55, LIGHT.lum)
  text(tx, ty, s, floor(236 * l), floor(244 * l), floor(250 * l))
end

-- ── opening the tank ────────────────────────────────────────────────────────
local function place_inside(c)
  local sp = c.sp
  c.z = rr(0.25, sp.zmax or 0.9)
  c.x = rr(12, W - 12)
  if sp.zone == "floor" then c.y = floor_y(c.z) - half_h(sp, c.z)
  elseif sp.zone == "surface" then c.y = rr(5, 12)
  else c.y = rr(10, max(12, floor_y(c.z) - 10 - half_h(sp, c.z))) end
  c.tx, c.ty, c.tz = c.x, c.y, c.z
  c.mode, c.age = "roam", rr(0, 20)
end

local function open_tank()
  light_update(0, true)
  if READY then build_reef() end
  -- the ones that live here and do not leave
  local stills = {}
  for i = 1, #SPECIES do if SPECIES[i].kind == "static" then stills[#stills + 1] = SPECIES[i] end end
  for i = 1, 8 do
    local sp = stills[1 + (i - 1) % #stills]
    local z = rr(0.3, 0.95)
    local c
    if sp.zone == "reeftop" then
      local r = REEF_X[1 + i % 2]
      z = 0.5
      c = make(sp, rr(r[1] + 6, r[2] - 6), floor_y(z) - rr(10, 16), z + 0.01)
    else
      c = make(sp, rr(6, W - 6), 0, z)
      c.y = floor_y(z) - body_len(sp, z) * 0.12
    end
    c.life, c.mode, c.flip = 1e9, "roam", rnd() < 0.5
  end
  local mor = rnd() < 0.5 and BY_NAME["green moray"] or BY_NAME["honeycomb moray"]
  local lpm = body_len(mor, HOLE and HOLE.z or 0.5)
  local m = make(mor, (HOLE and HOLE.x or 110) + lpm * 0.25, HOLE and HOLE.y or 40, HOLE and HOLE.z or 0.505)
  m.life, m.mode, m.dir, m.face = 1e9, "roam", 1, 1
  -- clownfish in the anemone, and a start of life in the water
  if #ANEMONE > 0 then spawn(BY_NAME["clownfish"]); spawn(BY_NAME["clownfish"]) end
  for i = 1, 9 do
    local sp = choose()
    if sp and not sp.rare then
      local c = spawn(sp)
      if c and not c.home then
        place_inside(c)
        if c.members then
          for j = 1, #c.members do
            local mm = c.members[j]
            mm.x, mm.y, mm.z = c.x + mm.ox * scale_at(c.z), c.y + mm.oy * scale_at(c.z), c.z + mm.oz
            mm.mode = "roam"
          end
        end
      end
    end
  end
end

open_tank()

-- Cut the first poses while the tank opens, so it does not open empty.
if READY then
  for i = 1, #LIFE do local c = LIFE[i]; if not c.leader and not c.ball then c.pose = pose_for(c) end end
  cut_wanted(120000)
end

-- ── a frame ─────────────────────────────────────────────────────────────────
local tprev = nil
local stillKey = nil
local LIST, ORDER = {}, {}
ORDER[1] = EELS; EELS.inOrder = true

function draw()
  if not READY then
    px.clear(4, 20, 40)
    text(4, 24, "OCEANARIUM", 120, 200, 240)
    text(4, 34, "NEEDS FIRMWARE 2.7.1+", 200, 220, 230)
    return
  end
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
  DT = dt
  T = T + dt
  if T > 7200 then T = T - 7200 end
  clock_run(dt)
  frameNo = frameNo + 1

  light_update(dt, false)
  if RAD and RAD.state() == 1 then read_room(dt) else no_room(dt) end
  lamp_update(T)
  populate(dt)

  -- the hunters this frame
  local np = 0
  for i = 1, #LIFE do
    local c = LIFE[i]
    if c.sp.pred >= 2 and not c.leader and c.sp.kind ~= "static" then np = np + 1; PRED[np] = c end
  end
  for i = np + 1, #PRED do PRED[i] = nil end

  for i = 1, #LIFE do move(LIFE[i], dt) end
  startleWas = ROOM.startle

  -- the gone go, and a shoal's members with their leader
  local i = 1
  while i <= #LIFE do
    local c = LIFE[i]
    local dead = gone(c) or (c.group and c.group.dead)
    if c.ball and c.river then
      dead = (c.vx > 0 and c.x > W + 90) or (c.vx < 0 and c.x < -90) or c.age > 90
    end
    if dead then
      c.dead = true
      NOW[c.sp] = (NOW[c.sp] or 1) - 1
      if c.home then
        if ANEM_USED[c.home] then ANEM_USED[c.home] = ANEM_USED[c.home] - 1 end
        if GRASS_USED[c.home] then GRASS_USED[c.home] = 0 end
      end
      table.remove(LIFE, i)
    else
      i = i + 1
    end
  end

  -- poses: ask for what is needed, cut what is missing, then look again
  for j = 1, #LIFE do
    local c = LIFE[j]
    if not c.leader and not c.ball then c.pose = pose_for(c) end
  end
  cut_wanted(SLICE)
  for j = 1, #LIFE do
    local c = LIFE[j]
    if not c.leader and not c.ball and not c.pose then c.pose = pose_for(c) end
  end
  if frameNo % 30 == 0 then forget_old() end

  -- the still picture, made again when the light has moved
  if light_tables() or not stillKey or not px.restore() then
    draw_still()
    px.save()
    stillKey = true
  end
  draw_surface()
  draw_shafts()
  draw_caustics()
  draw_snow(false)

  -- everything in the tank, back to front. The animals keep last frame's
  -- order, so sorting it again is a pass or two; the reef never moves and is
  -- merged in as it stands.
  local m = 0
  for j = 1, #ORDER do
    local c = ORDER[j]
    if not c.dead then m = m + 1; ORDER[m] = c end
  end
  for j = m + 1, #ORDER do ORDER[j] = nil end
  for j = 1, #LIFE do
    local c = LIFE[j]
    if not c.leader and not c.inOrder then m = m + 1; ORDER[m] = c; c.inOrder = true end
  end
  for a = 2, m do
    local v = ORDER[a]
    local b = a - 1
    while b >= 1 and ORDER[b].z > v.z do ORDER[b + 1] = ORDER[b]; b = b - 1 end
    ORDER[b + 1] = v
  end
  local n, di, oi = 0, 1, 1
  local nd = #DECOR
  while di <= nd or oi <= m do
    local d, o = DECOR[di], ORDER[oi]
    if o and (not d or o.z < d.z) then n = n + 1; LIST[n] = o; oi = oi + 1
    else n = n + 1; LIST[n] = d; di = di + 1 end
  end
  for j = n + 1, #LIST do LIST[j] = nil end
  for j = 1, n do
    local o = LIST[j]
    if o.decor then draw_decor(o)
    elseif o.eels then draw_eels(o)
    elseif o.ball then draw_ball(o)
    else draw_creature(o) end
  end

  draw_sparks()
  draw_snow(true)
  draw_caption()
  draw_clock()
end

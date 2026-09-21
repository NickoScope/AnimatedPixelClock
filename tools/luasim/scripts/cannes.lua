-- @upload-only
-- CANNES - the Festival d'Art Pyrotechnique, over the bay.
--
-- Not compiled into the image: this one is sent to a running panel with
-- effect_upload (src/lua/lua_store.h), which is what the four uploaded slots
-- are for. The marker on the line above is what keeps gen_effects.py from
-- embedding it.
--
-- Shells climb from the Croisette, burst above the water, and the water takes
-- it all back. Three things do most of the work:
--
--   * the canvas is not cleared between frames on this firmware, so the trails
--     are free: every frame blends the sky a little way towards its own colour
--     and whatever was drawn last frame becomes this frame's smoke. One
--     px.blend a pixel, not a read and a write.
--   * gravity and drag on every ember, so a burst opens fast, slows, and falls
--     - which is what makes a firework read as a firework rather than a circle.
--   * the bay mirrors the sky, dimmer and jittered, because still water is not
--     a mirror and a perfect reflection looks like a bug.
--
-- The random numbers come from an LCG written out here rather than from
-- math.random: an effect has to draw the same picture on the panel and in the
-- simulator or fx_parity cannot compare them, and Lua seeds its own generator
-- differently in each.

PERIOD = 60.0          -- px.t() is then the second hand
FPS = 15                -- see the note on FADE: the sky pass is the cost here

local W, H       = 128, 64
local HORIZON    = 45            -- sky above, the Croisette on it, sea below
local SEA_TOP    = HORIZON + 1

-- The night over the bay is not black: the town throws enough light to keep the
-- lower sky warm, and the sea keeps a little of it.
local SKY_R, SKY_G, SKY_B = 2, 3, 9
-- How far the sky is pulled back towards its own colour each frame.
--
-- A note for whoever optimises this next, because the obvious move does not
-- work. The 128 x 45 fade is 5,760 px.blend calls and looks like the whole
-- cost; it is not. Halving it to a checkerboard - 2,880 calls, with the
-- constant raised to keep the same decay - was measured on the panel at
-- 8.2 fps and 119 ms a frame, which is what the full pass measured. Not a
-- millisecond. The frame is spent somewhere else: the effect task shares core 0
-- with Wi-Fi, and at ~100,000 VM instructions a frame the bench rate accounts
-- for 40 ms of the 120. The rest is being preempted, not computed.
--
-- So the simple version stays. It draws better and costs the same.
local FADE = 0.14

local floor, sqrt, sin, cos = math.floor, math.sqrt, math.sin, math.cos
local PI2 = math.pi * 2

-- ------------------------------------------------------------------ random
-- xorshift32, and the reason it is not the textbook LCG is worth writing down:
-- this Lua is built with LUA_32BITS, so lua_Integer is int32 and lua_Number is
-- a single-precision float. `seed * 1103515245` therefore OVERFLOWS, the
-- sequence collapses, and the first version of this effect launched every shell
-- from one of two positions in one of two colours - eighty-one launches, two
-- pictures. It looked deliberate, which is the worst kind of wrong.
--
-- xorshift32 is built for exactly 32 bits: the shifts wrap by definition and
-- nothing ever needs a value the type cannot hold.
local seed = 0x2A1F3B7D
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7FFFFFFF) / 2147483648.0
end
local function between(a, b) return a + (b - a) * rnd() end

-- ------------------------------------------------------------------ embers
-- Parallel arrays rather than a table each: 200 little tables would be 200
-- allocations a burst, and the GC is generational but not free.
local MAX = 220
local ex, ey, evx, evy, elife, emax, er, eg, eb, ekind = {}, {}, {}, {}, {}, {}, {}, {}, {}, {}
local n = 0

local KIND_PEONY, KIND_WILLOW, KIND_GLITTER = 0, 1, 2

local function ember(x, y, vx, vy, life, r, g, b, kind)
  if n >= MAX then return end
  n = n + 1
  ex[n], ey[n], evx[n], evy[n] = x, y, vx, vy
  elife[n], emax[n] = life, life
  er[n], eg[n], eb[n], ekind[n] = r, g, b, kind
end

-- ------------------------------------------------------------------ shells
local MAXSH = 4
local shx, shy, shvy, shtarget, shr, shg, shb, shkind = {}, {}, {}, {}, {}, {}, {}, {}
local ns = 0

-- The palette of a festival night: nothing muddy, and each one has a highlight
-- the ember fades through so a star is white-hot before it takes its colour.
local PAL = {
  {255,  70,  60}, {255, 150,  40}, {255, 210,  90}, { 90, 255, 130},
  { 80, 180, 255}, {200, 120, 255}, {255, 255, 230}, {255,  90, 170},
}

local function launch()
  if ns >= MAXSH then return end
  ns = ns + 1
  -- From along the Croisette, never from the very edges: a shell that bursts
  -- half off the panel looks like a mistake rather than a firework.
  shx[ns] = floor(between(18, W - 18))
  shy[ns] = HORIZON - 1
  shvy[ns] = -between(1.25, 1.75)
  shtarget[ns] = between(13, 27)   -- high enough to open, low enough to be seen whole
  local c = PAL[floor(rnd() * #PAL) + 1]
  shr[ns], shg[ns], shb[ns] = c[1], c[2], c[3]
  local k = rnd()
  shkind[ns] = (k < 0.55) and KIND_PEONY or ((k < 0.82) and KIND_WILLOW or KIND_GLITTER)
end

local function burst(x, y, r, g, b, kind)
  -- No px.glow here. It was tried twice and both times it washed the middle of
  -- the burst to grey, which reads as a hole punched in the firework. The
  -- embers already carry the flash: each one is white-hot for its first breath
  -- and takes the shell's colour after, so the bloom happens on its own and in
  -- the right colour.
  local count = (kind == KIND_WILLOW) and 34 or 46
  local speed = (kind == KIND_WILLOW) and 1.05 or 1.55
  for i = 1, count do
    -- An even ring with a little scatter: a perfectly even one reads as a
    -- wheel, and a purely random one reads as a cloud.
    local a = (i / count) * PI2 + between(-0.06, 0.06)
    local s = speed * between(0.72, 1.0)
    local life = (kind == KIND_WILLOW) and between(48, 78) or between(30, 46)
    ember(x, y, cos(a) * s, sin(a) * s * 0.85, life, r, g, b, kind)
  end
end

local function shells()
  for i = ns, 1, -1 do
    shy[i] = shy[i] + shvy[i]
    shvy[i] = shvy[i] + 0.022              -- gravity slows the climb
    -- The rising trail, warm and thin, with a spark shed behind it.
    px.blend(floor(shx[i]), floor(shy[i]), 255, 200, 120, 0.95)
    if rnd() < 0.55 then
      ember(shx[i], shy[i], between(-0.12, 0.12), between(0.0, 0.25),
            between(5, 11), 255, 170, 70, KIND_GLITTER)
    end
    if shy[i] <= shtarget[i] or shvy[i] >= -0.15 then
      burst(shx[i], shy[i], shr[i], shg[i], shb[i], shkind[i])
      shx[i], shy[i], shvy[i] = shx[ns], shy[ns], shvy[ns]
      shtarget[i], shr[i], shg[i], shb[i], shkind[i] =
        shtarget[ns], shr[ns], shg[ns], shb[ns], shkind[ns]
      ns = ns - 1
    end
  end
end

-- ------------------------------------------------------------------- scene
local function bay()
  -- The sea, in bands: darker with depth, and the horizon a shade warmer where
  -- the town's light sits on it.
  for y = SEA_TOP, H - 1 do
    local d = (y - SEA_TOP) / (H - SEA_TOP)
    local v = 1.0 - d * 0.65
    px.rect(0, y, W, 1, floor(3 * v), floor(6 * v), floor(18 * v), true)
  end
  -- The Croisette: a line of lamps, the odd one brighter, and their light
  -- spilling one row down into the water.
  for x = 0, W - 1, 3 do
    local warm = (x * 7 % 11 == 0) and 1.0 or 0.55
    px.blend(x, HORIZON, floor(255 * warm), floor(190 * warm), floor(110 * warm), 0.85)
    px.blend(x, SEA_TOP, floor(180 * warm), floor(130 * warm), floor(70 * warm), 0.30)
  end
end

-- Whatever burns in the sky burns again in the water: dimmer, lower, and never
-- quite in the same place.
local function reflect()
  for i = 1, n do
    local y = ey[i]
    if y < HORIZON - 2 then
      local ry = SEA_TOP + (HORIZON - y) * 0.55
      if ry < H then
        local f = (elife[i] / emax[i]) * 0.42
        local jx = ex[i] + between(-0.9, 0.9)
        px.blend(floor(jx), floor(ry), er[i], eg[i], eb[i], f)
      end
    end
  end
end

-- ------------------------------------------------------------------- draw
local frame = 0

function draw()
  frame = frame + 1

  -- The whole sky pulled a little towards its own colour. This is the trails,
  -- the smoke and the clearing, all in one pass and one call a pixel.
  for y = 0, HORIZON - 1 do
    for x = 0, W - 1 do
      px.blend(x, y, SKY_R, SKY_G, SKY_B, FADE)
    end
  end

  bay()
  -- The town's light on the underside of the sky. Two rows is enough to stop
  -- the horizon reading as a cut edge.
  px.blend(0, HORIZON - 1, 26, 16, 10, 0.20)
  for x = 0, W - 1, 2 do
    px.blend(x, HORIZON - 1, 30, 18, 11, 0.22)
    px.blend(x, HORIZON - 2, 16, 10, 7, 0.14)
  end

  if rnd() < 0.10 and ns < MAXSH then launch() end
  shells()

  -- Embers: move, age, draw. Walked backwards so a dead one can be swapped
  -- with the last without disturbing the walk.
  for i = n, 1, -1 do
    ex[i] = ex[i] + evx[i]
    ey[i] = ey[i] + evy[i]
    evy[i] = evy[i] + 0.030               -- gravity
    evx[i] = evx[i] * 0.975               -- drag: the burst opens, then hangs
    evy[i] = evy[i] * 0.975
    elife[i] = elife[i] - 1

    local x, y = floor(ex[i]), floor(ey[i])
    if elife[i] <= 0 or y >= HORIZON or x < 0 or x >= W then
      ex[i], ey[i], evx[i], evy[i] = ex[n], ey[n], evx[n], evy[n]
      elife[i], emax[i] = elife[n], emax[n]
      er[i], eg[i], eb[i], ekind[i] = er[n], eg[n], eb[n], ekind[n]
      n = n - 1
    else
      local t = elife[i] / emax[i]
      local k = ekind[i]
      local r, g, b = er[i], eg[i], eb[i]
      if t > 0.82 then
        -- white-hot for the first breath, then its own colour
        local m = (t - 0.82) / 0.18
        r = floor(r + (255 - r) * m)
        g = floor(g + (255 - g) * m)
        b = floor(b + (255 - b) * m)
      end
      local a = t * t * 0.55 + t * 0.45    -- holds its light, then goes quickly
      if k == KIND_GLITTER and (frame + i) % 3 == 0 then a = a * 0.25 end
      if y >= 0 then px.blend(x, y, r, g, b, a) end
    end
  end

  reflect()

  -- The time, low over the water, the way a clock on a promenade would be.
  local tt = px.now()
  local s = string.format("%02d:%02d", tt.hour, tt.min)
  local tx = floor((W - px.width(s)) / 2)
  px.text(tx, H - 8, s, 210, 180, 130)
end

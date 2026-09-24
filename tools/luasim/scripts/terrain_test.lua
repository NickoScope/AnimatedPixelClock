-- @upload-only
-- TERRAIN TEST - px.terrain under every argument it clamps, for fx_parity.
--
-- Not a screen for the panel: fx_parity.py renders every script here through
-- luasim and fxhost and compares them pixel for pixel, and the golf scripts
-- only reach px.terrain after their first hole is built, far past the frames
-- the parity cases run. This one draws the ground from its first frame, with a
-- camera that circles, and on alternate frames with the extremes: the widest
-- columns, the shortest step and the longest reach, water and every pattern.

PERIOD = 20
FPS = 15

local W, H = px.size()
local floor, sin, cos = math.floor, math.sin, math.cos

-- a 64 x 48 grid of 4 m cells: a valley with a pond, a green, sand and stripes
local NX, NY, CELL = 64, 48, 4
local hb, kb, lb = {}, {}, {}
local rows_h, rows_k, rows_l = {}, {}, {}
for j = 0, NY - 1 do
  for i = 0, NX - 1 do
    local x, y = i * CELL, (j - NY / 2) * CELL
    local h = 1.5 + 1.2 * sin(x * 0.05) * cos(y * 0.07) + 0.02 * math.abs(y) ^ 1.2
    local k = 0
    if math.abs(y) < 14 then k = 2 elseif math.abs(y) < 18 then k = 1 end
    if (x - 200) ^ 2 + y ^ 2 < 144 then k, h = 4, 2.2 end
    if (x - 180) ^ 2 + (y - 16) ^ 2 < 40 then k, h = 5, 1.0 end
    if (x - 100) ^ 2 + (y + 30) ^ 2 < 300 then k, h = 6, 0.2 end
    if x < 12 and math.abs(y) < 8 then k, h = 7, 2.0 end
    hb[i + 1] = floor(math.max(0, math.min(255, (h + 2) / 0.05)))
    kb[i + 1] = k
    lb[i + 1] = ((i * 7 + j * 3) % 9 == 0) and 92 or 128
  end
  rows_h[j + 1] = string.char(table.unpack(hb, 1, NX))
  rows_k[j + 1] = string.char(table.unpack(kb, 1, NX))
  rows_l[j + 1] = string.char(table.unpack(lb, 1, NX))
end
local GRID = {w = NX, h = NY, cell = CELL, x0 = 0, y0 = -NY / 2 * CELL, hbase = -2, hscale = 0.05,
              height = table.concat(rows_h), kind = table.concat(rows_k), light = table.concat(rows_l)}
local function k(r, g, b, pat, per, amt) return string.char(r, g, b, pat, per, amt) end
local KINDS = k(86, 124, 50, 3, 1, 8) .. k(100, 148, 58, 0, 1, 0) .. k(108, 166, 62, 1, 9, 7) .. k(96, 172, 64, 0, 1, 0)
           .. k(104, 190, 74, 2, 3, 5) .. k(236, 222, 182, 3, 1, 4) .. k(38, 92, 128, 4, 1, 0) .. k(100, 172, 66, 2, 3, 5)

local frame = 0
function draw()
  frame = frame + 1
  local t = px.t() * 6.2832
  px.clear(150, 190, 230)
  local cam = {x = 128 + cos(t) * 150, y = sin(t) * 150, z = 30 + 20 * sin(t * 2), f = 100}
  cam.yaw = math.atan(-cam.y, 128 - cam.x)
  cam.hor = 20
  local extreme = frame % 2 == 0
  px.terrain{grid = GRID, cam = cam, kinds = KINDS, haze = {178, 196, 212}, fog0 = 60, fogr = 400,
             far = {58, 84, 46}, farh = 4, sky = {38, 98, 196}, deep = {26, 64, 96}, sun = {-0.45, -0.35, 0.82},
             colw = extreme and 8 or 1, step = extreme and 1.0 or 1.03, zfar = extreme and 1e9 or 700,
             grass = 60, frame = frame}
  -- the camera on the ground, looking along it: the near cells and the grain
  if frame % 3 == 0 then
    px.terrain{grid = GRID, cam = {x = 10, y = 0, z = 3, yaw = 0.1, hor = 30, f = 110}, kinds = KINDS, frame = frame}
  end
end

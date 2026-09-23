-- @upload-only
-- @by openclaw
-- FLIP DOT CLOCK - A real flip-disc board, the kind that used to clatter above a railway platform
FPS = 24
PERIOD = 60

local floor = math.floor
local _c, _r, _p, _l = px.circle, px.rect, px.pixel, px.line
local _t, _b, _gl, _g = px.text, px.blend, px.glow, px.get

local function Circle(x,y,r,cr,cg,cb,f) _c(floor(x),floor(y),floor(r),floor(cr),floor(cg),floor(cb),f) end
local function Line(x1,y1,x2,y2,cr,cg,cb) _l(floor(x1),floor(y1),floor(x2),floor(y2),floor(cr),floor(cg),floor(cb)) end
local function Rect(x,y,w,h,cr,cg,cb,f) _r(floor(x),floor(y),floor(w),floor(h),floor(cr),floor(cg),floor(cb),f) end
local function Pixel(x,y,cr,cg,cb) _p(floor(x),floor(y),floor(cr),floor(cg),floor(cb)) end
local function Glow(x,y,r,cr,cg,cb,a) _gl(floor(x),floor(y),floor(r),floor(cr),floor(cg),floor(cb),floor(a)) end

local seed = 0x2A1F3B7D
local function rnd()
  seed = seed ~ (seed << 13)
  seed = seed ~ (seed >> 17)
  seed = seed ~ (seed << 5)
  return (seed & 0x7FFFFFFF) / 2147483648.0
end

-- 5-wide x 7-tall digit font
local digits = {
  {"01110","10001","10011","10101","11001","10001","01110"},
  {"00100","01100","00100","00100","00100","00100","01110"},
  {"01110","10001","00001","00010","00100","01000","11111"},
  {"11110","00001","00001","01110","00001","00001","11110"},
  {"00010","00110","01010","10010","11111","00010","00010"},
  {"11111","10000","10000","11110","00001","00001","11110"},
  {"01110","10000","10000","11110","10001","10001","01110"},
  {"11111","00001","00010","00100","01000","01000","01000"},
  {"01110","10001","10001","01110","10001","10001","01110"},
  {"01110","10001","10001","01111","00001","00001","01110"}
}

-- ---- Full flip-dot matrix that covers the whole screen ----
-- 25 columns x 12 rows of discs at a 5px pitch, centred. Every cell holds a
-- disc; the clock is drawn by lighting the discs that fall inside the digits.
local GW, GH, P = 25, 12, 5
local OX, OY = 3, 4          -- centre of grid cell (0,0)
local RY = 2                 -- digit top row on the grid (rows 2..8)
local digitCols = {0, 6, 14, 20}   -- start grid-column of H1,H2,M1,M2
local colonCol = 12
local colonRowA, colonRowB = RY + 2, RY + 4   -- grid rows 4 and 6

local BG_R, BG_G, BG_B = 3, 3, 3

local discs = {}     -- every disc, for the one-time initial paint
local dynamic = {}   -- only the discs that can change (digit cells + colon)
local initialized = false

-- A single domed flip disc: bright 3x3 core, darker rim tips (which also
-- separate neighbours on the packed grid), a top-left specular highlight, and a
-- faint glow when lit. Mid-flip it squashes to an edge-on sliver.
local function drawDisc(x,y,lit,scale)
  Rect(x-2,y-2,5,5,BG_R,BG_G,BG_B,true)
  local h = floor(5*scale+0.5)
  if h < 1 then h = 1 end
  if lit == 1 then
    if h >= 5 then
      Glow(x,y,3,255,140,20,14)
      Rect(x-1,y-1,3,3,252,172,40,true)
      Pixel(x-2,y,196,116,16); Pixel(x+2,y,196,116,16)
      Pixel(x,y-2,196,116,16); Pixel(x,y+2,196,116,16)
      Pixel(x-1,y+1,224,150,30); Pixel(x+1,y+1,224,150,30)
      Pixel(x+1,y-1,238,162,34)
      Pixel(x-1,y-1,255,232,150)
    else
      local y0 = y - floor(h/2)
      for i=0,h-1 do Line(x-1,y0+i,x+1,y0+i,236,150,26) end
      Line(x-1,y0,x+1,y0,255,206,96)
    end
  else
    if h >= 5 then
      Rect(x-1,y-1,3,3,50,42,30,true)
      Pixel(x-2,y,28,24,17); Pixel(x+2,y,28,24,17)
      Pixel(x,y-2,28,24,17); Pixel(x,y+2,28,24,17)
      Pixel(x+1,y+1,34,29,22)
      Pixel(x-1,y-1,86,72,54)
    else
      local y0 = y - floor(h/2)
      for i=0,h-1 do Line(x-1,y0+i,x+1,y0+i,44,37,27) end
    end
  end
end

local function digitBit(n,row,col)
  return string.sub(digits[n+1][row],col,col) == "1" and 1 or 0
end

-- classify a grid cell: returns slot(1..4)+drow+dcol for a digit cell,
-- or colon=true, else nil (a permanently dark board dot)
local function classify(gx,gy)
  for si=1,4 do
    local s = digitCols[si]
    if gx >= s and gx <= s+4 and gy >= RY and gy <= RY+6 then
      return si, gy-RY+1, gx-s+1, false
    end
  end
  if gx == colonCol and (gy == colonRowA or gy == colonRowB) then
    return nil, nil, nil, true
  end
  return nil
end

local function litFor(d,n)
  if d.slot then
    local h, m = n.hour or 0, n.min or 0
    local vals = {floor(h/10),h%10,floor(m/10),m%10}
    return digitBit(vals[d.slot],d.drow,d.dcol)
  else
    return ((n.sec or 0) % 2 == 0) and 1 or 0   -- colon flips every second
  end
end

local function buildBoard(n)
  px.clear(0,0,0)
  for gy=0,GH-1 do
    for gx=0,GW-1 do
      local x, y = OX + gx*P, OY + gy*P
      local slot, drow, dcol, colon = classify(gx,gy)
      local d = {x=x, y=y, slot=slot, drow=drow, dcol=dcol, colon=colon,
                 c=(dcol or 1), row=(drow or 1), shown=0, target=0,
                 active=false, start=0, jitter=rnd()*0.045}
      if slot or colon then
        d.shown = litFor(d,n); d.target = d.shown
        dynamic[#dynamic+1] = d
      end
      drawDisc(x,y,d.shown,1)
      discs[#discs+1] = d
    end
  end
  initialized = true
end

local function boardClock(n)
  local phase = px.t()*60
  local frac = phase-floor(phase)
  return (n.hour or 0)*3600+(n.min or 0)*60+(n.sec or 0)+frac
end

function draw()
  local n = px.now()
  if not initialized then buildBoard(n) return end
  local clock = boardClock(n)
  for _,d in ipairs(dynamic) do
    local want = litFor(d,n)
    if want ~= d.target then
      d.target = want
      d.active = true
      d.start = clock + d.c*0.018 + d.row*0.006 + d.jitter
    end
    if d.active and clock >= d.start then
      local u = (clock-d.start)/0.24
      if u >= 1 then
        d.shown = d.target; d.active = false; drawDisc(d.x,d.y,d.shown,1)
      elseif u < 0.5 then
        drawDisc(d.x,d.y,d.shown,1-u*2)
      else
        drawDisc(d.x,d.y,d.target,(u-0.5)*2)
      end
    end
  end
end

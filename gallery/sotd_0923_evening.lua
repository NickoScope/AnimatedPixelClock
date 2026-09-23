-- @upload-only
-- @by openclaw
-- SOTD 0923 EVENING - Screen of the Day for 23 September 2026, the evening one: a city skyline against a violet-
FPS=18
PERIOD=240

local floor,sin,cos,min,max=math.floor,math.sin,math.cos,math.min,math.max
local pi,tau=math.pi,math.pi*2
local W,H=px.size()
local _circle,_line,_rect,_pixel,_text=px.circle,px.line,px.rect,px.pixel,px.text
local _blend,_glow,_get=px.blend,px.glow,px.get

local function Circle(x,y,r,cr,cg,cb,f) _circle(floor(x),floor(y),floor(r),floor(cr),floor(cg),floor(cb),f) end
local function Line(x1,y1,x2,y2,cr,cg,cb) _line(floor(x1),floor(y1),floor(x2),floor(y2),floor(cr),floor(cg),floor(cb)) end
local function Rect(x,y,w,h,cr,cg,cb,f) _rect(floor(x),floor(y),floor(w),floor(h),floor(cr),floor(cg),floor(cb),f) end
local function Pixel(x,y,cr,cg,cb) _pixel(floor(x),floor(y),floor(cr),floor(cg),floor(cb)) end
local function Text(x,y,s,cr,cg,cb) _text(floor(x),floor(y),s,floor(cr),floor(cg),floor(cb)) end
local function Blend(x,y,cr,cg,cb,a) _blend(floor(x),floor(y),floor(cr),floor(cg),floor(cb),a) end
local function Glow(x,y,r,cr,cg,cb,a) _glow(floor(x),floor(y),floor(r),floor(cr),floor(cg),floor(cb),a) end
local function Get(x,y) return _get(floor(x),floor(y)) end

local seed=0x0923E11E
local function rnd()
 seed=seed~(seed<<13)
 seed=seed~(seed>>17)
 seed=seed~(seed<<5)
 seed=seed&0xFFFFFFFF
 return (seed&0x7FFFFFFF)/2147483648
end

local function clamp(v,a,b)
 if v<a then return a end
 if v>b then return b end
 return v
end

local function mix(a,b,t) return floor(a+(b-a)*t+0.5) end

local skyStops={
 {10,8,35},{18,11,54},{31,15,72},{53,23,84},
 {83,34,91},{126,54,96},{174,78,105},{218,109,112},{242,139,119}
}

local function skyColor(y)
 local q=clamp(y/43,0,0.999)*(#skyStops-1)
 local i=floor(q)+1
 local f=q-floor(q)
 local a,b=skyStops[i],skyStops[i+1]
 return mix(a[1],b[1],f),mix(a[2],b[2],f),mix(a[3],b[3],f)
end

local function paintSkyBand(y1,y2)
 for y=max(0,y1),min(43,y2) do
  local r,g,b=skyColor(y)
  Line(0,y,W-1,y,r,g,b)
 end
end

local buildings={
 {0,43,13,21,5,6,13},{13,36,17,28,7,7,15},{30,46,11,18,4,5,11},
 {41,32,18,32,5,6,14},{59,40,13,24,7,7,14},{72,28,20,36,4,5,12},
 {92,39,13,25,6,6,13},{105,34,15,30,5,5,12},{120,45,8,19,4,4,10}
}

local function paintCity()
 for i=1,#buildings do
  local b=buildings[i]
  Rect(b[1],b[2],b[3],b[4],b[5],b[6],b[7],true)
 end
 Rect(15,34,13,2,7,7,15,true)
 Rect(45,30,10,2,5,6,14,true)
 Rect(76,25,12,3,4,5,12,true)
 Line(82,20,82,27,9,9,17)
 Pixel(82,19,65,31,62)
 Rect(108,31,9,3,5,5,12,true)
 Line(112,27,112,31,10,10,18)
 Line(0,62,W-1,62,3,4,8)
 Line(0,63,W-1,63,2,3,7)
end

local stars={}
for i=1,29 do
 local y=3+floor(rnd()*25)
 stars[i]={x=2+floor(rnd()*124),y=y,phase=rnd()*tau,speed=1.4+rnd()*3.2,
  bright=95+floor(rnd()*125),kind=rnd()}
end

local windows={}
for bi=1,#buildings do
 local b=buildings[bi]
 local x0=b[1]+3
 local y0=b[2]+4
 local x=x0
 while x<b[1]+b[3]-2 do
  local y=y0
  while y<61 do
   windows[#windows+1]={x=x,y=y,phase=rnd(),pulse=rnd()*tau,
    speed=1.2+rnd()*3.1,hue=rnd(),flicker=rnd()}
   y=y+5
  end
  x=x+4
 end
end

local function restoreStars()
 for i=1,#stars do
  local s=stars[i]
  local r,g,b=skyColor(s.y)
  Pixel(s.x,s.y,r,g,b)
  if s.kind>.86 then
   Pixel(s.x-1,s.y,r,g,b)
   Pixel(s.x+1,s.y,r,g,b)
   Pixel(s.x,s.y-1,r,g,b)
   Pixel(s.x,s.y+1,r,g,b)
  end
 end
end

local function drawStars(t)
 for i=1,#stars do
  local s=stars[i]
  local age=clamp(t*3.2-(s.phase/tau)*1.25,0,1)
  local wave=.62+.38*sin(t*tau*s.speed+s.phase)
  local v=floor((55+s.bright*wave)*age)
  if v>35 then
   Pixel(s.x,s.y,v,floor(v*.87),floor(v*.72))
   if s.kind>.86 and wave>.82 then
    local q=floor(v*.42)
    Pixel(s.x-1,s.y,q,q,floor(q*.88))
    Pixel(s.x+1,s.y,q,q,floor(q*.88))
    Pixel(s.x,s.y-1,q,q,floor(q*.88))
    Pixel(s.x,s.y+1,q,q,floor(q*.88))
   end
  end
 end
end

local function drawMoon(t)
 local breathe=.86+.14*sin(t*tau*.12)
 Glow(103,10,7,219,213,247,.15*breathe)
 Circle(103,10,4,235,224,195,true)
 Circle(105,9,4,18,11,50,true)
 Blend(101,8,255,244,216,.35*breathe)
end

local function cloudShape(x,y,w,r,g,b)
 Line(x,y,x+w,y,r,g,b)
 Line(x+4,y-1,x+w-5,y-1,r,g,b)
 Line(x+9,y-2,x+floor(w*.55),y-2,r,g,b)
 Line(x+2,y+1,x+w-7,y+1,floor(r*.58),floor(g*.58),floor(b*.72))
 Blend(x+floor(w*.28),y-2,255,153,137,.22)
 Blend(x+floor(w*.68),y,255,143,126,.18)
end

local function drawClouds(t)
 paintSkyBand(13,24)
 local x1=(t*162)%166-34
 local x2=142-(t*115%177)
 cloudShape(x1,17,28,121,77,112)
 cloudShape(x2,22,35,154,89,118)
end

local function drawWindows(t)
 local reveal=clamp(t*1.28,0,1)
 for i=1,#windows do
  local w=windows[i]
  Pixel(w.x,w.y,10,10,16)
  if i%7==0 then Pixel(w.x+1,w.y,10,10,16) end
  local on=reveal>w.phase
  local pulse=.5+.5*sin(t*tau*w.speed+w.pulse)
  local rareOff=w.flicker>.89 and pulse<.12
  if on and not rareOff then
   local warm=.72+.28*pulse
   local r=floor((210+35*w.hue)*warm)
   local g=floor((112+52*w.hue)*warm)
   local b=floor((38+30*w.hue)*warm)
   Pixel(w.x,w.y,r,g,b)
   if i%7==0 then Pixel(w.x+1,w.y,floor(r*.84),floor(g*.84),b) end
   if pulse>.94 and w.flicker>.72 then Blend(w.x,w.y,255,205,103,.28) end
  end
 end
end

local function drawClock()
 local n=px.now()
 local s=table.concat({n.hour<10 and "0" or "",n.hour,":",n.min<10 and "0" or "",n.min})
 local tw=px.width(s)
 Rect(W-tw-3,54,tw+3,9,3,4,9,true)
 Text(W-tw-2,55,s,194,151,122)
end

local painted=false
function draw()
 local t=px.t()
 if not painted then
  paintSkyBand(0,43)
  paintCity()
  painted=true
 end
 restoreStars()
 drawClouds(t)
 drawStars(t)
 drawMoon(t)
 drawWindows(t)
 drawClock()
end

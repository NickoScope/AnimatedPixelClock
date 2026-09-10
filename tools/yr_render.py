#!/usr/bin/env python3
"""Host render of the yacht radar page, mirroring src/yachtradar/yachtradar.cpp.

Reads the coastline straight out of the generated header, so the picture cannot
drift from what the panel draws.

  python3 tools/yr_render.py vessels.json out.png [scale]
"""
import json, pathlib, re, sys
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
W, H = 128, 64
G = json.load(open(ROOT / "tools/picopixel.json"))["glyphs"]
YADV = json.load(open(ROOT / "tools/picopixel.json"))["yAdvance"]

def _consts():
    t = (ROOT / "src/yachtradar/yachtradar.cpp").read_text()
    out = {}
    for m in re.finditer(r"static const (?:int16_t|uint16_t|float)\s+(YR_[A-Z0-9_]+)\s*=\s*(-?[\d.]+)f?;", t):
        out[m.group(1)] = float(m.group(2))
    return out
C = _consts()
CX, CY, RPX = int(C["YR_CX"]), int(C["YR_CY"]), int(C["YR_R_PX"])
X_TABLE, X_RIGHT, GAP = int(C["YR_X_TABLE"]), int(C["YR_X_RIGHT"]), int(C["YR_GAP"])
Y_TITLE, Y_SUB, Y_RULE = int(C["YR_Y_TITLE"]), int(C["YR_Y_SUB"]), int(C["YR_Y_RULE"])
Y_ROW0, ROW_H = int(C["YR_Y_ROW0"]), int(C["YR_ROW_H"])
CENTRE_LAT, CENTRE_LON = C["YR_CENTRE_LAT"], C["YR_CENTRE_LON"]
DAC_PER_KM, R_DAC = C["YR_DAC_PER_KM"], C["YR_R_DAC"]
ROWS = int(re.search(r"#define YR_TABLE_ROWS\s+(\d+)",
                     (ROOT / "src/yachtradar/yachtradar.h").read_text()).group(1))

def _coast():
    t = (ROOT / "src/yachtradar/coastline.h").read_text()
    segs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", t)]
    body = re.sub(r"//.*", "", re.search(r"kYrCoastXY\[[^\]]*\]\s*=\s*\{(.*?)\};", t, re.S).group(1))
    v = [int(x) for x in re.findall(r"-?\d+", body)]
    return segs, list(zip(v[0::2], v[1::2]))
SEGS, XY = _coast()

GRID=(22,30,36); COAST=(0,120,70); WHITE=(255,255,255); DIM=(110,122,128); RULE=(40,48,54)
px = [[(10,12,14)]*W for _ in range(H)]
def put(x,y,c):
    if 0<=x<W and 0<=y<H: px[y][x]=c
def tw(s): return sum((G.get(c) or G[" "])["adv"] for c in s.upper())
def text(x,y,s,c):
    for ch in s.upper():
        g=G.get(ch) or G[" "]; oy=(YADV-1)+g["yo"]
        for r in range(g["h"]):
            for cc in range(g["w"]):
                if (g["rows"][r]>>cc)&1: put(x+cc+g["xo"], y+r+oy, c)
        x+=g["adv"]
def line(x0,y0,x1,y1,c):
    dx,dy=abs(x1-x0),-abs(y1-y0); sx=1 if x0<x1 else -1; sy=1 if y0<y1 else -1; e=dx+dy
    while True:
        put(x0,y0,c)
        if x0==x1 and y0==y1: return
        e2=2*e
        if e2>=dy: e+=dy; x0+=sx
        if e2<=dx: e+=dx; y0+=sy
def circle(cx,cy,r,c):
    x,y,d=r,0,1-r
    while x>=y:
        for a,b in ((x,y),(y,x),(-x,y),(-y,x),(x,-y),(y,-x),(-x,-y),(-y,-x)): put(cx+a,cy+b,c)
        y+=1
        d = d+2*y+1 if d<0 else d+2*(y-(x:=x-1))+1

def colour(v):
    if v.get("len",0) >= 60: return (255,180,0)
    s = v.get("sog",0.0)
    return (0,200,95) if s<0.5 else ((0,200,200) if s<3.0 else (70,150,255))
def project(v):
    import math
    dx=(v["lon"]-CENTRE_LON)*111.320*math.cos(math.radians(CENTRE_LAT))*DAC_PER_KM
    dy=(v["lat"]-CENTRE_LAT)*110.574*DAC_PER_KM
    return CX+round(dx*RPX/R_DAC), CY-round(dy*RPX/R_DAC)
def rng(v):
    import math
    x,y=project(v); return math.hypot(x-CX, y-CY)

def main():
    b=json.load(open(sys.argv[1])); S=int(sys.argv[3]) if len(sys.argv)>3 else 9
    for r in (RPX//3, RPX*2//3, RPX): circle(CX,CY,r,GRID)
    for y in range(1,63): put(CX,y,GRID)
    for x in range(0,63): put(x,CY,GRID)
    i=0
    for s in SEGS:
        n=s&0x7FFF; pts=XY[i:i+n]; i+=n
        if s&0x8000: pts=pts+[pts[0]]
        for a,c in zip(pts,pts[1:]): line(a[0],a[1],c[0],c[1],COAST)
    for lbl,(x,y) in (("N",(CX-1,1)),("S",(CX-1,57)),("W",(0,CY-3)),("E",(57,CY-3))): text(x,y,lbl,GRID)

    ves=sorted(b["vessels"], key=rng)
    for v in ves:
        x,y=project(v)
        if not (0<=x<=62 and 0<=y<=63): continue
        c=colour(v); put(x,y,WHITE); put(x-1,y,c); put(x+1,y,c)
        if v.get("len",0)>=60: put(x,y-1,c); put(x,y+1,c)

    text(X_TABLE, Y_TITLE, "BAY OF CANNES", WHITE)
    text(X_TABLE, Y_SUB, b.get("sub", f"{len(ves)} NOW  {b.get('seen',len(ves))} SEEN"), DIM)
    for x in range(X_TABLE-1, X_RIGHT+1): put(x, Y_RULE, RULE)
    for i,v in enumerate(ves[:ROWS]):
        y=Y_ROW0+i*ROW_H; c=colour(v)
        w=f"{v['len']}M" if v.get("len") else ""; ww=tw(w)
        if w: text(X_RIGHT-ww, y, w, c)
        nm=v.get("name") or f"{v['mmsi']%1000000:06d}"
        room=(X_RIGHT-ww-GAP)-X_TABLE
        while nm and tw(nm)>room:
            nm = nm.rsplit(" ",1)[0] if " " in nm else nm[:-1]
        text(X_TABLE, y, nm, c)

    Image.frombytes("RGB",(W,H),bytes(ch for row in px for p in row for ch in p)) \
        .resize((W*S,H*S), Image.NEAREST).save(sys.argv[2])
    print(f"{sys.argv[2]}  {len(ves)} судов, в таблице {min(len(ves),ROWS)}")
main()

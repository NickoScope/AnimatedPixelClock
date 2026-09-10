#!/usr/bin/env python3
"""Render layout variants of the flight board side by side, from a real payload.

Variant A - what is implemented now.
Variant B - evidence-led revision:
    * column order Flight-Time-Destination (Melissa & Theopilus 2020: fastest
      of the orders tested with an eye tracker)
    * 5 rows at 10 px instead of 6 at 8 px (the same study's passengers named
      tight line spacing as the reason they had to reread)
    * status palette cut from 6 colours to 4 (Mijksenaar's Schiphol rule:
      limit colours, people cannot separate yellow from orange or orange from
      red at a glance) and yellow reserved for the "next flight" marker alone
"""
import json, sys, pathlib
from PIL import Image

W, H = 128, 64
FONT = json.load(open(pathlib.Path(__file__).parent / "glcdfont.json"))

def text(px, x, y, s, col):
    for ch in s:
        o = ord(ch) * 5
        for cx in range(5):
            bits = FONT[o + cx]
            for cy in range(8):
                if (bits >> cy) & 1 and 0 <= x+cx < W and 0 <= y+cy < H:
                    px[x+cx, y+cy] = col
        x += 6

def hline(px, y, col, x0=0, x1=W):
    for x in range(x0, x1):
        if 0 <= y < H: px[x, y] = col

PAL_A = {"sched":(200,200,200),"board":(0,220,220),"dep":(70,120,255),
         "land":(0,200,60),"delay":(255,170,0),"canc":(255,40,40)}
# Four well-separated hues. done/pending/late/off.
PAL_B = {"sched":(210,210,210),"board":(210,210,210),"dep":(0,190,255),
         "land":(0,190,255),"delay":(255,140,0),"canc":(255,40,40)}
GREY=(110,110,110); MARK=(255,210,0); RULE=(55,55,55)

def render(p, variant):
    img = Image.new("RGB",(W,H),(0,0,0)); px = img.load()
    text(px,1,0,p["apt"],(255,255,255))
    text(px,31,0,"DEP" if p["dir"]=="dep" else "ARR",MARK)
    text(px,97,0,p["upd"],GREY)
    hline(px,9,RULE)
    rows,now=p["f"],p["now_idx"]
    if variant=="A":
        vis,pitch,y0,pal=6,8,12,PAL_A
        cols=[("tm",3),("fn",37),("ct",91)]
    else:
        vis,pitch,y0,pal=5,10,12,PAL_B
        cols=[("fn",4),("tm",52),("ct",94)]
    start=max(0,min(now-1,len(rows)-vis))
    for i in range(vis):
        idx=start+i
        if idx>=len(rows): break
        r=rows[idx]; y=y0+i*pitch; col=pal.get(r["st"],GREY)
        if idx==now:
            for yy in range(y-1,y+7):
                if 0<=yy<H: px[0,yy]=MARK; px[1,yy]=MARK
        for key,x in cols: text(px,x,y,r[key],col)
    return img

def main():
    p=json.load(open(sys.argv[1])); S=8
    a,b=render(p,"A"),render(p,"B")
    gap=8
    out=Image.new("RGB",(W*2+gap,H),(30,30,30))
    out.paste(a,(0,0)); out.paste(b,(W+gap,0))
    out.resize(((W*2+gap)*S,H*S),Image.NEAREST).save(sys.argv[2])
    print(f"{sys.argv[2]}  слева A (сейчас), справа B (по исследованиям)")
main()

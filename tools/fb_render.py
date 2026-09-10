#!/usr/bin/env python3
"""Host render of the flight board page, mirroring src/flightboard/flightboard.cpp.

Same font, same constants, same clipping rule. Used to judge the layout before
hardware exists.  python3 tools/fb_render.py payload.json out.png [scale]
"""
import json, sys, pathlib
from PIL import Image

W, H = 128, 64
FB = json.load(open(pathlib.Path(__file__).parent / "picopixel.json"))
G, YADV, ASC = FB["glyphs"], FB["yAdvance"], 4
X_TIME, X_FLIGHT, X_DEST, X_RIGHT, GAP = 2, 22, 50, 126, 2
CODE_GAP = 2
Y_HEADER, Y_RULE, Y_ROW0, ROW_H, VISIBLE = 1, 8, 11, 7, 7

APT = {"LFMD":"CANNES","LFMN":"NICE","LFPG":"PARIS CDG",
       "EGLL":"LONDON","EDDF":"FRANKFURT","EHAM":"AMSTERDAM"}
ARR = {"sched":"DUE","board":"GATE","dep":"IN AIR",
       "land":"LANDED","delay":"DELAY","canc":"CANX"}
DEP = dict(ARR, sched="ON TIME", dep="DEPART")
COL = {"sched":(210,210,210),"board":(0,220,220),"dep":(70,140,255),
       "land":(0,200,90),"delay":(255,140,0),"canc":(255,56,56)}
SIG, DIM, RULE = (255,180,0), (120,132,138), (52,60,64)

def tw(s):  return sum((G.get(c) or G[" "])["adv"] for c in s.upper())

def text(px, x, y, s, col):
    for ch in s.upper():
        g = G.get(ch) or G[" "]
        oy = (YADV - 1) + g["yo"]
        for r in range(g["h"]):
            for c in range(g["w"]):
                if (g["rows"][r] >> c) & 1:
                    X, Y = x + c + g["xo"], y + r + oy
                    if 0 <= X < W and 0 <= Y < H: px[X, Y] = col
        x += g["adv"]

def main():
    b = json.load(open(sys.argv[1]))
    S = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    img = Image.new("RGB", (W, H), (10, 13, 14)); px = img.load()

    dep = b["dir"] == "dep"; ST = DEP if dep else ARR
    apt = APT.get(b["apt"], b["apt"])
    text(px, X_TIME, Y_HEADER, apt, (255,255,255))
    text(px, X_TIME + tw(apt) + 6, Y_HEADER, "DEPARTURES" if dep else "ARRIVALS", SIG)
    text(px, X_RIGHT - tw(b["upd"]), Y_HEADER, b["upd"], DIM)
    for x in range(W): px[x, Y_RULE] = RULE

    rows, now = b["f"], b["now_idx"]
    start = max(0, min(now - 1, len(rows) - VISIBLE))
    for i in range(VISIBLE):
        idx = start + i
        if idx >= len(rows): break
        r = rows[idx]; y = Y_ROW0 + i * ROW_H; col = COL.get(r["st"], DIM)
        if idx == now:
            for yy in range(y, y + ROW_H - 1): px[0, yy] = SIG
        text(px, X_TIME, y, r["tm"], col)
        text(px, X_FLIGHT, y, r["fn"], col)

        word = ST.get(r["st"], "")
        wordW = tw(word)
        if word: text(px, X_RIGHT - wordW, y, word, col)

        text(px, X_DEST, y, r["ct"], col)                 # code, always
        x_city = X_DEST + tw(r["ct"]) + CODE_GAP
        room = (X_RIGHT - wordW - GAP) - x_city
        city = r.get("cy") or r.get("city") or ""
        while city and tw(city) > room:                   # drop whole trailing words
            city = city.rsplit(" ", 1)[0] if " " in city else ""
        if " " in city and len(city.rsplit(" ", 1)[-1]) <= 2:   # dangling AM/DE/SUR
            city = city.rsplit(" ", 1)[0]
        if city: text(px, x_city, y, city, col)

    img.resize((W * S, H * S), Image.NEAREST).save(sys.argv[2])
    print(f"{sys.argv[2]}  rows {start}..{start+VISIBLE-1} of {len(rows)}")

main()

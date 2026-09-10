#!/usr/bin/env python3
"""Host render of the flight board page.

Every constant, status word and airport name is read out of
src/flightboard/flightboard.cpp through fb_layout, so this cannot drift from
the firmware: change the firmware and this render changes with it. Used to
judge the layout before hardware exists.

  python3 tools/fb_render.py payload.json out.png [scale]
"""
import json, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from PIL import Image

import fb_layout                      # constants come from the firmware, never from here
L = fb_layout.load()
_g = L["geom"]

W, H = 128, 64
FB = json.load(open(pathlib.Path(__file__).parent / "picopixel.json"))
G, YADV, ASC = FB["glyphs"], FB["yAdvance"], _g["FB_ASCENT"]
X_TIME, X_FLIGHT   = _g["FB_X_TIME"], _g["FB_X_FLIGHT"]
X_DEST, X_RIGHT    = _g["FB_X_DEST"], _g["FB_X_RIGHT"]
GAP, CODE_GAP      = _g["FB_GAP"], _g["FB_CODE_GAP"]
Y_HEADER, Y_RULE   = _g["FB_Y_HEADER"], _g["FB_Y_RULE"]
Y_ROW0, ROW_H      = _g["FB_Y_ROW0"], _g["FB_ROW_H"]
VISIBLE            = _g["FB_VISIBLE"]

APT = dict(zip(L["airports"], L["airport_names"]))
ARR, DEP = L["status"]["arr"], L["status"]["dep"]
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

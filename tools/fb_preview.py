#!/usr/bin/env python3
"""Host-side preview of the flight board page.

Draws the same 128x64 layout the firmware draws, from a real captured payload,
so the layout can be judged before the panels arrive. Numbers here mirror the
constants at the top of src/flightboard/flightboard.cpp - if you change one,
change the other.

  python3 tools/fb_preview.py payload.json out.png [scale]
"""
import json, sys, pathlib
from PIL import Image, ImageDraw

W, H, SCALE = 128, 64, int(sys.argv[3]) if len(sys.argv) > 3 else 8

X_TIME, X_FLIGHT, X_CODE = 1, 37, 91
Y_HEADER, Y_RULE, Y_ROW0, ROW_H, VISIBLE = 0, 9, 12, 8, 6

STATUS = {
    "sched": (200, 200, 200), "board": (0, 220, 220), "dep": (70, 120, 255),
    "land": (0, 200, 60), "delay": (255, 170, 0), "canc": (255, 40, 40),
}
GREY = (120, 120, 120)

# Real Adafruit GFX built-in font: 5x7 glyphs on a 6 px advance, 8 px cell,
# cursor is the glyph's TOP-left. Bitmaps extracted from the library's
# glcdfont.c, so this preview is pixel-exact against the device.
FONT = json.load(open(pathlib.Path(__file__).parent / "glcdfont.json"))

def text(img, x, y, s, col):
    px = img.load()
    for ch in s:
        o = ord(ch) * 5
        for cx in range(5):
            bits = FONT[o + cx]
            for cy in range(8):
                if (bits >> cy) & 1:
                    X, Y = x + cx, y + cy
                    if 0 <= X < W and 0 <= Y < H:
                        px[X, Y] = col
        x += 6

def main():
    p = json.load(open(sys.argv[1]))
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)

    text(img, X_TIME, Y_HEADER, p["apt"], (255, 255, 255))
    text(img, X_TIME + 30, Y_HEADER, "DEP" if p["dir"] == "dep" else "ARR", (255, 200, 0))
    text(img, X_CODE + 6, Y_HEADER, p["upd"], GREY)
    d.line([(0, Y_RULE), (W - 1, Y_RULE)], fill=(60, 60, 60))

    rows, now = p["f"], p["now_idx"]
    start = max(0, min(now - 1, len(rows) - VISIBLE))
    for i in range(VISIBLE):
        idx = start + i
        if idx >= len(rows): break
        r = rows[idx]
        y = Y_ROW0 + i * ROW_H
        col = STATUS.get(r["st"], GREY)
        if idx == now:
            d.rectangle([0, y - 1, 0, y + 5], fill=(255, 200, 0))
        text(img, X_TIME + 2, y, r["tm"], col)
        text(img, X_FLIGHT, y, r["fn"], col)
        text(img, X_CODE, y, r["ct"], col)

    img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(sys.argv[2])
    print(f"{sys.argv[2]}  {W*SCALE}x{H*SCALE}  окно строк {start}..{start+VISIBLE-1} из {len(rows)}")

main()

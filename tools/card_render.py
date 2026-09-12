#!/usr/bin/env python3
"""Host render of a card, mirroring drawBody() in src/cards/cards.cpp.

Cards are laid out by measuring, not by counting characters, and that is
exactly the kind of thing that looks fine in the source and wrong on the panel.
This draws one without a flash cycle.

  python3 tools/card_render.py out.png
"""
import json, pathlib, sys
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
F = json.load(open(ROOT / "tools/picopixel.json"))
G, YADV = F["glyphs"], F["yAdvance"]
W, H = 128, 64

def tw(s): return sum((G.get(c) or G[" "])["adv"] for c in s.upper())

def mk():
    return [[(0, 0, 0) for _ in range(W)] for _ in range(H)]

def put(px, x, y, c):
    if 0 <= x < W and 0 <= y < H: px[y][x] = c

def text(px, x, y, s, c):
    for ch in s.upper():
        g = G.get(ch) or G[" "]
        oy = (YADV - 1) + g["yo"]
        for r in range(g["h"]):
            for cc in range(g["w"]):
                if (g["rows"][r] >> cc) & 1: put(px, x + cc + g["xo"], y + r + oy, c)
        x += g["adv"]

def rect(px, x, y, w, h, c, fill=True):
    for j in range(h):
        for i in range(w):
            if fill or j in (0, h-1) or i in (0, w-1): put(px, x+i, y+j, c)

def wrap(s, maxw):
    if tw(s) <= maxw: return s, ""
    cut = 0
    for i, ch in enumerate(s):
        if ch != " ": continue
        if tw(s[:i]) <= maxw: cut = i
        else: break
    if not cut: return s, ""
    return s[:cut], s[cut+1:]

# The same TOP-of-line numbers as CARD_Y_* in src/cards/cards.cpp.
Y_TITLE, Y_RULE, Y_TEXT, Y_TEXT1, Y_TEXT2, Y_BAR, W_TEXT = 6, 14, 28, 24, 32, 52, 118
X_ICON, Y_ICON, X_TXT2, ICON = 6, 22, 28, 16

def load_icon(path):
    """The wire format the firmware reads: 512 bytes, RGB565 big-endian."""
    import struct
    d = pathlib.Path(path).read_bytes()
    assert len(d) == ICON*ICON*2, f"{path}: {len(d)} bytes"
    out = []
    for i in range(ICON*ICON):
        v = struct.unpack_from(">H", d, i*2)[0]
        out.append((((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
    return out

def card(title, body, colour=(255,255,255), progress=None,
         bar=(0,200,255), notify=False, hold=False, icon=None):
    px = mk()
    if notify:
        rect(px, 0, 0, W, H, colour, fill=False)
    x0, x1 = (X_TXT2, 124) if icon else (0, W)
    if icon:
        ic = load_icon(icon)
        for y in range(ICON):
            for x in range(ICON):
                put(px, X_ICON + x, Y_ICON + y, ic[y*ICON + x])
    ctr = lambda s: x0 + ((x1 - x0) - tw(s)) // 2
    if title:
        text(px, ctr(title), Y_TITLE, title, (120,132,138))
        rect(px, 8, Y_RULE, 112, 1, (40,48,54))
    l1, l2 = wrap(body, x1 - x0 - 4)
    if l2:
        text(px, ctr(l1), Y_TEXT1, l1, colour)
        text(px, ctr(l2), Y_TEXT2, l2, colour)
    else:
        text(px, ctr(l1), Y_TEXT, l1, colour)
    if progress is not None:
        x, y, w, h = 10, Y_BAR, 108, 6
        rect(px, x, y, w, h, (40,48,54), fill=False)
        fill = (w - 2) * progress // 100
        if fill: rect(px, x+1, y+1, fill, h-2, bar)
    if notify and hold:
        text(px, (W - tw("PRESS")) // 2, 56, "PRESS", (90,100,110))
    return px

def main():
    scenes = [
        ("простая карточка",  card("", "BIN DAY TOMORROW")),
        ("заголовок + текст", card("LAUNDRY", "42 MINUTES LEFT", (0,200,255))),
        ("с полосой",         card("WASHING", "RINSE", (0,200,255), progress=68)),
        ("длинный текст",     card("ELECTRICITY", "CHEAP UNTIL 06:00 TOMORROW", (60,255,90))),
        ("перенос на две",    card("GARDEN", "WATERING SKIPPED BECAUSE RAIN IS FORECAST", (0,200,255))),
        ("одно длинное слово",card("", "SUPERCALIFRAGILISTIC", (255,120,255))),
        ("иконка + текст",    card("WATERING", "STARTS AT 06:00", (0,200,255),
                                   icon="/tmp/drop.i16")),
        ("иконка + полоса",   card("LAUNDRY", "RINSE", (0,200,255), progress=68,
                                   icon="/tmp/drop.i16")),
        ("уведомление",       card("DOORBELL", "SOMEONE AT THE GATE", (255,180,0),
                                   notify=True, hold=True, icon="/tmp/bell.i16")),
    ]
    S = 5
    out = Image.new("RGB", (W*S, (H*S + 8) * len(scenes)), (18,18,18))
    for k, (_, px) in enumerate(scenes):
        im = Image.frombytes("RGB", (W,H), bytes(v for row in px for p in row for v in p))
        out.paste(im.resize((W*S, H*S), Image.NEAREST), (0, k * (H*S + 8)))
    out.save(sys.argv[1] if len(sys.argv) > 1 else "cards.png")
    print("сцены:", ", ".join(n for n, _ in scenes))

main()

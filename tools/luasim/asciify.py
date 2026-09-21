"""An image into asciicker-style cells, for a panel whose font is plain ASCII.

asciicker's idea, from render.cpp: a cell is not a character on a background -
it is **two colours and a glyph that says where the boundary between them
runs**. AverageGlyph() picks the glyph from a quadrant coverage mask, and the
two colours carry the levels. That is why its pictures hold far more than a
brightness ramp does.

It can reach for CP437's half-blocks and quadrants. PicopixelFB has none of
those - it is 0x20..0x7E, and px.text() folds lowercase onto uppercase, so the
usable alphabet is about 67 shapes. Two things follow, and both are in the
firmware's favour:

- the background colour is free: px.rect() fills the cell before px.text()
  draws into it, which is exactly asciicker's fg/bk pair;
- the cell is only 3x5 = 15 pixels, small enough to match the glyph's **whole**
  bitmap against the wanted ink rather than four quadrant averages. So this
  does what AverageGlyph does, only exactly.

Per cell: split its 15 pixels into two colour groups, take the mask of which
pixels belong to the lighter one, and find the glyph whose own ink best matches
that mask - trying the inverse too, since painting the light colour as the
background and drawing the dark one is often the better fit.
"""
import argparse
import json
import re
from PIL import Image, ImageEnhance, ImageFilter

CELL_W, CELL_H = 3, 5
INC = "/Users/apple/AnimatedPixelClock-netbroker/tools/luasim/font_picopixel.inc"


def glyph_bitmaps():
    """Every glyph's ink inside one cell, positioned the way px.text puts it.

    Glyphs wider than the cell are dropped rather than clipped: at a 3-pixel
    step they would spill onto the neighbour, and then the picture would not be
    the picture that was measured."""
    s = open(INC).read()
    bm = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", s.split("kPicoGlyphs")[0])]
    rows = re.findall(
        r"\{\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
        s.split("kPicoGlyphs")[1])
    out = {}
    for i, (off, w, h, adv, xo, yo) in enumerate(rows):
        ch = chr(0x20 + i)
        off, w, h, xo, yo = int(off), int(w), int(h), int(xo), int(yo)
        if "a" <= ch <= "z":        continue    # px.text upper-cases: no new shape
        if ch in '"\\':             continue    # awkward inside a Lua string
        if xo < 0 or xo + w > CELL_W:  continue # would spill onto the next cell
        mask = 0
        bit = 0
        for gy in range(h):
            for gx in range(w):
                on = (bm[off + (bit >> 3)] >> (7 - (bit & 7))) & 1
                bit += 1
                if on:
                    cx, cy = gx + xo, gy + yo + 4
                    if 0 <= cx < CELL_W and 0 <= cy < CELL_H:
                        mask |= 1 << (cy * CELL_W + cx)
        out[ch] = mask
    return out


GLYPHS = glyph_bitmaps()
FULL = (1 << (CELL_W * CELL_H)) - 1
SPREAD = 0.55      # how far the two colours part on a flat cell

# The density ramp, used only where there is no edge to describe: every glyph
# ordered by how much of the cell its ink fills. This is the one place a
# brightness ramp is the right tool rather than the lazy one.
_by_cov = sorted(((bin(m).count("1") / (CELL_W * CELL_H), ch) for ch, m in GLYPHS.items()))


def ramp_for(l):
    """Coverage and glyph for a cell of this luminance. Brighter cells get more
    ink, because the ink is the light colour."""
    want = max(0.0, min(1.0, l / 255.0))
    best = min(_by_cov, key=lambda kc: abs(kc[0] - want))
    return best


def best_glyph(want):
    """The glyph, and whether the colours must be swapped, that puts ink closest
    to `want`. Ties go to the earlier entry, which keeps flat areas on ' '."""
    best, bch, bswap = 99, " ", False
    for ch, m in GLYPHS.items():
        d = bin(m ^ want).count("1")
        if d < best:
            best, bch, bswap = d, ch, False
        d = bin(m ^ (FULL ^ want)).count("1")
        if d < best:
            best, bch, bswap = d, ch, True
    return bch, bswap, best


def lum(p):
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]


def cell_to_ansi(px, flat=10):
    """15 pixels -> (bg, fg, glyph). The two colours are the means of the two
    groups a luminance split makes; the mask is which pixels are the lighter."""
    ls = [lum(p) for p in px]
    lo, hi = min(ls), max(ls)
    if hi - lo < flat:
        # A flat cell has no edge for a glyph to describe - but leaving it blank
        # is what turns the whole thing back into a colour mosaic. asciicker
        # never has an empty cell either: a flat patch still gets a character,
        # and it is the character's density that carries the level.
        #
        # So: pick the glyph whose ink coverage matches how bright this cell is,
        # then split the two colours around the cell's own mean rather than away
        # from it - fg = C(1 + s(1-k)), bg = C(1 - s k), whose average over the
        # cell is exactly C. The texture appears and the picture does not shift.
        m = tuple(sum(c[i] for c in px) // len(px) for i in range(3))
        k, ch = ramp_for(sum(ls) / len(ls))
        if ch == " ":
            return m, m, " "
        fg = tuple(min(255, int(v * (1 + SPREAD * (1 - k)))) for v in m)
        bg = tuple(max(0, int(v * (1 - SPREAD * k))) for v in m)
        return bg, fg, ch
    mid = (lo + hi) / 2
    # One refinement pass, which is k-means with k=2 stopped early: enough on 15
    # samples, and it keeps the load budget uninteresting.
    for _ in range(3):
        a = [p for p, l in zip(px, ls) if l < mid]
        b = [p for p, l in zip(px, ls) if l >= mid]
        if not a or not b:
            break
        mid = (sum(lum(p) for p in a) / len(a) + sum(lum(p) for p in b) / len(b)) / 2
    dark = [p for p, l in zip(px, ls) if l < mid] or px
    light = [p for p, l in zip(px, ls) if l >= mid] or px
    cdark = tuple(sum(c[i] for c in dark) // len(dark) for i in range(3))
    clight = tuple(sum(c[i] for c in light) // len(light) for i in range(3))

    want = 0
    for i, l in enumerate(ls):
        if l >= mid:
            want |= 1 << i
    ch, swap, _ = best_glyph(want)
    # swap: the glyph matched the inverse, so the light colour becomes the
    # background and the dark one is what gets drawn.
    return (clight, cdark, ch) if swap else (cdark, clight, ch)


def convert(img, cols, rows, sharpen=0.0, flat=10):
    img = img.convert("RGB").resize((cols * CELL_W, rows * CELL_H), Image.LANCZOS)
    # asciicker gets its character look for free: its scenes are 3D geometry, so
    # every cell has a real edge running through it and the glyph has something
    # to say. A downsampled painting has none - each cell comes out flat, every
    # glyph resolves to a space, and the result is a colour mosaic wearing the
    # word ASCII. Putting the within-cell contrast back is not decoration; it is
    # what gives the glyph its job.
    if sharpen > 0:
        img = img.filter(ImageFilter.UnsharpMask(radius=1, percent=int(sharpen * 100), threshold=0))
    p = img.load()
    out = []
    for cy in range(rows):
        line = []
        for cx in range(cols):
            px = [p[cx * CELL_W + x, cy * CELL_H + y]
                  for y in range(CELL_H) for x in range(CELL_W)]
            line.append(cell_to_ansi(px, flat))
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--cols", type=int, default=20)
    ap.add_argument("--rows", type=int, default=12)
    ap.add_argument("--crop", default="", help="l,t,r,b as fractions of the image")
    ap.add_argument("--contrast", type=float, default=1.0)
    ap.add_argument("--saturation", type=float, default=1.0)
    ap.add_argument("--gamma", type=float, default=1.0)
    ap.add_argument("--sharpen", type=float, default=0.0, help="unsharp at the cell scale")
    ap.add_argument("--flat", type=int, default=10, help="below this luminance spread a cell is one colour")
    ap.add_argument("--spread", type=float, default=0.55)
    ap.add_argument("--out", default="art.json")
    ap.add_argument("--preview", default="")
    args = ap.parse_args()

    im = Image.open(args.image).convert("RGB")
    if args.crop:
        l, t, r, b = (float(v) for v in args.crop.split(","))
        W, H = im.size
        im = im.crop((int(l * W), int(t * H), int(r * W), int(b * H)))
    # The panel is 8 bits a channel behind a diffuser in a lit room, and the
    # painting is four centuries of varnish. Lift it before quantising, not
    # after: the two-colour split is what has to see the contrast.
    if args.gamma != 1.0:
        g = args.gamma
        im = im.point(lambda v: int(255 * ((v / 255) ** (1 / g))))
    if args.contrast != 1.0:
        im = ImageEnhance.Contrast(im).enhance(args.contrast)
    if args.saturation != 1.0:
        im = ImageEnhance.Color(im).enhance(args.saturation)

    global SPREAD
    SPREAD = args.spread
    art = convert(im, args.cols, args.rows, args.sharpen, args.flat)
    json.dump({"cols": args.cols, "rows": args.rows,
               "cells": [[[list(bg), list(fg), ch] for bg, fg, ch in row] for row in art]},
              open(args.out, "w"))

    used = sorted({ch for row in art for _, _, ch in row})
    print(f"{args.cols}x{args.rows} ячеек, {len(used)} разных глифов: {''.join(used)}")

    if args.preview:
        # Draw it the way the panel will: fill the cell, then the glyph over it.
        out = Image.new("RGB", (args.cols * CELL_W, args.rows * CELL_H))
        q = out.load()
        for cy, row in enumerate(art):
            for cx, (bg, fg, ch) in enumerate(row):
                m = GLYPHS.get(ch, 0)
                for y in range(CELL_H):
                    for x in range(CELL_W):
                        on = (m >> (y * CELL_W + x)) & 1
                        q[cx * CELL_W + x, cy * CELL_H + y] = fg if on else bg
        out.resize((out.width * 8, out.height * 8), Image.NEAREST).save(args.preview)
        print(f"превью: {args.preview}")


if __name__ == "__main__":
    main()

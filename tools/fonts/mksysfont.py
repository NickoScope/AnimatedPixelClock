#!/usr/bin/env python3
"""The system font's Cyrillic, for the classic 5x7 font every screen prints with.

Writes src/fonts/classic_font.h: the classic font's printable ASCII (for the
places that draw without Adafruit GFX: Lua and the simulator), the Cyrillic
block U+0400..U+045F in the same 5-column format, the MISSING glyph, and the
Latin-1 letters and signs the classic font has among its own CP437 glyphs.

    python3 tools/fonts/mksysfont.py           # rewrite the header
    python3 tools/fonts/mksysfont.py --check   # exit 1 if the header is stale
    python3 tools/fonts/mksysfont.py --show    # every Cyrillic letter as text
    python3 tools/fonts/mksysfont.py --png F   # a review sheet, Latin beside Cyrillic

Sources, both checked in:
- the classic font is Adafruit GFX's glcdfont.c (BSD licence, in the
  PlatformIO library folder after one build);
- the Cyrillic comes from X11 misc-fixed 6x10 (tools/fonts/x11/6x10-cyrillic.bdf,
  trimmed from the Xorg repository; public domain). 6x10, not 5x7: its capitals
  are 5 wide and 7 tall and its lowercase 5 tall, which is exactly the classic
  font's Latin. X11 5x7 draws capitals 4 x 6 and would stand smaller beside it.

The cell is the classic font's: 5 columns of 8 rows, bit 0 the top row, drawn
6 wide with a blank column. 6x10's rows move up one, so its capitals land on
rows 0..6 like the Latin ones; a mark in its top row (Ё's dots) stays on row 0. One descender row survives, as in the Latin g, p, y; the rows
lost are listed by --show.

Letters that look the same as Latin ones are taken from the Latin font itself,
so А and A are the same pixels and cannot drift apart.
"""
import glob, pathlib, re, sys, codecs

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "src/fonts/classic_font.h"
BDF = ROOT / "tools/fonts/x11/6x10-cyrillic.bdf"
FIRST, LAST = 0x0400, 0x045F

SAME_AS_LATIN = {
    0x0410: "A", 0x0412: "B", 0x0415: "E", 0x041A: "K", 0x041C: "M", 0x041D: "H",
    0x041E: "O", 0x0420: "P", 0x0421: "C", 0x0422: "T", 0x0425: "X",
    0x0405: "S", 0x0406: "I", 0x0408: "J",
    0x0430: "a", 0x0435: "e", 0x043E: "o", 0x0440: "p", 0x0441: "c", 0x0443: "y",
    0x0445: "x", 0x0455: "s", 0x0456: "i", 0x0458: "j",
}

# Drawn by hand, over 6x10. Й: 6x10's breve sits on its top row with a blank
# row under it, and moving every row up one (to_cell) closes that gap - the
# breve lands flat on the И and the pair reads as А (the owner, 2026-09-24:
# ЭЙС showed as ЭАС). Here the breve is a cup on rows 0-1 and the И stands on
# rows 2-6, as 5x7 LCD fonts draw it; the lowercase й has the same shape.
HAND = {
    0x0419: [0x7C, 0x21, 0x12, 0x09, 0x7C],   # Й
}

# What the panel draws for a code point it has no glyph for: solid, as in the
# small font, because no letter or digit is.
MISSING = [0x7F] * 5


def glcdfont():
    cand = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/glcdfont.c"))
    if not cand:
        raise SystemExit("mksysfont: build once so Adafruit GFX is downloaded")
    t = pathlib.Path(cand[0]).read_text()
    body = t.split("{", 1)[1].split("};", 1)[0]
    b = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", re.sub(r"//.*", "", body))]
    assert len(b) == 256 * 5, len(b)
    return [b[i * 5:i * 5 + 5] for i in range(256)]


def adafruit_licence():
    cand = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/license.txt"))
    if not cand:
        raise SystemExit("mksysfont: build once so Adafruit GFX is downloaded")
    return pathlib.Path(sorted(cand)[0]).read_text().strip()


def bdf_glyphs():
    s = BDF.read_text(encoding="latin-1")
    asc = int(re.search(r"FONT_ASCENT (\d+)", s).group(1))
    out = {}
    for m in re.finditer(r"ENCODING (\d+)\n.*?BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)\nBITMAP\n(.*?)ENDCHAR", s, re.S):
        cp = int(m.group(1))
        w, h, xo, yo = map(int, m.group(2, 3, 4, 5))
        grid = [[0] * 6 for _ in range(10)]
        top = asc - (yo + h)
        for r, hx in enumerate(m.group(6).split()):
            v, nb = int(hx, 16), len(hx) * 4
            for x in range(w):
                if (v >> (nb - 1 - x)) & 1:
                    grid[top + r][xo + x] = 1
        out[cp] = grid
    return out


def to_cell(grid):
    """6x10 rows -> the classic 8-row cell. Returns 5 column bytes and what was lost.

    Every row moves up one, so the capitals stand on the Latin baseline. A mark
    in 6x10's top row (Ё's dots, Й's breve) has a blank row under it there; it
    lands on row 0 with the letter, as the classic font's own Ä does."""
    lost = []
    cols = [0] * 5
    for r in range(10):
        for x in range(6):
            if not grid[r][x]:
                continue
            y = max(r - 1, 0)
            if 0 <= y < 8 and x < 5:
                cols[x] |= 1 << y
            else:
                lost.append((r, x))
    return cols, lost


def build():
    lat = glcdfont()
    src = bdf_glyphs()
    cyr, lost, origin = {}, {}, {}
    for cp in range(FIRST, LAST + 1):
        if cp in SAME_AS_LATIN:
            cyr[cp] = lat[ord(SAME_AS_LATIN[cp])]
            origin[cp] = "latin"
        elif cp in HAND:
            cyr[cp] = HAND[cp]
            origin[cp] = "hand"
        elif cp in src:
            cyr[cp], lost[cp] = to_cell(src[cp])
            origin[cp] = "x11"
        else:
            cyr[cp] = MISSING
            origin[cp] = "missing"
    # Latin-1 to the classic font's CP437 glyphs, where it has them.
    lat1 = {}
    for b in range(0x80, 0x100):
        u = ord(bytes([b]).decode("cp437"))
        if 0xA0 <= u <= 0xFF and u not in lat1:
            lat1[u] = b
    return lat, cyr, lost, origin, lat1


def header():
    lat, cyr, lost, origin, lat1 = build()
    L = []
    L.append("#pragma once")
    L.append("// ---- GENERATED by tools/fonts/mksysfont.py - do not edit by hand ----")
    L.append("// The classic 5x7 font as data: 5 column bytes a glyph, bit 0 the top row, drawn")
    L.append("// 6 wide. Printable ASCII is Adafruit GFX's glcdfont.c (BSD licence), for the")
    L.append("// places that draw without Adafruit GFX (Lua, the simulator); the display itself")
    L.append("// draws ASCII through Adafruit's drawChar, the same pixels. The Cyrillic block")
    L.append("// U+0400..U+045F is X11 misc-fixed 6x10 (public domain, tools/fonts/x11/),")
    L.append("// with the letters that look Latin taken from the Latin glyphs themselves.")
    L.append("//")
    L.append("// The glcdfont.c data carries Adafruit's licence, which its terms ask to keep")
    L.append("// with any copy (Adafruit GFX Library, license.txt, reproduced verbatim):")
    L.append("//")
    for line in adafruit_licence().splitlines():
        L.append(("// " + line).rstrip())
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("static const uint16_t kClassicCyrFirst = 0x%04X, kClassicCyrLast = 0x%04X;" % (FIRST, LAST))
    L.append("")
    L.append("static const uint8_t kClassicAscii[95][5] = {")
    for c in range(0x20, 0x7F):
        ch = chr(c)
        L.append("    {%s},  // %s" % (", ".join("0x%02X" % v for v in lat[c]), "'\\\\'" if ch == "\\" else repr(ch)))
    L.append("};")
    L.append("")
    L.append("static const uint8_t kClassicCyr[%d][5] = {" % (LAST - FIRST + 1))
    for cp in range(FIRST, LAST + 1):
        L.append("    {%s},  // U+%04X %s %s" % (", ".join("0x%02X" % v for v in cyr[cp]), cp, chr(cp), origin[cp]))
    L.append("};")
    L.append("")
    L.append("static const uint8_t kClassicMissing[5] = {%s};" % ", ".join("0x%02X" % v for v in MISSING))
    L.append("")
    L.append("// Latin-1 (U+00A0..U+00FF): the classic font's own CP437 glyph where it has one")
    L.append("// (the degree sign, plus-minus, the accented letters), MISSING where it has none.")
    L.append("static const uint8_t kClassicLatin1[96][5] = {")
    for u in range(0xA0, 0x100):
        g = lat[lat1[u]] if u in lat1 else MISSING
        L.append("    {%s},  // U+%04X %s%s" % (", ".join("0x%02X" % v for v in g), u, chr(u) if u > 0xA0 else "nbsp",
                                                (" cp437 0x%02X" % lat1[u]) if u in lat1 else " missing"))
    L.append("};")
    L.append("// ---- END GENERATED ----")
    return "\n".join(L) + "\n"


def show():
    lat, cyr, lost, origin, _ = build()
    for cp in range(FIRST, LAST + 1):
        cols = cyr[cp]
        print("U+%04X %s  %s%s" % (cp, chr(cp), origin[cp], ("  lost rows/cols " + str(lost[cp])) if lost.get(cp) else ""))
        for y in range(8):
            print("   " + "".join("#" if (cols[x] >> y) & 1 else "." for x in range(5)))


def png(path):
    from PIL import Image, ImageDraw
    lat, cyr, _, origin, _ = build()
    lines = ["ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz",
             "".join(chr(c) for c in range(0x410, 0x430)) + "Ё",
             "".join(chr(c) for c in range(0x430, 0x450)) + "ё",
             "Съешь же ещё этих мягких булок"]
    S, W = 6, 6 * 34
    img = Image.new("RGB", (W * S, (len(lines) * 10 + 2) * S), (8, 10, 8))
    d = ImageDraw.Draw(img)
    for li, text in enumerate(lines):
        for i, ch in enumerate(text):
            c = ord(ch)
            cols = lat[c] if c < 0x80 else cyr.get(c, MISSING)
            col = (255, 190, 90) if c >= 0x400 and origin.get(c) == "x11" else (230, 230, 230)
            for x in range(5):
                for y in range(8):
                    if (cols[x] >> y) & 1:
                        X, Y = (i * 6 + x + 1) * S, (li * 10 + y + 1) * S
                        d.rectangle([X, Y, X + S - 2, Y + S - 2], fill=col)
    img.save(path)
    print(path)


if __name__ == "__main__":
    if "--show" in sys.argv:
        show()
    elif "--png" in sys.argv:
        png(sys.argv[sys.argv.index("--png") + 1])
    elif "--check" in sys.argv:
        ok = OUT.exists() and OUT.read_text() == header()
        print("classic_font.h " + ("in step" if ok else "STALE: run python3 tools/fonts/mksysfont.py"))
        sys.exit(0 if ok else 1)
    else:
        OUT.write_text(header())
        print("wrote", OUT.relative_to(ROOT))

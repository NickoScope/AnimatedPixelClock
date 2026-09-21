"""PicopixelFB as a BDF, so chafa can match against the panel's real glyphs.

chafa's --glyph-file takes anything FreeType reads. Giving it the actual font
matters: its symbol choice is a shape match, and matching against some other
font's idea of an 'S' would put the wrong character on the panel.

The cell here is 4 x 5 - three pixels of glyph and one of air, which is the
advance px.text uses and the step the panel will draw at.
"""
import re

INC = "/Users/apple/AnimatedPixelClock-netbroker/tools/luasim/font_picopixel.inc"
CW, CH = 4, 5

s = open(INC).read()
bm = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", s.split("kPicoGlyphs")[0])]
rows = re.findall(r"\{\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
                  s.split("kPicoGlyphs")[1])

glyphs = []
for i, (off, w, h, adv, xo, yo) in enumerate(rows):
    code = 0x20 + i
    ch = chr(code)
    off, w, h, xo, yo = int(off), int(w), int(h), int(xo), int(yo)
    if "a" <= ch <= "z":
        continue                       # emitted below, with the uppercase shape
    if xo < 0 or xo + w > CW - 1:
        continue                       # wider than the 3 px of ink a cell holds:
                                       # clipping it here would make chafa match
                                       # against a glyph the panel cannot draw
    grid = [[0] * CW for _ in range(CH)]
    bit = 0
    for gy in range(h):
        for gx in range(w):
            on = (bm[off + (bit >> 3)] >> (7 - (bit & 7))) & 1
            bit += 1
            if on:
                cx, cy = gx + xo, gy + yo + 4      # px.text lands ink at rows +2..+6
                if 0 <= cx < CW and 0 <= cy < CH:
                    grid[cy][cx] = 1
    glyphs.append((code, ch, grid))
    if "A" <= ch <= "Z":
        # px.text upper-cases before it looks a glyph up (lua_px.cpp:202), so on
        # this panel 'a' IS 'A'. Telling chafa that lets it use the whole range
        # while what it picks still draws exactly as it matched.
        glyphs.append((code + 32, ch.lower(), grid))

glyphs.sort()
out = ["STARTFONT 2.1",
       "FONT -nickoscope-picopixelfb-medium-r-normal--5-50-75-75-c-40-iso10646-1",
       "SIZE 5 75 75",
       f"FONTBOUNDINGBOX {CW} {CH} 0 0",
       "STARTPROPERTIES 3",
       "FONT_ASCENT 5", "FONT_DESCENT 0", "DEFAULT_CHAR 32",
       "ENDPROPERTIES",
       f"CHARS {len(glyphs)}"]
for code, ch, grid in glyphs:
    out += [f"STARTCHAR U+{code:04X}", f"ENCODING {code}",
            "SWIDTH 800 0", f"DWIDTH {CW} 0", f"BBX {CW} {CH} 0 0", "BITMAP"]
    for r in grid:
        v = 0
        for x in range(CW):
            v = (v << 1) | r[x]
        v <<= (8 - CW)                  # BDF pads each row to whole bytes, MSB first
        out.append(f"{v:02X}")
    out.append("ENDCHAR")
out.append("ENDFONT")
open("picopixel.bdf", "w").write("\n".join(out) + "\n")
print(f"{len(glyphs)} глифов -> picopixel.bdf")

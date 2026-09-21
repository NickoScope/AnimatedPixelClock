#!/usr/bin/env python3
"""chafa's ANSI output into a Lua table of characters and colours.

The pipeline this closes:

    python3 mkbdf.py                       PicopixelFB -> picopixel.bdf
    chafa --glyph-file picopixel.bdf --symbols ascii --size WxH \
          -f symbols --font-ratio 4/5 --fg-only -c truecolor in.png > cells.txt
    python3 chafa_to_lua.py cells.txt      -> the table an effect embeds

Why chafa rather than a ramp written here: per cell it searches the whole symbol
set for the glyph whose bitmap best matches which of the cell's pixels are
foreground, and evaluates the colours jointly with that choice. Choosing a
character by brightness and the colour afterwards - which is what every hand
-written converter in this directory's history did - loses the shape, and the
shape is most of what a character can say.

--fg-only matters: without it chafa also paints a background colour per cell,
and the result reads as a mosaic rather than as type. Here the ink is the
picture and the ground stays black.
"""
import re
import sys

SGR = re.compile(r"\x1b\[([0-9;]*)m")


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace").read().splitlines():
        if not line.strip():
            continue
        cells, fg, i = [], (255, 255, 255), 0
        while i < len(line):
            m = SGR.match(line, i)
            if m:
                p = m.group(1).split(";")
                if len(p) >= 5 and p[0] == "38" and p[1] == "2":
                    fg = tuple(int(v) for v in p[2:5])
                i = m.end()
                continue
            cells.append((line[i], fg))
            i += 1
        if cells:
            rows.append(cells)
    return rows


def main():
    rows = parse(sys.argv[1])
    h = len(rows)
    w = max(len(r) for r in rows)
    glyphs, colours = [], []
    for r in rows:
        r = r + [(" ", (0, 0, 0))] * (w - len(r))
        # " and \ would have to be escaped in a Lua string; neither carries
        # enough ink to be worth it.
        glyphs.append("".join(c if c not in '"\\' else " " for c, _ in r))
        colours.append("".join(f"{v:02X}" for _, f in r for v in f))
    print(f"local ART_W, ART_H = {w}, {h}")
    print("local ART_G = {\n  " + ",\n  ".join('"' + g + '"' for g in glyphs) + ",\n}")
    print("local ART_F = {\n  " + ",\n  ".join('"' + c + '"' for c in colours) + ",\n}")


if __name__ == "__main__":
    main()

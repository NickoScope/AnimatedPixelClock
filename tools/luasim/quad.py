"""libtcod's generate_quadrant_graphic, transcribed from the source.

src/libtcod/image_c.c:711-805, credited there to "Jeff Lait's code posted on
r.g.r.d". It is the best of the four libraries at this, and the two things it
does that the others do not are exactly the two that were wrong here before:

**Quarter blocks.** asciicker cannot draw a single covered quadrant, because
CP437 has no quarter block - it substitutes an ordered dither (render.cpp:2146),
and that dither is the speckle. libtcod reaches for U+2596..U+259D and draws the
quadrant as geometry. Eight codepoints cover all sixteen masks, because a mask
and its complement are the same shape with the colours swapped - hence the
negative entries in quadrant_to_codepoint.

**The two colours are reduced in RGB, by weighted merge.** Not a luminance
threshold, which is what was here before and why the hues drifted. Each further
quadrant is compared against both palette entries AND against the distance
between them; the closest pair is merged, weighted by how many quadrants each
entry already holds. A colour that is nearer the two existing ones than they are
to each other joins one of them; a colour further out than that displaces the
pair, which collapse together.

SadConsole, for contrast, averages the whole cell to one colour and picks a
block by its brightness (Host.MonoGame/Extensions.cs, ToSurface) - one sample a
cell against libtcod's four. BearLibTerminal has no converter at all; what it
offers is sub-cell offsets and layers, which do not apply here.

The codepoints are not needed below: quadrants are drawn as rectangles, and a
rectangle is exact where a glyph is an approximation.
"""


def dist2(a, b):
    return (a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2


def lerp(a, b, t):
    """TCOD_color_lerp: from a towards b by t."""
    return tuple(int(a[i] + (b[i]-a[i]) * t + 0.5) for i in range(3))


def quadrant_cell(desired):
    """desired = (TL, TR, BL, BR). Returns (fg_quadrant_set, bg, fg).

    Quadrant 0 is the reference and never enters the mask, exactly as in the
    original: `X 1 / 2 4`.
    """
    mask = 0
    qi = 1
    while qi < 4 and desired[qi] == desired[0]:
        qi += 1
    if qi == 4:
        return set(), desired[0], desired[0]

    pal = [desired[0], desired[qi]]
    w = [qi, 1]
    mask |= 1 << (qi - 1)
    qi += 1
    while qi < 4:
        q = desired[qi]
        if q == pal[0]:
            w[0] += 1
        elif q == pal[1]:
            mask |= 1 << (qi - 1)
            w[1] += 1
        else:
            d0q, d1q, d01 = dist2(q, pal[0]), dist2(q, pal[1]), dist2(pal[0], pal[1])
            if d0q < d1q:
                if d0q <= d01:
                    pal[0] = lerp(q, pal[0], w[0] / (1.0 + w[0]))
                    w[0] += 1
                else:
                    pal[0] = lerp(pal[0], pal[1], w[1] / (w[0] + w[1]))
                    w[0] += 1
                    pal[1] = q
                    mask = 1 << (qi - 1)
            else:
                if d1q <= d01:
                    pal[1] = lerp(q, pal[1], w[1] / (1.0 + w[1]))
                    w[1] += 1
                    mask |= 1 << (qi - 1)
                else:
                    pal[0] = lerp(pal[0], pal[1], w[1] / (w[0] + w[1]))
                    w[0] += 1
                    pal[1] = q
                    mask = 1 << (qi - 1)
        qi += 1

    # quadrant index 1..3 is in the foreground when its bit is set; 0 never is
    fg_set = {i for i in (1, 2, 3) if mask & (1 << (i - 1))}
    return fg_set, pal[0], pal[1]

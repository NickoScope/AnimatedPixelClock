#!/usr/bin/env python3
"""A photograph into a Lua effect that draws it.

Not ASCII art - the picture itself, at the panel's own 128x64.

**Aspect first.** The panel is 2:1 and almost no photograph is, so `--aspect`
decides what happens to the difference: `fit` (the default) keeps the whole
picture undistorted and fills the sides with a blurred, darkened copy of
itself; `fill` crops to 2:1 and loses the edges; `stretch` is the old
behaviour and squashed a 1007x1078 portrait 2.14x flat.

**Then colour.** `--truecolor` (the default since the script limit became
50 KB) writes the colour itself, four base64 characters a pixel, with no
palette and no dithering - about 35 KB. `--palette` is the older 256-colour
dithered encoding at about 20 KB, kept because it is half the size.

What the palette version cost, and why dropping it is worth 15 KB: 256 colours
band in a sky or a cheek, and the dither that hides the banding is itself
visible as speckle. The panel can address 174^3 colours - see the note in the
generated header for where that number comes from - so a palette is throwing
away a great deal.

The rest is what makes it fit and makes it cheap:

**In palette mode, a palette as large as will fit**: 256 colours, not the
sixteen the firmware's own animation format allows. Two base64 characters an
index, Floyd-Steinberg on the way in.

**And no run-length coding**, which was tried and thrown away. Dithering is what
keeps a face from banding at this size, and dithering is exactly what destroys
runs: 8,192 pixels came out as 7,232 runs, so the length byte was pure overhead
and the file went over the limit. Two characters a pixel, flat, is smaller than
three characters a run - 19 KB against 25 - and it is also simpler.

**Drawn once.** This firmware does not clear the canvas between frames, so the
photograph is painted on the first frame and never again. After that the effect
costs one text call a second for the clock, or nothing at all without it.

    python3 tools/luasim/photo_to_lua.py photo.jpg --name red_hat
    python3 tools/luasim/photo_to_lua.py photo.jpg --name red_hat \\
        --crop 0.05,0.32,0.95,0.74 --clock br
"""

import argparse
import pathlib
import sys

from PIL import Image, ImageEnhance

W, H = 128, 64
# base64url, so a run index and a length are one character each and the Lua
# string needs no escaping.
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"


def enc2(v):
    """An index as two base64 characters: 0..4095, of which 256 are used."""
    return ALPHA[v >> 6] + ALPHA[v & 63]


def encode_rows(idx, cols):
    """Each row as two base64 characters a pixel."""
    rows = []
    for y in range(H):
        rows.append("".join(enc2(idx[y * W + x]) for x in range(W)))
    return rows


def encode_rows_true(rgb):
    """Each row as FOUR base64 characters a pixel: 24 bits, the colour itself.

    No palette and no dithering, because with a 50 KB budget neither is needed:
    8,192 pixels at four characters is 32,768, and the whole file lands near
    35 KB. What that buys is not a longer palette - it is no palette, so no
    quantisation error to dither away and none of the dither noise that
    quantisation then needs.
    """
    rows = []
    for y in range(H):
        out = []
        for x in range(W):
            r, g, b = rgb[y * W + x]
            v = (r << 16) | (g << 8) | b
            out.append(ALPHA[(v >> 18) & 63] + ALPHA[(v >> 12) & 63]
                       + ALPHA[(v >> 6) & 63] + ALPHA[v & 63])
        rows.append("".join(out))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--name", required=True)
    ap.add_argument("--crop", default="", help="l,t,r,b as fractions")
    ap.add_argument("--colors", type=int, default=256,
                    help="2..256, palette mode only. Ignored with --truecolor")
    ap.add_argument("--truecolor", action="store_true", default=None,
                    help="24-bit colour, four characters a pixel, no palette and "
                         "no dithering. The default now that a script may be 50 KB")
    ap.add_argument("--palette", dest="truecolor", action="store_false",
                    help="the old 256-colour dithered encoding, about 20 KB")
    ap.add_argument("--contrast", type=float, default=1.0)
    ap.add_argument("--saturation", type=float, default=1.0)
    ap.add_argument("--brightness", type=float, default=1.0)
    ap.add_argument("--sharpen", type=float, default=0.0,
                    help="unsharp after the downscale. A face at 128x64 has lost "
                         "every edge it had; a little back is worth a lot")
    ap.add_argument("--aspect", default="fit", choices=["fit", "fill", "stretch"],
                    help="fit (default): the whole picture, undistorted, with the "
                         "sides filled by a blurred copy of itself. fill: crop to "
                         "the panel's 2:1 and lose the edges. stretch: the old "
                         "behaviour, which squashed a portrait flat")
    ap.add_argument("--clock", default="br", choices=["br", "bl", "tr", "tl", "none"],
                    help="where the time goes, or none")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    if not 2 <= args.colors <= 256:
        sys.exit("--colors must be 2..256")
    if args.truecolor is None:
        args.truecolor = True

    im = Image.open(args.image).convert("RGB")
    if args.crop:
        l, t, r, b = (float(v) for v in args.crop.split(","))
        iw, ih = im.size
        im = im.crop((int(l * iw), int(t * ih), int(r * iw), int(b * ih)))
    if args.brightness != 1.0:
        im = ImageEnhance.Brightness(im).enhance(args.brightness)
    if args.contrast != 1.0:
        im = ImageEnhance.Contrast(im).enhance(args.contrast)
    if args.saturation != 1.0:
        im = ImageEnhance.Color(im).enhance(args.saturation)

    # The panel is 2:1 and almost no photograph is. Resizing straight to 128x64
    # squashes a portrait flat - a 1007x1078 picture came out compressed 2.14x
    # vertically, which is what a wide face on the panel actually was.
    if args.aspect == "stretch":
        im = im.resize((W, H), Image.LANCZOS)
    elif args.aspect == "fill":
        iw, ih = im.size
        k = max(W / iw, H / ih)
        im = im.resize((max(W, round(iw * k)), max(H, round(ih * k))), Image.LANCZOS)
        iw, ih = im.size
        l, t = (iw - W) // 2, (ih - H) // 2
        im = im.crop((l, t, l + W, t + H))
    else:
        iw, ih = im.size
        k = min(W / iw, H / ih)
        fw, fh = max(1, round(iw * k)), max(1, round(ih * k))
        small = im.resize((fw, fh), Image.LANCZOS)
        # The bars are the photograph itself, blown up, blurred and darkened -
        # black bars on a lit panel read as a fault, and this reads as depth.
        from PIL import ImageFilter  # noqa: PLC0415
        back = im.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(3))
        back = ImageEnhance.Brightness(back).enhance(0.45)
        back.paste(small, ((W - fw) // 2, (H - fh) // 2))
        im = back
    if args.sharpen > 0:
        from PIL import ImageFilter
        im = im.filter(ImageFilter.UnsharpMask(radius=1,
                                               percent=int(args.sharpen * 100),
                                               threshold=0))
    if args.truecolor:
        rows = encode_rows_true(list(im.getdata()))
        palette = ""
    else:
        # Dithered: at 64 colours a photograph's skin tones band badly without
        # it, and on a 2 mm pitch the dither is invisible from across a room.
        q = im.quantize(colors=args.colors, method=Image.MEDIANCUT,
                        dither=Image.FLOYDSTEINBERG)
        pal = q.getpalette()[: args.colors * 3]
        rows = encode_rows(list(q.getdata()), args.colors)
        palette = "".join(f"{v:02X}" for v in pal)

    if args.truecolor:
        head = [
            "-- Generated by tools/luasim/photo_to_lua.py in 24-bit colour: four",
            "-- base64 characters a pixel, no palette and no dithering. With a 50 KB",
            "-- budget neither is needed, and dropping them drops the two things that",
            "-- gave the 256-colour version away - the banding quantisation leaves in",
            "-- a sky or a cheek, and the speckle the dither then adds to hide it.",
            "--",
            "-- It is also CHEAPER per pixel, which is the part that is not obvious.",
            "-- The old encoding cost three calls into C a pixel: two string.sub to",
            "-- read the index and one px.pixel. This costs two - a single",
            "-- string.byte returns all four characters at once - so the fuller",
            "-- picture paints faster than the 256-colour one did.",
            "--",
            "-- How much fuller, measured rather than assumed: the canvas blits",
            "-- through drawPixelRGB888 with no 565 step (lua_px.h), and the HUB75",
            "-- driver runs 8 bits a channel. But it then puts every channel through",
            "-- a CIE 1931 gamma table, and that table maps 256 inputs onto 174",
            "-- distinct outputs - 82 of them collapse onto a neighbour, nearly all",
            "-- at the dark end, where inputs 0..4 are all black. So the panel can",
            "-- address 174^3 = 5,268,024 colours, not 16.7 million, and the gain",
            "-- over a palette is smallest in the shadows.",
            "--",
            "-- Painted on the first frame and never again, because this firmware",
            "-- does not clear the canvas between frames. After that the effect costs",
            "-- one text call a second, or nothing at all if the clock is off.",
        ]
    else:
        head = [
            "-- Generated by tools/luasim/photo_to_lua.py. The picture is quantised to",
            f"-- {args.colors} colours with Floyd-Steinberg and written two base64",
            "-- characters a pixel. It is painted on the first frame and never again,",
            "-- because this firmware does not clear the canvas between frames - so",
            "-- after that the effect costs one text call a second, or nothing at all",
            "-- if the clock is off.",
        ]
    body = [
        "-- @upload-only",
        f"-- {args.name.upper()} - a photograph, drawn once.",
        "--",
        *head,
        "",
        "PERIOD = 60.0",
        "FPS = 2",
        "",
        f'local PAL = "{palette}"',
        "local ROWS = {",
    ]
    for r in rows:
        body.append(f'  "{r}",')
    if args.truecolor:
        body += [
            "}",
            "",
            "-- Character code -> its six bits. Indexed by BYTE, so the decode below",
            "-- needs no string work at all beyond the one read.",
            'local A = "' + ALPHA + '"',
            "local V = {}",
            "for i = 1, #A do V[A:byte(i)] = i - 1 end",
            "",
            "local byte, pixel = string.byte, px.pixel",
            "local painted = false",
            "",
            "local function paint()",
            "  for y = 1, #ROWS do",
            "    local row = ROWS[y]",
            "    local yy = y - 1",
            "    local i = 1",
            "    for x = 0, 127 do",
            "      -- One call, four characters, 24 bits of colour.",
            "      local a, b, c, d = byte(row, i, i + 3)",
            "      pixel(x, yy, V[a] * 4 + (V[b] >> 4),",
            "            (V[b] & 15) * 16 + (V[c] >> 2),",
            "            (V[c] & 3) * 64 + V[d])",
            "      i = i + 4",
            "    end",
            "  end",
            "end",
            "",
        ]
    else:
        body += [
            "}",
            "",
            "-- The palette, unpacked once at load rather than parsed per pixel.",
            "local R, G, B = {}, {}, {}",
            f"for i = 0, {args.colors - 1} do",
            "  local h = i * 6",
            "  R[i] = tonumber(PAL:sub(h + 1, h + 2), 16)",
            "  G[i] = tonumber(PAL:sub(h + 3, h + 4), 16)",
            "  B[i] = tonumber(PAL:sub(h + 5, h + 6), 16)",
            "end",
            "",
            'local A = "' + ALPHA + '"',
            "local VAL = {}",
            "for i = 1, #A do VAL[A:sub(i, i)] = i - 1 end",
            "",
            "local painted = false",
            "",
            "local function paint()",
            "  for y = 1, #ROWS do",
            "    local row = ROWS[y]",
            "    local yy = y - 1",
            "    for x = 0, 127 do",
            "      local i = x * 2 + 1",
            "      local c = VAL[row:sub(i, i)] * 64 + VAL[row:sub(i + 1, i + 1)]",
            "      px.pixel(x, yy, R[c], G[c], B[c])",
            "    end",
            "  end",
            "end",
            "",
        ]
    body += [
        "function draw()",
        "  if not painted then",
        "    paint()",
        "    painted = true",
        "  end",
    ]
    if args.clock != "none":
        pos = {"br": ("W - w - 2", "H - 8"), "bl": ("2", "H - 8"),
               "tr": ("W - w - 2", "1"), "tl": ("2", "1")}[args.clock]
        body += [
            "",
            "  -- The time, over the photograph rather than beside it. A filled box",
            "  -- behind it was tried and looks like a sticker; four offset copies in",
            "  -- black are a proper outline and let the picture through.",
            "  local t = px.now()",
            '  local s = string.format("%02d:%02d", t.hour, t.min)',
            "  local W, H = px.size()",
            "  local w = px.width(s)",
            f"  local tx, ty = {pos[0]}, {pos[1]}",
            "  px.text(tx - 1, ty, s, 0, 0, 0)",
            "  px.text(tx + 1, ty, s, 0, 0, 0)",
            "  px.text(tx, ty - 1, s, 0, 0, 0)",
            "  px.text(tx, ty + 1, s, 0, 0, 0)",
            "  px.text(tx, ty, s, 245, 245, 250)",
        ]
    body += ["end", ""]

    out = pathlib.Path(args.out) if args.out else \
        pathlib.Path(__file__).resolve().parent / "scripts" / f"{args.name}.lua"
    out.write_text("\n".join(body))
    # The limit comes from the firmware header through validate.py, never from a
    # number typed here. This line said "% of 24576" until the cap was raised to
    # 50 KB and then quietly reported nonsense - a tool that measures against a
    # remembered constant measures nothing.
    cap = 0
    try:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import validate as V  # noqa: PLC0415
        v = V.check(out)
        cap = (v or {}).get("maxBytes", 0)
    except Exception:  # noqa: BLE001
        pass
    size = out.stat().st_size
    pct = f", {100 * size // cap}% of the panel's {cap // 1024} KB limit" if cap else ""
    how = "24-bit, no palette" if args.truecolor else f"{args.colors} colours, dithered"
    print(f"{out}  {size} B  ({how}, {args.aspect}, 8192 pixels{pct})")


if __name__ == "__main__":
    main()

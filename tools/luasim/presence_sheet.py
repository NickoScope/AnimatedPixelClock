#!/usr/bin/env python3
"""Compose a contact sheet from luasim raw frames: one row per variant, one
column per timestamp, the same timestamps across every row.

    python3 tools/luasim/presence_sheet.py --fps 10 --at 2 --at 7 --at 45 \\
        --row "6 m=gifs/presence/room_radar_real_6m.raw" \\
        --row "4 m=gifs/presence/room_radar_real_4m.raw" \\
        --out gifs/presence/room_radar_real_contact.png

Preview tooling: the frames it reads are rendered from a recorded session and
live outside the repository, and so does what it writes.
"""
import argparse, pathlib
from PIL import Image, ImageDraw

W, H = 128, 64
FRAME = W * H * 3
PAD, LABEL_W, CAP_H = 6, 78, 14
INK, SUB, BG = (235, 235, 235), (150, 150, 150), (16, 16, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--row", action="append", required=True, help='"label=path.raw"')
    ap.add_argument("--at", action="append", type=float, required=True, help="seconds into the window")
    ap.add_argument("--fps", type=float, default=10.0)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--title", default=None)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    rows = []
    for spec in a.row:
        label, _, path = spec.partition("=")
        rows.append((label, pathlib.Path(path).read_bytes()))

    cw, ch = W * a.scale, H * a.scale
    top = CAP_H + (18 if a.title else 0)
    sheet = Image.new("RGB", (LABEL_W + len(a.at) * (cw + PAD) + PAD,
                              top + len(rows) * (ch + PAD) + PAD), BG)
    d = ImageDraw.Draw(sheet)
    if a.title:
        d.text((PAD, 4), a.title, fill=INK)
    for c, t in enumerate(a.at):
        d.text((LABEL_W + c * (cw + PAD), top - 12), f"t+{t:g}s", fill=SUB)
    for r, (label, raw) in enumerate(rows):
        y = top + r * (ch + PAD)
        d.text((PAD, y + ch // 2 - 4), label, fill=INK)
        n = len(raw) // FRAME
        for c, t in enumerate(a.at):
            i = min(n - 1, max(0, int(round(t * a.fps))))
            img = Image.frombytes("RGB", (W, H), raw[i * FRAME:(i + 1) * FRAME])
            sheet.paste(img.resize((cw, ch), Image.NEAREST), (LABEL_W + c * (cw + PAD), y))
    sheet.save(a.out)
    print(f"{a.out}: {len(rows)} rows x {len(a.at)} timestamps")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Render the fx3d scenes on the Mac with the firmware's own headers.

  python3 tools/fx3d/render.py                       # every scene, mono and red-blue
  python3 tools/fx3d/render.py --only cube,stars --modes 1 --seconds 4

Into tools/fx3d/out/ (not committed):
  <scene>_<mode>.gif    128x64 at 6x with the LED look, 30 fps
  <scene>_eyes.png      stereo only: left | right | what the panel shows
  sheet_<mode>.png      one still per scene
  timings.json          microseconds per frame on this Mac, NOT on the panel

What the previews show is what the panel emits: each code goes through the
HUB75 library's own CIE 1931 table (kCie8 in fx3d_model.h) to linear light,
then to sRGB for this screen. The anaglyph previews can be looked at through
the owner's glasses on the Mac, but the Mac's primaries are not the panel's
LEDs, so the crosstalk seen there says nothing about the panel's.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / "out"
sys.path.insert(0, str(ROOT / "tools/audiofx"))
from gfx import led_image  # noqa: E402

W, H = 128, 64
MODES = {0: "mono", 1: "redblue", 2: "redcyan", 3: "redgreen"}
FPS = 30


def cie_table():
    src = (ROOT / "src/fx3d/fx3d_model.h").read_text()
    body = re.search(r"kCie8\[256\] = \{(.*?)\};", src, re.S).group(1)
    t = np.array([int(v) for v in re.findall(r"\d+", body)], dtype=np.float64)
    assert len(t) == 256
    return t / 255.0


def srgb(linear):
    a = np.clip(linear, 0.0, 1.0)
    return np.where(a <= 0.0031308, 12.92 * a, 1.055 * np.power(a, 1 / 2.4) - 0.055)


def build():
    OUT.mkdir(exist_ok=True)
    exe = OUT / "fx3d_host"
    subprocess.run(["c++", "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wdouble-promotion",
                    "-I", str(ROOT / "src/fx3d"), str(HERE / "fx3d_host.cpp"), "-o", str(exe)], check=True)
    return exe


def scene_ids():
    src = (ROOT / "src/fx3d/fx3d_catalog.h").read_text()
    return re.findall(r'\{"([a-z0-9_]+)", "', src)


def run(exe, scene, mode, frames, seed, page=-1):
    path = OUT / f"{scene}_{MODES[mode]}.bin"
    r = subprocess.run([str(exe), scene, str(mode), str(frames), str(FPS), str(seed), str(path), str(page)],
                       capture_output=True, text=True, check=True)
    stats = json.loads(r.stdout)
    raw = path.read_bytes()
    assert raw[:4] == b"FX3D"
    n = int.from_bytes(raw[8:12], "little")
    eyes = raw[13]
    per = W * H * 3 + (2 * W * H if eyes else 0)
    frames_out = []
    for k in range(n):
        base = 16 + k * per
        codes = np.frombuffer(raw, np.uint8, W * H * 3, base).reshape(H, W, 3)
        lr = None
        if eyes:
            left = np.frombuffer(raw, np.uint8, W * H, base + W * H * 3).reshape(H, W)
            right = np.frombuffer(raw, np.uint8, W * H, base + W * H * 4).reshape(H, W)
            lr = (left, right)
        frames_out.append((codes, lr))
    path.unlink()
    return frames_out, stats


def emitted(codes, cie):
    """What the panel emits, as sRGB bytes for this screen."""
    return (srgb(cie[codes]) * 255.0 + 0.5).astype(np.uint8)


def gif(frames, cie, path):
    imgs = [led_image(emitted(c, cie), scale=6, gap=1).quantize(colors=256, dither=Image.Dither.NONE)
            for c, _ in frames]
    imgs[0].save(path, save_all=True, append_images=imgs[1:], duration=int(1000 / FPS), loop=0, optimize=False)


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 8 * len(text) + 8, 16], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(200, 200, 200))
    return img


def eyes_png(frame, cie, path):
    codes, (left, right) = frame
    def grey(p):
        g = (srgb(p / 255.0) * 255.0 + 0.5).astype(np.uint8)
        return np.stack([g, g, g], axis=-1)
    tiles = [label(led_image(grey(left), 4, 1), "left eye"), label(led_image(grey(right), 4, 1), "right eye"),
             label(led_image(emitted(codes, cie), 4, 1), "on the panel")]
    w, h = tiles[0].size
    sheet = Image.new("RGB", (3 * w + 16, h), (24, 24, 24))
    for i, t in enumerate(tiles):
        sheet.paste(t, (i * (w + 8), 0))
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--modes", default="0,1")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--seed", type=int, default=20260918)
    ap.add_argument("--still", type=float, default=2.0, help="time of the contact sheet's still")
    args = ap.parse_args()
    exe = build()
    cie = cie_table()
    ids = scene_ids()
    if args.only:
        ids = [s for s in ids if s in args.only.split(",")]
    modes = [int(m) for m in args.modes.split(",")]
    frames_n = int(args.seconds * FPS)
    still = min(frames_n - 1, int(args.still * FPS))
    timings = []
    sheets = {m: [] for m in modes}
    for scene in ids:
        for m in modes:
            frames, stats = run(exe, scene, m, frames_n, args.seed)
            timings.append(stats)
            gif(frames, cie, OUT / f"{scene}_{MODES[m]}.gif")
            sheets[m].append((scene, emitted(frames[still][0], cie)))
            if frames[still][1] is not None and m == modes[-1]:
                eyes_png(frames[still], cie, OUT / f"{scene}_eyes.png")
            print(f"{scene:8s} {MODES[m]:8s} host {stats['host_us_avg']:8.1f} us avg {stats['host_us_max']:8.1f} max")
    for m, items in sheets.items():
        if not items:
            continue
        tiles = [label(led_image(rgb, 4, 1), name) for name, rgb in items]
        w, h = tiles[0].size
        cols = 2
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (w + 8), rows * (h + 8)), (24, 24, 24))
        for i, t in enumerate(tiles):
            sheet.paste(t, ((i % cols) * (w + 8), (i // cols) * (h + 8)))
        sheet.save(OUT / f"sheet_{MODES[m]}.png")
    (OUT / "timings.json").write_text(json.dumps(timings, indent=1))


if __name__ == "__main__":
    main()

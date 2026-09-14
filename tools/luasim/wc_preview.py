#!/usr/bin/env python3
"""Previews of the world clock page, drawn by the page's own C++ (wchost).

fx_parity.py builds tools/luasim/wchost; run it once first. Then:

  python3 tools/luasim/wc_preview.py

Writes into preview/, stills at 1:1 and 6x, GIFs at 1:1 and 4x, all at
12:34 CEST on 13 September 2026:

  world_home_cannes     home Cannes, the name settled
  world_home_long       a long custom name as home: SAINT PETERSBURG, 61 of 72 px
  world_custom_added    a custom city just added as home, at the bottom of a breath
  world_name_settles    the chosen design: the name breathes with the dot for
                        10 s after a change, eases out, and holds still
  world_name_breathes   the alternative: the name breathes with the dot for good
"""
import pathlib, subprocess, sys, tempfile
from PIL import Image

SIM = pathlib.Path(__file__).resolve().parent
OUT = SIM / "preview"
W, H = 128, 64
FPS = 10                                    # the page's own refresh (main.cpp)
sys.path.insert(0, str(SIM))
from gen_tz import load as tz_table

TZ = tz_table()
LONG  = ("SAINT PETERSBURG", 59.94, 30.31, "Europe/Moscow")
ADDED = ("SAN FRANCISCO", 37.77, -122.42, "America/Los_Angeles")


def frames(n, *flags):
    with tempfile.TemporaryDirectory() as tmp:
        raw = pathlib.Path(tmp) / "f.raw"
        subprocess.run([str(SIM / "wchost"), "-", str(n), str(raw), "--fps", str(FPS), *flags], check=True)
        data = raw.read_bytes()
    return [Image.frombytes("RGB", (W, H), data[i * W * H * 3:(i + 1) * W * H * 3]) for i in range(n)]


def custom(city):
    name, lat, lon, zone = city
    return ["--custom", f"{name}|{lat}|{lon}|{TZ[zone]}", "--home", "100"]


def still(img, name):
    img.save(OUT / f"{name}.png")
    img.resize((W * 6, H * 6), Image.NEAREST).save(OUT / f"{name}@6x.png")
    print(f"preview/{name}.png, @6x")


def gif(imgs, name):
    for scale, suffix in ((1, ""), (4, "@4x")):
        big = [i.resize((W * scale, H * scale), Image.NEAREST) for i in imgs]
        big[0].save(OUT / f"{name}{suffix}.gif", save_all=True, append_images=big[1:],
                    duration=1000 // FPS, loop=0)
    print(f"preview/{name}.gif, @4x: {len(imgs)} frames")


if not (SIM / "wchost").exists():
    sys.exit("wc_preview: run fx_parity.py first, it builds wchost")
# Half a second in: the breathing home dot at its brightest.
still(frames(FPS // 2 + 1, "--settled")[-1], "world_home_cannes")
still(frames(FPS // 2 + 1, "--settled", *custom(LONG))[-1], "world_home_long")
# 2.0 s after the change: |sin(2 pi)| = 0, the dimmest the name gets.
still(frames(2 * FPS + 1, *custom(ADDED))[-1], "world_custom_added")
gif(frames(14 * FPS, *custom(ADDED)), "world_name_settles")
gif(frames(4 * FPS, "--always", *custom(ADDED)), "world_name_breathes")

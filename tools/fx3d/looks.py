#!/usr/bin/env python3
"""Show real screens with every 3D look, on the Mac, with the firmware's code.

  python3 tools/fx3d/looks.py              # the Lua pages via luasim, and the weather screen

The screens are the panel's own pages rendered on the host: the Lua pages by
tools/luasim (the same Lua and px.* as the firmware) and the weather screen's
preview from tools/climate/preview. Each is shown flat, with the four looks
for the glasses (in red-blue) and the four without (in mono).

Into tools/fx3d/out/ (not committed): looks_<screen>_<look>_<mode>.mp4,
looks_sheet_mono.png, looks_sheet_redblue.png and looks.html.
"""
import base64
import html
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import render  # noqa: E402
from render import OUT, W, H, FPS, emitted, cie_table, led_image, label, mp4, build  # noqa: E402

LUASIM = ROOT / "tools/luasim"
SCREENS = [("snake_clock", "Часы-змейка"), ("tetris_clock", "Тетрис-часы"), ("world_clock", "Мировые часы"),
           ("minecraft", "Minecraft"), ("snooker_clock", "Снукер-часы"), ("weather", "Погода (превью)")]
GLASSES = ["pop", "layers", "float", "dome"]
BARE = ["wiggle", "card", "relief", "drum"]
TITLES = {"flat": "как есть", "pop": "выпуклость", "layers": "слои по цвету", "float": "парение",
          "dome": "купол", "wiggle": "покачивание", "card": "карточка", "relief": "рельеф", "drum": "барабан"}
SECONDS = 6.0


def screen_raw(name):
    raw = OUT / f"screen_{name}.raw"
    if name == "weather":
        img = Image.open(ROOT / "tools/climate/preview/a_line_live_128x64.png").convert("RGB")
        raw.write_bytes(np.asarray(img, dtype=np.uint8).tobytes())
        return raw
    exe = LUASIM / "luasim"
    if not exe.exists():
        subprocess.run(["make"], cwd=LUASIM, check=True, capture_output=True)
    subprocess.run([str(exe), f"scripts/{name}.lua", str(int(SECONDS * 20)), str(raw), "--start", "10:08",
                    "--yday", "260", "--utc", "2", "--year", "2026"], cwd=LUASIM, check=True, capture_output=True)
    return raw


def run_look(exe, look, mode, raw):
    path = OUT / "look.bin"
    frames_n = int(SECONDS * FPS)
    subprocess.run([str(exe), f"look:{look}", str(mode), str(frames_n), str(FPS), "1", str(path), str(raw)],
                   check=True, capture_output=True, text=True)
    data = path.read_bytes()
    eyes = data[13]
    per = W * H * 3 + (2 * W * H if eyes else 0)
    frames = [(np.frombuffer(data, np.uint8, W * H * 3, 16 + k * per).reshape(H, W, 3), None)
              for k in range(frames_n)]
    path.unlink()
    return frames


def main():
    exe = build()
    cie = cie_table()
    still = int(2.5 * FPS)
    rows = {"mono": [], "redblue": []}
    cards = []
    for name, title in SCREENS:
        raw = screen_raw(name)
        tiles = {"mono": [], "redblue": []}
        videos = []
        for look, mode in [("flat", 0)] + [(l, 1) for l in GLASSES] + [(l, 0) for l in BARE]:
            frames = run_look(exe, look, mode, raw)
            m = render.MODES[mode]
            out = OUT / f"looks_{name}_{look}_{m}.mp4"
            mp4(frames, cie, out)
            videos.append((look, m, out))
            tiles[m].append(label(led_image(emitted(frames[still][0], cie), 3, 1), f"{look}"))
            if look in ("card", "relief", "drum"):   # the geometry looks work with the glasses too
                rb = run_look(exe, look, 1, raw)
                tiles["redblue"].append(label(led_image(emitted(rb[still][0], cie), 3, 1), f"{look}"))
        for m in rows:
            w, h = tiles[m][0].size
            row = Image.new("RGB", (len(tiles[m]) * (w + 6), h), (24, 24, 24))
            for i, t in enumerate(tiles[m]):
                row.paste(t, (i * (w + 6), 0))
            rows[m].append(row)
        media = "".join(
            f'<figure><video autoplay loop muted playsinline src="data:video/mp4;base64,'
            f'{base64.b64encode(p.read_bytes()).decode()}"></video><figcaption>{html.escape(TITLES[l])} · '
            f'{"анаглиф" if m == "redblue" else "без очков"}</figcaption></figure>' for l, m, p in videos)
        cards.append(f'<section><h2>{html.escape(title)}</h2><div class="media">{media}</div></section>')
        print(f"{name}: {len(videos)} looks")
    for m, rs in rows.items():
        width = max(r.size[0] for r in rs)
        sheet = Image.new("RGB", (width, sum(r.size[1] + 6 for r in rs)), (24, 24, 24))
        y = 0
        for r in rs:
            sheet.paste(r, (0, y))
            y += r.size[1] + 6
        sheet.save(OUT / f"looks_sheet_{m}.png")
    page = f"""<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>3D-режимы: любой экран</title>
<style>
:root {{ color-scheme: dark; --bg:#0d0f12; --card:#16191e; --ink:#e6e8eb; --dim:#9aa3ad; }}
body {{ margin:0; background:var(--bg); color:var(--ink); font:15px/1.5 -apple-system, system-ui, sans-serif; }}
main {{ max-width:1500px; margin:0 auto; padding:16px; }} h1 {{ font-size:22px; margin:8px 0 4px; }}
.lead {{ color:var(--dim); margin:0 0 16px; }} section {{ background:var(--card); border-radius:10px; padding:14px 16px; margin:0 0 14px; }}
h2 {{ font-size:18px; margin:0 0 8px; }} .media {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(300px, 1fr)); gap:10px; }}
figure {{ margin:0; }} video {{ width:100%; height:auto; background:#000; border-radius:4px; display:block; }}
figcaption {{ color:var(--dim); font-size:12.5px; margin-top:3px; }}
</style></head><body><main>
<h1>3D как режим отображения любого экрана — первый взгляд</h1>
<p class="lead">Страницы нарисованы их собственным кодом на Mac (Lua-страницы — тем же Lua, что в прошивке; погода —
из превью её экрана). Страница ничего не знает про 3D: режим берёт готовый кадр. «Выпуклость», «слои», «парение» и
«купол» — для очков (анаглиф, красный — левый глаз); «покачивание», «карточка», «рельеф» и «барабан» — без очков.
Параллакс — 2 пикселя.</p>
{"".join(cards)}
</main></body></html>"""
    (OUT / "looks.html").write_text(page)
    print(f"looks.html {(OUT / 'looks.html').stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()

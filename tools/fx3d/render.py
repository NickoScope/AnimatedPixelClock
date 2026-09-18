#!/usr/bin/env python3
"""Render the fx3d scenes on the Mac with the firmware's own headers.

  python3 tools/fx3d/render.py                       # every scene, mono and red-blue
  python3 tools/fx3d/render.py --only cube,stars --modes 1 --seconds 4

Into tools/fx3d/out/ (not committed):
  <scene>_<mode>.gif    128x64 at 6x with the LED look, 30 fps
  <scene>_<mode>.mp4    the same at 30 fps, H.264 (for the showreel page)
  <scene>_eyes.png      stereo only: left | right | what the panel shows
  sheet_<mode>.png      one still per scene
  calib_pages.png       the six calibration pages, in mono and red-blue
  timings.json          microseconds per frame on this Mac, NOT on the panel
  showreel.html         one self-contained page: every scene, mono and
                        anaglyph, what it is, where it fits, what it costs

What the previews show is what the panel emits: each code goes through the
HUB75 library's own CIE 1931 table (kCie8 in fx3d_model.h) to linear light,
then to sRGB for this screen. The anaglyph previews can be looked at through
the owner's glasses on the Mac, but the Mac's primaries are not the panel's
LEDs, so the crosstalk seen there says nothing about the panel's.
"""
import argparse
import base64
import html
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


NOTES = {
    "calib": ("Калибровка очков", "служебная страница",
              "Бриф, §7. Чистые R, G, B со шкалой; каждому глазу своя фигура (чужая фигура — утечка, "
              "L не с той стороны — поменять глаза); плоскость панели и стык; квадраты впереди, на панели и позади."),
    "cube": ("Стерео-куб", "страница, заставка",
             "Бриф, MVP. Каркасный куб вращается вокруг плоскости панели: половина перед ней, половина за ней."),
    "layers": ("Слои глубины", "страница",
               "Бриф, MVP. Кольцо впереди, квадрат на панели, треугольник позади, рамка — сама плоскость панели. "
               "Фигуры покачиваются: движение помогает глазу прочесть глубину."),
    "stars": ("Звёзды", "заставка",
              "Бриф, MVP. Полёт сквозь звёздное поле с воспроизводимым seed. Перед панелью звёзды гаснут у краёв, "
              "чтобы рамка не резала то, что стоит перед ней."),
    "helix": ("Спираль", "заставка",
              "После MVP. Двойная спираль вращается вокруг своей оси: каждая точка качается между «перед» и «за»."),
    "rings": ("Кольца", "заставка",
              "После MVP. Кольца летят на зрителя по извилистой трубе. Всё за панелью — край ничего не режет."),
    "dial": ("Часы в слоях", "стиль часов",
             "После MVP. Циферблат на плоскости панели, цифры чуть впереди, стрелки ближе, секундная ближе всех; "
             "часы и минуты цифрами — позади, по бокам."),
    "torus": ("Тор со светом", "страница, заставка",
              "Сплошной тор с рассеянным светом и белым бликом; цвет бежит по кругу."),
    "vclock": ("Воксельные часы", "стиль часов",
               "Время кубиками 5×7, блок покачивается, чтобы читалась толщина; сменившаяся цифра переворачивается, "
               "двоеточие отбивает секунды."),
    "voxel": ("Полёт над землёй", "заставка, страница",
              "Ландшафт способом Comanche (1992): карта 256×256 из seed, свет запечён. В анаглифе — рельеф светом "
              "на чёрном, цвета в очках всё равно не видны."),
    "tunnel": ("Туннель", "заставка",
               "Полёт по трубе из неоновых колец и полос. Геометрия считается один раз на глаз, кадр только сдвигает узор."),
    "blobs": ("Метаболы", "заставка",
              "Четыре шара сливаются (рэймарчинг SDF, гладкий минимум). Самая дорогая сцена: цена на панели решит, "
              "полное ли разрешение."),
    "globe": ("Глобус", "страница рядом с мировыми часами",
              "Земля с сегодняшними днём и ночью: та же модель Солнца и та же маска суши (Natural Earth 1:110m), "
              "что у мировых часов, — граница дня и ночи на двух страницах совпадает. Города мировых часов — точками."),
    "terrain": ("Холмы звука", "визуализатор",
                "Спектр визуализатора рельефом: свежий спереди, прошлые уходят вдаль, на бите передний гребень "
                "вспыхивает. Звук в превью синтетический: 120 BPM, бочка, малый, хэт, мелодия."),
}


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


def mp4(frames, cie, path):
    # 4x with a one-pixel gap: still reads as LEDs, and a sixth of the 6x file.
    h, w = H * 4, W * 4
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                           "-s", f"{w}x{h}", "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-crf", "23",
                           "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(path)], stdin=subprocess.PIPE)
    for c, _ in frames:
        ff.stdin.write(np.asarray(led_image(emitted(c, cie), scale=4, gap=1)).tobytes())
    ff.stdin.close()
    ff.wait()


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


def calib_pages(exe, cie):
    rows = []
    for m in (0, 1):
        tiles = []
        for page in range(6):
            frames, _ = run(exe, "calib", m, 1, 20260918, page)
            tiles.append(led_image(emitted(frames[0][0], cie), 3, 1))
        w, h = tiles[0].size
        row = Image.new("RGB", (6 * (w + 6), h), (24, 24, 24))
        for i, t in enumerate(tiles):
            row.paste(t, (i * (w + 6), 0))
        rows.append(row)
    sheet = Image.new("RGB", (rows[0].size[0], 2 * rows[0].size[1] + 6), (24, 24, 24))
    sheet.paste(rows[0], (0, 0))
    sheet.paste(rows[1], (0, rows[0].size[1] + 6))
    sheet.save(OUT / "calib_pages.png")


def b64(path):
    return base64.b64encode(path.read_bytes()).decode()


def showreel(ids, timings):
    t = {(x["scene"], x["mode"]): x for x in timings}
    cards = []
    for scene in ids:
        title, where, text = NOTES.get(scene, (scene, "", ""))
        mono, rb = t.get((scene, 0)), t.get((scene, 1))
        cost = []
        if mono:
            cost.append(f"моно {mono['host_us_avg']:.0f} мкс")
        if rb:
            cost.append(f"анаглиф {rb['host_us_avg']:.0f} мкс")
        size = (mono or rb or {}).get("scene_bytes", 0)
        if scene == "calib":
            media = f'<img src="data:image/png;base64,{b64(OUT / "calib_pages.png")}" alt="calibration pages">'
        else:
            media = "".join(
                f'<figure><video autoplay loop muted playsinline src="data:video/mp4;base64,{b64(OUT / f"{scene}_{MODES[m]}.mp4")}">'
                f'</video><figcaption>{"моно" if m == 0 else "анаглиф, красный — левый глаз"}</figcaption></figure>'
                for m in (0, 1) if (OUT / f"{scene}_{MODES[m]}.mp4").exists())
        cards.append(f"""<section><h2>{html.escape(title)} <code>{scene}</code></h2>
<p class="where">{html.escape(where)}</p><p>{html.escape(text)}</p>
<div class="media">{media}</div>
<p class="cost">Кадр на этом Mac: {", ".join(cost)} · объект сцены {size / 1024:.1f} КБ (на панели — PSRAM) ·
цена на панели: ждёт стендовой прошивки</p></section>""")
    page = f"""<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>3D-эффекты: превью</title>
<style>
:root {{ color-scheme: dark; --bg:#0d0f12; --card:#16191e; --ink:#e6e8eb; --dim:#9aa3ad; --accent:#7fd1ff; }}
body {{ margin:0; background:var(--bg); color:var(--ink); font:15px/1.5 -apple-system, system-ui, sans-serif; }}
main {{ max-width:1400px; margin:0 auto; padding:16px; }}
h1 {{ font-size:22px; margin:8px 0 4px; }} .lead {{ color:var(--dim); margin:0 0 16px; }}
section {{ background:var(--card); border-radius:10px; padding:14px 16px; margin:0 0 14px; }}
h2 {{ font-size:18px; margin:0; }} h2 code {{ color:var(--dim); font-size:13px; margin-left:6px; }}
.where {{ color:var(--accent); margin:2px 0 6px; font-size:13px; }}
.media {{ display:flex; gap:12px; flex-wrap:wrap; }} figure {{ margin:0; flex:1 1 420px; }}
video, img {{ width:100%; height:auto; background:#000; border-radius:4px; display:block; image-rendering:pixelated; }}
figcaption, .cost {{ color:var(--dim); font-size:12.5px; margin-top:4px; }}
</style></head><body><main>
<h1>3D-эффекты для панели 128×64 — превью</h1>
<p class="lead">Отрендерено на Mac тем же кодом (src/fx3d), который пойдёт в прошивку; свет — тот, что выдаст панель
через таблицу CIE её драйвера. Анаглиф можно смотреть в очках PYRUVAE прямо с экрана Mac, но утечку каналов на
LED-панели это не показывает: у экрана Mac другие первичные цвета. Параллакс в превью — 2 пикселя, стартовая
настройка брифа.</p>
{"".join(cards)}
</main></body></html>"""
    (OUT / "showreel.html").write_text(page)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--modes", default="0,1")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--seed", type=int, default=20260918)
    ap.add_argument("--still", type=float, default=2.0, help="time of the contact sheet's still")
    ap.add_argument("--showreel", action="store_true", help="also write MP4s and showreel.html")
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
            if args.showreel:
                mp4(frames, cie, OUT / f"{scene}_{MODES[m]}.mp4")
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
    if args.showreel:
        calib_pages(exe, cie)
        showreel(ids, timings)


if __name__ == "__main__":
    main()

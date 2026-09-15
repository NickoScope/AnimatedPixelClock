#!/usr/bin/env python3
"""Render the visualizer effects, the six on the panel today and the eight
proposed ones, from a WAV through the onboard DSP chain.

  python3 tools/audiofx/render.py                      # all of them, from wav/showreel.wav
  python3 tools/audiofx/render.py --only w3_spectrogram,classic_eq --no-mp4

Into tools/audiofx/out/:
  <name>.gif          128x64 at 6x with the LED look, 20 fps (every third 60 Hz frame)
  <name>.mp4          the same at 60 fps with the audio, to judge the sync (needs ffmpeg)
  contact_current.png one row per current effect, stills at the moments in STILLS
  contact_wow.png     the same for the proposed effects

The current effects get what vizIngest() would get from the microphone: a
packet every 40 ms. The proposed ones get every 20 ms DSP frame. The panel
draws the visualizer at 60 Hz (src/main.cpp), so this does too.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dsp  # noqa: E402
import effects_current as cur  # noqa: E402
import effects_wow as wow  # noqa: E402
from gfx import H, W, Canvas, led_image  # noqa: E402

FPS = 60
GIF_EVERY = 3
SCALE = 6
OUT = HERE / "out"
STILLS = [(0.62, "kick"), (1.90, "groove + snare"), (2.95, "sweep"), (3.90, "silence"), (4.62, "groove returns")]
CURRENT_TITLES = {"classic_eq": "Classic EQ (0)", "neon_mirror": "Neon Mirror (1)",
                  "phosphor_waterfall": "Phosphor Waterfall (2)", "purple_led_stage": "Purple LED Stage (3)",
                  "starfield_overdrive": "Starfield Overdrive (5)", "oscilloscope": "Oscilloscope (6)"}


def catalogue():
    items = [(name, "current", CURRENT_TITLES[name], (lambda s=style: cur.Visualizer(s)))
             for style, name in cur.STYLES.items()]
    items += [(cls.name, "wow", cls.title, cls) for cls in wow.ALL]
    return items


def render(name, kind, factory, frames, wav, seconds, mp4):
    eff = factory()
    cv = Canvas()
    ff = None
    if mp4 and shutil.which("ffmpeg"):
        ff = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", f"{W * SCALE}x{H * SCALE}", "-r", str(FPS), "-i", "-", "-i", str(wav),
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-c:a", "aac", "-shortest",
             str(OUT / f"{name}.mp4")], stdin=subprocess.PIPE)
    gif, stills = [], {}
    still_frames = {int(round(t * FPS)): i for i, (t, _) in enumerate(STILLS)}
    fi = 0
    for n in range(int(seconds * FPS)):
        now = n * 1000.0 / FPS
        while fi < len(frames) and frames[fi]["t"] * 1000.0 <= now:
            f = frames[fi]
            if kind == "current":
                if f["published"]:
                    eff.ingest(f["bands8"], f["wave"], f["t"] * 1000.0)
            else:
                eff.update(f)
            fi += 1
        cv.clear()
        if kind == "current" and eff.last_rx is not None:
            eff.render(cv, int(now))
        elif kind == "wow" and eff.f is not None:
            eff.render(cv, now, 1.0 / FPS)
            eff.clock(cv, "18:27")
        rgb = cv.rgb()
        img = led_image(rgb, SCALE)
        if ff:
            ff.stdin.write(np.asarray(img).tobytes())
        if n % GIF_EVERY == 0:
            gif.append(img.convert("P", palette=Image.ADAPTIVE, colors=128))
        if n in still_frames:
            stills[still_frames[n]] = rgb.copy()
    if ff:
        ff.stdin.close()
        ff.wait()
    # 50 ms a frame. GIF counts centiseconds: 1000 // FPS * GIF_EVERY (48) played as 40 ms, 25 % fast.
    gif[0].save(OUT / f"{name}.gif", save_all=True, append_images=gif[1:],
                duration=round(1000 * GIF_EVERY / FPS), loop=0, disposal=1)
    return stills


def contact(rows, path):
    cell = 3
    cw, ch = W * cell, H * cell
    label_w, pad, header = 200, 10, 30
    img = Image.new("RGB", (label_w + len(STILLS) * (cw + pad) + pad, header + len(rows) * (ch + pad) + pad))
    d = ImageDraw.Draw(img)
    for k, (t, label) in enumerate(STILLS):
        d.text((label_w + pad + k * (cw + pad), 10), f"{t:.2f} s  {label}", fill=(170, 170, 170))
    for r, (title, stills) in enumerate(rows):
        y = header + r * (ch + pad)
        d.text((10, y + ch // 2 - 6), title, fill=(210, 210, 210))
        for k in range(len(STILLS)):
            if k in stills:
                img.paste(led_image(stills[k], cell), (label_w + pad + k * (cw + pad), y))
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", default=str(HERE / "wav/showreel.wav"))
    ap.add_argument("--only", default="")
    ap.add_argument("--no-mp4", action="store_true")
    args = ap.parse_args()
    wav = pathlib.Path(args.wav)
    if not wav.exists():
        subprocess.run([sys.executable, str(HERE / "gen_wavs.py")], check=True)
    OUT.mkdir(exist_ok=True)
    frames = dsp.run(wav)
    seconds = len(dsp.load_wav(wav)) / float(dsp.FS)
    print(f"{wav.name}: {len(frames)} DSP frames, beats at {[round(f['t'], 2) for f in frames if f['beat']]}")
    only = set(filter(None, args.only.split(",")))
    sheets = {"current": [], "wow": []}
    for name, kind, title, factory in catalogue():
        if only and name not in only:
            continue
        stills = render(name, kind, factory, frames, wav, seconds, not args.no_mp4)
        sheets[kind].append((title, stills))
        print(f"  {name}.gif")
    for kind, rows in sheets.items():
        if rows and not only:
            contact(rows, OUT / f"contact_{kind}.png")
            print(f"  contact_{kind}.png")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Render the Matrix Rain variants for visualizer style 2 from a WAV, fed both
ways the panel can feed them: the microphones' 20 ms DSP frames, and a PC
companion's 40 ms packets through PcFrameDeriver (viz_frame.py).

  python3 tools/audiofx/render_matrix.py

Into tools/audiofx/out/ (not committed):
  <variant>_mic.gif, <variant>_pc.gif   6x LED look, 20 fps, real time
  contact_matrix.png                    one row per variant and feed, the five stills of render.py

Also prints, per variant and feed, the glyphs and pixel writes per 60 Hz frame:
the basis of the CPU estimate.
"""
import pathlib
import sys

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dsp  # noqa: E402
import effects_matrix as mx  # noqa: E402
import render  # noqa: E402
import viz_frame  # noqa: E402
from gfx import BLACK, FONT, WHITE, W, Canvas, led_image  # noqa: E402

FPS, GIF_EVERY, SCALE = 60, 3, 6
OUT = HERE / "out"


class CountingCanvas(Canvas):
    """gfx.Canvas that counts glyphs drawn and lit pixels written, as drawChar() would write them."""

    def __init__(self):
        super().__init__()
        self.glyphs = 0
        self.pixels = 0

    def text(self, x, y, s, c):
        for ch in s:
            code = ord(ch)
            self.glyphs += 1
            self.pixels += sum(bin(FONT[code * 5 + i] & 0xFF).count("1") for i in range(5))
        super().text(x, y, s, c)


def run(cls, frames, seconds, tag):
    eff, cv, fi = cls(), CountingCanvas(), 0
    gif, stills, glyphs, pixels = [], {}, [], []
    still_frames = {int(round(t * FPS)): i for i, (t, _) in enumerate(render.STILLS)}
    for n in range(int(seconds * FPS)):
        now = n * 1000.0 / FPS
        while fi < len(frames) and frames[fi]["t"] * 1000.0 <= now:
            eff.update(frames[fi])
            fi += 1
        cv.clear()
        cv.glyphs = cv.pixels = 0
        if eff.f is not None:
            eff.render(cv, now, 1.0 / FPS)
        glyphs.append(cv.glyphs)
        pixels.append(cv.pixels)
        cv.fill_rect(W - 34, 0, 34, 10, BLACK)       # drawVizClock(), as every style
        Canvas.text(cv, W - 31, 1, "18:27", WHITE)
        rgb = cv.rgb()
        if n % GIF_EVERY == 0:
            gif.append(led_image(rgb, SCALE).convert("P", palette=Image.ADAPTIVE, colors=64))
        if n in still_frames:
            stills[still_frames[n]] = rgb.copy()
    gif[0].save(OUT / f"{cls.name}_{tag}.gif", save_all=True, append_images=gif[1:],
                duration=round(1000 * GIF_EVERY / FPS), loop=0, disposal=1)
    print(f"  {cls.name}_{tag}.gif   glyphs/frame mean {sum(glyphs) / len(glyphs):5.1f} max {max(glyphs):3d}   "
          f"lit pixels/frame mean {sum(pixels) / len(pixels):6.0f} max {max(pixels):5d}   "
          f"beats {sum(1 for f in frames if f['beat'])}")
    return stills


def main():
    wav = HERE / "wav/showreel.wav"
    OUT.mkdir(exist_ok=True)
    frames = dsp.run(wav)
    seconds = len(dsp.load_wav(wav)) / float(dsp.FS)
    feeds = (("mic", viz_frame.mic_frames(frames)), ("pc", viz_frame.pc_frames(frames)))
    rows = []
    for cls in mx.ALL:
        for tag, fr in feeds:
            rows.append((f"{cls.title} ({tag})", run(cls, fr, seconds, tag)))
    render.contact(rows, OUT / "contact_matrix.png")
    print("  contact_matrix.png")
    print("PSRAM per variant (C++ layout):", mx.PSRAM_BYTES)


if __name__ == "__main__":
    main()

"""A 128x64 RGB565 canvas with the Adafruit GFX calls the visualizer makes,
drawn pixel for pixel the way the library draws them, plus the LED-look
upscale the previews use.

Sources, all in this tree:
  rgb565()   vizRgb() in src/viz/visualizer.cpp (no clamping)
  rgbc()     rgb() in src/viz/oscilloscope.cpp (clamps to 0..255)
  to888()    MatrixPanel_I2S_DMA::color565to888() in the HUB75 library header
  line()     Adafruit_GFX::writeLine()
  text()     Adafruit_GFX::drawChar() and write(), classic 5x7 font, size 1
  circle()   Adafruit_GFX::drawCircle(), midpoint
"""
import json
import pathlib

import numpy as np
from PIL import Image

W, H = 128, 64
ROOT = pathlib.Path(__file__).resolve().parents[2]
FONT = json.load(open(ROOT / "tools/glcdfont.json"))
BLACK, WHITE = 0x0000, 0xFFFF


def rgb565(r, g, b):
    return (((int(r) & 0xF8) << 8) | ((int(g) & 0xFC) << 3) | (int(b) >> 3)) & 0xFFFF


def rgbc(r, g, b):
    return rgb565(min(255, max(0, int(r))), min(255, max(0, int(g))), min(255, max(0, int(b))))


def unpack(c):
    """unpack() in src/viz/oscilloscope.cpp."""
    return ((c >> 11) & 0x1F) * 255 // 31, ((c >> 5) & 0x3F) * 255 // 63, (c & 0x1F) * 255 // 31


def lround(v):
    return int(np.floor(v + 0.5)) if v >= 0 else -int(np.floor(-v + 0.5))


def to888_array(px):
    px = px.astype(np.uint32)
    r = (px >> 8) & 0xF8
    g = (px >> 3) & 0xFC
    b = (px << 3) & 0xFF
    return np.stack([r | (r >> 5), g | (g >> 6), b | (b >> 5)], axis=-1).astype(np.uint8)


class Canvas:
    def __init__(self):
        self.px = np.zeros((H, W), np.uint16)

    def clear(self):
        self.px[:] = 0

    def pixel(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            self.px[y, x] = c

    def hline(self, x, y, w, c):
        x, y, w = int(x), int(y), int(w)
        if w <= 0 or not 0 <= y < H:
            return
        x0, x1 = max(0, x), min(W, x + w)
        if x1 > x0:
            self.px[y, x0:x1] = c

    def vline(self, x, y, h, c):
        x, y, h = int(x), int(y), int(h)
        if h <= 0 or not 0 <= x < W:
            return
        y0, y1 = max(0, y), min(H, y + h)
        if y1 > y0:
            self.px[y0:y1, x] = c

    def fill_rect(self, x, y, w, h, c):
        x, y, w, h = int(x), int(y), int(w), int(h)
        if w <= 0 or h <= 0:
            return
        x0, x1, y0, y1 = max(0, x), min(W, x + w), max(0, y), min(H, y + h)
        if x1 > x0 and y1 > y0:
            self.px[y0:y1, x0:x1] = c

    def line(self, x0, y0, x1, y1, c):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        steep = abs(y1 - y0) > abs(x1 - x0)
        if steep:
            x0, y0, x1, y1 = y0, x0, y1, x1
        if x0 > x1:
            x0, x1, y0, y1 = x1, x0, y1, y0
        dx, dy = x1 - x0, abs(y1 - y0)
        err = dx // 2
        ystep = 1 if y0 < y1 else -1
        while x0 <= x1:
            if steep:
                self.pixel(y0, x0, c)
            else:
                self.pixel(x0, y0, c)
            err -= dy
            if err < 0:
                y0 += ystep
                err += dx
            x0 += 1

    def circle(self, x0, y0, r, c):
        x0, y0, r = int(x0), int(y0), int(r)
        if r <= 0:
            self.pixel(x0, y0, c)
            return
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
        for px, py in ((x0, y0 + r), (x0, y0 - r), (x0 + r, y0), (x0 - r, y0)):
            self.pixel(px, py, c)
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            for px, py in ((x0 + x, y0 + y), (x0 - x, y0 + y), (x0 + x, y0 - y), (x0 - x, y0 - y),
                           (x0 + y, y0 + x), (x0 - y, y0 + x), (x0 + y, y0 - x), (x0 - y, y0 - x)):
                self.pixel(px, py, c)

    def fill_circle(self, x0, y0, r, c):
        x0, y0, r = int(x0), int(y0), int(r)
        for dy in range(-r, r + 1):
            half = int((r * r - dy * dy) ** 0.5)
            self.hline(x0 - half, y0 + dy, 2 * half + 1, c)

    def text(self, x, y, s, c):
        x, y = int(x), int(y)
        for ch in s:
            code = ord(ch)
            if code >= 176:
                code += 1
            for i in range(5):
                bits = FONT[code * 5 + i]
                for j in range(8):
                    if bits & 1:
                        self.pixel(x + i, y + j, c)
                    bits >>= 1
            x += 6

    def rgb(self):
        return to888_array(self.px)


class RgbCanvas:
    """Float RGB 0..255 for effects that fade or add light: persistence, glow.
    On the panel this is a PSRAM buffer; every pixel still ends as RGB565."""

    def __init__(self):
        self.buf = np.zeros((H, W, 3), np.float32)

    def fade(self, k):
        self.buf *= k

    def add(self, x, y, r, g, b):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            p = self.buf[y, x]
            p[0] = min(235.0, p[0] + r)
            p[1] = min(235.0, p[1] + g)
            p[2] = min(235.0, p[2] + b)

    def blit(self, cv):
        q = np.clip(self.buf, 0, 255).astype(np.uint32)
        cv.px[:] = (((q[..., 0] & 0xF8) << 8) | ((q[..., 1] & 0xFC) << 3) | (q[..., 2] >> 3)).astype(np.uint16)


def led_image(rgb, scale=6, gap=1):
    """Each LED a (scale-gap) square on black, so the preview reads as a panel."""
    big = np.repeat(np.repeat(rgb, scale, axis=0), scale, axis=1)
    if gap:
        rows = (np.arange(big.shape[0]) % scale) >= scale - gap
        cols = (np.arange(big.shape[1]) % scale) >= scale - gap
        big[rows, :, :] = 0
        big[:, cols, :] = 0
    return Image.fromarray(big)

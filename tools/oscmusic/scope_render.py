#!/usr/bin/env python3
"""Render oscilloscope music (stereo WAV, L = X, R = Y) as a 128x64 panel clip.

The beam's dwell decides brightness, as on a real CRT: every sample lands in a
supersampled grid, so a slow part of the path collects more samples and glows
brighter. Frames keep a phosphor afterglow and a little bloom, then are mapped
to a fixed 16-level green palette (the panel's PCA1 clips are 4 bpp).

  scope_render.py sheet WAV OUT.png [--every S]          one frame every S seconds
  scope_render.py clip  WAV OUT.gif --start S --dur D     a clip for gif2pca
  scope_render.py pca   WAV OUT.pca --start S --dur D     the clip as PCA1 itself, as the
                                                          portal's clip maker writes it
"""
import argparse, struct, wave, sys
import numpy as np
from PIL import Image

W, H = 128, 64
SQ = 64                 # the scope face: a 64x64 square in the middle
SS = 4                  # supersampling per axis
FPS = 25
DECAY = 0.55            # afterglow kept per 40 ms frame
GAIN = 1.0 / 10.0       # samples per face pixel that reach ~63 % brightness

def read_span(path, start_s, dur_s):
    w = wave.open(path)
    rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
    if width != 2 or ch < 2:
        sys.exit(f"{path}: need 16-bit stereo, got {8*width}-bit {ch} ch")
    w.setpos(min(int(start_s * rate), w.getnframes()))
    raw = w.readframes(int(dur_s * rate))
    a = np.frombuffer(raw, dtype="<i2").reshape(-1, ch)[:, :2].astype(np.float32) / 32768.0
    return rate, a, w.getnframes() / rate

def palette16():
    # black, deep green through phosphor green to a white-hot core
    pal = []
    for i in range(16):
        v = i / 15.0
        r = int(255 * max(0.0, v - 0.75) / 0.25 * 0.85) if v > 0.75 else int(20 * v)
        g = int(255 * (v ** 0.8))
        b = int(255 * max(0.0, v - 0.8) / 0.2 * 0.7) if v > 0.8 else int(40 * v)
        pal.append((min(r, 255), min(g, 255), min(b, 255)))
    return pal

def face_counts(lr):
    n = SQ * SS
    x = np.clip(((lr[:, 0] * 0.5 + 0.5) * n).astype(np.int32), 0, n - 1)
    y = np.clip(((0.5 - lr[:, 1] * 0.5) * n).astype(np.int32), 0, n - 1)
    grid = np.zeros((n, n), dtype=np.float32)
    np.add.at(grid, (y, x), 1.0)
    return grid.reshape(SQ, SS, SQ, SS).sum(axis=(1, 3)) / SS   # per face pixel, path-length scaled

def bloom(img):
    k = np.array([1, 2, 1], dtype=np.float32) / 4.0
    b = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 1, img)
    return np.apply_along_axis(lambda c: np.convolve(c, k, mode="same"), 0, b)

def render(rate, a, frames, per_frame_scale):
    spf = rate // FPS
    glow = np.zeros((SQ, SQ), dtype=np.float32)
    pal = np.array(palette16(), dtype=np.uint8)
    out = []
    for f in range(frames):
        chunk = a[f * spf:(f + 1) * spf]
        if len(chunk) == 0:
            break
        glow = glow * DECAY + face_counts(chunk) * per_frame_scale
        v = 1.0 - np.exp(-glow * GAIN)
        v = np.clip(v + 0.3 * bloom(v), 0.0, 1.0)
        idx = np.round(v * 15).astype(np.uint8)
        frame = np.zeros((H, W), dtype=np.uint8)
        x0 = (W - SQ) // 2
        frame[:, x0:x0 + SQ] = idx
        out.append(frame)
    return out, pal

def to_image(frame, pal):
    im = Image.fromarray(frame, mode="P")
    flat = [c for rgb in pal for c in rgb] + [0] * (768 - 48)
    im.putpalette(flat)
    return im

def write_pca(path, frames):
    # PCA1 as gif2pca lays it out, with the fixed palette kept as it is (no
    # re-quantising through a GIF): 16 RGB565 colours, 40 ms a frame, 4 bpp with
    # the high nibble the left pixel. Byte for byte the portal's clip maker.
    if not 1 <= len(frames) <= 65535:
        sys.exit(f"{len(frames)} frames: PCA1 counts 1 to 65535")
    ms = 1000 // FPS
    with open(path, "wb") as f:
        f.write(b"PCA1" + struct.pack("<HHBBH", len(frames), ms, 16, 1, 0))
        f.write(struct.pack("<16H", *[((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in palette16()]))
        f.write(struct.pack("<%dH" % len(frames), *[ms] * len(frames)))
        for fr in frames:
            f.write((fr[:, 0::2] << 4 | fr[:, 1::2]).astype(np.uint8).tobytes())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["sheet", "clip", "pca"])
    ap.add_argument("wav"); ap.add_argument("out")
    ap.add_argument("--every", type=float, default=15.0)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--dur", type=float, default=14.0)
    a = ap.parse_args()
    # 192 kHz carries four times the samples per frame of 48 kHz: normalise so
    # the same drawing reaches the same brightness at any rate.
    if a.mode == "sheet":
        rate, _, total = read_span(a.wav, 0, 0.01)
        scale = 48000.0 / rate
        tiles = []
        t = a.every / 2
        while t < total - 1:
            rate, span, _ = read_span(a.wav, t, 12.0 / FPS)
            frames, pal = render(rate, span, 12, scale)
            tiles.append((t, to_image(frames[-1], pal).convert("RGB")))
            t += a.every
        cols = 4
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (W * 2 + 8), rows * (H * 2 + 18)), (30, 30, 30))
        from PIL import ImageDraw
        d = ImageDraw.Draw(sheet)
        for i, (t, im) in enumerate(tiles):
            cx, cy = (i % cols) * (W * 2 + 8), (i // cols) * (H * 2 + 18)
            sheet.paste(im.resize((W * 2, H * 2), Image.NEAREST), (cx, cy + 16))
            d.text((cx + 2, cy + 2), f"{int(t)//60}:{int(t)%60:02d}", fill=(220, 220, 220))
        sheet.save(a.out)
        print(f"sheet: {len(tiles)} tiles, track {total:.0f} s, rate {rate}")
    elif a.mode == "pca":
        rate, span, total = read_span(a.wav, a.start, a.dur)
        frames, _ = render(rate, span, int(a.dur * FPS), 48000.0 / rate)
        write_pca(a.out, frames)
        print(f"pca: {len(frames)} frames at {FPS} fps from {a.start:.1f} s, rate {rate}, {44 + len(frames) * 4098} bytes")
    else:
        rate, span, total = read_span(a.wav, a.start, a.dur)
        frames, pal = render(rate, span, int(a.dur * FPS), 48000.0 / rate)
        ims =[to_image(f, pal) for f in frames]
        ims[0].save(a.out, save_all=True, append_images=ims[1:], duration=1000 // FPS, loop=0, optimize=False)
        big = [im.convert("RGB").resize((W * 4, H * 4), Image.NEAREST) for im in ims[::2]]
        big[0].save(a.out.replace(".gif", "_x4.gif"), save_all=True, append_images=big[1:], duration=2000 // FPS, loop=0)
        print(f"clip: {len(ims)} frames at {FPS} fps from {a.start:.1f} s, rate {rate}")

if __name__ == "__main__":
    main()

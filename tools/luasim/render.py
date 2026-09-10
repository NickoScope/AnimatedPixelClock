#!/usr/bin/env python3
"""Turn luasim's raw frames into a PNG or an animated GIF."""
import sys, pathlib
from PIL import Image
W, H = 128, 64
raw = pathlib.Path(sys.argv[1]).read_bytes()
out = sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 8
n = len(raw) // (W * H * 3)
frames = [Image.frombytes("RGB", (W, H), raw[i*W*H*3:(i+1)*W*H*3])
          .resize((W*scale, H*scale), Image.NEAREST) for i in range(n)]
if out.endswith(".gif") and n > 1:
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=50, loop=0)
else:
    frames[0].save(out)
print(f"{out}: {n} frames")

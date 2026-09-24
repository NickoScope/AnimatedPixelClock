"""luasim frames -> an MP4 that looks like the panel: each LED a round dot (or "pixels" for plain squares).

    python3 tools/luasim/golf_video.py scripts/golf_old_course.lua out.mp4 10:00 [led|pixels]
"""
import sys, subprocess, pathlib
import numpy as np
W, H, SC = 128, 64, 8
script, out = sys.argv[1], sys.argv[2]
start = sys.argv[3] if len(sys.argv) > 3 else "10:00"
raw = pathlib.Path(out + ".raw")
subprocess.run(["./luasim", script, "1800", str(raw), "--start", start], check=True,
               cwd="/Users/apple/AnimatedPixelClock-netbroker/tools/luasim", capture_output=True)
b = np.frombuffer(raw.read_bytes(), dtype=np.uint8)
n = len(b) // (W * H * 3)
frames = b[: n * W * H * 3].reshape(n, H, W, 3).astype(np.float32)
# a round LED on a dark board, with a little glow
yy, xx = np.mgrid[0:SC, 0:SC]
d = np.sqrt((xx - (SC - 1) / 2) ** 2 + (yy - (SC - 1) / 2) ** 2) / (SC / 2)
mask = np.clip(1.45 - d * 0.95, 0.10, 1.0) ** 0.9
if len(sys.argv) > 4 and sys.argv[4] == 'pixels':
    mask = np.ones_like(mask)
mask = np.tile(mask, (H, W))[..., None]
ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                       "-s", f"{W*SC}x{H*SC}", "-r", "15", "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                       "-crf", "18", "-preset", "medium", out], stdin=subprocess.PIPE)
for i in range(n):
    f = frames[i].repeat(SC, axis=0).repeat(SC, axis=1)
    f = np.clip(f * mask[..., 0][..., None] * (1.0 if (len(sys.argv) > 4 and sys.argv[4] == 'pixels') else 1.22) + 4, 0, 255).astype(np.uint8)
    ff.stdin.write(f.tobytes())
ff.stdin.close(); ff.wait()
raw.unlink()
print(out, n, "frames")

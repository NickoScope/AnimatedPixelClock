#!/usr/bin/env python3
"""Hold visualizer styles 7-14 in the firmware (src/viz/wow/) to effects_wow.py, pixel for pixel.

  make -C tools/audiofx/host wow            (or python3 tools/audiofx/host/compare_wow.py)
  python3 tools/audiofx/host/compare_wow.py --pc-gifs    also: the eight fed from PC packets, as GIFs

test_wow is built twice. With real = double it computes as Python does and must
match exactly; that proves the port. With real = float it runs as the panel runs
it, and its difference from the previews is reported, not failed. Both render
the showreel from the C++ DSP's frames (frames.bin), which the Python effects
then render too, 60 frames a second, without the clock.
"""
import argparse
import pathlib
import struct
import subprocess
import sys

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent
AFX = HOST.parent
sys.path.insert(0, str(AFX))
import dsp  # noqa: E402
import effects_wow as wow  # noqa: E402
import effects_matrix  # noqa: E402
from gfx import H, W, Canvas, led_image, to888_array  # noqa: E402

FPS = 60
WAV = AFX / "wav/showreel.wav"
FONT = AFX.parent / "glcdfont.json"
RECORD = struct.Struct("<d32B128B32f32f4f4B")
EFFECTS = wow.ALL + [effects_matrix.CodeEQ]   # the engine's effects 0-8; 8 is style 2


def load_frames(path):
    data = path.read_bytes()
    if data[:4] != b"WOWF":
        raise SystemExit(f"{path}: not a frames file")
    (n,) = struct.unpack_from("<I", data, 4)
    out, off = [], 8
    for _ in range(n):
        v = RECORD.unpack_from(data, off)
        off += RECORD.size
        out.append({"t": v[0], "bands8": list(v[1:33]), "wave": list(v[33:161]),
                    "level": list(v[161:193]), "peak": list(v[193:225]),
                    "bass": v[225], "mid": v[226], "treble": v[227], "strength": v[228],
                    "beat": bool(v[229]), "clipping": bool(v[230]), "steps": v[231]})
    return out


def python_frames(cls, frames, seconds):
    """render.py's loop for a wow effect, without the clock."""
    eff, cv, fi, out = cls(), Canvas(), 0, []
    for n in range(int(seconds * FPS)):
        now = n * 1000.0 / FPS
        while fi < len(frames) and frames[fi]["t"] * 1000.0 <= now:
            eff.update(frames[fi])
            fi += 1
        cv.clear()
        if eff.f is not None:
            eff.render(cv, now, 1.0 / FPS)
        out.append(cv.px.copy())
    return out


def compare(py, raw):
    c = np.fromfile(raw, dtype="<u2").reshape(-1, H, W)
    r = {"frames": len(py), "differ": 0, "pixels": 0, "worst": 0, "channel": 0, "first": None}
    if len(c) != len(py):
        r["differ"] = -1
        return r
    for n, p in enumerate(py):
        k = int((c[n] != p).sum())
        if not k:
            continue
        r["differ"] += 1
        r["pixels"] += k
        r["worst"] = max(r["worst"], k)
        r["first"] = n if r["first"] is None else r["first"]
        r["channel"] = max(r["channel"], int(np.abs(to888_array(c[n]).astype(int) - to888_array(p).astype(int)).max()))
    return r


def line(name, r):
    if r["differ"] < 0:
        return f"  {name:22s} FRAME COUNT DIFFERS"
    if not r["differ"]:
        return f"  {name:22s} {r['frames']} frames, identical"
    return (f"  {name:22s} {r['differ']:3d}/{r['frames']} frames differ, worst {r['worst']} px in a frame, "
            f"{r['pixels']} px in all, largest channel step {r['channel']}/255, first at frame {r['first']}")


def gifs(outdir, prefix):
    (AFX / "out").mkdir(exist_ok=True)
    for k, cls in enumerate(EFFECTS):
        c = np.fromfile(outdir / f"{prefix}{k}.raw", dtype="<u2").reshape(-1, H, W)
        imgs = [led_image(to888_array(c[n]), 6).convert("P", colors=128) for n in range(0, len(c), 3)]
        imgs[0].save(AFX / "out" / f"pc_{cls.name}.gif", save_all=True, append_images=imgs[1:], duration=50,
                     loop=0, disposal=1)
        print(f"  out/pc_{cls.name}.gif")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pc-gifs", action="store_true")
    args = ap.parse_args()
    if not WAV.exists():
        subprocess.run([sys.executable, str(AFX / "gen_wavs.py")], check=True)
    subprocess.run(["make", "-s", "-C", str(HOST), "build/test_wow_d", "build/test_wow_f"], check=True)
    seconds = len(dsp.load_wav(WAV)) / float(dsp.FS)
    bad = 0
    results = {}
    for kind in ("d", "f"):
        outdir = HOST / "build" / f"wow_{kind}"
        outdir.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(HOST / f"build/test_wow_{kind}"), str(WAV), str(FONT), str(outdir)], check=True)
    frames = load_frames(HOST / "build/wow_d/frames.bin")
    print(f"showreel: {len(frames)} DSP frames, beats at {[round(f['t'], 2) for f in frames if f['beat']]}")
    for k, cls in enumerate(EFFECTS):
        py = python_frames(cls, frames, seconds)
        results[cls.name] = (compare(py, HOST / f"build/wow_d/wow_{k}.raw"), compare(py, HOST / f"build/wow_f/wow_{k}.raw"))
    print("\nreal = double (the port against the Python reference; must be identical):")
    for name, (d, _) in results.items():
        print(line(name, d))
        bad += bool(d["differ"])
    print("\nreal = float (as the panel computes; reported):")
    for name, (_, f) in results.items():
        print(line(name, f))
    pcdir = HOST / "build/wow_pc"
    pcdir.mkdir(parents=True, exist_ok=True)
    print("\nfed from PC packets (PcFrameDeriver on the DSP's 40 ms packets):")
    subprocess.run([str(HOST / "build/test_wow_d"), str(WAV), str(FONT), str(pcdir), "--pc"], check=True)
    pc_frames = load_frames(pcdir / "frames.bin")
    print("\nfed from PC packets, real = double, against the Python effects on the same derived frames:")
    for k, cls in enumerate(EFFECTS):
        r = compare(python_frames(cls, pc_frames, seconds), pcdir / f"pc_wow_{k}.raw")
        print(line(cls.name, r))
        bad += bool(r["differ"])
    if args.pc_gifs:
        gifs(pcdir, "pc_wow_")
    print("\nthe C++ effects match effects_wow.py" if not bad else f"\n{bad} effect(s) differ in the double build")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

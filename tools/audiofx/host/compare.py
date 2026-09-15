#!/usr/bin/env python3
"""Hold the firmware's DSP to the Python reference, frame by frame, on every test WAV.

  make -C tools/audiofx/host check        (or python3 tools/audiofx/host/compare.py)

Builds src/audio/audio_dsp.cpp for this machine, runs it and tools/audiofx/dsp.py
over tools/audiofx/wav/*.wav (made by gen_wavs.py if missing) and fails on:
  - a different band -> FFT bin table
  - a different number of frames
  - any frame where gated, beat, clip, silent or published differ
  - a band byte or a waveform byte more than BYTE_TOL apart
  - level_db or ref_db more than DB_TOL apart
The C++ runs a float32 FFT, numpy a float64 one; bytes are truncated after
scaling, so a value sitting on a boundary may land one apart. Nothing else may.

It also scores the beats against each WAV's ground truth (.json) and prints it.
"""
import csv
import io
import json
import pathlib
import subprocess
import sys

HOST = pathlib.Path(__file__).resolve().parent
AFX = HOST.parent
sys.path.insert(0, str(AFX))
import dsp  # noqa: E402

BYTE_TOL = 1
DB_TOL = 0.02
FLAGS = ("gated", "beat", "clip", "silent", "published")


def build():
    subprocess.run(["make", "-s", "-C", str(HOST), "build/test_dsp"], check=True)
    return HOST / "build/test_dsp"


def cpp_rows(exe, wav):
    out = subprocess.run([str(exe), str(wav)], check=True, capture_output=True, text=True).stdout
    return list(csv.DictReader(io.StringIO(out)))


def py_rows(wav):
    buf = io.StringIO()
    frames = dsp.run(wav)
    tmp = HOST / "build" / (wav.stem + ".py.csv")
    dsp.write_csv(frames, tmp)
    return list(csv.DictReader(open(tmp))), frames


def score(beats, truth):
    kicks = truth.get("kicks")
    if kicks is None:
        return ""
    used, hits, lat = set(), 0, []
    allowed = truth.get("snares", [])
    extra = 0
    for b in beats:
        m = [k for k in kicks if -0.03 <= b - k <= 0.07 and k not in used]
        if m:
            used.add(m[0])
            hits += 1
            lat.append(b - m[0])
        elif not any(-0.03 <= b - a <= 0.07 for a in allowed):
            extra += 1
    mean = 1000 * sum(lat) / len(lat) if lat else 0.0
    return f"kicks {hits}/{len(kicks)}, other beats {extra}, mean latency {mean:.0f} ms"


def main():
    wavdir = AFX / "wav"
    if not (wavdir / "showreel.wav").exists():
        subprocess.run([sys.executable, str(AFX / "gen_wavs.py")], check=True)
    exe = build()
    bad = 0

    bins = subprocess.run([str(exe), "--bins"], check=True, capture_output=True, text=True).stdout.split()
    cpp_bins = [tuple(int(v) for v in line.split(",")) for line in bins]
    if cpp_bins != dsp.band_bins():
        print("band table differs:\n  C++   ", cpp_bins, "\n  Python", dsp.band_bins())
        bad += 1
    edges = [dsp.F_LO * (dsp.F_HI / dsp.F_LO) ** (i / dsp.BANDS) / (dsp.FS / dsp.N) for i in range(dsp.BANDS + 1)]
    margin = min(min(e - int(e), int(e) + 1 - e) for e in edges if abs(e - round(e)) > 0 or True)
    print(f"band table: {len(cpp_bins)} bands, nearest band edge {margin:.4f} bins from an integer")

    for wav in sorted(wavdir.glob("*.wav")):
        c = cpp_rows(exe, wav)
        p, frames = py_rows(wav)
        problems = []
        if len(c) != len(p):
            problems.append(f"frames: C++ {len(c)}, Python {len(p)}")
        worst_b = worst_w = 0
        worst_db = 0.0
        for rc, rp in zip(c, p):
            for flag in FLAGS:
                if rc[flag] != rp[flag]:
                    problems.append(f"frame {rp['k']} {flag}: C++ {rc[flag]}, Python {rp[flag]}")
            for i in range(dsp.BANDS):
                worst_b = max(worst_b, abs(int(rc[f"b{i}"]) - int(rp[f"b{i}"])))
            for i in range(dsp.WAVE_POINTS):
                worst_w = max(worst_w, abs(int(rc[f"w{i}"]) - int(rp[f"w{i}"])))
            for col in ("level_db", "ref_db"):
                worst_db = max(worst_db, abs(float(rc[col]) - float(rp[col])))
        if worst_b > BYTE_TOL:
            problems.append(f"band bytes up to {worst_b} apart")
        if worst_w > BYTE_TOL:
            problems.append(f"waveform bytes up to {worst_w} apart")
        if worst_db > DB_TOL:
            problems.append(f"dB up to {worst_db:.3f} apart")
        truth = json.loads((wav.with_suffix(".json")).read_text())
        beats = [f["t"] for f in frames if f["beat"]]
        verdict = "ok" if not problems else "MISMATCH"
        print(f"  {wav.name:18s} {len(p):4d} frames  {verdict:8s} bands +-{worst_b} wave +-{worst_w} "
              f"dB +-{worst_db:.3f}  beats {len(beats)}  {score(beats, truth)}")
        for msg in problems[:8]:
            print("      " + msg)
        bad += bool(problems)
    print("\nC++ and Python agree" if not bad else f"\n{bad} file(s) disagree")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

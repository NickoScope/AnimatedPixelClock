#!/usr/bin/env python3
"""Hold the firmware's Lua effect runtime to luasim, pixel for pixel.

Builds the reference (luasim) and fxhost, which is src/lua/lua_fx.cpp,
lua_px.cpp and nslua_sandbox.cpp - the files the panel runs - compiled for
this machine against a stand-in display. Renders every script in scripts/
through both with the same clock and compares the raw frames byte for byte.
Then measures each script: exact instruction counts for the load and for a
frame, host draw time, Lua heap peak, and the C stack the host thread used.

  python3 tools/luasim/fx_parity.py           # parity and measurements
  python3 tools/luasim/fx_parity.py --quick   # parity only

Needs one PlatformIO build first, for Adafruit GFX's gfxfont.h and the font
(gen_font.py has the same requirement). Exit status 1 on any difference.
"""
import glob, pathlib, re, shutil, subprocess, sys, tempfile

ROOT  = pathlib.Path(__file__).resolve().parents[2]
SIM   = ROOT / "tools/luasim"
BUILD = SIM / ".fxbuild"
LUA   = ROOT / "src/lua/vendor/lua"
W, H  = 128, 64
FRAME = W * H * 3

# (label, start, frames, extra flags). Every script sees every case, including
# the ones it ignores: a clock-free script must still match at any clock.
CASES = [
    ("12:34, 240 frames",       "12:34", 240, []),
    ("23:59, hour wraps",       "23:59",  90, []),
    ("day sweep, 96 frames",    "00:00",  96, ["--sweep"]),
    ("06:10, yday 0, UTC-5",    "06:10",  60, ["--yday", "0", "--utc", "-5"]),
]

# The bench on the panel (src/lua/nslua_bench.cpp, phase 6b): kCompute took
# 64.1 ms per nslua_run, of which 2.54 ms is the fresh sandboxed state.
BENCH_COMPUTE = "local x = 0 for i = 1, 50000 do x = x + i % 7 end\nfunction draw() end\n"
BENCH_MS = 64.1 - 2.54


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build():
    sh(["make", "-s", "-C", str(SIM), "luasim"])
    gfx = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/gfxfont.h"))
    if not gfx:
        sys.exit("fx_parity: build the firmware once so Adafruit GFX is downloaded")
    gfx_dir = str(pathlib.Path(gfx[0]).parent)
    BUILD.mkdir(exist_ok=True)
    objs = []
    for c in sorted(LUA.glob("*.c")):
        if c.name in ("lua.c", "luac.c"):
            continue
        o = BUILD / (c.stem + ".o")
        if not o.exists() or o.stat().st_mtime < c.stat().st_mtime:
            sh(["cc", "-O2", "-I", str(LUA), "-c", str(c), "-o", str(o)])
        objs.append(str(o))
    srcs = [ROOT / "src/lua/lua_fx.cpp", ROOT / "src/lua/lua_px.cpp",
            ROOT / "src/lua/nslua_sandbox.cpp", SIM / "fxhost.cpp"]
    sh(["c++", "-std=gnu++11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", str(ROOT / "src/lua"), "-I", str(SIM / "host"), "-I", gfx_dir,
        *map(str, srcs), *objs, "-o", str(SIM / "fxhost"), "-lm", "-lpthread"])


def render(tool, script, out, start, frames, flags):
    r = subprocess.run([str(SIM / tool), str(script), str(frames), str(out), "--start", start, *flags],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{tool} {script.name}: {r.stderr.strip()}")
    return r.stderr


def stats(stderr):
    out = {}
    for line in stderr.splitlines():
        if line.startswith("fxhost:"):
            toks = line.split()[1:]
            if toks[0] == "stack_used":
                out["stack_used"] = int(toks[1])
                continue
            out["name"] = toks[0]
            for k, v in zip(toks[1::2], toks[2::2]):
                out[k] = float(v)
    return out


def compare(a, b):
    """(frames that differ, pixels that differ, largest channel difference)."""
    da, db = a.read_bytes(), b.read_bytes()
    if len(da) != len(db):
        return (-1, -1, -1)
    frames = pixels = worst = 0
    for f in range(len(da) // FRAME):
        fa, fb = da[f * FRAME:(f + 1) * FRAME], db[f * FRAME:(f + 1) * FRAME]
        if fa == fb:
            continue
        frames += 1
        for i in range(0, FRAME, 3):
            if fa[i:i + 3] != fb[i:i + 3]:
                pixels += 1
                worst = max(worst, *(abs(fa[i + k] - fb[i + k]) for k in range(3)))
    return (frames, pixels, worst)


def main():
    quick = "--quick" in sys.argv
    build()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="fxparity"))
    bad = 0
    try:
        scripts = sorted((SIM / "scripts").glob("*.lua"))
        # Negative control: the same script one minute apart must differ. If it
        # does not, the comparison is blind and "identical" below means nothing.
        tetris = SIM / "scripts/tetris_clock.lua"
        render("luasim", tetris, tmp / "a.raw", "12:34", 30, [])
        render("fxhost", tetris, tmp / "b.raw", "12:35", 30, [])
        if compare(tmp / "a.raw", tmp / "b.raw")[0] <= 0:
            sys.exit("fx_parity: the comparison cannot see a one-minute difference - not trusting it")
        print("negative control: 12:34 against 12:35 differs, so the comparison can see a change")
        print("parity against luasim (frames differing / pixels differing / worst channel delta)")
        for s in scripts:
            for label, start, frames, flags in CASES:
                ref, got = tmp / "ref.raw", tmp / "got.raw"
                render("luasim", s, ref, start, frames, flags)
                render("fxhost", s, got, start, frames, flags + ["--panel-limits"])
                fr, px, worst = compare(ref, got)
                ok = fr == 0
                bad += not ok
                print(f"  {s.name:18s} {label:24s} {'identical' if ok else f'{fr} frames, {px} px, max {worst}':>22s}")
        if quick:
            return
        print("\nmeasurements, 12:34 case, 240 frames (instructions exact; times are this host's)")
        print(f"  {'script':14s} {'load instr':>11s} {'draw instr avg/max':>19s} {'host draw ms avg/max':>21s}"
              f" {'heap peak KB':>12s} {'incr: ms / KB':>13s} {'stack KB':>8s} {'fps':>4s} {'period':>6s}")
        calib = tmp / "calib.lua"
        calib.write_text(BENCH_COMPUTE)
        ns = BENCH_MS * 1e6 / stats(render("fxhost", calib, tmp / "c.raw", "12:34", 1, ["--exact"]))["load_instr"]
        # The harness itself (file I/O, printf, thread start) uses stack too:
        # measured on a script that does nothing, and taken off every figure.
        empty = tmp / "empty.lua"
        empty.write_text("function draw() end\n")
        base = stats(render("fxhost", empty, tmp / "e.raw", "12:34", 240, []))["stack_used"]
        for s in scripts:
            ex = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, ["--exact"]))
            tm = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, []))
            gn = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, ["--incremental"]))
            print(f"  {ex['name']:14s} {ex['load_instr']:11.0f} {ex['instr_avg']:9.0f}/{ex['instr_max']:<9.0f}"
                  f" {tm['draw_ms_avg']:10.3f}/{tm['draw_ms_max']:<10.3f} {tm['heap_peak'] / 1024:12.0f}"
                  f" {gn['draw_ms_avg']:6.3f}/{gn['heap_peak'] / 1024:<6.0f} {(tm['stack_used'] - base) / 1024:8.1f}"
                  f" {tm['fps']:4.0f} {tm['period']:6.0f}")
            print(f"  {'':14s} panel estimate at the bench's VM rate: load {ex['load_instr'] * ns / 1e6:.0f} ms,"
                  f" draw {ex['instr_avg'] * ns / 1e6:.1f}/{ex['instr_max'] * ns / 1e6:.1f} ms (VM only, px.* C work not included)")
        print(f"\n  bench calibration: {ns:.0f} ns per instruction on the panel (kCompute, phase 6b)")
        print(f"  stack KB is above the harness's own {base / 1024:.1f} KB, on this host's ABI - not Xtensa's")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        if bad:
            print(f"\n{bad} case(s) differ")
    sys.exit(1 if bad else 0)


main()

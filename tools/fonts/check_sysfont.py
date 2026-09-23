#!/usr/bin/env python3
"""Host test of the system font: the panel's print() over the real Adafruit GFX.

Compiles tools/fonts/sysfont_host_test.cpp with Adafruit GFX's own
Adafruit_GFX.cpp (from the PlatformIO library folder, after one build) and the
Arduino stand-ins in tools/fonts/hoststub, and runs it. Also checks that the
generated font headers are in step with their generators.

    python3 tools/fonts/check_sysfont.py
"""
import glob, pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    ok = True
    for gen in ("tools/fonts/mksysfont.py", "tools/fonts/mkcyr.py"):
        r = subprocess.run([sys.executable, str(ROOT / gen), "--check"], capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        ok &= r.returncode == 0
    gfx = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library")),
                 key=lambda p: "matrix-waveshare-rgb" not in p)   # the panel's own copy first
    if not gfx:
        sys.exit("check_sysfont: build once so Adafruit GFX is downloaded")
    gfx = pathlib.Path(gfx[0])
    with tempfile.TemporaryDirectory() as d:
        exe = pathlib.Path(d) / "sysfont"
        cmd = ["c++", "-std=gnu++17", "-O1", "-Wall", "-Wno-unused-function", "-DARDUINO=100",
               "-I", str(ROOT / "tools/fonts/hoststub"), "-I", str(gfx), "-I", str(ROOT / "src/fonts"),
               str(ROOT / "tools/fonts/sysfont_host_test.cpp"), str(gfx / "Adafruit_GFX.cpp"),
               "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-4000:])
            sys.exit("check_sysfont: the host test did not build")
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        ok &= r.returncode == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

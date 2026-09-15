"""Builds tools/market/panel/market_host_test.cpp against the firmware's market model.

One binary, four uses: the tests, the number formatting for the comparison with
render.py, a page's layout dump and raster from a spec, and a single payload
through the panel's ingest. Built into .pio/market/ and rebuilt whenever a
source is newer than it. Needs ArduinoJson and the Adafruit GFX library from
.pio/libdeps (build any env once); the fonts come from there too, through the
stub in hoststub/ that stands in for Adafruit_GFX.h.
"""
import glob
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXE = ROOT / ".pio" / "market" / "market_host_test"
SRC = ROOT / "src/market"
EXE_LOCAL = ROOT / ".pio" / "market" / "market_host_local"
LOCAL_EXAMPLE = HERE / "local_defaults_example.h"
SOURCES = [HERE / "market_host_test.cpp", SRC / "market_model.cpp", SRC / "market_layout.cpp", SRC / "market_pico.cpp",
           SRC / "market_settings.cpp", SRC / "market_model.h", SRC / "market_layout.h", SRC / "market_settings.h",
           SRC / "market_settle.h", ROOT / "src/panel/panel.h", ROOT / "src/panel/panel_pages.h", LOCAL_EXAMPLE,
           HERE / "hoststub/Adafruit_GFX.h", ROOT / "src/fonts/picopixel_fb.h"]


def libdep(name):
    hits = sorted(glob.glob(str(ROOT / ".pio/libdeps/*" / name)))
    if not hits:
        sys.exit(f"probe.py: build any env once so PlatformIO installs {name}")
    return hits[0]


def compile_exe(exe, defines):
    if exe.exists() and all(exe.stat().st_mtime >= s.stat().st_mtime for s in SOURCES):
        return exe
    exe.parent.mkdir(parents=True, exist_ok=True)
    gfx = libdep("Adafruit GFX Library")
    aj = libdep("ArduinoJson") + "/src"
    cmd = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-const-variable", *defines,
           "-I", str(SRC), "-I", str(ROOT / "src"), "-I", str(ROOT / "src/fonts"), "-I", str(HERE / "hoststub"), "-I", gfx, "-I", aj,
           *[str(s) for s in SOURCES if s.suffix == ".cpp"], "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe


def build():
    """The neutral defaults, whatever src/market/market_local_defaults.h holds on this machine."""
    return compile_exe(EXE, ["-DMARKET_NO_LOCAL_DEFAULTS"])


def build_local():
    """The local defaults mechanism, with the committed example header (relative to src/market/)."""
    return compile_exe(EXE_LOCAL, ['-DMARKET_LOCAL_DEFAULTS_FILE="../../tools/market/panel/local_defaults_example.h"'])


def run(*args, stdin=None, local=False):
    r = subprocess.run([str(build_local() if local else build()), *args], input=stdin, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


if __name__ == "__main__":
    print(build())

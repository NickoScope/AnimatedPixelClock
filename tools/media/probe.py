"""Builds tools/media/media_host_test.cpp against the firmware's media model.

One binary, three uses: the tests, the transliteration render.py draws with,
and a single payload through the panel's parser. Built into .pio/media/ and
rebuilt whenever a source is newer than it.
"""
import glob
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXE = ROOT / ".pio" / "media" / "media_host_test"
SOURCES = [HERE / "media_host_test.cpp", ROOT / "src/media/media_model.cpp", ROOT / "src/media/media_model.h"]


def build():
    libs = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/ArduinoJson/src")))
    if not libs:
        sys.exit("probe.py: build any env once so PlatformIO installs ArduinoJson")
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SOURCES):
        return EXE
    EXE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/media"), "-I", libs[0],
                    str(SOURCES[0]), str(SOURCES[1]), "-o", str(EXE)], check=True)
    return EXE


def translit(lines):
    """The firmware's translit() for each line; control characters are spaces first, as copyUtf8 makes them."""
    clean = ["".join(" " if (ord(ch) < 0x20 or 0x7F <= ord(ch) < 0xA0) else ch for ch in s) for s in lines]
    r = subprocess.run([str(build()), "translit"], input="\n".join(clean) + "\n", capture_output=True, text=True, check=True)
    out = r.stdout.split("\n")
    return out[:len(clean)]


def parse(leaf, path):
    r = subprocess.run([str(build()), "parse", leaf, str(path)], capture_output=True, text=True)
    return r.returncode, r.stdout.strip()

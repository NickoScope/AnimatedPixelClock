#!/usr/bin/env python3
"""Host test of the presence radar's rules.

src/presence/presence_model.h and presence_parse.h are plain C++ (the second
one needs ArduinoJson, and nothing else), so they are compiled here with the
host compiler and checked against the contract in docs/16-presence-radar.md:
the payload the AppDaemon app publishes, the signed mm/s the scene has to see
as cm/s, the age at which a target stops being in the room, the smoothing that
must not outlive its data, and the ring the trail reads. No board, no Arduino.

These are the same numbers tools/luasim/presence_panel.py renders into the
previews. If the two disagree, the previews are not what the panel will do.

  python3 tools/presence/check_presence.py

Needs one PlatformIO build first, for ArduinoJson (tools/luasim/fx_parity.py
has the same requirement, for Adafruit GFX).
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def arduinojson():
    libs = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src"))
    if not libs:
        sys.exit("check_presence: ArduinoJson is not unpacked yet - run one "
                 "`pio run -e matrix-waveshare-rgb` first")
    return libs[0]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "presence_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/presence"),
                        "-I", str(arduinojson()),
                        str(HERE / "presence_host_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()

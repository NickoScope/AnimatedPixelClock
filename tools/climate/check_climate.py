#!/usr/bin/env python3
"""Host test of the onboard climate sensor's arithmetic.

src/climate/shtc3.h and src/climate/climate_model.h are plain C++, so they are
compiled here with the host compiler and checked against the numbers Sensirion
prints: the CRC examples of the SHTC3 datasheet's Table 16, the measurement of
its Figure 7 (63 %RH, 23.7 °C), the ID mask of Table 15, and the design guide's
"1 °C at 90 %RH is 5 %RH". No board, no Arduino.

  python3 tools/climate/check_climate.py
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "climate_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/climate"),
                        str(HERE / "climate_host_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Host test of the infrared remote's rules and its serial console.

src/ir/ir_map.h and src/ir/ir_console.h are plain C++ - no Arduino, no receiver
library, no ArduinoJson - so they are compiled here with the host compiler and
checked against the timings of Vishay application note 80071 rev 2.3 ("Data
Formats for IR Remote Control", THE NEC CODE: a held key repeats in a 108 ms
time slot): a button that stays down through one lost repeat and not two, a
repeat frame that extends only the slot still being held, detents that are
drained exactly once, a learned code that lives in one slot only, and every
comparison holding across the millis() wrap.

The console's grammar is tested here too, because that is the whole of the
test surface before a receiver is soldered: `ir ok` on the serial port has to
reach the same dispatch a decoded frame does.

  python3 tools/ir/check_ir.py
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "ir_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/ir"),
                        str(HERE / "ir_host_test.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()

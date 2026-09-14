#!/usr/bin/env python3
"""Host checks for the flight board's direct AeroAPI fetch.

1. The synthetic fixtures in tools/flightboard/samples/ are what
   gen_aero_fixture.py writes (nothing in them came from FlightAware).
2. Compiles src/flightboard/aero_transform.cpp with the project's own
   ArduinoJson and runs tools/flightboard/aero_host_test.cpp: time parsing,
   the queries, the city cleanup, ident checks, every board rule, the tracked
   flight pick, cadence and expiry, and fb_settings.h's validation and counters.
3. Requires both boards to be identical to aero_ref.py's, the Python port
   written from Home Assistant's automation, and the accent fold to match.

  python3 tools/flightboard/check_aero.py [--real <dir> <now epoch>]

--real runs the C++ transform alone over answers saved elsewhere (for example
Home Assistant's sensor attributes, wrapped as {"arrivals": [...]}) and prints
the boards; nothing from such a run is stored here.
"""
import glob
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import aero_ref  # noqa: E402
import gen_aero_fixture as gen  # noqa: E402


def main():
    libs = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/ArduinoJson/src")))
    if not libs:
        sys.exit("check_aero.py: build any env once so PlatformIO installs ArduinoJson")
    bad = subprocess.run([sys.executable, str(HERE / "gen_aero_fixture.py"), "--check"]).returncode != 0
    if bad:
        print("  fixtures are stale - run python3 tools/flightboard/gen_aero_fixture.py")

    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "aero_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/flightboard"),
                        "-I", libs[0], str(HERE / "aero_host_test.cpp"), str(ROOT / "src/flightboard/aero_transform.cpp"),
                        "-o", str(exe)], check=True)
        if "--real" in sys.argv:
            i = sys.argv.index("--real")
            r = subprocess.run([str(exe), sys.argv[i + 1], "real", sys.argv[i + 2]], capture_output=True, text=True)
            print(r.stdout, end="")
            sys.exit(r.returncode)
        r = subprocess.run([str(exe), str(HERE / "samples")], capture_output=True, text=True)

    boards, fold = {}, None
    for line in r.stdout.splitlines():
        if line.startswith("BOARD "):
            _, d, js = line.split(" ", 2)
            boards[d] = json.loads(js)
        elif line.startswith("FOLD "):
            fold = line[5:]
        else:
            print(" ", line)
    bad = bad or r.returncode != 0

    lists = {k: json.loads((HERE / "samples" / f"aero_{k}.json").read_text())
             for k in ("arrivals", "scheduled_arrivals", "departures", "scheduled_departures")}
    for d, past, nxt in (("arr", "arrivals", "scheduled_arrivals"), ("dep", "departures", "scheduled_departures")):
        py = aero_ref.board(lists[past][past], lists[nxt][nxt], d == "dep", gen.NOW)
        same = boards.get(d) == py
        print(f"  {d} board: {py['count']} rows, now_idx {py['now_idx']}; Python reference: {'identical' if same else 'DIFFERENT'}")
        if not same:
            print("    C++:   ", json.dumps(boards.get(d)))
            print("    Python:", json.dumps(py))
            bad = True
    want = aero_ref.clean_city("".join(chr(c) for c in range(0xC0, 0x180)))
    print(f"  accent fold over U+00C0..U+017F: {'identical' if fold == want else 'DIFFERENT'}")
    if fold != want:
        print("    C++:   ", fold)
        print("    Python:", want)
        bad = True
    print("\naero checks:", "FAILED" if bad else "passed")
    sys.exit(1 if bad else 0)


main()

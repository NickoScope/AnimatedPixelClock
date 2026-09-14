#!/usr/bin/env python3
"""Host checks for the rail board's direct Realtime Trains fetch.

1. Compiles src/railboard/rtt_transform.cpp with the project's own ArduinoJson
   and runs tools/railboard/direct_host_test.cpp: time parsing, the query, the
   token answer, and the transform against samples/rtt_location_small.json,
   with and without the parse filter.
2. Sizes: synthetic full-schema answers of 60, 120 and 240 services, compact
   and indented, through the same filter - body bytes and parse peak.
3. If tools/railboard/rtt_client.py is there, runs its Python port of the same
   transform on the small fixture and requires the identical lists.
4. Compiles and runs tools/railboard/settings_host_test.cpp: the portal's
   settings validation and the "due soon" rule (src/railboard/rb_settings.h).

  python3 tools/railboard/check_direct.py
"""
import glob
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import gen_rtt_fixture as gen  # noqa: E402


def main():
    libs = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/ArduinoJson/src")))
    if not libs:
        sys.exit("check_direct.py: build any env once so PlatformIO installs ArduinoJson")
    small = gen.SMALL
    if json.loads(small.read_text()) != gen.small():
        sys.exit(f"check_direct.py: {small.name} is stale - run python3 tools/railboard/gen_rtt_fixture.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        bigs = []
        for n in (60, 120, 240):
            for indent in (None, 2):
                p = tmp / f"big_{n}_{'pretty' if indent else 'compact'}.json"
                p.write_text(json.dumps(gen.big(n), indent=indent, separators=None if indent else (",", ":")))
                bigs.append(p)
        exe = tmp / "direct_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/railboard"),
                        "-I", libs[0], str(HERE / "direct_host_test.cpp"), str(ROOT / "src/railboard/rtt_transform.cpp"),
                        "-o", str(exe)], check=True)
        r = subprocess.run([str(exe), str(small), *map(str, bigs)], capture_output=True, text=True)
        lists = None
        for line in r.stdout.splitlines():
            if line.startswith("LISTS "):
                lists = json.loads(line[6:])
            elif line.startswith("SIZE "):
                f = dict(kv.split("=") for kv in line.split()[2:])
                print(f"  {pathlib.Path(line.split()[1]).name:24s} {int(f['bytes']):>8,} B  "
                      f"{f['services']:>4} services  parse peak {int(f['peak']):>7,} B  {f['ms']} ms")
            else:
                print(" ", line)
        bad = r.returncode != 0

        exe2 = tmp / "settings_host_test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/railboard"),
                        "-I", libs[0], str(HERE / "settings_host_test.cpp"), "-o", str(exe2)], check=True)
        r2 = subprocess.run([str(exe2)], capture_output=True, text=True)
        for line in r2.stdout.splitlines():
            print(" ", line)
        bad = bad or r2.returncode != 0

        client = HERE / "rtt_client.py"
        if client.exists() and lists is not None:
            spec = importlib.util.spec_from_file_location("rtt_client", client)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            py = mod.normalise(json.loads(small.read_text()), gen.NOW, "GLD")
            same = py == lists
            print(f"  Python port (rtt_client.normalise) on the small fixture: {'identical' if same else 'DIFFERENT'}")
            if not same:
                print("    C++:   ", json.dumps(lists, sort_keys=True))
                print("    Python:", json.dumps(py, sort_keys=True))
                bad = True
        elif lists is None:
            bad = True
    print("\ndirect checks:", "FAILED" if bad else "passed")
    sys.exit(1 if bad else 0)


main()

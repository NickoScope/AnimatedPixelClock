#!/usr/bin/env python3
"""Host test of the 3D effects' arithmetic and of every scene.

src/fx3d/*.h are plain C++ - no Arduino - so they are compiled here with the
host compiler and checked: the stereo rules of the colleagues' brief, section
10 (zero parallax at Z0, none at all with no baseline, near and far diverging
in opposite directions, no vertical parallax, clipping before the divide,
red-blue without green, a swap that swaps); the baseline that keeps every
scene inside its disparity budget; lines and points that keep their light and
their sub-pixel position; triangles that cover a mesh without holes or
overlaps; the CIE 1931 table compared with the HUB75 library's own file; and
every scene of the catalogue in mono, red-blue and red-cyan: no allocation in a
frame, the same frames from the same seed, still drawing after a month.

It builds twice, as C++11 (the firmware's gnu++11) and C++17, both under the
address and undefined-behaviour sanitizers, and once more with
-Wdouble-promotion: the ESP32-S3 has a single-precision FPU and emulates
double in software, so no float may silently become one.

  python3 tools/fx3d/check_fx3d.py
"""
import glob
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INC = ["-I", str(ROOT / "src/fx3d")]


def library_table():
    hits = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/ESP32 HUB75 LED MATRIX PANEL DMA Display/src/cie_luts.h")))
    return hits[0] if hits else "-"


JSC = pathlib.Path("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc")


def page_script(tmp):
    """The /fx3d page's script must parse: JavaScriptCore's checkSyntax, which
    reads without running. A broken snippet is checked first, so a checker that
    accepts everything cannot pass."""
    if not JSC.exists():
        print("page script NOT checked: no JavaScriptCore here")
        return True
    src = (ROOT / "src/fx3d/fx3d_page.h").read_text()
    page = re.search(r'R"FX3D\((.*)\)FX3D"', src, re.S).group(1)
    js = re.search(r"<script>(.*)</script>", page, re.S).group(1)
    (tmp / "bad.js").write_text("const a = {;\n")
    (tmp / "page.js").write_text(js)
    def parses(f):
        r = subprocess.run([str(JSC), "-e", f"try {{ checkSyntax('{f}'); print('ok'); }} catch (e) {{ print(e); }}"],
                           capture_output=True, text=True)
        return r.stdout.strip() == "ok", r.stdout.strip()
    bad, _ = parses(tmp / "bad.js")
    good, why = parses(tmp / "page.js")
    if bad or not good:
        print(f"page script: FAIL ({'the checker accepts anything' if bad else why})")
        return False
    print(f"page script parses ({len(page.encode())} B page)")
    return True


def main():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        # Float only: the model and every scene, with double promotion an error.
        probe = tmp / "probe.cpp"
        probe.write_text('#include "fx3d_catalog.h"\n#include "fx3d_present.h"\n'
                         'int main() { return fx3d::kCatalogCount > 0 ? 0 : 1; }\n')
        subprocess.run(["c++", "-std=c++11", "-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-Wshadow",
                        "-Wdouble-promotion", *INC, str(probe)], check=True)
        print("float only: no double promotion in src/fx3d")
        ok = page_script(tmp) and ok
        lib = library_table()
        for std in ("c++11", "c++17"):
            exe = tmp / f"fx3d_host_test_{std}"
            subprocess.run(["c++", f"-std={std}", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                            *INC, str(HERE / "fx3d_host_test.cpp"), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe), lib], capture_output=True, text=True)
            sys.stdout.write(f"[{std}] " + r.stdout)
            sys.stderr.write(r.stderr)
            ok = ok and r.returncode == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Host checks for the media player.

1. media_host_test.cpp against src/media/media_model.cpp: parsing and bounds,
   transliteration, progress extrapolation, the track key, command payloads and
   the parse peak of the largest payloads the app may send.
2. Every sample in tools/media/samples/ through the panel's parser: OK, or for
   a file named <leaf>_bad_<reason>.json, refused with exactly that reason.
3. The AppDaemon app against a fake Home Assistant and a fake broker
   (tools/media/appdaemon/test_matrix_media.py), and its payloads against the
   panel's parser.
4. The portal's script parses (JavaScriptCore's jsc, when this is a Mac).

  python3 tools/media/check_media.py
"""
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import probe  # noqa: E402

JSC = pathlib.Path("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc")


def main():
    bad = 0
    exe = probe.build()

    print("1. media model")
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout.rstrip())
    bad += r.returncode != 0

    print("2. samples through the panel's parser")
    for path in sorted((HERE / "samples").glob("*.json")):
        leaf = path.name.split("_", 1)[0]
        m = re.match(r"\w+?_bad_(\w+)\.json$", path.name)
        code, out = probe.parse(leaf, path)
        good = (code == 1 and out == "REFUSED " + m[1]) if m else code == 0
        print(f"  {path.name:34s} {out:30s} {'ok' if good else 'UNEXPECTED'}")
        bad += not good

    print("3. AppDaemon app")
    r = subprocess.run([sys.executable, str(HERE / "appdaemon/test_matrix_media.py")], capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().splitlines()
    print("\n".join("  " + line for line in tail[-6:]))
    bad += r.returncode != 0

    print("4. portal script")
    src = (ROOT / "src/web/web_panel_js.h").read_text()
    m = re.search(r'R"(\w*)\((.*)\)\1"', src, re.S)
    if not JSC.exists():
        print("  jsc not found: not checked")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            js = pathlib.Path(tmp, "panel.js")
            js.write_text(m[2])
            r = subprocess.run([str(JSC), "-e", 'try { new Function(read("%s")); print("parses"); } catch (e) { print("FAILS " + e); }' % js],
                               capture_output=True, text=True)
        print(f"  web_panel_js.h, {len(m[2]):,} B: {r.stdout.strip()}")
        bad += r.stdout.strip() != "parses"

    print(f"\n{'all passed' if not bad else str(bad) + ' FAILED'}")
    sys.exit(1 if bad else 0)


main()

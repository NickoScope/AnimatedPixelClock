#!/usr/bin/env python3
"""Push the firmware's layout into the browser simulation in the other repo.

The simulation lives in the knowledge-base repo (NickoScope/waveshare-rgb-matrix-
p2-64x64) but its numbers belong to the firmware. This writes them across and
stamps the layout digest into the page so tools/fb_check.py can tell whether the
two repos still agree.

  python3 tools/fb_sim_build.py [path-to-knowledge-base-repo]
  FB_KB_REPO=/path/to/repo python3 tools/fb_sim_build.py
"""
import json, os, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fb_layout

FORK = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_KB = FORK.parent / "LED-MATRIX APOLLO"

BEGIN = "/* BEGIN GENERATED LAYOUT - tools/fb_sim_build.py in the firmware repo */"
END   = "/* END GENERATED LAYOUT */"

def main():
    kb = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.environ.get("FB_KB_REPO", DEFAULT_KB))
    page = kb / "sim/flightboard-sim.html"
    if not page.exists():
        raise SystemExit(f"fb_sim_build: no simulation at {page}\n"
                         f"  pass the knowledge-base repo path, or set FB_KB_REPO")
    L = fb_layout.load()

    (kb / "sim/flightboard-layout.json").write_text(
        json.dumps(L, indent=2, sort_keys=True) + "\n")

    block = (f"{BEGIN}\n"
             f"const LAYOUT={json.dumps(L, separators=(',', ':'), sort_keys=True)};\n"
             f"{END}")
    t = page.read_text()
    if BEGIN in t:
        t = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), block, t, flags=re.S)
    else:                                   # first run: insert ahead of the data
        t = t.replace("const BOARDS=", block + "\nconst BOARDS=", 1)
    page.write_text(t)
    print(f"fb_sim_build: {page}")
    print(f"  layout digest {L['digest']}  (font {L['font']})")

main()

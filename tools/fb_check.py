#!/usr/bin/env python3
"""Fail if the flight board layout has drifted between the two repos.

The firmware is the source of truth. This compares what it says now against
what every consumer currently carries, and names anything stale.

  python3 tools/fb_check.py [path-to-knowledge-base-repo]

Exit 0 = in step, 1 = drift (with the fix printed).
"""
import json, os, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fb_layout

FORK = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_KB = FORK.parent / "LED-MATRIX APOLLO"

def main():
    kb = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.environ.get("FB_KB_REPO", DEFAULT_KB))
    L = fb_layout.load()
    want = L["digest"]
    print(f"firmware  src/flightboard/flightboard.cpp   digest {want}")

    bad = []
    # 1. the host renderer reads fb_layout directly, so it can only be stale if
    #    somebody reintroduced literals; check that it holds no bare constants.
    rend = (FORK / "tools/fb_render.py").read_text()
    if re.search(r"^\s*(X_DEST|X_FLIGHT|GAP|CODE_GAP)\s*=\s*\d", rend, re.M):
        bad.append("tools/fb_render.py has hard-coded layout constants again")
    else:
        print("consumer  tools/fb_render.py                  reads fb_layout - always in step")

    # 2. the browser simulation in the knowledge-base repo carries a stamped copy
    page = kb / "sim/flightboard-sim.html"
    if not page.exists():
        bad.append(f"simulation not found at {page}")
    else:
        m = re.search(r'const LAYOUT=(\{.*?\});', page.read_text(), re.S)
        if not m:
            bad.append(f"{page} carries no generated LAYOUT block")
        else:
            got = json.loads(m.group(1)).get("digest")
            state = "in step" if got == want else f"STALE (has {got})"
            print(f"consumer  {page.name:35s} {state}")
            if got != want: bad.append(f"{page} is stale")

    js = kb / "sim/flightboard-layout.json"
    if js.exists():
        got = json.loads(js.read_text()).get("digest")
        print(f"consumer  {js.name:35s} " + ("in step" if got == want else f"STALE (has {got})"))
        if got != want: bad.append(f"{js} is stale")

    if bad:
        print("\nDRIFT:")
        for b in bad: print("  -", b)
        print("\nfix:  python3 tools/fb_sim_build.py")
        sys.exit(1)
    print("\nall consumers in step with the firmware.")

main()

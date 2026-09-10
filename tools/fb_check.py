#!/usr/bin/env python3
"""Fail if anything has drifted from the layout in flightboard.cpp.

  python3 tools/fb_check.py      # exit 1 on drift, with the fix printed
"""
import json, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fb_layout

ROOT = pathlib.Path(__file__).resolve().parent.parent
L, bad = fb_layout.load(), []
want = L["digest"]
print(f"firmware  src/flightboard/flightboard.cpp   digest {want}")

if re.search(r"^\s*(X_DEST|X_FLIGHT|GAP|CODE_GAP)\s*=\s*\d",
             (ROOT / "tools/fb_render.py").read_text(), re.M):
    bad.append("tools/fb_render.py has hard-coded layout constants again")
else:
    print("consumer  tools/fb_render.py            reads fb_layout - always in step")

for rel, pat in (("sim/flightboard-sim.html", r"const LAYOUT=(\{.*?\});"),
                 ("sim/flightboard-layout.json", None)):
    f = ROOT / rel
    if not f.exists():
        bad.append(f"{rel} is missing"); continue
    raw = f.read_text()
    m = re.search(pat, raw, re.S) if pat else None
    try:
        got = json.loads(m.group(1) if m else raw).get("digest")
    except Exception:
        got = None
    print(f"consumer  {rel:29s} " + ("in step" if got == want else f"STALE (has {got})"))
    if got != want: bad.append(f"{rel} is stale")

if bad:
    print("\nDRIFT:")
    for b in bad: print("  -", b)
    print("\nfix:  python3 tools/fb_sim_build.py")
    sys.exit(1)
print("\nall in step.")

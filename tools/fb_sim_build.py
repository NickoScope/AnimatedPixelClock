#!/usr/bin/env python3
"""Regenerate the layout block in sim/flightboard-sim.html from the firmware.

  python3 tools/fb_sim_build.py
"""
import json, pathlib, re, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fb_layout

ROOT  = pathlib.Path(__file__).resolve().parent.parent
PAGE  = ROOT / "sim/flightboard-sim.html"
BEGIN = "/* BEGIN GENERATED LAYOUT - tools/fb_sim_build.py */"
END   = "/* END GENERATED LAYOUT */"

L = fb_layout.load()
(ROOT / "sim/flightboard-layout.json").write_text(json.dumps(L, indent=2, sort_keys=True) + "\n")
block = f"{BEGIN}\nconst LAYOUT={json.dumps(L, separators=(',', ':'), sort_keys=True)};\n{END}"
t = PAGE.read_text()
t = (re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), block, t, flags=re.S)
     if BEGIN in t else
     re.sub(r"/\* BEGIN GENERATED LAYOUT.*?/\* END GENERATED LAYOUT \*/", block, t, flags=re.S))
PAGE.write_text(t)
print(f"fb_sim_build: sim/flightboard-sim.html   digest {L['digest']}  font {L['font']}")

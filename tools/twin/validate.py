#!/usr/bin/env python3
"""validate.py CPI - check a timing configuration on measurements calibrate.py did not use.

Held out from the fit: KINETIC DIGITS LED as committed in 372ce25 (dots cube and the ball with
lasers, measured on the panel with kd_ball.py at 13:59 on 2026-09-29: draw avg 24.9 and 52.4 ms)
and OCEANARIUM (37-47 ms a frame on the panel, the figure in the v2.7.3 notes).
"""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate as C

cpi = sys.argv[1] if len(sys.argv) > 1 else "2.45"
out = os.path.join(C.HOME, "calibration")
os.makedirs(out, exist_ok=True)
C.flags = lambda name: ["--cpi", cpi]
t = C.Twin(f"validate-cpi{cpi}", 18190, out)
res = {"cpi": cpi}
try:
    t.wait_up()
    t.post("/api/panel", {"carousel": {"enabled": False}})
    t.post("/api/panel", {"show": {"page": 0}}); time.sleep(2)
    print(t.upload(os.path.join(C.HOME, "research", "kinetic_372ce25.lua"), "KINETIC_DIGITS_LED")[:120], flush=True)
    effects = json.loads(t.get("/api/lua"))["effects"]
    kd = next(i for i, e in enumerate(effects) if "KINETIC" in e)
    oc = next(i for i, e in enumerate(effects) if "OCEANARIUM" in e)
    t.post("/api/lua", {"show": oc})
    res["oceanarium"] = t.wait_reports("OCEANARIUM", len(t.reports()))
    print("OCEANARIUM", res["oceanarium"], flush=True)
    t.post("/api/panel", {"show": {"page": 0}}); time.sleep(2)
    t.post("/api/lua", {"show": kd}); time.sleep(3)
    for _ in range(8):                       # kd_ball.py: eight presses, then the two scenes
        t.post("/api/lua", {"click": True}); time.sleep(1.2)
    for scene in ("dots cube", "dots ball"):
        t.post("/api/lua", {"click": True}); time.sleep(1)
        res[scene] = t.wait_reports(effects[kd], len(t.reports()))
        print(scene, res[scene], flush=True)
finally:
    json.dump(res, open(os.path.join(out, f"validate-cpi{cpi}.json"), "w"), indent=1)
    t.stop()

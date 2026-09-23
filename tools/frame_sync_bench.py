#!/usr/bin/env python3
"""Measure, on the panel, how the frame copy is timed to the scan.

Firmware with HUB75_FRAME_IN_PSRAM has one DMA frame and copies the drawn
frame into it at each flip (src/display/matrix_display.h). /api/frame switches
the timing at run time and counts, from the DMA's own position, the frames
that showed a pass half old and half new. This runs every mode on the same
pages for the same time, with the carousel held off so the page does not
change under the measurement, and puts the carousel back afterwards.

    python3 tools/frame_sync_bench.py 192.168.4.89
    python3 tools/frame_sync_bench.py 192.168.4.89 --pages 7,3 --seconds 30

Modes: off (the negative control: the probe must see mixed passes), follow
(write each changed row once the scan has passed it), ahead (wait for the last
row, then write ahead of the scan).
"""
import argparse, json, sys, time, urllib.request

MODES = {"off": 0, "follow": 1, "ahead": 2}


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=10) as r:
        return json.loads(r.read())


def post(base, path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read() or b"{}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address")
    ap.add_argument("--pages", default="7,3,5,2", help="page numbers, comma separated")
    ap.add_argument("--modes", default="off,follow,ahead")
    ap.add_argument("--seconds", type=int, default=30)
    a = ap.parse_args()
    base = "http://" + a.address
    panel = get(base, "/api/panel")
    names = {p["i"]: p["name"] for p in panel["pages"]}
    was_on = panel["carousel"]["enabled"]
    was_page = panel["now"]["page"]
    rows = []
    try:
        post(base, "/api/panel", {"carousel": {"enabled": False}})
        for page in (int(x) for x in a.pages.split(",")):
            post(base, "/api/panel", {"show": {"page": page}})
            time.sleep(3)
            for mode in a.modes.split(","):
                get(base, f"/api/frame?sync={MODES[mode]}")
                time.sleep(a.seconds)
                f = get(base, "/api/frame")
                rows.append((page, names.get(page, "?"), mode, f))
                print(f"{page:>2} {names.get(page, '?'):<15} {mode:<6} "
                      f"flips/s {f['flipsPerSecond']:>5}  changed {f['changedFrames']:>5}  "
                      f"mixed {f['mixedFrames']:>5}  in-flight rows {f['rowsRewrittenWhileShown']:>5}  "
                      f"copy avg/max {f['copyAvgUs']:>5}/{f['copyMaxUs']:>5} us  "
                      f"wait avg/max {f['waitAvgUs']:>5}/{f['waitMaxUs']:>5} us", flush=True)
    finally:
        get(base, "/api/frame?sync=1")
        post(base, "/api/panel", {"carousel": {"enabled": was_on}})
        if not was_on:
            post(base, "/api/panel", {"show": {"page": was_page}})
    bad = [r for r in rows if r[2] != "off" and r[3]["mixedFrames"]]
    ctl = [r for r in rows if r[2] == "off" and not r[3]["mixedFrames"] and r[3]["changedFrames"]]
    if ctl:
        print("NEGATIVE CONTROL FAILED: sync off showed no mixed frame on", [r[1] for r in ctl])
    print("mixed with sync on:", "none" if not bad else ", ".join(f"{r[1]}/{r[2]} {r[3]['mixedFrames']}" for r in bad))
    return 1 if ctl else 0


if __name__ == "__main__":
    sys.exit(main())

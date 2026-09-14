#!/usr/bin/env python3
"""Render the flight board with a pinned tracked flight, from the synthetic fixtures.

The boards come from aero_ref.py, which check_aero.py holds identical to the
C++ transform; the layout, fonts and pinned-row colours from
src/flightboard/flightboard.cpp through tools/fb_render.py. The tracked row's
word and colour mirror trackWord() in flightboard.cpp by hand - that function
needs the display, so it is not compiled here. Times are shown in
Europe/Paris, the zone the panel is set to.

  python3 tools/flightboard/previews.py      # PNGs into tools/flightboard/preview/
"""
import datetime as dt
import json
import pathlib
import subprocess
import sys
from zoneinfo import ZoneInfo

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import aero_ref  # noqa: E402
import gen_aero_fixture as gen  # noqa: E402

OUT = HERE / "preview"
ZONE = ZoneInfo("Europe/Paris")
GREY, WHITE, CYAN, AMBER, BLUE, GREEN, RED, MAGENTA = (
    (120, 132, 138), (210, 210, 210), (0, 220, 220), (255, 170, 0), (70, 140, 255), (0, 200, 60), (255, 40, 40),
    (255, 60, 200))


def hm(utc):
    return dt.datetime.fromtimestamp(utc, ZONE).strftime("%H:%M") if utc else "--:--"


def word(state, delay, gate=None, dep_in_min=None):
    """flightboard.cpp trackWord(), for the states the scenes use."""
    if state == "SCHED":
        if gate and dep_in_min is not None and dep_in_min <= 60:
            return f"GATE {gate}", CYAN
        return "ON TIME", WHITE
    if state == "DELAYED":
        return f"DELAY {delay}", AMBER
    if state == "ENROUTE":
        return (f"LATE {delay}", AMBER) if delay >= 15 else ("IN AIR", BLUE)
    return {"WAIT": ("...", GREY), "NOTFOUND": ("NO FLIGHT", GREY), "TAXI": ("TAXI", CYAN),
            "LANDED": ("LANDED", GREEN), "CANCELLED": ("CANX", RED), "DIVERTED": ("DIVERT", MAGENTA)}[state]


def board(direction):
    past, nxt = ("departures", "scheduled_departures") if direction == "dep" else ("arrivals", "scheduled_arrivals")
    lists = {k: json.loads((HERE / "samples" / f"aero_{k}.json").read_text())[k] for k in (past, nxt)}
    b = aero_ref.board(lists[past], lists[nxt], direction == "dep", gen.NOW)
    rows = [dict(r, tm=hm(r["t"])) for r in b["rows"]]
    # flightboardIngest's clamp: a now_idx past the end points at the last row.
    now = b["now_idx"] if b["now_idx"] < len(rows) or not rows else len(rows) - 1
    return {"apt": "LFMN", "dir": direction, "upd": hm(gen.NOW), "now_idx": now, "f": rows}


def tracked(case, **over):
    e = json.loads((HERE / "samples" / "aero_tracks.json").read_text())["cases"][case]["expect"]
    e = dict(e, **over)
    w, col = word(e["state"], e["delay_min"], e.get("gate"), e.get("dep_in_min"))
    return {"tm": hm(e["shown"]), "fn": e.get("fn_shown", e["fn"]), "route": f'{e["frm"]}-{e["to"]}', "w": w,
            "col": list(col)}


SCENES = {
    "arrivals_pinned_enroute": (board("arr"), tracked("enroute")),
    "departures_pinned_delayed": (board("dep"), tracked("delayed")),
    "departures_pinned_gate": (board("dep"), tracked("ahead_6h", state="SCHED", shown=gen.NOW + 35 * 60, gate="B12",
                                                     dep_in_min=35)),
    "arrivals_pinned_landed": (board("arr"), tracked("landed")),
    "arrivals_pinned_late": (board("arr"), tracked("enroute", delay_min=25)),
    "departures_pinned_cancelled": (board("dep"), tracked("cancelled")),
    "departures_pinned_diverted": (board("dep"), tracked("diverted")),
    # The widest case: an ICAO ident and a long word leave no room for the route's origin.
    "departures_pinned_icao_ident": (board("dep"), tracked("delayed", fn_shown="AFR7301", delay_min=125)),
    "custom_airport_header": (dict(board("arr"), apt="KJFK", name="NEW YORK"), tracked("taxi")),
    "no_tracker_for_comparison": (board("arr"), None),
}


def main():
    OUT.mkdir(exist_ok=True)
    for name, (payload, track) in SCENES.items():
        p = dict(payload)
        if track:
            p["track"] = track
        src = OUT / f"{name}.json"
        src.write_text(json.dumps(p, indent=1) + "\n")
        subprocess.run([sys.executable, str(ROOT / "tools/fb_render.py"), str(src), str(OUT / f"{name}.png"), "8"],
                       check=True)
        src.unlink()


if __name__ == "__main__":
    main()

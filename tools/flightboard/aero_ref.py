#!/usr/bin/env python3
"""Reference port, in Python, of the flight board rules in src/flightboard/aero_transform.cpp.

Written from Home Assistant's automation flightboard_serve v3.7 (its payload
template, read through the HA MCP on 2026-09-14), not from the C++, so the two
ports check each other. Where the C++ departs from the automation on purpose
the same departure is here, marked DIFF:

  DIFF ct   IATA, else ICAO, else code. The automation's `default()` does not
            replace a null code_iata, so it published "ct": null.
  DIFF cy   accents folded and undrawable characters dropped; the automation
            only upper-cased.
  DIFF land actual_in or actual_on. The automation read actual_in only, so a
            flight on the runway, not yet at its gate, showed IN AIR.
  DIFF fid  without fa_flight_id: ident|(scheduled_out or scheduled_in). Jinja
            would write "None" for a null time; only the key's uniqueness matters.

  python3 tools/flightboard/aero_ref.py <past.json> <next.json> arr|dep <now epoch>
"""
import datetime as dt
import json
import re
import sys
import unicodedata

WINDOW, DELAY, HALF, KEEP = 7200, 900, 7, 15
FN_LEN, CT_LEN, CY_LEN = 9, 5, 15     # fb_model.h, less the NUL

# Letters NFD does not take apart, as aero_transform.cpp's tables spell them.
SINGLE = {"Æ": "A", "æ": "A", "Ð": "D", "ð": "D", "Ø": "O", "ø": "O", "Þ": "T", "þ": "T", "ß": "S",
          "×": " ", "÷": " ", "Đ": "D", "đ": "D", "Ħ": "H", "ħ": "H", "ı": "I", "Ĳ": "J", "ĳ": "J",
          "ĸ": "K", "Ŀ": "L", "ŀ": "L", "Ł": "L", "ł": "L", "ŉ": "N", "Ŋ": "N", "ŋ": "N", "Œ": "O",
          "œ": "O", "Ŧ": "T", "ŧ": "T", "ſ": "S"}


def ts(s):
    if not isinstance(s, str) or not re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?(Z|[+-]\d\d:?\d\d)$", s):
        return None
    return int(dt.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp())


def clean_city(raw):
    s = (raw or "").split("(")[0].split("/")[0]
    out = []
    for ch in s:
        if ord(ch) < 0x80:
            c = ch.upper()
        elif 0xC0 <= ord(ch) <= 0x17F:
            c = SINGLE.get(ch) or unicodedata.normalize("NFD", ch)[0].upper()
        else:
            c = " "
        out.append(c if re.match(r"[A-Z0-9 .\-']", c) else " ")
    return re.sub(" +", " ", "".join(out)).strip()


def code(apt):
    apt = apt or {}
    return apt.get("code_iata") or apt.get("code_icao") or apt.get("code") or ""


def status(f):
    so, eo, si, ei = (ts(f.get(k)) for k in ("scheduled_out", "estimated_out", "scheduled_in", "estimated_in"))
    if f.get("cancelled"):
        return "canc"
    if f.get("actual_in") or f.get("actual_on"):     # DIFF land: the automation read actual_in only
        return "land"
    if f.get("actual_off"):
        return "dep"
    if f.get("actual_out"):
        return "board"
    if (so and eo and eo - so > DELAY) or (si and ei and ei - si > DELAY):
        return "delay"
    return "sched"


def board(past, nxt, dep, now):
    lo, hi = now - WINDOW, now + WINDOW
    seen, paired = [], []
    for f in (past or []) + (nxt or []):
        fid = f.get("fa_flight_id") or ((f.get("ident") or "") + "|" + (f.get("scheduled_out") or f.get("scheduled_in") or ""))
        if fid in seen:
            continue
        keys = ("actual_out", "estimated_out", "scheduled_out") if dep else ("actual_in", "estimated_in", "scheduled_in")
        raw = next((f[k] for k in keys if f.get(k) and ts(f[k])), None)
        if not raw:
            continue
        a = ts(raw)
        fn = f.get("ident_iata") or f.get("ident_icao") or f.get("ident")
        if lo <= a <= hi and fn:
            seen.append(fid)
            paired.append((f, a))
    paired.sort(key=lambda p: p[1])
    nidx = next((i for i, p in enumerate(paired) if p[1] >= now), len(paired))
    items = []
    for f, a in paired:
        other = f.get("destination") if dep else f.get("origin")
        items.append({"fn": (f.get("ident_iata") or f.get("ident_icao") or f.get("ident"))[:FN_LEN - 1],
                      "ct": code(other)[:CT_LEN - 1],
                      "cy": clean_city((other or {}).get("city"))[:CY_LEN - 1].rstrip(),
                      "st": status(f), "t": a})
    start = max(nidx - HALF, 0)
    capped = items[start:start + KEEP]
    return {"count": len(capped), "now_idx": min(nidx - start, len(capped)), "rows": capped}


if __name__ == "__main__":
    past, nxt = (json.load(open(p)) for p in sys.argv[1:3])
    dep = sys.argv[3] == "dep"
    key = ("departures", "scheduled_departures") if dep else ("arrivals", "scheduled_arrivals")
    print(json.dumps(board(past[key[0]], nxt[key[1]], dep, int(sys.argv[4])), indent=1))

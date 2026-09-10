#!/usr/bin/env python3
"""Single source of truth for the flight board layout.

src/flightboard/flightboard.cpp is the ONLY place these numbers are written by
hand. Everything else - the host renderer, the browser simulation in the
knowledge-base repo - reads them from here, so the three cannot drift apart
without the check script noticing.

  python3 tools/fb_layout.py            # human-readable dump
  python3 tools/fb_layout.py --json     # machine-readable, what consumers use
"""
import hashlib, json, pathlib, re, subprocess, sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src/flightboard/flightboard.cpp"

def _ints(t):
    out = {}
    for m in re.finditer(r"static const (?:u?int\d+_t)\s+(FB_[A-Z0-9_]+)\s*=\s*(-?\d+)\s*;", t):
        out[m.group(1)] = int(m.group(2))
    return out

def _status(t):
    body = re.search(r"static const char \*statusWord\(.*?\n\}", t, re.S)
    if not body: raise SystemExit("fb_layout: statusWord() not found")
    arr, dep = {}, {}
    for m in re.finditer(r'case\s+FB_([A-Z]+):\s*return\s+(.*?);', body.group(0)):
        key, expr = m.group(1).lower(), m.group(2).strip()
        tern = re.match(r'departures\s*\?\s*"([^"]*)"\s*:\s*"([^"]*)"', expr)
        if tern:   dep[key], arr[key] = tern.group(1), tern.group(2)
        elif expr.startswith('"'): dep[key] = arr[key] = expr.strip('"')
    return arr, dep

def _strlist(t, name):
    m = re.search(rf"{name}\[\]\s*=\s*\{{(.*?)\}};", t, re.S)
    return re.findall(r'"([^"]*)"', m.group(1)) if m else []

def load():
    t = SRC.read_text()
    c = _ints(t)
    arr, dep = _status(t)
    font = re.search(r"display\.setFont\(&(\w+)\)", t)
    lay = {
        "_source": "src/flightboard/flightboard.cpp",
        "font": font.group(1) if font else None,
        "geom": {k: c[k] for k in sorted(c)},
        "status": {"arr": arr, "dep": dep},
        "airports": _strlist(t, "FB_AIRPORTS"),
        "airport_names": _strlist(t, "FB_AIRPORT_NAMES"),
    }
    lay["digest"] = hashlib.sha256(
        json.dumps({k: v for k, v in lay.items() if k != "_source"},
                   sort_keys=True, separators=(",", ":")).encode()).hexdigest()[:16]
    # Provenance, deliberately OUTSIDE the digest: it answers "which firmware
    # state do these docs describe", which is a different question from "has the
    # layout drifted". Folding it in would make every commit look like drift.
    lay["firmware"] = _provenance()
    return lay

def _provenance():
    def git(*a):
        try:
            return subprocess.run(("git",) + a, cwd=SRC.parent, capture_output=True,
                                  text=True, check=True).stdout.strip()
        except Exception:
            return None
    dirty = git("status", "--porcelain", "--", str(SRC))
    return {
        "repo":   git("remote", "get-url", "origin"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": git("rev-parse", "HEAD"),
        # True when flightboard.cpp had uncommitted edits at generation time, so
        # "commit" names the last committed state rather than what was read.
        "dirty":  bool(dirty),
    }

if __name__ == "__main__":
    L = load()
    if "--json" in sys.argv:
        print(json.dumps(L, indent=2, sort_keys=True))
    else:
        print(f"font {L['font']}   digest {L['digest']}")
        for k, v in L["geom"].items(): print(f"  {k:16s} {v}")
        for d in ("arr", "dep"):
            print(f"  status[{d}]  " + "  ".join(f"{k}={v!r}" for k, v in L["status"][d].items()))
        print("  airports      " + ", ".join(f"{a}={n}" for a, n in
              zip(L["airports"], L["airport_names"])))

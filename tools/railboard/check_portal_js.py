#!/usr/bin/env python3
"""renderRb() actually runs, in both configurations. src/web/web_panel_js.h.

Written because it did not. On 2026-09-20 a line read `dx.token` two lines
above `var dx = ...`: `var` hoists the declaration but not the assignment, so
on a panel that fetches directly and has never heard from Home Assistant -
exactly the configuration the line was written for - renderRb threw a
TypeError and the whole rail card stopped updating. A parse check cannot see
that; only running it can. The flight board has had this check for a while
(tools/flightboard/check_portal_js.py); the rail board had seven checks and
none of them touched the portal.
"""
import json, pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = (ROOT / "src/web/web_panel_js.h").read_text()

m = re.search(r"function renderRb\(d\)\s*\{", SRC)
if not m:
    sys.exit("check_portal_js: renderRb not found in web_panel_js.h")
depth, i = 0, m.end() - 1
while i < len(SRC):
    if SRC[i] == "{": depth += 1
    elif SRC[i] == "}":
        depth -= 1
        if depth == 0: break
    i += 1
body = SRC[m.start():i + 1]

# Two shapes, both real: a panel with its own token and no Home Assistant, and
# one that has heard from Home Assistant.
CASES = {
    "direct, Home Assistant never seen": {
        "ha": {"have": False}, "source": "direct",
        "direct": {"built": True, "token": True, "kind": "refresh-exchanged", "validUntil": 0,
                   "state": "OK", "http": 200, "fetching": False, "fetchedAgo": 26, "nextIn": 3,
                   "left": 8948, "limit": 9000, "interval": 120},
    },
    "Home Assistant reported an error": {
        "ha": {"have": True, "err": "AUTH", "code": 401, "retry": 0, "left": None}, "source": "ha",
        "direct": {"built": True, "token": False, "state": "WAITING"},
    },
    "no token at all": {
        "ha": {"have": False}, "source": "none",
        "direct": {"built": False, "token": False, "state": "WAITING"},
    },
}
COMMON = {"dep": {"have": True, "stale": False, "src": "direct"}, "arr": {"have": True, "stale": False, "src": "direct"},
          "mqtt": {"connected": True, "configured": True, "status": "ok"}, "refused": 0,
          "cfg": {"from": "web", "rows": 8}, "now": 1789915378, "synced": True, "diag": False,
          "station": "Guildford", "named": True, "crs": "GLD", "list": "dep", "showing": True,
          "select": {"topic": "nickoscope_matrix/railboard/select", "sent": True},
          "knob": "auto", "diagCfg": False, "haShadowed": 0, "jsonPeak": 1124}

HARNESS = """
var out = {};
function setText(id, v) { out[id] = String(v); }
function $(id) { return null; }
function londonTime(t) { return "00:00:00"; }
function ago(s) { return s + " s ago"; }
function listLine(o) { return "5 services"; }
function focused(el) { return false; }
function renderRbSettings(d) {}
var rbLast = null, KIND = { "refresh-exchanged": "refresh token, exchanged" };
// The page's other helpers, stubbed to the least that lets renderRb run. Each
// one was added because the run said it was missing - that is the point of
// running it rather than parsing it.
function pageIdx(key) { return 9; }
function $$(sel) { return []; }
function esc(s) { return String(s); }
function setVal(id, v) { out[id] = String(v); }
function show(id, on) {}
function num(v) { return Number(v) || 0; }
function renderPresets() { return null; }
%s
var cases = %s;
var failed = 0;
for (var name in cases) {
  try { renderRb(cases[name]); }
  catch (e) { failed++; print("FAIL " + name + ": " + e); continue; }
  if (!out.rbHa) { failed++; print("FAIL " + name + ": rbHa was never set"); }
}
print(Object.keys(cases).length + " cases, " + failed + " failed");
if (failed) quit(1);
"""

cases = {k: dict(COMMON, **v) for k, v in CASES.items()}
script = HARNESS % (body, json.dumps(cases))
jsc = "/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
if not pathlib.Path(jsc).exists():
    sys.exit("check_portal_js: no jsc on this host")
p = subprocess.run([jsc, "-e", script], capture_output=True, text=True)
print(p.stdout.strip() or p.stderr.strip())
sys.exit(p.returncode)

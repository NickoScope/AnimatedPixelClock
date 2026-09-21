#!/usr/bin/env python3
"""The no-Home-Assistant branch of bringup.py, exercised.

That branch decides what a panel should stop showing when there is no broker
behind it, and it could not be tested on the panel we have: that one's broker
works, which is the other branch. So `panel_stub.py` serves a real panel's own
captured answers - taken off the hardware, then scrubbed of anything that names
a network, a person or a place - with only the facts under test changed.

The stub follows the firmware where it matters: `enable` is one bit per KEY,
not per page (src/panel/panel.cpp:414), so switching off `market` switches off
all four market pages at once.

    python3 tools/agent/tests/test_bringup_no_ha.py

Exits 0 when every case holds, 1 on the first that does not.
"""

import json
import pathlib
import subprocess
import sys
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
BRINGUP = HERE.parent / "bringup.py"
STUB = HERE / "panel_stub.py"
PY = sys.executable
PORT = 8791


def start(scenario):
    p = subprocess.Popen([PY, str(STUB), str(PORT), scenario],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{PORT}/api/info", timeout=0.5).read()
            return p
        except Exception:  # noqa: BLE001
            time.sleep(0.1)
    p.kill()
    raise RuntimeError("the stub never answered")


def bringup(*args):
    r = subprocess.run([PY, str(BRINGUP), "--host", f"127.0.0.1:{PORT}", "--json", *args],
                       capture_output=True, text=True, timeout=60)
    return json.loads(r.stdout or "{}"), r.returncode


def pages_off():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/api/panel", timeout=5) as r:
        d = json.loads(r.read())
    return {p["name"] for p in d["pages"] if not p["on"]}


FAILURES = []


def check(name, got, want):
    if got == want:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}\n          got  {got!r}\n          want {want!r}")
        FAILURES.append(name)


def case(scenario, title, fn):
    print(f"\n{title}")
    p = start(scenario)
    try:
        fn()
    finally:
        p.terminate()
        p.wait(timeout=5)


def main():
    def no_broker():
        out, code = bringup("--name", "TestPanel")
        check("exit 0", code, 0)
        acts = " | ".join(out["result"]["actions"])
        # Four pages share the `market` key. Naming one of them - which is what
        # a dict keyed by page key silently does - tells a person three lies.
        for nm in ("MARKETS", "TICKER", "PORTFOLIO", "HOLDINGS"):
            check(f"{nm} named in the report", nm in acts, True)
        check("the panel really switched them off",
              {"MARKETS", "TICKER", "PORTFOLIO", "HOLDINGS", "MEDIA", "FLIGHTS",
               "TRAINS"} <= pages_off(), True)
        # Running it twice must not report work it did not do.
        out2, code2 = bringup("--name", "TestPanel")
        check("second run is quiet", code2, 0)
        check("second run switches nothing off",
              any("выключена" in a for a in out2["result"]["actions"]), False)

    def stale_broker():
        out, code = bringup("--name", "TestPanel")
        # A configured broker that is merely down is NOT a panel without Home
        # Assistant, and four pages must not go dark over a restarting broker.
        check("refuses to decide", code, 1)
        check("nothing was switched off", pages_off() & {"MARKETS", "MEDIA"}, set())
        check("says why", any("НЕ тронуты" in a for a in out["result"]["actions"]), True)
        out2, code2 = bringup("--name", "TestPanel", "--force-no-ha")
        check("--force-no-ha acts", code2, 0)
        check("and then they are off",
              {"MARKETS", "TICKER", "PORTFOLIO", "HOLDINGS", "MEDIA"} <= pages_off(), True)

    def direct_keys():
        out, code = bringup("--name", "TestPanel")
        check("exit 0", code, 0)
        off = pages_off()
        # Each has an API key of its own, so neither depends on the broker.
        check("FLIGHTS left alone", "FLIGHTS" in off, False)
        check("TRAINS left alone", "TRAINS" in off, False)
        check("the MQTT-only ones still go", {"MARKETS", "MEDIA"} <= off, True)

    def connected():
        out, code = bringup("--name", "TestPanel")
        check("exit 0", code, 0)
        check("nothing switched off", pages_off() & {"MARKETS", "MEDIA", "FLIGHTS"}, set())
        check("says so", any("nothing to switch off" in a
                             for a in out["result"]["actions"]), True)

    def keep_ha():
        out, code = bringup("--name", "TestPanel", "--keep-ha")
        check("exit 0", code, 0)
        check("nothing switched off", pages_off() & {"MARKETS", "MEDIA"}, set())

    case("none", "no broker at all", no_broker)
    case("stale", "a broker that is configured but down", stale_broker)
    case("direct", "no broker, but flights and trains have their own keys", direct_keys)
    case("connected", "a broker that works", connected)
    case("none", "--keep-ha", keep_ha)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failed: {', '.join(FAILURES)}")
        return 1
    print("every case holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())

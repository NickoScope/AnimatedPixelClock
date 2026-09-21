#!/usr/bin/env python3
"""A panel with no Home Assistant behind it, so bringup.py's no-HA branch can run.

The real panel here has a broker and a connected one, which is precisely the
branch that never gets exercised. Rather than disturb a wall-mounted device,
this serves the panel's OWN captured answers - /api/info, /api/panel,
/api/flightboard, /api/railboard, /api/media, all taken off the hardware
minutes ago - with only the facts under test changed:

  mqtt      -> configured/connected as the scenario asks
  direct    -> whether flights and trains have a key of their own

It also behaves like the firmware where that matters: `enable` is a bitmask per
KEY, not per page (src/panel/panel.cpp:414), so switching off `market` switches
off all four market pages at once; and /api/panel answers with the new state in
the same body, which is what bringup.py verifies against.

    python3 stub.py <port> <scenario>

Scenarios: none (no broker), stale (configured, not connected),
           direct (no broker, but flights and trains have their own keys),
           connected (a broker that works - the control case).
"""

import json
import pathlib
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = pathlib.Path(__file__).resolve().parent
SNAP = {n: json.loads((HERE / f"{n}.json").read_text())
        for n in ("info", "panel", "flightboard", "railboard", "media")}

SCENARIOS = {
    "none":      ({"configured": False, "connected": False, "status": "OFF"}, False),
    "stale":     ({"configured": True, "connected": False, "status": "NO BROKER"}, False),
    "direct":    ({"configured": False, "connected": False, "status": "OFF"}, True),
    "connected": ({"configured": True, "connected": True, "status": "NO DATA"}, False),
}

MQTT, DIRECT = SCENARIOS[sys.argv[2]]
# The panel's own answer, with only the facts under test replaced.
PAGES = [dict(p) for p in SNAP["panel"]["pages"]]
CALLS = []


def panel_body():
    b = dict(SNAP["panel"])
    b["pages"] = [dict(p) for p in PAGES]
    return b


def route(path):
    if path.startswith("/api/info"):
        return dict(SNAP["info"])
    if path.startswith("/api/panel"):
        return panel_body()
    for name in ("flightboard", "railboard", "media"):
        if path.startswith("/api/" + name):
            b = dict(SNAP[name])
            b["mqtt"] = dict(MQTT)
            if name == "flightboard":
                b["direct"] = {"key": DIRECT, "budget": 0}
            if name == "railboard":
                b["direct"] = {"token": DIRECT}
            return b
    return None


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        CALLS.append(("GET", self.path))
        body = route(self.path)
        if body is None:
            self._send(404, {"error": "no such route"})
        else:
            self._send(200, body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        payload = json.loads(self.rfile.read(n) or b"{}")
        CALLS.append(("POST", self.path, payload))

        if self.path.startswith("/api/rename"):
            SNAP["info"]["deviceName"] = payload["name"]
            self._send(200, {"success": True, "name": payload["name"]})
            return

        if self.path.startswith("/api/panel"):
            en = payload.get("enable")
            if en:
                key, on = en.get("key"), en.get("on")
                # The firmware's bitmask: one bit per KEY, every page with that
                # key follows it. Four market pages, one bit.
                hit = 0
                for p in PAGES:
                    if p.get("key") == key:
                        p["on"] = on
                        hit += 1
                if not hit:
                    self._send(400, {"error": "enable.key unknown"})
                    return
            self._send(200, panel_body())
            return

        self._send(404, {"error": "no such route"})


if __name__ == "__main__":
    srv = HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
    print(f"stub panel on 127.0.0.1:{sys.argv[1]} scenario={sys.argv[2]}", flush=True)
    srv.serve_forever()

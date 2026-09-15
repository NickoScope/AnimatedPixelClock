#!/usr/bin/env python3
"""Serve the generated portal with a mocked panel, for the Market page in a browser.

The page, its style and scripts come out of src/web/web_assets.h exactly as
the panel serves them (gunzipped here). /api/portal and /api/panel answer
enough for the portal to load; /api/market answers with the firmware's own
registry and default config (market_host_test webjson) and validates every
POST with the firmware's own mks::apply (market_host_test apply), so what the
browser shows on a refused save is the message the panel would send. The
model part is a plausible mock built from the preview numbers.

  python3 tools/market/panel/mock_portal.py [PORT]      # default 8765; Ctrl-C stops it

Every POST body is printed, so a run doubles as a record of what the page sends.
GET /mock/nomem?on=1 makes /api/market answer as a panel without PSRAM for the store.
"""
import gzip
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import probe  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

ASSETS = {"/": ("WEB_INDEX_GZ", "text/html"), "/portal.css": ("WEB_PORTAL_CSS_GZ", "text/css"),
          "/portal.js": ("WEB_PORTAL_JS_GZ", "application/javascript"), "/favicon.svg": ("WEB_FAVICON_GZ", "image/svg+xml"),
          "/panel.css": ("WEB_PANEL_CSS_GZ", "text/css"), "/panel.js": ("WEB_PANEL_JS_GZ", "application/javascript")}


def blobs():
    text = (ROOT / "src/web/web_assets.h").read_text()
    out = {}
    for name, body in re.findall(r"static const uint8_t (\w+)\[\] PROGMEM = \{(.*?)\};", text, re.S):
        out[name] = gzip.decompress(bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", body)))
    return out


BLOBS = blobs()
FORM_NAMES = sorted(set(re.findall(r'\bform\["(\w+)"\]\s*=', (ROOT / "src/web/web.cpp").read_text())))

exe = probe.build()
WEB = json.loads(subprocess.run([str(exe), "webjson"], capture_output=True, text=True, check=True).stdout)
STATE = {"config": WEB["config"], "payload": WEB["payload"], "showing": None, "posts": []}

# The model, as the previews had it (tools/market/preview/README.md).
def model():
    cfg = STATE["config"]
    win = cfg["display.window"]
    idx = [("^GSPC", "SPX", 7619.98, 4.58, "USD", "OPEN", -0.0048), ("^IXIC", "NDX", 26186.0, 5.65, "USD", "OPEN", 0.003),
           ("^FCHI", "CAC", 8118.0, 0.434, "EUR", "CLOSED", None), ("^GDAXI", "DAX", 25441.0, 2.72, "EUR", "POST", None)]
    # Made-up prices for whatever the tickers list holds.
    tk = [(t["sym"], 100.0 + 37.5 * i, 0.5 + 0.25 * i, "USD") for i, t in enumerate(cfg["tickers"])]
    names = {e["sym"]: e["name"] for e in cfg["indices"]}
    def sym(s, name, last, chg, cur, live=None, day=None):
        o = {"sym": s, "name": name, "windows": 0x7E0, "w": {"have": True, "cur": cur, "last": last, "chg": chg, "hi": last * 1.02, "lo": last / 5,
             "from": "2000-01-01", "to": "2026-09-14"}}
        if live:
            o["live"] = {"last": last, "state": live, "feed": "LIVE" if live == "OPEN" else "CLOSED", "delayS": 0}
            if day is not None:
                o["live"]["day"] = day
        return o
    return {"gen": 118, "bytes": 84956,
            "indices": [sym(s, names.get(s, n), l, c, cur, live, day) for s, n, l, c, cur, live, day in idx if s in names],
            "tickers": [sym(s, s, l, c, cur) for s, l, c, cur in tk],
            "portfolio": {"hold": {"have": True, "windows": 0x7E0, "cur": cfg["portfolio.currency"], "value": 89800.95, "chg": 7.98, "sinceStart": 7.98,
                                   "cagr": 0.086, "mdd": -0.198, "div": 8803.9, "terDrag": 170.51, "cash": 0.0055, "bench": True, "gross": False,
                                   "from": "2000-01-01", "to": "2026-09-14"},
                          "rebal": {"have": True, "windows": 0x7E0, "cur": cfg["portfolio.currency"], "value": 89800.95, "chg": 7.98, "sinceStart": 7.98,
                                    "cagr": 0.086, "mdd": -0.198, "div": 8803.9, "terDrag": 170.51, "cash": 0.0055, "bench": True, "gross": False,
                                    "from": "2000-01-01", "to": "2026-09-14"}},
            "holdings": {"hold": {"have": True, "asof": "2026-09-14", "cashNow": 0.55, "dropped": 0,
                                  "rows": [{"sym": p["sym"], "tgt": p["w"], "now": p["w"] * 0.99, "ret": 0.5, "entry": "2010-12-01"} for p in cfg["portfolio.positions"]]},
                         "rebal": {"have": False}},
            "live": {"have": True, "n": 8, "dropped": 0, "ts": 1789412345}, "intraday": {"have": True, "sym": cfg["display.ticker"], "n": 49, "ts": 1789412345},
            "tape": {"have": True, "ts": 1789412345, "x": [{"n": n, "s": "OPEN" if n in ("NYSE", "NASDAQ") else "CLOSED", "t": "22:00"} for n in cfg["tape.exchanges"]]},
            "status": {"have": True, "asof": "2026-09-14", "fetched": "2026-09-15T07:02Z", "state": "ok", "err": "",
                       "symbols": [{"sym": e["sym"], "asof": "2026-09-14", "bars": 6700, "ter": 0.0003, "terSrc": "yahoo"} for e in cfg["indices"] + cfg["tickers"]]},
            "window": win}


def market_json():
    if STATE.get("nomem"):   # the panel found no PSRAM for the store: what marketWebJson sends then
        return {"success": True, "page": 6, "showing": False, "ready": False, "dev": "", "root": "", "handler": False, "subscribed": False,
                "bridge": "online", "accepted": 0, "refused": 0, "lastRefusal": "", "jsonPeak": 0, "storeBytes": 112000, "recordBytes": 84972,
                "mqtt": {"configured": True, "connected": True, "status": "OK"}}
    cfg = STATE["config"]
    return {"success": True, "page": 6, "showing": STATE["showing"] is not None, "ready": True, "dev": "d20ec8",
            "root": "nickoscope_matrix/d20ec8/market/", "handler": True, "subscribed": True, "bridge": "online",
            "accepted": 118, "refused": 0, "lastRefusal": "", "jsonPeak": 6120, "rxAgoS": 42,
            "config": cfg, "registry": WEB["registry"],
            "nvs": {"saved": True, "note": "66 rows, 1633 B", "bytes": 1633, "writes": 3, "fails": 0, "max": 6144},
            "cfgOut": {"sent": True, "tooBig": False, "bytes": len(json.dumps(STATE["payload"], separators=(",", ":"))), "max": 4096, "count": 3, "agoS": 42},
            "fs": {"ready": True, "have": True, "note": "read 84972 B, gen 118", "bytes": 84972, "writes": 1, "fails": 0, "lastMs": 412, "pending": False, "gen": 118},
            "defaults": "neutral",
            "view": {"window": cfg["display.window"], "mode": "hold" if cfg["rebal.mode"] == "hold" else "rebal", "ticker": cfg["display.ticker"],
                     "presets": ["YTD", "1Y", "3Y", "5Y", "10Y", "MAX"]},
            "model": model(), "mqtt": {"configured": True, "connected": True, "status": "OK"}}


def apply(body):
    """A POST's config through the firmware's mks::apply, over the current config."""
    with tempfile.TemporaryDirectory() as tmp:
        base, inp = pathlib.Path(tmp, "base.json"), pathlib.Path(tmp, "in.json")
        base.write_text(json.dumps(STATE["config"]))
        inp.write_text(json.dumps(body))
        r = subprocess.run([str(exe), "apply", str(base), str(inp)], capture_output=True, text=True)
    return json.loads(r.stdout)


def reset(group):
    rows = WEB["registry"]["rows"]
    body = {r["key"]: r["def"] for r in rows if group == "all" or r["group"] == group}
    return apply(body)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send_json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ASSETS:
            name, ctype = ASSETS[path]
            data = BLOBS[name]
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if path == "/api/portal":
            form = {n: "" for n in FORM_NAMES}
            form.update({"deviceName": "mock-panel", "brightness": 50, "color_0": "#ffffff"})
            self.send_json({"ver": "mock", "built": "today", "ip": "127.0.0.1", "freeHeap": 123456, "minBright": 0,
                            "features": "panel carousel cards flights trains world yachts lua mqtt media market",
                            "scopeTrailMax": 8, "scopeTrailDefault": 2, "colorRows": [], "form": form})
            return
        if path == "/api/panel":
            pages = [{"i": i, "key": k, "name": n, "enabled": True, "secs": 20} for i, (k, n) in enumerate(
                [("clock", "CLOCK"), ("world", "WORLD CLOCK"), ("flights", "FLIGHTS"), ("trains", "TRAINS"), ("yachts", "YACHTS"),
                 ("media", "MEDIA"), ("market", "MARKETS"), ("market", "TICKER"), ("market", "PORTFOLIO"), ("market", "HOLDINGS")])]
            self.send_json({"success": True, "now": {"page": 6, "key": "market", "name": "MARKETS", "style": 0, "styleName": "classic", "entered": False,
                                                     "hz": 20, "off": False, "mode": "clock", "time": "12:00"},
                            "pages": pages, "styles": [], "carousel": {"enabled": True, "idleS": 30, "slotS": 0, "allStyles": True}})
            return
        if path == "/api/market":
            self.send_json(market_json())
            return
        if path.startswith("/mock/nomem"):   # /mock/nomem?on=1 | ?on=0
            STATE["nomem"] = "on=1" in self.path
            self.send_json({"nomem": STATE["nomem"]})
            return
        self.send_json({"success": True})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        STATE["posts"].append((path, body))
        print(f"POST {path} {json.dumps(body)[:600]}", flush=True)
        if path != "/api/market":
            self.send_json({"success": True})
            return
        if "config" in body:
            r = apply(body["config"])
            if not r["ok"]:
                self.send_json({"success": False, "error": r["error"]}, 400)
                return
            STATE["config"], STATE["payload"] = r["config"], r["payload"]
        elif "show" in body:
            if body["show"] not in ("markets", "ticker", "portfolio", "holdings"):
                self.send_json({"success": False, "error": "show must be markets, ticker, portfolio or holdings"}, 400)
                return
            STATE["showing"] = body["show"]
        elif "reset" in body:
            r = reset(body["reset"])
            if not r["ok"]:
                self.send_json({"success": False, "error": r["error"]}, 400)
                return
            STATE["config"], STATE["payload"] = r["config"], r["payload"]
        elif "republish" not in body:
            self.send_json({"success": False, "error": "send exactly one of config, show, republish, reset"}, 400)
            return
        self.send_json(market_json())


if __name__ == "__main__":
    print(f"mock portal on http://localhost:{PORT}/  ({len(WEB['registry']['rows'])} registry rows, {len(FORM_NAMES)} form values)", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

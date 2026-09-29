#!/usr/bin/env python3
"""Offline tests of sync.py: two fake devices behind a fake network, each answering its routes the way
the firmware's handlers do (web.cpp, web_panel.cpp at c4ddd3a). No socket is opened.

  python3 -m unittest tools/twin/test_sync.py -v
"""
import contextlib
import copy
import io
import json
import os
import sys
import unittest
import urllib.parse
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sync as S  # noqa: E402

# /save reads these checkboxes by hasArg alone, every time (web.cpp:1596-1990): absent = off.
ALWAYS = ("showClock", "rpmKFormat", "netMBFormat", "enableScheduledDimming", "enableScheduledOff", "notifyEnabled",
          "marioSmoothAnimation", "marioIdleEncounters", "pongHorizontalBounce", "pongDigitShatter",
          "pacmanPelletRandomSpacing", "pacmanBounceEnabled", "snakeWallBorder", "snakeShowDate", "tetrisIdleTumble",
          "tetrisDigitBounce", "tetrisSmoothGame", "tetrisSmallClock", "tetrisShowDate", "asteroidsShowDate",
          "asteroidsTransparent", "dinoShowClouds", "dinoShowDate", "matrixShowDate", "matrixTransparent",
          "showIPAtBoot")
# ... and these only when their group's marker(s) came with the form.
GROUPS = ((("weatherLat", "weatherLon"), ("weatherEnabled", "weatherFahrenheit")),          # web.cpp:1705-1713
          (("climateIntervalS",), ("climateEnabled", "climateRhFollowsT", "climateHa")),   # :1726-1735
          (("irCard",), ("irEnabled",)),                                                    # :1744-1745
          (("presenceScaleM",), ("presenceMirrorX",)),                                      # :1753-1755
          (("ambientStyle",), ("ambientEnabled", "ambientShowClock", "vizShowClock",
                               "scopeGrid", "scopeFill", "scopeFlat")),                     # :1765-1799
          (("micGainDb", "micGateDb"), ("micAgc",)))                                        # :1809-1813
REGIONS = {5: "CET-1CEST,M3.5.0,M10.5.0/3", 7: "GMT0BST,M3.5.0/1,M10.5.0"}
PAGE_KEYS = ("clock", "world", "flights", "trains", "yachts", "media", "market")
BUILTIN_APTS = (("LFMD", "CEQ", "CANNES"), ("LFMN", "NCE", "NICE"), ("LFPG", "CDG", "PARIS CDG"),
                ("EGLL", "LHR", "LONDON"), ("EDDF", "FRA", "FRANKFURT"), ("EHAM", "AMS", "AMSTERDAM"))
PANEL_KEY = "PANEL-SECRET-KEY-1"      # the panel's weather key: must reach nothing and no output
TWIN_KEY = "TWIN-OWN-KEY-2"           # the twin's: must stay and never be printed
MONEY = 123456.78                      # a market value outside the display groups


def rgb565_to_hex(c):                  # web.cpp:962-967
    r, g, b = ((c >> 11) & 0x1F) * 255 // 31, ((c >> 5) & 0x3F) * 255 // 63, (c & 0x1F) * 255 // 31
    return f"#{r:02x}{g:02x}{b:02x}"


def hex_to_rgb565(h):                  # web.cpp:2229-2231
    rgb = int(h[1:], 16)
    r8, g8, b8 = (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF
    return ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)


def base_form(name):
    f = {k: False for k in ALWAYS}
    f.update(showClock=True, showIPAtBoot=True, notifyEnabled=True, tetrisShowDate=True, pongHorizontalBounce=True)
    f.update({"timezoneRegion": 5, "clockStyle": 0, "cycleConfig": "1:300,0:300", "tronBikeStyle": 0,
              "marioBounceHeight": 35, "use24Hour": 1, "dateFormat": 0, "clockPosition": 0, "rowMode": 0,
              "clockOffset": 0, "colonBlinkMode": 1, "colonBlinkRate": 10, "displayBrightness": 255,
              "dimStartTime": "22:00", "dimEndTime": "07:00", "dimBrightness": 50, "offStartTime": "00:00",
              "offEndTime": "07:00", "notifyPosition": 0,
              "weatherEnabled": True, "weatherLat": "0.0000", "weatherLon": "0.0000", "weatherFahrenheit": False,
              "weatherApiKey": "", "climateEnabled": True, "climateIntervalS": 10, "climateTempOffset": "0.0",
              "climateHumOffset": "0.0", "climateRhFollowsT": True, "climateShow": 1, "climateHa": True,
              "irEnabled": True, "irCard": 1, "presenceScaleM": 4, "presenceMirrorX": False, "presenceSource": 1,
              "ambientStyle": 0, "ambientCustomFile": "", "ambientEnabled": False, "ambientShowClock": True,
              "ambientStartHour": 20, "ambientEndHour": 23, "vizStyle": 3, "vizShowClock": True, "scopeGrid": True,
              "scopeFill": False, "scopeFlat": False, "scopeTrail": 3, "scopeGain": 100, "audioSource": 0,
              "micGainDb": 30, "micGateDb": -60, "micAgc": True, "vizBeatFx": 100,
              "deviceName": name, "useStaticIP": 0, "staticIP": "192.168.1.100", "gateway": "192.168.1.1",
              "subnet": "255.255.255.0", "dns1": "8.8.8.8", "dns2": "8.8.4.4", "ntpServer1": "pool.ntp.org",
              "ntpServer2": "time.nist.gov"})
    return f


class Fake:
    """One device: its settings, and its routes as the firmware answers them."""

    def __init__(self, name, mac, ip, version="2.7.5", build="Sep 29 2026 22:06:33"):
        self.ip, self.mac, self.version, self.build = ip, mac, version, build
        self.form = base_form(name)
        self.tz, self.gmt, self.dst = REGIONS[5], 60, True
        self.metrics = {"metricLabels": [""] * 20, "metricOrder": list(range(20)), "metricCompanions": [0] * 20,
                        "metricPositions": [255] * 20, "metricBarPositions": [255] * 20, "metricBarMin": [0] * 20,
                        "metricBarMax": [100] * 20, "metricBarWidths": [60] * 20, "metricBarOffsets": [0] * 20}
        self.colors = [rgb565_to_hex((i * 997) & 0xFFFF) for i in range(63)]
        self.log_on = False
        self.pages = {k: True for k in PAGE_KEYS}
        self.cards_on = True
        self.carousel = {"enabled": False, "idleS": 60, "slotS": 15, "allStyles": True}
        self.knob = {"reverse": False, "lockoutMs": 10, "debounceMs": 20, "detent": -1}
        self.effects = ["AQUARIUM", "CANNES", "FLOW"]
        self.in_walk = [True, True, True]
        self.scripts = ["AQUARIUM", "CANNES", "FLOW"]
        self.cities = [{"id": i, "kind": "builtin", "name": n, "lat": 1.0 + i, "lon": 2.0, "tz": "Europe/Paris"}
                       for i, n in enumerate(("CANNES", "MOSCOW", "NEW YORK", "LONDON", "DUBAI", "ALMATY"))]
        self.custom_cities = [None] * 6
        self.home, self.home_chosen = 0, False
        self.crs, self.favourites, self.rb_from = "GLD", [], "ha"
        self.rb_token, self.aero_key, self.ais_key = False, False, False
        self.airport, self.dir = 1, "alt"
        self.custom_apts = [None] * 6
        self.selections = []                  # (airport code, dir) each time the selection changed
        self.budget = {"floor_min": 15, "day_cap": 30, "month_cap": 900}
        self.tracked = []
        self.market = {"display.window": "MAX", "display.ticker": True, "portfolio.capital": 10000.0,
                       "portfolio.positions": [{"sym": "VT", "w": 100}]}
        self.market_groups = {"display.window": "display", "display.ticker": "display",
                              "portfolio.capital": "portfolio", "portfolio.positions": "portfolio"}
        self.selected = ""
        self.buttons = [{"n": n, "fn": "none", "bound": True, "proto": "NEC", "code": f"0xFF{n:04X}"}
                        for n in range(1, 11)]
        self.requests = []                    # (method, path with query, body)
        self.ignore = set()                   # routes whose POST answers 200 and changes nothing

    # -- reads ----------------------------------------------------------------------------------
    def apt(self, i):
        if i < 6:
            icao, iata, name = BUILTIN_APTS[i]
            return {"id": i, "code": icao, "iata": iata, "name": name, "tz": "Europe/Paris", "kind": "builtin"}
        a = self.custom_apts[i - 100] if 100 <= i < 106 else None
        return dict(a, id=i, kind="custom") if a else None

    def airports(self):
        return [self.apt(i) for i in list(range(6)) + list(range(100, 106)) if self.apt(i)]

    def all_cities(self):
        return self.cities + [dict(c, id=100 + i, kind="custom") for i, c in enumerate(self.custom_cities) if c]

    def get(self, path):
        f = self.form
        if path == "/api/info":
            return {"model": "AnimatedPixelClock", "deviceName": f["deviceName"], "mac": self.mac, "ip": self.ip,
                    "version": self.version, "build": self.build, "logOn": self.log_on, "freeInternalHeap": 1}
        if path == "/api/portal":
            return {"ver": self.version, "spriteColors": list(self.colors), "form": copy.deepcopy(f)}
        if path == "/api/export":
            e = {"clockStyle": f["clockStyle"], "climateHa": f["climateHa"], "deviceName": f["deviceName"],
                 "weatherApiKey": f["weatherApiKey"], "timezoneString": self.tz, "gmtOffset": self.gmt,
                 "daylightSaving": self.dst, "metricNames": ["x"] * 20, "spriteColors": list(self.colors)}
            e.update(copy.deepcopy(self.metrics))
            return e
        if path == "/api/panel":
            return {"pages": [{"i": i, "key": k, "on": v} for i, (k, v) in enumerate(self.pages.items())],
                    "cardsOn": self.cards_on, "carousel": dict(self.carousel, running=False)}
        if path == "/api/knob":
            return dict(self.knob, stats={"cw": 3})
        if path == "/api/lua":
            return {"effects": list(self.effects), "inWalk": list(self.in_walk),
                    "uploaded": {"scripts": [{"name": n, "bytes": 1} for n in self.scripts]}}
        if path == "/api/worldclock":
            return {"cities": self.all_cities(), "home": self.home, "homeChosen": self.home_chosen}
        if path == "/api/railboard":
            return {"crs": self.crs, "favourites": list(self.favourites),
                    "cfg": {"from": self.rb_from, "rows": 8, "level": 100}, "direct": {"token": self.rb_token},
                    "mqtt": {"configured": True, "connected": True}}
        if path == "/api/flightboard":
            return {"airport": self.airport, "dir": self.dir, "airports": self.airports(),
                    "limits": {"custom": 6, "name": 12, "namePx": 56, "advance": [4] * 95},
                    "direct": {"built": True, "key": self.aero_key, "budget": dict(self.budget)},
                    "tracked": [{"ident": t} for t in self.tracked]}
        if path == "/api/market":
            return {"config": copy.deepcopy(self.market),
                    "registry": {"rows": [{"key": k, "group": g} for k, g in self.market_groups.items()]}}
        if path == "/api/media":
            return {"selected": self.selected}
        if path == "/api/ir":
            return {"buttons": copy.deepcopy(self.buttons)}
        if path == "/api/yachtradar":
            return {"keyPresent": self.ais_key}
        return None

    # -- writes ---------------------------------------------------------------------------------
    def save(self, body):
        args = dict(urllib.parse.parse_qsl(body, keep_blank_values=True))
        has, f = args.__contains__, self.form
        for k in ALWAYS:
            f[k] = has(k)
        for markers, boxes in GROUPS:
            if all(has(m) for m in markers):
                for b in boxes:
                    f[b] = has(b)
        if has("climateIntervalS"):     # toFloat("") is 0 (web.cpp:1729-1730)
            f["climateTempOffset"] = "%.1f" % float(args.get("climateTempOffset") or 0)
            f["climateHumOffset"] = "%.1f" % float(args.get("climateHumOffset") or 0)
        if has("weatherLat") and has("weatherLon") and has("weatherApiKey"):    # web.cpp:1714
            f["weatherApiKey"] = args["weatherApiKey"]
        for k, v in args.items():       # a value field is kept when it did not come
            if k in f and not isinstance(f[k], bool) and k not in ("climateTempOffset", "climateHumOffset",
                                                                  "weatherApiKey", "timezoneRegion"):
                f[k] = int(v) if isinstance(f[k], int) else v
        if has("timezoneRegion") and int(args["timezoneRegion"]) in REGIONS:   # web.cpp:1554-1568
            f["timezoneRegion"] = int(args["timezoneRegion"])
            self.tz, self.dst = REGIONS[f["timezoneRegion"]], True
        m = self.metrics
        for i in range(20):             # web.cpp:2047-2120
            n = str(i + 1)
            if has("label_" + n):
                m["metricLabels"][i] = args["label_" + n].strip()
            if has("order_" + n):
                m["metricOrder"][i] = int(args["order_" + n])
            m["metricCompanions"][i] = int(args.get("companion_" + n, 0))
            m["metricPositions"][i] = int(args.get("position_" + n, 255))
            m["metricBarPositions"][i] = int(args.get("barPosition_" + n, 255))
            for arr, field in (("metricBarMin", "barMin_"), ("metricBarMax", "barMax_")):
                if has(field + n):
                    m[arr][i] = int(args[field + n])
            m["metricBarWidths"][i] = int(args.get("barWidth_" + n, 60))
            m["metricBarOffsets"][i] = int(args.get("barOffset_" + n, 0))
        for slot in range(len(self.colors)):
            v = args.get(f"color_{slot}")
            if v and len(v) == 7 and v[0] == "#":
                self.colors[slot] = rgb565_to_hex(hex_to_rgb565(v))
        return {"success": True, "networkChanged": False}

    def select_airport(self, apt, direction):
        if (apt, direction) != (self.airport, self.dir):
            self.airport, self.dir = apt, direction
            self.selections.append((self.apt(apt)["code"], direction))

    def post(self, path, d):
        if path == "/api/import":
            if "climateHa" in d:
                self.form["climateHa"] = bool(d["climateHa"])
            if "timezoneString" in d:
                self.tz, self.form["timezoneRegion"] = d["timezoneString"], -1
            return {"success": True}
        if path == "/api/panel":
            if "enable" in d:
                if d["enable"]["key"] == "clock" and not d["enable"]["on"]:
                    return 400, {"error": "the clock page cannot be switched off"}
                if d["enable"]["key"] == "cards":
                    self.cards_on = d["enable"]["on"]
                else:
                    self.pages[d["enable"]["key"]] = d["enable"]["on"]
            if "carousel" in d:
                self.carousel.update(d["carousel"])
            return self.get("/api/panel")
        if path == "/api/knob":
            self.knob.update({k: d[k] for k in S.KNOB_KEYS if k in d})
            return self.get("/api/knob")
        if path == "/api/lua":
            w = d["walk"]
            if self.effects[w["i"]] != w["name"]:
                return 409, {"error": "walk.name is not effect i any more"}
            self.in_walk[w["i"]] = w["on"]
            return self.get("/api/lua")
        if path == "/api/worldclock":
            if "add" in d:
                a = d["add"]
                if any(c["name"] == a["name"] for c in self.all_cities()):
                    return 409, {"error": "a city with that name is already on the map"}
                slot = self.custom_cities.index(None)
                self.custom_cities[slot] = {k: a[k] for k in ("name", "lat", "lon", "tz")}
                if a.get("home"):
                    self.home, self.home_chosen = 100 + slot, True
            elif "remove" in d:
                self.custom_cities[d["remove"] - 100] = None
            elif "home" in d:
                self.home, self.home_chosen = d["home"], True
            elif d.get("auto"):
                self.home, self.home_chosen = 0, False
            return self.get("/api/worldclock")
        if path == "/api/railboard":
            if "crs" in d:
                self.crs = d["crs"]
            if "favourites" in d:
                self.favourites = list(d["favourites"])
            return self.get("/api/railboard")
        if path == "/api/flightboard":
            apt, direction = self.airport, d.get("dir", self.dir)
            if "airport" in d:
                apt = d["airport"]
            if "add" in d:
                a = d["add"]
                if any(x["code"] == a["icao"] for x in self.airports()):
                    return 409, {"error": "that airport is already in the list"}
                slot = self.custom_apts.index(None)
                self.custom_apts[slot] = {"code": a["icao"], "iata": a.get("iata", ""), "name": a["name"],
                                          "tz": a["tz"]}
                if a.get("select"):
                    apt = 100 + slot
            if "budget" in d:
                self.budget.update(d["budget"])
            if "track" in d:
                self.tracked.append(d["track"])
            if "untrack" in d:
                self.tracked.remove(d["untrack"])
            self.select_airport(apt, direction)     # panelSetFlightboard (web_panel.cpp:572)
            return self.get("/api/flightboard")
        if path == "/api/market":
            self.market.update(d["config"])
            return self.get("/api/market")
        if path == "/api/media":
            self.selected = d["select"]
            return self.get("/api/media")
        return 404, {"error": "no route"}

    def handle(self, method, path, query, body, ctype):
        """(status, text): the route's answer."""
        if method == "GET" and not query:
            doc = self.get(path)
            return (200, json.dumps(doc)) if doc is not None else (404, "")
        if method == "GET" and path == "/api/log":
            self.log_on = query["on"] == "1"
            return 200, "log bytes, not JSON"
        if method == "GET" and path == "/api/ir/fn":
            b = self.buttons[int(query["btn"]) - 1]
            b["fn"] = query["fn"]
            b.pop("page", None)
            if query["fn"] == "page":
                b["page"] = int(query["page"])
            return 200, json.dumps({"buttons": self.buttons})
        if method != "POST":
            return 404, ""
        if path in self.ignore:                      # a 200 that did nothing (tools/agent/panel.py)
            return 200, json.dumps({"success": True})
        if path == "/save":
            if ctype != "application/x-www-form-urlencoded":
                return 400, ""
            return 200, json.dumps(self.save(body.decode()))
        if path != "/api/import":
            if ctype != "application/json":          # readBody (web_panel.cpp:170-172)
                return 415, json.dumps({"error": "Content-Type must be application/json"})
            if len(body) > (8192 if path == "/api/market" else 1024):
                return 413, json.dumps({"error": "too big"})
        out = self.post(path, json.loads(body))
        if isinstance(out, tuple):
            return out[0], json.dumps(out[1])
        return 200, json.dumps(dict(out, success=True))


class Net:
    """The network sync.http would reach: devices by address, every request logged on its device."""

    def __init__(self, *devices):
        self.by = {d.ip: d for d in devices}

    def http(self, method, url, body=None, ctype=None, timeout=20.0, tries=6):
        u = urllib.parse.urlsplit(url)
        dev = self.by.get(u.netloc)
        if dev is None:
            raise S.SyncError(f"{method} {url}: no answer (no such host)")
        query = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
        dev.requests.append((method, u.path + (f"?{u.query}" if u.query else ""), body))
        return dev.handle(method, u.path, query, body, ctype)


def panel_and_twin():
    """The panel with what differed on 2026-09-29 22:50 (fidelity.txt), and the twin as it was."""
    p = Fake("NickoScopeMatrix-64x128-01", S.PANEL_MAC, "10.0.0.89", "2.7.6", "Sep 29 2026 22:41:53")
    f = p.form
    f.update(clockStyle=16, enableScheduledDimming=True, dimBrightness=21, enableScheduledOff=True,
             presenceMirrorX=True, climateHa=False, weatherApiKey=PANEL_KEY)
    f.update(tetrisShowDate=False, weatherFahrenheit=True, micAgc=False, scopeFill=True, snakeShowDate=True)
    f["panelOnlyField"] = 3                       # a newer firmware's field
    p.log_on = True
    p.metrics["metricLabels"][0], p.metrics["metricPositions"][0] = "CPU", 0
    p.colors[3] = "#ff0000"
    p.custom_cities[0] = {"name": "RIGA", "lat": 56.946, "lon": 24.10589, "tz": "Europe/Riga"}
    p.custom_cities[1] = {"name": "GUILDFORD", "lat": 51.23536, "lon": -0.57427, "tz": "Europe/London"}
    p.home, p.home_chosen = 101, True
    p.favourites = ["GLD", "WAT", "CLJ", "WOK", "SUR"]
    p.rb_token = p.aero_key = p.ais_key = True
    p.market.update({"display.window": "1Y", "portfolio.capital": MONEY})
    p.scripts.append("AUTUMN")
    p.buttons[4]["fn"] = "power"
    p.knob["lockoutMs"] = 12
    t = Fake("TWIN-NickoScopeMatrix-64x128-01", S.TWIN_MAC, "10.0.0.68")
    t.form.update(weatherApiKey=TWIN_KEY, staticIP="10.9.9.9", twinOnlyField=7)
    t.buttons[4]["fn"] = "none"
    return p, t


def run_sync(net, *args):
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(S, "http", net.http), mock.patch.object(S, "SETTLE_S", 0), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = S.main(list(args) + ["--panel", "10.0.0.89", "--twin", "10.0.0.68"])
    return code, out.getvalue() + err.getvalue()


def writes(dev):
    return [r for r in dev.requests if r[0] != "GET" or "?" in r[1]]


class Safety(unittest.TestCase):
    def test_source_can_only_read(self):
        src = S.Source("10.0.0.89")
        for name in ("post", "post_form", "action", "_send"):
            self.assertFalse(hasattr(src, name), name)
        net = Net(Fake("P", S.PANEL_MAC, "10.0.0.89"))
        with mock.patch.object(S, "http", net.http):
            for path in ("/reset", "/api/reboot", "/save", "/api/log", "/api/log?on=1", "/api/presence/mirror?on=1",
                         "/api/clock/style?id=1", "/api/ir/fn?btn=1&fn=ok", "/api/info?x=1"):
                with self.assertRaises(S.SyncError):
                    src.get(path)
        self.assertEqual(net.by["10.0.0.89"].requests, [])

    def test_an_address_carries_no_path_or_query(self):
        # review 2026-09-29: resolve() takes anything with a dot as an address, and "addr/api/log?on=0#"
        # reached the panel as GET /api/log?on=0
        for bad in ("10.0.0.89/api/log?on=0#", "10.0.0.89?x=1", "user@10.0.0.89", "10.0.0.89#frag"):
            with self.assertRaises(S.SyncError, msg=bad):
                S.locate("panel", bad, S.PANEL_MAC)
        self.assertEqual(S.locate("panel", "10.0.0.89:8080", S.PANEL_MAC), "10.0.0.89:8080")

    def test_the_panel_is_only_read_through_a_whole_apply(self):
        p, t = panel_and_twin()
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        self.assertTrue(p.requests)
        for method, path, body in p.requests:
            self.assertEqual(method, "GET")
            self.assertIsNone(body)
            self.assertIn(path, S.READ_ROUTES)
        self.assertTrue(writes(t))

    def test_the_guard_refuses_and_nothing_is_written(self):
        cases = (("name", lambda t: t.form.update(deviceName="NickoScopeMatrix-64x128-02"), "not TWIN-"),
                 ("vendor MAC", lambda t: setattr(t, "mac", "90:E5:B1:D2:0E:2D"), "universally administered"),
                 ("panel's MAC", lambda t: setattr(t, "mac", S.PANEL_MAC), "universally administered"),
                 ("model", lambda t: setattr(t, "get", lambda path, g=t.get: dict(g(path), model="Other")
                                             if path == "/api/info" else g(path)), "not AnimatedPixelClock"))
        for what, spoil, reason in cases:
            with self.subTest(what):
                p, t = panel_and_twin()
                spoil(t)
                code, out = run_sync(Net(p, t), "--apply")
                self.assertEqual(code, 2, out)
                self.assertIn(reason, out)
                self.assertEqual(writes(t), [])
                self.assertEqual(writes(p), [])

    def test_the_guard_itself(self):
        info = {"model": "AnimatedPixelClock", "deviceName": "TWIN-X", "mac": S.TWIN_MAC}
        self.assertIsNone(S.twin_guard(info, "10.0.0.68", S.PANEL_MAC, "10.0.0.89"))
        self.assertIn("panel's address", S.twin_guard(info, "10.0.0.89", S.PANEL_MAC, "10.0.0.89"))
        self.assertIn("panel's MAC", S.twin_guard(info, "10.0.0.68", S.TWIN_MAC, "10.0.0.89"))
        self.assertIn("not a MAC", S.twin_guard(dict(info, mac=""), "10.0.0.68", S.PANEL_MAC, "10.0.0.89"))

    def test_twin_refuses_to_be_made_for_the_panel(self):
        p, t = panel_and_twin()
        with mock.patch.object(S, "http", Net(p, t).http), self.assertRaises(S.SyncError):
            S.Twin("10.0.0.89", S.TWIN_MAC, "10.0.0.68")          # the panel as the target
        self.assertEqual(writes(p), [])

    def test_twin_writes_only_its_routes(self):
        p, t = panel_and_twin()
        with mock.patch.object(S, "http", Net(p, t).http):
            twin = S.Twin("10.0.0.68", S.PANEL_MAC, "10.0.0.89")
            for call in (lambda: twin.post("/reset", {}), lambda: twin.post("/api/reboot", {}),
                         lambda: twin.post("/save", {}), lambda: twin.post_form("/api/import", []),
                         lambda: twin.action("/api/presence/mirror", {"on": "1"}),
                         lambda: twin.action("/api/log", {"clear": "1"}),
                         lambda: twin.post("/api/panel", {"x": "y" * 2000})):
                with self.assertRaises(S.SyncError):
                    call()
        self.assertEqual(writes(t), [])

    def test_dry_run_and_diff_write_nothing(self):
        for mode in ([], ["--dry-run"], ["--diff"]):
            with self.subTest(mode):
                p, t = panel_and_twin()
                code, out = run_sync(Net(p, t), *mode)
                self.assertEqual(code, 0, out)
                self.assertEqual(writes(t), [])
                self.assertEqual(writes(p), [])
                self.assertIn("form.clockStyle", out)
                self.assertIn("0 -> 16", out)


class FullForm(unittest.TestCase):
    def saved(self):
        p, t = panel_and_twin()
        before = copy.deepcopy(t.form)
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        (body,) = [r[2] for r in t.requests if r[1] == "/save"]
        return p, t, before, dict(urllib.parse.parse_qsl(body.decode(), keep_blank_values=True)), out

    def test_the_form_carries_every_field_the_twin_has(self):
        p, t, before, form, _ = self.saved()
        for k, v in before.items():
            if k in S.FORM_IDENTITY or k in S.FORM_SECRET:
                self.assertNotIn(k, form, k)             # /save keeps an absent one (web.cpp:1714, 1970-2034)
            elif k == "climateHa":
                self.assertEqual(form.get(k), "1")       # the twin's own until the override, last
            elif isinstance(p.form.get(k, v), bool):
                self.assertEqual(k in form, p.form.get(k, v), k)
                if k in form:
                    self.assertEqual(form[k], "1")
            else:
                self.assertEqual(form[k], str(p.form.get(k, v)), k)
        for markers, _ in GROUPS:
            for m in markers:
                self.assertIn(m, form)
        for _, field in S.METRIC_FIELDS:
            for i in range(1, 21):
                self.assertIn(f"{field}{i}", form)
        self.assertEqual(sum(k.startswith("color_") for k in form), 63)
        self.assertNotIn("panelOnlyField", form)         # the twin's firmware has no such field

    def test_nothing_is_reset_and_everything_copied(self):
        p, t, before, _, out = self.saved()
        for k in S.FORM_IDENTITY:
            self.assertEqual(t.form[k], before[k], k)
        self.assertEqual(t.form["weatherApiKey"], TWIN_KEY)
        self.assertEqual(t.form["twinOnlyField"], 7)
        for k, v in p.form.items():
            if k in S.FORM_IDENTITY or k in S.FORM_SECRET or k == "panelOnlyField":
                continue
            self.assertEqual(t.form[k], v, k)            # climateHa: false by the override, the panel's too
        self.assertEqual(t.metrics, p.metrics)
        self.assertEqual(t.colors, p.colors)
        self.assertTrue(t.log_on)
        self.assertEqual(t.favourites, p.favourites)
        self.assertEqual(t.knob, p.knob)
        self.assertEqual(t.buttons[4]["fn"], "power")
        self.assertEqual(t.market["display.window"], "1Y")
        self.assertEqual([c and c["name"] for c in t.custom_cities[:2]], ["RIGA", "GUILDFORD"])
        self.assertEqual((t.home, t.home_chosen), (101, True))
        self.assertIn("panelOnlyField", out)             # reported, not copied
        self.assertIn("lua.scripts", out)                # AUTUMN is only on the panel
        self.assertIn("AUTUMN", out)

    def test_colours_survive_the_round_trip(self):
        for c in range(0x10000):
            self.assertEqual(hex_to_rgb565(rgb565_to_hex(c)), c)

    def test_a_custom_zone_goes_by_import(self):
        p, t = panel_and_twin()
        p.form["timezoneRegion"], p.tz = -1, "<+04>-4"
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        self.assertEqual(t.tz, "<+04>-4")
        imports = [json.loads(r[2]) for r in t.requests if r[1] == "/api/import"]
        self.assertEqual(imports[0]["timezoneString"], "<+04>-4")


class Secrets(unittest.TestCase):
    def test_never_sent_never_printed(self):
        p, t = panel_and_twin()
        net = Net(p, t)
        outputs = [run_sync(net, mode)[1] for mode in ("--dry-run", "--diff", "--apply")]
        for out in outputs:
            for secret in (PANEL_KEY, TWIN_KEY, str(MONEY)):
                self.assertNotIn(secret, out)
            self.assertIn("secret.aeroApiKey", out)
            self.assertIn('panel "set", twin "not set"', out)
        for _, path, body in t.requests:
            self.assertNotIn(PANEL_KEY, path)
            if body:
                self.assertNotIn(PANEL_KEY.encode(), body)
                self.assertNotIn(TWIN_KEY.encode(), body)
        self.assertEqual(t.form["weatherApiKey"], TWIN_KEY)
        self.assertIn('"MAX" -> "1Y"', outputs[0])        # a display value may be printed
        self.assertIn("market.portfolio.capital", outputs[0])
        self.assertIn("(not printed)", outputs[0])
        self.assertEqual(t.market["portfolio.capital"], MONEY)     # copied, only not shown

    def test_a_secret_never_enters_the_settings(self):
        p, _ = panel_and_twin()
        s = S.settings_of({r: p.get(r) for r in S.READ_ROUTES})
        self.assertNotIn(PANEL_KEY, json.dumps(s))
        self.assertEqual(s["secret.weatherApiKey"], "set")


class Overrides(unittest.TestCase):
    def test_listed_with_the_owners_date(self):
        with open(os.path.join(HERE, "sync.py"), encoding="utf-8") as f:
            src = f.read()
        self.assertIn("The owner, 2026-09-29 22:50", src)
        self.assertEqual(S.OVERRIDE_NAMES, ("form.climateHa", "pages.trains", "pages.flights", "flightboard.selection"))

    def test_applied_last_and_the_copy_never_touches_them(self):
        p, t = panel_and_twin()
        plan = S.make_plan({r: p.get(r) for r in S.READ_ROUTES}, {r: t.get(r) for r in S.READ_ROUTES})
        flags = [s.override for s in plan.steps]
        self.assertEqual(flags, sorted(flags))               # every override after every copy
        self.assertEqual(sum(flags), 4)
        for s in plan.steps:
            if not s.override:
                self.assertFalse(set(s.covers) & set(S.OVERRIDE_NAMES), s.label)
                self.assertNotIn("airport", s.body if isinstance(s.body, dict) else {})
                self.assertNotIn("dir", s.body if isinstance(s.body, dict) else {})
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        self.assertFalse(t.form["climateHa"])
        self.assertFalse(t.pages["trains"])
        self.assertFalse(t.pages["flights"])
        self.assertTrue(p.pages["trains"])
        self.assertEqual(t.apt(t.airport)["code"], "ZZZZ")
        self.assertEqual(t.dir, p.dir)
        self.assertEqual(t.selections, [("ZZZZ", "alt")])     # never a built-in airport on the way
        last = [r for r in writes(t)][-4:]
        self.assertEqual([r[1] for r in last], ["/api/import", "/api/panel", "/api/panel", "/api/flightboard"])
        self.assertEqual(json.loads(last[0][2]), {"climateHa": False})

    def test_override_wins_over_the_panel(self):
        p, t = panel_and_twin()
        p.form["climateHa"] = True                        # even if the panel had it on
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        self.assertFalse(t.form["climateHa"])
        self.assertIn("form.climateHa", out)

    def test_the_no_request_airport_is_reused(self):
        p, t = panel_and_twin()
        t.custom_apts[2] = {"code": "ZZZZ", "iata": "", "name": "NO REQUESTS", "tz": "Europe/Paris"}
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 0, out)
        (body,) = [json.loads(r[2]) for r in t.requests if r[0] == "POST" and r[1] == "/api/flightboard"]
        self.assertEqual(body, {"airport": 102, "dir": "alt"})

    def test_a_second_run_sends_nothing(self):
        p, t = panel_and_twin()
        net = Net(p, t)
        self.assertEqual(run_sync(net, "--apply")[0], 0)
        n = len(writes(t))
        code, out = run_sync(net, "--apply")
        self.assertEqual(code, 0, out)
        self.assertEqual(len(writes(t)), n)
        self.assertIn("requests to the twin: none", out)
        self.assertIn("false on the twin, as the override wants (the panel: true)", out)


class Afterwards(unittest.TestCase):
    def test_a_write_that_did_nothing_is_a_failure(self):
        p, t = panel_and_twin()
        t.ignore.add("/api/knob")                         # 200, and nothing changed
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 1, out)
        self.assertIn("NOT DONE", out)
        self.assertIn("/api/knob", out.split("NOT DONE")[1])

    def test_a_refused_write_does_not_stop_the_overrides(self):
        p, t = panel_and_twin()
        p.effects[1] = "RENAMED"                          # the walk step will name no effect of the twin's
        p.in_walk[0] = False
        t.post = (lambda path, d, orig=t.post: (409, {"error": "walk.name"}) if path == "/api/lua" else orig(path, d))
        code, out = run_sync(Net(p, t), "--apply")
        self.assertEqual(code, 1, out)
        self.assertIn("FAILED", out)
        self.assertFalse(t.form["climateHa"])
        self.assertEqual(t.apt(t.airport)["code"], "ZZZZ")


class Http(unittest.TestCase):
    def test_503_is_asked_again_and_a_post_is_not_repeated(self):
        import urllib.error

        class Answer:
            status = 200

            def __init__(self, text):
                self.text = text

            def read(self):
                return self.text.encode()

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        busy = urllib.error.HTTPError("u", 503, "busy", {"Retry-After": "0"}, io.BytesIO(b""))
        with mock.patch.object(S.urllib.request, "urlopen", side_effect=[busy, Answer("{}")]) as u, \
                mock.patch.object(S.time, "sleep"):
            self.assertEqual(S.http("GET", "http://x/api/info"), (200, "{}"))
        self.assertEqual(u.call_count, 2)
        with mock.patch.object(S.urllib.request, "urlopen", side_effect=OSError("timed out")) as u, \
                mock.patch.object(S.time, "sleep"), self.assertRaises(S.SyncError):
            S.http("POST", "http://x/api/knob", b"{}", "application/json")
        self.assertEqual(u.call_count, 1)


if __name__ == "__main__":
    unittest.main()

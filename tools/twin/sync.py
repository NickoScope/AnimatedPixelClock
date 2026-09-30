#!/usr/bin/env python3
"""sync.py - the panel's settings onto the virtual twin in one command, then off on the twin what the
house must hear from the panel only.

  sync.py [--dry-run] [--panel P] [--twin T]
      read both devices (GET only) and print the plan: each setting that differs, the twin's value
      now -> after, and the requests --apply would send, in order. The default; nothing is written.
  sync.py --apply [--panel P] [--twin T]
      send them to the twin, read both again and print what still differs. Identity, secrets,
      firmware state, what HTTP cannot carry and the owner's overrides are expected to; anything
      else is a failure (exit status 1).
  sync.py --diff [--panel P] [--twin T]
      every difference, by kind; nothing is written.

P and T are a MAC, a device name or an address (tools/agent/panel.py resolve). By default the panel
is 90:E5:B1:D2:0E:2C and the twin 02:54:57:49:4E:01, found by their MAC over mDNS
(tools/agent/discover.py).

Safety. The panel is only read: its client, Source, has one method, a GET of a route that only reads
(READ_ROUTES), never with a query string. The twin is written only through Twin, which exists only
after the twin's own /api/info says TWIN-..., a locally administered MAC (bit 0x02 of the first
byte, IEEE 802 as twin.py MAC) and neither the panel's MAC nor its address. Secrets never enter the
plan, a request or the output - only "set" or "not set": weatherApiKey (the one HTTP gives out,
web.cpp:1216, 2316), the AeroAPI key, the RTT token, the AIS key and the MQTT login (flags only).
Market settings can hold the owner's money, so their values are printed for the display groups only.

How each thing is copied (firmware 2.7.5, c4ddd3a; 2.7.6 changed no route):
  POST /save      one form, the portal's whole: the twin's own /api/portal values with the copied
                  fields replaced by the panel's. /save takes an absent checkbox as "off" - 26 of
                  them always, the rest when their group's marker came (weatherLat+weatherLon,
                  climateIntervalS, irCard, presenceScaleM, ambientStyle, micGainDb+micGateDb;
                  web.cpp:1596-1990) - and without the metric rows it resets them (web.cpp:2070-2120).
                  So every field goes: a true checkbox as 1, a false one left out, the 20 metric
                  rows from the panel's /api/export, the sprite colours as color_<slot>. Left out on
                  purpose, as /save keeps an absent one: deviceName (web.cpp:1970), useStaticIP and
                  the static address (web.cpp:1992-2034; a change restarts, :2251-2263), weatherApiKey
                  (web.cpp:1714). Applied at once, no reboot (web.cpp:2242-2249).
  POST /api/import  the zone string, only when the panel's zone is no region of the list
                  (timezoneRegion -1); /save sets a region (web.cpp:1554-1568).
  /api/panel (pages, carousel), /api/knob, /api/lua (effects in the walk), /api/worldclock (custom
  cities, home), /api/railboard (station, favourites, its settings when the portal owns them),
  /api/flightboard (custom airports, budget, tracked flights, the airport selected and its half, by
  the airport's ICAO code), /api/market (the keys that differ),
  /api/media (the player), GET /api/ir/fn (a remote button's function), GET /api/log?on= (the log).
  Not over HTTP: Lua scripts (no route gives a script's source), the remote's learned codes (they
  come from a real frame: tools/twin/learn_remote.txt), secrets, identity (name, MAC, address,
  Wi-Fi) and firmware state (metric names, usage counters, caches, crash report).

The owner's overrides come last, over whatever the copy brought: OVERRIDES below - climateHa off
(2026-09-29 22:50) and fbAskHa off (2026-09-30 22:40). The rail board publishes its station, retained, on every broker connect, and nothing in
the firmware switches that off (railboard.cpp:555-569): the twin keeps the panel's station, so run this
again after changing the panel's.

The owner, 2026-09-30 19:35: the twin keeps no paid screen off by force. Its portal has the Keys page
(/api/keys, as the panel's: a key is written there and never given out), and its boards work from the
keys entered there; secrets are never copied. So the trains and flights pages and the flight board's
airport are copied like the rest, and the airport the old override added, NO_ASK, is removed from the
twin. Without a key the twin makes no paid call of its own (aero_direct.cpp:725, rtt_direct.cpp:737). Nor
does it have one made for it: a board with a broker and no AeroAPI key asks Home Assistant for a built-in
airport's board over MQTT (fb_mqtt.cpp:103-136, at most once per airport and half in 15 min and 12 an hour,
fb_mqtt.cpp:24-45), and HA fetches it with the owner's key. The owner, 2026-09-30 22:40: a twin without a key
of its own must not ask. Firmware 2.7.13 has the switch (fbAskHa, /api/export and /api/import only), and it
is an override here; a twin whose firmware has no such switch and would ask is warned about.

Tests: python3 -m unittest tools/twin/test_sync.py -v   (fake devices, no network)
"""
import argparse, json, os, socket, sys, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
AGENT = os.path.join(os.path.dirname(HERE), "agent")      # discover.py, panel.py

PANEL_MAC = "90:E5:B1:D2:0E:2C"      # the panel: discover.py --mac 90:E5:B1:D2:0E:2C
TWIN_MAC = "02:54:57:49:4E:01"       # twin.py MAC

# GET of these only reads. /api/info, /api/portal and /api/export have GET handlers only
# (web.cpp:172, 184, 310), /api/ir is the table (web.cpp:247), and the panel group writes only
# inside `if (isPost())` (web_panel.cpp:381-968; route() at :1392-1399). No query string, ever:
# the GETs that take one change state (web.cpp:191-217, 262-307).
READ_ROUTES = ("/api/info", "/api/portal", "/api/export", "/api/panel", "/api/knob", "/api/lua",
               "/api/worldclock", "/api/railboard", "/api/flightboard", "/api/market", "/api/media",
               "/api/ir", "/api/yachtradar")
REQUIRED = ("/api/info", "/api/portal", "/api/export")
# The twin is written through these alone: POST routes (web.cpp:181, 311; web_panel.cpp:1404-1431)
# and the two GETs that store one setting (web.cpp:191-197 the log switch, :262-270 a remote
# button's function). /reset, /api/reboot and the rest cannot be sent.
POST_ROUTES = ("/save", "/api/import", "/api/panel", "/api/knob", "/api/lua", "/api/worldclock",
               "/api/railboard", "/api/flightboard", "/api/market", "/api/media")
ACTIONS = {"/api/log": ("on",), "/api/ir/fn": ("btn", "fn", "page")}
BODY_MAX = 1024                       # the panel group's JSON bodies (web_panel.cpp:136, 165)
BODY_MAX_MARKET = 8192                # /api/market (web_panel.cpp:841)
SETTLE_S = 3.0   # NVS catches up 2.5 s after a module's change (panel.cpp markDirty, railboard RB_SETTLE_MS)

# /save keeps each of these when the field is absent (`if (server.hasArg(...))`), so they are left out.
FORM_IDENTITY = ("deviceName", "useStaticIP", "staticIP", "gateway", "subnet", "dns1", "dns2")
FORM_SECRET = ("weatherApiKey",)
MAX_METRICS = 20                      # config.h:20
# /api/export's array -> /save's field, numbered from 1 (web.cpp:2047-2120, 2343-2417)
METRIC_FIELDS = (("metricLabels", "label_"), ("metricOrder", "order_"), ("metricCompanions", "companion_"),
                 ("metricPositions", "position_"), ("metricBarPositions", "barPosition_"),
                 ("metricBarMin", "barMin_"), ("metricBarMax", "barMax_"), ("metricBarWidths", "barWidth_"),
                 ("metricBarOffsets", "barOffset_"))
CAROUSEL_KEYS = ("enabled", "idleS", "slotS", "allStyles")              # web_panel.cpp:437-452
KNOB_KEYS = ("reverse", "lockoutMs", "debounceMs", "detent")            # web_panel.cpp:878-900
RB_CFG_KEYS = ("rows", "switch_s", "level", "stale_s", "due_min", "clock_seconds",
               "row_color", "head_color", "due_color")                  # rb_settings.h:105-160
BUDGET_KEYS = ("floor_min", "day_cap", "month_cap")                     # /api/flightboard direct.budget
MARKET_SHOWN = ("display", "display_adv")   # registry groups whose values may be printed

# The airport the override of 2026-09-29 added and selected on the twin, until the owner dropped it on
# 2026-09-30 19:35: never compared, and removed from the twin where it is left (plan_flightboard).
NO_ASK = {"icao": "ZZZZ", "iata": "", "name": "NO REQUESTS"}

# The owner, 2026-09-29 22:50: "1. перенеси все настройки 2. выключи на двойнике" - copy
# everything, then switch off on the twin what must run once in the house. Applied after the copy, over it; the
# copy never writes these names. (name, the value, what it stops) The trains and flights pages and the flight
# board's airport were here too until the owner, 2026-09-30 19:35: now they are copied.
OVERRIDES = (
    ("form.climateHa", False,
     "the twin's indoor sensor as a second device in Home Assistant: switched off, the firmware sends the "
     "empty retained discovery configs, and HA removes the entities (climate.cpp:127-131, 165-182)"),
    # The owner, 2026-09-30 22:40 (firmware 2.7.13, settings.fbAskHa).
    ("export.fbAskHa", False,
     "a twin without an AeroAPI key of its own asking Home Assistant for a flight board over MQTT: each ask is "
     "a fetch HA pays for with the owner's key (fb_mqtt.cpp:24-45, 103-136); switched off, the retained boards "
     "still come, free"),
)
OVERRIDE_NAMES = tuple(o[0] for o in OVERRIDES)

COPY, OVERRIDE, IDENTITY, SECRET, STATE, MANUAL = "copy", "override", "identity", "secret", "state", "manual"
MISSING = object()
HIDDEN = object()     # a value that is not printed


class SyncError(Exception):
    """Something a person can act on; the command stops with it."""


# ---- HTTP -----------------------------------------------------------------------------------------

def http(method, url, body=None, ctype=None, timeout=20.0, tries=6):
    """(status, text) of one request. 503 is the firmware standing aside - a fetch holds the network or
    the heap is short (webBusyRefuse, webRefuseBig; tools/agent/panel.py) - and nothing was done, so it
    is asked again. A POST is never repeated after a transport fault: it may have landed."""
    last = None
    for attempt in range(tries):
        req = urllib.request.Request(url, data=body, method=method,
                                     headers={"Content-Type": ctype} if ctype else {})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            if e.code == 503:
                last = "503, busy"
                time.sleep(float(e.headers.get("Retry-After") or 1) + 0.4 * attempt)
                continue
            return e.code, e.read().decode("utf-8", "replace")
        except (urllib.error.URLError, OSError) as e:
            last = str(getattr(e, "reason", e))
            if method != "GET":
                break
            time.sleep(0.8)
    raise SyncError(f"{method} {url}: no answer ({last})")


def _error_text(text):
    try:
        d = json.loads(text)
        return str(d.get("error") or d.get("message") or "")[:160]
    except Exception:   # noqa: BLE001 - not JSON: a short excerpt
        return text[:160]


class Source:
    """A device that is only read: one GET of a route in READ_ROUTES, no query string. The panel is
    reached through this class alone, and it has no way to send anything else."""

    def __init__(self, address):
        self.address = address

    def get(self, path):
        if path not in READ_ROUTES:
            raise SyncError(f"GET {path}: not a route that only reads; refused")
        status, text = http("GET", f"http://{self.address}{path}")
        if status == 404:
            return None
        if status != 200:
            raise SyncError(f"GET {self.address}{path}: HTTP {status} {_error_text(text)}")
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            raise SyncError(f"GET {self.address}{path}: not JSON") from None


def norm_mac(mac):
    return (mac or "").strip().upper().replace("-", ":")


def same_host(a, b):
    def ip(h):
        try:
            return socket.gethostbyname(h.split(":")[0])
        except OSError:
            return h
    return a == b or ip(a) == ip(b)


def twin_guard(info, address, src_mac, src_address):
    """Why this device must not be written to, or None. Its own /api/info decides, read just now."""
    info = info or {}
    name, mac = info.get("deviceName") or "", norm_mac(info.get("mac"))
    if info.get("model") != "AnimatedPixelClock":
        return "its /api/info is not AnimatedPixelClock's"
    if not name.startswith("TWIN-"):
        return f"its name is {name or 'empty'}, not TWIN-..."
    try:
        first = int(mac.split(":")[0], 16)
    except ValueError:
        return f"its MAC {mac or '(none)'} is not a MAC"
    if not first & 0x02:
        return f"its MAC {mac} is universally administered (a vendor's chip), not the twin's locally administered one"
    if mac == norm_mac(src_mac):
        return f"it has the panel's MAC {mac}"
    if same_host(address, src_address):
        return f"{address} is the panel's address"
    return None


class Twin(Source):
    """The device written to. Constructed only for one whose /api/info passes twin_guard, read here."""

    def __init__(self, address, src_mac, src_address):
        super().__init__(address)
        self.info = self.get("/api/info")
        why = twin_guard(self.info, address, src_mac, src_address)
        if why:
            raise SyncError(f"refusing to write to {address}: {why}")

    def post(self, path, obj):
        if path not in POST_ROUTES or path == "/save":
            raise SyncError(f"POST {path}: not a route this tool writes")
        data = json.dumps(obj, separators=(",", ":")).encode()
        cap = BODY_MAX_MARKET if path == "/api/market" else None if path == "/api/import" else BODY_MAX
        if cap and len(data) > cap:
            raise SyncError(f"POST {path}: {len(data)} bytes, the route takes {cap}")
        return self._send("POST", path, data, "application/json")

    def post_form(self, path, pairs):
        if path != "/save":
            raise SyncError(f"POST {path}: only /save takes a form")
        # application/x-www-form-urlencoded: the WebServer parses every pair, no count limit
        # (arduino-esp32 2.0.17 WebServer Parsing.cpp:161-163, 276-320; its 32 is for multipart).
        return self._send("POST", path, urllib.parse.urlencode(pairs).encode(), "application/x-www-form-urlencoded")

    def action(self, path, params):
        if path not in ACTIONS or set(params) - set(ACTIONS[path]):
            raise SyncError(f"GET {path}: not a setting this tool writes")
        return self._send("GET", f"{path}?{urllib.parse.urlencode(params)}", parse=path != "/api/log")

    def _send(self, method, path, data=None, ctype=None, parse=True):
        status, text = http(method, f"http://{self.address}{path}", data, ctype)
        if status != 200:
            raise SyncError(f"HTTP {status} {_error_text(text)}".strip())
        if not parse:       # /api/log answers with the log itself; its state rides in headers
            return {}
        try:
            doc = json.loads(text) if text else {}
        except json.JSONDecodeError:
            return {}
        if isinstance(doc, dict) and doc.get("success") is False:
            raise SyncError(f"refused: {_error_text(text)}")
        return doc


def snapshot(dev):
    """Every read route of DEV: {route: document, or None when this build has no such route}."""
    snap = {}
    for path in READ_ROUTES:
        snap[path] = dev.get(path)
        if snap[path] is None and path in REQUIRED:
            raise SyncError(f"{dev.address}{path}: 404, so not this firmware")
    return snap


# ---- the settings as one flat dict --------------------------------------------------------------

def _flag(v):
    return "set" if v else "not set"


def city_key(c):
    """A custom city as compared: coordinates to 4 places, since they come back through a float."""
    return [c.get("name"), round(float(c.get("lat", 0)), 4), round(float(c.get("lon", 0)), 4), c.get("tz")]


def settings_of(snap):
    """{name: value} of everything compared. Secrets only as "set" / "not set"."""
    s = {}
    info, portal, export = snap["/api/info"], snap["/api/portal"], snap["/api/export"]
    form = portal.get("form") or {}
    for k, v in form.items():
        if k not in FORM_SECRET:
            s["form." + k] = v
    for arr, _ in METRIC_FIELDS:
        if arr in export:
            s["metrics." + arr] = export[arr]
    for k in ("timezoneString", "gmtOffset", "daylightSaving"):
        if k in export:
            s["tz." + k] = export[k]
    if "spriteColors" in portal:
        s["colors"] = [str(c).lower() for c in portal["spriteColors"]]
    s["logOn"] = bool(info.get("logOn"))
    s["identity.mac"] = norm_mac(info.get("mac"))
    s["firmware"] = f"{info.get('version')} ({info.get('build')})"
    s["secret.weatherApiKey"] = _flag(export.get("weatherApiKey") or form.get("weatherApiKey"))
    if "fbAskHa" in export:                                  # firmware 2.7.13; only in export and import
        s["export.fbAskHa"] = export["fbAskHa"]

    pn = snap.get("/api/panel")
    if pn:
        for p in pn.get("pages", []):
            if p.get("key") and "pages." + p["key"] not in s:
                s["pages." + p["key"]] = bool(p.get("on"))
        if "cardsOn" in pn:
            s["pages.cards"] = bool(pn["cardsOn"])
        car = pn.get("carousel") or {}
        s["carousel"] = {k: car[k] for k in CAROUSEL_KEYS if k in car}
    kn = snap.get("/api/knob")
    if kn:
        s["knob"] = {k: kn[k] for k in KNOB_KEYS if k in kn}
    lua = snap.get("/api/lua")
    if lua:
        walk = lua.get("inWalk") or []
        for i, name in enumerate(lua.get("effects") or []):
            s["lua.walk." + name] = bool(walk[i]) if i < len(walk) else True
        s["lua.scripts"] = sorted(x.get("name") for x in (lua.get("uploaded") or {}).get("scripts", []))
    wc = snap.get("/api/worldclock")
    if wc:
        cities = wc.get("cities") or []
        s["worldclock.cities"] = [city_key(c) for c in sorted(cities, key=lambda c: c.get("id", 0))
                                  if c.get("kind") == "custom"]
        home = next((c for c in cities if c.get("id") == wc.get("home")), None)
        s["worldclock.home"] = home.get("name") if wc.get("homeChosen") and home else None
    rb = snap.get("/api/railboard")
    if rb:
        s["railboard.crs"] = rb.get("crs")
        s["railboard.favourites"] = list(rb.get("favourites") or [])
        cfg = rb.get("cfg") or {}
        # "web": the portal set them and NVS holds them; otherwise Home Assistant's or the build's
        # (railboard.cpp:1103), which are not this device's settings to copy.
        s["railboard.config"] = ({k: cfg.get(k) for k in RB_CFG_KEYS} if cfg.get("from") == "web"
                                 else "Home Assistant's or the build's")
        s["secret.rttToken"] = _flag((rb.get("direct") or {}).get("token"))
        s["secret.mqttLogin"] = _flag((rb.get("mqtt") or {}).get("configured"))
    fb = snap.get("/api/flightboard")
    if fb:
        apts = {a.get("id"): a for a in fb.get("airports") or []}
        sel = apts.get(fb.get("airport")) or {}
        s["flightboard.selection"] = [sel.get("code"), fb.get("dir")]
        s["flightboard.custom"] = sorted([a.get("code"), a.get("iata", ""), a.get("name", ""), a.get("tz", "")]
                                         for a in apts.values() if a.get("kind") == "custom")
        d = fb.get("direct") or {}
        if d.get("built", True) and isinstance(d.get("budget"), dict):
            s["flightboard.budget"] = {k: d["budget"].get(k) for k in BUDGET_KEYS}
        s["flightboard.tracked"] = sorted(t.get("ident") for t in fb.get("tracked") or [])
        s["secret.aeroApiKey"] = _flag(d.get("key"))
    mk = snap.get("/api/market")
    if mk and isinstance(mk.get("config"), dict):
        for k, v in mk["config"].items():
            s["market." + k] = v
    md = snap.get("/api/media")
    if md:
        s["media.selected"] = md.get("selected") or ""
    ir = snap.get("/api/ir")
    if ir:
        for b in ir.get("buttons") or []:
            s[f"ir.{b.get('n')}.fn"] = [b.get("fn"), b.get("page")]
            s[f"ir.{b.get('n')}.code"] = f"{b.get('proto')} {b.get('code')}" if b.get("bound") else "not learned"
    yr = snap.get("/api/yachtradar")
    if yr:
        s["secret.aisKey"] = _flag(yr.get("keyPresent"))
    return s


def kind_of(name):
    if name in OVERRIDE_NAMES:
        return OVERRIDE
    if name.startswith("secret."):
        return SECRET
    if name.startswith("identity.") or (name.startswith("form.") and name[5:] in FORM_IDENTITY):
        return IDENTITY
    if name == "firmware":
        return STATE
    if name == "lua.scripts" or (name.startswith("ir.") and name.endswith(".code")):
        return MANUAL
    return COPY


def override_value(name, src, dst):
    for n, v, _ in OVERRIDES:
        if n == name:
            return v
    raise KeyError(name)


def private_names(*snaps):
    """market.* names whose values are not printed: all but the display groups (market_settings.h
    Group; the registry's rows name each key's group). A key hidden on either device stays hidden."""
    hidden = set()
    for snap in snaps:
        mk = snap.get("/api/market") or {}
        groups = {r.get("key"): r.get("group") for r in ((mk.get("registry") or {}).get("rows") or [])}
        hidden |= {"market." + k for k in (mk.get("config") or {}) if groups.get(k) not in MARKET_SHOWN}
    return hidden


def show(value, width=64):
    if value is MISSING:
        return "(none)"
    if value is HIDDEN:
        return "(not printed)"
    text = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    return text if len(text) <= width else text[:width - 3] + "..."


def show_change(a, b):
    """now -> after; for long lists only the entries that differ."""
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b) and len(a) > 4:
        idx = [i for i in range(len(a)) if a[i] != b[i]]
        parts = [f"[{i}] {show(a[i], 24)} -> {show(b[i], 24)}" for i in idx[:4]]
        return ", ".join(parts) + (f", and {len(idx) - 4} more" if len(idx) > 4 else "")
    return f"{show(a)} -> {show(b)}"


# ---- the plan -------------------------------------------------------------------------------------

class Step:
    """One request to the twin. method: "form" (POST /save), "json" (POST) or "action" (a GET that
    stores one setting)."""

    def __init__(self, label, method, path, body, covers, override=False):
        self.label, self.method, self.path, self.body = label, method, path, body
        self.covers, self.override = list(covers), override

    def run(self, twin):
        if self.method == "form":
            return twin.post_form(self.path, self.body)
        if self.method == "json":
            return twin.post(self.path, self.body)
        return twin.action(self.path, self.body)


class Plan:
    def __init__(self, ssnap, dsnap):
        self.ssnap, self.dsnap = ssnap, dsnap
        self.src, self.dst = settings_of(ssnap), settings_of(dsnap)
        self.private = private_names(ssnap, dsnap)
        self.steps, self.changes, self.notes, self.warnings, self.overridden = [], [], [], [], []
        self.done = set()
        self.view = dict(self.dst)
        if "flightboard.custom" in self.view:    # the override's own airport is not a copied one
            self.view["flightboard.custom"] = [a for a in self.view["flightboard.custom"] if a[0] != NO_ASK["icao"]]
        self.todo = set()
        for n in sorted(set(self.src) | set(self.view)):
            k = kind_of(n)
            a, b = self.src.get(n, MISSING), self.view.get(n, MISSING)
            if k == OVERRIDE or a == b:
                continue
            if k == COPY:
                self.todo.add(n)
            else:
                self.note(n, k, self.other_text(n, k, a, b))

    def other_text(self, n, k, a, b):
        if k == SECRET:
            return f"panel {show(a)}, twin {show(b)}: never copied"
        if n == "lua.scripts":
            only_p = sorted(set(a if a is not MISSING else []) - set(b if b is not MISSING else []))
            only_t = sorted(set(b if b is not MISSING else []) - set(a if a is not MISSING else []))
            return (f"only on the panel: {', '.join(only_p) or '-'}; only on the twin: {', '.join(only_t) or '-'}. "
                    "No route gives a script's source (web_panel.cpp:968-1230): upload from the gallery")
        if k == MANUAL:
            return (f"panel {show(a)}, twin {show(b)}: learned from a real frame; "
                    "on the twin: tools/twin/learn_remote.txt")
        if k == STATE:
            return f"panel {show(a)}, twin {show(b)}"
        return f"panel {show(a)}, twin {show(b)}: the twin keeps its own"

    def value(self, n, v):
        return HIDDEN if n in self.private and v is not MISSING else v

    def note(self, name, kind, text):
        self.notes.append((name, kind, text))
        self.done.add(name)

    def step(self, label, method, path, body, covers, override=False):
        self.steps.append(Step(label, method, path, body, covers, override))
        for n in covers:
            if n in self.done:
                continue
            self.done.add(n)
            after = override_value(n, self.src, self.dst) if override else self.src.get(n, MISSING)
            self.changes.append((n, OVERRIDE if override else COPY, self.view.get(n, MISSING), after))

    def both(self, prefix):
        return sorted(n for n in self.todo if n.startswith(prefix) and n in self.src and n in self.dst
                      and n not in self.done)

    def finish(self):
        for n in sorted(self.todo - self.done):
            if n not in self.dst:
                self.note(n, COPY, "the twin's firmware has none of this")
            elif n not in self.src:
                self.note(n, COPY, "only the twin's firmware has this; kept")
            else:
                self.note(n, COPY, "no route copies it")


def save_form(ssnap, dsnap):
    """The /save form: see the module's docstring. Pairs of (field, text)."""
    sform = ssnap["/api/portal"].get("form") or {}
    dform = dsnap["/api/portal"].get("form") or {}
    pairs = []
    for k, twin_value in dform.items():
        if k in FORM_IDENTITY or k in FORM_SECRET:
            continue
        v = twin_value if ("form." + k) in OVERRIDE_NAMES or k not in sform else sform[k]
        if isinstance(v, bool):
            if v:
                pairs.append((k, "1"))        # /save reads a checkbox by hasArg alone
        elif v is not None:
            pairs.append((k, str(v)))
    sexp, dexp = ssnap["/api/export"], dsnap["/api/export"]
    for arr, field in METRIC_FIELDS:
        values = sexp.get(arr, dexp.get(arr))
        for i, v in enumerate((values or [])[:MAX_METRICS]):
            pairs.append((f"{field}{i + 1}", str(v)))     # an empty label clears it, as on the panel
    colors = ssnap["/api/portal"].get("spriteColors") or dsnap["/api/portal"].get("spriteColors") or []
    # RGB565 -> "#rrggbb" (web.cpp:962-967) and back (web.cpp:2229-2231) is exact for every value.
    for slot, c in enumerate(colors):
        pairs.append((f"color_{slot}", str(c)))
    return pairs


def plan_save(p):
    names = [n for n in sorted(p.todo) if n.startswith(("form.", "metrics.", "tz.")) or n == "colors"]
    names = [n for n in names if n in p.src and n in p.dst]
    region = (p.ssnap["/api/portal"].get("form") or {}).get("timezoneRegion", -1)
    custom_zone = not isinstance(region, int) or region < 0
    save = [n for n in names if not (n.startswith("tz.") and custom_zone)]
    if save:
        pairs = save_form(p.ssnap, p.dsnap)
        shown = ", ".join(n.split(".", 1)[-1] for n in save[:6]) + (f", +{len(save) - 6}" if len(save) > 6 else "")
        p.step(f"POST /save  the whole form, {len(pairs)} fields; changes {shown}", "form", "/save", pairs, save)
    zone = [n for n in names if n.startswith("tz.") and custom_zone]
    if zone:
        exp = p.ssnap["/api/export"]
        body = {k: exp[k] for k in ("timezoneString", "gmtOffset", "daylightSaving") if k in exp}
        p.step("POST /api/import  the panel's zone string, which is no region of the list", "json", "/api/import",
               body, zone)


def plan_panel(p):
    for n in p.both("pages."):
        key, on = n[len("pages."):], p.src[n]
        p.step(f"POST /api/panel  page {key} {'on' if on else 'off'}", "json", "/api/panel",
               {"enable": {"key": key, "on": on}}, [n])
    if p.both("carousel"):
        body = {"carousel": dict(p.src["carousel"])}
        p.step(f"POST /api/panel  {show(body)}", "json", "/api/panel", body, ["carousel"])
    if p.both("knob"):
        p.step(f"POST /api/knob  {show(p.src['knob'])}", "json", "/api/knob", dict(p.src["knob"]), ["knob"])


def plan_lua(p):
    effects = (p.dsnap.get("/api/lua") or {}).get("effects") or []
    for n in p.both("lua.walk."):
        name = n[len("lua.walk."):]
        p.step(f"POST /api/lua  effect {name} {'in' if p.src[n] else 'out of'} the walk", "json", "/api/lua",
               {"walk": {"i": effects.index(name), "on": p.src[n], "name": name}}, [n])
    # An effect on one device only: its script is missing, which the lua.scripts note says.
    p.done |= {n for n in p.todo if n.startswith("lua.walk.")}


def plan_worldclock(p):
    sdoc, ddoc = p.ssnap.get("/api/worldclock") or {}, p.dsnap.get("/api/worldclock") or {}
    home = p.src.get("worldclock.home")
    added_home = False
    if p.both("worldclock.cities"):
        want, have = p.src["worldclock.cities"], p.dst["worldclock.cities"]
        for c in ddoc.get("cities") or []:
            if c.get("kind") == "custom" and city_key(c) not in want:
                p.step(f"POST /api/worldclock  remove {c.get('name')} ({c.get('id')})", "json", "/api/worldclock",
                       {"remove": c.get("id")}, ["worldclock.cities"])
        for c in sorted(sdoc.get("cities") or [], key=lambda c: c.get("id", 0)):
            if c.get("kind") != "custom" or city_key(c) in have:
                continue
            add = {k: c.get(k) for k in ("name", "lat", "lon", "tz")}
            if c.get("name") == home:
                add["home"] = added_home = True
            label = (f"add {add['name']} ({add['lat']}, {add['lon']}, {add['tz']})"
                     + (" as home" if add.get("home") else ""))
            p.step(f"POST /api/worldclock  {label}", "json", "/api/worldclock", {"add": add},
                   ["worldclock.cities"] + (["worldclock.home"] if add.get("home") else []))
    if p.both("worldclock.home") and not added_home:
        if home is None:
            p.step("POST /api/worldclock  home by location", "json", "/api/worldclock", {"auto": True},
                   ["worldclock.home"])
        else:
            city = next((c for c in ddoc.get("cities") or [] if c.get("name") == home), None)
            if city is None:
                p.note("worldclock.home", COPY, f"the twin has no city {home} to make home")
            else:
                # A new home is put on screen (web_panel.cpp:764-771).
                p.step(f"POST /api/worldclock  home {home} ({city.get('id')}); the twin shows it",
                       "json", "/api/worldclock", {"home": city.get("id")}, ["worldclock.home"])


def plan_railboard(p):
    if p.both("railboard.crs"):
        # A new station goes out retained on nickoscope_matrix/railboard/select (railboard.cpp:386-396):
        # the panel's own, so Home Assistant hears what it already has.
        crs = p.src["railboard.crs"]
        p.step(f"POST /api/railboard  station {crs}", "json", "/api/railboard", {"crs": crs}, ["railboard.crs"])
    if p.both("railboard.favourites"):
        fav = p.src["railboard.favourites"]
        p.step(f"POST /api/railboard  favourites {show(fav)}", "json", "/api/railboard", {"favourites": fav},
               ["railboard.favourites"])
    if p.both("railboard.config"):
        cfg = p.src["railboard.config"]
        if isinstance(cfg, dict):
            p.step(f"POST /api/railboard  config {show(cfg)}", "json", "/api/railboard", {"config": cfg},
                   ["railboard.config"])
        else:
            p.note("railboard.config", COPY, "the panel follows Home Assistant's settings, the twin its portal's; "
                   "{\"reset\":true} would hand them back but also erases the favourites in NVS "
                   "(railboard.cpp:451-459, 504-508): not sent")


def plan_flightboard(p):
    fb = p.dsnap.get("/api/flightboard") or {}
    sel = p.src.get("flightboard.selection") if p.both("flightboard.selection") else None
    if p.both("flightboard.custom"):
        have = {a[0] for a in p.view["flightboard.custom"]}
        want = {a[0] for a in p.src["flightboard.custom"]}
        for icao, iata, name, tz in p.src["flightboard.custom"]:
            if icao not in have:
                add = {"icao": icao, "iata": iata, "name": name, "tz": tz}
                body, covers = {"add": add}, ["flightboard.custom"]
                if sel and sel[0] == icao:           # the panel's airport is this new one: added and selected
                    add["select"] = True
                    body["dir"] = sel[1]
                    covers.append("flightboard.selection")
                    sel = None
                p.step(f"POST /api/flightboard  add airport {show(add)}", "json", "/api/flightboard", body, covers)
        extra = sorted(have - want)
        if extra:
            p.note("flightboard.custom", COPY, f"the twin also has {', '.join(extra)}: not removed - a removal "
                   "resubscribes the board, which may ask Home Assistant for one (panel.cpp:576-585)")
    if sel:
        icao, direction = sel
        apt = next((a for a in fb.get("airports") or [] if a.get("code") == icao), None)
        if apt is None:
            p.note("flightboard.selection", COPY, f"the twin has no airport {icao} to select")
        else:
            p.step(f"POST /api/flightboard  select {icao} ({apt.get('id')}), dir {direction}", "json", "/api/flightboard",
                   {"airport": apt.get("id"), "dir": direction}, ["flightboard.selection"])
    # What the old override added: gone from the twin once another airport is selected there, unless the panel
    # has one such itself.
    old = next((a for a in fb.get("airports") or [] if a.get("kind") == "custom" and a.get("code") == NO_ASK["icao"]
                and a.get("name") == NO_ASK["name"]), None)
    panel_has = any(a.get("code") == NO_ASK["icao"] for a in (p.ssnap.get("/api/flightboard") or {}).get("airports") or [])
    if old is not None and not panel_has and (p.src.get("flightboard.selection") or [None])[0] != NO_ASK["icao"]:
        p.step(f"POST /api/flightboard  remove {NO_ASK['icao']} \"{NO_ASK['name']}\" ({old.get('id')}), the old override's "
               "airport", "json", "/api/flightboard", {"remove": old.get("id")}, [])
    if p.both("flightboard.budget"):
        b = p.src["flightboard.budget"]
        p.step(f"POST /api/flightboard  budget {show(b)}", "json", "/api/flightboard", {"budget": b},
               ["flightboard.budget"])
    if p.both("flightboard.tracked"):
        want, have = p.src["flightboard.tracked"], p.dst["flightboard.tracked"]
        for ident in have:
            if ident not in want:
                p.step(f"POST /api/flightboard  untrack {ident}", "json", "/api/flightboard", {"untrack": ident},
                       ["flightboard.tracked"])
        for ident in want:
            if ident not in have:
                p.step(f"POST /api/flightboard  track {ident}", "json", "/api/flightboard", {"track": ident},
                       ["flightboard.tracked"])


def plan_market(p):
    names = p.both("market.")
    chunk, chunks = {}, []
    for n in names:
        key = n[len("market."):]
        trial = dict(chunk, **{key: p.src[n]})
        if chunk and len(json.dumps({"config": trial}, separators=(",", ":"))) > BODY_MAX_MARKET:
            chunks.append(chunk)
            trial = {key: p.src[n]}
        chunk = trial
    if chunk:
        chunks.append(chunk)
    for c in chunks:
        # The keys only: the values can be the owner's money.
        p.step(f"POST /api/market  config, {len(c)} key(s): {', '.join(sorted(c))}", "json", "/api/market",
               {"config": c}, ["market." + k for k in c])


def plan_media(p):
    if p.both("media.selected"):
        sel = p.src["media.selected"]
        if sel:
            p.step(f"POST /api/media  select {sel}", "json", "/api/media", {"select": sel}, ["media.selected"])
        else:
            p.note("media.selected", COPY, "the panel has none selected, and select takes an entity id only "
                   "(media_ha.cpp:478-480): the twin's stays")


def plan_ir(p):
    for n in p.both("ir."):
        if not n.endswith(".fn"):
            continue
        fn, page = p.src[n]
        params = {"btn": n.split(".")[1], "fn": fn}
        if fn == "page":
            params["page"] = page
        p.step(f"GET /api/ir/fn?{urllib.parse.urlencode(params)}", "action", "/api/ir/fn", params, [n])


def plan_log(p):
    if p.both("logOn"):
        on = "1" if p.src["logOn"] else "0"
        p.step(f"GET /api/log?on={on}  the network log", "action", "/api/log", {"on": on}, ["logOn"])


def plan_overrides(p):
    for name, _, why in OVERRIDES:
        want = override_value(name, p.src, p.dst)
        have = p.dst.get(name, MISSING)
        if have is MISSING:
            p.note(name, OVERRIDE, "the twin's firmware has none of this; nothing to switch off")
            continue
        if have == want:
            p.overridden.append((name, have, why))
            p.done.add(name)
            continue
        if name == "form.climateHa":
            p.step("POST /api/import  {\"climateHa\":false}", "json", "/api/import", {"climateHa": False}, [name], True)
        if name == "export.fbAskHa":
            p.step("POST /api/import  {\"fbAskHa\":false}", "json", "/api/import", {"fbAskHa": False}, [name], True)


def asks_ha(dsnap):
    """Whether the twin would ask Home Assistant for flight boards and nothing can stop it: its firmware has no
    fbAskHa, a broker is set (mqtt.configured), it has no AeroAPI key (or no direct fetch built), and the airport
    shown is a built-in one - the only kind HA serves (fb_mqtt.cpp mayAsk)."""
    exp, fb = dsnap.get("/api/export") or {}, dsnap.get("/api/flightboard") or {}
    if "fbAskHa" in exp or not (fb.get("mqtt") or {}).get("configured"):
        return False
    direct = fb.get("direct") or {}
    if direct.get("built") is not False and direct.get("key"):
        return False
    sel = next((a for a in fb.get("airports") or [] if a.get("id") == fb.get("airport")), {})
    return sel.get("kind") == "builtin"


def make_plan(ssnap, dsnap):
    p = Plan(ssnap, dsnap)
    for part in (plan_save, plan_panel, plan_lua, plan_worldclock, plan_railboard, plan_flightboard,
                 plan_market, plan_media, plan_ir, plan_log):
        part(p)
    p.finish()
    plan_overrides(p)          # last, over the copy
    si, di = ssnap["/api/info"], dsnap["/api/info"]
    if (si.get("version"), si.get("build")) != (di.get("version"), di.get("build")):
        p.warnings.append(f"the panel runs {si.get('version')} ({si.get('build')}), the twin {di.get('version')} "
                          f"({di.get('build')}): a field only one of them has is listed below, not copied")
    if asks_ha(dsnap):
        p.warnings.append(f"the twin has an MQTT broker and no AeroAPI key, and its firmware {di.get('version')} has no "
                          "fbAskHa (2.7.13): its flight board asks Home Assistant for a built-in airport's board, and HA "
                          "pays for it with the owner's key - update the twin's firmware, or remove the broker from its settings")
    car = p.src.get("carousel") or {}
    if car.get("enabled") and car.get("allStyles"):
        p.warnings.append("the panel's carousel walks the clock styles, so its clockStyle is the one on screen now, "
                          "which the carousel does not store (clock_style.cpp:52-62)")
    return p


# ---- printing ---------------------------------------------------------------------------------------

def header(role, addr, info, extra=""):
    print(f"{role:5} {info.get('deviceName')}  {norm_mac(info.get('mac'))}  {addr}  "
          f"{info.get('version')} ({info.get('build')}){extra}")


RAIL_LIMIT = ("limit: the rail board still publishes its station, retained, on every broker connect "
              "(railboard.cpp:555-569), and no setting stops it; the twin holds the panel's station, so run this "
              "again after changing the panel's")


def wrap(text, indent, width=108):
    words, lines, line = text.split(), [], ""
    for w in words:
        if line and len(indent) + len(line) + 1 + len(w) > width:
            lines.append(line)
            line = w
        else:
            line = f"{line} {w}" if line else w
    return "\n".join(indent + x for x in lines + [line])


def print_plan(p, steps=True, why=True):
    for w in p.warnings:
        print(f"note: {w}")
    copies = [c for c in p.changes if c[1] == COPY]
    print(f"\ncopy from the panel ({len(copies)}):" if copies else "\ncopy from the panel: nothing differs")
    for n, _, a, b in copies:
        print(f"  {n:38} {show_change(p.value(n, a), p.value(n, b))}")
    print("\nthe owner's overrides (2026-09-29 22:50, 2026-09-30 22:40), applied last:")
    reasons = {n: r for n, _, r in OVERRIDES}
    for n, _, a, b in (c for c in p.changes if c[1] == OVERRIDE):
        print(f"  {n:38} {show_change(a, b)}")
        if why:
            print(wrap(reasons[n], " " * 6))
    for n, v, _ in p.overridden:
        print(f"  {n:38} {show(v)} on the twin, as the override wants (the panel: {show(p.src.get(n, MISSING))})")
    for n, k, text in (x for x in p.notes if x[1] == OVERRIDE):
        print(f"  {n:38} {text}")
    if why:
        print(wrap(RAIL_LIMIT, "  "))
    others = [x for x in p.notes if x[1] != OVERRIDE]
    if others:
        print("\nnot copied:")
        for n, k, text in others:
            print(f"  {k:8} {n:29} {text}")
    print("  state    (never compared)              metric names, usage counters, market record, icons, "
          "crash report, uptime")
    if steps:
        print(f"\nrequests to the twin, in order ({len(p.steps)}):" if p.steps else "\nrequests to the twin: none")
        for i, s in enumerate(p.steps, 1):
            print(f"  {i:2}. {'[override] ' if s.override else ''}{s.label}")


def locate(role, given, default_mac):
    """The address of the device GIVEN names (a MAC, a name, an address), or of DEFAULT_MAC."""
    if AGENT not in sys.path:
        sys.path.insert(0, AGENT)
    import panel as P     # tools/agent/panel.py: mDNS by MAC, or an address taken at its word
    try:
        addr = P.resolve(given or default_mac)["address"]
    except P.PanelError as e:
        raise SyncError(f"the {role}: {e}") from None
    # resolve() takes anything with a dot as an address, so a path or a query could ride in on it and
    # reach the panel through Source.get's f-string: only a host or an IP, with an optional port.
    u = urllib.parse.urlsplit(f"http://{addr}")
    if u.path or u.query or u.fragment or "@" in u.netloc or not u.hostname:
        raise SyncError(f"the {role}: {addr!r} is not a host or an address")
    return addr


def run(a):
    src_addr = locate("panel", a.panel, PANEL_MAC)
    dst_addr = locate("twin", a.twin, TWIN_MAC)
    source, twin_read = Source(src_addr), Source(dst_addr)       # both only read until the guard passes
    ssnap, dsnap = snapshot(source), snapshot(twin_read)
    smac = norm_mac(ssnap["/api/info"].get("mac"))
    if not a.panel and smac != PANEL_MAC:
        raise SyncError(f"{src_addr} answers with MAC {smac}, not the panel's {PANEL_MAC}")
    why = twin_guard(dsnap["/api/info"], dst_addr, smac, src_addr)
    header("panel", src_addr, ssnap["/api/info"], "  read only")
    header("twin", dst_addr, dsnap["/api/info"],
           f"  REFUSED: {why}" if why else "  writable: TWIN- name, locally administered MAC, not the panel")
    p = make_plan(ssnap, dsnap)
    if a.mode == "diff":
        print_plan(p, steps=False)
        return 0
    print_plan(p)
    if a.mode == "dry-run":
        print("\ndry run: nothing was sent (--apply sends it)")
        return 0
    if why:
        print(f"\nnot applied: {why}", file=sys.stderr)
        return 2
    twin = Twin(dst_addr, smac, src_addr)        # reads /api/info again and guards again
    print("\napplying:")
    failed = 0
    for i, s in enumerate(p.steps, 1):
        try:
            s.run(twin)
            print(f"  {i:2}. ok      {s.label}")
        except SyncError as e:
            failed += 1
            print(f"  {i:2}. FAILED  {s.label}: {e}")
    time.sleep(SETTLE_S)
    after = make_plan(snapshot(source), snapshot(twin))
    print("\nafter: what still differs")
    print_plan(after, steps=False, why=False)
    if after.steps:
        print(f"\nNOT DONE: {len(after.steps)} request(s) would still be needed:")
        for s in after.steps:
            print(f"  - {s.label}")
    ok = not failed and not after.steps
    print("\n" + ("done: everything copied; the rest differs by design (above)" if ok else
                  f"incomplete: {failed} request(s) failed, {len(after.steps)} still needed"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", dest="mode", action="store_const", const="dry-run",
                      help="print the plan, send nothing (the default)")
    mode.add_argument("--apply", dest="mode", action="store_const", const="apply", help="do it on the twin")
    mode.add_argument("--diff", dest="mode", action="store_const", const="diff", help="only the differences")
    ap.add_argument("--panel", help=f"the source: MAC, name or address (default {PANEL_MAC})")
    ap.add_argument("--twin", help=f"the target: MAC, name or address (default {TWIN_MAC})")
    ap.set_defaults(mode="dry-run")
    a = ap.parse_args(argv)
    try:
        return run(a)
    except SyncError as e:
        print(f"sync.py: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())

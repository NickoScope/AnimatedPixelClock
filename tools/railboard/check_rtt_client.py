#!/usr/bin/env python3
"""Host checks for tools/railboard/rtt_client.py and the AppDaemon app.

A fake Realtime Trains on 127.0.0.1 answers the two endpoints the way
main.yml describes them (lines 1018-1169, 1644-1669), and the client is run
through each case it has to handle: a refresh token used as an access token
(401, exchange, retry), the access token reused, re-exchanged before it
expires, a refused token (no hammering afterwards), a 429 with Retry-After,
a 204, a bad body, and the shell_command path end to end. The tokens are
made-up strings; the check also requires that none of them appears in
anything the client returns, prints or saves except the state file's access
token. Then the AppDaemon app runs once against stand-in appdaemon and paho
modules and must publish the three retained topics.

  python3 tools/railboard/check_rtt_client.py
"""
import contextlib
import importlib
import io
import json
import os
import pathlib
import sys
import tempfile
import threading
import types
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "appdaemon"))
import gen_rtt_fixture as gen   # noqa: E402
import rtt_client as rtt        # noqa: E402

REFRESH, ACCESS, WRONG = "FAKE-REFRESH-7f3a", "FAKE-ACCESS-19bc", "FAKE-WRONG-0000"
SMALL = gen.SMALL.read_bytes()


class Fake:
    mode = "normal"          # normal | rate | empty | garbage
    valid_s = 3600
    log = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        auth = self.headers.get("Authorization", "")
        path = self.path.split("?")[0]
        Fake.log.append((path, auth))
        quota = [("X-RateLimit-Remaining-Day", "4321"), ("X-RateLimit-Limit-Day", "5000")]
        if path == rtt.ACCESS_TOKEN:
            if auth == "Bearer " + REFRESH:
                until = rtt._dt.datetime.fromtimestamp(Fake.now + Fake.valid_s, rtt._dt.timezone.utc)
                body = json.dumps({"token": ACCESS, "entitlements": [], "validUntil": until.isoformat()}).encode()
                return self._send(200, body, quota)
            return self._send(401)
        if path == rtt.LOCATION:
            if auth != "Bearer " + ACCESS:
                return self._send(401)
            if Fake.mode == "rate":
                return self._send(429, b"", [("Retry-After", "120")])
            if Fake.mode == "empty":
                return self._send(204, b"", quota)
            if Fake.mode == "garbage":
                return self._send(200, b"<html>maintenance</html>", quota)
            return self._send(200, SMALL, quota)
        return self._send(404)


fails = 0


def expect(cond, what):
    global fails
    print(("  ok    " if cond else "  FAIL  ") + what)
    fails += not cond


def no_tokens(blob, what):
    s = blob if isinstance(blob, str) else json.dumps(blob)
    expect(REFRESH not in s and ACCESS not in s and WRONG not in s, "no token in " + what)


def main():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    rtt.API = "http://127.0.0.1:%d" % srv.server_address[1]      # read at request time
    Fake.now = gen.NOW
    clock = [gen.NOW]

    print("refresh token, kind auto")
    c = rtt.RttClient("Bearer " + REFRESH, kind="auto", clock=lambda: clock[0])
    Fake.log.clear()
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "" and r["code"] == 200, "401, exchange, then 200")
    expect([p for p, _ in Fake.log] == [rtt.LOCATION, rtt.ACCESS_TOKEN, rtt.LOCATION], "three requests in that order")
    expect(r["kind"] == "refresh-exchanged" and r["rl"] == "4321" and r["limit"] == "5000", "kind and quota reported")
    lists = rtt.normalise(r["content"], clock[0], "GLD")
    expect(lists is not None and len(lists["dep"]) == 8 and len(lists["arr"]) == 5, "the small fixture's 8 and 5 rows")
    r.pop("content")
    no_tokens(r, "the result")
    no_tokens(repr(c), "repr()")

    Fake.log.clear()
    clock[0] += 30
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "" and len(Fake.log) == 1, "the access token is reused: one request")

    Fake.log.clear()
    clock[0] += Fake.valid_s - 200       # inside the 5 min margin
    Fake.now = clock[0]
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "" and [p for p, _ in Fake.log] == [rtt.LOCATION, rtt.ACCESS_TOKEN, rtt.LOCATION],
           "near validUntil it is not used again: exchanged anew")

    print("kind refresh")
    c = rtt.RttClient(REFRESH, kind="refresh", clock=lambda: clock[0])
    Fake.log.clear()
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "" and [p for p, _ in Fake.log] == [rtt.ACCESS_TOKEN, rtt.LOCATION], "exchange first, then the line-up")

    print("a refused token")
    c = rtt.RttClient(WRONG, kind="auto", clock=lambda: clock[0])
    Fake.log.clear()
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "AUTH" and r["kind"] == "refused" and r["backoff"] == rtt.AUTH_FIRST_S, "AUTH, refused, 15 min back-off")
    expect(len(Fake.log) == 2, "two requests: the line-up and the exchange")
    Fake.log.clear()
    for _ in range(30):                   # 10 minutes of 20 s polls
        clock[0] += 20
        r = c.fetch("GLD", clock[0])
    expect(len(Fake.log) == 0 and r["err"] == "AUTH", "no request at all during the back-off")
    clock[0] += rtt.AUTH_FIRST_S
    r = c.fetch("GLD", clock[0])
    expect(r["backoff"] == 2 * rtt.AUTH_FIRST_S, "refused again: the back-off doubles")

    print("429, 204, a bad body")
    c = rtt.RttClient(REFRESH, kind="refresh", clock=lambda: clock[0])
    Fake.mode = "rate"
    Fake.now = clock[0]
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "RATE" and r["retry"] == 120 and r["backoff"] == 120, "RATE with Retry-After 120")
    Fake.log.clear()
    clock[0] += 60
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "RATE" and not Fake.log, "no request before Retry-After has passed")
    clock[0] += 61
    Fake.mode = "empty"
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "" and r["code"] == 204 and rtt.normalise(r["content"], clock[0], "GLD")["dep"] == [],
           "204: valid and empty")
    Fake.mode = "garbage"
    r = c.fetch("GLD", clock[0])
    expect(r["err"] == "BAD", "a 200 that is not a line-up: BAD")
    Fake.mode = "normal"

    print("network failure")
    saved, rtt.API = rtt.API, "http://127.0.0.1:9"
    r = rtt.RttClient(REFRESH, kind="refresh", timeout=2).fetch("GLD")
    expect(r["err"] == "NET" and r["code"] == 0, "nothing listening: NET")
    rtt.API = saved

    print("shell_command path")
    with tempfile.TemporaryDirectory() as tmp:
        secrets = os.path.join(tmp, "secrets.yaml")
        state = os.path.join(tmp, "state.json")
        with open(secrets, "w") as f:
            f.write('other: "x"\nrtt_bearer: "Bearer %s"\n' % REFRESH)
        Fake.now = int(rtt.time.time())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rtt._cli(["fetch", "GLD", "--secrets", secrets, "--state", state])
        out = json.loads(buf.getvalue())
        expect(out["err"] == "" and out["departures"]["dir"] == "dep" and out["arrivals"]["crs"] == "GLD"
               and out["status"]["code"] == 200, "one JSON object with both payloads and the status")
        no_tokens(buf.getvalue(), "stdout")
        expect(oct(os.stat(state).st_mode & 0o777) == "0o600", "state file 0600")
        st = json.load(open(state))
        expect(st["access"] == ACCESS and REFRESH not in json.dumps(st), "state keeps the access token, never the refresh token")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rtt._cli(["fetch", "gld", "--secrets", secrets, "--state", state])
        out = json.loads(buf.getvalue())
        expect(out["err"] == "BAD" and out.get("error") == "ValueError" and "departures" not in out, "a bad code refused")
        with open(secrets, "w") as f:
            f.write('rtt_bearer: "Bearer %s"\nrtt_bearer: "twice"\n' % REFRESH)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rtt._cli(["fetch", "GLD", "--secrets", secrets, "--state", state])
        out = json.loads(buf.getvalue())
        expect(out.get("error") == "LookupError", "two rtt_bearer lines refused")
        no_tokens(buf.getvalue(), "stdout on a secrets error")

    print("AppDaemon app, with stand-in appdaemon and paho")
    published = []
    logs = []

    class Hass:
        def __init__(self, args):
            self.args = args

        def log(self, msg, level="INFO"):
            logs.append((level, msg))

        def get_state(self, entity):
            return "GLD"

        def listen_state(self, cb, entity):
            self.listener = cb

        def run_every(self, cb, start, interval):
            self.poller, self.interval = cb, interval

        def run_in(self, cb, delay, **kw):
            cb(kw)

    class Client:
        def __init__(self, *a, **k):
            pass

        def username_pw_set(self, u, p):
            pass

        def connect_async(self, host, port, keepalive):
            pass

        def loop_start(self):
            pass

        def loop_stop(self):
            pass

        def disconnect(self):
            pass

        def publish(self, topic, payload, qos=0, retain=False):
            published.append((topic, payload, retain))

    mods = {"appdaemon": types.ModuleType("appdaemon"), "appdaemon.plugins": types.ModuleType("appdaemon.plugins"),
            "appdaemon.plugins.hass": types.ModuleType("appdaemon.plugins.hass"),
            "appdaemon.plugins.hass.hassapi": types.ModuleType("appdaemon.plugins.hass.hassapi"),
            "paho": types.ModuleType("paho"), "paho.mqtt": types.ModuleType("paho.mqtt"),
            "paho.mqtt.client": types.ModuleType("paho.mqtt.client")}
    mods["appdaemon.plugins.hass.hassapi"].Hass = Hass
    mods["paho.mqtt.client"].Client = Client
    mods["paho.mqtt.client"].CallbackAPIVersion = types.SimpleNamespace(VERSION2=2)
    sys.modules.update(mods)
    app_mod = importlib.import_module("railboard_rtt")
    Fake.now = int(rtt.time.time())
    app = app_mod.RailboardRtt({"rtt_token": REFRESH, "rtt_kind": "refresh", "mqtt_host": "127.0.0.1",
                                "mqtt_user": "u", "mqtt_pass": "p"})
    app.initialize()
    app.poller({})                         # the old positional convention
    topics = sorted(t for t, _, _ in published)
    expect(topics == ["nickoscope_matrix/railboard/GLD/arrivals", "nickoscope_matrix/railboard/GLD/departures",
                      "nickoscope_matrix/railboard/GLD/status"] and all(r for _, _, r in published),
           "three retained topics published")
    dep = json.loads(next(p for t, p, _ in published if t.endswith("/departures")))
    expect(dep["v"] == 1 and dep["dir"] == "dep" and dep["stn"] == "Guildford", "departures payload, schema v1")
    published.clear()
    app.listener("input_text.railboard_crs", "state", "WAT", "GLD")   # the keyword convention via run_in
    expect(len(published) == 3, "a change of station polls at once")
    no_tokens(json.dumps(logs), "the app's log")
    no_tokens(json.dumps(published), "what the app published")
    app.terminate()

    srv.shutdown()
    print("\nrtt_client checks:", "FAILED (%d)" % fails if fails else "passed")
    sys.exit(1 if fails else 0)


main()

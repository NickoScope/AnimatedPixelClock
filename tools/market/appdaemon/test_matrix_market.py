#!/usr/bin/env python3
"""matrix_market against a fake AppDaemon, a fake broker, a fake HTTP door and the saved samples.

Nothing here opens a socket: AppDaemon and paho are stand-ins that record what
the app asks of them (the call shapes of AppDaemon 4.5.13 and paho-mqtt 2.1.0, as
the media app's tests), Yahoo and the ECB are canned answers from
tools/market/samples/, and the maths is checked against tools/market/market_ref.py.

  python3 tools/market/appdaemon/test_matrix_market.py
"""
import contextlib
import datetime as dt
import io
import json
import pathlib
import re
import sys
import tempfile
import time
import types
import unittest

HERE = pathlib.Path(__file__).resolve().parent          # tools/market/appdaemon
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))                    # market_ref.py

SECRET = "s3cr3t-broker-pass"
UTC = dt.timezone.utc
D = dt.date


# ── the stand-ins ────────────────────────────────────────────────────────────
class FakeHass:
    def __init__(self, args):
        self.args = args
        self.logs, self.timers, self.every, self.daily = [], [], [], []

    def log(self, msg, level="INFO"):
        self.logs.append((level, msg))

    def run_in(self, callback, delay, *args, **kwargs):
        self.timers.append((callback, delay))
        return "timer"

    def run_every(self, callback, start=None, interval=0, *args, **kwargs):
        self.every.append((callback, start, interval))
        return "timer"

    def run_daily(self, callback, start=None, *args, **kwargs):
        self.daily.append((callback, start))
        return "timer"

    def get_timezone(self):
        return "Europe/Paris"

    def cancel_timer(self, handle):
        self.cancelled = getattr(self, "cancelled", 0) + 1


class FakeClient:
    """paho's Client as the app uses it, with the connection modelled: before the broker answers a QoS 0
    publish is dropped and a QoS 1 publish queued until the connection (paho-mqtt 2.1 client.publish, read
    through deepwiki). loop_start() connects at once unless auto_connect is False."""
    auto_connect = True

    def __init__(self, api_version, client_id=None):
        self.api_version, self.client_id = api_version, client_id
        self.published, self.subscribed, self.will, self.login = [], [], None, None
        self.dropped, self.queued, self.connected = [], [], False
        self.on_connect = self.on_message = None

    def username_pw_set(self, username, password=None):
        self.login = (username, password)

    def will_set(self, topic, payload=None, qos=0, retain=False):
        self.will = (topic, payload, qos, retain)

    def connect_async(self, host, port, keepalive):
        self.target = (host, port, keepalive)

    def loop_start(self):
        if self.auto_connect:
            self.go_online()

    def go_online(self):
        self.connected = True
        self.published.extend(self.queued)
        self.queued = []
        if self.on_connect:
            self.on_connect(self, None, None, types.SimpleNamespace(is_failure=False), None)

    def loop_stop(self):
        pass

    def disconnect(self):
        self.connected = False

    def publish(self, topic, payload=None, qos=0, retain=False):
        msg = (topic, payload, qos, retain)
        if self.connected:
            self.published.append(msg)
        elif qos == 0:
            self.dropped.append(msg)
        else:
            self.queued.append(msg)
        return types.SimpleNamespace(rc=0 if (self.connected or qos) else 4, wait_for_publish=lambda timeout=None: None)

    def subscribe(self, topics):
        self.subscribed.append(topics)


def install_fakes():
    for name in ("appdaemon", "appdaemon.plugins", "appdaemon.plugins.hass", "paho", "paho.mqtt"):
        sys.modules.setdefault(name, types.ModuleType(name))
    hassapi = types.ModuleType("appdaemon.plugins.hass.hassapi")
    hassapi.Hass = FakeHass
    sys.modules["appdaemon.plugins.hass.hassapi"] = hassapi
    client = types.ModuleType("paho.mqtt.client")
    client.Client = FakeClient
    client.CallbackAPIVersion = types.SimpleNamespace(VERSION2="VERSION2")
    sys.modules["paho.mqtt.client"] = client


install_fakes()

from matrix_market import config, encode, ledger, publisher, registry, windows, timeline, stats  # noqa: E402
from matrix_market import history, live, portfolio, tape, ha_entities, cli  # noqa: E402
from matrix_market.app import MatrixMarket  # noqa: E402
from matrix_market.features import Context  # noqa: E402
from matrix_market.store import Store  # noqa: E402
from matrix_market.providers import base, ecb, fixtures, httpio, yahoo  # noqa: E402
from matrix_market._ref import market_ref as M  # noqa: E402

SAMPLES = M.SAMPLES
ECB_CSV = (SAMPLES / M.ECB_SAMPLE).read_text("utf-8")          # EXR.M.USD.EUR.SP00.E, end of period (previews v2)
ECB_TABLE = M.load_ecb_monthly(SAMPLES / M.ECB_SAMPLE)          # {(year, month): USD per EUR}


class FakeHttp:
    """Canned answers by URL substring; an Exception value is raised; every call is recorded."""

    def __init__(self, answers):
        self.answers, self.calls, self.jar = answers, [], None

    def _find(self, url):
        for key, val in self.answers.items():
            if key in url:
                return val
        raise base.NotFound("HTTP 404 on " + url)

    def get(self, url, params=None, headers=None):
        self.calls.append((url, dict(params or {})))
        val = self._find(url)
        if isinstance(val, Exception):
            raise val
        return val if isinstance(val, bytes) else val(params)

    def get_json(self, url, params=None, headers=None):
        return json.loads(self.get(url, params, headers).decode("utf-8"))

    def get_text(self, url, params=None, headers=None):
        return self.get(url, params, headers).decode("utf-8")


def sample_bytes(name):
    return (SAMPLES / name).read_bytes()


def fake_ecb_ensure(store, http=None):
    (pathlib.Path(store.root) / M.ECB_SAMPLE).write_text(ECB_CSV, "utf-8")
    return ecb.EcbRates.from_store(store)


REAL_ECB_ENSURE = history.ecb_mod.ensure       # the real one, for the tests of ecb.py itself
history.ecb_mod.ensure = fake_ecb_ensure          # no ECB fetch from a test


def voo_only(**over):
    d = {"v": 1, "portfolio": {"positions": [{"sym": "VOO", "w": 100.0}]}}
    d.update(over)
    return d


def fixture_store(tmp, settings=None):
    """A store filled from the samples through the history feature (the fixtures provider)."""
    st = Store(tmp)
    s = settings or config.from_panel(config.from_defaults(), voo_only())
    ctx = Context(s, st, fixtures.FixtureProvider(), publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3")), lambda *a, **k: None)
    h = history.HistoryFeature(ctx)
    h.fetch_all()
    return st, ctx, h


# ── config ───────────────────────────────────────────────────────────────────
class ConfigTests(unittest.TestCase):
    def test_defaults_are_the_neutral_example(self):
        """Privacy rule: the committed defaults are the four indices and the 60/40 VFINX/VBMFX pair, 10 000 EUR
        from 2000-01-01; the real allocation lives in apps_market.local.yaml (gitignored)."""
        s = config.from_defaults()
        self.assertEqual([(p["sym"], p["w"], p["asset_class"]) for p in s.positions()], [("VFINX", 60.0, "equity"), ("VBMFX", 40.0, "bond")])
        self.assertEqual([i["name"] for i in s.indices()], ["SPX", "NDX", "CAC", "DAX"])
        self.assertEqual((s["portfolio.capital"], s["portfolio.currency"], s["portfolio.inception"]), (10000.0, "EUR", "2000-01-01"))
        self.assertEqual(s.mode(), "hold")
        self.assertIn("EURUSD=X", s.watched_symbols())
        self.assertNotIn("EURUSD=X", s.live_symbols())
        self.assertEqual(s.benchmark(), "^SP500TR")                  # owner, 11:43
        self.assertEqual(s.watched_symbols(), ["^GSPC", "^IXIC", "^FCHI", "^GDAXI", "VFINX", "VBMFX", "^SP500TR", "EURUSD=X"])
        self.assertEqual(s.bench_symbols(), ["^SP500TR"])
        self.assertEqual(s.rules(), M.Rules(), "the defaults are simulate()'s rules")
        self.assertEqual(s.blend_legs(), [])
    def test_nothing_private_in_the_committed_files(self):
        """Privacy rule and audit MINOR 14: no weight or fund of the owner's allocation, no broker address, no panel id
        in the app's committed files. The needles are spelled backwards so this file does not hold them either."""
        needles = [x[::-1] for x in ("PHCS", "TICV", "TLCV", "BV", "JGP", "RHSA", "IXF", "KGV", "YDNI", "MDLG", "QNV", "IQNV",
                                     "Q" * 3, "53.4.861.291", "ce02d")]      # a palindrome: built, never spelled
        pat = re.compile(r"(?<![A-Za-z0-9.])(%s)(?![A-Za-z0-9])" % "|".join(re.escape(n) for n in needles))
        files = [pathlib.Path(HERE, "apps_market.yaml"), pathlib.Path(HERE, "README.md"), pathlib.Path(HERE.parent, "check_market.py"),
                 pathlib.Path(HERE.parent, "ha", "portfolio_view.json"), pathlib.Path(__file__)] + sorted(pathlib.Path(HERE, "matrix_market").rglob("*.py"))
        for f in files:
            hits = pat.findall(f.read_text("utf-8"))
            self.assertEqual(hits, [], f.name)
        self.assertNotIn("23.7", pathlib.Path(HERE, "matrix_market/config.py").read_text())

    def test_local_override(self):
        tmp = pathlib.Path(tempfile.mkdtemp(), "local.json")
        tmp.write_text(json.dumps({"portfolio": {"positions": [{"sym": "AAA", "w": 30, "asset_class": "equity"}, {"sym": "BBB", "w": 70}]},
                                   "ticker": "AAA", "device": "f00d42", "mqtt_host": "192.0.2.77"}))
        local = config.load_local(tmp)
        s = config.from_args({"device": "a1b2c3", "portfolio": {"capital": 5000}}, local)
        self.assertEqual(s.errors, [])
        self.assertEqual(s.unknown, {}, "the private wiring keys are not settings")
        self.assertEqual(([p["sym"] for p in s.positions()], s["ticker"], s["portfolio.capital"]), (["AAA", "BBB"], "AAA", 5000.0))
        wiring, _ = config.app_args(config.merge_wiring({"device": "<panel-id>", "mqtt_host": "<your-broker>"}, local))
        self.assertEqual((wiring["device"], wiring["mqtt_host"], wiring["store_dir"]), ("f00d42", "192.0.2.77", "/config/market"))
        self.assertIsNone(config.load_local(pathlib.Path(tempfile.mkdtemp(), "none.json")))
        bad = pathlib.Path(tempfile.mkdtemp(), "local.json")
        bad.write_text("portfolio:\n  positions: []\n")
        with self.assertRaises(config.LocalFileError):
            config.load_local(bad)
        with self.assertRaises(ValueError):
            config.app_args({"device": "<panel-id>", "mqtt_host": "192.0.2.10"})
        with self.assertRaises(ValueError):
            config.app_args({"device": "a1b2c3", "mqtt_host": "<your-broker>"})
        with self.assertRaises(ValueError):
            config.app_args({"device": "a1b2c3"})

    def test_change_classes(self):
        a = config.from_defaults()
        for payload, kind in (({"ticker": "VFINX"}, "ticker"), ({"live": {"poll_s": 90}}, "schedule"),
                              ({"stale": {"session_min": 30}, "ticker": "VBMFX"}, "schedule"), ({"display": {"tz": "Europe/Paris"}}, "light"),
                              ({"portfolio": {"capital": 20000}}, "maths"), ({"bench": {"symbol": "^GSPC"}, "ticker": "VBMFX"}, "maths"),
                              ({}, "none")):
            b = config.from_panel(a, dict({"v": 2}, **payload))
            self.assertEqual(config.classify(config.changed_keys(a.data, b.data)), kind, payload)

    def test_refusals_are_named_and_the_rest_applies(self):
        payload = {"v": 2, "portfolio": {"capital": -5, "currency": "GBP", "rebalance": "yes",
                                         "positions": [{"sym": "VOO", "w": 60}, {"sym": "ZZZ", "w": 60}]},
                   "presets": ["2Y"], "live": {"poll_s": 5}, "tape": {"exchanges": ["MOEX"]},
                   "tickers": ["AAPL", "aapl"], "indices": ["^GSPC"], "ticker": "voo", "refresh": {"live_s": 5}}
        s = config.from_panel(config.from_defaults(), payload)
        codes = {e["key"]: e["code"] for e in s.errors}
        self.assertEqual(codes, {"portfolio.capital": "RANGE", "portfolio.currency": "ENUM", "portfolio.rebalance": "TYPE",
                                 "portfolio.positions": "RANGE", "presets": "ENUM", "live.poll_s": "RANGE",
                                 "tape.exchanges": "ENUM", "tickers": "DUP"})
        self.assertEqual(s.unknown, {"refresh": {"live_s": 5}}, "the v1 refresh keys are unknown now: kept, not refused")
        self.assertEqual(s["portfolio.capital"], 10000.0)          # the layer below kept
        self.assertEqual(len(s.positions()), 2, "the refused list: the neutral example stays")
        self.assertEqual(s.indices(), [{"sym": "^GSPC", "name": "SPX"}])   # a good key beside bad ones applies
        self.assertEqual(s["ticker"], "VOO")

    def test_unknown_keys_are_kept(self):
        s = config.from_panel(config.from_defaults(), {"v": 1, "alerts": {"drift": 5}, "portfolio": {"rebalance": True, "tax": 0.3}})
        self.assertEqual(s.errors, [])
        self.assertEqual(s.unknown, {"alerts": {"drift": 5}, "portfolio": {"tax": 0.3}})
        self.assertTrue(s["portfolio.rebalance"])
        out = s.to_payload()
        self.assertEqual((out["alerts"]["drift"], out["portfolio"]["tax"], out["v"]), (5, 0.3, 2))
        self.assertEqual(out["alert"]["day_move_pct"], 0.0, "the known group keeps its defaults; `alerts` is unknown and kept whole")

    def test_version_rules(self):
        base_s = config.from_defaults()
        s = config.from_panel(base_s, {"indices": ["^GSPC"]})
        self.assertEqual([e["code"] for e in s.errors], ["V_MISSING"])
        self.assertEqual(len(s.indices()), 4)                        # ignored whole
        s = config.from_panel(base_s, {"v": "1", "indices": ["^GSPC"]})
        self.assertEqual([e["code"] for e in s.errors], ["V_MISSING"])
        s = config.from_panel(base_s, {"v": 3, "indices": ["^GSPC"]})
        self.assertEqual([e["code"] for e in s.errors], ["V_NEWER"])
        self.assertEqual(len(s.indices()), 1)                        # known keys used
        s = config.from_panel(base_s, {"v": 2, "indices": ["^GSPC"]})
        self.assertEqual(s.errors, [], "v2 is this app's config version (docs/18: the panel sends v2)")
        s = config.from_panel(base_s, {"v": 0})
        self.assertEqual([e["code"] for e in s.errors], ["V_BAD"])
        s = config.from_panel(base_s, "not an object")
        self.assertEqual([e["code"] for e in s.errors], ["V_MISSING"])

    def test_three_layers(self):
        apps = config.from_args({"device": "a1b2c3", "mqtt_host": "x", "portfolio": {"capital": 5000, "currency": "USD"}, "module": "m", "class": "C"})
        self.assertEqual(apps.errors, [])
        self.assertEqual((apps["portfolio.capital"], apps["portfolio.currency"]), (5000.0, "USD"))
        s = config.from_panel(apps, {"v": 1, "portfolio": {"capital": "lots", "currency": "EUR"}})
        self.assertEqual((s["portfolio.capital"], s["portfolio.currency"]), (5000.0, "EUR"))   # refused -> apps.yaml's

    def test_ter_percent_and_positions(self):
        s = config.from_panel(config.from_defaults(), {"v": 1, "ter": {"vwce.de": 0.22, "VOO": 0.03},
                                                       "portfolio": {"positions": [{"sym": "VOO", "w": 50, "entry": "2015-01-02"}]}})
        self.assertEqual(s.errors, [])
        self.assertAlmostEqual(s.ter_overrides()["VWCE.DE"], 0.0022)
        cfg = s.ref_config()
        self.assertEqual((cfg.positions[0].sym, cfg.positions[0].w, cfg.positions[0].entry), ("VOO", 50.0, D(2015, 1, 2)))
        self.assertEqual(cfg.currency, "EUR")

    def test_app_args(self):
        with self.assertRaises(ValueError):
            config.app_args({"device": "D20EC8"})
        w, errors = config.app_args({"device": "a1b2c3", "mqtt_host": "192.0.2.10", "features": ["history", "dance"]})
        self.assertEqual(w["features"], ["history"])
        self.assertEqual(errors[0]["code"], "ENUM")
        self.assertEqual((w["mqtt_host"], w["provider"], w["topic_base"], w["store_dir"]), ("192.0.2.10", "yahoo", "nickoscope_matrix", "/config/market"))

    def test_schema_table_and_groups(self):
        table = config.schema_table()
        for s in config.SCHEMA:
            self.assertIn("`%s`" % s.key, table)
        self.assertEqual(len({s.key for s in config.SCHEMA}), len(config.SCHEMA))
        self.assertTrue(all(s.since <= config.CONFIG_V for s in config.SCHEMA))
        for s in config.SCHEMA:
            self.assertIn(s.type, config.TYPES, s.key)


# ── encode ───────────────────────────────────────────────────────────────────
class EncodeTests(unittest.TestCase):
    def test_round_trip(self):
        pts = [100.0 + 37.5 * ((i * 7919) % 128) / 128.0 for i in range(128)]
        lo, hi, b64 = encode.pack(pts)
        encode.check(b64)
        self.assertEqual((lo, hi), (min(pts), max(pts)))
        back = encode.decode(b64, lo, hi)
        self.assertEqual(len(back), 128)
        self.assertLessEqual(max(abs(a - b) for a, b in zip(pts, back)), (hi - lo) / 65535 / 2 + 1e-9)
        self.assertEqual((back[pts.index(min(pts))], back[pts.index(max(pts))]), (lo, hi))
        self.assertLessEqual(encode.max_error(pts, lo, hi), (hi - lo) / 65535 / 2 + 1e-9)

    def test_flat_and_bad(self):
        lo, hi, b64 = encode.pack([5.0] * 128)
        self.assertEqual(set(encode.unpack(b64)), {32767})
        self.assertEqual(encode.decode(b64, lo, hi), [5.0] * 128)
        with self.assertRaises(ValueError):
            encode.check(encode.pack([1.0, 2.0])[2])
        with self.assertRaises(ValueError):
            encode.check("not base64!!")


# ── store ────────────────────────────────────────────────────────────────────
class StoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.st = Store(self.tmp)
        self.voo = M.load_yahoo_chart(SAMPLES / "yahoo_VOO_max_1mo.json")

    def test_names(self):
        self.assertEqual((Store.escape if hasattr(Store, "escape") else __import__("matrix_market.store", fromlist=["escape"]).escape)("^GSPC"), "_5eGSPC")

    def test_new_append_same_replace(self):
        self.assertEqual(self.st.save("VOO", self.voo), ("new", 193))
        self.assertEqual(self.st.save("VOO", self.voo), ("same", 0))
        more = M.Series("VOO", "USD", self.voo.dates[-40:] + [D(2026, 10, 1)], self.voo.closes[-40:] + [700.0],
                        self.voo.dividends + [(D(2026, 9, 29), 1.9)], None, self.voo.meta)
        self.assertEqual(self.st.save("VOO", more), ("appended", 1))
        s = self.st.load("VOO")
        self.assertEqual((len(s.dates), s.last, s.dividends[-1]), (194, D(2026, 10, 1), (D(2026, 9, 29), 1.9)))
        halved = M.Series("VOO", "USD", self.voo.dates[-40:], [c / 2 for c in self.voo.closes[-40:]], self.voo.dividends, None, self.voo.meta)
        self.assertEqual(self.st.save("VOO", halved), ("mismatch", 0), "a partial answer with other closes writes nothing")
        self.assertEqual(len(self.st.load("VOO").dates), 194, "the full history stays (audit B1)")
        self.assertEqual(self.st.save("VOO", halved, full=True), ("short", 0), "a 'full' answer that starts 16 years late is refused")
        self.assertEqual(len(self.st.load("VOO").dates), 194)
        whole = M.Series("VOO", "USD", list(self.voo.dates), [c / 2 for c in self.voo.closes], self.voo.dividends, None, self.voo.meta)
        self.assertEqual(self.st.save("VOO", whole, full=True), ("replaced", -1))
        self.assertEqual(self.st.entry("VOO")["how"], "replaced")
        self.assertIn("replaced", self.st.entry("VOO"))
        self.assertEqual(len(self.st.load("VOO").dates), 193)
        self.assertAlmostEqual(self.st.load("VOO").closes[0], self.voo.closes[0] / 2)

    def test_ter_and_status(self):
        self.st.save("VOO", self.voo)
        self.st.set_ter("VOO", 0.0003, "yahoo", "2026-08-01T00:00:00Z")
        self.assertGreater(self.st.ter_age_days("VOO"), 30)
        self.assertIsNone(self.st.ter_age_days("ZZZ"))
        self.assertEqual(self.st.load("VOO").ter, 0.0003)
        more = M.Series("VOO", "USD", self.voo.dates[-5:], self.voo.closes[-5:], [], None, self.voo.meta)
        self.st.save("VOO", more)
        self.assertEqual(self.st.load("VOO").ter, 0.0003, "an append keeps the TER")
        self.st.mark_error("ZZZ", "ProviderError: HTTP 500 on /v8/finance/chart/ZZZ " + "x" * 100)
        rows = self.st.status_symbols(["VOO", "ZZZ"])
        self.assertEqual(rows["VOO"], {"asof": "2026-09-14", "bars": 193, "ter": 0.0003, "terSrc": "yahoo"})
        self.assertEqual(rows["ZZZ"]["bars"], 0)
        self.assertLessEqual(len(rows["ZZZ"]["err"]), 80)
        self.assertEqual(Store(self.tmp).entry("VOO")["bars"], 193, "the manifest persists")

    def test_corrupt_and_tables(self):
        self.st.save("VOO", self.voo)
        self.st.path("VOO").write_text("{not json")
        self.assertIsNone(self.st.load("VOO"))
        self.st.save_table("t", {"a": 1})
        self.assertEqual(self.st.load_table("t"), {"a": 1})
        self.assertIsNone(self.st.load_table("nope"))
        self.assertEqual(self.st.load_all(["VOO", "ZZZ"]), {})


class DailyHistoryTests(unittest.TestCase):
    """Audit B1 reproduced: a daytime fetch then the 07:00 run must not cost the history.

    A synthetic fund with a close on every weekday from 2000-01-03 to 2026-09-14 (6 966 bars). Yahoo's daily
    answer appends a bar for the session in progress and moves its close; after the close the bar is final.
    """
    END = D(2026, 9, 14)

    @classmethod
    def weekdays(cls, start, end):
        out, d = [], start
        while d <= end:
            if d.weekday() < 5:
                out.append(d)
            d += dt.timedelta(days=1)
        return out

    def setUp(self):
        self.days = self.weekdays(D(2000, 1, 3), self.END)
        self.closes = [100.0 + i * 0.01 for i in range(len(self.days))]
        self.session = D(2026, 9, 15)             # the next session: 09:30-16:00 New York = 13:30-20:00Z
        self.session_close = 175.0                # its final close
        self.calls = []
        test = self

        class Provider:
            name = "fake"

            def history(self, sym, start, end=None, interval="1d"):
                test.calls.append(start)
                now = dt.datetime.fromtimestamp(test.clock, UTC)
                dates = [d for d in test.days if d >= start]
                closes = [c for d, c in zip(test.days, test.closes) if d >= start]
                s_open = dt.datetime(2026, 9, 15, 13, 30, tzinfo=UTC)
                if now >= s_open:                        # Yahoo's bar for 15 SEP: moving in the session, final after
                    dates.append(test.session)
                    closes.append(172.5 if now < dt.datetime(2026, 9, 15, 20, 0, tzinfo=UTC) else test.session_close)
                on = D(2026, 9, 15) if now < dt.datetime(2026, 9, 15, 20, 0, tzinfo=UTC) else D(2026, 9, 16)
                start_ts = int(dt.datetime(on.year, on.month, on.day, 13, 30, tzinfo=UTC).timestamp())
                meta = {"gmtoffset": -14400, "currentTradingPeriod": {"regular": {"start": start_ts, "end": start_ts + 23400, "gmtoffset": -14400}}}
                return M.Series(sym, "USD", dates, closes, [], None, meta, list(closes))

        self.provider = Provider()
        self.store = Store(tempfile.mkdtemp())
        self.clock = dt.datetime(2026, 9, 15, 5, 0, tzinfo=UTC).timestamp()
        s = config.from_panel(config.from_defaults(), {"v": 2, "portfolio": {"positions": [{"sym": "FUND", "w": 100}]}})
        self.ctx = Context(s, self.store, self.provider, publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3")),
                           lambda *a, **k: None, clock=lambda: self.clock)
        self.hist = history.HistoryFeature(self.ctx)

    def fetch(self, at):
        self.clock = at.timestamp()
        return self.hist.fetch_symbols(["FUND"])["FUND"]

    def test_daytime_then_morning_keeps_every_bar(self):
        self.assertEqual(self.fetch(dt.datetime(2026, 9, 15, 5, 0, tzinfo=UTC)), "new")
        self.assertEqual(len(self.store.load("FUND").dates), 6966)
        self.assertEqual(self.fetch(dt.datetime(2026, 9, 15, 15, 0, tzinfo=UTC)), "same", "the open session's bar is not stored")
        self.assertEqual(self.store.load("FUND").last, self.END)
        self.assertEqual(self.fetch(dt.datetime(2026, 9, 16, 5, 0, tzinfo=UTC)), "appended")   # 07:00 Paris
        s = self.store.load("FUND")
        self.assertEqual((len(s.dates), s.last, s.closes[-1]), (6967, self.session, self.session_close))
        self.assertEqual(self.calls[1], self.END - dt.timedelta(days=history.OVERLAP_DAYS), "the incremental request")

    def test_changed_past_closes_refetch_the_whole_history(self):
        self.fetch(dt.datetime(2026, 9, 15, 5, 0, tzinfo=UTC))
        self.closes[-10] *= 1.5                       # a restatement ten bars back
        self.calls.clear()
        self.assertEqual(self.fetch(dt.datetime(2026, 9, 16, 5, 0, tzinfo=UTC)), "replaced")
        self.assertEqual(self.calls, [self.END - dt.timedelta(days=history.OVERLAP_DAYS), D(2000, 1, 1)])
        s = self.store.load("FUND")
        self.assertEqual(len(s.dates), 6967, "the full answer replaced the file, not the 60-day one")
        self.assertEqual(s.closes[-11], self.closes[-10])

    def test_a_failed_full_refetch_keeps_the_store(self):
        self.fetch(dt.datetime(2026, 9, 15, 5, 0, tzinfo=UTC))
        self.closes[-10] *= 1.5
        real = self.provider.history

        def history_429_on_full(sym, start, end=None, interval="1d"):
            if start == D(2000, 1, 1):
                raise base.RateLimited("HTTP 429 on /v8/finance/chart/FUND")
            return real(sym, start)
        self.provider.history = history_429_on_full
        self.assertEqual(self.fetch(dt.datetime(2026, 9, 16, 5, 0, tzinfo=UTC)), "429")
        self.assertEqual(len(self.store.load("FUND").dates), 6966)

    def test_open_session_day(self):
        meta = {"gmtoffset": -14400, "currentTradingPeriod": {"regular": {"start": 1789479000, "end": 1789502400, "gmtoffset": -14400}}}
        self.assertEqual(history.open_session_day(meta, dt.datetime(2026, 9, 15, 14, 0, tzinfo=UTC)), D(2026, 9, 15))
        self.assertEqual(history.open_session_day(meta, dt.datetime(2026, 9, 15, 8, 0, tzinfo=UTC)), D(2026, 9, 15), "before the open")
        self.assertIsNone(history.open_session_day(meta, dt.datetime(2026, 9, 15, 21, 0, tzinfo=UTC)), "after the close")
        self.assertIsNone(history.open_session_day(meta, dt.datetime(2026, 9, 14, 14, 0, tzinfo=UTC)), "periods of another day")
        self.assertEqual(history.open_session_day({}, dt.datetime(2026, 9, 15, 14, 0, tzinfo=UTC)), D(2026, 9, 15))
        fx = json.loads(sample_bytes("yahoo_EURUSDX.json"))["chart"]["result"][0]["meta"]      # London, regular 00:00-23:59
        self.assertEqual(history.open_session_day(fx, dt.datetime(2026, 9, 15, 12, 0, tzinfo=UTC)), D(2026, 9, 15))


# ── registries, windows, metrics ─────────────────────────────────────────────
class RegistryTests(unittest.TestCase):
    def test_tables(self):
        self.assertEqual(windows.WINDOWS.names()[:6], list(M.PRESETS))
        self.assertEqual(windows.METRICS.names()[:6], ["chg", "hi", "lo", "cagr", "mdd", "ann"])
        self.assertEqual(windows.WINDOWS.names()[6:], ["1M", "3M", "6M", "MTD", "WTD"])
        self.assertEqual(ledger.MODES.names(), ["hold", "rebal"])
        self.assertEqual(ledger.LINES.names(), ["pts", "px", "bench", "gross", "tell"])
        self.assertEqual(sorted(registry.FEATURES.names()), ["ha_entities", "history", "live", "portfolio", "tape"])
        self.assertEqual(registry.PROVIDERS.names(), ["fixtures", "yahoo"])
        self.assertEqual(timeline.TIMELINES.names(), ["union", "calendar"])
        self.assertEqual(registry.EXCHANGES.names(), ["NYSE", "NASDAQ", "LSE", "XETRA", "EURONEXT", "TOKYO"])
        r = registry.Registry("thing")
        r.register("a", 1)
        with self.assertRaises(ValueError):
            r.register("a", 2)
        with self.assertRaises(KeyError):
            r.get("b")
        r.unregister("a")
        self.assertNotIn("a", r)

    def test_a_new_metric_plugs_in(self):
        s = M.load_yahoo_chart(SAMPLES / "yahoo_VOO_max_1mo.json")
        wd, wv = s.dates, s.closes
        got = windows.metrics(wd, wv)
        self.assertEqual({k: got[k] for k in ("chg", "hi", "lo", "cagr", "mdd")}, {k: M.report(wd, wv)[k] for k in ("chg", "hi", "lo", "cagr", "mdd")})
        self.assertEqual(sorted(got), ["ann", "cagr", "chg", "hi", "lo", "mdd"])
        windows.METRICS.register("bars", lambda wd, wv, ctx: len(wv))
        try:
            p = history.index_payload(s, "5Y", s.last, D(2000, 1, 1), "VOO")
            self.assertIn("bars", p)
            self.assertEqual(windows.extra_metric_names(), ["ann", "bars"])
            S = M.load_samples()
            res = ledger.compute(M.preview_config(), S["series"], S["fx"], "^GSPC")
            self.assertGreater(ledger.portfolio_payload(res, "hold", "5Y")["bars"], 0)
        finally:
            windows.METRICS.unregister("bars")
        self.assertNotIn("bars", history.index_payload(s, "5Y", s.last, D(2000, 1, 1), "VOO"))

    def test_a_new_mode_plugs_in(self):
        ledger.MODES.register("band", lambda cfg, series, fx, days, dividends=True, bar_years=None, rules=None: M.simulate(cfg, series, fx, days, "rebal", dividends, bar_years))
        try:
            S = M.load_samples()
            res = ledger.compute(M.preview_config(), S["series"], S["fx"], "^GSPC")
            self.assertEqual(res.modes, ["hold", "rebal", "band"])
            self.assertEqual(ledger.portfolio_payload(res, "band", "MAX")["value"], ledger.portfolio_payload(res, "rebal", "MAX")["value"])
        finally:
            ledger.MODES.unregister("band")


# ── ledger: the adapter equals the reference ─────────────────────────────────
class LedgerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.S = M.load_samples()
        cls.P = M.run_preview_portfolio(samples=cls.S)
        cls.res = ledger.compute(cls.P["cfg"], cls.S["series"], cls.S["fx"], "^GSPC", bench_in_portfolio_currency=False)

    def test_equals_the_reference(self):
        for mode in ("hold", "rebal"):
            self.assertEqual(self.res.ledger(mode).v, self.P["runs"][mode].v)
            self.assertEqual(self.res.ledger(mode, False).v, self.P["runs"][mode + "_nodiv"].v)
            self.assertEqual(self.res.ledger(mode).entries, {"VOO": D(2010, 12, 1)})
        self.assertAlmostEqual(self.res.ledger("hold").v[-1], 89800.95, places=2)      # preview/README.md
        self.assertEqual(self.res.bench, self.P["bench"])
        self.assertIs(ledger.bar_years_for(self.res.days), M.calendar_years)          # monthly fixtures
        self.assertIsNone(ledger.bar_years_for([D(2026, 9, 1) + dt.timedelta(days=i) for i in range(10)]))

    def test_payloads_equal_the_reference(self):
        for mode in ("hold", "rebal"):
            for preset in M.PRESETS:
                mine = ledger.portfolio_payload(self.res, mode, preset)
                ref = M.portfolio_payload(self.P["cfg"], self.P["runs"][mode], self.P["runs"][mode + "_nodiv"], self.P["bench"], preset)
                for k, v in ref.items():
                    if isinstance(v, float):
                        self.assertAlmostEqual(mine[k], v, places=6, msg=(mode, preset, k))     # six decimals kept on the wire
                    elif k in ("pts", "px", "bench"):
                        self.assertLessEqual(max(abs(a - b) for a, b in zip(encode.unpack(mine[k]), encode.unpack(v))), 1, (mode, preset, k))
                    else:
                        self.assertEqual(mine[k], v, (mode, preset, k))
                self.assertEqual(sorted(k for k in mine if k not in ref), ["ann", "fx_effect", "mdd_peak", "mdd_recovery", "mdd_trough", "measure", "twr"])
                self.assertNotIn("flows", mine, "no contributions, no withdrawals: no xirr_ann, no flows flag")
            h = ledger.holdings_payload(self.res, mode)
            ref_h = M.holdings_payload(self.P["cfg"], self.P["runs"][mode], self.S["series"], self.S["fx"])
            for mine_r, ref_r in zip(h["rows"], ref_h["rows"]):
                self.assertEqual((mine_r["sym"], mine_r["tgt"], mine_r["entry"]), (ref_r["sym"], ref_r["tgt"], ref_r["entry"]))
                self.assertAlmostEqual(mine_r["now"], ref_r["now"], places=4)
                self.assertAlmostEqual(mine_r["ret"], ref_r["ret"], places=4)
            self.assertEqual({k: v for k, v in h.items() if k in ("v", "asof", "mode", "cash")}, {k: v for k, v in ref_h.items() if k != "rows"})
            self.assertEqual(sorted(k for k in h if k not in ("v", "asof", "mode", "rows", "cash")), ["classes", "rebal"])
        for preset in ("MTD", "WTD", "1M", "3M", "6M"):
            x = ledger.portfolio_payload(self.res, "hold", preset)
            self.assertEqual((x["preset"], x["n"]), (preset, 128))
            encode.check(x["pts"])
            self.assertGreaterEqual(D.fromisoformat(x["from"]), D(2026, 3, 1), preset)
            ip = history.index_payload(self.S["series"]["VOO"], preset, D(2026, 9, 14), D(2000, 1, 1), "VOO")
            self.assertEqual((ip["preset"], ip["n"], ip["ann"]), (preset, 128, None))
        p = ledger.portfolio_payload(self.res, "hold", "5Y")
        self.assertAlmostEqual(p["chg"], 0.893, places=3)                              # preview/README.md: +89.3 %
        self.assertAlmostEqual(p["div"], 4267.55, places=2)
        self.assertEqual((p["mdd_peak"], p["mdd_trough"], p["mdd_recovery"]), ("2025-01-27", "2025-04-01", "2025-10-01"))
        for preset in ("YTD", "1M", "3M", "6M", "MTD", "WTD"):
            self.assertIsNone(ledger.portfolio_payload(self.res, "hold", preset)["ann"], preset + ": never annualised (owner, 11:43)")
        for preset in ("1Y", "3Y", "5Y", "10Y", "MAX"):
            self.assertIsNotNone(ledger.portfolio_payload(self.res, "hold", preset)["ann"], preset)
        self.assertAlmostEqual(ledger.portfolio_payload(self.res, "hold", "1Y")["ann"], ledger.portfolio_payload(self.res, "hold", "1Y")["chg"], places=2)

    def test_lines_switches(self):
        p = ledger.portfolio_payload(self.res, "hold", "MAX", {"px": False, "bench": False})
        self.assertEqual([k for k in ("pts", "px", "bench", "gross", "tell") if k in p], ["pts", "px"], "px cannot be switched off (M6)")
        t = ledger.portfolio_payload(self.res, "hold", "MAX", {"tell": True})
        self.assertEqual([k for k in ("pts", "px", "bench", "gross", "tell") if k in t], ["pts", "px", "tell"], "the telltale takes the benchmark's slot")
        lg = ledger.portfolio_payload(self.res, "hold", "MAX", scale="log")
        self.assertEqual(lg["scale"], "log")
        self.assertAlmostEqual(10 ** lg["max"], max(self.res.ledger("hold").v) if False else 10 ** lg["max"])
        self.assertLess(lg["max"], 6)        # log10 of a five-figure value
        self.assertNotIn("scale", p)
        g = ledger.portfolio_payload(self.res, "hold", "MAX", {"gross": True})
        self.assertIn("gross", g)
        L = self.res.ledger("hold")
        self.assertGreaterEqual(g["max"], L.v[-1] + L.ter[-1] - 1e-6)
        for k in ("pts", "px", "bench", "gross"):
            encode.check(g[k])

    def test_fx_with_the_ecb_hook(self):
        rates = ecb.EcbRates.from_sample()
        fx = ledger.fx_table(self.S["series"], rates.rate)
        self.assertAlmostEqual(fx.usd_per_eur(D(2001, 6, 15)), ECB_TABLE[(2001, 6)])
        self.assertAlmostEqual(fx.factor("USD", "EUR", D(1999, 1, 20)), 1 / ECB_TABLE[(1999, 1)])
        self.assertEqual(fx.usd_per_eur(D(2001, 6, 15)), M.load_samples()["fx"].usd_per_eur(D(2001, 6, 15)), "one FX path with the reference")
        with self.assertRaises(M.NoFxRate):
            fx.usd_per_eur(D(1998, 12, 31))
        with self.assertRaises(M.NoFxRate):
            ledger.fx_table(self.S["series"]).usd_per_eur(D(2001, 6, 15))          # no table: refuses

    def test_timeline(self):
        days = timeline.build(self.S["series"], D(2000, 1, 1))
        self.assertEqual(days, self.P["days"])
        self.assertNotIn(D(2026, 9, 15), days, "the FX 'today' stamp is not a trading day")
        try:
            import exchange_calendars  # noqa: F401
        except ImportError:
            self.assertEqual(timeline.build(self.S["series"], D(2000, 1, 1), "calendar"), days)


# ── providers ────────────────────────────────────────────────────────────────
class YahooProviderTests(unittest.TestCase):
    def make(self, answers):
        self.sleeps = []
        p = yahoo.YahooProvider(FakeHttp(answers), gap_s=10, sleep=self.sleeps.append)
        return p

    def test_history_parses_like_the_samples(self):
        p = self.make({"/v8/finance/chart/VOO": sample_bytes("yahoo_VOO_max_1mo.json")})
        s = p.history("VOO", D(2000, 1, 1))
        ref = M.load_yahoo_chart(SAMPLES / "yahoo_VOO_max_1mo.json")
        self.assertEqual((s.dates, s.closes, s.dividends, s.currency), (ref.dates, ref.closes, ref.dividends, ref.currency))
        self.assertEqual(s.meta["splits"], [["2013-10-24", "1:2"]])
        self.assertEqual(M.load_yahoo_chart(SAMPLES / "yahoo_VOO_max_1mo.json").meta["splits"], [["2013-10-24", "1:2"]])
        url, params = p.http.calls[0]
        self.assertEqual((params["interval"], params["events"], params["period1"]), ("1d", "div,split", 946684800))
        p.history("VOO", D(2000, 1, 1))
        self.assertEqual(len(self.sleeps), 1, "the second call waited the gap")
        self.assertAlmostEqual(self.sleeps[0], 10, delta=0.5)
        self.assertEqual(p.session_meta("VOO")["exchangeName"], "PCX")

    def test_429_backs_off(self):
        p = self.make({"/v8/finance/chart/": base.RateLimited("HTTP 429 on /v8/finance/chart/ZZZ")})
        with self.assertRaises(base.RateLimited):
            p.history("ZZZ", D(2000, 1, 1))
        self.assertTrue(p.backoff.blocked())
        self.assertAlmostEqual(p.backoff.remaining(), 300, delta=2)
        n = len(p.http.calls)
        with self.assertRaises(base.RateLimited):
            p.quotes(["ZZZ"])
        self.assertEqual(len(p.http.calls), n, "blocked: no request went out")
        b = base.Backoff(clock=lambda: 0)
        self.assertEqual([b.hit(), b.hit(), b.hit(), b.hit()], [300, 900, 3600, 3600])
        b.ok()
        self.assertEqual(b.hit(), 300)
        self.assertEqual(b.hit(retry_after=1200), 1200)

    def test_quotes_both_spark_shapes(self):
        voo = json.loads(sample_bytes("yahoo_VOO_max_1mo.json"))["chart"]["result"][0]
        v7 = {"spark": {"result": [{"symbol": "VOO", "response": [voo]}]}}
        p = self.make({"/v7/finance/spark": json.dumps(v7).encode()})
        q = p.quotes(["VOO"])["VOO"]
        self.assertEqual((q.last, q.prev, q.ts, q.currency), (voo["meta"]["regularMarketPrice"], voo["meta"]["chartPreviousClose"],
                                                            voo["meta"]["regularMarketTime"], "USD"))
        self.assertEqual(p.http.calls[0][1], {"symbols": "VOO", "range": "1d", "interval": "5m"})
        v8 = {"VOO": {"symbol": "VOO", "timestamp": [1, 2], "close": [10.0, None], "previousClose": 9.0}}
        p = self.make({"/v7/finance/spark": json.dumps(v8).encode()})
        q = p.quotes(["VOO"])["VOO"]
        self.assertEqual((q.last, q.prev, q.ts), (10.0, 9.0, 2))
        self.assertAlmostEqual(q.day, 1 / 9)
        self.assertEqual(p.quotes([]), {})

    def test_intraday(self):
        meta = json.loads(sample_bytes("yahoo_GSPC_1y_1d.json"))["chart"]["result"][0]["meta"]
        start, end = meta["currentTradingPeriod"]["regular"]["start"], meta["currentTradingPeriod"]["regular"]["end"]
        times = [start + 60 * i for i in range(0, 200)]
        closes = [7600.0 + (i % 13) for i in range(200)]
        closes[5] = None
        d = {"chart": {"result": [{"meta": meta, "timestamp": times, "indicators": {"quote": [{"close": closes}]}}], "error": None}}
        p = self.make({"/v8/finance/chart/%5EGSPC": json.dumps(d).encode(), "/v8/finance/chart/^GSPC": json.dumps(d).encode()})
        it = p.intraday("^GSPC")
        self.assertEqual((len(it.times), it.session_start, it.session_end, it.prev_close), (199, start, end, meta["chartPreviousClose"]))
        pl = live.intraday_payload(it)
        self.assertEqual(sorted(pl), ["max", "min", "n", "pts", "sym", "ts"], "the panel's intraday shape (src/market/README.md)")
        u = encode.unpack(pl["pts"])
        self.assertEqual(len(u), 128, "the line stays 128 slots")
        self.assertEqual(pl["n"], 199 * 128 // 390 + 1, "n is the slots filled: the last bar is minute 199 of 390")
        self.assertTrue(all(x == 0 for x in u[pl["n"]:]))
        self.assertEqual(pl["ts"], times[-1])
        self.assertLessEqual(len(publisher.encode(pl)), publisher.PAYLOAD_MAX)

    def test_fund_info_direct(self):
        p = self.make({"/v1/test/getcrumb": b"Abc.crumb", "/quoteSummary/VOO": sample_bytes("yahoo_VOO.json"),
                       "/quoteSummary/AAPL": sample_bytes("yahoo_AAPL.json")})
        p.use_yfinance = "no"
        info = p.fund_info("VOO")
        self.assertAlmostEqual(info.ter, 0.0003, places=6)
        self.assertEqual(info.source, "yahoo")
        self.assertEqual(p.http.calls[1][1], {"modules": "fundProfile,price", "crumb": "Abc.crumb"})
        self.assertIsNone(p.fund_info("AAPL").ter)
        self.assertEqual(len([c for c in p.http.calls if "getcrumb" in c[0]]), 1, "one crumb for the session")

    def test_not_found(self):
        d = {"chart": {"result": None, "error": {"code": "Not Found", "description": "No data found, symbol may be delisted"}}}
        p = self.make({"/v8/finance/chart/": json.dumps(d).encode()})
        with self.assertRaises(base.NotFound):
            p.history("NOPE", D(2000, 1, 1))


class HttpDoorTests(unittest.TestCase):
    """httpio: the User-Agent comes from the setting; a 429 is logged with it and the body; errors that are not
    OSError still become ProviderError (audits B2, M7, MINOR 8). The opener is stubbed: no socket."""

    def door(self, answer, ua=None):
        logs = []
        h = httpio.Http(user_agent=ua, log=lambda msg, level="INFO": logs.append((level, msg)))
        seen = []

        class Opener:
            def open(self, req, timeout=None):
                seen.append(req)
                if isinstance(answer, BaseException):
                    raise answer
                return contextlib.nullcontext(io.BytesIO(answer))
        h._opener = Opener()
        return h, seen, logs

    def test_the_user_agent_is_the_setting(self):
        wiring, _ = config.app_args({"device": "a1b2c3", "mqtt_host": "192.0.2.10"})
        self.assertEqual(wiring["user_agent"], "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36")
        h, seen, _ = self.door(b"{}", ua=wiring["user_agent"])
        h.get_json("https://example.invalid/x")
        self.assertEqual(seen[0].get_header("User-agent"), wiring["user_agent"])
        wiring, _ = config.app_args({"device": "a1b2c3", "mqtt_host": "192.0.2.10", "user_agent": "UA-under-test/1.0"})
        h, seen, _ = self.door(b"{}", ua=wiring["user_agent"])
        h.get("https://example.invalid/x")
        self.assertEqual(seen[0].get_header("User-agent"), "UA-under-test/1.0")
        self.assertEqual(yahoo.YahooProvider(user_agent="UA-under-test/1.0").http.ua, "UA-under-test/1.0")
        self.assertEqual(yahoo.YahooProvider().http.ua, httpio.DEFAULT_UA)

    def test_a_429_is_logged_with_the_user_agent_and_the_body(self):
        import email.message
        import urllib.error
        hdrs = email.message.Message()
        err = urllib.error.HTTPError("https://query1.finance.yahoo.com/v8/finance/chart/X?crumb=SECRETCRUMB", 429, "Too Many Requests",
                                     hdrs, io.BytesIO(b"Too Many Requests\r\n" + b"z" * 200))
        h, _, logs = self.door(err, ua="UA-429/1.0")
        with self.assertRaises(base.RateLimited):
            h.get("https://query1.finance.yahoo.com/v8/finance/chart/X", {"crumb": "SECRETCRUMB"})
        self.assertEqual(len(logs), 1)
        level, msg = logs[0]
        self.assertEqual(level, "WARNING")
        self.assertIn("UA-429/1.0", msg)
        self.assertIn("Too Many Requests..zzz", msg)
        self.assertNotIn("SECRETCRUMB", msg, "the path only, never the query")
        self.assertLess(msg.count("z"), 81)

    def test_errors_that_are_not_oserror(self):
        import http.client
        h, _, _ = self.door(http.client.IncompleteRead(b"par"))
        with self.assertRaises(base.ProviderError):
            h.get("https://example.invalid/x")
        h, _, _ = self.door(http.client.BadStatusLine("junk"))
        with self.assertRaises(base.ProviderError):
            h.get("https://example.invalid/x")

    def test_a_chart_the_parser_cannot_read(self):
        d = {"chart": {"result": [{"meta": {"symbol": "X"}, "timestamp": [1], "indicators": {}}], "error": None}}
        p = yahoo.YahooProvider(FakeHttp({"/v8/finance/chart/": json.dumps(d).encode()}), gap_s=0, sleep=lambda s: None)
        with self.assertRaises(base.ProviderError):
            p.history("X", D(2000, 1, 1))

    def test_the_module_is_not_named_http(self):
        self.assertFalse(pathlib.Path(HERE, "matrix_market/providers/http.py").exists())

    def test_backoff_cap_and_reset(self):
        t = [0.0]
        b = base.Backoff(clock=lambda: t[0])
        self.assertEqual(b.hit(retry_after=86400), 3600, "a Retry-After of a day is capped at the last step")
        self.assertEqual(b.hit(), 900)
        t[0] += 7 * 3600                                     # a quiet spell longer than reset_after
        self.assertEqual(b.hit(), 300, "the ladder starts again")
        self.assertFalse(base.Backoff(clock=lambda: 0.0).blocked())

    def test_a_429_schedules_one_retry(self):
        later = []
        ctx = Context(config.from_defaults(), Store(tempfile.mkdtemp()), None,
                      publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3")), lambda *a, **k: None)
        ctx._scheduler = types.SimpleNamespace(run_in=lambda cb, delay: later.append((cb, delay)) or "h")
        ctx.data["refresh"] = lambda reason, fetch: later.append((reason, fetch))

        class Provider:
            name = "fake"
            backoff = base.Backoff(clock=time.time)

            def history(self, sym, start, end=None, interval="1d"):
                self.backoff.hit()
                raise base.RateLimited("HTTP 429")
        ctx.provider = Provider()
        h = history.HistoryFeature(ctx)
        self.assertEqual(h.fetch_symbols(["AAA", "BBB"]), {"AAA": "429"})
        h.fetch_symbols(["AAA"])
        timers = [x for x in later if callable(x[0])]
        self.assertEqual(len(timers), 1, "one retry pending, however many 429s")
        self.assertAlmostEqual(timers[0][1], 300 + history.RETRY_MARGIN_S, delta=2)
        timers[0][0]()
        self.assertIn(("retry", "stale"), later, "the retry fetches what is still missing or stale, not everything")
        self.assertFalse(h.retry_pending)

    def test_one_bad_symbol_does_not_stop_the_others(self):
        ctx = Context(config.from_defaults(), Store(tempfile.mkdtemp()), fixtures.FixtureProvider(),
                      publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3")), lambda *a, **k: None)
        real = ctx.provider.history

        def history_(sym, start, end=None, interval="1d"):
            if sym == "^IXIC":
                raise KeyError("close")
            return real(sym, start)
        ctx.provider.history = history_
        out = history.HistoryFeature(ctx).fetch_symbols(["^IXIC", "VOO"])
        self.assertEqual(out, {"^IXIC": "error", "VOO": "new"})
        self.assertIn("KeyError", ctx.store.entry("^IXIC")["err"])


class EcbAndFixturesTests(unittest.TestCase):
    def test_ecb_table_and_store(self):
        """The end-of-period table through market_ref's loader: 72 months 1999-01..2004-12; fetched into the store under
        the sample's name; an answer of the average series (A) refused, nothing saved."""
        self.assertEqual((len(ECB_TABLE), min(ECB_TABLE), max(ECB_TABLE)), (72, (1999, 1), (2004, 12)))
        r = ecb.EcbRates.from_sample()
        self.assertTrue(r.complete())
        self.assertEqual(r.rate(2001, 6), ECB_TABLE[(2001, 6)])
        self.assertIsNone(r.rate(1998, 12))
        st = Store(tempfile.mkdtemp())
        self.assertIsNone(REAL_ECB_ENSURE(st, None), "no file, no network: none")
        http = FakeHttp({"/service/data/EXR/M.USD.EUR.SP00.E": ECB_CSV.encode()})
        got = REAL_ECB_ENSURE(st, http)
        self.assertEqual(got.table, ECB_TABLE)
        self.assertEqual(http.calls[0][1], {"startPeriod": "1999-01", "endPeriod": "2004-12", "format": "csvdata"})
        self.assertTrue(pathlib.Path(st.root, M.ECB_SAMPLE).exists())
        self.assertEqual(REAL_ECB_ENSURE(st, None).table, ECB_TABLE, "from the store, no second request")
        st2 = Store(tempfile.mkdtemp())
        average = ECB_CSV.replace("EXR.M.USD.EUR.SP00.E,", "EXR.M.USD.EUR.SP00.A,")
        with self.assertRaises(base.ProviderError):
            REAL_ECB_ENSURE(st2, FakeHttp({"/service/data/EXR/": average.encode()}))
        self.assertFalse(pathlib.Path(st2.root, M.ECB_SAMPLE).exists())

    def test_fixtures_provider(self):
        p = fixtures.FixtureProvider()
        self.assertAlmostEqual(p.history("VOO", D(2000, 1, 1)).ter, 0.0003, places=6)
        self.assertEqual(p.history("^GSPC", D(2000, 1, 1)).first, D(1984, 12, 1))
        q = p.quotes(["^GSPC", "VOO", "ZZZ"])
        self.assertEqual(set(q), {"^GSPC", "VOO"})
        self.assertAlmostEqual(q["^GSPC"].last, 7619.98, places=2)
        with self.assertRaises(base.NotFound):
            p.history("ZZZ", D(2000, 1, 1))
        with self.assertRaises(base.NotFound):
            p.intraday("VOO")

    def test_pacer(self):
        clock = [1000.0]
        sleeps = []

        def sleep(s):
            sleeps.append(s)
            clock[0] += s
        pacer = base.Pacer(10, sleep, lambda: clock[0])
        pacer.wait()
        clock[0] += 3
        pacer.wait()
        self.assertEqual(sleeps, [7.0])


# ── publisher ────────────────────────────────────────────────────────────────
class PublisherTests(unittest.TestCase):
    def test_shrink_rules(self):
        p = {"v": 1, "pts": "a" * 344, "px": "b" * 344, "bench": "c" * 344, "gross": "d" * 344, "value": 1.0}
        data, notes = publisher.fit("portfolio/hold/MAX", dict(p), 900)
        self.assertIsNotNone(data)
        d = json.loads(data)
        self.assertEqual([k for k in ("pts", "px", "bench", "gross") if k in d], ["pts", "px"])   # gross, then bench dropped
        self.assertIsNone(publisher.fit("portfolio/hold/MAX", {"v": 1, "pts": "a" * 344, "px": "b" * 344}, 500)[0],
                          "refused rather than published without px (M6)")
        self.assertEqual(len(notes), 2)
        live_p = {"v": 1, "ts": 1, "q": {"S%02d" % i: {"last": 1.5, "prev": 1.4, "day": 0.07, "state": "OPEN", "asof": 1, "hi": 1.6, "lo": 1.3} for i in range(16)}}
        data, notes = publisher.fit("live", json.loads(json.dumps(live_p)), 900)
        d = json.loads(data)
        self.assertTrue(all("hi" not in r for r in d["q"].values()), "every day high/low went before a quote did")
        self.assertLess(len(d["q"]), 16)
        self.assertIn("S00", d["q"])
        data, notes = publisher.fit("live", json.loads(json.dumps(live_p)), 1700)
        self.assertEqual(len(json.loads(data)["q"]), 16, "room for the quotes without high/low: none dropped")
        data, notes = publisher.fit("index/VOO/MAX", {"v": 1, "pts": "x" * 2000}, 1900)
        self.assertIsNone(data)
        st = {"v": 1, "state": "ok", "symbols": {"S%02d" % i: {"asof": "2026-09-14", "bars": 6700, "ter": 0.0003, "terSrc": "yahoo"} for i in range(40)},
              "cfgErr": [{"key": "k%d" % i, "code": "RANGE"} for i in range(8)]}
        data, _ = publisher.fit("status", st, 1900)
        self.assertLessEqual(len(data), 1900)
        self.assertEqual(json.loads(data)["state"], "ok")

    def test_generations(self):
        c = publisher.DryRunClient()
        P = publisher.Publisher(c, publisher.Topics("nickoscope_matrix", "a1b2c3"), clock=lambda: 1000)
        s1 = P.new_snapshot("2026-09-14")
        s1.add("index/^GSPC/MAX", {"a": 1})
        s1.add("index/VOO/MAX", {"a": 2})
        P.publish_snapshot(s1, {"state": "ok"})
        topics = [t for t, *_ in c.published]
        self.assertEqual(topics[-1], "nickoscope_matrix/a1b2c3/market/status")
        self.assertEqual(json.loads(c.published[-1][1])["gen"], s1.gen)
        self.assertEqual(json.loads(c.published[0][1])["gen"], s1.gen)
        self.assertTrue(all(r for *_, r in c.published), "retained")
        s2 = P.new_snapshot("2026-09-14")
        self.assertGreater(s2.gen, s1.gen)
        s2.add("index/^GSPC/MAX", {"a": 3})
        P.publish_snapshot(s2, {"state": "ok"})
        cleared = [(t, p) for t, p, q, r in c.published if p == b""]
        self.assertEqual(cleared, [("nickoscope_matrix/a1b2c3/market/index/VOO/MAX", b"")])
        self.assertEqual(json.loads(c.published[-1][1])["gen"], s2.gen)
        P.publish("index/X/MAX", {"pts": "x" * 2000})
        s3 = P.new_snapshot(None)
        P.publish_snapshot(s3, {})
        self.assertEqual(json.loads(c.published[-1][1])["refused"], ["index/X/MAX"])


# ── live and tape ────────────────────────────────────────────────────────────
class LiveTests(unittest.TestCase):
    def test_live_payload(self):
        q = {"VOO": base.Quote("VOO", 699.3, 700.0, 1789416000, "USD", 701.0, 695.5, {"gmtoffset": -14400})}
        p = live.live_payload(q, {"VOO": "OPEN"}, 1789416060)
        self.assertEqual(p["ts"], 1789416060)
        row = p["q"]["VOO"]
        self.assertEqual((row["last"], row["prev"], row["state"], row["asof"], row["hi"], row["lo"]),
                         (699.3, 700.0, "OPEN", "2026-09-14", 701.0, 695.5))       # asof: the exchange's local DATE (the panel's optDate)
        self.assertEqual((row["delay_s"], row["feed"]), (60, "LIVE"))
        self.assertEqual((live.px(12345.6789), live.px(699.3456), live.px(12.34567)), (12345.68, 699.346, 12.3457))
        self.assertAlmostEqual(row["day"], -0.001, places=5)

    def test_tick_polls_only_while_open(self):
        tmp = tempfile.mkdtemp()
        st, ctx, h = fixture_store(tmp)
        h.load()
        f = live.LiveFeature(ctx)
        ctx.data["tape_open"] = lambda: []
        f.tick()
        self.assertEqual(f.polls, 0)
        self.assertEqual([t for t, *_ in ctx.publisher.client.published], [])
        ctx.data["tape_open"] = lambda: ["NYSE"]
        ctx.data["tape_exchange_of"] = lambda sym: "NYSE"
        ctx.data["tape"] = [{"n": "NYSE", "s": "OPEN"}]
        f.tick()
        self.assertEqual(f.polls, 1)
        topic, payload, qos, retain = ctx.publisher.client.published[-1]
        self.assertTrue(topic.endswith("/live") and retain)
        d = json.loads(payload)
        self.assertEqual(d["q"]["VOO"]["state"], "OPEN")
        self.assertEqual(set(d["q"]), {"^GSPC", "^IXIC", "^FCHI", "^GDAXI", "VOO"})
        self.assertLessEqual(len(payload), publisher.PAYLOAD_MAX)


class TapeTests(unittest.TestCase):
    NY = json.loads(sample_bytes("yahoo_VOO_max_1mo.json"))["chart"]["result"][0]["meta"]      # PCX; saved 06:40Z 15 SEP: the periods are the 15 SEP session
    PAR = json.loads(sample_bytes("yahoo_FCHI_max_1mo.json"))["chart"]["result"][0]["meta"]    # PAR
    GER = json.loads(sample_bytes("yahoo_GDAXI.json"))["chart"]["result"][0]["meta"]

    def test_exchange_of_meta(self):
        self.assertEqual([tape.exchange_of_meta(m) for m in (self.NY, self.PAR, self.GER)], ["NYSE", "EURONEXT", "XETRA"])
        aapl = json.loads(sample_bytes("yahoo_AAPL_5y_1wk.json"))["chart"]["result"][0]["meta"]
        self.assertEqual(tape.exchange_of_meta(aapl), "NASDAQ")
        self.assertIsNone(tape.exchange_of_meta({"exchangeName": "CCY"}))

    def test_states_from_the_meta_periods(self):
        at = lambda h, m, day=15: dt.datetime(2026, 9, day, h, m, tzinfo=UTC)
        self.assertEqual(dt.datetime.fromtimestamp(1789479000, UTC), at(13, 30), "regular.start is 09:30 New York on 15 SEP")
        self.assertEqual(tape.state_from_meta(self.NY, at(14, 30)), ("OPEN", dt.datetime.fromtimestamp(1789502400, UTC)))
        self.assertEqual(tape.state_from_meta(self.NY, at(21, 0))[0], "POST")
        self.assertEqual(tape.state_from_meta(self.NY, at(9, 0))[0], "PRE")
        self.assertEqual(tape.state_from_meta(self.NY, at(3, 0)), ("CLOSED", dt.datetime.fromtimestamp(1789459200, UTC)))
        self.assertEqual(tape.state_from_meta(self.NY, at(14, 30, 14)), ("CLOSED", dt.datetime.fromtimestamp(1789459200, UTC)),
                         "the day before: Yahoo's periods point at the next session")
        self.assertIsNone(tape.state_from_meta(self.NY, at(14, 30, 16)), "yesterday's periods are not used")
        self.assertEqual(tape.state_from_meta(self.PAR, at(10, 0))[0], "OPEN")
        self.assertEqual(tape.state_from_meta(self.PAR, at(16, 0))[0], "CLOSED")

    def test_exchange_state_falls_back_and_goes_stale(self):
        tue = dt.datetime(2026, 9, 16, 14, 30, tzinfo=UTC)                     # 10:30 New York, Wednesday: yesterday's meta
        st, nxt = tape.exchange_state("NYSE", [self.NY], tue)
        self.assertEqual(st, "OPEN")
        self.assertEqual(nxt, dt.datetime(2026, 9, 16, 20, 0, tzinfo=UTC))
        self.assertEqual(tape.exchange_state("NYSE", [self.NY], tue, last_quote_ts=int(tue.timestamp()) - 1500)[0], "STALE")
        self.assertEqual(tape.exchange_state("NYSE", [self.NY], tue, last_quote_ts=int(tue.timestamp()) - 300)[0], "OPEN")
        sat = dt.datetime(2026, 9, 19, 14, 30, tzinfo=UTC)
        st, nxt = tape.exchange_state("NYSE", [self.NY], sat)
        self.assertEqual((st, nxt.weekday()), ("CLOSED", 0))
        self.assertEqual(tape.exchange_state("TOKYO", [], tue), ("--", None), "the function says unknown; the tape does not send it")

    def test_feature_publishes_on_change(self):
        tmp = tempfile.mkdtemp()
        clock = [1789482600.0]                                                  # 2026-09-15 14:30Z: 10:30 New York, 16:30 Paris
        s = config.from_panel(config.from_defaults(), voo_only(display={"tz": "Europe/Paris"}))
        st = Store(tmp)
        pub = publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3"))
        ctx = Context(s, st, fixtures.FixtureProvider(), pub, lambda *a, **k: None, clock=lambda: clock[0])
        f = tape.TapeFeature(ctx)
        h = history.HistoryFeature(ctx)
        h.fetch_all()
        h.load()                                                                # emits "series": the tape ticks
        rows = json.loads(pub.client.published[-1][1])["x"]
        by = {r["n"]: r for r in rows}
        self.assertEqual((by["NYSE"]["s"], by["NYSE"]["t"]), ("OPEN", "22:00"))   # 20:00Z is 22:00 Paris
        self.assertEqual((by["EURONEXT"]["s"], by["EURONEXT"]["t"]), ("OPEN", "17:30"))
        self.assertEqual((by["XETRA"]["s"], by["XETRA"]["t"]), ("OPEN", "17:30"))
        self.assertEqual(by["NASDAQ"]["s"], "OPEN")
        self.assertNotIn("LSE", by, "an exchange with no known state is left out: the panel refuses '--' (MINOR 3)")
        n = len(pub.client.published)
        clock[0] += 60
        f.tick()
        self.assertEqual(len(pub.client.published), n, "no change, no publish")
        clock[0] += 15 * 60
        f.tick()
        self.assertEqual(len(pub.client.published), n + 1, "the 15-minute republish")
        self.assertEqual(sorted(f.open_exchanges()), ["EURONEXT", "NASDAQ", "NYSE", "XETRA"])


# ── HA discovery ─────────────────────────────────────────────────────────────
class HaEntitiesTests(unittest.TestCase):
    def test_sensors(self):
        s = config.from_panel(config.from_defaults(), voo_only())
        topics = publisher.Topics("nickoscope_matrix", "a1b2c3")
        sens = {oid: cfg for c, oid, cfg in ha_entities.sensors(s, topics, "a1b2c3")}
        for oid in ("portfolio_value", "portfolio_return", "portfolio_cagr", "portfolio_mdd", "portfolio_div", "portfolio_ter_drag",
                    "portfolio_cash", "portfolio_mode", "holding_voo_share", "holding_voo_return", "cash_share", "exchange_nyse", "status"):
            self.assertIn(oid, sens)
        v = sens["portfolio_value"]
        self.assertEqual(v["state_topic"], "nickoscope_matrix/a1b2c3/market/portfolio/hold/MAX")
        self.assertEqual(v["unit_of_measurement"], "EUR")
        self.assertEqual(v["availability"][0]["topic"], "nickoscope_matrix/a1b2c3/market/ha")
        self.assertEqual(v["unique_id"], "matrix_market_a1b2c3_portfolio_value")
        self.assertEqual(v["default_entity_id"], "sensor.matrix_market_portfolio_value")
        self.assertTrue(all("object_id" not in cfg for _, _, cfg in ha_entities.sensors(s, topics, "a1b2c3")), "removed in HA 2026.4")
        self.assertEqual(sens["portfolio_return"]["state_topic"], "nickoscope_matrix/a1b2c3/market/portfolio/hold/1Y")
        self.assertIn("selectattr('sym','eq','VOO')", sens["holding_voo_share"]["value_template"])
        s2 = config.from_panel(s, voo_only(rebal={"mode": "calendar"}))
        self.assertIn("/portfolio/rebal/MAX", {oid: cfg for c, oid, cfg in ha_entities.sensors(s2, topics, "a1b2c3")}["portfolio_value"]["state_topic"])

    def test_publish_and_remove(self):
        tmp = tempfile.mkdtemp()
        s = config.from_panel(config.from_defaults(), {"v": 1, "portfolio": {"positions": [{"sym": "VOO", "w": 50}, {"sym": "ZZZ", "w": 50}]}})
        pub = publisher.Publisher(publisher.DryRunClient(), publisher.Topics("nickoscope_matrix", "a1b2c3"))
        ctx = Context(s, Store(tmp), fixtures.FixtureProvider(), pub, lambda *a, **k: None)
        f = ha_entities.HaEntitiesFeature(ctx, "a1b2c3")
        f.start()
        self.assertEqual(pub.client.published, [], "start() sends nothing: discovery waits for the connection")
        f.on_connect()
        self.assertTrue(all(q == 1 and r for _, _, q, r in pub.client.published), "qos 1, retained")
        topics = [t for t, *_ in pub.client.published]
        self.assertIn("homeassistant/sensor/matrix_market_a1b2c3/holding_zzz_share/config", topics)
        self.assertTrue(all(t.startswith("homeassistant/sensor/matrix_market_a1b2c3/") and t.endswith("/config") for t in topics))
        n = len(topics)
        ctx.settings = config.from_panel(config.from_defaults(), voo_only())
        f.on_config(ctx.settings)
        removed = [t for t, p, q, r in pub.client.published[n:] if p == b""]
        self.assertEqual(sorted(removed), ["homeassistant/sensor/matrix_market_a1b2c3/holding_zzz_return/config",
                                           "homeassistant/sensor/matrix_market_a1b2c3/holding_zzz_share/config"])


    def test_stats_templates_read_keys_the_stats_payload_has(self):
        """Audit MINOR 11: the XIRR sensor read `xirr`, the payload carries `xirr_ann`. Every value_json key a
        stats-topic sensor reads must be a key of a real stats payload (with flows, so xirr_ann has a value)."""
        S = M.load_samples()
        cfg = M.Config(10000.0, "EUR", D(2000, 1, 1), M.preview_config().positions, 1000.0, "year")
        res = ledger.compute(cfg, S["series"], ledger.fx_table(S["series"], ecb.EcbRates.from_sample().rate), "^GSPC")
        payload = ledger.stats_payload(res, "hold", "MAX")
        self.assertIsNotNone(payload["xirr_ann"])
        s = config.from_panel(config.from_defaults(), {"v": 2, "alert": {"drawdown_pct": 20}})
        topics = publisher.Topics("nickoscope_matrix", "a1b2c3")
        stats_sensors = [(oid, cfg) for _, oid, cfg in ha_entities.sensors(s, topics, "a1b2c3") if "/market_stats/" in cfg["state_topic"]]
        self.assertGreater(len(stats_sensors), 15)
        for oid, sc in stats_sensors:
            for key in re.findall(r"value_json\.([A-Za-z_][A-Za-z0-9_]*)", sc["value_template"] + sc.get("json_attributes_template", "")):
                self.assertIn(key, payload, "%s reads %s" % (oid, key))
        self.assertIn("value_json.xirr_ann", dict(stats_sensors)["portfolio_xirr"]["value_template"])

    def test_the_easy_stock_card_follows_the_settings(self):
        from matrix_market import cli
        s = config.from_panel(config.from_defaults(), {"v": 2, "tickers": ["AAPL"], "ticker": "AAPL"})
        self.assertEqual(cli.easy_stock_card(s)[0]["entity"], "sensor.aapl")
        self.assertEqual(cli.easy_stock_card(config.from_defaults())[0]["entity"], "sensor.vfinx", "else the first position")
        view = json.dumps(cli.lovelace_view(config.from_defaults()))
        self.assertIn("sensor.matrix_market_portfolio_value", view)
        self.assertNotIn("sensor.voo", view)


class DiscoveryOnConnectTests(unittest.TestCase):
    """Audit M3: discovery published before the connection was lost (QoS 0 dropped) and the cache then blocked
    a resend. Now: nothing before the broker answers, every config at QoS 1 after it, again on every reconnect,
    and again when Home Assistant says online."""

    def setUp(self):
        FakeClient.auto_connect = False
        self.addCleanup(setattr, FakeClient, "auto_connect", True)
        self.app = MatrixMarket(app_args())
        self.app.initialize()
        settle(self.app)
        self.addCleanup(self.app.terminate)

    def discovery(self, msgs):
        return [m for m in msgs if m[0].startswith("homeassistant/") and m[0].endswith("/config")]

    def test_first_start(self):
        a = self.app
        self.assertEqual(self.discovery(a._mq.dropped + a._mq.queued + a._mq.published), [], "nothing sent before the connection")
        self.assertEqual(a._mq.dropped, [m for m in a._mq.dropped if not m[0].startswith("homeassistant/")])
        a._mq.go_online()
        settle(a)
        sent = self.discovery(a._mq.published)
        n = len(ha_entities.sensors(a._settings, a._topics, "a1b2c3"))
        self.assertEqual(len(sent), n)
        self.assertTrue(all(q == 1 and r for _, _, q, r in sent))
        self.assertIsNotNone(last(a, "status"), "the generation follows the connection")

    def test_reconnect_and_ha_restart_send_it_again(self):
        a = self.app
        a._mq.go_online()
        settle(a)
        n = len(self.discovery(a._mq.published))
        a._mq.disconnect()
        a._mq.go_online()                                  # the cache must not block the resend
        settle(a)
        self.assertEqual(len(self.discovery(a._mq.published)), 2 * n)
        a._on_message(a._mq, None, types.SimpleNamespace(topic="homeassistant/status", payload=b"offline"))
        settle(a)
        self.assertEqual(len(self.discovery(a._mq.published)), 2 * n)
        a._on_message(a._mq, None, types.SimpleNamespace(topic="homeassistant/status", payload=b"online"))
        settle(a)
        self.assertEqual(len(self.discovery(a._mq.published)), 3 * n)


# ── the largest configuration fits the bus ───────────────────────────────────
class SizeTests(unittest.TestCase):
    def test_largest_config(self):
        S = M.load_samples()
        bases = [S["series"][k] for k in ("VOO", "^GSPC", "^IXIC", "^FCHI", "^GDAXI")]
        series = {}
        for k in range(16):
            b = bases[k % len(bases)]
            c0 = b.closes[0]
            power = 0.7 + 0.1 * (k % 7)
            series["SYN%02dXXX" % k] = M.Series("SYN%02dXXX" % k, "USD", list(b.dates), [c0 * (c / c0) ** power for c in b.closes],
                                                 list(b.dividends), 0.0045, dict(b.meta))
        for k, (sym, mn, s) in enumerate(S["indices"]):
            series[sym] = s
        series["EURUSD=X"] = S["series"]["EURUSD=X"]
        payload = {"v": 1, "indices": [{"sym": s, "name": "IDX%d" % i} for i, s in enumerate(["^GSPC", "^IXIC", "^FCHI", "^GDAXI", "SYN00XXX", "SYN01XXX", "SYN02XXX", "SYN03XXX"])],
                   "tickers": ["SYN%02dXXX" % k for k in range(8, 16)],
                   "portfolio": {"positions": [{"sym": "SYN%02dXXX" % k, "w": 6.25, "entry": "2015-06-15"} for k in range(16)]},
                   "lines": {"gross": True}}
        s = config.from_panel(config.from_defaults(), payload)
        self.assertEqual(s.errors, [])
        cfg = s.ref_config()
        fx = ledger.fx_table(series, ecb.EcbRates.from_sample().rate)
        res = ledger.compute(cfg, series, fx, "^GSPC")
        sizes = {}
        for mode in res.modes:
            for preset in list(M.PRESETS) + list(windows.EXTRA_PRESETS):
                p = ledger.portfolio_payload(res, mode, preset, {"px": True, "bench": True, "gross": True})
                data, notes = publisher.fit("portfolio/%s/%s" % (mode, preset), p)
                self.assertEqual(notes, [], "no line had to go: four lines and the mdd dates fit")
                sizes["portfolio/%s/%s" % (mode, preset)] = len(data)
            data, notes = publisher.fit("holdings/%s" % mode, ledger.holdings_payload(res, mode))
            self.assertEqual(notes, [])
            sizes["holdings/%s" % mode] = len(data)
        for it in s.indices() + s.tickers():
            for preset in M.PRESETS:
                p = history.index_payload(series[it["sym"]], preset, res.asof, cfg.inception, it["name"])
                sizes["index/%s/%s" % (it["sym"], preset)] = len(publisher.encode(p))
        q = {sym: base.Quote(sym, 12345.6789, 12300.1234, 1789416000, "USD", 12400.5, 12200.25, {"gmtoffset": -14400}) for sym in s.live_symbols()}
        data, notes = publisher.fit("live", live.live_payload(q, {sym: "OPEN" for sym in q}, 1789416060))
        kept = json.loads(data)["q"]
        self.assertGreaterEqual(len(kept), 16, "the design's budget: 16 quotes; beyond that the last ones go")
        self.assertTrue(all("hi" not in r and "delay_s" not in r for r in kept.values()), "the day high/low and the delay go before a quote does")
        q16 = dict(list(q.items())[:16])
        data16, notes16 = publisher.fit("live", live.live_payload(q16, {sym: "OPEN" for sym in q16}, 1789416060))
        self.assertEqual(len(json.loads(data16)["q"]), 16, "16 quotes fit, with their feed and delay (docs/18)")
        sizes["live"] = len(data)
        for mode in res.modes:
            for preset in M.PRESETS:
                data, notes = publisher.fit("stats/%s/%s" % (mode, preset), ledger.stats_payload(res, mode, preset))
                self.assertEqual(notes, [], "the stats topic fits whole for 16 positions")
                sizes["stats/%s/%s" % (mode, preset)] = len(data)
        sizes["tape"] = len(publisher.encode({"v": 1, "ts": 1, "x": [{"n": n, "s": "CLOSED", "t": "09:30"} for n in registry.EXCHANGES.names()]}))
        st = Store(tempfile.mkdtemp())
        status = {"v": 1, "asof": "2026-09-14", "fetched": "2026-09-15T07:02Z", "state": "ok", "err": "",
                  "symbols": {sym: {"asof": "2026-09-14", "bars": 6700, "ter": 0.0045, "terSrc": "yahoo"} for sym in s.watched_symbols()}}
        data, notes = publisher.fit("status", status)
        sizes["status"] = len(data)
        worst = max(sizes.values())
        self.assertLessEqual(worst, publisher.PAYLOAD_MAX, sorted(sizes.items(), key=lambda kv: -kv[1])[:3])
        self.assertEqual(len(sizes), 2 * 11 + 2 + 16 * 6 + 3 + 2 * 6)


# ── the v2 rules, by hand ────────────────────────────────────────────────────
def mk(sym, cur, bars, divs=(), ter=None):
    return M.Series(sym, cur, [D.fromisoformat(d) for d, _ in bars], [float(c) for _, c in bars],
                    [(D.fromisoformat(d), float(a)) for d, a in divs], ter)


FLAT_FX = M.FxTable([D(2000, 1, 1)], [1.0])
A_BARS = [("2023-01-02", 100), ("2023-06-01", 110), ("2023-12-29", 120), ("2024-06-03", 140), ("2024-12-31", 150)]
B_BARS = [("2023-01-02", 100), ("2023-06-01", 100), ("2023-12-29", 100), ("2024-06-03", 100), ("2024-12-31", 100)]


def two_asset(a_divs=(), **cfg_kw):
    A, B = mk("A", "EUR", A_BARS, a_divs), mk("B", "EUR", B_BARS)
    series = {"A": A, "B": B}
    cfg = M.Config(10000.0, "EUR", D(2023, 1, 2), [M.Position("A", 50.0), M.Position("B", 50.0)], **cfg_kw)
    return cfg, series, M.timeline(list(series.values()), cfg.inception)


class RulesTests(unittest.TestCase):
    """test_market_ref's two-asset drift (A 100 -> 150, B flat) under each rule, the arithmetic by hand."""

    def run_ext(self, cfg, series, days, mode, rules, dividends=True, bar_years=None):
        return M.simulate_ext(cfg, series, FLAT_FX, days, mode, dividends, bar_years, rules)

    def test_defaults_equal_simulate(self):
        cfg, series, days = two_asset([("2023-06-01", 10)])
        for mode in ("hold", "rebal"):
            a, b = M.simulate(cfg, series, FLAT_FX, days, mode), self.run_ext(cfg, series, days, mode, M.Rules())
            self.assertEqual((a.v, a.cash, a.qty, a.events), (b.v, b.cash, b.qty, b.events))
        self.assertEqual(self.run_ext(cfg, series, days, "hold", None).v[-1], M.simulate(cfg, series, FLAT_FX, days, "hold").v[-1])

    def test_dividend_modes(self):
        """A pays 10 on 2023-06-01 (A = 110, 50 sh -> 500). Sweep: 500 cash until 2023-12-29, then 250 into A at 120
        (2.0833 sh) and 250 into B at 100 (2.5 sh): end 52.0833 x 150 + 52.5 x 100 = 13 062.50. Reinvest: 500 buys A at
        110 (4.5455 sh): end 54.5455 x 150 + 5 000 = 13 181.82. Drop: 12 500, the no-dividend case."""
        cfg, series, days = two_asset([("2023-06-01", 10)])
        sweep = self.run_ext(cfg, series, days, "hold", M.Rules())
        self.assertAlmostEqual(sweep.v[-1], 13062.5)
        self.assertAlmostEqual(sweep.div[-1], 500.0)
        re = self.run_ext(cfg, series, days, "hold", M.Rules(dividends="reinvest_paydate"))
        self.assertAlmostEqual(re.v[-1], 54.545454545 * 150 + 5000, places=4)
        self.assertAlmostEqual(re.cash[days.index(D(2023, 6, 1))], 0.0)
        self.assertAlmostEqual(re.div[-1], 500.0)
        drop = self.run_ext(cfg, series, days, "hold", M.Rules(dividends="drop"))
        self.assertAlmostEqual(drop.v[-1], 12500.0)
        self.assertEqual(drop.div[-1], 0.0)

    def test_withholding_tax(self):
        """30 % off the 500: 350 to cash, 175 + 175 bought at the year-end: (50 + 175/120) x 150 + 51.75 x 100 = 12 893.75."""
        cfg, series, days = two_asset([("2023-06-01", 10)])
        L = self.run_ext(cfg, series, days, "hold", M.Rules(tax_div_pct=30))
        self.assertAlmostEqual(L.v[-1], 12893.75)
        self.assertAlmostEqual(L.tax[-1], 150.0)
        self.assertAlmostEqual(L.div[-1], 350.0)

    def test_trade_costs(self):
        """1 per trade inside the budget: the start buys 4 999 of each (cash exactly 0), end 49.99 x 250 = 12 497.50."""
        cfg, series, days = two_asset()
        L = self.run_ext(cfg, series, days, "hold", M.Rules(cost_fixed=1.0))
        self.assertAlmostEqual(L.cash[0], 0.0)
        self.assertAlmostEqual(L.v[-1], 49.99 * 150 + 49.99 * 100)
        self.assertEqual((L.trades, L.costs[-1]), (2, 2.0))
        Lp = self.run_ext(cfg, series, days, "rebal", M.Rules(cost_pct=1.0))
        self.assertGreater(Lp.trades, 2, "the year-end rebalances trade")
        self.assertLess(Lp.v[-1], 12375.0, "costs come off the reference's 12 375")

    def test_bands(self):
        """Bands 5/25 checked daily: A's weight 52.4 % (06-01) and 54.5 % (12-29) stay inside 45-55; 58.3 % on 2024-06-03
        breaks out -> rebalance at V 12 000: A 6 000/140 = 42.857 sh, B 60 sh; end 42.857 x 150 + 6 000 = 12 428.57."""
        cfg, series, days = two_asset()
        L = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="bands", band_check="daily"))
        self.assertEqual(L.rebal_dates, [D(2024, 6, 3)])
        self.assertAlmostEqual(L.v[-1], 6000 / 140 * 150 + 6000, places=6)
        cal = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="calendar"))
        self.assertAlmostEqual(cal.v[-1], 12375.0)                 # the reference's REBAL
        self.assertEqual(cal.rebal_dates, [D(2023, 12, 29), D(2024, 12, 31)])
        both = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="calendar_or_bands", band_check="daily"))
        # after the 2023-12-29 rebalance A is 45.83 sh: on 2024-06-03 6 416.67 / 11 916.67 = 53.8 %, inside 45-55
        self.assertEqual(both.rebal_dates, [D(2023, 12, 29), D(2024, 12, 31)])
        tight = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="calendar_or_bands", band_check="daily", band_abs_pts=2.0))
        self.assertEqual(tight.rebal_dates, [D(2023, 6, 1), D(2023, 12, 29), D(2024, 6, 3), D(2024, 12, 31)],
                         "52.4 % on 06-01 and 53.8 % on 2024-06-03 break a 2-point band; the calendar dates stay")
        hold = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="hold"))
        self.assertAlmostEqual(hold.v[-1], 12500.0)

    def test_quarterly_calendar(self):
        """Period ends 2023-06-01, 2023-12-29, 2024-06-03, 2024-12-31 (the fixture's bars in those quarters); the chain
        10 500 -> A 5 250/110, B 52.5; 10 977.27 -> A 5 488.64/120, B 54.886; 11 892.05 -> A 5 946.02/140, B 59.46;
        end 42.4716 x 150 + 5 946.02 = 12 316.76."""
        cfg, series, days = two_asset()
        L = self.run_ext(cfg, series, days, "rebal", M.Rules(rebal="calendar", calendar="quarterly"))
        self.assertEqual(L.rebal_dates, [D(2023, 6, 1), D(2023, 12, 29), D(2024, 6, 3), D(2024, 12, 31)])
        qa = 5250 / 110
        v2 = qa * 120 + 5250
        qa = v2 / 2 / 120
        v3 = qa * 140 + v2 / 2
        qa = v3 / 2 / 140
        v4 = qa * 150 + v3 / 2
        self.assertAlmostEqual(L.v[-1], v4, places=6)
        self.assertAlmostEqual(L.v[-1], 12316.76, places=1)

    def test_withdrawal_and_twr(self):
        """1 000 a year from the anniversary (2024-01-02 -> the 2024-06-03 bar): no cash, so pro rata sales at
        A 140 / B 100 (583.33 + 416.67); end 45.833 x 250 = 11 458.33. Selling pro rata leaves the TWR at the
        drift's +25 %; the XIRR is positive and below it."""
        cfg, series, days = two_asset()
        L = self.run_ext(cfg, series, days, "hold", M.Rules(withdraw_amount=1000.0))
        self.assertAlmostEqual(L.withdrawn[-1], 1000.0)
        self.assertAlmostEqual(L.v[-1], (50 - 1000 / 12000 * 50) * 250, places=6)
        self.assertEqual(L.flows[days.index(D(2024, 6, 3))], -1000.0)
        self.assertAlmostEqual(M.twr(L.v, L.flows, 0, len(days) - 1), 0.25, places=9)
        x = M.ledger_xirr(cfg, L)
        self.assertTrue(0 < x < 0.25)
        Lp = self.run_ext(cfg, series, days, "hold", M.Rules(withdraw_pct=10.0, withdraw_every="month"))
        self.assertGreater(Lp.withdrawn[-1], 1000.0)

    def test_contribution_only_and_cash_yield(self):
        """1 000 a year, contrib_only: on 2024-06-03 A = 7 000, B = 5 000, V 13 000, targets 6 500: the 1 000 buys B
        (10 sh) and nothing of A. Without the rule the cash waits for the 31 December sweep (5 sh of A, 5 of B)."""
        cfg, series, days = two_asset(contrib_amount=1000.0)
        L = self.run_ext(cfg, series, days, "hold", M.Rules(contrib_only=True))
        i = days.index(D(2024, 6, 3))
        self.assertAlmostEqual(L.cash[i], 0.0)
        self.assertEqual((round(L.qty["A"], 6), round(L.qty["B"], 6)), (50.0, 60.0))
        self.assertEqual(L.flows[i], 1000.0)
        plain = self.run_ext(cfg, series, days, "hold", M.Rules())
        self.assertAlmostEqual(plain.cash[i], 1000.0)
        self.assertAlmostEqual(plain.qty["B"], 55.0)
        # The cash waits from June to December earning nothing: 1.05 x 1.047619 x 1.090909 x 1.038462 - 1 = 24.6 %,
        # while value_end / value_start - 1 = 35 % counts the contribution as return (docs/19 §4's point).
        # PP's convention (the contract): the 1 000 counts at the start of 2024-06-03, so that day is 13 000 / (11 000 + 1 000):
        # 1.05 x 11 000/10 500 x 13 000/12 000 x 13 500/13 000 - 1 = 11/10 x 13 500/12 000 - 1 = 23.75 %
        self.assertAlmostEqual(M.twr(plain.v, plain.flows, 0, len(days) - 1), 1.05 * (11000 / 10500) * (13000 / 12000) * (13500 / 13000) - 1, places=9)
        self.assertAlmostEqual(M.twr(plain.v, plain.flows, 0, len(days) - 1), 0.2375, places=9)
        self.assertAlmostEqual(plain.v[-1] / plain.v[0] - 1, 0.35)
        cfg2 = M.Config(10000.0, "EUR", D(2023, 1, 2), [M.Position("A", 40.0), M.Position("B", 40.0)])
        Ly = M.simulate_ext(cfg2, series, FLAT_FX, days, "hold", True, M.calendar_years, M.Rules(cash_yield_pct=5.0))
        L0 = M.simulate_ext(cfg2, series, FLAT_FX, days, "hold", True, M.calendar_years, M.Rules())
        i = days.index(D(2023, 6, 1))
        self.assertAlmostEqual(Ly.cash[i], 2000 * (1 + 0.05 * (D(2023, 6, 1) - D(2023, 1, 2)).days / 365.25), places=6)
        self.assertAlmostEqual(Ly.cash[-1], 2000.0, msg="the interest above the cash floor is swept into positions at the year-end")
        self.assertGreater(Ly.v[-1], L0.v[-1])

    def test_returns_helpers(self):
        self.assertAlmostEqual(M.twr([100.0, 160.0], [0.0, 50.0], 0, 1), 160 / 150 - 1, msg="PP's convention, the default")
        self.assertAlmostEqual(M.twr([100.0, 160.0], [0.0, 50.0], 0, 1, "close"), 0.10)
        self.assertAlmostEqual(M.xirr([(D(2021, 1, 4), -1000.0), (D(2022, 1, 4), 1100.0)]), 0.10, places=9,
                               msg="Excel's 365-day year: one 365-day year at 10 %")
        self.assertEqual(M.twr([100.0, 121.0], [0.0, 0.0], 0, 1), 121 / 100 - 1, "no flows: the direct ratio, bit for bit")
        self.assertAlmostEqual(M.xirr([(D(2020, 1, 1), -1000.0), (D(2022, 1, 1), 1210.0)]), 0.0999, places=3)
        self.assertIsNone(M.xirr([(D(2020, 1, 1), -1.0)]))
        self.assertAlmostEqual(M.annualised(0.21, 2.0), 0.10)
        self.assertIsNone(M.annualised(0.10, 0.9), "never under a year (GIPS 2.A.12)")
        self.assertAlmostEqual(M.annualised(0.10, 365 / 365.25), 0.10, places=3, msg="365 days is a year")
        self.assertIsNone(M.annualised(0.10, 364 / 365.25))
        self.assertEqual(M.band_bounds(30.0), (25.0, 35.0))
        self.assertEqual(M.band_bounds(1.25), (0.9375, 1.5625))
        self.assertEqual(sorted(M.period_ends([D(2024, 1, 2), D(2024, 3, 28), D(2024, 4, 1), D(2024, 12, 31)], "quarterly")),
                         [D(2024, 3, 28), D(2024, 12, 31)])


class FlowAwareFiguresTests(unittest.TestCase):
    """Audit M5 and M4, and the reserve rule of MINOR 13."""

    @staticmethod
    def weekdays(a, b):
        out, d = [], a
        while d <= b:
            if d.weekday() < 5:
                out.append(d)
            d += dt.timedelta(days=1)
        return out

    def test_contributions_do_not_count_as_return(self):
        """The auditor's case: 10 000 EUR from 2000 plus 1 000 a year into a fund that grows 7 % a year, weekday bars
        2000-01-03..2010-12-31. On V the CAGR read 12.03 % and the volatility 5.50 %. On the unit value: TWR a year
        6.666 % (Portfolio Performance's convention; the close convention reads 6.668 %, the auditor's figure),
        the CAGR equal to it, the volatility 0.10 % (only the year-end cash sweeps), no drawdown."""
        days = self.weekdays(D(2000, 1, 3), D(2010, 12, 31))
        f = M.Series("F", "EUR", days, [100 * 1.07 ** ((d - days[0]).days / 365.25) for d in days])
        cfg = M.Config(10000.0, "EUR", D(2000, 1, 1), [M.Position("F", 100.0)], 1000.0, "year")
        res = ledger.compute(cfg, {"F": f}, M.FxTable([D(1990, 1, 1)], [1.0]), None)
        p = ledger.portfolio_payload(res, "hold", "MAX")
        sp = ledger.stats_payload(res, "hold", "MAX")
        L = res.ledger("hold")
        years = M.years_between(L.days[0], L.days[-1])
        self.assertAlmostEqual(p["ann"], 0.06666, places=4)
        self.assertAlmostEqual((1 + M.twr(L.v, L.flows, 0, len(L.v) - 1, "close")) ** (1 / years) - 1, 0.06668, places=4)
        self.assertAlmostEqual((L.v[-1] / L.v[0]) ** (1 / years) - 1, 0.1203, places=4, msg="what the CAGR read on V")
        self.assertAlmostEqual(p["cagr"], p["ann"], places=6)
        self.assertAlmostEqual(sp["vol"], 0.0010, places=4)
        self.assertGreater(sp["mdd"]["mdd"], -0.001)
        self.assertEqual((p["flows"], sp["flows"]), (True, True))
        self.assertAlmostEqual(p["sinceStart"], L.v[-1] / (10000.0 + 10 * 1000.0) - 1, places=6, msg="ten contributions")
        u = ledger.unit_values(L)
        self.assertAlmostEqual(u[-1] / u[0] - 1, M.twr(L.v, L.flows, 0, len(L.v) - 1), places=9, msg="the unit value is the TWR")

    def test_since_start_counts_withdrawals(self):
        """test_withdrawal_and_twr's ledger: V ends at 11 458.33 after 1 000 taken out: (11 458.33 + 1 000) / 10 000 - 1."""
        cfg, series, days = two_asset()
        res = ledger.compute(cfg, series, FLAT_FX, None, rules=M.Rules(withdraw_amount=1000.0), days=days)
        p = ledger.portfolio_payload(res, "hold", "MAX")
        self.assertAlmostEqual(p["sinceStart"], (res.ledger("hold").v[-1] + 1000.0) / 10000.0 - 1, places=6)
        self.assertAlmostEqual(p["twr"], 0.25, places=6)

    def test_the_blend_is_in_the_portfolio_currency(self):
        """A USD leg 100 -> 110 while EURUSD=X goes 1.00 -> 1.25: in EUR 100 -> 88, so 10 000 EUR -> 8 800."""
        leg = M.Series("LEG", "USD", [D(2020, 1, 2), D(2021, 1, 4)], [100.0, 110.0], [], None, {}, [100.0, 110.0])
        fx = M.FxTable([D(2020, 1, 2), D(2021, 1, 4)], [1.0, 1.25])
        line = ledger.blend_benchmark([{"sym": "LEG", "w": 100.0}], {"LEG": leg}, leg.dates, 10000.0, D(2020, 1, 2), fx, "EUR")
        self.assertEqual(line[0], 10000.0)
        self.assertAlmostEqual(line[1], 8800.0)
        with self.assertRaises(M.NoFxRate):
            ledger.blend_benchmark([{"sym": "LEG", "w": 100.0}], {"LEG": leg}, leg.dates, 10000.0, D(2020, 1, 2),
                                   M.FxTable([], []), "EUR")
        cfg = M.Config(10000.0, "EUR", D(2020, 1, 2), [M.Position("LEG", 100.0)])
        fund = M.Series("FUND", "EUR", leg.dates, [1.0, 1.0])
        res = ledger.compute(M.Config(10000.0, "EUR", D(2020, 1, 2), [M.Position("FUND", 100.0)]), {"FUND": fund, "LEG": leg},
                             M.FxTable([], []), "LEG", blend=[{"sym": "LEG", "w": 100.0}], extra_bench=["LEG"])
        self.assertEqual((res.bench, res.benches), (None, {}), "refused, never a line in its own currency")
        self.assertEqual(sorted(res.bench_err), ["LEG: no FX rate", "blend: no FX rate"])

    def test_the_benchmark_starts_on_the_first_trading_day(self):
        """On daily bars the inception 2000-01-01 (a Saturday) has no bar: the benchmark takes its base on the ledger's first
        day, Monday 2000-01-03, and starts at the capital. The golden report on the real store showed `benchmark None`:
        close_on(2000-01-01) was None and the line was dropped without a word. An index with no bar that day is named."""
        days = FlowAwareFiguresTests.weekdays(D(2000, 1, 3), D(2000, 3, 31))
        idx = M.Series("^TR", "USD", days, [1000.0 + i for i in range(len(days))], [], None, {}, [1000.0 + i for i in range(len(days))])
        fund = M.Series("F", "EUR", days, [100.0] * len(days))
        fx = M.FxTable([D(1999, 1, 1)], [1.25])
        res = ledger.compute(M.Config(10000.0, "EUR", D(2000, 1, 1), [M.Position("F", 100.0)]), {"F": fund, "^TR": idx}, fx, "^TR")
        self.assertEqual(res.days[0], D(2000, 1, 3))
        self.assertEqual(res.bench_sym, "^TR")
        self.assertAlmostEqual(res.bench[0], 10000.0)
        self.assertAlmostEqual(res.bench[-1], 10000.0 * idx.closes[-1] / idx.closes[0])
        late = M.Series("^LATE", "USD", days[5:], [1.0] * (len(days) - 5), [], None, {}, [1.0] * (len(days) - 5))
        res = ledger.compute(M.Config(10000.0, "EUR", D(2000, 1, 1), [M.Position("F", 100.0)]), {"F": fund, "^LATE": late}, fx, "^LATE")
        self.assertIsNone(res.bench)
        self.assertEqual(res.bench_err, ["^LATE: no bar on 2000-01-03"])

    def test_the_reserve_rule(self):
        """A trades from 2022-01-03 at 100; B starts 2023-06-01. 10 000, 50/50, REBAL on the calendar.
          start: A 50 sh (5 000), B's 5 000 held back in cash.
          2022-12-30, A 110, V = 5 500 + 5 000 = 10 500, only A trades:
            value_weight:   A -> 10 500 x 0.5 = 5 250 (47.7273 sh), cash 5 250
            capital_weight: held back (10 000 + 0) x 0.5 = 5 000; A -> 10 500 - 5 000 = 5 500 (50 sh), cash 5 000
          2023-12-29, A 120, B 100, both trade:
            value_weight:   V = 5 250 / 110 x 120 + 5 250 = 10 977.27 -> 5 488.64 each
            capital_weight: V = 50 x 120 + 5 000 = 11 000 -> 5 500 each
        HOLD does not look at the rule."""
        A = mk("A", "EUR", [("2022-01-03", 100), ("2022-12-30", 110), ("2023-06-01", 110), ("2023-12-29", 120), ("2024-01-02", 120)])
        B = mk("B", "EUR", [("2023-06-01", 100), ("2023-12-29", 100), ("2024-01-02", 100)])
        cfg = M.Config(10000.0, "EUR", D(2022, 1, 3), [M.Position("A", 50.0), M.Position("B", 50.0)])
        series = {"A": A, "B": B}
        days = M.timeline([A, B], cfg.inception)
        i = days.index(D(2022, 12, 30))
        vw = M.simulate_ext(cfg, series, FLAT_FX, days, "rebal", True, None, M.Rules(reserve="value_weight"))
        cw = M.simulate_ext(cfg, series, FLAT_FX, days, "rebal", True, None, M.Rules(reserve="capital_weight"))
        self.assertAlmostEqual(vw.cash[i], 5250.0)
        self.assertAlmostEqual(cw.cash[i], 5000.0)
        self.assertAlmostEqual(vw.v[-1], 5250 / 110 * 120 + 5250)
        self.assertAlmostEqual(cw.v[-1], 11000.0)
        self.assertAlmostEqual(cw.qty["A"], 5500.0 / 120)
        self.assertEqual(vw.v, M.simulate(cfg, series, FLAT_FX, days, "rebal").v, "value_weight is the reference")
        self.assertEqual(M.simulate_ext(cfg, series, FLAT_FX, days, "hold", True, None, M.Rules(reserve="capital_weight")).v,
                         M.simulate(cfg, series, FLAT_FX, days, "hold").v)
        self.assertEqual(config.from_defaults().rules().reserve, "value_weight")
        s = config.from_args({"rebal": {"reserve": "capital_weight"}})
        self.assertEqual((s.errors, s.rules().reserve), ([], "capital_weight"))


class PanelEdgeRulesTests(unittest.TestCase):
    """The panel's refusals the app must not trigger: px always (M6), stats outside market/# (MINOR 1), retained
    leaves remembered across a restart (MINOR 2), no '--' (MINOR 3), names as validName (MINOR 4), delay_s with
    every feed, xirr_ann with every flows."""

    def test_names_as_the_panel_validates_them(self):
        base_s = config.from_defaults()
        s = config.from_panel(base_s, {"v": 2, "indices": [{"sym": "^GSPC", "name": "spx"}]})
        self.assertEqual([e["key"] for e in s.errors], ["indices"], "lower case is refused, as validName does")
        s = config.from_panel(base_s, {"v": 2, "indices": [{"sym": "^GSPC", "name": "S&P"}, {"sym": "^IXIC", "name": "NDX"}]})
        self.assertEqual([e["code"] for e in s.errors], ["TYPE"])
        s = config.from_panel(base_s, {"v": 2, "indices": [{"sym": "^GSPC", "name": "SPX-TR"}, {"sym": "eurusd=x"}]})
        self.assertEqual(s.errors, [])
        self.assertEqual([i["name"] for i in s.indices()], ["SPX-TR", "EURUSD=X"])
        for item in config.DEFAULT_INDICES + config.SCHEMA_BY_KEY["bench.extra"].default:
            self.assertRegex(item["name"], r"^[A-Z0-9.^=\-]{1,8}$")

    def test_stats_leave_the_market_tree(self):
        t = publisher.Topics("nickoscope_matrix", "a1b2c3")
        self.assertEqual(t.leaf("stats/hold/MAX"), "nickoscope_matrix/a1b2c3/market_stats/hold/MAX")
        self.assertFalse(t.leaf("stats/hold/MAX").startswith(t.root))
        self.assertEqual(t.leaf("portfolio/hold/MAX"), "nickoscope_matrix/a1b2c3/market/portfolio/hold/MAX")

    def test_leaves_survive_a_restart(self):
        st = Store(tempfile.mkdtemp())
        c1 = publisher.DryRunClient()
        P1 = publisher.Publisher(c1, publisher.Topics("n", "a1b2c3"), clock=lambda: 1000, store=st)
        s1 = P1.new_snapshot(None)
        s1.add("index/AAA/MAX", {"a": 1})
        s1.add("index/BBB/MAX", {"a": 2})
        P1.publish_snapshot(s1, {"state": "ok"})
        c2 = publisher.DryRunClient()                       # the app restarts; BBB left the lists meanwhile
        P2 = publisher.Publisher(c2, publisher.Topics("n", "a1b2c3"), clock=lambda: 2000, store=st)
        self.assertEqual(P2.generation_leaves, {"index/AAA/MAX", "index/BBB/MAX"})
        s2 = P2.new_snapshot(None)
        s2.add("index/AAA/MAX", {"a": 3})
        P2.publish_snapshot(s2, {"state": "ok"})
        self.assertIn(("n/a1b2c3/market/index/BBB/MAX", b"", 0, True), c2.published)
        self.assertEqual(st.load_table("published_leaves")["leaves"], ["index/AAA/MAX"])
        P2.clear("index/AAA/MAX")
        self.assertEqual(st.load_table("published_leaves")["leaves"], [])

    def test_the_direct_payloads_carry_v(self):
        """The panel's decoder refused `tape` for a missing `v` (its host binary on the dry run, 2026-09-15): tape, live
        and intraday are published directly, not through a snapshot. Publisher.publish now sets it for every payload."""
        c = publisher.DryRunClient()
        P = publisher.Publisher(c, publisher.Topics("n", "a1b2c3"))
        for leaf, payload in (("tape", {"ts": 1789416000, "x": [{"n": "NYSE", "s": "OPEN"}]}), ("live", {"ts": 1789416000, "q": {}}),
                              ("intraday/VOO", {"sym": "VOO", "ts": 1789416000, "n": 0, "min": 0, "max": 0, "pts": ""})):
            P.publish(leaf, payload)
            self.assertEqual(json.loads(c.published[-1][1])["v"], 1, leaf)
        tmp = tempfile.mkdtemp()
        st, ctx, h = fixture_store(tmp)
        f = tape.TapeFeature(ctx)
        h.load()
        taped = [json.loads(p) for t, p, *_ in ctx.publisher.client.published if t.endswith("/tape")]
        self.assertTrue(taped and all(x["v"] == 1 for x in taped), "the tape feature's own payload")

    def test_no_quote_without_a_known_state(self):
        tmp = tempfile.mkdtemp()
        st, ctx, h = fixture_store(tmp)
        h.load()
        f = live.LiveFeature(ctx)
        ctx.data["tape_open"] = lambda: ["NYSE"]
        ctx.data["tape_exchange_of"] = lambda sym: "NYSE" if sym == "VOO" else "TOKYO"
        ctx.data["tape"] = [{"n": "NYSE", "s": "OPEN"}, {"n": "TOKYO", "s": "--"}]
        f.tick()
        d = json.loads(ctx.publisher.client.published[-1][1])
        self.assertEqual(set(d["q"]), {"VOO"}, "the quotes whose exchange state is unknown are left out")
        self.assertTrue(all("delay_s" in q and q["feed"] in ("LIVE", "DELAYED", "STALE", "CLOSED") for q in d["q"].values()),
                        "delay_s with every feed: the panel never shows LIVE without it")

    def test_xirr_ann_with_every_flows(self):
        S = M.load_samples()
        cfg = M.Config(10000.0, "EUR", D(2000, 1, 1), M.preview_config().positions, 1000.0, "year")
        res = ledger.compute(cfg, S["series"], ledger.fx_table(S["series"], ecb.EcbRates.from_sample().rate), "^GSPC")
        for preset in M.PRESETS:
            p = ledger.portfolio_payload(res, "hold", preset)
            self.assertIs(p["flows"], True)
            self.assertIn("xirr_ann", p)
            self.assertIsNotNone(p["xirr_ann"])
            self.assertIn("fx_effect", p)
            self.assertNotIn("fx", p)
        real = M.ledger_xirr
        M.ledger_xirr = lambda *a, **k: None                # a solver that finds no rate
        try:
            p = ledger.portfolio_payload(res, "hold", "MAX")
        finally:
            M.ledger_xirr = real
        self.assertEqual((p["flows"], p["xirr_ann"]), (True, None), "the key stays, null")


class RobustnessTests(unittest.TestCase):
    """Audit MINOR 5-9 and NIT 5: every change class handled, the debounce guarded, terminate in order with one
    writer per store, a failing feature keeping its records, only missing or stale symbols fetched, the keepalive."""

    def app(self, **over):
        a = MatrixMarket(app_args(**over))
        a.initialize()
        settle(a)
        self.addCleanup(a.terminate)
        return a

    def config(self, a, payload):
        a._on_message(a._mq, None, types.SimpleNamespace(topic=ROOT + "config", payload=json.dumps(payload).encode()))
        settle(a)

    def test_classes_are_not_exclusive(self):
        a = config.from_defaults()
        b = config.from_panel(a, {"v": 2, "ticker": "VFINX", "live": {"poll_s": 90}, "display": {"tz": "Europe/Paris"}})
        self.assertEqual(config.classes(config.changed_keys(a.data, b.data)), {"ticker", "schedule", "light"})
        self.assertEqual(config.classify(config.changed_keys(a.data, b.data)), "schedule")

    def test_ticker_and_schedule_in_one_save(self):
        a = self.app()
        self.config(a, {"v": 2, "ticker": "^GSPC"})
        n, timers = len(a._mq.published), len(a.every)
        self.config(a, {"v": 2, "ticker": "VOO", "live": {"poll_s": 120}})
        self.assertIn((ROOT + "intraday/^GSPC", b"", 0, True), a._mq.published[n:], "the ticker switch ran")
        self.assertGreater(len(a.every), timers, "and the schedule too")

    def test_schedule_and_light_in_one_save(self):
        a = self.app()
        n, timers = len(a._mq.published), len(a.every)
        self.config(a, {"v": 2, "live": {"poll_s": 120}, "display": {"tz": "Asia/Tokyo"}})
        self.assertGreater(len(a.every), timers)
        self.assertTrue(any(t == ROOT + "tape" for t, *_ in a._mq.published[n:]), "the tape heard the new zone")
        self.assertFalse([t for t in a.timers if t[1] == 5], "no recompute")

    def test_the_gap_reaches_the_pacer(self):
        a = self.app()
        a._provider.pacer = base.Pacer(10)
        self.config(a, {"v": 2, "fetch": {"history_gap_s": 30}})
        self.assertEqual(a._provider.pacer.gap, 30.0)

    def test_one_writer_per_store(self):
        root = tempfile.mkdtemp()
        s1 = Store(root)
        s2 = Store(root, wait_s=0)
        self.assertEqual((s1.writable, s2.writable), (True, False))
        s2.save_table("x", {"a": 1})
        self.assertIsNone(s1.load_table("x"), "the second store wrote nothing")
        self.assertEqual(s2.refused_writes, 1)
        s1.close()
        s3 = Store(root, wait_s=0)
        self.assertTrue(s3.writable, "free again after close()")
        s3.close()

    def test_terminate_in_order(self):
        a = MatrixMarket(app_args())
        a.initialize()
        settle(a)
        ran = []
        a._worker.put(lambda: (time.sleep(0.3), ran.append(1)))
        a.terminate()
        self.assertEqual(ran, [1], "the current job ended before terminate went on")
        self.assertFalse(a._worker.thread.is_alive())
        self.assertEqual(a._mq.published[-1], (ROOT + "ha", "offline", 1, True))
        self.assertIsNone(a._store._lock_fh, "the store let go")
        self.assertTrue(Store(a._store.root, wait_s=0).writable)

    def test_a_failing_feature_keeps_its_records(self):
        a = self.app()
        pf = next(f for f in a._features if f.name == "portfolio")

        def boom(snapshot, fetch=True):
            raise KeyError("no such series")
        pf.contribute = boom
        n = len(a._mq.published)
        a._refresh("test", False)
        cleared = [t for t, p, *_ in a._mq.published[n:] if p == b""]
        self.assertEqual(cleared, [], "nothing cleared")
        self.assertEqual(last(a, "status")["failed"], ["portfolio"])
        self.assertIn("portfolio/hold/MAX", a._publisher.generation_leaves)
        self.assertTrue(any("KeyError in portfolio.contribute" in m and "no such series" in m for _, m in a.logs))

    def test_only_missing_or_stale_symbols_are_fetched(self):
        a = self.app()
        calls = []
        real = a._provider.history
        a._provider.history = lambda sym, start, end=None, interval="1d": calls.append(sym) or real(sym, start)
        a._refresh("start", "stale")
        self.assertEqual(calls, [], "everything was fetched just now, and ^SP500TR (no sample) already failed today")
        self.assertIn("errAt", a._store.entry("^SP500TR"))
        hist = next(f for f in a._features if f.name == "history")
        a._store.entry("^IXIC")["fetched"] = "2000-01-01T00:00:00Z"      # stale
        a._store.entry("OLDSYM")["fetched"] = "2000-01-01T00:00:00Z"     # no longer watched
        self.assertEqual(hist.stale_symbols(), ["^IXIC"])
        a._refresh("start", "stale")
        self.assertEqual(calls, ["^IXIC"])
        calls.clear()
        self.config(a, {"v": 2, "indices": ["^GSPC", "^IXIC", "^FCHI", "^GDAXI", "AAPL"]})
        [t for t in a.timers if t[1] == 5][-1][0]()
        settle(a)
        self.assertEqual(calls, ["AAPL"], "a config change fetches the new symbol alone")

    def test_stale_follows_the_local_daily_time(self):
        ctx = Context(config.from_defaults(), Store(tempfile.mkdtemp()), fixtures.FixtureProvider(),
                      publisher.Publisher(publisher.DryRunClient(), publisher.Topics("t", "a1b2c3")), lambda *a, **k: None)
        from zoneinfo import ZoneInfo
        ctx.tz = ZoneInfo("Europe/Paris")
        h = history.HistoryFeature(ctx)
        ctx.store.save("^GSPC", M.load_yahoo_chart(SAMPLES / "yahoo_GSPC_max_1mo.json"), fetched="2026-09-15T04:30:00Z")   # 06:30 Paris
        at = lambda y, m, d, hh, mi: dt.datetime(y, m, d, hh, mi, tzinfo=UTC)
        self.assertNotIn("^GSPC", h.stale_symbols(at(2026, 9, 15, 4, 50)), "06:50 Paris: today's 07:00 has not come")
        self.assertIn("^GSPC", h.stale_symbols(at(2026, 9, 15, 5, 10)), "07:10 Paris: fetched before today's 07:00")

    def test_the_keepalive_is_status_alone(self):
        a = self.app()
        n = len(a._mq.published)
        a._ctx.data["keepalive"]()
        settle(a)
        self.assertEqual([t for t, *_ in a._mq.published[n:]], [ROOT + "status"])
        self.assertEqual(last(a, "status")["gen"], a._publisher._gen)


class OracleTests(unittest.TestCase):
    """The app's TWR, ANN and XIRR against tools/market/preview_returns.py, the independent oracle of the previews,
    on its contributions case: VOO from the samples, 10 000 EUR from 2000-01-01 plus 1 000 a year. Equal for every
    window and both modes: to 12 places unrounded, to the payload's 6 decimals as published. The MAX headline is the
    previews' TWR +764.59 %, ANN +8.41 %, XIRR +9.79 % (frames.json: TWR +765%, ANN +8.4%, XIRR +9.8%)."""

    def test_equal_to_the_oracle(self):
        import preview_returns as R
        P = M.run_preview_portfolio()
        S, cfg = P["samples"], P["cfg"]
        c = M.Config(cfg.capital, cfg.currency, cfg.inception, cfg.positions, 1000.0, "year")
        res = ledger.compute(c, S["series"], S["fx"], "^GSPC")
        self.assertEqual(res.days, P["days"])
        for mode in ("hold", "rebal"):
            L = M.simulate(c, S["series"], S["fx"], P["days"], mode, True, M.calendar_years)
            self.assertEqual(res.ledger(mode).v, L.v)
            for preset in M.PRESETS:
                w = R.window_figures(c, L, preset)
                start, i0, i1, wd, wv = ledger._window(res, mode, preset)
                rb = ledger.returns_block(res, mode, i0, i1, wd, preset)
                self.assertAlmostEqual(rb["twr"], w["twr"], places=12, msg=(mode, preset))
                self.assertEqual(rb["ann"] is None, w["ann"] is None, (mode, preset))
                if w["ann"] is not None:
                    self.assertAlmostEqual(rb["ann"], w["ann"], places=12, msg=(mode, preset))
                self.assertAlmostEqual(rb["xirr_ann"], w["xirr"], places=9, msg=(mode, preset))
                p = ledger.portfolio_payload(res, mode, preset)
                self.assertAlmostEqual(p["twr"], w["twr"], places=6)
                self.assertAlmostEqual(p["xirr_ann"], w["xirr"], places=6)
        p = ledger.portfolio_payload(res, "hold", "MAX")
        self.assertEqual((round(p["twr"] * 100, 2), round(p["ann"] * 100, 2), round(p["xirr_ann"] * 100, 2)), (764.59, 8.41, 9.79))


class StatsTests(unittest.TestCase):
    def test_hand_cases(self):
        m = [0.01, -0.02, 0.03]
        mean = sum(m) / 3
        sd = ((sum((x - mean) ** 2 for x in m)) / 2) ** 0.5
        self.assertAlmostEqual(stats.vol_ann(m), sd * 12 ** 0.5)
        self.assertAlmostEqual(stats.sharpe(m), mean / sd * 12 ** 0.5)
        self.assertAlmostEqual(stats.sortino(m), mean / (0.02 ** 2 / 3) ** 0.5 * 12 ** 0.5)
        self.assertAlmostEqual(stats.sharpe(m, rf_pct=1.2), (mean - 0.001) / sd * 12 ** 0.5, places=6)
        self.assertAlmostEqual(stats.calmar(0.08, -0.2), 0.4)
        self.assertIsNone(stats.vol_ann([0.01]))
        days = [D(2024, 1, 1), D(2024, 2, 1), D(2024, 3, 1), D(2024, 4, 1), D(2024, 5, 1), D(2024, 6, 1)]
        vals = [100, 110, 99, 105, 112, 100]
        eps = stats.drawdown_episodes(days, vals)
        self.assertEqual([(e["peak"], e["trough"], e["recovery"], round(e["depth"], 4)) for e in eps],
                         [(D(2024, 5, 1), D(2024, 6, 1), None, -0.1071), (D(2024, 2, 1), D(2024, 3, 1), D(2024, 5, 1), -0.1)])
        self.assertEqual(stats.max_drawdown(days, vals), {"mdd": -0.1071428571428571, "peak": "2024-05-01", "trough": "2024-06-01", "recovery": None})
        # the window's first balance, then the month ends (audit MINOR 12: the first partial month counts)
        self.assertEqual(stats.month_ends([D(2024, 1, 2), D(2024, 1, 31), D(2024, 2, 1)], [1, 2, 3]),
                         ([D(2024, 1, 2), D(2024, 1, 31), D(2024, 2, 1)], [1, 2, 3]))
        self.assertEqual(stats.month_ends([D(2024, 1, 31), D(2024, 2, 1), D(2024, 2, 29)], [1, 2, 3]),
                         ([D(2024, 1, 31), D(2024, 2, 29)], [1, 3]), "a first day that is a month end is not doubled")
        self.assertAlmostEqual(stats.max_drawdown([D(2024, 1, 10), D(2024, 1, 31)], [100.0, 90.0], "month_end")["mdd"], -0.1)
        yrs = stats.calendar_year_returns([D(2023, 1, 3), D(2023, 12, 29), D(2024, 6, 3), D(2024, 12, 31), D(2025, 3, 3)], [100, 110, 120, 99, 105])
        self.assertEqual({y: round(r, 4) for y, r in yrs.items()}, {2023: 0.1, 2024: -0.1, 2025: 0.0606})
        best, worst = stats.best_worst_year([D(2023, 1, 3), D(2023, 12, 29), D(2024, 12, 31)], [100, 110, 99])
        self.assertEqual((best["y"], worst["y"]), (2023, 2024))
        r = stats.rolling([D(2023, 1, 3), D(2023, 12, 29), D(2024, 1, 3), D(2024, 6, 3), D(2024, 12, 31)], [100, 110, 111, 120, 99], 1)
        self.assertEqual((r["n"], round(r["max"], 4), round(r["last"], 4)), (3, 0.2, -0.1))
        b = stats.block(days, [float(v) for v in vals], None)
        self.assertEqual(set(b), {"mdd", "mddME", "ddNow", "vol", "sharpe", "sortino", "calmar", "topDD", "bestY", "worstY", "roll1y", "roll3y"})


class V2PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.S = M.load_samples()
        cls.cfg = M.preview_config()
        cls.fx = ledger.fx_table(cls.S["series"], ecb.EcbRates.from_sample().rate)

    def test_stats_payload(self):
        res = ledger.compute(self.cfg, self.S["series"], self.fx, "^GSPC", extra_bench=["^SP500TR", "^IXIC"])
        sp = ledger.stats_payload(res, "hold", "MAX")
        self.assertEqual([b["sym"] for b in sp["bench"]], ["^GSPC", "^IXIC"], "^SP500TR has no sample: left out, not invented")
        self.assertAlmostEqual(sp["twr"], 7.98, places=1)
        self.assertEqual((sp["flows"], sp["xirr_ann"]), (False, None), "no flows: no XIRR (owner, 11:43)")
        self.assertEqual(res.bench[0], 10000.0, "the benchmark starts at C, in EUR")
        own = M.benchmark(self.S["series"]["^GSPC"], res.days, 10000.0, self.cfg.inception)
        self.assertNotEqual(round(res.bench[-1]), round(own[-1]), "converted into the portfolio currency, ECB rates before 2003-12")
        self.assertAlmostEqual(res.bench[-1] / own[-1], self.fx.usd_per_eur(res.days[0]) / self.fx.usd_per_eur(res.days[-1]), places=6)
        cfgc = M.Config(10000.0, "EUR", self.cfg.inception, self.cfg.positions, 100.0, "year")
        withf = ledger.stats_payload(ledger.compute(cfgc, self.S["series"], self.fx, "^GSPC"), "hold", "MAX")
        self.assertTrue(withf["flows"])
        self.assertIsNotNone(withf["xirr_ann"])
        self.assertLess(sp["fx_effect"], 0, "a EUR portfolio of a USD fund since 2000: the dollar's fall costs return")
        self.assertEqual(sp["mdd"]["peak"], "2020-01-01")
        self.assertEqual(sp["contrib"][0][0], "VOO")
        self.assertEqual(sp["outOfBand"], [])
        self.assertAlmostEqual(sp["classes"]["cash"], 0.55, places=2)
        self.assertFalse(sp["real"])
        self.assertLessEqual(len(publisher.encode(sp)), publisher.PAYLOAD_MAX)
        h = ledger.holdings_payload(res, "hold")
        self.assertEqual((h["rebal"], h["classes"]), (0, {"cash": 0.55}))
        self.assertNotIn("out", h["rows"][0])
        self.assertEqual((h["rows"][0]["lo"], h["rows"][0]["hi"]), (95.0, 105.0))

    def test_real_terms_and_blend(self):
        hicp = ecb.HicpIndex({"2000-01": 50.0, "2026-09": 100.0})
        self.assertEqual(hicp.factor(D(2000, 1, 1), D(2026, 9, 14)), 0.5)
        self.assertEqual(hicp.level(D(2010, 6, 1)), 50.0, "carried forward")
        res = ledger.compute(self.cfg, self.S["series"], self.fx, "^GSPC", real=lambda d: hicp.factor(D(2000, 1, 1), d))
        p = ledger.portfolio_payload(res, "hold", "MAX")
        self.assertTrue(p["real"])
        self.assertAlmostEqual(p["value"], res.ledger("hold").v[-1] * 0.5)
        legs = [{"sym": "VOO", "w": 60.0}, {"sym": "^IXIC", "w": 40.0}]
        res = ledger.compute(self.cfg, self.S["series"], self.fx, "^GSPC", blend=legs)
        self.assertEqual(res.bench_sym, "blend")
        self.assertAlmostEqual(res.bench[0], 10000.0, places=6)     # every line starts at the capital
        self.assertGreater(res.bench[-1], 10000.0)
        self.assertIn("^GSPC", res.benches)

    def test_proxy_and_adj(self):
        fund, proxy = self.S["series"]["VOO"], self.S["series"]["^GSPC"]
        sp = ledger.splice_proxy(fund, proxy)
        self.assertEqual(sp.first, proxy.first)
        self.assertEqual(sp.close_on(fund.first), fund.closes[0])
        self.assertEqual(sp.closes[-1], fund.closes[-1])
        self.assertEqual(sp.meta["proxy"]["until"], fund.first.isoformat())
        self.assertEqual(len(sp.dividends), len(fund.dividends))
        st = Store(tempfile.mkdtemp())
        st.save("VOO", fund)
        back = st.load("VOO")
        self.assertEqual(back.adj, fund.adj)
        self.assertTrue(st.entry("VOO")["adj"])
        more = M.Series("VOO", "USD", fund.dates[-3:] + [D(2026, 10, 1)], fund.closes[-3:] + [700.0], [], None, fund.meta, fund.adj[-3:] + [700.0])
        st.save("VOO", more)
        self.assertEqual(len(st.load("VOO").adj), len(fund.dates) + 1)

    def test_hicp_fixture(self):
        text = (SAMPLES / "ecb_ICP_M_U2_N_000000_4_INX_1999_2000.csv").read_text("utf-8")
        table = ecb.parse_csvdata(text, ecb.HICP_SERIES)
        self.assertEqual(len(table), 15)
        self.assertEqual(table["1999-01"], 73.76)
        self.assertEqual(ecb.parse_csvdata(text, M.ECB_SERIES), {}, "no EUR/USD rows in the HICP answer")
        idx = ecb.HicpIndex(table, "2026-09-15T00:00:00Z")
        st = Store(tempfile.mkdtemp())
        idx.save(st)
        self.assertEqual(ecb.HicpIndex.from_store(st).table, table)
        self.assertEqual(ecb.ensure_hicp(st, None).table, table)

    def test_feed_labels(self):
        self.assertEqual(live.feed_of("OPEN", 30, 1200), "LIVE")
        self.assertEqual(live.feed_of("OPEN", 900, 1200), "DELAYED")
        self.assertEqual(live.feed_of("OPEN", 1300, 1200, moved=False), "STALE")
        self.assertEqual(live.feed_of("STALE", 30, 1200), "STALE")
        self.assertEqual(live.feed_of("CLOSED", 30, 1200), "CLOSED")
        self.assertEqual(live.feed_of(None, None, 1200), "CLOSED")
        q = {"VOO": base.Quote("VOO", 699.3, 700.0, 1789416000, "USD")}
        row = live.live_payload(q, {"VOO": "OPEN"}, 1789416000 + 905, 1200)["q"]["VOO"]
        self.assertEqual((row["delay_s"], row["feed"]), (905, "DELAYED"))
        row = live.live_payload(q, {"VOO": "CLOSED"}, 1789416000 + 3 * 86400, 1200)["q"]["VOO"]
        self.assertEqual((row["delay_s"], row["feed"]), (65535, "CLOSED"), "a weekend-old quote: the panel's u32 cap")
        row = live.live_payload(q, {"VOO": "OPEN"}, 1789416000 + 1300, 1200, {"VOO": 1789416000})["q"]["VOO"]
        self.assertEqual(row["feed"], "STALE")

    def test_alert_entities(self):
        s = config.from_panel(config.from_defaults(), {"v": 2, "alert": {"day_move_pct": 3, "drawdown_pct": 20, "out_of_band": True},
                                                       "portfolio": {"positions": [{"sym": "VOO", "w": 100}]}})
        topics = publisher.Topics("nickoscope_matrix", "a1b2c3")
        sens = {oid: (c, cfg) for c, oid, cfg in ha_entities.sensors(s, topics, "a1b2c3")}
        self.assertEqual({sens[k][0] for k in ("alert_day_move", "alert_drawdown", "alert_out_of_band")}, {"binary_sensor"})
        self.assertIn("0.03", sens["alert_day_move"][1]["value_template"])
        for oid in ("portfolio_xirr", "portfolio_vol", "portfolio_sharpe", "benchmark_1", "benchmark_3", "class_gold", "portfolio_mdd_dates"):
            self.assertIn(oid, sens)
        self.assertEqual(sens["portfolio_xirr"][1]["state_topic"], "nickoscope_matrix/a1b2c3/market_stats/hold/MAX")
        off = {oid for c, oid, cfg in ha_entities.sensors(config.from_defaults(), topics, "a1b2c3")}
        self.assertFalse(any(o.startswith("alert_") for o in off), "alerts are off by default")


# ── the panel's contract (wip/market-panel, src/market/README.md and market_settings.cpp) ──
PANEL_DEFAULT_CONFIG = {
    "v": 2,
    "indices": [{"sym": "^GSPC", "name": "SPX"}, {"sym": "^IXIC", "name": "NDX"}, {"sym": "^FCHI", "name": "CAC"}, {"sym": "^GDAXI", "name": "DAX"}],
    "tickers": [{"sym": "VFINX", "name": "VFINX"}, {"sym": "VBMFX", "name": "VBMFX"}, {"sym": "AAPL", "name": "AAPL"}, {"sym": "IWDA.AS", "name": "IWDA"}],
    "portfolio": {"capital": 10000, "currency": "EUR", "inception": "2000-01-01",
                  "positions": [{"sym": "SYN%02dXXX" % k, "w": 6.25, "asset_class": "equity"} for k in range(16)],   # synthetic weights
                  "contrib": {"amount": 0, "every": "year"}, "rebalance": False},
    "rebal": {"mode": "hold", "calendar": "annual", "bands": {"abs_pts": 5, "rel_pct": 25, "check": "monthly"}, "contrib_only": False},
    "ticker": "VOO", "tape": {"exchanges": ["NYSE", "NASDAQ", "LSE", "XETRA", "EURONEXT", "TOKYO"]},
    "returns": {"measure": "twr", "real": False}, "dividends": {"mode": "sweep_yearend"},
    "contrib": {"index_inflation": False}, "withdraw": {"amount": 0, "kind": "amount", "every": "year"}, "cash": {"yield_pct": 0},
    "ter": {}, "tax": {"div_withholding_pct": 0}, "costs": {"per_trade_fixed": 0, "per_trade_pct": 0},
    "display": {"fx_effect": False, "scale": "linear"},
    "bench": {"mode": "single", "symbol": "^SP500TR", "blend": [], "telltale": False},
    "mdd_basis": "daily", "stats": {"risk_free_pct": 0},
    "live": {"poll_s": 60}, "fetch": {"daily_at": "07:00"}, "stale": {"session_min": 20, "history_days": 3},
    "alert": {"day_move_pct": 0, "drawdown_pct": 0, "out_of_band": False},
    "presets": ["YTD", "1Y", "3Y", "5Y", "10Y", "MAX"],
}


class PanelContractTests(unittest.TestCase):
    def test_default_config_is_understood_whole(self):
        s = config.from_panel(config.from_defaults(), json.loads(json.dumps(PANEL_DEFAULT_CONFIG)))
        self.assertEqual(s.errors, [])
        self.assertEqual(s.unknown, {}, "every key the panel sends is a setting")
        self.assertEqual(s.mode(), "hold")
        self.assertEqual(s.rules(), M.Rules(), "the panel's defaults are simulate()'s rules")
        self.assertEqual(s.presets(), list(M.PRESETS))
        self.assertEqual(s["ticker"], "VOO")
        self.assertEqual([t["sym"] for t in s.tickers()], ["VFINX", "VBMFX", "AAPL", "IWDA.AS"])
        self.assertEqual(s.watched_symbols()[-2:], ["^SP500TR", "EURUSD=X"])
        self.assertEqual(s.status_symbols()[:16], ["SYN%02dXXX" % k for k in range(16)], "positions first: the panel keeps 16 status symbols")
        self.assertLess(len(publisher.encode(PANEL_DEFAULT_CONFIG)), publisher.CONFIG_MAX, "the panel's ~1.9 KB payload passes the app's config cap")

    def test_panel_choices(self):
        base_s = config.from_defaults()
        s = config.from_panel(base_s, {"v": 2, "rebal": {"mode": "calendar"}, "portfolio": {"rebalance": True}})
        self.assertEqual((s.mode(), s.rules().rebal), ("rebal", "calendar"))
        s = config.from_panel(base_s, {"v": 2, "rebal": {"mode": "bands"}, "withdraw": {"amount": 2, "kind": "pct", "every": "quarter"},
                                       "portfolio": {"contrib": {"amount": 100, "every": "quarter"}},
                                       "presets": ["YTD", "1Y", "3Y", "5Y", "10Y", "MAX", "WTD", "3M"]})
        self.assertEqual((s.mode(), s.rules().rebal, s.rules().withdraw_pct, s.rules().withdraw_amount, s.rules().withdraw_every),
                         ("rebal", "bands", 2.0, 0.0, "quarter"))
        self.assertEqual(s.presets(), ["WTD", "3M", "YTD", "1Y", "3Y", "5Y", "10Y", "MAX"], "the panel's order")
        self.assertEqual(s.ref_config().contrib_every, "quarter")
        s = config.from_panel(base_s, {"v": 2, "bench": {"mode": "blend", "blend": []}})
        self.assertEqual([l["sym"] for l in s.blend_legs()], ["VFINX", "VBMFX"], "[] means the portfolio's own weights")
        s = config.from_panel(base_s, {"v": 2, "bench": {"mode": "blend", "blend": "60_40"}})
        self.assertEqual(s.blend_legs(), [{"sym": "VFINX", "w": 60.0}, {"sym": "VBMFX", "w": 40.0}])
        self.assertEqual(s.watched_symbols()[-2:], ["^SP500TR", "EURUSD=X"])
        self.assertNotIn("^SP500TR", s.live_symbols(), "benchmarks are never polled live")

    def test_quarterly_periods(self):
        cfg = M.Config(1000.0, "EUR", D(2023, 1, 2), [], 10.0, "quarter")
        self.assertEqual(M.contribution_dates(cfg, D(2024, 1, 1)), [D(2023, 4, 2), D(2023, 7, 2), D(2023, 10, 2)])
        self.assertEqual(M.withdrawal_dates(cfg, M.Rules(withdraw_amount=1.0, withdraw_every="quarter"), D(2023, 8, 1)), [D(2023, 4, 2), D(2023, 7, 2)])

    def test_mutual_funds_are_not_polled(self):
        tmp = tempfile.mkdtemp()
        st, ctx, h = fixture_store(tmp)
        h.load()
        vfinx = M.Series("VFINX", "USD", [D(2026, 9, 14)], [500.0], [], None, {"instrumentType": "MUTUALFUND", "exchangeName": "NAS"})
        ctx.data["series"]["VFINX"] = vfinx
        ctx.settings = config.from_panel(ctx.settings, {"v": 2, "tickers": ["VFINX", "VOO"]})
        f = live.LiveFeature(ctx)
        polled = []
        ctx.provider.quotes = lambda syms: polled.append(list(syms)) or {}
        ctx.data["tape_open"] = lambda: ["NYSE"]
        f.tick()
        self.assertEqual(len(polled), 1)
        self.assertNotIn("VFINX", polled[0])
        self.assertIn("VOO", polled[0])


# ── the app with the fakes ───────────────────────────────────────────────────
def app_args(**over):
    args = {"device": "a1b2c3", "mqtt_host": "192.0.2.10", "mqtt_user": "u", "mqtt_pass": SECRET, "provider": "fixtures",
            "store_dir": tempfile.mkdtemp(), "features": ["history", "portfolio", "live", "tape", "ha_entities"], "local_file": "",
            "portfolio": {"positions": [{"sym": "VOO", "w": 100.0, "asset_class": "equity"}]}, "display": {"tz": "Europe/Paris"}}
    args.update(over)
    return args


ROOT = "nickoscope_matrix/a1b2c3/market/"


def last(app, leaf):
    want = app._topics.leaf(leaf)
    for topic, payload, qos, retain in reversed(app._mq.published):
        if topic == want:
            return json.loads(payload) if payload else None
    return None


def settle(app):
    app._worker.q.join()


class AppTests(unittest.TestCase):
    def setUp(self):
        self.app = MatrixMarket(app_args())
        self.app.initialize()
        settle(self.app)

    def tearDown(self):
        self.app.terminate()

    def test_wiring(self):
        a = self.app
        self.assertEqual(a._mq.will, (ROOT + "ha", "offline", 1, True))
        self.assertEqual(a._mq.login, ("u", SECRET))
        self.assertEqual([f.name for f in a._features], ["history", "portfolio", "live", "tape", "ha_entities"])
        self.assertEqual([d[1] for d in a.daily], ["07:00:00"])
        self.assertEqual(sorted(e[2] for e in a.every), [60, 60, 6 * 3600])          # live, tape, keepalive
        self.assertEqual(a._mq.subscribed, [[(ROOT + "config", 1), ("homeassistant/status", 1)]])
        self.assertIn((ROOT + "ha", "online", 1, True), a._mq.published)
        self.assertEqual(a._mq.dropped, [], "nothing published before the broker answered")

    def test_first_generation(self):
        a = self.app
        st = last(a, "status")
        self.assertEqual((st["state"], st["asof"], st["v"]), ("ok", "2026-09-14", 1))
        self.assertIn("gen", st)
        self.assertEqual(st["symbols"]["VOO"]["bars"], 193)
        self.assertAlmostEqual(st["symbols"]["VOO"]["ter"], 0.0003, places=6)
        p = last(a, "portfolio/hold/MAX")
        self.assertEqual(p["gen"], st["gen"])
        self.assertAlmostEqual(p["value"], 89678.85, places=2)                     # VOO alone on the fixtures' timeline
        self.assertEqual(set(k for k in p if k in ("pts", "px", "bench", "gross")), {"pts", "px", "bench"})
        self.assertEqual(last(a, "holdings/hold")["rows"][0]["sym"], "VOO")
        self.assertEqual(last(a, "index/^GSPC/1Y")["name"], "SPX")
        leaves = {t[len(ROOT):] for t, *_ in a._mq.published if t.startswith(ROOT)}
        for leaf in ("index/^GDAXI/YTD", "portfolio/rebal/10Y", "holdings/rebal", "tape", "status"):
            self.assertIn(leaf, leaves)
        self.assertNotIn("live", leaves, "nothing polled outside a session on the fixtures' clock")
        for topic, payload, qos, retain in a._mq.published:
            if isinstance(payload, bytes):
                self.assertLessEqual(len(payload), publisher.PAYLOAD_MAX, topic)
                self.assertTrue(retain, topic)
        sp = last(a, "stats/hold/MAX")
        self.assertEqual((sp["gen"], sp["real"], sp["bench"][0]["sym"]), (st["gen"], False, "^GSPC"), "^SP500TR has no history: the first index stands in")
        self.assertIn("sharpe", sp)
        self.assertEqual(last(a, "holdings/hold")["rows"][0]["cls"], "equity")
        self.assertEqual(p["measure"], "twr")
        self.assertAlmostEqual(p["twr"], p["chg"], places=6)
        self.assertNotIn("realErr", st)
        self.assertTrue(any(t.startswith("homeassistant/sensor/matrix_market_a1b2c3/") for t, *_ in a._mq.published))
        self.assertTrue(pathlib.Path(a._store.root, "config_last.json").exists())

    def config(self, a, payload):
        a._on_message(a._mq, None, types.SimpleNamespace(topic=ROOT + "config", payload=json.dumps(payload).encode() if not isinstance(payload, bytes) else payload))
        settle(a)

    def fire_debounce(self, a):
        pending = [t for t in a.timers if t[1] == 5]
        self.assertTrue(pending, "a maths change is debounced by 5 s")
        pending[-1][0]()
        settle(a)
        a.timers = [t for t in a.timers if t[1] != 5]

    def test_config_from_the_panel(self):
        a = self.app
        gen0 = last(a, "status")["gen"]
        self.config(a, {"v": 2, "rebal": {"mode": "calendar"}, "indices": ["^GSPC", "^GDAXI"]})
        self.assertEqual(last(a, "status")["gen"], gen0, "nothing republished before the debounce")
        self.fire_debounce(a)
        st = last(a, "status")
        self.assertGreater(st["gen"], gen0)
        self.assertNotIn("cfgErr", st)
        self.assertEqual(a._settings.mode(), "rebal")
        cleared = [t for t, p, *_ in a._mq.published if p == b"" and t.startswith(ROOT + "index/")]
        self.assertTrue(any("^IXIC" in t for t in cleared) and any("^FCHI" in t for t in cleared), "the dropped indices are cleared")
        disc = [t for t, p, *_ in a._mq.published if "portfolio_value/config" in t]
        self.assertIn(b"/portfolio/rebal/MAX", [p for t, p, *_ in a._mq.published if "portfolio_value/config" in t][-1])
        self.config(a, b'{"v":1,"portfolio":{"capital":"much"},"zzz":1}')
        self.fire_debounce(a)
        st = last(a, "status")
        self.assertEqual(st["cfgErr"], [{"key": "portfolio.capital", "code": "TYPE"}])
        self.assertEqual(a._settings.mode(), "hold", "the bad payload's rebal.mode is absent: back to apps.yaml's")
        self.assertEqual(a._settings.unknown, {"zzz": 1})
        self.config(a, b"not json")
        self.assertEqual(last(a, "status")["cfgErr"][0]["code"], "TYPE")
        self.config(a, b"x" * 9000)

    def test_config_diff_branches(self):
        """The panel's audit rule: identical -> nothing; ticker -> the intraday switch; schedule -> timers only;
        maths -> one debounced recompute; a symbol leaving -> its topics cleared."""
        a = self.app
        n = len(a._mq.published)
        gen0 = last(a, "status")["gen"]
        self.config(a, {"v": 2})
        self.config(a, {"v": 2})                                      # identical, twice (a reconnect)
        self.assertEqual(len(a._mq.published), n, "an identical config publishes nothing")
        # ticker only
        self.config(a, {"v": 2, "ticker": "^GSPC"})
        self.assertEqual(len(a._mq.published), n, "a first ticker: nothing to clear, nothing republished")
        self.config(a, {"v": 2, "ticker": "VOO"})
        self.assertEqual(a._mq.published[n:], [(ROOT + "intraday/^GSPC", b"", 0, True)], "the old symbol's intraday line is cleared")
        self.assertEqual(last(a, "status")["gen"], gen0)
        self.assertEqual(a._settings["ticker"], "VOO")
        n = len(a._mq.published)
        # schedule only
        timers_before = len(a.every)
        self.config(a, {"v": 2, "ticker": "VOO", "live": {"poll_s": 120}, "stale": {"session_min": 30}})
        self.assertEqual(len(a._mq.published), n, "a schedule change publishes nothing")
        self.assertGreater(len(a.every), timers_before, "the timers were made again")
        self.assertGreaterEqual(getattr(a, "cancelled", 0), 2, "and the old ones cancelled")
        self.assertEqual(a.every[-1][2] if a.every[-1][2] != 60 else a.every[-2][2], 120)
        self.assertFalse([t for t in a.timers if t[1] == 5], "no debounce for a schedule change")
        # maths: a burst of saves, one run
        self.config(a, {"v": 2, "ticker": "VOO", "live": {"poll_s": 120}, "stale": {"session_min": 30}, "portfolio": {"capital": 20000}})
        self.config(a, {"v": 2, "ticker": "VOO", "live": {"poll_s": 120}, "stale": {"session_min": 30}, "portfolio": {"capital": 30000}})
        self.assertEqual(last(a, "status")["gen"], gen0, "nothing yet: debounced")
        self.assertGreaterEqual(getattr(a, "cancelled", 0), 3, "the second save cancelled the first debounce")
        self.fire_debounce(a)
        st = last(a, "status")
        self.assertGreater(st["gen"], gen0)
        self.assertAlmostEqual(last(a, "portfolio/hold/MAX")["sinceStart"], last(a, "portfolio/hold/MAX")["value"] / 30000 - 1, places=6)
        # a symbol leaves the watch lists: its retained topics are cleared at once
        n = len(a._mq.published)
        self.config(a, {"v": 2, "ticker": "VOO", "live": {"poll_s": 120}, "stale": {"session_min": 30}, "portfolio": {"capital": 30000},
                        "indices": ["^GSPC", "^IXIC", "^FCHI"]})
        cleared = {t for t, p, q, r in a._mq.published[n:] if p == b""}
        self.assertIn(ROOT + "index/^GDAXI/MAX", cleared)
        self.assertIn(ROOT + "intraday/^GDAXI", cleared)
        self.fire_debounce(a)
        self.assertNotIn("index/^GDAXI/MAX", a._publisher.generation_leaves, "the new generation no longer carries the symbol")
        self.assertIn("index/^FCHI/MAX", a._publisher.generation_leaves)

    def test_subset_of_features(self):
        a = MatrixMarket(app_args(features=["history"]))
        a.initialize()
        settle(a)
        leaves = {t[len(ROOT):] for t, *_ in a._mq.published if t.startswith(ROOT)}
        self.assertIn("index/^GSPC/MAX", leaves)
        self.assertFalse(any(l.startswith("portfolio/") for l in leaves))
        self.assertEqual(last(a, "status")["state"], "ok")
        a.terminate()
        self.assertEqual(a._mq.published[-1][:2], (ROOT + "ha", "offline"))

    def test_missing_fund_is_named(self):
        a = MatrixMarket(app_args(portfolio={"positions": [{"sym": "VOO", "w": 50}, {"sym": "ZZZ", "w": 50}]}))
        a.initialize()
        settle(a)
        st = last(a, "status")
        self.assertEqual(st["pfErr"], "no history for ZZZ")
        self.assertIn("err", st["symbols"]["ZZZ"])
        self.assertEqual(st["state"], "ok")
        self.assertIsNone(last(a, "portfolio/hold/MAX"))
        a.terminate()

    def test_local_override_in_the_app(self):
        store_dir = tempfile.mkdtemp()
        pathlib.Path(store_dir, "local.json").write_text(json.dumps({"portfolio": {"positions": [{"sym": "VOO", "w": 100}]}, "ticker": "VOO"}))
        args = app_args(portfolio={"positions": [{"sym": "ZZZ", "w": 100}]}, store_dir=store_dir)
        args.pop("local_file")                          # the default: <store_dir>/local.json
        a = MatrixMarket(args)
        a.initialize()
        settle(a)
        self.assertEqual(([p["sym"] for p in a._settings.positions()], a._settings["ticker"]), (["VOO"], "VOO"))
        self.assertTrue(any("local override read from local.json" in m for _, m in a.logs))
        self.assertIsNotNone(last(a, "portfolio/hold/MAX"))
        a.terminate()

    def test_no_secret_in_the_log(self):
        self.assertTrue(self.app.logs)
        self.assertFalse(any(SECRET in m for _, m in self.app.logs))
        self.assertFalse(any("crumb=" in m for _, m in self.app.logs))


# ── the CLI, offline ─────────────────────────────────────────────────────────
class CliTests(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = cli.main(list(argv))
        return code, out.getvalue()

    def test_offline_round(self):
        tmp = tempfile.mkdtemp()
        cfg = pathlib.Path(tmp, "voo.json")
        cfg.write_text(json.dumps(voo_only()))
        common = ["--offline", "--no-local", "--store", tmp, "--config", str(cfg)]
        code, out = self.run_cli(*common, "fetch")
        self.assertEqual(code, 0)
        self.assertIn("VOO       new", out)
        code, out = self.run_cli(*common, "compute")
        self.assertEqual(code, 0, out)
        self.assertIn("largest", out)
        code, out = self.run_cli(*common, "report")
        self.assertEqual(code, 0, out)
        self.assertIn("HOLD  entries: VOO 2010-12-01", out)
        self.assertIn("89678.85", out)
        code, out = self.run_cli(*common, "publish", "--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("nickoscope_matrix/a1b2c3/market/portfolio/hold/MAX", out)
        code, out = self.run_cli("schema")
        self.assertEqual(code, 0)
        self.assertIn("`portfolio.capital`", out)


class RestartTests(unittest.TestCase):
    """Final audit MAJOR: after a restart the first generation uses the panel's last config, not apps.yaml."""

    PANEL = {"v": 2, "tickers": [{"sym": "AAPL", "name": "AAPL"}], "ticker": "AAPL",
             "portfolio": {"positions": [{"sym": "IWDA.AS", "w": 100.0, "asset_class": "equity"}]}}

    def message(self, a, payload):
        a._on_message(a._mq, None, types.SimpleNamespace(topic=ROOT + "config", payload=payload))
        settle(a)

    def fire_debounce(self, a):
        for cb, delay in list(a.timers):
            if getattr(cb, "__name__", "") == "_debounced":
                a.timers.remove((cb, delay))
                cb()
        settle(a)

    def test_restart_starts_from_the_panel_config(self):
        store = tempfile.mkdtemp()
        raw = json.dumps(self.PANEL, separators=(",", ":")).encode()
        first = MatrixMarket(app_args(store_dir=store))
        first.initialize()
        settle(first)
        self.message(first, raw)
        self.fire_debounce(first)
        first.terminate()

        again = MatrixMarket(app_args(store_dir=store))
        again.initialize()
        settle(again)
        self.addCleanup(again.terminate)
        pub = [(t[len(ROOT):], p) for t, p, q, r in again._mq.published if t.startswith(ROOT)]
        holdings = [json.loads(p) for leaf, p in pub if leaf == "holdings/hold" and p]
        self.assertTrue(holdings, "a first generation was published")
        self.assertEqual({r["sym"] for r in holdings[0]["rows"] if r.get("sym") != "CASH"}, {"IWDA.AS"},
                         "the panel's allocation, not apps.yaml's")
        cleared = [leaf for leaf, p in pub if not p and leaf.startswith("ticker/AAPL/")]
        self.assertEqual(cleared, [], "the panel's ticker is not cleared")
        n = len(again._mq.published)
        self.message(again, raw)
        self.assertFalse([t for t in again.timers if getattr(t[0], "__name__", "") == "_debounced"],
                         "the retained config is identical: no recompute")
        self.assertEqual(len(again._mq.published), n)


class AppDaemonArgsTests(unittest.TestCase):
    """HA 2026-09-15: AppDaemon 4.5 puts its own AppConfig fields into self.args, config_path as a pathlib.Path
    (appdaemon/models/config/app.py, 4.5.13). They must not reach the settings, config_last or any payload."""

    def test_appdaemon_fields_are_dropped(self):
        store = tempfile.mkdtemp()
        args = app_args(store_dir=store)
        args.update({"name": "matrix_market", "config_path": pathlib.Path("/config/apps/apps.yaml"),
                     "module": "matrix_market.app", "class": "MatrixMarket", "priority": 50.0,
                     "dependencies": set(), "disable": False, "pin_app": True})
        a = MatrixMarket(args)
        a.initialize()
        settle(a)
        self.addCleanup(a.terminate)
        a._refresh("probe", False)                     # raised TypeError on a PosixPath before the fix
        last = a._store.load_table("config_last")
        self.assertIsNotNone(last)
        json.dumps(last)                                # every value is JSON-native
        for key in ("config_path", "name", "module", "class", "priority", "dependencies", "disable", "pin_app"):
            self.assertNotIn(key, last, key)

    def test_a_failing_job_logs_where(self):
        a = MatrixMarket(app_args())
        a.initialize()
        settle(a)
        self.addCleanup(a.terminate)
        def boom():
            raise ValueError("probe")
        a._worker.put(boom)
        settle(a)
        lines = [msg for level, msg in a.logs if "ValueError in boom" in msg]
        self.assertTrue(lines, "the failure is logged")
        self.assertIn("test_matrix_market.py:", lines[-1], "with the file and line it came from")


if __name__ == "__main__":
    unittest.main(verbosity=1)

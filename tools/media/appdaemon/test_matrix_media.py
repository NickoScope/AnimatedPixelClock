#!/usr/bin/env python3
"""matrix_media.py against a fake Home Assistant and a fake broker.

AppDaemon and paho are replaced by stand-ins that record what the app asks of
them - the service calls, the publishes, the listeners and the timers - with
the call shapes the app uses (AppDaemon 4.5.13, paho-mqtt 2.1.0). Nothing here
talks to a network. The payloads the app publishes also go through the panel's
own parser (tools/media/probe.py), so the two ends are tested against each other.

  python3 tools/media/appdaemon/test_matrix_media.py
"""
import json
import pathlib
import sys
import tempfile
import threading
import time
import types
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

SECRET = "s3cr3t-broker-pass"


class FakeHass:
    def __init__(self, args, states):
        self.args = args
        self.states = states
        self.logs, self.calls, self.listens, self.timers, self.every = [], [], [], [], []
        self.results = {}

    def log(self, msg, level="INFO"):
        self.logs.append((level, msg))

    def get_state(self, entity_id=None, attribute=None, default=None, **kwargs):
        s = self.states.get(entity_id)
        if s is None:
            return None
        if attribute == "all":
            return json.loads(json.dumps(s))
        if attribute:
            return s["attributes"].get(attribute)
        return s["state"]

    def listen_state(self, callback, entity_id, **kwargs):
        self.listens.append((entity_id, kwargs))
        return "handle"

    def run_in(self, callback, delay, *args, **kwargs):
        self.timers.append((callback, delay))
        return "timer"

    def run_every(self, callback, start=None, interval=0, *args, **kwargs):
        self.every.append((callback, start, interval))
        return "timer"

    def call_service(self, service, **data):
        self.calls.append((service, data))
        r = self.results.get(service)
        if isinstance(r, Exception):
            raise r
        return r if r is not None else {"success": True, "result": {}}


class FakeClient:
    def __init__(self, api_version, client_id=None):
        self.api_version, self.client_id = api_version, client_id
        self.published, self.subscribed, self.will, self.login = [], [], None, None

    def username_pw_set(self, username, password=None):
        self.login = (username, password)

    def will_set(self, topic, payload=None, qos=0, retain=False):
        self.will = (topic, payload, qos, retain)

    def connect_async(self, host, port, keepalive):
        self.target = (host, port, keepalive)

    def loop_start(self):
        pass

    def loop_stop(self):
        pass

    def disconnect(self):
        pass

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.published.append((topic, payload, qos, retain))

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
import matrix_media as mm  # noqa: E402

ROOT = "nickoscope_matrix/a1b2c3/media/"
NOW = int(time.time())


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%S.000000+00:00", time.gmtime(epoch))


def states():
    return {
        # attribute names and shapes as read live from the owner's Home Assistant, 2026-09-14
        "media_player.nickoscope32_audio_s3": {"state": "playing", "attributes": {
            "volume_level": 0.5, "is_volume_muted": False, "media_content_id": "spotify--sRNf2Spf://track/52vHgE4QfUoEyn8h3IqT7T",
            "media_content_type": "music", "media_duration": 241, "media_position": 56,
            "media_position_updated_at": iso(NOW - 30), "media_title": "Olive Tree", "media_artist": "Quinn XCII",
            "media_album_name": "Olive Tree", "app_id": "music_assistant", "mass_player_type": "player",
            "friendly_name": "NickoScope32 Audio S3"}},
        "media_player.yandex_station_u00c533009fayb": {"state": "paused", "attributes": {
            "volume_level": 0.4, "is_volume_muted": False, "media_content_type": "music", "media_duration": 225,
            "media_position": 95, "media_position_updated_at": iso(NOW - 600), "media_title": "Звезда по имени Солнце",
            "media_artist": "Кино", "friendly_name": "Яндекс Станция 2"}},
        "media_player.eversolo_dmp_a8": {"state": "off", "attributes": {"friendly_name": "Eversolo DMP-A8"}},
        "media_player.loud_esp32s3": {"state": "unavailable", "attributes": {"friendly_name": "loud-esp32s3"}},
    }


FAVS = {"success": True, "result": {"context": {}, "response": {"items": [
    {"media_type": "radio", "uri": "library://radio/2", "name": "101 SMOOTH JAZZ", "version": "", "image": None, "favorite": True},
    {"media_type": "radio", "uri": "library://radio/5", "name": "80s80s Radio", "version": "", "image": None, "favorite": True},
    {"media_type": "radio", "uri": "bad\"uri", "name": "refused", "version": "", "image": None, "favorite": True},
], "limit": 16, "offset": 0, "order_by": "name", "media_type": "radio"}}}


def make(**over):
    args = {"device": "a1b2c3", "mqtt_host": "192.168.4.35", "mqtt_user": "u", "mqtt_pass": SECRET,
            "players": ["media_player.nickoscope32_audio_s3", "media_player.yandex_station_u00c533009fayb",
                        "media_player.eversolo_dmp_a8", "media_player.loud_esp32s3"],
            "default_player": "media_player.nickoscope32_audio_s3", "ma_config_entry_id": "01KSTJEYV7QZEMM7CMJEWXVEHP"}
    args.update(over)
    app = mm.MatrixMedia(args, states())
    app.results["music_assistant/get_library"] = FAVS
    app.initialize()
    return app


def last(app, leaf):
    for topic, payload, qos, retain in reversed(app._mq.published):
        if topic == ROOT + leaf:
            return json.loads(payload) if payload not in ("online", "offline") else payload, retain, payload
    return None, None, None


def count(app, leaf):
    return sum(1 for t, *_ in app._mq.published if t == ROOT + leaf)


def cmd(app, **msg):
    msg.setdefault("v", 1)
    app.handle(ROOT + "cmd", json.dumps(msg).encode())


class App(unittest.TestCase):
    def setUp(self):
        self.app = make()
        self.app._on_connect(self.app._mq, None, None, types.SimpleNamespace(is_failure=False), None)
        self.app._inbox.join()      # the resync runs on the app's worker, not on paho's thread

    def tearDown(self):
        self.app.terminate()

    def test_setup(self):
        a = self.app
        self.assertEqual(a._mq.will, (ROOT + "ha", "offline", 1, True))
        self.assertEqual(a._mq.api_version, "VERSION2")
        self.assertEqual(sorted(e for e, _ in a.listens), sorted(a._players))
        self.assertTrue(all(kw == {"attribute": "all"} for _, kw in a.listens))
        self.assertEqual(a.every[0][2], 60)
        self.assertEqual(a._mq.subscribed, [[(ROOT + "select", 1), (ROOT + "cmd", 0)]])
        self.assertEqual(last(a, "ha")[:2], ("online", True))
        # On paho's thread _on_connect only says online and subscribes; the lists,
        # the favourites read and the state wait for the app's worker.
        quiet = make()
        quiet._stop.set()
        quiet._worker.join(timeout=3)
        quiet._on_connect(quiet._mq, None, None, types.SimpleNamespace(is_failure=False), None)
        self.assertEqual([t for t, *_ in quiet._mq.published], [ROOT + "ha"])
        self.assertFalse(quiet.calls)
        self.assertEqual(quiet._inbox.qsize(), 1)
        quiet.terminate()
        with self.assertRaises(ValueError):
            make(device="A1B2C3")
        with self.assertRaises(ValueError):
            make(players=["light.kitchen"])

    def test_payloads(self):
        a = self.app
        st, retain, raw = last(a, "state")
        self.assertTrue(retain)
        self.assertEqual((st["player"], st["st"], st["kind"], st["src"], st["vol"], st["dur"], st["pos"]),
                         ("media_player.nickoscope32_audio_s3", "playing", "music", "MA", 50, 241, 56))
        self.assertEqual(st["pos_at"], NOW - 30)
        self.assertLessEqual(abs(st["ts"] - NOW), 5)
        pl, retain, _ = last(a, "players")
        self.assertTrue(retain)
        self.assertEqual(pl["sel"], "media_player.nickoscope32_audio_s3")
        self.assertEqual([p["st"] for p in pl["list"]], ["playing", "paused", "off", "unavailable"])
        self.assertIn("Яндекс Станция 2".encode(), [t for t in a._mq.published if t[0] == ROOT + "players"][-1][1])  # UTF-8, not \\u
        fv, retain, _ = last(a, "favs")
        self.assertEqual(fv["list"], [{"id": "library://radio/2", "name": "101 SMOOTH JAZZ"}, {"id": "library://radio/5", "name": "80s80s Radio"}])
        service, data = next(c for c in a.calls if c[0] == "music_assistant/get_library")
        self.assertEqual(data, {"config_entry_id": "01KSTJEYV7QZEMM7CMJEWXVEHP", "media_type": "radio", "favorite": True,
                                "limit": 16, "order_by": "name", "return_response": True})
        for topic, payload, qos, retain in a._mq.published:
            self.assertLessEqual(len(payload), mm.PAYLOAD_MAX, topic)

    def test_payloads_parse_on_the_panel(self):
        import probe
        a = self.app
        cmd(a, cmd="select", player="media_player.yandex_station_u00c533009fayb")
        with tempfile.TemporaryDirectory() as tmp:
            for leaf in ("state", "players", "favs"):
                _, _, raw = last(a, leaf)
                path = pathlib.Path(tmp, leaf + ".json")
                path.write_bytes(raw)
                code, out = probe.parse(leaf, path)
                self.assertEqual(code, 0, leaf + ": " + out)

    def test_real_changes_only(self):
        a = self.app
        n = count(a, "state")
        a._last_state_at = 0
        a._changed("media_player.nickoscope32_audio_s3", None, None, None, {})
        self.assertEqual(count(a, "state"), n, "nothing changed")
        s = a.states["media_player.nickoscope32_audio_s3"]["attributes"]
        s["media_position"] = 56 + 30
        s["media_position_updated_at"] = iso(NOW)          # re-anchored where the old anchor already was
        a._changed("media_player.nickoscope32_audio_s3", None, None, None, {})
        self.assertEqual(count(a, "state"), n, "a re-anchor is not a change")
        s["media_position"] = 200                            # a seek
        a._changed("media_player.nickoscope32_audio_s3", None, None, None, {})
        self.assertEqual(count(a, "state"), n + 1)
        a._changed("media_player.eversolo_dmp_a8", None, None, None, {})
        self.assertEqual(count(a, "state"), n + 1, "another player's change is not the followed one's")

    def test_throttle_and_keepalive(self):
        a = self.app
        n = count(a, "state")
        a.states["media_player.nickoscope32_audio_s3"]["attributes"]["media_title"] = "Another"
        a._changed("media_player.nickoscope32_audio_s3", None, None, None, {})
        self.assertEqual(count(a, "state"), n, "throttled")
        self.assertEqual(len(a.timers), 1)
        a._changed("media_player.nickoscope32_audio_s3", None, None, None, {})
        self.assertEqual(len(a.timers), 1, "one timer, however many changes")
        a.timers[0][0]()
        self.assertEqual(count(a, "state"), n + 1)
        self.assertEqual(last(a, "state")[0]["title"], "Another")
        a._tick()
        self.assertEqual(count(a, "state"), n + 1, "keepalive not due")
        a._last_state_at -= 120
        a._tick()
        self.assertEqual(count(a, "state"), n + 2, "keepalive: the same state, a new ts")

    def test_commands(self):
        a = self.app
        calls = lambda: [c for c in a.calls if not c[0].startswith("music_assistant/get_library")]
        p = "media_player.nickoscope32_audio_s3"
        cmd(a, cmd="toggle", player=p)
        cmd(a, cmd="next", player=p)
        cmd(a, cmd="vol", player=p, value=42)
        cmd(a, cmd="vol", player=p, value=150)
        cmd(a, cmd="vol", player=p, value=True)
        cmd(a, cmd="vol_step", player=p, value=-4)
        cmd(a, cmd="mute", player=p, value=True)
        cmd(a, cmd="mute", player=p, value="yes")
        cmd(a, cmd="play_fav", player=p, id="library://radio/5")
        cmd(a, cmd="dance", player=p)
        self.assertEqual(calls(), [
            ("media_player/media_play_pause", {"entity_id": p}),
            ("media_player/media_next_track", {"entity_id": p}),
            ("media_player/volume_set", {"entity_id": p, "volume_level": 0.42}),
            ("media_player/volume_set", {"entity_id": p, "volume_level": 0.46}),
            ("media_player/volume_mute", {"entity_id": p, "is_volume_muted": True}),
            ("music_assistant/play_media", {"entity_id": p, "media_id": "library://radio/5", "media_type": "radio"}),
        ])
        a.states[p]["attributes"].pop("volume_level")
        cmd(a, cmd="vol_step", player=p, value=2)
        self.assertEqual(calls()[-1], ("media_player/volume_up", {"entity_id": p}))

    def test_refusals_reach_the_panel(self):
        a = self.app
        p = "media_player.nickoscope32_audio_s3"
        before = len(a.calls)
        cmd(a, cmd="play_fav", player=p, id="library://radio/99")
        self.assertEqual(len(a.calls), before)
        a._last_state_at = 0
        a._deferred_state()
        self.assertEqual(last(a, "state")[0]["err"], "NOT_A_FAV")
        cmd(a, cmd="toggle", player="media_player.eversolo_dmp_a8")
        self.assertEqual(a._err, "NOT_FOLLOWED")
        a.results["media_player/media_next_track"] = {"success": False, "error": {"code": "x"}}
        cmd(a, cmd="next", player=p)
        self.assertEqual(a._err, "CALL_FAILED")
        a.results["media_player/media_next_track"] = RuntimeError("boom " + SECRET)
        cmd(a, cmd="next", player=p)
        self.assertEqual(a._err, "CALL_FAILED")
        cmd(a, cmd="toggle", player=p)          # a good one clears it
        self.assertEqual(a._err, "")
        cmd(a, cmd="select", player="media_player.yandex_station_u00c533009fayb")
        cmd(a, cmd="play_fav", player="media_player.yandex_station_u00c533009fayb", id="library://radio/2")
        self.assertEqual(a._err, "NOT_MA")
        cmd(a, cmd="select", player="media_player.loud_esp32s3")
        before = len(a.calls)
        cmd(a, cmd="toggle", player="media_player.loud_esp32s3")
        self.assertEqual((len(a.calls), a._err), (before, "UNAVAILABLE"))

    def test_select(self):
        a = self.app
        a.handle(ROOT + "select", b'{"v":1,"player":"media_player.yandex_station_u00c533009fayb"}')
        self.assertEqual(a._followed, "media_player.yandex_station_u00c533009fayb")
        self.assertEqual(last(a, "players")[0]["sel"], "media_player.yandex_station_u00c533009fayb")
        st = last(a, "state")[0]
        self.assertEqual((st["player"], st["st"], st["src"], st["title"], st["artist"]),
                         ("media_player.yandex_station_u00c533009fayb", "paused", "YA", "Звезда по имени Солнце", "Кино"))
        a.handle(ROOT + "select", b'{"v":1,"player":"media_player.kitchen"}')
        self.assertEqual(a._followed, "media_player.yandex_station_u00c533009fayb")
        a.handle(ROOT + "select", b"not json")
        a.handle(ROOT + "select", b'{"v":2,"player":"media_player.eversolo_dmp_a8"}')
        self.assertEqual(a._followed, "media_player.yandex_station_u00c533009fayb")

    def test_queue_hands_over(self):
        a = self.app
        msg = types.SimpleNamespace(topic=ROOT + "cmd", payload=b'{"v":1,"cmd":"pause","player":"media_player.nickoscope32_audio_s3"}')
        a._on_message(a._mq, None, msg)
        a._on_message(a._mq, None, types.SimpleNamespace(topic=ROOT + "cmd", payload=b"x" * 600))   # oversized: dropped
        for _ in range(50):
            if any(c[0] == "media_player/media_pause" for c in a.calls):
                break
            time.sleep(0.05)
        self.assertIn(("media_player/media_pause", {"entity_id": "media_player.nickoscope32_audio_s3"}), a.calls)

    def test_no_favourites_without_an_entry(self):
        a = make(ma_config_entry_id=None)
        a._on_connect(a._mq, None, None, types.SimpleNamespace(is_failure=False), None)
        a._inbox.join()
        self.assertEqual(last(a, "favs")[0]["list"], [])
        self.assertFalse(any(c[0] == "music_assistant/get_library" for c in a.calls))
        a.terminate()

    def test_no_secret_in_the_log(self):
        a = self.app
        a.results["media_player/media_play"] = RuntimeError("password " + SECRET)
        cmd(a, cmd="play", player="media_player.nickoscope32_audio_s3")
        self.assertTrue(a.logs)
        self.assertFalse(any(SECRET in m for _, m in a.logs))


class Helpers(unittest.TestCase):
    def test_clip(self):
        self.assertEqual(mm.clip("Звезда", 5), "Зв")      # 4 bytes: a whole letter more would be 6
        self.assertEqual(mm.clip(None, 5), "")
        self.assertEqual(mm.clip("abc", 5), "abc")

    def test_state_words(self):
        self.assertEqual([mm.map_state(s) for s in ("buffering", "on", "standby", "unknown", None)],
                         ["playing", "idle", "off", "unavailable", "unavailable"])

    def test_kind(self):
        self.assertEqual(mm.kind_of({"media_content_id": "library://radio/2"}), "radio")
        self.assertEqual(mm.kind_of({"media_content_type": "channel"}), "radio")
        self.assertEqual(mm.kind_of({"media_content_id": "media-source://tts/cloud?message=hi"}), "tts")
        self.assertEqual(mm.kind_of({"media_content_type": "music", "media_content_id": "spotify://track/1"}), "music")

    def test_epoch(self):
        self.assertEqual(mm.epoch("2026-09-13T19:47:53.938864+00:00"), 1789328873)
        self.assertIsNone(mm.epoch("yesterday"))
        self.assertIsNone(mm.epoch(None))

    def test_fit(self):
        p = {"v": 1, "player": "media_player.x", "st": "playing", "title": "Я" * 400, "artist": "Я" * 400, "album": "Я" * 400, "ts": NOW}
        data = mm.fit_state(p)
        self.assertLessEqual(len(data), mm.PAYLOAD_MAX)
        json.loads(data)
        lst = {"v": 1, "ts": NOW, "list": [{"id": "radiobrowser://radio/" + "f" * 60, "name": "Я" * 23} for _ in range(40)]}
        data = mm.fit_list(lst)
        self.assertLessEqual(len(data), mm.PAYLOAD_MAX)
        self.assertGreater(len(json.loads(data)["list"]), 0)

    def test_response_shapes(self):
        items = [{"uri": "library://radio/1", "name": "A"}]
        for shape in ({"result": {"response": {"items": items}}}, {"response": {"items": items}}, {"items": items}):
            self.assertEqual(mm.response_items(shape), items)
        for shape in ({"success": False, "result": {"response": {"items": items}}}, None, [], {"result": 3}):
            self.assertIsNone(mm.response_items(shape))

    def test_same_state(self):
        a = {"v": 1, "player": "p", "st": "playing", "pos": 10, "pos_at": 1000, "ts": 1}
        self.assertTrue(mm.same_state(a, dict(a, pos=20, pos_at=1010, ts=2), 1020))
        self.assertFalse(mm.same_state(a, dict(a, pos=40, pos_at=1010), 1020))
        self.assertFalse(mm.same_state(a, dict(a, st="paused"), 1020))
        self.assertFalse(mm.same_state(a, {k: v for k, v in a.items() if k != "pos"}, 1020))
        self.assertFalse(mm.same_state(None, a, 1020))


if __name__ == "__main__":
    unittest.main(verbosity=1)

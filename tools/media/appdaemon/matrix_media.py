"""Media player for the LED panel: Home Assistant and Music Assistant to MQTT, and back.

Follows one media_player - the one the panel chose, when it is in `players` -
and publishes the compact retained payloads the panel draws:

  <base>/<device>/media/state     the followed player: on real changes, and a keepalive
  <base>/<device>/media/players   the allowed players and their states
  <base>/<device>/media/favs      Music Assistant's favourite radio stations
  <base>/<device>/media/ha        online | offline  (offline is this client's will)

and takes

  <base>/<device>/media/select    retained, from the panel: {"v":1,"player":"media_player.x"}
  <base>/<device>/media/cmd       from the panel: toggle play pause next prev vol vol_step mute
                                  select play_fav

The contract, field by field, is src/media/README.md in the firmware.

What "real change" means here: any field but the timestamp and the position,
or a position more than two seconds from where the previous anchor puts it
now. A player that re-anchors its position every few seconds while it plays is
not a change; a seek is. On top of that, one state payload a second at most
(min_interval) and the same state again every `keepalive` seconds, so the panel
can tell a quiet player from a dead app.

Why AppDaemon, as the rail board's option A: comparing positions against their
anchor, throttling and a keepalive are Python's job rather than Jinja's; and
publishing straight to the broker with paho keeps a call_service event every
few seconds out of the recorder. No token: the HASS plugin's connection reads
states and calls actions, and the broker login comes from AppDaemon's
secrets.yaml. The log names commands, entity ids and error codes, never a
payload's content or a credential.

Sources (read, not recalled): AppDaemon 4.5.13 (the version add-on 0.19.2
pins) hassapi.call_service and its return_response, adapi.listen_state /
run_in / run_every / get_state, utils.sync_decorator (API calls from another
thread go through run_coroutine_threadsafe); paho-mqtt 2.1.0 client.py;
Home Assistant 2026.9.2 media_player services.yaml (volume_set volume_level
0..1, volume_mute is_volume_muted) and music_assistant services.py /
media_player.py (get_library response items with uri and name; play_media
media_id, media_type; mass_player_type attribute on MA players).
"""
import json
import queue
import re
import threading
import time
from datetime import datetime

import appdaemon.plugins.hass.hassapi as hass
import paho.mqtt.client as mqtt

SCHEMA = 1
PAYLOAD_MAX = 1900        # the panel's bus buffer is 2048 B with the topic and header in it
CMD_MAX = 512
MAX_PLAYERS = 8
MAX_FAVS = 16

# Byte bounds for UTF-8 strings, one under the panel's buffers (src/media/media_model.h).
NAME_BYTES, TITLE_BYTES, ARTIST_BYTES, ALBUM_BYTES = 47, 127, 95, 95

PLAYER_RE = re.compile(r"media_player\.[a-z0-9_]{1,50}")
DEVICE_RE = re.compile(r"[0-9a-f]{6}")
TAG_RE = re.compile(r"[A-Z0-9]{1,4}")
FAV_RE = re.compile(r"[\x21-\x7e]{1,95}")

# Home Assistant's states onto the panel's five.
STATE_MAP = {
    "playing": "playing", "buffering": "playing",
    "paused": "paused",
    "idle": "idle", "on": "idle",
    "off": "off", "standby": "off",
}

SIMPLE = {
    "toggle": "media_player/media_play_pause",
    "play": "media_player/media_play",
    "pause": "media_player/media_pause",
    "next": "media_player/media_next_track",
    "prev": "media_player/media_previous_track",
}


# ── pure helpers: tools/media/appdaemon/test_matrix_media.py tests these ──────
def clip(text, max_bytes):
    """A string cut to max_bytes of UTF-8 at a whole code point; None and non-strings are ''."""
    if not isinstance(text, str):
        return ""
    raw = text.encode("utf-8")
    return raw[:max_bytes].decode("utf-8", errors="ignore") if len(raw) > max_bytes else text


def map_state(state):
    return STATE_MAP.get(state, "unavailable")


def valid_player(entity_id):
    return isinstance(entity_id, str) and PLAYER_RE.fullmatch(entity_id) is not None


def valid_fav(fav_id):
    return isinstance(fav_id, str) and FAV_RE.fullmatch(fav_id) is not None and '"' not in fav_id and "\\" not in fav_id


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def epoch(iso):
    """media_position_updated_at as an epoch second, or None."""
    if isinstance(iso, datetime):
        return int(iso.timestamp())
    if not isinstance(iso, str):
        return None
    try:
        return int(datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp())
    except ValueError:
        return None


def kind_of(attrs):
    """music, radio or tts. A heuristic, not verified against every integration:
    Music Assistant items are <provider>://<media_type>/<id> (as get_library
    returns them), so a radio's content id carries ://radio/."""
    ctype = str(attrs.get("media_content_type") or "").lower()
    cid = str(attrs.get("media_content_id") or "").lower()
    if ctype == "tts" or cid.startswith("media-source://tts"):
        return "tts"
    if ctype in ("radio", "channel") or "://radio/" in cid:
        return "radio"
    return "music"


def source_tag(entity_id, attrs, overrides):
    if entity_id in overrides:
        return overrides[entity_id]
    if "mass_player_type" in attrs:          # Music Assistant's extra attribute
        return "MA"
    if entity_id.startswith("media_player.yandex_station"):
        return "YA"
    return "HA"


def compose_state(entity_id, state_obj, overrides, err=""):
    """The state payload without ts, from get_state(entity, attribute="all")."""
    if not entity_id:
        return {"v": SCHEMA, "player": "", "st": "unavailable"}
    s = state_obj or {}
    a = s.get("attributes") or {}
    st = map_state(s.get("state"))
    p = {
        "v": SCHEMA,
        "player": entity_id,
        "name": clip(a.get("friendly_name") or entity_id.split(".", 1)[1], NAME_BYTES),
        "src": source_tag(entity_id, a, overrides),
        "st": st,
        "kind": kind_of(a),
    }
    if st != "unavailable":
        for key, attr, limit in (("title", "media_title", TITLE_BYTES), ("artist", "media_artist", ARTIST_BYTES),
                                 ("album", "media_album_name", ALBUM_BYTES)):
            text = clip(a.get(attr), limit)
            if text:
                p[key] = text
        vol = a.get("volume_level")
        if isinstance(vol, (int, float)) and not isinstance(vol, bool):
            p["vol"] = max(0, min(100, int(round(vol * 100))))
        if isinstance(a.get("is_volume_muted"), bool):
            p["muted"] = a["is_volume_muted"]
        dur = a.get("media_duration")
        if isinstance(dur, (int, float)) and not isinstance(dur, bool) and 0 < dur <= 604800:
            p["dur"] = int(round(dur))
        pos, at = a.get("media_position"), epoch(a.get("media_position_updated_at"))
        if isinstance(pos, (int, float)) and not isinstance(pos, bool) and 0 <= pos <= 604800 and at:
            p["pos"] = int(pos)
            p["pos_at"] = at
    if err:
        p["err"] = err
    return p


def implied_position(p, now):
    if "pos" not in p:
        return None
    if p.get("st") == "playing" and p.get("pos_at"):
        return p["pos"] + max(0, now - p["pos_at"])
    return p["pos"]


def same_state(a, b, now):
    """True when b says nothing a did not: ts ignored, positions compared where their anchors put them now."""
    if a is None or b is None:
        return False
    strip = ("ts", "pos", "pos_at")
    if {k: v for k, v in a.items() if k not in strip} != {k: v for k, v in b.items() if k not in strip}:
        return False
    pa, pb = implied_position(a, now), implied_position(b, now)
    if pa is None or pb is None:
        return pa is pb
    return abs(pa - pb) <= 2


def encode(payload):
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def fit_state(payload):
    """Encoded, shortened until it fits PAYLOAD_MAX: the album first, then the artist, then the title."""
    data = encode(payload)
    for key in ("album", "artist", "title", "name"):
        while len(data) > PAYLOAD_MAX and payload.get(key):
            payload[key] = clip(payload[key], max(0, len(payload[key].encode("utf-8")) - 16))
            if not payload[key]:
                del payload[key]
            data = encode(payload)
    return data


def fit_list(payload):
    """Encoded, with entries dropped from the end until it fits PAYLOAD_MAX."""
    data = encode(payload)
    while len(data) > PAYLOAD_MAX and payload["list"]:
        payload["list"].pop()
        data = encode(payload)
    return data


def response_items(result):
    """get_library's items from what AppDaemon's call_service returned, or None.
    AppDaemon 4.5.13 returns the websocket result; its docstring reads
    ["result"]["response"]. The other shapes are accepted defensively."""
    if not isinstance(result, dict):
        return None
    if result.get("success") is False:
        return None
    for path in (("result", "response"), ("response",), ()):
        node = result
        for key in path:
            node = node.get(key) if isinstance(node, dict) else None
        if isinstance(node, dict) and isinstance(node.get("items"), list):
            return node["items"]
    return None


def favs_from(items):
    out, seen = [], set()
    for it in items:
        if not isinstance(it, dict):
            continue
        uri = it.get("uri")
        if not valid_fav(uri) or uri in seen:
            continue
        seen.add(uri)
        out.append({"id": uri, "name": clip(it.get("name") or uri, NAME_BYTES)})
        if len(out) >= MAX_FAVS:
            break
    return out


# ── the app ──────────────────────────────────────────────────────────────────
class MatrixMedia(hass.Hass):
    def initialize(self):
        a = self.args
        device = str(a.get("device", ""))
        if not DEVICE_RE.fullmatch(device):
            raise ValueError("matrix_media: device must be the panel's six lower-case hex digits")
        root = "%s/%s/media/" % (a.get("topic_base", "nickoscope_matrix"), device)
        self._t = {leaf: root + leaf for leaf in ("state", "players", "favs", "ha", "select", "cmd")}

        players = [p for p in (a.get("players") or []) if valid_player(p)]
        if len(players) != len(a.get("players") or []):
            self.log("media: players that are not media_player ids were left out", level="WARNING")
        if len(players) > MAX_PLAYERS:
            self.log("media: only the first %d players are used" % MAX_PLAYERS, level="WARNING")
        self._players = players[:MAX_PLAYERS]
        if not self._players:
            raise ValueError("matrix_media: players must list at least one media_player")
        default = a.get("default_player")
        self._followed = default if default in self._players else self._players[0]
        self._tags = {k: v for k, v in (a.get("tags") or {}).items() if valid_player(k) and TAG_RE.fullmatch(str(v))}
        self._entry = a.get("ma_config_entry_id")
        self._keepalive = max(15, int(a.get("keepalive", 60)))
        self._min_interval = max(0.5, float(a.get("min_interval", 1)))
        self._favs_every = max(60, int(a.get("favs_refresh", 900)))

        self._lock = threading.RLock()
        self._last_state = None
        self._last_state_at = 0.0
        self._state_timer = False
        self._last_players = None
        self._favs = None            # the list last published
        self._favs_at = 0.0
        self._err = ""
        self._stop = threading.Event()
        self._inbox = queue.Queue(maxsize=32)
        self._worker = threading.Thread(target=self._work, name="matrix_media_cmd", daemon=True)
        self._worker.start()

        self._mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="appdaemon-matrix-media-" + device)
        if a.get("mqtt_user"):
            self._mq.username_pw_set(a["mqtt_user"], a.get("mqtt_pass"))
        self._mq.will_set(self._t["ha"], "offline", qos=1, retain=True)
        self._mq.on_connect = self._on_connect
        self._mq.on_message = self._on_message
        self._mq.connect_async(a["mqtt_host"], int(a.get("mqtt_port", 1883)), 30)
        self._mq.loop_start()

        for p in self._players:
            self.listen_state(self._changed, p, attribute="all")
        self.run_every(self._tick, "now", self._keepalive)
        self.log("media: %d players, following %s, device %s, favourites %s"
                 % (len(self._players), self._followed, device, "on" if self._entry else "off"))

    def terminate(self):
        self._stop.set()
        try:
            self._mq.publish(self._t["ha"], "offline", qos=1, retain=True)
            self._mq.loop_stop()
            self._mq.disconnect()
        except Exception:   # shutting down either way
            pass

    # ── MQTT ─────────────────────────────────────────────────────────────────
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if getattr(reason_code, "is_failure", False):
            self.log("media: broker refused the connection: %s" % reason_code, level="WARNING")
            return
        client.publish(self._t["ha"], "online", qos=1, retain=True)
        client.subscribe([(self._t["select"], 1), (self._t["cmd"], 0)])
        # Retained topics may have been lost with the broker: everything again,
        # from the worker - this is paho's thread, and reading the favourites
        # waits on Home Assistant.
        try:
            self._inbox.put_nowait((None, b""))
        except queue.Full:
            self.log("media: command queue full, the resync waits for the keepalive", level="WARNING")

    def _resync(self):
        with self._lock:
            self._last_players = None
            self._favs = None
            self._last_state = None
        self._publish_players()
        self._publish_favs(force=True)
        self._publish_state(force=True)

    def _on_message(self, client, userdata, msg):
        # paho's thread: hand over and return, so the network loop never waits on Home Assistant.
        if len(msg.payload) > CMD_MAX:
            return
        try:
            self._inbox.put_nowait((msg.topic, bytes(msg.payload)))
        except queue.Full:
            self.log("media: command queue full, one dropped", level="WARNING")

    def _work(self):
        while not self._stop.is_set():
            try:
                topic, raw = self._inbox.get(timeout=1)
            except queue.Empty:
                continue
            try:
                if topic is None:
                    self._resync()
                else:
                    self.handle(topic, raw)
            except Exception as e:   # the type only: a message could quote a payload
                self.log("media: %s handling %s" % (type(e).__name__, topic.rsplit("/", 1)[-1] if topic else "the resync"),
                         level="WARNING")
            finally:
                self._inbox.task_done()

    def _publish(self, leaf, data, retain=True):
        self._mq.publish(self._t[leaf], data, qos=0, retain=retain)

    # ── Home Assistant to the panel ──────────────────────────────────────────
    def _changed(self, entity, attribute, old, new, *args, **kwargs):
        if entity == self._followed:
            self._want_state()
        self._publish_players()

    def _want_state(self):
        with self._lock:
            if self._state_timer:
                return
            wait = self._min_interval - (time.time() - self._last_state_at)
            if wait > 0:
                self._state_timer = True
                self.run_in(self._deferred_state, max(1, int(wait + 0.999)))
                return
        self._publish_state()

    def _deferred_state(self, *args, **kwargs):
        with self._lock:
            self._state_timer = False
        self._publish_state()

    def _publish_state(self, force=False):
        with self._lock:
            now = int(time.time())
            payload = compose_state(self._followed, self.get_state(self._followed, attribute="all"), self._tags, self._err)
            if not force and same_state(self._last_state, payload, now):
                return
            payload["ts"] = now
            self._publish("state", fit_state(payload))
            self._last_state = payload
            self._last_state_at = time.time()

    def _publish_players(self):
        with self._lock:
            lst = []
            for p in self._players:
                s = self.get_state(p, attribute="all") or {}
                name = (s.get("attributes") or {}).get("friendly_name") or p.split(".", 1)[1]
                lst.append({"id": p, "name": clip(name, NAME_BYTES), "st": map_state(s.get("state"))})
            body = {"v": SCHEMA, "sel": self._followed, "list": lst}
            if body == self._last_players:
                return
            self._last_players = json.loads(json.dumps(body))
            body["ts"] = int(time.time())
            self._publish("players", fit_list(body))

    def _publish_favs(self, force=False):
        if not self._entry:
            items = []
        else:
            try:
                result = self.call_service("music_assistant/get_library", config_entry_id=self._entry, media_type="radio",
                                           favorite=True, limit=MAX_FAVS, order_by="name", return_response=True)
            except Exception as e:
                self.log("media: %s reading Music Assistant favourites" % type(e).__name__, level="WARNING")
                return
            found = response_items(result)
            if found is None:
                self.log("media: Music Assistant favourites came back in a shape this app does not know", level="WARNING")
                return
            items = favs_from(found)
        with self._lock:
            self._favs_at = time.time()
            if not force and items == self._favs:
                return
            self._favs = items
            self._publish("favs", fit_list({"v": SCHEMA, "ts": int(time.time()), "list": list(items)}))

    def _tick(self, *args, **kwargs):
        if time.time() - self._last_state_at >= self._keepalive - 1:
            self._publish_state(force=True)
        if time.time() - self._favs_at >= self._favs_every:
            self._publish_favs()

    # ── the panel to Home Assistant ──────────────────────────────────────────
    def handle(self, topic, raw):
        try:
            msg = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            self.log("media: %s was not JSON" % topic.rsplit("/", 1)[-1], level="WARNING")
            return
        if not isinstance(msg, dict) or msg.get("v") != SCHEMA:
            return
        if topic == self._t["select"]:
            self._select(msg.get("player"))
            return
        if topic != self._t["cmd"]:
            return
        cmd = msg.get("cmd")
        if cmd == "select":
            self._select(msg.get("player"))
            return
        player = msg.get("player")
        eid = self._followed
        if player is not None and player != eid:
            self._fail("NOT_FOLLOWED", cmd)   # meant for a player this app no longer follows
            return
        state = self.get_state(eid)
        if state in (None, "unavailable", "unknown"):
            self._fail("UNAVAILABLE", cmd)
            return
        value = msg.get("value")
        if cmd in SIMPLE:
            self._call(cmd, SIMPLE[cmd], eid)
        elif cmd == "vol":
            if not is_int(value) or not 0 <= value <= 100:
                return
            self._call(cmd, "media_player/volume_set", eid, volume_level=value / 100)
        elif cmd == "vol_step":
            if not is_int(value) or not -20 <= value <= 20 or value == 0:
                return
            cur = self.get_state(eid, attribute="volume_level")
            if isinstance(cur, (int, float)) and not isinstance(cur, bool):
                self._call(cmd, "media_player/volume_set", eid, volume_level=max(0.0, min(1.0, round(cur + value / 100, 2))))
            else:
                self._call(cmd, "media_player/volume_up" if value > 0 else "media_player/volume_down", eid)
        elif cmd == "mute":
            if not isinstance(value, bool):
                return
            self._call(cmd, "media_player/volume_mute", eid, is_volume_muted=value)
        elif cmd == "play_fav":
            fav = msg.get("id")
            if not valid_fav(fav) or not any(f["id"] == fav for f in (self._favs or [])):
                self._fail("NOT_A_FAV", cmd)
                return
            if "mass_player_type" not in (self.get_state(eid, attribute="all") or {}).get("attributes", {}):
                self._fail("NOT_MA", cmd)   # play_media of Music Assistant targets its own players only
                return
            self._call(cmd, "music_assistant/play_media", eid, media_id=fav, media_type="radio")
        else:
            self.log("media: unknown command ignored", level="DEBUG")

    def _select(self, player):
        if not valid_player(player) or player not in self._players:
            self.log("media: the panel chose %s, which is not in players; still following %s"
                     % (player if valid_player(player) else "an invalid id", self._followed), level="WARNING")
            self._publish_players()
            return
        with self._lock:
            changed = player != self._followed
            self._followed = player
            if changed:
                self._err = ""
        if changed:
            self.log("media: following %s" % player)
        self._publish_players()
        self._publish_state(force=changed)

    def _call(self, cmd, service, entity_id, **data):
        try:
            result = self.call_service(service, entity_id=entity_id, **data)
        except Exception as e:
            self.log("media: %s calling %s" % (type(e).__name__, service), level="WARNING")
            self._fail("CALL_FAILED", cmd)
            return
        if isinstance(result, dict) and result.get("success") is False:
            self.log("media: Home Assistant refused %s for %s" % (service, entity_id), level="WARNING")
            self._fail("CALL_FAILED", cmd)
            return
        self.log("media: %s -> %s" % (cmd, entity_id), level="DEBUG")
        if self._err:
            with self._lock:
                self._err = ""
            self._want_state()

    def _fail(self, code, cmd):
        self.log("media: %s not done: %s" % (cmd if isinstance(cmd, str) and len(cmd) <= 12 else "command", code),
                 level="WARNING")
        with self._lock:
            self._err = code
        self._want_state()

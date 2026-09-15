"""The AppDaemon app: MatrixMarket wires the features apps.yaml lists and runs one worker.

    matrix_market:
      module: matrix_market.app        # this package; the __init__.py makes it one (APPGUIDE, "App Packages")
      class: MatrixMarket
      features: [history, portfolio, live, tape, ha_entities]
      ...                              # the wiring (config.APP_SCHEMA) and the settings (config.SCHEMA)

Threads, as the media app: AppDaemon's callbacks and paho's network thread only
enqueue; one worker thread does every fetch, compute and publish, so a slow
Yahoo answer never holds a scheduler thread and the payloads of a generation go
out in order. The log names symbols, topics' leaves, counts and exception
types, never a payload's content, a URL with a crumb or a credential.

A generation (`_refresh`): every feature's contribute() in `order`, then the
publisher sends the set, clears what the previous generation had and this one
lacks, and ends with `status` carrying the generation. Triggers: the start
(a fetch when the store has not been fetched today), the daily timer, a panel
`config` that changes something (a fetch only when it brings a symbol the store
lacks), the 6-hour keepalive (no fetch).
"""
import datetime as dt
import json
import pathlib
import queue
import threading
import time
from typing import Callable, List, Optional

import appdaemon.plugins.hass.hassapi as hass
import paho.mqtt.client as mqtt

from . import SCHEMA_V, __version__, config
from . import ha_entities, history, live, portfolio, tape  # noqa: F401  (registrations)
from ._ref import market_ref as M
from .features import Context, Feature
from .publisher import CONFIG_MAX, Publisher, Topics
from .registry import FEATURES, PROVIDERS
from .store import Store


DEBOUNCE_S = 5          # a burst of portal saves costs one recompute (the panel's audit rule)
TERMINATE_JOIN_S = 30   # terminate() waits this long for the worker's current job (an HTTP request times out at 20 s)
OFFLINE_WAIT_S = 3      # and this long for the broker to take `offline`


class Worker:
    """One thread, one queue of callables; exceptions are logged by type and the loop goes on."""

    def __init__(self, log: Callable[..., None], name: str = "matrix_market"):
        self.log = log
        self.q: "queue.Queue" = queue.Queue(maxsize=64)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, name=name, daemon=True)
        self.busy = threading.Lock()

    def start(self) -> None:
        self.thread.start()

    def stop(self, timeout: float = 5.0) -> None:
        self.stop_event.set()
        if self.thread.is_alive():
            self.thread.join(timeout)

    def put(self, fn: Callable, *args) -> bool:
        try:
            self.q.put_nowait((fn, args))
            return True
        except queue.Full:
            self.log("market: worker queue full, one job dropped", level="WARNING")
            return False

    def _run(self) -> None:
        while not self.stop_event.is_set():
            try:
                fn, args = self.q.get(timeout=1)
            except queue.Empty:
                continue
            try:
                with self.busy:
                    fn(*args)
            except Exception as e:              # the message too (audit MINOR 8); messages carry paths, never queries
                self.log("market: %s in %s: %s" % (type(e).__name__, getattr(fn, "__name__", "a job"), str(e)[:200]), level="WARNING")
            finally:
                self.q.task_done()


def build_features(ctx: Context, names: List[str], device: str) -> List[Feature]:
    feats = []
    for n in names:
        cls = FEATURES.get(n)
        feats.append(cls(ctx, device) if n == "ha_entities" else cls(ctx))
    return sorted(feats, key=lambda f: f.order)


class MatrixMarket(hass.Hass):
    def initialize(self):
        args = dict(self.args or {})
        store_dir = args.get("store_dir") or config.DEFAULT_STORE_DIR
        # The local override: <store_dir>/local.json unless local_file says otherwise ("" = none). Never under
        # apps/, where AppDaemon would read it as app configuration (audit M2).
        local_path = None if args.get("local_file") == "" else pathlib.Path(args.get("local_file") or pathlib.Path(store_dir, config.LOCAL_FILE))
        local, local_note = None, "none"
        if local_path is not None:
            try:
                local = config.load_local(local_path)
                local_note = "read from %s" % local_path.name if local is not None else "none at %s" % local_path
            except config.LocalFileError as e:
                local_note = "refused: %s" % e
        wiring, wiring_errors = config.app_args(config.merge_wiring(args, local))
        self._device = wiring["device"]
        base = config.from_args(args, local)
        self._base = base                        # apps.yaml (+ the local file): what the panel's config lays over
        self._settings = base
        for e in wiring_errors + base.errors:
            self.log("market: apps.yaml %s: %s (%s)" % (e["key"], e["code"], e["msg"]), level="WARNING")
        self.log("market: local override %s" % local_note, level="WARNING" if local_note.startswith("refused") else "INFO")

        store_dir = wiring.get("store_dir") or config.DEFAULT_STORE_DIR
        self._store = Store(store_dir)
        # Start from the panel's last config, not from apps.yaml. The panel's retained config is read only after
        # the first generation is queued; a first generation from apps.yaml would publish another allocation to
        # the panel, write it into HA's statistics and clear the panel's own symbols (final audit, MAJOR).
        # When the retained config then arrives it is identical, so nothing is recomputed.
        last = self._store.load_table("config_last")
        if last:
            restored = config.from_panel(base, last)
            if not restored.errors:
                self._settings = restored
                self.log("market: starting from the panel's last config", level="INFO")
            else:
                self.log("market: the stored panel config was refused (%s); starting from apps.yaml"
                         % ", ".join(e["key"] or e["code"] for e in restored.errors[:4]), level="WARNING")
        provider_cls = PROVIDERS.get(wiring["provider"])
        self._provider = provider_cls(gap_s=float(self._settings["fetch.history_gap_s"]), user_agent=wiring["user_agent"], log=self.log)
        self._topics = Topics(wiring["topic_base"], self._device)

        self._mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="appdaemon-matrix-market-" + self._device)
        if wiring.get("mqtt_user"):
            self._mq.username_pw_set(wiring["mqtt_user"], wiring.get("mqtt_pass"))
        self._mq.will_set(self._topics.ha, "offline", qos=1, retain=True)
        self._mq.on_connect = self._on_connect
        self._mq.on_message = self._on_message
        self._publisher = Publisher(self._mq, self._topics, self.log, store=self._store)

        self._worker = Worker(self.log)
        tz = None
        try:
            from zoneinfo import ZoneInfo
            tz = ZoneInfo(str(self.get_timezone()))
        except Exception:
            tz = None
        self._ctx = Context(self._settings, self._store, self._provider, self._publisher, self.log,
                            scheduler=self, enqueue=self._worker.put, tz=tz, stopping=self._worker.stop_event)
        self._ctx.data["device"] = self._device
        self._ctx.data["user_agent"] = wiring["user_agent"]
        self._ctx.data["refresh"] = self.request_refresh
        self._ctx.data["keepalive"] = self.request_keepalive
        self._features = build_features(self._ctx, wiring["features"], self._device)
        self._lock = threading.RLock()
        self._last_config_raw: Optional[bytes] = None
        self._debounce_handle = None
        self._debounce_fetch = False
        self._debounce_lock = threading.Lock()      # the worker writes these, AppDaemon's timer thread reads them (MINOR 6)

        self._worker.start()
        for f in self._features:
            f.start()
        self._mq.connect_async(wiring["mqtt_host"], int(wiring["mqtt_port"]), 30)
        self._mq.loop_start()
        self.request_refresh("start", fetch="stale")
        self.log("market: v%s, device %s, features %s, %d symbols, store %s"
                 % (__version__, self._device, ",".join(f.name for f in self._features), len(self._settings.watched_symbols()), store_dir))

    def terminate(self):
        """Stop the features, let the worker finish its current job (bounded), say `offline` at QoS 1 and wait for
        the broker to take it, stop paho's loop, and only then let the store go (audit MINOR 7)."""
        for f in self._features:
            try:
                f.stop()
            except Exception as e:
                self.log("market: %s stopping %s" % (type(e).__name__, f.name), level="WARNING")
        self._worker.stop(TERMINATE_JOIN_S)
        busy = self._worker.thread.is_alive()
        if busy:
            self.log("market: the worker is still busy after %d s; the store stays locked until it ends" % TERMINATE_JOIN_S,
                     level="WARNING")
        try:
            info = self._mq.publish(self._topics.ha, "offline", qos=1, retain=True)
            wait = getattr(info, "wait_for_publish", None)
            if wait is not None:
                wait(OFFLINE_WAIT_S)
        except Exception as e:
            self.log("market: %s publishing offline" % type(e).__name__, level="WARNING")
        try:
            self._mq.loop_stop()
            self._mq.disconnect()
        except Exception:
            pass
        if not busy:
            self._store.close()

    # ── MQTT ─────────────────────────────────────────────────────────────────
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if getattr(reason_code, "is_failure", False):
            self.log("market: broker refused the connection: %s" % reason_code, level="WARNING")
            return
        client.publish(self._topics.ha, "online", qos=1, retain=True)
        client.subscribe([(self._topics.config, 1), (self._ha_status_topic(), 1)])
        # On the worker, not on paho's thread: discovery (the cache reset) and the last generation, which the
        # broker may have lost. Nothing is published before this point (audit M3).
        self._worker.put(self._connected)

    def _ha_status_topic(self) -> str:
        return "%s/status" % self._settings["ha.prefix"]

    def _connected(self) -> None:
        for f in self._features:
            try:
                f.on_connect()
            except Exception as e:
                self.log("market: %s in %s.on_connect: %s" % (type(e).__name__, f.name, str(e)[:120]), level="WARNING")
        self._refresh("reconnect", False)

    def _on_message(self, client, userdata, msg):
        if msg.topic == self._ha_status_topic():
            if len(msg.payload) <= 16:
                self._worker.put(self._ha_status, bytes(msg.payload))
            return
        if msg.topic != self._topics.config or len(msg.payload) > CONFIG_MAX:
            return
        self._worker.put(self._apply_config, bytes(msg.payload))

    def _ha_status(self, payload: bytes) -> None:
        for f in self._features:
            hook = getattr(f, "on_ha_status", None)
            if hook is not None:
                hook(payload)

    # ── the panel's config ───────────────────────────────────────────────────
    def _apply_config(self, raw: bytes) -> None:
        if raw == self._last_config_raw:
            return
        self._last_config_raw = raw
        if not raw:                                        # the retained config was cleared: back to apps.yaml
            payload: object = {"v": SCHEMA_V}
        else:
            try:
                payload = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                self.log("market: config was not JSON", level="WARNING")
                payload = None
        new = config.from_panel(self._base, payload) if payload is not None else config.Settings(
            self._base.data, [{"key": "", "code": "TYPE", "msg": "config is not JSON"}], self._base.unknown)
        for e in new.errors:
            self.log("market: config %s: %s" % (e["key"], e["code"]), level="WARNING")
        old = self._settings
        changed = config.changed_keys(old.data, new.data)
        kinds = config.classes(changed)
        with self._lock:
            self._settings = new
            self._ctx.settings = new
        self.log("market: config %s: %s" % ("+".join(sorted(kinds)) or "identical", ", ".join(sorted(changed)[:8]) or "-"),
                 level="INFO" if kinds else "DEBUG")
        if not kinds:
            if new.errors and not old.errors:
                self._refresh("config-errors", False)      # the same settings, new refusals to report
            return
        # every class a save carries, not the heaviest alone (audit MINOR 5)
        if "ticker" in kinds:
            live_f = next((f for f in self._features if f.name == "live"), None)
            if live_f is not None:
                live_f.on_ticker(old.get("ticker"), new.get("ticker"))
        if "schedule" in kinds:
            pacer = getattr(self._provider, "pacer", None)
            if pacer is not None and "fetch.history_gap_s" in changed:
                pacer.gap = float(new["fetch.history_gap_s"])
            for f in self._features:
                f.reschedule()
        if kinds & {"light", "maths"}:
            for f in self._features:
                try:
                    f.on_config(new)
                except Exception as e:
                    self.log("market: %s in %s.on_config: %s" % (type(e).__name__, f.name, str(e)[:160]), level="WARNING")
        if "maths" not in kinds:
            return
        # the maths or the history: one recompute after a quiet 5 s, however many saves the portal makes
        hist = next((f for f in self._features if f.name == "history"), None)
        gone = [sym for sym in old.watched_symbols() if sym not in new.watched_symbols()]
        if gone:
            from .ledger import clear_symbol_leaves
            clear_symbol_leaves(self._publisher, gone, config.ALL_PRESETS)
        with self._debounce_lock:
            self._debounce_fetch = self._debounce_fetch or bool(hist and hist.missing_symbols())
            if self._debounce_handle is not None:
                try:
                    self.cancel_timer(self._debounce_handle)
                except Exception:
                    pass
            self._debounce_handle = self.run_in(self._debounced, DEBOUNCE_S)

    def _debounced(self, *args, **kwargs):
        with self._debounce_lock:
            self._debounce_handle = None
            fetch, self._debounce_fetch = self._debounce_fetch, False
        self._worker.put(self._refresh, "config", "missing" if fetch else False)

    # ── generations ──────────────────────────────────────────────────────────
    def request_refresh(self, reason: str, fetch=True) -> None:
        self._worker.put(self._refresh, reason, fetch)

    def request_keepalive(self) -> None:
        self._worker.put(self._keepalive)

    def _keepalive(self) -> None:
        """status alone, with the generation it belongs to (audit NIT 5)."""
        status = self.status_payload()
        status["gen"] = self._publisher._gen
        self._publisher.publish("status", status)

    def _refresh(self, reason: str = "manual", fetch=True) -> None:
        t0 = time.time()
        snap = self._publisher.new_snapshot(None)
        failed = []
        for f in self._features:
            try:
                f.contribute(snap, fetch)
            except Exception as e:
                failed.append(f.name)
                self.log("market: %s in %s.contribute: %s; its previous records stay" % (type(e).__name__, f.name, str(e)[:200]),
                         level="WARNING")
        snap.asof = (self._ctx.data.get("asof") or dt.date.min).isoformat() if self._ctx.data.get("asof") else None
        status = self.status_payload()
        if failed:
            status["failed"] = failed
        # a feature that failed keeps its previous records: nothing is cleared this generation (audit MINOR 8)
        sizes = self._publisher.publish_snapshot(snap, status, keep_previous=bool(failed))
        self._store.save_table("config_last", self._settings.to_payload())
        self.log("market: %s: %d payloads, largest %d B, as of %s, in %.1f s"
                 % (reason, len(sizes), max(sizes.values()) if sizes else 0, snap.asof, time.time() - t0))

    def status_payload(self) -> dict:
        s = self._settings
        asof = self._ctx.data.get("asof")
        symbols = self._store.status_symbols(s.status_symbols())
        out = {"v": SCHEMA_V, "asof": asof.isoformat() if asof else None, "fetched": None, "state": "ok", "err": "",
               "symbols": symbols, "ver": __version__}
        for f in self._features:
            try:
                out.update(f.status())
            except Exception:
                pass
        if asof is None:
            out["state"], out["err"] = "error", out.get("err") or "no data"
        elif M.weekdays_after(asof, dt.date.today()) > int(s["stale.history_days"]):
            out["state"] = "stale"
        elif out.get("err"):
            out["state"] = "ok"          # a rate-limited fetch with a good store is still ok, the err says why
        if s.errors:
            out["cfgErr"] = [{"key": e["key"], "code": e["code"]} for e in s.errors[:8]]
        return out

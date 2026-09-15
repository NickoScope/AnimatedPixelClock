"""The feature contract and the context they share.

A feature is a class registered in registry.FEATURES with start() / stop() /
on_config(settings), and two optional hooks: contribute(snapshot), which adds its
retained payloads to a generation (history, portfolio), and status(), whose
fields go into `status`. Each is independent: the app instantiates only those
apps.yaml lists, in `order`, and a missing one leaves the others working (a
portfolio without live quotes, a tape without a portfolio).

The Context is what they share: the settings, the store, the provider, the
publisher, a log, a small event bus (on/emit) for the loose coupling between
them (the history feature emits "series" when the store is fresh; the live
feature emits "quotes"; the tape reads both), a clock, and the app's scheduler
wrapped so every timer lands on the app's one worker thread.
"""
import threading
import time
from collections import defaultdict
from typing import Callable, Dict, List, Optional

from .config import Settings


class Context:
    def __init__(self, settings: Settings, store, provider, publisher, log: Callable[..., None],
                 scheduler=None, enqueue: Optional[Callable] = None, clock: Callable[[], float] = time.time, tz=None,
                 stopping: Optional[threading.Event] = None):
        self.settings, self.store, self.provider, self.publisher, self.log = settings, store, provider, publisher, log
        self._scheduler, self._enqueue, self.clock, self.tz = scheduler, enqueue, clock, tz
        self._listeners: Dict[str, List[Callable]] = defaultdict(list)
        self.data: dict = {}          # shared state by name: series, fx, days, ecb, result, quotes, tape
        self.stopping = stopping or threading.Event()     # set by terminate(): long loops end at their next step
        self.handles: List[object] = []

    # the bus
    def on(self, event: str, fn: Callable) -> None:
        self._listeners[event].append(fn)

    def emit(self, event: str, **kw) -> None:
        for fn in list(self._listeners.get(event, [])):
            try:
                fn(**kw)
            except Exception as e:      # one listener's failure is not another's
                self.log("market: %s in a %s listener" % (type(e).__name__, event), level="WARNING")

    # the worker
    def enqueue(self, fn: Callable, *args) -> None:
        if self._enqueue:
            self._enqueue(fn, *args)
        else:
            fn(*args)

    # the scheduler, every callback going through the worker; the handle comes back so a feature can cancel
    def every(self, fn: Callable, seconds: float, start="now"):
        if self._scheduler is None:
            return None
        h = self._scheduler.run_every(lambda *a, **k: self.enqueue(fn), start, seconds)
        self.handles.append(h)
        return h

    def daily(self, fn: Callable, hhmm: str):
        if self._scheduler is None:
            return None
        h = self._scheduler.run_daily(lambda *a, **k: self.enqueue(fn), hhmm + ":00")
        self.handles.append(h)
        return h

    def later(self, fn: Callable, seconds: float):
        if self._scheduler is None:
            return None
        h = self._scheduler.run_in(lambda *a, **k: self.enqueue(fn), seconds)
        self.handles.append(h)
        return h

    def cancel(self, handle) -> None:
        if self._scheduler is None or handle is None:
            return
        try:
            self._scheduler.cancel_timer(handle)
        except Exception as e:
            self.log("market: %s cancelling a timer" % type(e).__name__, level="DEBUG")
        if handle in self.handles:
            self.handles.remove(handle)

    def now(self) -> float:
        return self.clock()


class Feature:
    """The contract. Subclasses set `name` and `order` (lower runs first in a generation)."""
    name = ""
    order = 50

    def __init__(self, ctx: Context):
        self.ctx = ctx
        self.timers: List[object] = []

    def start(self) -> None:
        pass

    def stop(self) -> None:
        pass

    # timers a feature owns: kept so a schedule change can restart it cleanly
    def every(self, fn: Callable, seconds: float, start="now") -> None:
        self.timers.append(self.ctx.every(fn, seconds, start))

    def daily(self, fn: Callable, hhmm: str) -> None:
        self.timers.append(self.ctx.daily(fn, hhmm))

    def cancel_timers(self) -> None:
        for h in self.timers:
            self.ctx.cancel(h)
        self.timers = []

    def reschedule(self) -> None:
        """A live / fetch / stale setting changed: the timers again, nothing else (the panel's audit rule)."""
        self.cancel_timers()
        self.start()

    def on_config(self, settings: Settings) -> None:
        pass

    def on_connect(self) -> None:
        """The broker answered (on the worker, never on paho's thread): send what must follow a connection."""
        pass

    def contribute(self, snapshot, fetch: bool = True) -> None:
        pass

    def status(self) -> dict:
        return {}

    @property
    def settings(self) -> Settings:
        return self.ctx.settings

    def log(self, msg: str, level: str = "INFO") -> None:
        self.ctx.log("market/%s: %s" % (self.name, msg), level=level)

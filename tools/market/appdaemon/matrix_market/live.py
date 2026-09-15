"""Feature `live`: the session loop (docs/18, "Live loop").

Every refresh.live_s seconds (60: Yahoo's measured update period) while at least
one watched exchange is OPEN: one spark request for every symbol on the panel ->
`live`; one 1-minute chart request for the ticker the panel selected
(settings `ticker`) while its exchange is open -> `intraday/<sym>`, 128 slots
across the regular session, the slots not yet reached left at zero (the panel
draws the first `filled`). Outside sessions nothing is polled. A 429 arms the
provider's back-off and the loop skips until it lifts.

Which exchanges are open comes from the tape feature when it is wired (its
`tape_open` hook in the context); without it, from the weekday rule on each
symbol's stored session hours.
"""
import datetime as dt
from typing import Dict, List, Optional

from . import tape as tape_mod
from ._ref import market_ref as M
from .features import Feature
from .providers.base import NotFound, ProviderError, Quote, RateLimited
from .registry import FEATURES


def px(x: float) -> float:
    """A price with the decimals its size needs: 2 above 1 000, 3 above 100, 4 below (the bus budget)."""
    a = abs(x)
    return round(x, 2 if a >= 1000 else 3 if a >= 100 else 4)


LIVE_MAX_DELAY_S = 120          # docs/18 "Settings after the research": LIVE under 120 s, DELAYED from 120 s
DELAY_MAX_S = 65535             # the panel's decoder: delay_s is a u32 up to 65535
DAILY_PRICED = ("MUTUALFUND",)  # priced once a day: never in the spark poll (coordinator, 2026-09-15)


def feed_of(state: Optional[str], delay_s: Optional[int], stale_after_s: int, moved: bool = True) -> str:
    """The freshness label (docs/19 §6.3): LIVE under LIVE_MAX_DELAY_S, DELAYED from there, STALE when
    the quote has not moved for stale_after_s while the calendar says open, CLOSED otherwise."""
    if state not in ("OPEN", "STALE"):
        return "CLOSED"
    if state == "STALE" or (delay_s is not None and delay_s >= stale_after_s and not moved):
        return "STALE"
    if delay_s is not None and delay_s >= LIVE_MAX_DELAY_S:
        return "DELAYED"
    return "LIVE"


def live_payload(quotes: Dict[str, Quote], states: Dict[str, str], ts: int, stale_after_s: int = 1200,
                 previous: Optional[Dict[str, int]] = None) -> dict:
    """`live`: per symbol the last, the previous close, the day change, the exchange state, the quote's
    time, its delay (now - regularMarketTime) and feed label; the day high/low when they fit."""
    q: Dict[str, dict] = {}
    previous = previous or {}
    for sym, x in quotes.items():
        # asof is the quote's DATE, the exchange's local day (the panel's optDate: a string YYYY-MM-DD)
        off = int((x.meta or {}).get("gmtoffset") or 0)
        row = {"last": px(x.last)}
        if x.ts:
            row["asof"] = (dt.datetime.fromtimestamp(x.ts, dt.timezone.utc) + dt.timedelta(seconds=off)).date().isoformat()
        if x.prev is not None:
            row["prev"] = px(x.prev)
            row["day"] = round(x.last / x.prev - 1.0, 5) if x.prev else 0.0
        st = states.get(sym)
        if st:
            row["state"] = st
        delay = min(max(0, ts - x.ts), DELAY_MAX_S) if x.ts else DELAY_MAX_S   # always published; the panel reads a u32 up to 65535
        row["delay_s"] = delay
        moved = previous.get(sym) != x.ts if sym in previous else True
        row["feed"] = feed_of(st, delay, stale_after_s, moved)
        if x.day_hi is not None and x.day_lo is not None:
            row["hi"], row["lo"] = px(x.day_hi), px(x.day_lo)
        q[sym] = row
    return {"ts": ts, "q": q}


def intraday_payload(it, n: int = M.POINTS) -> Optional[dict]:
    """128 slots across the regular session: each the last 1-minute close inside it; trailing slots zero."""
    if not it.closes or it.session_end <= it.session_start:
        return None
    span = it.session_end - it.session_start
    vals: List[Optional[float]] = [None] * n
    for t, c in zip(it.times, it.closes):
        j = int((t - it.session_start) * n // span)
        if 0 <= j < n:
            vals[j] = c
    last_slot = max(0, min(n - 1, int((it.times[-1] - it.session_start) * n // span)))
    last: Optional[float] = None
    for j in range(last_slot + 1):
        if vals[j] is None:
            if last is not None:
                vals[j] = last             # a quiet minute carries the previous close
        else:
            last = vals[j]
    valid = [v for v in vals[:last_slot + 1] if v is not None]
    if not valid:
        return None
    lo, hi, b64 = M.pack_pts(valid)
    u = M.unpack_pts(b64) + [0] * (n - len(valid))
    import base64
    import struct
    b64 = base64.b64encode(struct.pack("<%dH" % n, *u)).decode()
    # the panel's shape (src/market/README.md): n is the slots filled so far, the line stays 128 slots
    return {"sym": it.sym, "ts": it.times[-1], "n": len(valid), "min": lo, "max": hi, "pts": b64}


@FEATURES.register("live")
class LiveFeature(Feature):
    name = "live"
    order = 30

    def __init__(self, ctx):
        super().__init__(ctx)
        self.quotes: Dict[str, Quote] = {}
        self.polls = 0
        self.last_error = ""

    def start(self) -> None:
        self.every(self.tick, int(self.settings["live.poll_s"]))
        self.last_ts: Dict[str, int] = getattr(self, "last_ts", {})

    def on_config(self, settings) -> None:
        pass

    def on_ticker(self, old: Optional[str], new: Optional[str]) -> None:
        """Only `ticker` changed: the old symbol's retained intraday line is cleared and the poll follows the
        new one at its next tick (no fetch, no recompute, no republish - the panel's audit rule)."""
        if old and old != new:
            self.ctx.publisher.clear("intraday/%s" % old)
        self.log("intraday follows %s" % (new or "nothing"))

    # ── which exchanges are open ─────────────────────────────────────────────
    def open_exchanges(self) -> List[str]:
        hook = self.ctx.data.get("tape_open")
        if hook:
            return hook()
        # no tape feature: the weekday rule on each stored symbol's hours
        now = dt.datetime.fromtimestamp(self.ctx.now(), dt.timezone.utc)
        out = []
        for sym, s in (self.ctx.data.get("series") or {}).items():
            if s.meta.get("currentTradingPeriod") and M.exchange_state(M.session_from_meta(s.meta), now) == "OPEN":
                out.append(tape_mod.exchange_of_meta(s.meta) or s.meta.get("exchangeName") or sym)
        return sorted(set(out))

    def symbol_open(self, sym: str, open_now: List[str]) -> bool:
        hook = self.ctx.data.get("tape_exchange_of")
        ex = hook(sym) if hook else None
        if ex is None:
            s = (self.ctx.data.get("series") or {}).get(sym)
            ex = tape_mod.exchange_of_meta(s.meta) if s is not None else None
            if ex is None and s is not None and s.meta.get("currentTradingPeriod"):
                now = dt.datetime.fromtimestamp(self.ctx.now(), dt.timezone.utc)
                return M.exchange_state(M.session_from_meta(s.meta), now) == "OPEN"
        return ex in open_now

    # ── the loop ─────────────────────────────────────────────────────────────
    def tick(self) -> None:
        provider = self.ctx.provider
        backoff = getattr(provider, "backoff", None)
        if backoff is not None and backoff.blocked():
            return
        open_now = self.open_exchanges()
        if not open_now:
            return
        series = self.ctx.data.get("series") or {}
        symbols = [sym for sym in self.settings.live_symbols()
                   if str((series.get(sym).meta if sym in series else {}).get("instrumentType", "")).upper() not in DAILY_PRICED]
        if not symbols:
            return
        try:
            quotes = provider.quotes(symbols)
        except RateLimited as e:
            self.last_error = "rate limited"
            self.log("429 on the live poll (%s)" % e, level="WARNING")
            return
        except ProviderError as e:
            self.last_error = type(e).__name__
            self.log("live poll: %s" % type(e).__name__, level="WARNING")
            return
        self.polls += 1
        self.last_error = ""
        self.quotes = quotes
        self.ctx.data["quotes"] = quotes
        self.ctx.emit("quotes", quotes=quotes)
        states = {sym: ("OPEN" if self.symbol_open(sym, open_now) else "CLOSED") for sym in quotes}
        tape_states = {r["n"]: r["s"] for r in (self.ctx.data.get("tape") or [])}
        hook = self.ctx.data.get("tape_exchange_of")
        for sym in states:
            ex = hook(sym) if hook else None
            if ex in tape_states:
                states[sym] = tape_states[ex]
        known = tape_mod.PANEL_STATES
        quotes = {sym: q for sym, q in quotes.items() if states.get(sym) in known}    # a quote with no known state is not sent (MINOR 3)
        prev_ts = getattr(self, "last_ts", {})
        self.ctx.publisher.publish("live", live_payload(quotes, states, int(self.ctx.now()),
                                                        int(self.settings["stale.session_min"]) * 60, prev_ts))
        self.last_ts = {sym: x.ts for sym, x in quotes.items()}
        sym = self.settings.get("ticker")
        if sym and self.symbol_open(sym, open_now):
            try:
                it = provider.intraday(sym)
            except RateLimited as e:
                self.log("429 on the intraday line (%s)" % e, level="WARNING")
                return
            except (NotFound, ProviderError) as e:
                self.log("intraday %s: %s" % (sym, type(e).__name__), level="WARNING")
                return
            p = intraday_payload(it)
            if p is not None:
                self.ctx.publisher.publish("intraday/%s" % sym, p)

    def status(self) -> dict:
        out: dict = {}
        if self.last_error:
            out["liveErr"] = self.last_error
        return out

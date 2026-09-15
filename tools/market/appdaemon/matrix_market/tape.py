"""Feature `tape`: the exchange states for the tape, and the "is this symbol's exchange open" question.

Exchanges are a table (registry.EXCHANGES): the tape's name, the exchange_calendars
code, the Yahoo `exchangeName` codes that map a stored symbol's meta to it, and
the time zone. Three sources for a state, best first:

1. Yahoo's `currentTradingPeriod` from the freshest stored or live meta of a
   symbol on that exchange, used with its absolute timestamps while they are
   today's session (a holiday shows as "no session today" because Yahoo's
   periods then point at the next session);
2. `exchange_calendars` (sessions, opens, closes; holidays) when installed,
   with PRE / POST hours from the Yahoo meta;
3. the weekday rule on the meta's hours (market_ref.exchange_state): no holidays.

A session the source calls OPEN while the freshest quote on that exchange has
not moved for STALE_AFTER_S is STALE, not OPEN (docs/18). `t` is the next
change in the panel's local time. Published on every state change and every
15 minutes.
"""
import datetime as dt
from typing import Dict, List, Optional, Tuple

from ._ref import market_ref as M
from .features import Feature
from .registry import EXCHANGES, FEATURES

STALE_AFTER_S = 20 * 60
PANEL_STATES = ("OPEN", "PRE", "POST", "CLOSED", "STALE")      # market_model.cpp kExStateKeys without "--"
REPUBLISH_S = 15 * 60
CHECK_S = 60

# Yahoo exchangeName codes seen in the saved samples: NMS (AAPL), PCX (VOO), SNP (^GSPC), NIM (^IXIC),
# PAR (^FCHI), GER (^GDAXI), AMS (IWDA.AS). The others are Yahoo's usual codes, not verified here; a
# code that never matches only means no cross-check for that exchange, never a wrong state.
EXCHANGES.register("NYSE", {"cal": "XNYS", "yahoo": ("NYQ", "PCX", "NYS", "SNP", "ASE", "BTS"), "tz": "America/New_York"})
EXCHANGES.register("NASDAQ", {"cal": "XNAS", "yahoo": ("NMS", "NGM", "NCM", "NIM", "NAS"), "tz": "America/New_York"})
EXCHANGES.register("LSE", {"cal": "XLON", "yahoo": ("LSE", "IOB"), "tz": "Europe/London"})
EXCHANGES.register("XETRA", {"cal": "XETR", "yahoo": ("GER", "FRA", "ETR"), "tz": "Europe/Berlin"})
EXCHANGES.register("EURONEXT", {"cal": "XPAR", "yahoo": ("PAR", "AMS", "BRU", "LIS", "ENX"), "tz": "Europe/Paris"})
EXCHANGES.register("TOKYO", {"cal": "XTKS", "yahoo": ("JPX", "TYO", "OSA"), "tz": "Asia/Tokyo"})


def exchange_of_meta(meta: dict) -> Optional[str]:
    code = str(meta.get("exchangeName") or "")
    for name, ex in EXCHANGES.items():
        if code in ex["yahoo"]:
            return name
    return None


def _periods(meta: dict) -> Optional[List[Tuple[int, int, str]]]:
    """(start, end, state) from currentTradingPeriod, absolute epoch seconds, ordered."""
    ctp = meta.get("currentTradingPeriod") or {}
    out = []
    for key, st in (("pre", "PRE"), ("regular", "OPEN"), ("post", "POST")):
        p = ctp.get(key) or {}
        a, b = p.get("start"), p.get("end")
        if a is not None and b is not None and b > a:
            out.append((int(a), int(b), st))
    return sorted(out) or None


def _local_day(ts: int, meta: dict) -> dt.date:
    return (dt.datetime.fromtimestamp(ts, dt.timezone.utc) + dt.timedelta(seconds=int(meta.get("gmtoffset") or 0))).date()


def state_from_meta(meta: dict, now: dt.datetime) -> Optional[Tuple[str, Optional[dt.datetime]]]:
    """Source 1: the meta's absolute periods, when they describe the exchange's current local day."""
    periods = _periods(meta)
    if not periods:
        return None
    now_ts = int(now.timestamp())
    today = _local_day(now_ts, meta)
    period_day = _local_day(periods[0][0], meta)
    if period_day > today:                          # Yahoo points at the next session: no session today
        nxt = dt.datetime.fromtimestamp(periods[0][0], dt.timezone.utc)
        return "CLOSED", nxt
    if period_day < today:
        return None                                 # yesterday's periods: not usable
    for a, b, st in periods:
        if a <= now_ts < b:
            return st, dt.datetime.fromtimestamp(b, dt.timezone.utc)
    nxt = next((a for a, b, st in periods if a > now_ts), None)
    return "CLOSED", (dt.datetime.fromtimestamp(nxt, dt.timezone.utc) if nxt else None)


def state_from_calendar(ex: dict, meta: Optional[dict], now: dt.datetime) -> Optional[Tuple[str, Optional[dt.datetime]]]:
    """Source 2: exchange_calendars, when importable. PRE/POST from the meta's hours when they are known."""
    try:
        import exchange_calendars as xcals
        import pandas as pd
    except ImportError:
        return None
    try:
        cal = xcals.get_calendar(ex["cal"])
        ts = pd.Timestamp(now).tz_convert("UTC") if now.tzinfo else pd.Timestamp(now, tz="UTC")
        if cal.is_open_on_minute(ts, ignore_breaks=True):
            return "OPEN", cal.next_close(ts).to_pydatetime()
        nxt_open = cal.next_open(ts).to_pydatetime()
        if meta and _periods(meta):
            ses = M.session_from_meta(meta)
            local = now + dt.timedelta(seconds=ses["gmtoffset"])
            t = local.hour * 3600 + local.minute * 60 + local.second
            day_is_session = cal.is_session(pd.Timestamp(local.date()))
            if day_is_session:
                for k, st in (("pre", "PRE"), ("post", "POST")):
                    a, b = ses[k]
                    if a < b and a <= t < b:
                        return st, now + dt.timedelta(seconds=b - t)
        return "CLOSED", nxt_open
    except Exception:
        return None


def state_from_hours(meta: dict, now: dt.datetime) -> Tuple[str, Optional[dt.datetime]]:
    """Source 3: the weekday rule on the meta's hours (no holidays)."""
    ses = M.session_from_meta(meta)
    st = M.exchange_state(ses, now)
    local = now + dt.timedelta(seconds=ses["gmtoffset"])
    t = local.hour * 3600 + local.minute * 60 + local.second
    bounds = sorted({x for k in ("pre", "regular", "post") for x in ses[k] if ses[k][0] < ses[k][1]})
    nxt = next((b for b in bounds if b > t), None)
    if nxt is None:                                 # tomorrow's first boundary (weekends skipped)
        days = 1
        while (local + dt.timedelta(days=days)).weekday() >= 5:
            days += 1
        nxt_dt = now + dt.timedelta(days=days, seconds=(bounds[0] if bounds else 0) - t)
    else:
        nxt_dt = now + dt.timedelta(seconds=nxt - t)
    if st == "CLOSED" and local.weekday() >= 5:
        days = 7 - local.weekday()
        nxt_dt = now + dt.timedelta(days=days, seconds=(bounds[0] if bounds else 0) - t)
    return st, nxt_dt


def exchange_state(name: str, metas: List[dict], now: dt.datetime, last_quote_ts: Optional[int] = None,
                   stale_after_s: int = STALE_AFTER_S) -> Tuple[str, Optional[dt.datetime]]:
    """The state of one exchange from the freshest of `metas` (its symbols'), the three sources in order."""
    ex = EXCHANGES.get(name)
    metas = sorted([m for m in metas if m], key=lambda m: int(m.get("regularMarketTime") or 0), reverse=True)
    meta = metas[0] if metas else None
    result = state_from_meta(meta, now) if meta else None
    if result is None:
        result = state_from_calendar(ex, meta, now)
    if result is None:
        result = state_from_hours(meta, now) if meta and meta.get("currentTradingPeriod") else ("--", None)
    st, nxt = result
    if st == "OPEN" and last_quote_ts is not None and now.timestamp() - last_quote_ts > stale_after_s:
        st = "STALE"
    return st, nxt


@FEATURES.register("tape")
class TapeFeature(Feature):
    name = "tape"
    order = 40

    def __init__(self, ctx):
        super().__init__(ctx)
        self.states: Dict[str, str] = {}
        self.published_at = 0.0
        self.ctx.on("series", self._on_series)
        self.ctx.on("quotes", self._on_quotes)
        self.metas: Dict[str, dict] = {}          # symbol -> the freshest meta known
        self.quote_ts: Dict[str, int] = {}        # symbol -> regularMarketTime of the last quote

    def start(self) -> None:
        self.every(self.tick, CHECK_S)
        self.ctx.data["tape_open"] = self.open_exchanges
        self.ctx.data["tape_exchange_of"] = self.exchange_of_symbol

    def on_config(self, settings) -> None:
        self.tick(force=True)

    def _on_series(self, series=None, **_) -> None:
        for sym, s in (series or {}).items():
            if s.meta and s.meta.get("currentTradingPeriod"):
                self.metas[sym] = s.meta
        self.tick(force=True)

    def _on_quotes(self, quotes=None, **_) -> None:
        for sym, q in (quotes or {}).items():
            if q.meta and q.meta.get("currentTradingPeriod"):
                self.metas[sym] = q.meta
            if q.ts:
                self.quote_ts[sym] = q.ts
        self.tick()

    # ── the questions the others ask ─────────────────────────────────────────
    def exchange_of_symbol(self, sym: str) -> Optional[str]:
        meta = self.metas.get(sym)
        return exchange_of_meta(meta) if meta else None

    def open_exchanges(self) -> List[str]:
        return [n for n, st in self.states.items() if st in ("OPEN", "STALE")]

    def compute(self, now: Optional[dt.datetime] = None) -> List[dict]:
        now = now or dt.datetime.fromtimestamp(self.ctx.now(), dt.timezone.utc)
        tz = None
        try:
            from zoneinfo import ZoneInfo
            tz = ZoneInfo(self.settings.get("display.tz") or "UTC") if self.settings.get("display.tz") else self.ctx.tz
        except Exception:
            tz = None
        rows = []
        for name in self.settings.get("tape.exchanges") or []:
            if name not in EXCHANGES:
                continue
            metas = [m for sym, m in self.metas.items() if exchange_of_meta(m) == name]
            qts = [self.quote_ts[sym] for sym, m in self.metas.items() if exchange_of_meta(m) == name and sym in self.quote_ts]
            st, nxt = exchange_state(name, metas, now, max(qts) if qts else None, int(self.settings.get("stale.session_min") or 20) * 60)
            if st not in PANEL_STATES:
                continue                   # unknown ("--" is the panel's own word for none; it refuses it from the app, audit MINOR 3)
            row = {"n": name, "s": st}
            if nxt is not None:
                local = nxt.astimezone(tz) if tz else nxt
                row["t"] = local.strftime("%H:%M")
            rows.append(row)
        return rows

    def tick(self, force: bool = False) -> None:
        rows = self.compute()
        states = {r["n"]: r["s"] for r in rows}
        changed = states != self.states
        self.states = states
        self.ctx.data["tape"] = rows
        if not rows:
            return
        if force or changed or self.ctx.now() - self.published_at >= REPUBLISH_S:
            self.ctx.publisher.publish("tape", {"ts": int(self.ctx.now()), "x": rows})
            self.published_at = self.ctx.now()
            if changed:
                self.ctx.emit("tape", states=states)

    def status(self) -> dict:
        return {}

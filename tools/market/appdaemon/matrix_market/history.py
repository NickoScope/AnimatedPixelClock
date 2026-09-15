"""Feature `history`: the daily fetch, the store, the ECB table, the TER, and the index/ticker payloads.

Order 10: it runs first in a generation and leaves the loaded series, the FX
table and the as-of in the context for the others (`series`, `fx`, `asof`, `ecb`).

The fetch (docs/18, "Fetch" and "The previews"): once a day at refresh.daily_at
and on a config change; one symbol per refresh.history_gap_s (the provider's
pacer); a 429 stops the run - the symbols not reached keep their store and the
status says so; a failed symbol keeps its store and is marked. Daily bars from
2000-01-01 (or the inception when earlier) on the first fetch, then from 60 days
before the stored last bar so the store's 30-bar corporate-action check has an
overlap. The TER once per refresh.ter_days per position. The ECB 1999-2003
table once, from its own HTTP door (not Yahoo's back-off).
"""
import datetime as dt
from typing import Dict, List, Optional

from . import windows
from ._ref import market_ref as M
from .features import Feature
from .providers import ecb as ecb_mod
from .providers.base import NotFound, ProviderError, RateLimited
from .providers.httpio import Http
from .registry import FEATURES

Date = dt.date
HISTORY_FROM = Date(2000, 1, 1)
OVERLAP_DAYS = 60
RETRY_MARGIN_S = 5            # after the back-off lifts
RETRY_DEFAULT_S = 300         # a provider without a Backoff


def open_session_day(meta: dict, now_utc: dt.datetime) -> Optional[Date]:
    """The exchange-local date whose regular session is not over at `now_utc`, or None.

    Yahoo appends a bar for the current session to a daily answer and moves its close
    until the session ends. From meta.currentTradingPeriod.regular: while now is before
    the session's end and that end falls on the exchange's local today, today's bar is
    unfinished. When the periods point at another day (the evening after a close, a
    weekend, a holiday) today's session, if any, is over. Without session hours, a bar
    dated the exchange's local today is taken as unfinished until that day ends."""
    off = int((meta or {}).get("gmtoffset") or 0)
    today = (now_utc + dt.timedelta(seconds=off)).date()
    reg = ((meta or {}).get("currentTradingPeriod") or {}).get("regular") or {}
    end = reg.get("end")
    if end is None:
        return today
    end_off = int(reg.get("gmtoffset", off) or 0)
    end_day = (dt.datetime.fromtimestamp(int(end), dt.timezone.utc) + dt.timedelta(seconds=end_off)).date()
    if now_utc.timestamp() < int(end) and end_day == today:
        return today
    return None


def closed_bars(s: "M.Series", now_utc: dt.datetime) -> "M.Series":
    """`s` without the bars of a session still open at `now_utc` (audit B1: never stored, never compared)."""
    day = open_session_day(s.meta, now_utc)
    if day is None or not s.dates or s.dates[-1] < day:
        return s
    n = M.bisect.bisect_left(s.dates, day)
    adj = s.adj[:n] if s.adj else s.adj
    return M.Series(s.sym, s.currency, s.dates[:n], s.closes[:n], [x for x in s.dividends if x[0] < day], s.ter, s.meta, adj)


def index_payload(s: "M.Series", preset: str, asof: Date, inception: Date, name: Optional[str]) -> Optional[dict]:
    """market_ref's index/ticker payload plus the extra metrics of windows.METRICS."""
    from .ledger import base_index_payload
    p = base_index_payload(s, preset, asof, inception, name)
    if p is None or not windows.extra_metric_names():
        return p
    end = min(asof, s.last)
    wd, wv = windows.series_window(s.dates, s.closes, windows.start_of(preset, end, inception, s.dates), end)
    ctx = {"preset": preset, "series": s}
    for n in windows.extra_metric_names():
        x = windows.METRICS.get(n)(wd, wv, ctx)
        p[n] = round(x, 6) if isinstance(x, float) else x
    return p


@FEATURES.register("history")
class HistoryFeature(Feature):
    name = "history"
    order = 10

    def __init__(self, ctx):
        super().__init__(ctx)
        self.last_fetch: Optional[str] = None
        self.last_error = ""
        self.rate_limited_at: Optional[float] = None
        self.ecb_http = Http(user_agent=ctx.data.get("user_agent"), log=ctx.log)
        self.retry_pending = False
        self.progress = None                 # the CLI hooks a printer here: progress(sym, how, detail)

    # ── scheduling ───────────────────────────────────────────────────────────
    def start(self) -> None:
        s = self.settings
        self.daily(lambda: self.ctx.data["refresh"]("daily", True), s["fetch.daily_at"])
        # run_every's start accepts "now", "immediate", a parsed time, a time or a datetime (AD_API_REFERENCE);
        # "now" fires first after one interval, which is the keepalive's intent
        # the keepalive republishes status alone (docs/18: "status only when nothing changed"; audit NIT 5)
        self.every(lambda: (self.ctx.data.get("keepalive") or (lambda: None))(), int(s["fetch.keepalive_h"]) * 3600, start="now")

    def on_config(self, settings) -> None:
        pass            # the app triggers a refresh; new symbols are fetched there

    def retry_later(self) -> None:
        """A 429 stopped a fetch: one retry when the provider's block lifts, never two pending (audit B2).
        With Backoff's cap the retry comes within the hour, so a refused setting cannot silence the app."""
        refresh = self.ctx.data.get("refresh")
        if self.retry_pending or refresh is None:
            return
        backoff = getattr(self.ctx.provider, "backoff", None)
        wait = (backoff.remaining() if backoff is not None else RETRY_DEFAULT_S) + RETRY_MARGIN_S
        self.retry_pending = True

        def retry():
            self.retry_pending = False
            refresh("retry", "stale")
        self.ctx.later(retry, wait)
        self.log("a fetch retry in %d s" % wait)

    # ── the fetch ────────────────────────────────────────────────────────────
    def fetch_symbols(self, symbols: List[str]) -> Dict[str, str]:
        """Fetch `symbols` in order, paced by the provider. Returns {sym: how}; stops on 429."""
        store, provider = self.ctx.store, self.ctx.provider
        inception = Date.fromisoformat(self.settings["portfolio.inception"])
        full_start = min(HISTORY_FROM, inception)
        now = dt.datetime.fromtimestamp(self.ctx.now(), dt.timezone.utc)
        out: Dict[str, str] = {}
        for sym in symbols:
            if self.ctx.stopping.is_set():       # terminate(): stop between symbols (audit MINOR 7)
                break
            stored = store.load(sym)
            # a file from before the adjclose column (v0.1) is fetched whole again, once
            whole = stored is None or (stored.adj is None and not sym.endswith("=X"))
            start = full_start if whole else stored.last - dt.timedelta(days=OVERLAP_DAYS)
            try:
                fresh = closed_bars(provider.history(sym, start), now)
                how, added = ("empty", 0) if not fresh.dates else store.save(sym, fresh, provider.name, full=whole)
                if how == "mismatch":
                    # past closes changed (a split, a restatement): the whole history again, and only that replaces the file
                    self.log("%s: past closes changed; fetching the whole history again" % sym)
                    fresh = closed_bars(provider.history(sym, full_start), now)
                    how, added = ("empty", 0) if not fresh.dates else store.save(sym, fresh, provider.name, full=True)
                if how in ("short", "empty"):
                    store.mark_error(sym, "an answer without the stored history (%s); store kept" % how)
                    self.log("%s: %s answer, the store is kept" % (sym, how), level="WARNING")
            except RateLimited as e:
                self.rate_limited_at = self.ctx.now()
                self.last_error = "rate limited"
                self.log("429 on %s; the rest keep their store (%s)" % (sym, e), level="WARNING")
                out[sym] = "429"
                if self.progress:
                    self.progress(sym, "429", str(e))
                self.retry_later()
                break
            except Exception as e:           # ProviderError, or anything else: this symbol only (audit MINOR 8)
                why = "%s: %s" % (type(e).__name__, str(e)[:120])
                store.mark_error(sym, why)
                self.log("%s: %s; its store is kept" % (sym, why), level="WARNING")
                out[sym] = "error"
                if self.progress:
                    self.progress(sym, "error", why)
                continue
            out[sym] = how
            if self.progress:
                self.progress(sym, how, "%d bars %s..%s, +%d, %d dividends, %s"
                              % (len(fresh.dates), fresh.first, fresh.last, added, len(fresh.dividends), fresh.currency))
        return out

    def fetch_ter(self, symbols: List[str], force: bool = False) -> Dict[str, str]:
        """The expense ratio of the positions whose TER is older than refresh.ter_days."""
        store, provider = self.ctx.store, self.ctx.provider
        max_age = float(self.settings["fetch.ter_days"])
        out: Dict[str, str] = {}
        for sym in symbols:
            age = store.ter_age_days(sym)
            if not force and age is not None and age < max_age:
                continue
            try:
                info = provider.fund_info(sym)
            except RateLimited as e:
                self.rate_limited_at = self.ctx.now()
                self.log("429 reading the TER of %s (%s)" % (sym, e), level="WARNING")
                out[sym] = "429"
                self.retry_later()
                break
            except NotFound:
                store.set_ter(sym, None, "none")
                out[sym] = "none"
                continue
            except Exception as e:
                self.log("%s: TER %s: %s" % (sym, type(e).__name__, str(e)[:120]), level="WARNING")
                out[sym] = "error"
                continue
            store.set_ter(sym, info.ter, info.source or "none")
            out[sym] = "ok" if info.ter is not None else "none"
            if self.progress:
                self.progress(sym, "ter", "%s (%s)" % (info.ter, info.source))
        return out

    def fetch_ecb(self) -> Optional[ecb_mod.EcbRates]:
        try:
            rates = ecb_mod.ensure(self.ctx.store, self.ecb_http)
        except ProviderError as e:
            self.log("ECB: %s" % type(e).__name__, level="WARNING")
            rates = ecb_mod.EcbRates.from_store(self.ctx.store)
        self.ctx.data["ecb"] = rates
        return rates

    def fetch_hicp(self) -> Optional[ecb_mod.HicpIndex]:
        """The euro-area HICP, only when real terms or indexed contributions are on."""
        s = self.settings
        if not (s["returns.real"] or s["contrib.index_inflation"]):
            self.ctx.data["hicp"] = ecb_mod.HicpIndex.from_store(self.ctx.store)
            return self.ctx.data["hicp"]
        try:
            idx = ecb_mod.ensure_hicp(self.ctx.store, self.ecb_http)
        except ProviderError as e:
            self.log("ECB HICP: %s" % type(e).__name__, level="WARNING")
            idx = ecb_mod.HicpIndex.from_store(self.ctx.store)
        self.ctx.data["hicp"] = idx
        return idx

    def fetch_order(self) -> List[str]:
        """FX first (every conversion needs it), then indices, positions, tickers, proxies and the benchmarks."""
        s = self.settings
        order = (s.fx_symbols() + [i["sym"] for i in s.indices()] + [p["sym"] for p in s.positions()]
                 + [t["sym"] for t in s.tickers()] + list(s.proxies().values()) + s.bench_symbols())
        seen, out = set(), []
        for sym in order:
            if sym not in seen:
                seen.add(sym)
                out.append(sym)
        return out

    def daily_boundary(self, now: Optional[dt.datetime] = None) -> dt.datetime:
        """The most recent fetch.daily_at that has passed, in the app's time zone."""
        now = now or dt.datetime.fromtimestamp(self.ctx.now(), dt.timezone.utc)
        local = now.astimezone(self.ctx.tz or dt.timezone.utc)
        hh, mm = (int(x) for x in self.settings["fetch.daily_at"].split(":"))
        boundary = local.replace(hour=hh, minute=mm, second=0, microsecond=0)
        return boundary - dt.timedelta(days=1) if boundary > local else boundary

    @staticmethod
    def _utc(stamp: Optional[str]) -> Optional[dt.datetime]:
        try:
            return dt.datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc) if stamp else None
        except ValueError:
            return None

    def _failed_since(self, sym: str, boundary: dt.datetime) -> bool:
        """This symbol's fetch already failed after the boundary: it waits for the next daily run rather than being asked
        again at every start, retry and config change (a typo, a delisting). A 429 is not a failure here: the symbols a
        429 stopped carry no errAt, so the retry reaches them."""
        at = self._utc((self.ctx.store.manifest["symbols"].get(sym) or {}).get("errAt"))
        return at is not None and at >= boundary

    def stale_symbols(self, now: Optional[dt.datetime] = None) -> List[str]:
        """The watched symbols with no stored file, or last fetched before the most recent fetch.daily_at in the
        app's time zone (the manifest's `fetched` is UTC; audit MINOR 9: a local date against a UTC stamp refetched
        everything, and symbols no longer watched counted), less those that failed since then. What a start or a
        retry fetches."""
        boundary = self.daily_boundary(now)
        out = []
        for sym in self.fetch_order():
            if self._failed_since(sym, boundary):
                continue
            at = self._utc((self.ctx.store.manifest["symbols"].get(sym) or {}).get("fetched"))
            if self.ctx.store.load_raw(sym) is None or at is None or at < boundary:
                out.append(sym)
        return out

    def fetch_all(self, symbols: Optional[List[str]] = None) -> Dict[str, str]:
        """`symbols` (default: every watched one, in fetch_order) paced by the provider; then the ECB tables and
        the TERs whose age calls for it."""
        s = self.settings
        symbols = self.fetch_order() if symbols is None else list(symbols)
        result = self.fetch_symbols(symbols)
        self.fetch_ecb()
        self.fetch_hicp()
        if "429" not in result.values():
            result.update({"TER:" + k: v for k, v in self.fetch_ter([p["sym"] for p in s.positions()]).items()})
        self.last_fetch = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
        if "429" not in result.values():
            self.last_error = ""
        return result

    def missing_symbols(self) -> List[str]:
        """The watched symbols with no stored file, less those that failed since the last daily fetch time."""
        boundary = self.daily_boundary()
        return [sym for sym in self.settings.watched_symbols()
                if self.ctx.store.load_raw(sym) is None and not self._failed_since(sym, boundary)]

    # ── the generation ───────────────────────────────────────────────────────
    def load(self) -> Dict[str, "M.Series"]:
        s = self.settings
        series = self.ctx.store.load_all(s.watched_symbols())
        ecb = self.ctx.data.get("ecb") or ecb_mod.EcbRates.from_store(self.ctx.store)
        self.ctx.data["ecb"] = ecb
        if "hicp" not in self.ctx.data:
            self.ctx.data["hicp"] = ecb_mod.HicpIndex.from_store(self.ctx.store)
        from .ledger import fx_table
        self.ctx.data["series"] = series
        self.ctx.data["fx"] = fx_table(series, ecb.rate if ecb else None)
        traded = [x for sym, x in series.items() if sym not in s.fx_symbols() and x.dates]
        self.ctx.data["asof"] = M.as_of(traded) if traded else None
        self.ctx.emit("series", series=series)
        return series

    def contribute(self, snapshot, fetch=True) -> None:
        """`fetch`: True or "all" (the daily run), "stale" (a start, a retry), "missing" (a config change), False."""
        if fetch:
            symbols = self.stale_symbols() if fetch == "stale" else self.missing_symbols() if fetch == "missing" else None
            if symbols is None or symbols:
                self.fetch_all(symbols)
        series = self.load()
        asof = self.ctx.data.get("asof")
        if asof is None:
            return
        s = self.settings
        inception = Date.fromisoformat(s["portfolio.inception"])
        for kind, items in (("index", s.indices()), ("ticker", s.tickers())):
            for it in items:
                x = series.get(it["sym"])
                if x is None or not x.dates:
                    continue
                for preset in s.presets():
                    p = index_payload(x, preset, asof, inception, it.get("name"))
                    if p is not None:
                        snapshot.add("%s/%s/%s" % (kind, it["sym"], preset), p)

    def status(self) -> dict:
        out: dict = {}
        if self.last_fetch:
            out["fetched"] = self.last_fetch
        backoff = getattr(self.ctx.provider, "backoff", None)
        if backoff is not None and backoff.blocked():
            out["backoff"] = int(backoff.remaining())
        if self.last_error:
            out["err"] = self.last_error
        return out

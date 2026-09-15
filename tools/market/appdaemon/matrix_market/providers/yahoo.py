"""Yahoo Finance, the design's source (docs/18, "Data source"), behind the Provider interface.

  history      GET query1 /v8/finance/chart/<sym>?period1&period2&interval=1d&events=div,split
               parsed by market_ref.parse_yahoo_chart (the same parser as the saved samples)
  quotes       GET query1 /v7/finance/spark?symbols=A,B,..&range=1d&interval=5m - one call for
               every symbol on the panel (probed 2026-09-15: seven symbols, 15 KB, 0.35 s)
  intraday     GET /v8/finance/chart/<sym>?range=1d&interval=1m - the selected ticker's line
  fund_info    the expense ratio: yfinance's funds_data.fund_operations when the package is
               installed (its crumb handling), else GET query2 /v1/test/getcrumb then
               /v10/finance/quoteSummary/<sym>?modules=fundProfile,price&crumb=..  (probed
               2026-09-15 without a cookie; parse_yahoo_ter reads the saved answers)
  session_meta the last chart answer's meta for the symbol (currentTradingPeriod, gmtoffset)

Pacing is the provider's own: every request waits on one Pacer (the history gap
from the settings, 10 s by default), and a 429 arms one Backoff (5, 15, 60 min);
while it is armed every call raises RateLimited at once without touching the
network. Rate limits are not published by Yahoo (docs/18); the 429 episode of
2026-09-15 09:30 is why this is here.

yfinance's role is deliberately small: its DataFrame path for history would
give the same closes as the v8 answer it downloads (Close with auto_adjust=False
is Yahoo's split-adjusted close), so the app takes the v8 JSON directly and
keeps one parser for the samples, the store and the tests. The yfinance path
for the expense ratio is used when the import succeeds and read by label
("Expense Ratio" in fund_operations' index); it is not exercised on the Mac,
where the package is not installed, and the direct path stands in.
"""
import datetime as dt
import time
from typing import Dict, Optional, Sequence

from .._ref import market_ref as M
from ..registry import PROVIDERS
from .base import Backoff, FundInfo, Intraday, NotFound, Pacer, Provider, ProviderError, Quote, RateLimited
from .httpio import DEFAULT_UA, Http

Date = dt.date
CHART = "https://query1.finance.yahoo.com/v8/finance/chart/%s"
SPARK = "https://query1.finance.yahoo.com/v7/finance/spark"
CRUMB = "https://query2.finance.yahoo.com/v1/test/getcrumb"
SUMMARY = "https://query2.finance.yahoo.com/v10/finance/quoteSummary/%s"
HISTORY_FROM = Date(2000, 1, 1)         # the design: daily bars from 2000 (or the listing)


def _epoch(d: Date) -> int:
    return int(dt.datetime(d.year, d.month, d.day, tzinfo=dt.timezone.utc).timestamp())


def _quote(sym: str, r: dict) -> Optional[Quote]:
    """One chart/spark result -> Quote. `r` has meta, timestamp, indicators.quote[0].close."""
    meta = r.get("meta") or {}
    closes = ((r.get("indicators") or {}).get("quote") or [{}])[0].get("close") or []
    last_bar = next((c for c in reversed(closes) if c is not None), None)
    last = meta.get("regularMarketPrice", last_bar)
    if last is None:
        return None
    prev = meta.get("chartPreviousClose", meta.get("previousClose"))
    ts = int(meta.get("regularMarketTime") or (r.get("timestamp") or [0])[-1] or 0)
    return Quote(sym, float(last), float(prev) if prev is not None else None, ts, meta.get("currency"),
                 meta.get("regularMarketDayHigh"), meta.get("regularMarketDayLow"), meta)


@PROVIDERS.register("yahoo")
class YahooProvider(Provider):
    name = "yahoo"

    def __init__(self, http: Optional[Http] = None, gap_s: float = 10.0, backoff_steps: Sequence[float] = (300, 900, 3600),
                 use_yfinance: str = "auto", sleep=time.sleep, user_agent: Optional[str] = None, log=None, **_):
        self.http = http or Http(user_agent=user_agent or DEFAULT_UA, log=log)
        self.pacer = Pacer(gap_s, sleep)
        self.backoff = Backoff(backoff_steps)
        self.use_yfinance = use_yfinance
        self._meta: Dict[str, dict] = {}
        self._crumb: Optional[str] = None
        self.requests = 0

    # ── the one door ─────────────────────────────────────────────────────────
    def _get(self, url: str, params: Optional[dict] = None) -> dict:
        if self.backoff.blocked():
            raise RateLimited("backing off for %d s more" % self.backoff.remaining())
        self.pacer.wait()
        self.requests += 1
        try:
            d = self.http.get_json(url, params)
        except RateLimited as e:
            wait = self.backoff.hit(e.retry_after)
            raise RateLimited("%s; backing off %d s" % (e, wait), e.retry_after) from None
        self.backoff.ok()
        return d

    @staticmethod
    def _result(d: dict, sym: str) -> dict:
        chart = d.get("chart") or {}
        if chart.get("error"):
            code = (chart["error"] or {}).get("code", "error")
            if code in ("Not Found", "not_found") or "No data found" in str(chart["error"].get("description", "")):
                raise NotFound("%s: %s" % (sym, code))
            raise ProviderError("%s: chart %s" % (sym, code))
        results = chart.get("result") or []
        if not results:
            raise NotFound("%s: an answer without a result" % sym)
        return results[0]

    # ── history ──────────────────────────────────────────────────────────────
    def raw_chart(self, symbol: str, period1: Optional[int] = None, period2: Optional[int] = None,
                  interval: str = "1d", events: str = "div,split", range: Optional[str] = None) -> dict:
        params: dict = {"interval": interval, "events": events}
        if range:
            params["range"] = range
        else:
            params["period1"] = period1 if period1 is not None else _epoch(HISTORY_FROM)
            params["period2"] = period2 if period2 is not None else int(time.time())
        d = self._get(CHART % symbol, params)
        r = self._result(d, symbol)
        if r.get("meta"):
            self._meta[symbol] = r["meta"]
        return d

    def history(self, symbol: str, start: Date, end: Optional[Date] = None, interval: str = "1d") -> "M.Series":
        d = self.raw_chart(symbol, _epoch(start), _epoch(end + dt.timedelta(days=1)) if end else None, interval)
        r = self._result(d, symbol)
        if not r.get("timestamp"):
            raise NotFound("%s: an answer without bars" % symbol)
        try:
            return M.parse_yahoo_chart(d)
        except (KeyError, TypeError, ValueError, IndexError, AttributeError) as e:   # a shape this parser does not know
            raise ProviderError("%s: chart answer not parsed (%s: %s)" % (symbol, type(e).__name__, str(e)[:60])) from None

    # ── quotes and the session line ──────────────────────────────────────────
    def quotes(self, symbols: Sequence[str]) -> Dict[str, Quote]:
        if not symbols:
            return {}
        d = self._get(SPARK, {"symbols": ",".join(symbols), "range": "1d", "interval": "5m"})
        try:
            return self._parse_spark(d)
        except (KeyError, TypeError, ValueError, IndexError, AttributeError) as e:
            raise ProviderError("spark answer not parsed (%s: %s)" % (type(e).__name__, str(e)[:60])) from None

    def _parse_spark(self, d) -> Dict[str, Quote]:
        out: Dict[str, Quote] = {}
        spark = d.get("spark") if isinstance(d, dict) else None
        if isinstance(spark, dict):                          # v7: {"spark":{"result":[{"symbol","response":[{meta,...}]}]}}
            for item in spark.get("result") or []:
                sym = item.get("symbol")
                resp = (item.get("response") or [None])[0]
                if sym and resp:
                    q = _quote(sym, resp)
                    if q:
                        out[sym] = q
                        if resp.get("meta"):
                            self._meta[sym] = resp["meta"]
        else:                                                # v8 spark: {"AAPL":{"symbol","timestamp","close","previousClose",..}}
            for sym, item in d.items():
                if not isinstance(item, dict) or "close" not in item:
                    continue
                closes = item.get("close") or []
                last = next((c for c in reversed(closes) if c is not None), None)
                if last is None:
                    continue
                prev = item.get("chartPreviousClose", item.get("previousClose"))
                ts = int((item.get("timestamp") or [0])[-1] or 0)
                out[sym] = Quote(sym, float(last), float(prev) if prev is not None else None, ts, None, None, None, {})
        return out

    def intraday(self, symbol: str) -> Intraday:
        d = self._get(CHART % symbol, {"range": "1d", "interval": "1m", "includePrePost": "false"})
        try:
            return self._parse_intraday(d, symbol)
        except (KeyError, TypeError, ValueError, IndexError, AttributeError) as e:
            raise ProviderError("%s: intraday answer not parsed (%s: %s)" % (symbol, type(e).__name__, str(e)[:60])) from None

    def _parse_intraday(self, d, symbol: str) -> Intraday:
        r = self._result(d, symbol)
        meta = r.get("meta") or {}
        if meta:
            self._meta[symbol] = meta
        closes = ((r.get("indicators") or {}).get("quote") or [{}])[0].get("close") or []
        times, vals = [], []
        for t, c in zip(r.get("timestamp") or [], closes):
            if c is not None:
                times.append(int(t))
                vals.append(float(c))
        reg = ((meta.get("currentTradingPeriod") or {}).get("regular") or {})
        return Intraday(symbol, times, vals, meta.get("chartPreviousClose", meta.get("previousClose")),
                        int(reg.get("start") or 0), int(reg.get("end") or 0), meta.get("currency"), meta)

    def session_meta(self, symbol: str) -> dict:
        if symbol not in self._meta:
            self.raw_chart(symbol, interval="1d", range="5d")
        return self._meta[symbol]

    # ── the expense ratio ────────────────────────────────────────────────────
    def fund_info(self, symbol: str) -> FundInfo:
        if self.use_yfinance in ("auto", "yes"):
            info = self._fund_info_yfinance(symbol)
            if info is not None:
                return info
            if self.use_yfinance == "yes":
                raise ProviderError("%s: yfinance gave no expense ratio" % symbol)
        return self._fund_info_direct(symbol)

    def _fund_info_yfinance(self, symbol: str) -> Optional[FundInfo]:
        try:
            import yfinance as yf                    # optional; the add-on's python_packages
        except ImportError:
            return None
        try:
            self.pacer.wait()
            ops = yf.Ticker(symbol).funds_data.fund_operations
        except Exception as e:                       # the type only: a message could carry a URL with a crumb
            raise ProviderError("%s: yfinance %s" % (symbol, type(e).__name__)) from None
        try:
            for label in getattr(ops, "index", []):
                if "expense ratio" in str(label).lower():
                    row = ops.loc[label]
                    val = row.iloc[0] if hasattr(row, "iloc") else row
                    if val is not None and val == val:
                        return FundInfo(symbol, float(val), "yfinance", kind="FUND")
        except Exception:
            return None
        return None

    def _fund_info_direct(self, symbol: str) -> FundInfo:
        if not self._crumb:
            self.pacer.wait()
            try:
                self._crumb = self.http.get_text(CRUMB).strip()
            except RateLimited as e:
                self.backoff.hit(e.retry_after)
                raise
            if not self._crumb or len(self._crumb) > 40 or "{" in self._crumb:
                self._crumb = None
                raise ProviderError("%s: no crumb from Yahoo" % symbol)
        try:
            d = self._get(SUMMARY % symbol, {"modules": "fundProfile,price", "crumb": self._crumb})
        except ProviderError as e:
            if "401" in str(e) or "403" in str(e):
                self._crumb = None               # the crumb expired: the next call fetches a new one
            raise
        qs = d.get("quoteSummary") or {}
        if qs.get("error") or not qs.get("result"):
            raise NotFound("%s: quoteSummary %s" % (symbol, (qs.get("error") or {}).get("code", "empty")))
        sym, ter = M.parse_yahoo_ter(d)
        price = qs["result"][0].get("price") or {}
        return FundInfo(symbol, ter, "yahoo" if ter is not None else "", price.get("shortName"), price.get("quoteType"))

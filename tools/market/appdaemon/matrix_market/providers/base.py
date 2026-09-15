"""The Provider interface, its value types, its errors, and the pacing helpers.

A provider answers five questions about a symbol - its history (bars, dividends,
splits, currency, exchange meta), its fund information (the expense ratio), a
quote for many symbols at once, the day's intraday line, and its exchange
sessions - and may raise RateLimited (the caller backs off) or ProviderError
(the caller marks the symbol and keeps its store). History comes back as
market_ref.Series so the maths and the store take it as it is.
"""
import datetime as dt
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence

from .._ref import market_ref as M

Date = dt.date


class ProviderError(Exception):
    """The source did not answer usably. The message names the symbol and the HTTP code, never a payload."""


class RateLimited(ProviderError):
    """HTTP 429: the caller backs off (Backoff) and stops the run."""

    def __init__(self, msg: str, retry_after: Optional[float] = None):
        super().__init__(msg)
        self.retry_after = retry_after


class NotFound(ProviderError):
    """An unknown symbol, or a service this provider does not have."""


@dataclass
class Quote:
    sym: str
    last: float
    prev: Optional[float]           # the previous regular close
    ts: int                         # regularMarketTime, epoch seconds
    currency: Optional[str] = None
    day_hi: Optional[float] = None
    day_lo: Optional[float] = None
    meta: dict = field(default_factory=dict)   # the answer's meta (currentTradingPeriod, gmtoffset, ...)

    @property
    def day(self) -> Optional[float]:
        return (self.last / self.prev - 1.0) if self.prev else None


@dataclass
class Intraday:
    sym: str
    times: List[int]                # epoch seconds of the 1-minute bars
    closes: List[float]
    prev_close: Optional[float]
    session_start: int              # regular session, epoch seconds
    session_end: int
    currency: Optional[str] = None
    meta: dict = field(default_factory=dict)


@dataclass
class FundInfo:
    sym: str
    ter: Optional[float]            # a fraction per year, Yahoo's raw; None for a stock or unknown
    source: str = ""                # "yahoo", "yfinance", "override", ""
    name: Optional[str] = None
    kind: Optional[str] = None      # ETF, MUTUALFUND, EQUITY, INDEX, CURRENCY


class Provider:
    """The interface. Subclasses implement what their source has and raise NotFound for the rest."""
    name = "base"

    def history(self, symbol: str, start: Date, end: Optional[Date] = None, interval: str = "1d") -> "M.Series":
        raise NotImplementedError

    def dividends(self, symbol: str, start: Date):
        return self.history(symbol, start).dividends

    def splits(self, symbol: str, start: Date):
        return list(self.history(symbol, start).meta.get("splits") or [])

    def fund_info(self, symbol: str) -> FundInfo:
        raise NotFound("%s: no fund information from %s" % (symbol, self.name))

    def quotes(self, symbols: Sequence[str]) -> Dict[str, Quote]:
        raise NotFound("no quotes from %s" % self.name)

    def intraday(self, symbol: str) -> Intraday:
        raise NotFound("%s: no intraday line from %s" % (symbol, self.name))

    def session_meta(self, symbol: str) -> dict:
        """Yahoo's meta shape: currentTradingPeriod {pre, regular, post: {start, end}}, gmtoffset,
        exchangeTimezoneName, exchangeName, fullExchangeName."""
        raise NotFound("%s: no session meta from %s" % (symbol, self.name))

    def raw_chart(self, symbol: str, **params) -> dict:
        """The source's raw history answer, for saving a sample. Not every provider has one."""
        raise NotFound("%s: no raw answers from %s" % (symbol, self.name))


class Pacer:
    """At least `gap` seconds between two calls, across threads. `sleep` is injectable for tests."""

    def __init__(self, gap: float, sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic):
        self.gap, self._sleep, self._clock = float(gap), sleep, clock
        self._last: Optional[float] = None
        self._lock = threading.Lock()

    def wait(self) -> float:
        """Sleep the remainder of the gap; returns the seconds slept."""
        with self._lock:
            now = self._clock()
            slept = 0.0
            if self._last is not None:
                rest = self.gap - (now - self._last)
                if rest > 0:
                    self._sleep(rest)
                    slept = rest
            self._last = self._clock()
            return slept


class Backoff:
    """The 429 ladder: blocked for steps[0], then steps[1], ... on repeated hits; one clean call resets.

    Two guards so that one bad setting (a User-Agent Yahoo refuses, audit B2) cannot silence the app:
    - `cap`: no wait is longer, a Retry-After included (default: the last step, an hour);
    - `reset_after`: a hit more than this long after the previous one starts the ladder again at the
      first step, so after a quiet spell the app tries again soon rather than an hour later.
    The caller schedules its own retry for when the block lifts (history.HistoryFeature.retry_later)."""

    def __init__(self, steps: Sequence[float] = (300.0, 900.0, 3600.0), clock: Callable[[], float] = time.time,
                 cap: Optional[float] = None, reset_after: float = 6 * 3600.0):
        self.steps, self._clock = tuple(steps), clock
        self.cap = float(cap if cap is not None else self.steps[-1])
        self.reset_after = float(reset_after)
        self.level = 0
        self.until = 0.0
        self.hits = 0
        self.last_hit: Optional[float] = None

    def hit(self, retry_after: Optional[float] = None) -> float:
        now = self._clock()
        if self.last_hit is not None and now - self.last_hit > self.reset_after:
            self.level = 0
        wait = self.steps[min(self.level, len(self.steps) - 1)]
        if retry_after:
            wait = max(wait, float(retry_after))
        wait = min(wait, self.cap)
        self.until = now + wait
        self.level = min(self.level + 1, len(self.steps) - 1)
        self.hits += 1
        self.last_hit = now
        return wait

    def ok(self) -> None:
        self.level = 0
        self.until = 0.0

    def blocked(self) -> bool:
        return self._clock() < self.until

    def remaining(self) -> float:
        return max(0.0, self.until - self._clock())

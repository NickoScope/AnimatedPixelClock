#!/usr/bin/env python3
"""Reference maths of the market dashboard: the ledger on daily bars.

docs/18-stock-dashboard.md in the knowledge base (v3, "Portfolio maths, v3:
the ledger") is the specification; this file is its arithmetic, written once
so the AppDaemon app and its tests have a fixed point to compare with. Pure
functions over plain lists, standard library only: it runs on the bare
Python 3.9 of the Mac and on Home Assistant alike.

The design's words, kept as names here:
  PX        the positions alone, sum of qty x close x FX
  V         the ledger, PX + cash: the value the panel shows
  HOLD      31 December puts the cash to work, buying only, never selling
  REBAL     31 December sets every trading position to its target weight
  X_s(d)    portfolio currency per unit of s's currency on day d

The 31 December rule, as the owner fixed it on 2026-09-15 09:23: in BOTH modes
all cash (dividends received, contributions) goes into positions at the
year-end close, pro rata to the target weights of the positions that already
trade. The only cash that stays is the reserved share (capital x weight) of a
position whose fund has not started trading; it is deployed at the first
31 December after the fund's first bar, in HOLD as a buy of that position, in
REBAL through the normal rebalance. Between year-ends cash just accumulates.

Fixtures: the saved Yahoo answers in samples/ are monthly, weekly or quarterly
bars. Their bars are used AS the daily timeline here, one bar = one "day": the
ledger does not care about the spacing, only about the order of events. The
one exception is the TER drag, which the design states per trading day
(value x TER / 252); simulate() takes a `bar_years` callable so a monthly
fixture charges a month's worth per bar. Dividends and contributions land on
the first timeline day on or after their date, which on a daily timeline is
the ex-date itself.

FX: `EURUSD=X` is USD per EUR, so a USD price in EUR is close / EURUSD. Yahoo's
series starts 2003-12-01; before that come the ECB's monthly reference rates,
series EXR.M.USD.EUR.SP00.E: end of period, the month's last daily reference
rate, which is what a monthly bar's close is (A would be the month's average).
They are saved from the ECB Data Portal in samples/ (ECB_SAMPLE) and handed to
FxTable as its `ecb` hook by ecb_monthly_table(); load_samples() does that. The
default hook is still the stub that answers nothing, so an FxTable built
without the table, or a month outside it, refuses with NoFxRate. No rate is
ever typed in.

  python3 tools/market/market_ref.py     # the preview portfolio on the samples
"""
import base64
import bisect
import csv
import datetime as dt
import json
import pathlib
import struct
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

HERE = pathlib.Path(__file__).resolve().parent
SAMPLES = HERE / "samples"

TRADING_DAYS_PER_YEAR = 252          # the design's TER formula: per trading day
POINTS = 128                         # points per published line
PRESETS = ("YTD", "1Y", "3Y", "5Y", "10Y", "MAX")
STALE_TRADING_DAYS = 3               # AS OF older than this many trading days -> STALE
CAGR_MIN_YEARS = 3                   # cagr reported when the window is at least this long
Date = dt.date


# ── calendar helpers ─────────────────────────────────────────────────────────
def add_years(d: Date, n: int) -> Date:
    """The same calendar day n years on; 29 February falls back to the 28th."""
    try:
        return d.replace(year=d.year + n)
    except ValueError:
        return d.replace(year=d.year + n, day=28)


def add_months(d: Date, n: int) -> Date:
    m = d.month - 1 + n
    y, m = d.year + m // 12, m % 12 + 1
    last = (dt.date(y + (m == 12), m % 12 + 1, 1) - dt.timedelta(days=1)).day
    return dt.date(y, m, min(d.day, last))


def years_between(a: Date, b: Date) -> float:
    return (b - a).days / 365.25


def calendar_years(prev: Date, cur: Date) -> float:
    """`bar_years` for fixtures whose bars are not days: the calendar time between bars."""
    return (cur - prev).days / 365.25


def weekdays_after(a: Date, b: Date) -> int:
    """Mon-Fri days in (a, b]. Exchange holidays are not known here."""
    n, d = 0, a
    while d < b:
        d += dt.timedelta(days=1)
        n += d.weekday() < 5
    return n


def is_stale(asof: Date, today: Date) -> bool:
    return weekdays_after(asof, today) > STALE_TRADING_DAYS


# ── series ───────────────────────────────────────────────────────────────────
@dataclass
class Series:
    """One symbol's bars: ascending dates, split-adjusted closes, dividends at Yahoo's event date."""
    sym: str
    currency: str
    dates: List[Date]
    closes: List[float]
    dividends: List[Tuple[Date, float]] = field(default_factory=list)
    ter: Optional[float] = None       # fraction per year, None for a stock
    meta: dict = field(default_factory=dict)
    adj: Optional[List[float]] = None  # adjclose, split- and dividend-adjusted: total-return benchmarks only (council rule 1)

    def index_on(self, day: Date) -> int:
        """Index of the last bar on or before `day`, -1 before the first bar."""
        return bisect.bisect_right(self.dates, day) - 1

    def close_on(self, day: Date) -> Optional[float]:
        """Carried forward from the last bar on or before `day`; None before the first bar."""
        i = self.index_on(day)
        return self.closes[i] if i >= 0 else None

    def trades(self, day: Date) -> bool:
        """A symbol exists from its first bar."""
        return bool(self.dates) and self.dates[0] <= day

    @property
    def first(self) -> Date:
        return self.dates[0]

    @property
    def last(self) -> Date:
        return self.dates[-1]


def _local_date(ts: int, gmtoffset: int) -> Date:
    return (dt.datetime.fromtimestamp(ts, dt.timezone.utc) + dt.timedelta(seconds=gmtoffset)).date()


def load_yahoo_chart(path) -> Series:
    """A saved chart v8 answer -> Series; parse_yahoo_chart() does the work."""
    with open(path) as fh:
        return parse_yahoo_chart(json.load(fh))


def parse_yahoo_chart(d: dict) -> Series:
    """A chart v8 answer (decoded JSON) -> Series. Bar dates are the exchange's local date of the stamp.

    Holes (null closes) are dropped: the timeline rule carries the previous bar
    forward. Two stamps on one local day keep the later one (Yahoo appends a
    "today" bar). Dividends take events.dividends[].date, the real event date,
    not the bar stamp they are filed under. Splits, when present, go to
    meta["splits"] as (date, "num:den") for the store's corporate-action check;
    quantities are never scaled by them (close[] is already split-adjusted).
    """
    r = d["chart"]["result"][0]
    m = r["meta"]
    off = m.get("gmtoffset", 0)
    dates, closes, adjs = [], [], []
    adj_src = ((r["indicators"].get("adjclose") or [{}])[0].get("adjclose")) or []
    for i, (t, c) in enumerate(zip(r["timestamp"], r["indicators"]["quote"][0]["close"])):
        if c is None:
            continue
        a = adj_src[i] if i < len(adj_src) and adj_src[i] is not None else c
        day = _local_date(t, off)
        if dates and day == dates[-1]:
            closes[-1], adjs[-1] = float(c), float(a)
            continue
        dates.append(day)
        closes.append(float(c))
        adjs.append(float(a))
    divs = sorted((_local_date(v["date"], off), float(v["amount"]))
                  for v in r.get("events", {}).get("dividends", {}).values())
    splits = sorted([_local_date(v["date"], off).isoformat(), str(v.get("splitRatio") or f"{v.get('numerator')}:{v.get('denominator')}")]
                    for v in r.get("events", {}).get("splits", {}).values())
    meta = dict(m)
    if splits:
        meta["splits"] = splits
    return Series(m["symbol"], m["currency"], dates, closes, divs, None, meta, adjs if adj_src else None)


def load_yahoo_ter(path) -> Tuple[str, Optional[float]]:
    """A saved quoteSummary answer -> (symbol, expense ratio or None); parse_yahoo_ter() does the work."""
    with open(path) as fh:
        return parse_yahoo_ter(json.load(fh))


def parse_yahoo_ter(d: dict) -> Tuple[str, Optional[float]]:
    """A quoteSummary answer (fundProfile, price) -> (symbol, expense ratio or None)."""
    r = d["quoteSummary"]["result"][0]
    sym = r.get("price", {}).get("symbol")
    raw = (r.get("fundProfile", {}).get("feesExpensesInvestment", {})
            .get("annualReportExpenseRatio", {}).get("raw"))
    return sym, (float(raw) if raw is not None else None)


def as_of(series: List[Series]) -> Date:
    """The date of the oldest series on a page."""
    return min(s.last for s in series)


# ── FX ───────────────────────────────────────────────────────────────────────
class NoFxRate(Exception):
    """A conversion was asked for a day without a rate. Nothing is invented."""


def ecb_monthly_usd_per_eur(year: int, month: int) -> Optional[float]:
    """The default hook: no table, so None, and the caller refuses. The ECB's monthly
    rates are the saved table behind ecb_monthly_table(), which FxTable takes as its
    `ecb` hook (load_samples() does). Do not put numbers here."""
    return None


class FxTable:
    """EURUSD=X: USD per EUR, carried forward from the last bar on or before the day.

    factor(cur_from, cur_to, day) is `cur_to` per unit of `cur_from`:
    USD -> EUR divides by the rate, EUR -> USD multiplies. Only the pair the
    design has; another pair raises NoFxRate rather than guessing.
    """

    def __init__(self, dates: List[Date], rates: List[float],
                 ecb: Callable[[int, int], Optional[float]] = ecb_monthly_usd_per_eur):
        self.dates, self.rates, self.ecb = list(dates), [float(r) for r in rates], ecb

    @classmethod
    def from_series(cls, s: Series, ecb: Callable[[int, int], Optional[float]] = ecb_monthly_usd_per_eur) -> "FxTable":
        return cls(s.dates, s.closes, ecb)

    def usd_per_eur(self, day: Date) -> float:
        i = bisect.bisect_right(self.dates, day) - 1
        if i >= 0:
            return self.rates[i]
        r = self.ecb(day.year, day.month)
        if r is None:
            first = self.dates[0].isoformat() if self.dates else "never"
            raise NoFxRate(f"no EURUSD rate for {day.isoformat()}: the Yahoo series starts {first} "
                           f"and the ECB hook has no rate for {day.year}-{day.month:02d}. Refusing to invent one.")
        return r

    def factor(self, cur_from: str, cur_to: str, day: Date) -> float:
        if cur_from == cur_to:
            return 1.0
        if (cur_from, cur_to) == ("USD", "EUR"):
            return 1.0 / self.usd_per_eur(day)
        if (cur_from, cur_to) == ("EUR", "USD"):
            return self.usd_per_eur(day)
        raise NoFxRate(f"no FX series for {cur_from} -> {cur_to}")


# The ECB's monthly USD per EUR for the years before Yahoo's EURUSD=X. Checked on 2026-09-15
# against the API itself: in its code list CL_EXR_SUFFIX "E" is "End-of-period" and "A" is
# "Average"; for all 72 months 1999-01..2004-12 each E value is the last daily reference rate
# (EXR.D.USD.EUR.SP00.A) of its month, and each A value the mean of that month's daily rates.
ECB_SERIES = "EXR.M.USD.EUR.SP00.E"
ECB_START, ECB_END = "1999-01", "2004-12"
ECB_SAMPLE = "ecb_EXR_M_USD_EUR_SP00_E.csv"


def load_ecb_monthly(path) -> Dict[Tuple[int, int], float]:
    """The Data Portal's csvdata answer -> {(year, month): USD per EUR}. A row of another
    series refuses the file, so an average table cannot pass for the end-of-period one."""
    out = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            if row["KEY"] != ECB_SERIES:
                raise ValueError(f"{path}: a row of {row['KEY']}, expected {ECB_SERIES}")
            if row["OBS_VALUE"]:
                y, m = row["TIME_PERIOD"].split("-")
                out[(int(y), int(m))] = float(row["OBS_VALUE"])
    return out


def ecb_monthly_table(path=None) -> Callable[[int, int], Optional[float]]:
    """FxTable's `ecb` hook from the saved table (samples/ECB_SAMPLE unless `path`): the
    month's end-of-period rate, None for a month the table does not have."""
    table = load_ecb_monthly(path or SAMPLES / ECB_SAMPLE)
    return lambda year, month: table.get((year, month))


# ── the portfolio ────────────────────────────────────────────────────────────
@dataclass
class Position:
    sym: str
    w: float                          # target weight in percent, as the portal sends it
    entry: Optional[Date] = None      # an explicit entry date, else the inception


@dataclass
class Config:
    capital: float
    currency: str
    inception: Date
    positions: List[Position]
    contrib_amount: float = 0.0
    contrib_every: str = "year"       # "year" | "quarter" | "month", from the inception's anniversary
    rebalance: bool = False           # which mode the panel shows; both are computed


@dataclass
class Ledger:
    """One run of the ledger: per timeline day, and the state at the end."""
    mode: str
    days: List[Date]
    v: List[float]                    # V = PX + cash
    px: List[float]                   # PX, positions only
    cash: List[float]
    div: List[float]                  # cumulative dividends received
    ter: List[float]                  # cumulative TER drag estimate
    contrib: List[float]              # cumulative contributions
    qty: Dict[str, float]
    entries: Dict[str, Date]          # effective entry date per position
    entry_px: Dict[str, float]        # close x FX paid at entry
    events: List[Tuple[Date, str]]    # what happened when, for the report

    def index_on(self, day: Date) -> int:
        return bisect.bisect_right(self.days, day) - 1


def timeline(series: List[Series], start: Date) -> List[Date]:
    """The union of every series' bar dates from `start`: the fixtures' stand-in for
    the portfolio currency's trading calendar."""
    return sorted({d for s in series for d in s.dates if d >= start})


def year_ends(days: List[Date]) -> set:
    """The last timeline day of each calendar year, once a later day shows the year
    is over; the final day counts only if it is 31 December itself."""
    out = set()
    for i, d in enumerate(days):
        nxt = days[i + 1] if i + 1 < len(days) else None
        if (nxt is not None and nxt.year > d.year) or (nxt is None and (d.month, d.day) == (12, 31)):
            out.add(d)
    return out


def period_step(every: str) -> Callable[[Date, int], Date]:
    """The k-th anniversary of a date for "year", "quarter" or "month" (the portal's periods)."""
    if every == "year":
        return add_years
    if every == "quarter":
        return lambda d, k: add_months(d, 3 * k)
    return add_months


def contribution_dates(cfg: Config, last: Date) -> List[Date]:
    if not cfg.contrib_amount:
        return []
    step = period_step(cfg.contrib_every)
    out, k = [], 1
    while True:
        d = step(cfg.inception, k)
        if d > last:
            return out
        out.append(d)
        k += 1


def simulate(cfg: Config, series: Dict[str, Series], fx: FxTable, days: List[Date], mode: str,
             dividends: bool = True, bar_years: Optional[Callable[[Date, Date], float]] = None) -> Ledger:
    """The ledger, day by day. `mode` is "hold" or "rebal". `dividends=False` is the
    "no dividends" second run (the PX line). `bar_years(prev, cur)` is the year
    fraction one bar covers for the TER drag; None means a trading day, 1/252."""
    assert mode in ("hold", "rebal"), mode
    pos = {p.sym: p for p in cfg.positions}
    w = {p.sym: p.w / 100.0 for p in cfg.positions}
    sum_w = sum(w.values())
    if sum_w > 1.0 + 1e-9:
        raise ValueError(f"weights sum to {sum_w * 100:.1f} %")
    cash_floor = cfg.capital * (1.0 - sum_w)        # a deliberate cash allocation never invested (HOLD)
    qty = {s: 0.0 for s in w}
    reserved: Dict[str, float] = {}                 # capital x weight of a position not yet bought
    entries: Dict[str, Date] = {}
    entry_px: Dict[str, float] = {}
    events: List[Tuple[Date, str]] = []
    cash = cfg.capital
    div_cum = ter_cum = contrib_cum = 0.0
    div_ptr = {s: 0 for s in w}
    contribs = contribution_dates(cfg, days[-1] if days else cfg.inception)
    c_ptr = 0
    ye = year_ends(days)
    L = Ledger(mode, [], [], [], [], [], [], [], qty, entries, entry_px, events)

    def X(sym: str, d: Date) -> float:
        return fx.factor(series[sym].currency, cfg.currency, d)

    def price(sym: str, d: Date) -> float:              # close x FX, in the portfolio currency
        return series[sym].close_on(d) * X(sym, d)

    def entry_come(sym: str, d: Date) -> bool:
        e = pos[sym].entry
        return e is None or d >= e

    def buy(sym: str, amount: float, d: Date, why: str):
        nonlocal cash
        if amount <= 0:
            return
        p = price(sym, d)
        qty[sym] += amount / p
        cash -= amount
        if sym not in entries:
            entries[sym], entry_px[sym] = d, p
        events.append((d, f"{why} {sym} {amount:.2f} at {p:.4f}"))

    def px_value(d: Date) -> float:
        return sum(qty[s] * price(s, d) for s in w if qty[s])

    started = False
    prev: Optional[Date] = None
    for d in days:
        if d < cfg.inception:
            continue
        if not started:                                  # the first trading day on or after S
            started = True
            for s in w:
                if series[s].trades(d) and entry_come(s, d):
                    buy(s, cfg.capital * w[s], d, "start")
                else:
                    reserved[s] = cfg.capital * w[s]
                    events.append((d, f"reserve {s} {reserved[s]:.2f}"))
        else:                                            # an explicit entry date that has come
            for s in [s for s in reserved if pos[s].entry and d >= pos[s].entry and series[s].trades(d)]:
                buy(s, reserved.pop(s), d, "entry")
        if dividends:                                    # ex-dates landing on this timeline day
            for s in w:
                divs = series[s].dividends
                while div_ptr[s] < len(divs) and divs[div_ptr[s]][0] <= d:
                    # paid on the quantity held, for ex-dates from the entry day on (the design's
                    # order: the entry buy comes first, so an ex-date on the entry day is paid)
                    if qty[s] > 0 and divs[div_ptr[s]][0] >= entries[s]:
                        amt = divs[div_ptr[s]][1] * qty[s] * X(s, d)
                        cash += amt
                        div_cum += amt
                    div_ptr[s] += 1
        else:
            for s in w:
                divs = series[s].dividends
                while div_ptr[s] < len(divs) and divs[div_ptr[s]][0] <= d:
                    div_ptr[s] += 1
        while c_ptr < len(contribs) and contribs[c_ptr] <= d:
            cash += cfg.contrib_amount
            contrib_cum += cfg.contrib_amount
            events.append((d, f"contribution {cfg.contrib_amount:.2f}"))
            c_ptr += 1
        if prev is not None:                             # TER drag: value x TER x the bar's year fraction
            frac = bar_years(prev, d) if bar_years else 1.0 / TRADING_DAYS_PER_YEAR
            for s in w:
                if qty[s] and series[s].ter:
                    ter_cum += qty[s] * price(s, d) * series[s].ter * frac
        if d in ye:
            if mode == "hold":
                for s in [s for s in reserved if series[s].trades(d) and entry_come(s, d)]:
                    buy(s, reserved.pop(s), d, "year-end reserved")
                trading = [s for s in w if s in entries]
                free = cash - sum(reserved.values()) - cash_floor
                if free > 1e-9 and trading:
                    ws = sum(w[s] for s in trading)
                    for s in trading:
                        buy(s, free * w[s] / ws, d, "year-end cash")
            else:
                V = px_value(d) + cash
                trading = [s for s in w if series[s].trades(d) and entry_come(s, d)]
                for s in trading:
                    p = price(s, d)
                    target = V * w[s]
                    events.append((d, f"rebalance {s} {qty[s] * p:.2f} -> {target:.2f}"))
                    qty[s] = target / p
                    reserved.pop(s, None)
                    if s not in entries:
                        entries[s], entry_px[s] = d, p
                cash = V * (1.0 - sum(w[s] for s in trading))
        pxv = px_value(d)
        L.days.append(d)
        L.px.append(pxv)
        L.cash.append(cash)
        L.v.append(pxv + cash)
        L.div.append(div_cum)
        L.ter.append(ter_cum)
        L.contrib.append(contrib_cum)
        prev = d
    return L


# ── the extended ledger: the settings of docs/19 §5-6, every default = simulate() ──────
@dataclass
class Rules:
    """What the extended ledger honours beyond simulate()'s two modes (docs/18, "Settings after
    the research"; docs/19 §4-6). Every default is simulate()'s own behaviour, and
    test_market_ref checks simulate_ext(rules=Rules()) == simulate() to the last digit.

    dividends     sweep_yearend (cash until the 31 December sweep, the owner's rule) |
                  reinvest_paydate (bought back into the fund at that day's close, Portfolio
                  Visualizer's convention) | drop (no dividends at all)
    rebal         what REBAL means: hold | calendar | bands | calendar_or_bands
    calendar      annual (the last trading day of December) | semiannual | quarterly | monthly
    band_*        Swedroe's 5/25 rule (Bogleheads wiki, via docs/19 §4): abs_pts for targets of
                  20 % or more, rel_pct below; checked on the last trading day of each period
    contrib_only  contributions buy the underweight positions the day they arrive (no selling)
    withdraw_*    an amount and/or a percent of the balance, every year or month from the
                  inception's anniversary; sells pro rata when the cash does not cover it
    contrib_index a factor per date for inflation-indexed contributions (HICP ratio), None = off
    cash_yield_pct, tax_div_pct, cost_fixed, cost_pct: interest on the cash row, dividend
                  withholding, and a cost per trade (fixed + percent of the amount)
    """
    dividends: str = "sweep_yearend"
    rebal: str = "calendar"
    calendar: str = "annual"
    band_abs_pts: float = 5.0
    band_rel_pct: float = 25.0
    band_check: str = "monthly"
    contrib_only: bool = False
    withdraw_amount: float = 0.0
    withdraw_pct: float = 0.0
    withdraw_every: str = "year"
    contrib_index: Optional[Callable[[Date], float]] = None
    cash_yield_pct: float = 0.0
    tax_div_pct: float = 0.0
    cost_fixed: float = 0.0
    cost_pct: float = 0.0
    # What a REBAL keeps in cash for a fund that does not trade yet (owner, 2026-09-15 12:53: value_weight):
    #   value_weight    V x w, a share of the current value (the code since v1)
    #   capital_weight  (C + contributions so far) x w; the rest of V goes to the trading positions (and a
    #                   deliberate cash weight) pro rata, docs/18's first wording
    # When a late fund enters: calendar modes at the first period end (annual 31 December, semiannual,
    # quarterly, monthly) after its first bar, through the rebalance; bands mode at the first band-triggered
    # rebalance after its first bar, else at the 31 December sweep with its reserved capital x w (HOLD's
    # rule, since no rebalance ran). HOLD always enters it with capital x w.
    reserve: str = "value_weight"


@dataclass
class LedgerExt(Ledger):
    """simulate_ext's run: the Ledger plus what the new figures need."""
    flows: List[float] = field(default_factory=list)            # external flows per day: contributions +, withdrawals -
    local: Dict[str, List[float]] = field(default_factory=dict)  # the positions' value per currency, in that currency
    values: Dict[str, List[float]] = field(default_factory=dict) # each position's value per day, in the portfolio currency
    rebal_dates: List[Date] = field(default_factory=list)
    trades: int = 0
    costs: List[float] = field(default_factory=list)            # cumulative trading costs
    tax: List[float] = field(default_factory=list)              # cumulative dividend tax withheld
    withdrawn: List[float] = field(default_factory=list)        # cumulative withdrawals


def period_ends(days: List[Date], every: str) -> set:
    """The last timeline day of each period, once a later day shows the period is over; the final
    day counts only if it is the period's last calendar day (year_ends' rule, generalised).
    annual = year_ends(); semiannual: June and December; quarterly: March, June, September,
    December; monthly: every month."""
    if every == "annual":
        return year_ends(days)
    months = {"semiannual": (6, 12), "quarterly": (3, 6, 9, 12), "monthly": tuple(range(1, 13))}[every]
    out = set()
    for i, d in enumerate(days):
        if d.month not in months:
            continue
        nxt = days[i + 1] if i + 1 < len(days) else None
        last_cal = add_months(d.replace(day=1), 1) - dt.timedelta(days=1)
        if (nxt is not None and (nxt.year, nxt.month) > (d.year, d.month)) or (nxt is None and d == last_cal):
            out.add(d)
    return out


def check_days(days: List[Date], every: str) -> set:
    """When the bands are tested: daily, or the last timeline day of each ISO week or month."""
    if every == "daily":
        return set(days)
    if every == "monthly":
        return period_ends(days, "monthly")
    out = set()
    for i, d in enumerate(days):
        nxt = days[i + 1] if i + 1 < len(days) else None
        if (nxt is not None and nxt.isocalendar()[:2] != d.isocalendar()[:2]) or (nxt is None and d.weekday() == 4):
            out.add(d)
    return out


def band_bounds(w_pct: float, abs_pts: float = 5.0, rel_pct: float = 25.0) -> Tuple[float, float]:
    """Swedroe's 5/25 rule: +-abs_pts points for a target of 20 % or more, +-rel_pct of the target below."""
    if w_pct >= 20.0:
        return w_pct - abs_pts, w_pct + abs_pts
    return w_pct * (1.0 - rel_pct / 100.0), w_pct * (1.0 + rel_pct / 100.0)


def withdrawal_dates(cfg: Config, rules: Rules, last: Date) -> List[Date]:
    if not (rules.withdraw_amount or rules.withdraw_pct):
        return []
    step = period_step(rules.withdraw_every)
    out, k = [], 1
    while True:
        d = step(cfg.inception, k)
        if d > last:
            return out
        out.append(d)
        k += 1


def simulate_ext(cfg: Config, series: Dict[str, Series], fx: FxTable, days: List[Date], mode: str,
                 dividends: bool = True, bar_years: Optional[Callable[[Date, Date], float]] = None,
                 rules: Optional[Rules] = None) -> LedgerExt:
    """simulate() with the Rules' hooks. With rules=None (or Rules()) it IS simulate(), to the last
    digit, plus the extra records (flows, local values). `mode` "hold" ignores rules.rebal; "rebal"
    follows it. The owner's 31 December rule holds in every mode: a period end that is not a
    rebalance still puts the cash to work (HOLD's sweep)."""
    rules = rules or Rules()
    assert mode in ("hold", "rebal"), mode
    pos = {p.sym: p for p in cfg.positions}
    w = {p.sym: p.w / 100.0 for p in cfg.positions}
    sum_w = sum(w.values())
    if sum_w > 1.0 + 1e-9:
        raise ValueError(f"weights sum to {sum_w * 100:.1f} %")
    cash_floor = cfg.capital * (1.0 - sum_w)
    qty = {s: 0.0 for s in w}
    reserved: Dict[str, float] = {}
    entries: Dict[str, Date] = {}
    entry_px: Dict[str, float] = {}
    events: List[Tuple[Date, str]] = []
    cash = cfg.capital
    div_cum = ter_cum = contrib_cum = cost_cum = tax_cum = wd_cum = 0.0
    div_ptr = {s: 0 for s in w}
    contribs = contribution_dates(cfg, days[-1] if days else cfg.inception)
    withdrawals = withdrawal_dates(cfg, rules, days[-1] if days else cfg.inception)
    c_ptr = wd_ptr = 0
    rule = rules.rebal if mode == "rebal" else "hold"
    ye = year_ends(days)
    cal_ends = period_ends(days, rules.calendar) if rule in ("calendar", "calendar_or_bands") else set()
    checks = check_days(days, rules.band_check) if rule in ("bands", "calendar_or_bands") else set()
    currencies = sorted({series[s].currency for s in w})
    pay_div = dividends and rules.dividends != "drop"
    has_cost = bool(rules.cost_fixed or rules.cost_pct)
    L = LedgerExt(mode, [], [], [], [], [], [], [], qty, entries, entry_px, events)
    L.local = {c: [] for c in currencies}
    L.values = {s: [] for s in w}

    def X(sym: str, d: Date) -> float:
        return fx.factor(series[sym].currency, cfg.currency, d)

    def price(sym: str, d: Date) -> float:
        return series[sym].close_on(d) * X(sym, d)

    def entry_come(sym: str, d: Date) -> bool:
        e = pos[sym].entry
        return e is None or d >= e

    def cost_of(amount: float) -> float:
        return (rules.cost_fixed + abs(amount) * rules.cost_pct / 100.0) if has_cost else 0.0

    def buy(sym: str, amount: float, d: Date, why: str):
        """`amount` of the position bought at the close; the trade's cost comes from cash on top."""
        nonlocal cash, cost_cum
        if amount <= 0:
            return
        p = price(sym, d)
        c = cost_of(amount)
        qty[sym] += amount / p
        cash -= amount + c
        cost_cum += c
        L.trades += 1
        if sym not in entries:
            entries[sym], entry_px[sym] = d, p
        events.append((d, f"{why} {sym} {amount:.2f} at {p:.4f}"))

    def buy_budget(sym: str, budget: float, d: Date, why: str):
        """Spend `budget` of cash, the trade's cost inside it."""
        amount = (budget - rules.cost_fixed) / (1.0 + rules.cost_pct / 100.0) if has_cost else budget
        buy(sym, amount, d, why)

    def sell(sym: str, amount: float, d: Date, why: str):
        nonlocal cash, cost_cum
        p = price(sym, d)
        amount = min(amount, qty[sym] * p)
        if amount <= 0:
            return
        c = cost_of(amount)
        qty[sym] -= amount / p
        cash += amount - c
        cost_cum += c
        L.trades += 1
        events.append((d, f"{why} {sym} -{amount:.2f} at {p:.4f}"))

    def px_value(d: Date) -> float:
        return sum(qty[s] * price(s, d) for s in w if qty[s])

    def trading_now(d: Date) -> List[str]:
        return [s for s in w if series[s].trades(d) and entry_come(s, d)]

    def sweep(d: Date):
        """HOLD's 31 December: reserved shares of funds now trading, then the free cash pro rata."""
        for s in [s for s in reserved if series[s].trades(d) and entry_come(s, d)]:
            buy_budget(s, reserved.pop(s), d, "year-end reserved")
        trading = [s for s in w if s in entries]
        free = cash - sum(reserved.values()) - cash_floor
        if free > 1e-9 and trading:
            ws = sum(w[s] for s in trading)
            for s in trading:
                buy_budget(s, free * w[s] / ws, d, "year-end cash")

    def rebal_targets(V: float, trading: List[str]) -> Dict[str, float]:
        """Each trading position's value after a rebalance, by Rules.reserve (see there)."""
        if rules.reserve == "capital_weight":
            held_back = (cfg.capital + contrib_cum) * sum(w[s] for s in w if s not in trading)
            share = sum(w[s] for s in trading) + (1.0 - sum_w)
            base = max(V - held_back, 0.0)
            return {s: base * w[s] / share for s in trading} if share > 0 else {}
        return {s: V * w[s] for s in trading}

    def rebalance(d: Date, why: str):
        """REBAL: every trading position to its target at the close (rebal_targets: V x w by default);
        with trading costs the same targets are reached through sells then buys, the costs out of cash."""
        nonlocal cash
        V = px_value(d) + cash
        trading = trading_now(d)
        T = rebal_targets(V, trading)
        if not has_cost:
            for s in trading:
                p = price(s, d)
                target = T[s]
                events.append((d, f"{why} {s} {qty[s] * p:.2f} -> {target:.2f}"))
                qty[s] = target / p
                reserved.pop(s, None)
                if s not in entries:
                    entries[s], entry_px[s] = d, p
            if rules.reserve == "capital_weight":
                cash = V - sum(T.values())
            else:
                cash = V * (1.0 - sum(w[s] for s in trading))     # the reference's own expression, to the last digit
        else:
            for s in trading:
                cur = qty[s] * price(s, d)
                if cur > T[s]:
                    sell(s, cur - T[s], d, why)
            for s in trading:
                cur = qty[s] * price(s, d)
                if T[s] > cur:
                    buy(s, T[s] - cur, d, why)
                reserved.pop(s, None)
        L.rebal_dates.append(d)

    def out_of_band(d: Date) -> bool:
        V = px_value(d) + cash
        if V <= 0:
            return False
        for s in trading_now(d):
            if s not in entries:
                continue
            now = 100.0 * qty[s] * price(s, d) / V
            lo, hi = band_bounds(w[s] * 100.0, rules.band_abs_pts, rules.band_rel_pct)
            if now < lo or now > hi:
                return True
        return False

    def buy_underweight(d: Date, budget: float):
        """contrib_only: the new cash fills the shortfalls of the held positions, else pro rata."""
        held = [s for s in trading_now(d) if s in entries]
        if not held:
            return
        V = px_value(d) + cash
        short = {s: max(0.0, V * w[s] - qty[s] * price(s, d)) for s in held}
        tot = sum(short.values())
        for s in held:
            share = short[s] / tot if tot > 1e-9 else w[s] / sum(w[x] for x in held)
            buy_budget(s, budget * share, d, "contribution")

    started = False
    prev: Optional[Date] = None
    for d in days:
        if d < cfg.inception:
            continue
        if not started:
            started = True
            for s in w:
                if series[s].trades(d) and entry_come(s, d):
                    buy_budget(s, cfg.capital * w[s], d, "start")
                else:
                    reserved[s] = cfg.capital * w[s]
                    events.append((d, f"reserve {s} {reserved[s]:.2f}"))
        else:
            for s in [s for s in reserved if pos[s].entry and d >= pos[s].entry and series[s].trades(d)]:
                buy_budget(s, reserved.pop(s), d, "entry")
        for s in w:
            divs = series[s].dividends
            while div_ptr[s] < len(divs) and divs[div_ptr[s]][0] <= d:
                if pay_div and qty[s] > 0 and divs[div_ptr[s]][0] >= entries[s]:
                    gross = divs[div_ptr[s]][1] * qty[s] * X(s, d)
                    tax = gross * rules.tax_div_pct / 100.0
                    amt = gross - tax
                    tax_cum += tax
                    cash += amt
                    div_cum += amt
                    if rules.dividends == "reinvest_paydate":
                        buy_budget(s, amt, d, "reinvest")
                div_ptr[s] += 1
        flow = 0.0
        while c_ptr < len(contribs) and contribs[c_ptr] <= d:
            amount = cfg.contrib_amount * (rules.contrib_index(d) if rules.contrib_index else 1.0)
            cash += amount
            contrib_cum += amount
            flow += amount
            events.append((d, f"contribution {amount:.2f}"))
            if rules.contrib_only:
                buy_underweight(d, amount)
            c_ptr += 1
        while wd_ptr < len(withdrawals) and withdrawals[wd_ptr] <= d:
            V = px_value(d) + cash
            amt = min(V, rules.withdraw_amount + V * rules.withdraw_pct / 100.0)
            need = amt - cash
            if need > 0:
                pxv = px_value(d)
                for s in [s for s in w if qty[s]]:
                    sell(s, need * qty[s] * price(s, d) / pxv, d, "withdrawal")
            cash -= amt
            wd_cum += amt
            flow -= amt
            events.append((d, f"withdrawal {amt:.2f}"))
            wd_ptr += 1
        if prev is not None:
            frac = bar_years(prev, d) if bar_years else 1.0 / TRADING_DAYS_PER_YEAR
            if rules.cash_yield_pct and cash > 0:
                cash += cash * rules.cash_yield_pct / 100.0 * frac
            for s in w:
                if qty[s] and series[s].ter:
                    ter_cum += qty[s] * price(s, d) * series[s].ter * frac
        if rule == "hold":
            if d in ye:
                sweep(d)
        else:
            done = False
            if rule in ("calendar", "calendar_or_bands") and d in cal_ends:
                rebalance(d, "rebalance")
                done = True
            if not done and rule in ("bands", "calendar_or_bands") and d in checks and out_of_band(d):
                rebalance(d, "band rebalance")
                done = True
            if not done and d in ye:
                sweep(d)
        pxv = px_value(d)
        L.days.append(d)
        L.px.append(pxv)
        L.cash.append(cash)
        L.v.append(pxv + cash)
        L.div.append(div_cum)
        L.ter.append(ter_cum)
        L.contrib.append(contrib_cum)
        L.flows.append(flow)
        L.costs.append(cost_cum)
        L.tax.append(tax_cum)
        L.withdrawn.append(wd_cum)
        for c in currencies:
            L.local[c].append(sum(qty[s] * series[s].close_on(d) for s in w if qty[s] and series[s].currency == c))
        for s in w:
            L.values[s].append(qty[s] * price(s, d) if qty[s] else 0.0)
        prev = d
    return L


# ── returns: time-weighted, money-weighted, annualised ───────────────────────
def twr(v: List[float], flows: List[float], i0: int, i1: int, timing: str = "pp") -> float:
    """The time-weighted return over (i0, i1]: daily returns geometrically linked with the external
    flows taken out (GIPS 2.A.24).
    timing "pp" (the default, the contract): Portfolio Performance's daily formula as written,
    1 + r = (MVE + CFout) / (MVB + CFin), inflows at the start of the day, outflows at its end
    (help.portfolio-performance.info, "time-weighted", as docs/19 §6.1 names it and
    tools/market/preview_returns.py implements it independently).
    timing "close": a flow at the close of its day, when this ledger books it: 1 + r = (V - CF) / V_prev.
    The two differ on a flow day: the contribution sits in cash that day, so PP's reading dilutes the
    day's return (10 000 + 1 000 a year into a 7 % fund, 2000-2010 daily: 6.666 % a year vs 6.668 %).
    Without flows both give V_i1 / V_i0 - 1, bit for bit."""
    if i1 <= i0 or v[i0] <= 0:
        return 0.0
    if not any(flows[i] for i in range(i0 + 1, min(i1, len(flows) - 1) + 1)):
        return v[i1] / v[i0] - 1.0            # no flows: the direct ratio, bit-identical to report()'s chg
    g = 1.0
    for i in range(i0 + 1, i1 + 1):
        cf = flows[i] if i < len(flows) else 0.0
        prev, cur = v[i - 1], v[i]
        if timing == "pp":
            cf_in, cf_out = max(cf, 0.0), max(-cf, 0.0)
            denom, num = prev + cf_in, cur + cf_out
        else:
            denom, num = prev, cur - cf
        if denom <= 0:
            continue
        g *= num / denom
    return g - 1.0


XIRR_DAY_BASIS = 365            # Excel's XIRR year (Microsoft Support, XIRR function)


def xirr(cashflows: List[Tuple[Date, float]], lo: float = -0.9999, hi: float = 100.0) -> Optional[float]:
    """The annualised money-weighted return: the r with sum cf_i / (1 + r)^((d_i - d_1) / 365) = 0 (GIPS
    2.A.29: since inception, daily external flows; the Bogleheads wiki's "investor return", docs/19 §4).
    The year is Excel's: "All succeeding payments are discounted based on a 365-day year" (Microsoft
    Support, XIRR function, fetched 2026-09-15), so the owner's spreadsheet gives the same rate; TWR's
    annualised figure keeps market_ref's 365.25-day year, like cagr. Investments negative, the end value
    positive. Bisection; None when the flows have no sign change or no root in (lo, hi)."""
    if not cashflows or not (any(a < 0 for _, a in cashflows) and any(a > 0 for _, a in cashflows)):
        return None
    t0 = min(d for d, _ in cashflows)
    ts = [((d - t0).days / XIRR_DAY_BASIS, a) for d, a in cashflows]

    def npv(r: float) -> float:
        return sum(a / (1.0 + r) ** t for t, a in ts)
    f_lo, f_hi = npv(lo), npv(hi)
    if f_lo * f_hi > 0:
        return None
    for _ in range(200):
        mid = (lo + hi) / 2.0
        f_mid = npv(mid)
        if abs(f_mid) < 1e-10 or hi - lo < 1e-12:
            return mid
        if f_lo * f_mid < 0:
            hi, f_hi = mid, f_mid
        else:
            lo, f_lo = mid, f_mid
    return (lo + hi) / 2.0


def ledger_xirr(cfg: Config, L: LedgerExt, i1: Optional[int] = None) -> Optional[float]:
    """XIRR since inception from the ledger's own flows: -capital at the start, -contributions,
    +withdrawals, +V at the end."""
    if not L.days:
        return None
    i1 = len(L.days) - 1 if i1 is None else i1
    flows: List[Tuple[Date, float]] = [(L.days[0], -cfg.capital)]
    for i in range(1, i1 + 1):
        if i < len(L.flows) and L.flows[i]:
            flows.append((L.days[i], -L.flows[i]))
    flows.append((L.days[i1], L.v[i1]))
    return xirr(flows)


ONE_YEAR = 365.0 / 365.25      # a calendar year of 365 days counts as a year (years are ACT/365.25 here)


def annualised(total: float, years: float) -> Optional[float]:
    """(1 + R)^(1/years) - 1, and None under one year: returns for periods of less than one year
    must not be annualised (GIPS 2.A.12, docs/19 §4). A window of 365 days is one year."""
    if years < ONE_YEAR - 1e-9 or total <= -1.0:
        return None
    return (1.0 + total) ** (1.0 / years) - 1.0


# ── windows and the reported figures ─────────────────────────────────────────
def window_start(preset: str, asof: Date, inception: Date, days: List[Date]) -> Date:
    """YTD: the last trading day of the previous year; nY: the same day n years back;
    MAX: the inception. The preset only changes what is visible."""
    if preset == "MAX":
        return inception
    if preset == "YTD":
        prev = [d for d in days if d.year < asof.year]
        return prev[-1] if prev else days[0]
    return add_years(asof, -int(preset[:-1]))


def window_series(dates: List[Date], values: List[float], start: Date, end: Date) -> Tuple[List[Date], List[float]]:
    """The bars in (start, end] behind the value carried to `start`. A series that
    starts after `start` (a short history) begins at its first bar instead."""
    i0 = bisect.bisect_right(dates, start) - 1
    if i0 < 0:
        i0 = bisect.bisect_left(dates, start)
        if i0 >= len(dates) or dates[i0] > end:
            return [], []
        wd, wv = [dates[i0]], [values[i0]]
    else:
        wd, wv = [start], [values[i0]]
    i1 = bisect.bisect_right(dates, end)
    for i in range(i0 + 1, i1):
        wd.append(dates[i])
        wv.append(values[i])
    return wd, wv


def max_drawdown(values: List[float]) -> float:
    """The deepest fall from a previous peak, as a fraction <= 0."""
    peak, mdd = float("-inf"), 0.0
    for v in values:
        peak = max(peak, v)
        if peak > 0:
            mdd = min(mdd, v / peak - 1.0)
    return mdd


def report(wd: List[Date], wv: List[float]) -> dict:
    """The figures of one windowed line: last, chg over the window, hi, lo, cagr, mdd."""
    v0, v1 = wv[0], wv[-1]
    out = {"from": wd[0], "to": wd[-1], "last": v1, "chg": v1 / v0 - 1.0 if v0 else 0.0,
           "hi": max(wv), "lo": min(wv), "mdd": max_drawdown(wv), "cagr": None}
    if add_years(wd[0], CAGR_MIN_YEARS) <= wd[-1] and v0 > 0 and v1 > 0:
        out["cagr"] = (v1 / v0) ** (1.0 / years_between(wd[0], wd[-1])) - 1.0
    return out


def downsample(wd: List[Date], wv: List[float], n: int = POINTS) -> List[float]:
    """n points at equal time steps across the window: each is the last bar of its
    slice, or the bar carried into an empty slice. The last point is the last bar."""
    if not wv:
        return []
    span = (wd[-1] - wd[0]).days
    if span == 0:
        return [wv[-1]] * n
    offs = [(d - wd[0]).days for d in wd]
    out, j = [], 0
    for i in range(n):
        while j + 1 < len(offs) and offs[j + 1] * n <= span * (i + 1):
            j += 1
        out.append(wv[j])
    return out


def pack_pts(points: List[float], lo: Optional[float] = None, hi: Optional[float] = None) -> Tuple[float, float, str]:
    """128 uint16 values scaled between min and max, little-endian, base64: the
    payload's `pts`. A flat line sits mid-scale."""
    lo = min(points) if lo is None else lo
    hi = max(points) if hi is None else hi
    if hi > lo:
        u = [int(round((v - lo) / (hi - lo) * 65535)) for v in points]
    else:
        u = [32767] * len(points)
    return lo, hi, base64.b64encode(struct.pack("<%dH" % len(u), *u)).decode()


def unpack_pts(b64: str) -> List[int]:
    raw = base64.b64decode(b64)
    return list(struct.unpack("<%dH" % (len(raw) // 2), raw))


def index_payload(s: Series, preset: str, asof: Date, inception: Date, name: Optional[str] = None) -> Optional[dict]:
    """`index/<sym>/<preset>` and `ticker/<sym>/<preset>`, as the design lists them."""
    end = min(asof, s.last)
    wd, wv = window_series(s.dates, s.closes, window_start(preset, end, inception, s.dates), end)
    if not wv:
        return None
    r = report(wd, wv)
    pts = downsample(wd, wv)
    lo, hi, b64 = pack_pts(pts)
    return {"v": 1, "sym": s.sym, "name": name or s.sym, "cur": s.currency, "preset": preset,
            "from": wd[0].isoformat(), "to": wd[-1].isoformat(), "last": r["last"], "chg": r["chg"],
            "hi": r["hi"], "lo": r["lo"], "n": len(pts), "min": lo, "max": hi, "pts": b64,
            "cagr": r["cagr"], "mdd": r["mdd"]}


def quote(s: Series) -> dict:
    """The last close and the day change against the bar before it. Only a daily
    series gives a DAY change; on a monthly fixture this would be a month's."""
    prev = s.closes[-2] if len(s.closes) > 1 else None
    return {"sym": s.sym, "last": s.closes[-1], "prev": prev, "date": s.last,
            "dayChg": (s.closes[-1] / prev - 1.0) if prev else None}


def benchmark(idx: Series, days: List[Date], capital: float, inception: Date) -> Optional[List[float]]:
    """The first index from the same start, its price index scaled to the capital.
    In the index's own currency: a EUR version of a USD index from 2000 would
    need FX from 2000, which Yahoo does not have (see NoFxRate)."""
    base = idx.close_on(inception)
    if base is None:
        return None
    return [capital * idx.close_on(d) / base for d in days]


def benchmark_in(idx: Series, days: List[Date], capital: float, inception: Date, fx: FxTable,
                 currency: str) -> Optional[List[float]]:
    """The benchmark in the portfolio currency, growth of `capital` from the inception:
    capital x (close x X)(d) / (close x X)(inception), so a line whose first day is the
    inception starts at the capital exactly. None when the index has no bar by the
    inception; a day without an FX rate refuses with NoFxRate, as the ledger does."""
    base = idx.close_on(inception)
    if base is None:
        return None
    base *= fx.factor(idx.currency, currency, inception)
    return [capital * (idx.close_on(d) * fx.factor(idx.currency, currency, d) / base) for d in days]


def portfolio_payload(cfg: Config, L: Ledger, L_nodiv: Ledger, bench: Optional[List[float]], preset: str) -> dict:
    """`portfolio/<mode>/<preset>`. `pts` = V, `px` = the no-dividend run, `bench` = the
    index line, all scaled between one shared min and max so they share the axis.
    div and terDrag are the amounts inside the window; cash is the share at the end."""
    asof = L.days[-1]
    start = window_start(preset, asof, cfg.inception, L.days)
    wd, wv = window_series(L.days, L.v, start, asof)
    _, wpx = window_series(L_nodiv.days, L_nodiv.v, start, asof)
    wb = window_series(L.days, bench, start, asof)[1] if bench else []
    r = report(wd, wv)
    lines = {"pts": downsample(wd, wv), "px": downsample(wd, wpx)}
    if wb:
        lines["bench"] = downsample(wd, wb)
    lo = min(min(p) for p in lines.values())
    hi = max(max(p) for p in lines.values())
    i0 = max(L.index_on(start), 0)
    i1 = len(L.days) - 1
    out = {"v": 1, "cur": cfg.currency, "mode": L.mode, "preset": preset,
           "from": wd[0].isoformat(), "to": wd[-1].isoformat(),
           "value": L.v[i1], "chg": r["chg"],
           "sinceStart": L.v[i1] / (cfg.capital + L.contrib[i1]) - 1.0,
           "cagr": r["cagr"], "mdd": r["mdd"],
           "div": L.div[i1] - L.div[i0], "terDrag": L.ter[i1] - L.ter[i0],
           "cash": L.cash[i1] / L.v[i1] if L.v[i1] else 0.0,
           "n": len(lines["pts"]), "min": lo, "max": hi}
    for k, p in lines.items():
        out[k] = pack_pts(p, lo, hi)[2]
    return out


def holdings_payload(cfg: Config, L: Ledger, series: Dict[str, Series], fx: FxTable) -> dict:
    """`holdings/<mode>`: target -> current share, return since entry (price, in the
    portfolio currency), the effective entry date; the cash row last."""
    d = L.days[-1]
    V = L.v[-1]
    rows = []
    for p in cfg.positions:
        s = series[p.sym]
        if p.sym in L.entries:
            price = s.close_on(d) * fx.factor(s.currency, cfg.currency, d)
            rows.append({"sym": p.sym, "tgt": p.w, "now": 100.0 * L.qty[p.sym] * price / V if V else 0.0,
                         "ret": price / L.entry_px[p.sym] - 1.0, "entry": L.entries[p.sym].isoformat()})
        else:
            rows.append({"sym": p.sym, "tgt": p.w, "now": 0.0, "ret": None, "entry": None})
    return {"v": 1, "asof": d.isoformat(), "mode": L.mode, "rows": rows,
            "cash": {"tgt": 100.0 - sum(p.w for p in cfg.positions), "now": 100.0 * L.cash[-1] / V if V else 0.0}}


# ── exchange sessions, for the tape and the LIVE / CLOSE label ───────────────
STATES = ("OPEN", "PRE", "POST", "CLOSED")


def session_from_meta(meta: dict) -> dict:
    """Yahoo's meta.currentTradingPeriod -> seconds since local midnight per session,
    plus the exchange's UTC offset. The samples are the source of these hours."""
    off = meta["gmtoffset"]
    out = {"gmtoffset": off, "tz": meta.get("exchangeTimezoneName"), "name": meta.get("fullExchangeName")}
    for k in ("pre", "regular", "post"):
        p = meta["currentTradingPeriod"][k]
        a = (p["start"] + off) % 86400
        b = (p["end"] + off) % 86400
        out[k] = (a, b if b or p["end"] == p["start"] else 86400)
    return out


def exchange_state(session: dict, now_utc: dt.datetime) -> str:
    """OPEN inside the regular session, PRE / POST inside those, else CLOSED. Weekends
    are CLOSED; exchange holidays are not modelled here (the app has Yahoo's next
    session for that)."""
    local = now_utc + dt.timedelta(seconds=session["gmtoffset"])
    if local.weekday() >= 5:
        return "CLOSED"
    t = local.hour * 3600 + local.minute * 60 + local.second
    for k, st in (("regular", "OPEN"), ("pre", "PRE"), ("post", "POST")):
        a, b = session[k]
        if a < b and a <= t < b:
            return st
    return "CLOSED"


# ── the samples, as the previews use them ────────────────────────────────────
PREVIEW_INDICES = (("^GSPC", "SPX", "yahoo_GSPC_max_1mo.json"), ("^IXIC", "NDX", "yahoo_IXIC.json"),
                   ("^FCHI", "CAC", "yahoo_FCHI_max_1mo.json"), ("^GDAXI", "DAX", "yahoo_GDAXI.json"))
PREVIEW_FILES = {"VOO": "yahoo_VOO_max_1mo.json", "IWDA.AS": "yahoo_IWDAAS.json", "AAPL": "yahoo_AAPL_5y_1wk.json",
                 "EURUSD=X": "yahoo_EURUSDX.json", "^GSPC.1d": "yahoo_GSPC_1y_1d.json"}
PREVIEW_TER_FILES = ("yahoo_VOO.json", "yahoo_IWDA.AS.json", "yahoo_VWCE.DE.json", "yahoo_AAPL.json")
# The default benchmark (owner, 2026-09-15 11:43): the S&P 500 total return, kept off the timeline.
BENCH_TR = ("^SP500TR", "yahoo_SP500TR_1mo.json")
# The real preview portfolio: VOO alone, the fund with a monthly sample (yahoo_<SYM>_1mo.json,
# see fetch_samples.py). preview_config() keeps the funds listed here that have a sample,
# weights renormalised to 100 %.
# IWDA.AS and AAPL are parser fixtures only (an EUR instrument without dividends; weekly bars).
PREVIEW_PORTFOLIO = (("VOO", 100.0),)
# An example allocation for the many-position frames: fourteen synthetic series SYNTH01X..SYNTH14X
# (render.py derives them from the real samples), weights descending, 100 % together. Not a
# real portfolio.
EXAMPLE_ALLOCATION = tuple((f"SYNTH{k + 1:02d}X", w) for k, w in
                           enumerate((20.0, 15.0, 10.0, 10.0, 8.0, 7.0, 6.0, 5.0, 5.0, 4.0, 3.0, 3.0, 2.0, 2.0)))
assert abs(sum(w for _, w in PREVIEW_PORTFOLIO) - 100.0) < 1e-9
assert abs(sum(w for _, w in EXAMPLE_ALLOCATION) - 100.0) < 1e-9


def sample_file(sym: str) -> Optional[pathlib.Path]:
    for name in (PREVIEW_FILES.get(sym), f"yahoo_{sym.replace('.', '')}_1mo.json"):
        if name and (SAMPLES / name).exists():
            return SAMPLES / name
    return None


def preview_config() -> Config:
    have = [(s, w) for s, w in PREVIEW_PORTFOLIO if sample_file(s)]
    scale = 100.0 / sum(w for _, w in have)
    return Config(10000.0, "EUR", dt.date(2000, 1, 1), [Position(s, round(w * scale, 4)) for s, w in have])


def load_samples() -> dict:
    """Every saved answer, with the TERs attached: {"series": {sym: Series}, "fx": FxTable,
    "indices": [(sym, mnemonic, Series)], "daily": Series (^GSPC 1y/1d)}, and "bench_tr":
    Series (^SP500TR) when that sample is saved. The FX carries the ECB table as its hook
    when that is saved."""
    series = {sym: load_yahoo_chart(SAMPLES / f) for sym, f in PREVIEW_FILES.items() if sym != "^GSPC.1d"}
    for sym, _ in PREVIEW_PORTFOLIO:
        f = sample_file(sym)
        if f and sym not in series:
            series[sym] = load_yahoo_chart(f)
    indices = []
    for sym, mn, f in PREVIEW_INDICES:
        s = load_yahoo_chart(SAMPLES / f)
        series.setdefault(sym, s)
        indices.append((sym, mn, s))
    for f in PREVIEW_TER_FILES:
        sym, ter = load_yahoo_ter(SAMPLES / f)
        if sym in series:
            series[sym].ter = ter
    ecb = ecb_monthly_table() if (SAMPLES / ECB_SAMPLE).exists() else ecb_monthly_usd_per_eur
    out = {"series": series, "fx": FxTable.from_series(series["EURUSD=X"], ecb), "indices": indices,
           "daily": load_yahoo_chart(SAMPLES / PREVIEW_FILES["^GSPC.1d"])}
    if (SAMPLES / BENCH_TR[1]).exists():
        out["bench_tr"] = load_yahoo_chart(SAMPLES / BENCH_TR[1])
    return out


def run_preview_portfolio(cfg: Optional[Config] = None, samples: Optional[dict] = None) -> dict:
    """Both modes, with and without dividends, on the sample timeline."""
    cfg = cfg or preview_config()
    S = samples or load_samples()
    series, fx = S["series"], S["fx"]
    # the FX series is not a traded asset, and its "today" stamp is a day later than the exchanges'
    days = timeline([s for s in series.values() if s.sym != "EURUSD=X"], cfg.inception)
    runs = {}
    for mode in ("hold", "rebal"):
        runs[mode] = simulate(cfg, series, fx, days, mode, True, calendar_years)
        runs[mode + "_nodiv"] = simulate(cfg, series, fx, days, mode, False, calendar_years)
    bench = benchmark(S["indices"][0][2], days, cfg.capital, cfg.inception)
    bench_tr = benchmark_in(S["bench_tr"], days, cfg.capital, cfg.inception, fx, cfg.currency) if "bench_tr" in S else None
    return {"days": days, "runs": runs, "bench": bench, "bench_tr": bench_tr, "samples": S, "cfg": cfg}


def main():
    P = run_preview_portfolio()
    cfg = P["cfg"]
    print("positions: " + ", ".join(f"{p.sym} {p.w:g}" for p in cfg.positions))
    print(f"timeline {P['days'][0]} .. {P['days'][-1]}, {len(P['days'])} bars (monthly/weekly/quarterly fixtures)")
    for mode in ("hold", "rebal"):
        L = P["runs"][mode]
        print(f"\n{mode.upper()}  entries: " + ", ".join(f"{s} {d}" for s, d in sorted(L.entries.items(), key=lambda kv: kv[1])))
        for preset in PRESETS:
            p = portfolio_payload(cfg, L, P["runs"][mode + "_nodiv"], P["bench"], preset)
            cagr = f"{p['cagr'] * 100:+.2f} %" if p["cagr"] is not None else "   -   "
            print(f"  {preset:4} {p['from']}..{p['to']}  value {p['value']:10.2f}  chg {p['chg'] * 100:+8.2f} %  "
                  f"sinceStart {p['sinceStart'] * 100:+8.2f} %  cagr {cagr}  mdd {p['mdd'] * 100:6.2f} %  "
                  f"div {p['div']:8.2f}  ter~ {p['terDrag']:7.2f}  cash {p['cash'] * 100:5.2f} %")
        h = holdings_payload(cfg, L, P["samples"]["series"], P["samples"]["fx"])
        for r in h["rows"]:
            ret = f"{r['ret'] * 100:+.1f} %" if r["ret"] is not None else "-"
            print(f"  {r['sym']:8} tgt {r['tgt']:5.1f} now {r['now']:5.1f}  ret {ret}  entry {r['entry']}")
        print(f"  CASH     tgt {h['cash']['tgt']:5.1f} now {h['cash']['now']:5.1f}")


if __name__ == "__main__":
    main()

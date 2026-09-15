"""The thin adapter over tools/market/market_ref.py: the maths, once.

Nothing here computes a value the reference does not: the modes table hands the
run to market_ref.simulate_ext (which, with the default Rules, IS simulate()),
the payload figures come from market_ref's portfolio_payload / holdings_payload,
the returns from market_ref.twr / xirr / annualised, the lines from its
downsample and pack. What this module adds is the plumbing the app needs -
TERs attached from the store and the overrides, proxies spliced, the timeline
chosen, both modes run with and without dividends, the benchmarks picked, the
attribution and the stats block assembled - and three tables:

  MODES   name -> run(cfg, series, fx, days, dividends, bar_years, rules) -> LedgerExt
          "hold" and "rebal" today (rebal follows rules.rebal: calendar, bands, ..)
  LINES   name -> line(result, mode) -> List[float] on the timeline
          pts (V), px (no dividends), bench, gross (the EST counterfactual), tell (portfolio / benchmark)
  (windows.METRICS and windows.WINDOWS for a new metric or window)

Benchmarks (docs/19 §6.2): the panel's `bench` is bench.symbol's price index scaled
to the capital (^GSPC, approved) or, in blend mode, the legs' adjclose held from the
inception and never rebalanced; ^SP500TR and bench.extra go to the stats payload
for Home Assistant. Real terms deflate every line by the HICP ratio to the
inception month.
"""
import datetime as dt
import math
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

from . import stats as st
from . import windows
from ._ref import market_ref as M
from .registry import LINES, MODES
from .timeline import build as build_timeline

Date = dt.date
DAILY_MAX_GAP = 4


# ── the modes table ──────────────────────────────────────────────────────────
def _reference_mode(mode: str) -> Callable:
    def run(cfg: "M.Config", series: Dict[str, "M.Series"], fx: "M.FxTable", days: List[Date],
            dividends: bool = True, bar_years: Optional[Callable] = None, rules: Optional["M.Rules"] = None) -> "M.LedgerExt":
        return M.simulate_ext(cfg, series, fx, days, mode, dividends, bar_years, rules)
    run.__name__ = "run_" + mode
    return run


MODES.register("hold", _reference_mode("hold"))
MODES.register("rebal", _reference_mode("rebal"))


# ── the result of one compute ────────────────────────────────────────────────
@dataclass
class Result:
    cfg: "M.Config"
    series: Dict[str, "M.Series"]
    fx: "M.FxTable"
    days: List[Date]
    runs: Dict[str, "M.LedgerExt"]               # "<mode>" and "<mode>_nodiv"
    bench: Optional[List[float]] = None          # the panel's benchmark line on the timeline
    bench_sym: Optional[str] = None              # its name: a symbol, or "blend"
    benches: Dict[str, List[float]] = field(default_factory=dict)   # every benchmark by name (HA)
    ter_src: Dict[str, str] = field(default_factory=dict)
    modes: List[str] = field(default_factory=list)
    rules: "M.Rules" = field(default_factory=lambda: M.Rules())
    real: Optional[Callable[[Date], Optional[float]]] = None        # HICP factor(d) when real terms are on
    classes: Dict[str, Optional[str]] = field(default_factory=dict)

    @property
    def asof(self) -> Date:
        return self.days[-1]

    def ledger(self, mode: str, dividends: bool = True) -> "M.LedgerExt":
        return self.runs[mode if dividends else mode + "_nodiv"]


def attach_ter(series: Dict[str, "M.Series"], overrides_fraction: Dict[str, float], stored_ter: Dict[str, Optional[float]],
               stored_src: Dict[str, str]) -> Dict[str, str]:
    """Each Series' `ter`: the portal's override first, then the store's (Yahoo's). Returns the source per symbol."""
    src: Dict[str, str] = {}
    for sym, s in series.items():
        if sym in overrides_fraction:
            s.ter, src[sym] = overrides_fraction[sym], "override"
        elif stored_ter.get(sym) is not None:
            s.ter, src[sym] = stored_ter[sym], stored_src.get(sym) or "yahoo"
        else:
            s.ter = None
    return src


def splice_proxy(fund: "M.Series", proxy: "M.Series") -> "M.Series":
    """A young fund extended backwards with its proxy (docs/19 §5 "Proxy / backfill", Portfolio
    Visualizer's backfilling): the proxy's closes before the fund's first bar, scaled so the two
    meet at that bar; the proxy's dividends in that stretch, scaled the same way. The result is
    marked meta["proxy"]; the fund's own bars are untouched."""
    if not proxy.dates or not fund.dates or proxy.first >= fund.first:
        return fund
    ratio = fund.closes[0] / (proxy.close_on(fund.first) or proxy.closes[0])
    i = M.bisect.bisect_left(proxy.dates, fund.first)
    dates = proxy.dates[:i] + fund.dates
    closes = [c * ratio for c in proxy.closes[:i]] + fund.closes
    adj = None
    if fund.adj and proxy.adj:
        aratio = fund.adj[0] / proxy.adj[min(i, len(proxy.adj) - 1)]
        adj = [a * aratio for a in proxy.adj[:i]] + fund.adj
    divs = [(d, a * ratio) for d, a in proxy.dividends if d < fund.first] + list(fund.dividends)
    meta = dict(fund.meta)
    meta["proxy"] = {"sym": proxy.sym, "until": fund.first.isoformat(), "ratio": ratio}
    return M.Series(fund.sym, fund.currency, dates, closes, sorted(divs), fund.ter, meta, adj)


def fx_table(series: Dict[str, "M.Series"], ecb_rate: Optional[Callable[[int, int], Optional[float]]] = None) -> "M.FxTable":
    """EURUSD=X from the series, with the ECB hook for 1999-2003 when the table is loaded."""
    fx = series.get("EURUSD=X")
    if fx is None:
        return M.FxTable([], [], ecb_rate or M.ecb_monthly_usd_per_eur)
    return M.FxTable(fx.dates, fx.closes, ecb_rate or M.ecb_monthly_usd_per_eur)


def bar_years_for(days: List[Date]) -> Optional[Callable]:
    """The TER drag's year fraction per bar: None (1/252, the design's per-trading-day rule) on daily
    bars; the calendar span between bars when the timeline is coarser (the monthly fixtures of the
    previews), judged by the median spacing (over DAILY_MAX_GAP days = not daily)."""
    if len(days) < 3:
        return None
    gaps = sorted((b - a).days for a, b in zip(days, days[1:]))
    return M.calendar_years if gaps[len(gaps) // 2] > DAILY_MAX_GAP else None


# ── benchmarks ───────────────────────────────────────────────────────────────
def start_day(days: List[Date], inception: Date) -> Date:
    """The ledger's first day: the first timeline day on or after the inception (the inception when none is)."""
    i = M.bisect.bisect_left(days, inception)
    return days[i] if i < len(days) else inception


def price_benchmark(idx: "M.Series", days: List[Date], capital: float, inception: Date, adj: bool = False,
                    fx: Optional["M.FxTable"] = None, currency: Optional[str] = None) -> Optional[List[float]]:
    """market_ref.benchmark on the close (or the adjclose for a total-return series), converted into
    the portfolio currency when `fx` and `currency` are given (owner, 2026-09-15 11:43: every line
    starts at C in the portfolio's money; before 2003-12 the ECB rates carry the conversion). A day
    without a rate refuses (NoFxRate) rather than inventing one."""
    values = idx.adj if adj and idx.adj else idx.closes
    tr = M.Series(idx.sym, idx.currency, idx.dates, values, [], None, {})
    # the base is the ledger's first day, the first trading day on or after the inception (docs/18), not the calendar
    # inception: 2000-01-01 is a Saturday, and on daily bars close_on() of it is None, which dropped the line silently
    start = start_day(days, inception)
    if fx is None or currency is None or idx.currency == currency:
        return M.benchmark(tr, days, capital, start)
    base = tr.close_on(start)
    if base is None:
        return None
    base *= fx.factor(idx.currency, currency, start)
    return [capital * tr.close_on(d) * fx.factor(idx.currency, currency, d) / base for d in days]


def blend_benchmark(legs: List[dict], series: Dict[str, "M.Series"], days: List[Date], capital: float, inception: Date,
                    fx: Optional["M.FxTable"] = None, currency: Optional[str] = None) -> Optional[List[float]]:
    """The legs' adjclose (total return), each leg capital x w from the inception, never rebalanced, every leg in
    the portfolio currency when `fx` and `currency` are given (audit M4): value = share x (adj x X)(d) / (adj x X)(base).
    A leg without a bar at the inception sits flat (cash) until its first bar, which becomes its base. A day
    without an FX rate refuses (NoFxRate): no blend line rather than one in the wrong money."""
    out = [0.0] * len(days)
    got = False
    for leg in legs:
        s = series.get(leg["sym"])
        if s is None or not s.dates:
            continue
        vals = s.adj if s.adj else s.closes
        tr = M.Series(s.sym, s.currency, s.dates, vals, [], None, {})
        convert = fx is not None and currency is not None and s.currency != currency

        def money(d: Date, tr=tr, convert=convert, cur=s.currency) -> Optional[float]:
            c = tr.close_on(d)
            if c is None:
                return None
            return c * fx.factor(cur, currency, d) if convert else c
        share = capital * leg["w"] / 100.0
        base = money(start_day(days, inception))
        for i, d in enumerate(days):
            if base is None:
                if tr.trades(d):
                    base = money(tr.first)
                    out[i] += share * money(d) / base
                else:
                    out[i] += share
            else:
                out[i] += share * money(d) / base
        got = True
    return out if got else None


# ── compute ──────────────────────────────────────────────────────────────────
def compute(cfg: "M.Config", series: Dict[str, "M.Series"], fx: "M.FxTable", bench_sym: Optional[str] = None,
            modes: Optional[List[str]] = None, timeline_kind: str = "union", bar_years="auto",
            days: Optional[List[Date]] = None, rules: Optional["M.Rules"] = None, blend: Optional[List[dict]] = None,
            extra_bench: Optional[List[str]] = None, real: Optional[Callable[[Date], Optional[float]]] = None,
            classes: Optional[Dict[str, Optional[str]]] = None, bench_in_portfolio_currency: bool = True) -> Result:
    """Both modes (or `modes`), with and without dividends, on the timeline; the benchmark lines.
    `bar_years` "auto" picks bar_years_for(days). `blend` (legs) makes the panel's benchmark a blend.
    `bench_in_portfolio_currency` False keeps the reference's own-currency benchmark (the previews)."""
    modes = list(modes or MODES.names())
    rules = rules or M.Rules()
    missing = [p.sym for p in cfg.positions if p.sym not in series]
    if missing:
        raise KeyError("no history for %s" % ", ".join(missing))
    # The timeline is the market's calendar, not the holdings': every traded series (indices,
    # tickers, positions) contributes its days, so a fund that lists late is reserved cash on a
    # timeline that already runs (the reference's run_preview_portfolio does the same).
    days = days if days is not None else build_timeline(series, cfg.inception, timeline_kind, currency=cfg.currency)
    if not days:
        raise ValueError("no trading days from %s" % cfg.inception)
    if bar_years == "auto":
        bar_years = bar_years_for(days)
    runs: Dict[str, "M.LedgerExt"] = {}
    for mode in modes:
        run = MODES.get(mode)
        runs[mode] = run(cfg, series, fx, days, True, bar_years, rules)
        runs[mode + "_nodiv"] = run(cfg, series, fx, days, False, bar_years, rules)
    benches: Dict[str, List[float]] = {}
    bench_err: List[str] = []
    bfx, bcur = (fx, cfg.currency) if bench_in_portfolio_currency else (None, None)
    if blend:
        try:
            line = blend_benchmark(blend, series, days, cfg.capital, cfg.inception, bfx, bcur)
        except M.NoFxRate:
            line = None
            bench_err.append("blend: no FX rate")
        if line:
            benches["blend"] = line
    tried = set()
    for sym in [bench_sym] + list(extra_bench or []):
        if not sym or sym not in series or sym in tried:
            continue                                      # each symbol once, whether it is the primary, an extra, or both
        tried.add(sym)
        try:
            line = price_benchmark(series[sym], days, cfg.capital, cfg.inception, adj=bool(series[sym].dividends), fx=bfx, currency=bcur)
        except M.NoFxRate:
            line = None                                   # refused: never a line in its own currency (audit M4)
            bench_err.append("%s: no FX rate" % sym)
        if line:
            benches[sym] = line
        elif not any(e.startswith(sym + ":") for e in bench_err):
            bench_err.append("%s: no bar on %s" % (sym, start_day(days, cfg.inception)))   # said, never silent
    primary = "blend" if "blend" in benches else (bench_sym if bench_sym in benches else None)
    result = Result(cfg, series, fx, days, runs, benches.get(primary) if primary else None, primary, benches, {}, modes,
                    rules, real, dict(classes or {}))
    result.bench_err = bench_err
    return result


# ── the lines table ──────────────────────────────────────────────────────────
@LINES.register("pts")
def line_value(result: Result, mode: str) -> List[float]:
    return result.ledger(mode).v


@LINES.register("px")
def line_no_dividends(result: Result, mode: str) -> List[float]:
    return result.ledger(mode, dividends=False).v


@LINES.register("bench")
def line_benchmark(result: Result, mode: str) -> Optional[List[float]]:
    return result.bench


@LINES.register("gross")
def line_gross(result: Result, mode: str) -> List[float]:
    """The EST counterfactual: the fee drag added back to the ledger (V + TERdrag so far). The
    design's per-position net x exp(TER x years) compounds the fee; this first-order form is what
    the aggregate ledger allows. Off by default; labelled EST on the panel."""
    L = result.ledger(mode)
    return [v + t for v, t in zip(L.v, L.ter)]


@LINES.register("tell")
def line_telltale(result: Result, mode: str) -> Optional[List[float]]:
    """Simba's telltale: portfolio / benchmark, scaled to the capital (flat when they match)."""
    if not result.bench:
        return None
    L = result.ledger(mode)
    return [result.cfg.capital * v / b if b else result.cfg.capital for v, b in zip(L.v, result.bench)]


def deflate(days: List[Date], values: List[float], factor: Callable[[Date], Optional[float]]) -> List[float]:
    out, f_last = [], 1.0
    for d, v in zip(days, values):
        f = factor(d)
        f_last = f if f is not None else f_last
        out.append(v * f_last)
    return out


def _window(result: Result, mode: str, preset: str) -> Tuple[Date, int, int, List[Date], List[float]]:
    L = result.ledger(mode)
    start = windows.start_of(preset, L.days[-1], result.cfg.inception, L.days)
    wd, wv = windows.series_window(L.days, L.v, start, L.days[-1])
    i0 = max(L.index_on(start), 0)
    i1 = len(L.days) - 1
    return start, i0, i1, wd, wv


def _flows(L) -> List[float]:
    """A plug-in mode may return market_ref's plain Ledger: no flows means zero flows."""
    return getattr(L, "flows", None) or [0.0] * len(L.v)


def fx_effect(result: Result, mode: str, i0: int, i1: int) -> Optional[float]:
    """The FX part of the window's TWR: the TWR minus the TWR with every rate frozen at the window's
    start (docs/19 §6.7: Sharesight's currency component, Wealthfolio's fx_effect). 0 for a
    single-currency portfolio; None when the window's first day has no rate (no ECB table)."""
    L = result.ledger(mode)
    if i1 <= i0 or not getattr(L, "local", None):
        return 0.0
    cur = result.cfg.currency
    foreign = [c for c in L.local if c != cur]
    if not foreign:
        return 0.0
    day0 = L.days[i0]
    try:
        rate0 = {c: result.fx.factor(c, cur, day0) for c in foreign}
    except M.NoFxRate:
        return None
    frozen = []
    for i in range(i0, i1 + 1):
        v = L.cash[i] + (L.local[cur][i] if cur in L.local else 0.0)
        for c in foreign:
            v += L.local[c][i] * rate0[c]
        frozen.append(v)
    flows = _flows(L)
    return M.twr(L.v, flows, i0, i1) - M.twr(frozen, flows[i0:i1 + 1], 0, len(frozen) - 1)


def contributions(result: Result, mode: str, i0: int, i1: int) -> Dict[str, float]:
    """Each holding's contribution to the window return: the daily-linked sum of its weight at the
    previous close times its price return that day (the weight x return attribution of docs/19 §6.7,
    linked daily so rebalances and flows inside the window are honoured)."""
    L = result.ledger(mode)
    cur = result.cfg.currency
    out: Dict[str, float] = {}
    for sym, vals in (getattr(L, "values", None) or {}).items():
        s = result.series[sym]
        total = 0.0
        for i in range(i0 + 1, i1 + 1):
            v_prev = L.v[i - 1]
            if v_prev <= 0 or vals[i - 1] <= 0:
                continue
            p0 = s.close_on(L.days[i - 1])
            p1 = s.close_on(L.days[i])
            if not p0 or p1 is None:
                continue
            r = (p1 * result.fx.factor(s.currency, cur, L.days[i])) / (p0 * result.fx.factor(s.currency, cur, L.days[i - 1])) - 1.0
            total += vals[i - 1] / v_prev * r
        out[sym] = total
    return out


def has_flows(L) -> bool:
    return any(_flows(L))


def unit_values(L) -> List[float]:
    """The ledger's unit value (NAV per unit), on V's scale: V on the first day, then each day's time-weighted
    factor on market_ref.twr's convention (inflows at the start of the day, outflows at its end). Contributions
    and withdrawals move V, not this. Without flows it IS L.v (the same list), so the no-flow figures stay the
    reference's to the last digit. Every return and risk figure is taken on it (audit M5); V stays for the value
    and the drawn line."""
    flows = _flows(L)
    if not any(flows):
        return L.v
    u = [L.v[0]]
    for i in range(1, len(L.v)):
        cf = flows[i]
        denom = L.v[i - 1] + max(cf, 0.0)
        num = L.v[i] + max(-cf, 0.0)
        u.append(u[-1] * num / denom if denom > 0 else u[-1])
    return u


def perf_window(result: Result, mode: str, start: Date, real: bool = True) -> Tuple[List[Date], List[float]]:
    """The unit values of the window (deflated in real terms when on)."""
    L = result.ledger(mode)
    u = unit_values(L)
    if real and result.real is not None:
        u = deflate(L.days, u, result.real)
    return windows.series_window(L.days, u, start, L.days[-1])


def returns_block(result: Result, mode: str, i0: int, i1: int, wd: List[Date], preset: Optional[str] = None) -> dict:
    """twr (the window, flows out); ann for the long presets from one year; with flows on, xirr_ann
    since inception and flows: true (owner, 11:43: without flows xirr_ann is null); fx."""
    L = result.ledger(mode)
    flows = _flows(L)
    tw = M.twr(L.v, flows, i0, i1)
    if not isinstance(L, M.LedgerExt):
        L = M.LedgerExt(L.mode, L.days, L.v, L.px, L.cash, L.div, L.ter, L.contrib, L.qty, L.entries, L.entry_px, L.events, flows=flows)
    with_flows = has_flows(L)
    ann = windows.metric_ann(wd, [L.v[i0], L.v[i1]], {"preset": preset}) if len(wd) > 1 else None
    if ann is not None and abs(L.v[i0]) > 0:
        ann = M.annualised(tw, M.years_between(wd[0], wd[-1]))          # the TWR annualised, not the raw ratio
    return {"twr": tw, "ann": ann, "flows": with_flows,
            "xirr_ann": M.ledger_xirr(result.cfg, L, i1) if with_flows else None, "fx_effect": fx_effect(result, mode, i0, i1)}


def base_portfolio_payload(result: Result, mode: str, preset: str) -> dict:
    """market_ref.portfolio_payload for the six presets it knows; the same assembly with windows.start_of
    for the extra ones (WTD MTD 1M 3M 6M), which its window_start does not parse."""
    L, Ln = result.ledger(mode), result.ledger(mode, dividends=False)
    if preset in M.PRESETS:
        return M.portfolio_payload(result.cfg, L, Ln, result.bench, preset)
    asof = L.days[-1]
    start = windows.start_of(preset, asof, result.cfg.inception, L.days)
    wd, wv = windows.series_window(L.days, L.v, start, asof)
    _, wpx = windows.series_window(Ln.days, Ln.v, start, asof)
    wb = windows.series_window(L.days, result.bench, start, asof)[1] if result.bench else []
    r = M.report(wd, wv)
    lines = {"pts": windows.downsample(wd, wv), "px": windows.downsample(wd, wpx)}
    if wb:
        lines["bench"] = windows.downsample(wd, wb)
    lo = min(min(p) for p in lines.values())
    hi = max(max(p) for p in lines.values())
    i0, i1 = max(L.index_on(start), 0), len(L.days) - 1
    out = {"v": 1, "cur": result.cfg.currency, "mode": L.mode, "preset": preset, "from": wd[0].isoformat(), "to": wd[-1].isoformat(),
           "value": L.v[i1], "chg": r["chg"], "sinceStart": L.v[i1] / (result.cfg.capital + L.contrib[i1]) - 1.0,
           "cagr": r["cagr"], "mdd": r["mdd"], "div": L.div[i1] - L.div[i0], "terDrag": L.ter[i1] - L.ter[i0],
           "cash": L.cash[i1] / L.v[i1] if L.v[i1] else 0.0, "n": len(lines["pts"]), "min": lo, "max": hi}
    for k, pts in lines.items():
        out[k] = M.pack_pts(pts, lo, hi)[2]
    return out


def base_index_payload(s: "M.Series", preset: str, asof: Date, inception: Date, name: Optional[str]) -> Optional[dict]:
    """market_ref.index_payload for the six presets; the same assembly with windows.start_of for the extras."""
    if preset in M.PRESETS:
        return M.index_payload(s, preset, asof, inception, name)
    end = min(asof, s.last)
    wd, wv = windows.series_window(s.dates, s.closes, windows.start_of(preset, end, inception, s.dates), end)
    if not wv:
        return None
    r = M.report(wd, wv)
    pts = windows.downsample(wd, wv)
    lo, hi, b64 = M.pack_pts(pts)
    return {"v": 1, "sym": s.sym, "name": name or s.sym, "cur": s.currency, "preset": preset,
            "from": wd[0].isoformat(), "to": wd[-1].isoformat(), "last": r["last"], "chg": r["chg"],
            "hi": r["hi"], "lo": r["lo"], "n": len(pts), "min": lo, "max": hi, "pts": b64, "cagr": r["cagr"], "mdd": r["mdd"]}


ROUND = 6       # decimals kept on the portfolio payload's floats: nothing the panel prints changes, 4 lines fit


def portfolio_payload(result: Result, mode: str, preset: str, lines_on: Optional[Dict[str, bool]] = None,
                      measure: str = "twr", scale: str = "linear", mdd_basis: str = "daily") -> dict:
    """`portfolio/<mode>/<preset>`: market_ref's figures, the lines from LINES, one shared axis, plus
    the v2 fields twr / ann / xirr / fx / measure. With px and bench on, gross and tell off, linear
    scale and no flows, the v1 fields are market_ref.portfolio_payload byte for byte."""
    lines_on = dict({"bench": True, "gross": False, "tell": False}, **(lines_on or {}))
    lines_on["px"] = True                   # the panel requires px (market_model.cpp portfolioFrom): never optional (audit M6)
    if lines_on.get("tell") and result.bench:
        lines_on["bench"] = False          # the telltale IS portfolio / benchmark: it takes the benchmark's slot (4 lines at most)
    L, Ln = result.ledger(mode), result.ledger(mode, dividends=False)
    base = base_portfolio_payload(result, mode, preset)
    start, i0, i1, wd, wv = _window(result, mode, preset)
    lines: Dict[str, List[float]] = {}
    for name in LINES.names():
        if name != "pts" and not lines_on.get(name, False):
            continue
        values = LINES.get(name)(result, mode)
        if not values:
            continue
        if result.real is not None:
            values = deflate(L.days, values, result.real)
        _, wvals = windows.series_window(L.days, values, start, L.days[-1])
        if wvals:
            pts = windows.downsample(wd, wvals)
            if scale == "log":
                pts = [math.log10(v) if v > 0 else 0.0 for v in pts]
            lines[name] = pts
    lo = round(min(min(p) for p in lines.values()), ROUND)
    hi = round(max(max(p) for p in lines.values()), ROUND)
    out = {k: (round(v, ROUND) if isinstance(v, float) else v) for k, v in base.items()
           if k not in ("pts", "px", "bench", "gross", "min", "max")}
    out["min"], out["max"] = lo, hi
    for name, pts in lines.items():
        out[name] = M.pack_pts(pts, lo, hi)[2]
    rb = returns_block(result, mode, i0, i1, wd, preset)
    out["chg"] = round(rb["twr"], ROUND)        # the window figure is the TWR: identical without flows
    out["twr"] = out["chg"]
    if rb["flows"]:
        # with flows xirr_ann is always there; null only when the solver found no rate (counted in status, the panel shows XIRR --)
        out["flows"] = True
        out["xirr_ann"] = round(rb["xirr_ann"], 6) if rb["xirr_ann"] is not None else None
    out["fx_effect"] = round(rb["fx_effect"], 6) if rb["fx_effect"] is not None else None
    out["measure"] = measure
    # returns and risk on the unit value (audit M5): with flows, V's own ratios count the money put in as return
    pd_, pv = perf_window(result, mode, start, real=False)
    if rb["flows"]:
        rep = M.report(pd_, pv)
        out["cagr"] = round(rep["cagr"], ROUND) if rep["cagr"] is not None else None
        out["mdd"] = round(rep["mdd"], ROUND)
        withdrawn = getattr(L, "withdrawn", None)
        if withdrawn and withdrawn[i1]:
            out["sinceStart"] = round((L.v[i1] + withdrawn[i1]) / (result.cfg.capital + L.contrib[i1]) - 1.0, ROUND)
    dd = st.max_drawdown(pd_, pv, mdd_basis)     # the drawdown with its dates (owner, 11:43), on mdd_basis
    out["mdd"] = round(dd["mdd"], ROUND) if mdd_basis != "daily" else out["mdd"]
    out["mdd_peak"], out["mdd_trough"], out["mdd_recovery"] = dd["peak"], dd["trough"], dd["recovery"]
    if result.real is not None:
        rv = deflate(L.days, L.v, result.real)
        out["value"], out["real"] = rv[i1], True
    if scale == "log":
        out["scale"] = "log"
    ctx = {"preset": preset, "mode": mode, "ledger": L, "cfg": result.cfg}
    for name in windows.extra_metric_names():
        if name == "ann":
            out["ann"] = round(rb["ann"], 6) if rb["ann"] is not None else None
            continue
        x = windows.METRICS.get(name)(wd, wv, ctx)
        out[name] = round(x, 6) if isinstance(x, float) else x
    return out


def holdings_payload(result: Result, mode: str, out_of_band: bool = True) -> dict:
    """`holdings/<mode>`: market_ref's rows plus, per row, `cls` (the asset class), `lo` / `hi` (the
    Swedroe band, percent) and `out` when the holding is outside it; `rebal` counts the rebalance
    events; `classes` sums the current shares by class."""
    L = result.ledger(mode)
    p = M.holdings_payload(result.cfg, L, result.series, result.fx)
    rebal_dates = getattr(L, "rebal_dates", [])
    classes: Dict[str, float] = {}
    for row in p["rows"]:
        cls = result.classes.get(row["sym"])
        if cls:
            row["cls"] = cls
            classes[cls] = classes.get(cls, 0.0) + row["now"]
        lo, hi = M.band_bounds(row["tgt"], result.rules.band_abs_pts, result.rules.band_rel_pct)
        row["lo"], row["hi"] = round(lo, 2), round(hi, 2)
        if out_of_band and row["entry"] and (row["now"] < lo or row["now"] > hi):
            row["out"] = True
        # four decimals of a percent share and of a return: no digit the panel or HA prints changes,
        # and sixteen rows with their class and band stay under the bus budget
        row["now"] = round(row["now"], 4)
        if row["ret"] is not None:
            row["ret"] = round(row["ret"], 4)
    if p["cash"]["now"]:
        classes["cash"] = classes.get("cash", 0.0) + p["cash"]["now"]
    p["rebal"] = len(rebal_dates)
    if classes:
        p["classes"] = {k: round(v, 2) for k, v in classes.items()}
    return p


def stats_payload(result: Result, mode: str, preset: str, rf_pct: float = 0.0, basis: str = "daily") -> dict:
    """`stats/<mode>/<preset>` (docs/18 "Settings after the research": the payload budget puts the
    new figures on their own topic): the returns block, the risk block, the benchmarks, the
    per-holding attribution and bands, the classes, the ledger's event counts inside the window."""
    L = result.ledger(mode)
    start, i0, i1, wd, wv = _window(result, mode, preset)
    pd_, wvals = perf_window(result, mode, start)          # the unit value: every statistic below (audit M5)
    rb = returns_block(result, mode, i0, i1, wd, preset)
    cagr = M.report(pd_, wvals)["cagr"]
    out = {"mode": mode, "preset": preset, "from": wd[0].isoformat(), "to": wd[-1].isoformat()}
    out.update(rb)
    out.update(st.block(pd_, wvals, cagr, rf_pct, basis))
    benches = []
    for name, line in result.benches.items():
        _, wb = windows.series_window(L.days, line, start, L.days[-1])
        if len(wb) > 1 and wb[0] > 0:
            chg = wb[-1] / wb[0] - 1.0
            benches.append({"sym": name, "chg": round(chg, 5), "ann": M.annualised(chg, M.years_between(wd[0], wd[-1]))})
    out["bench"] = benches[:3]
    contrib = contributions(result, mode, i0, i1)
    if contrib:
        # sorted by contribution, top first: the list's order is the sort index (docs/19 §6.7)
        out["contrib"] = [[k, round(v, 4)] for k, v in sorted(contrib.items(), key=lambda kv: -kv[1])]
    V = L.v[i1]
    values = getattr(L, "values", None) or {}
    bands = {p.sym: M.band_bounds(p.w, result.rules.band_abs_pts, result.rules.band_rel_pct) for p in result.cfg.positions}
    out["outOfBand"] = [p.sym for p in result.cfg.positions if p.sym in L.entries and V and p.sym in values and
                        not (bands[p.sym][0] <= 100.0 * values[p.sym][i1] / V <= bands[p.sym][1])]
    classes: Dict[str, float] = {}
    if V:
        for p in result.cfg.positions:
            cls = result.classes.get(p.sym)
            if cls and p.sym in values:
                classes[cls] = classes.get(cls, 0.0) + 100.0 * values[p.sym][i1] / V
        classes["cash"] = classes.get("cash", 0.0) + 100.0 * L.cash[i1] / V
    out["classes"] = {k: round(v, 2) for k, v in classes.items()}
    out["rebal"] = sum(1 for d in getattr(L, "rebal_dates", []) if wd[0] <= d <= wd[-1])
    out["trades"] = getattr(L, "trades", 0)
    for key in ("costs", "tax", "withdrawn"):
        arr = getattr(L, key, None)
        if arr and arr[i1] - arr[i0]:
            out[key] = round(arr[i1] - arr[i0], 2)
    out["real"] = result.real is not None
    return _round_floats(out)


def _round_floats(x, nd: int = 5):
    if isinstance(x, float):
        return round(x, nd)
    if isinstance(x, dict):
        return {k: _round_floats(v, nd) for k, v in x.items()}
    if isinstance(x, list):
        return [_round_floats(v, nd) for v in x]
    return x


def figures(result: Result, presets=("MAX", "5Y")) -> Dict[str, Dict[str, dict]]:
    """{mode: {preset: payload without the lines}} - the golden figures for the report."""
    out: Dict[str, Dict[str, dict]] = {}
    for mode in result.modes:
        out[mode] = {}
        for preset in presets:
            p = portfolio_payload(result, mode, preset)
            out[mode][preset] = {k: v for k, v in p.items() if k not in ("pts", "px", "bench", "gross", "tell")}
    return out


def clear_symbol_leaves(publisher, symbols, presets) -> None:
    """The retained topics of symbols that left the watch lists: index/ticker windows and the intraday line."""
    for sym in symbols:
        for preset in presets:
            publisher.clear("index/%s/%s" % (sym, preset))
            publisher.clear("ticker/%s/%s" % (sym, preset))
        publisher.clear("intraday/%s" % sym)

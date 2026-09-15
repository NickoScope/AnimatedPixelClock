"""Windows (YTD, 1Y, 3Y, 5Y, 10Y, MAX) and the figures reported for one windowed line.

Both are tables. WINDOWS maps a preset name to `start(asof, inception, days)`;
the six of the design delegate to market_ref.window_start, so the previews and
the app agree. METRICS maps a field name to `fn(wd, wv, ctx) -> value`; the five
the design reports (chg, hi, lo, cagr, mdd) come from market_ref.report, and a
new figure (volatility, a rolling return, ...) is one more registration - it
appears in every index, ticker and portfolio payload and, by name, in the HA
sensors that ask for it. `ctx` carries what a metric may need beyond the
window: the preset, the inception, the ledger (for portfolio lines) or None.
"""
import datetime as dt
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from ._ref import market_ref as M
from .registry import METRICS, WINDOWS

Date = dt.date
PRESETS = tuple(M.PRESETS)
BASE_METRICS = ("chg", "hi", "lo", "cagr", "mdd")     # the design's, from market_ref.report


def _preset_start(preset: str) -> Callable:
    def start(asof: Date, inception: Date, days: List[Date]) -> Date:
        return M.window_start(preset, asof, inception, days)
    start.__name__ = "start_" + preset
    return start


for _p in PRESETS:
    WINDOWS.register(_p, _preset_start(_p))


# The extra presets (docs/19 §5 "Display": Ghostfolio WTD MTD, IBKR 1M, ffn 3m 6m), each off by default.
def _months_back(n: int) -> Callable:
    def start(asof: Date, inception: Date, days: List[Date]) -> Date:
        return M.add_months(asof, -n)
    start.__name__ = "start_%dM" % n
    return start


def start_mtd(asof: Date, inception: Date, days: List[Date]) -> Date:
    """The last trading day of the previous month (YTD's rule, a month wide)."""
    prev = [d for d in days if (d.year, d.month) < (asof.year, asof.month)]
    return prev[-1] if prev else days[0]


def start_wtd(asof: Date, inception: Date, days: List[Date]) -> Date:
    """The last trading day of the previous ISO week."""
    wk = asof.isocalendar()[:2]
    prev = [d for d in days if d.isocalendar()[:2] < wk]
    return prev[-1] if prev else days[0]


WINDOWS.register("1M", _months_back(1))
WINDOWS.register("3M", _months_back(3))
WINDOWS.register("6M", _months_back(6))
WINDOWS.register("MTD", start_mtd)
WINDOWS.register("WTD", start_wtd)
EXTRA_PRESETS = ("1M", "3M", "6M", "MTD", "WTD")


def start_of(preset: str, asof: Date, inception: Date, days: List[Date]) -> Date:
    return WINDOWS.get(preset)(asof, inception, days)


def series_window(dates: List[Date], values: List[float], start: Date, end: Date) -> Tuple[List[Date], List[float]]:
    return M.window_series(dates, values, start, end)


def downsample(wd: List[Date], wv: List[float], n: int = M.POINTS) -> List[float]:
    return M.downsample(wd, wv, n)


# ── the metrics table ────────────────────────────────────────────────────────
def _from_report(field: str) -> Callable:
    def metric(wd: List[Date], wv: List[float], ctx: Optional[dict] = None):
        return M.report(wd, wv)[field]
    metric.__name__ = "metric_" + field
    return metric


for _f in BASE_METRICS:
    METRICS.register(_f, _from_report(_f))


ANN_PRESETS = ("1Y", "3Y", "5Y", "10Y", "MAX")     # owner, 2026-09-15 11:43: never WTD MTD 1M 3M 6M YTD


def metric_ann(wd: List[Date], wv: List[float], ctx: Optional[dict] = None) -> Optional[float]:
    """The annualised window return for 1Y, 3Y, 5Y, 10Y and a MAX of one year or longer, None
    otherwise (GIPS 2.A.12; docs/19 §6.4; the owner's decision). `cagr` keeps its three-year rule."""
    if len(wv) < 2 or wv[0] <= 0:
        return None
    if ctx and ctx.get("preset") and ctx["preset"] not in ANN_PRESETS:
        return None
    return M.annualised(wv[-1] / wv[0] - 1.0, M.years_between(wd[0], wd[-1]))


METRICS.register("ann", metric_ann)
BASE_AND_ANN = BASE_METRICS + ("ann",)


def metrics(wd: List[Date], wv: List[float], ctx: Optional[dict] = None, names: Optional[Sequence[str]] = None) -> Dict[str, object]:
    """Every registered metric (or `names`) on one windowed line. The base five are computed
    once through market_ref.report; the rest call their functions."""
    names = list(names) if names is not None else METRICS.names()
    out: Dict[str, object] = {}
    base = None
    for n in names:
        if n in BASE_METRICS:
            base = base if base is not None else M.report(wd, wv)
            out[n] = base[n]
        else:
            out[n] = METRICS.get(n)(wd, wv, ctx)
    return out


def extra_metric_names() -> List[str]:
    """The metrics beyond market_ref.report's five: `ann` and whatever else is registered."""
    return [n for n in METRICS.names() if n not in BASE_METRICS]

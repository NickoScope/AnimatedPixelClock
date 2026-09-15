"""Risk and return statistics on a value series: drawdowns with dates, volatility, ratios, years, rolling.

Formulas, from the sources docs/19 names (read, not recalled):
- Periodicity: risk statistics from MONTHLY returns (GIPS 4.A.1 via docs/19 §4: the three-year
  annualised standard deviation is computed from monthly returns; Portfolio Visualizer does the
  same and takes its max drawdown from month-end balances).
- quantstats/stats.py (deepwiki, 2026-09-15): volatility = std(returns, ddof=1) x sqrt(periods);
  sharpe = mean(r - rf) / std(r - rf, ddof=1) x sqrt(periods); sortino's downside deviation =
  sqrt(sum(min(r - rf, 0)^2) / N) over ALL N returns, x sqrt(periods); calmar = CAGR / |MDD|;
  max_drawdown = min(V / running max) - 1; a drawdown episode runs from the first day the
  drawdown is below zero to the first day it is back at zero (the recovery), its valley is the
  argmin, its length (end - start).days + 1.
Here periods = 12 and the risk-free rate is a setting (default 0, as quantstats).

Nothing here is on the panel but MDD (docs/19 §5 "Risk metrics"): the rest feeds the
stats/<mode>/<preset> topic and Home Assistant's sensors.
"""
import datetime as dt
import math
from typing import Dict, List, Optional, Tuple

from ._ref import market_ref as M

Date = dt.date
PERIODS = 12


def month_ends(days: List[Date], values: List[float]) -> Tuple[List[Date], List[float]]:
    """The window's starting balance, then the last bar of each calendar month (the final bar always counts: it
    is the latest balance). Without the first point a window that starts mid-month loses its first, partial
    month from the returns and the drawdown (audit MINOR 12)."""
    if not days:
        return [], []
    md, mv = [days[0]], [values[0]]
    for i, d in enumerate(days):
        nxt = days[i + 1] if i + 1 < len(days) else None
        if i > 0 and (nxt is None or (nxt.year, nxt.month) != (d.year, d.month)):
            md.append(d)
            mv.append(values[i])
    return md, mv


def simple_returns(values: List[float]) -> List[float]:
    return [b / a - 1.0 for a, b in zip(values, values[1:]) if a > 0]


def monthly_returns(days: List[Date], values: List[float]) -> List[float]:
    return simple_returns(month_ends(days, values)[1])


def _std(xs: List[float], ddof: int = 1) -> Optional[float]:
    n = len(xs)
    if n - ddof <= 0:
        return None
    m = sum(xs) / n
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (n - ddof))


def vol_ann(monthly: List[float]) -> Optional[float]:
    sd = _std(monthly)
    return sd * math.sqrt(PERIODS) if sd is not None else None


def sharpe(monthly: List[float], rf_pct: float = 0.0) -> Optional[float]:
    ex = [r - rf_pct / 100.0 / PERIODS for r in monthly]
    sd = _std(ex)
    if not sd:
        return None
    return (sum(ex) / len(ex)) / sd * math.sqrt(PERIODS)


def sortino(monthly: List[float], rf_pct: float = 0.0) -> Optional[float]:
    ex = [r - rf_pct / 100.0 / PERIODS for r in monthly]
    if not ex:
        return None
    downside = math.sqrt(sum(min(x, 0.0) ** 2 for x in ex) / len(ex))
    if downside == 0:
        return None
    return (sum(ex) / len(ex)) / downside * math.sqrt(PERIODS)


def calmar(cagr: Optional[float], mdd: Optional[float]) -> Optional[float]:
    if cagr is None or not mdd:
        return None
    return cagr / abs(mdd)


def drawdowns(values: List[float]) -> List[float]:
    out, peak = [], float("-inf")
    for v in values:
        peak = max(peak, v)
        out.append(v / peak - 1.0 if peak > 0 else 0.0)
    return out


def drawdown_episodes(days: List[Date], values: List[float]) -> List[dict]:
    """quantstats' drawdown_details: an episode from the first day under the peak to the first day
    back at it. `peak` is the last day at the high before the fall, `trough` the argmin, `recovery`
    the day back at the peak or None while it lasts; depth <= 0; days = (end - start) + 1."""
    dd = drawdowns(values)
    out: List[dict] = []
    i = 0
    n = len(dd)
    while i < n:
        if dd[i] >= 0:
            i += 1
            continue
        start = i
        j = i
        trough = i
        while j < n and dd[j] < 0:
            if dd[j] < dd[trough]:
                trough = j
            j += 1
        end = j if j < n else None
        out.append({"peak": days[start - 1] if start > 0 else days[start], "start": days[start], "trough": days[trough],
                    "recovery": days[end] if end is not None else None, "depth": dd[trough],
                    "days": ((days[end] if end is not None else days[-1]) - days[start]).days + 1})
        i = j
    return sorted(out, key=lambda e: e["depth"])


def max_drawdown(days: List[Date], values: List[float], basis: str = "daily") -> dict:
    """The deepest episode with its dates; basis month_end takes the month-end balances first
    (Portfolio Visualizer's convention, for the golden comparison)."""
    if basis == "month_end":
        days, values = month_ends(days, values)
    eps = drawdown_episodes(days, values)
    if not eps:
        return {"mdd": 0.0, "peak": None, "trough": None, "recovery": None}
    e = eps[0]
    return {"mdd": e["depth"], "peak": e["peak"].isoformat(), "trough": e["trough"].isoformat(),
            "recovery": e["recovery"].isoformat() if e["recovery"] else None}


def top_drawdowns(days: List[Date], values: List[float], n: int = 5, basis: str = "daily") -> List[dict]:
    if basis == "month_end":
        days, values = month_ends(days, values)
    out = []
    for e in drawdown_episodes(days, values)[:n]:
        out.append({"dd": round(e["depth"], 4), "peak": e["peak"].isoformat(), "trough": e["trough"].isoformat(),
                    "rec": e["recovery"].isoformat() if e["recovery"] else None, "days": e["days"]})
    return out


def current_drawdown(values: List[float]) -> float:
    return drawdowns(values)[-1] if values else 0.0


def calendar_year_returns(days: List[Date], values: List[float]) -> Dict[int, float]:
    """Each calendar year's return: its last value over the previous year's last value (the first
    year over its first value)."""
    ye = M.year_ends(days)
    out: Dict[int, float] = {}
    base_v = values[0]
    base_y = days[0].year
    last_i = {}
    for i, d in enumerate(days):
        last_i[d.year] = i
    for y in sorted(last_i):
        i = last_i[y]
        if days[i] in ye or y == days[-1].year:
            if base_v > 0:
                out[y] = values[i] / base_v - 1.0
            base_v = values[i]
    return out


def best_worst_year(days: List[Date], values: List[float]) -> Tuple[Optional[dict], Optional[dict]]:
    yr = calendar_year_returns(days, values)
    if not yr:
        return None, None
    best = max(yr, key=lambda y: yr[y])
    worst = min(yr, key=lambda y: yr[y])
    return {"y": best, "ret": yr[best]}, {"y": worst, "ret": yr[worst]}


def rolling(days: List[Date], values: List[float], years: int) -> Optional[dict]:
    """Trailing `years`-year returns on every day that has a value that long before it (carried
    from the last bar on or before): the latest, the lowest, the highest, the mean."""
    rets = []
    j = 0
    for i, d in enumerate(days):
        back = M.add_years(d, -years)
        if back < days[0]:
            continue
        while j + 1 < len(days) and days[j + 1] <= back:
            j += 1
        if values[j] > 0:
            rets.append(values[i] / values[j] - 1.0)
    if not rets:
        return None
    return {"last": rets[-1], "min": min(rets), "max": max(rets), "mean": sum(rets) / len(rets), "n": len(rets)}


def block(days: List[Date], values: List[float], cagr: Optional[float], rf_pct: float = 0.0,
          basis: str = "daily") -> dict:
    """Every statistic of one windowed value line, ready for the stats payload."""
    monthly = monthly_returns(days, values)
    mdd = max_drawdown(days, values, basis)
    out = {"mdd": mdd, "mddME": max_drawdown(days, values, "month_end") if basis == "daily" else max_drawdown(days, values, "daily"),
           "ddNow": current_drawdown(values), "vol": vol_ann(monthly), "sharpe": sharpe(monthly, rf_pct),
           "sortino": sortino(monthly, rf_pct), "calmar": calmar(cagr, mdd["mdd"]), "topDD": top_drawdowns(days, values, 5, basis)}
    best, worst = best_worst_year(days, values)
    out["bestY"], out["worstY"] = best, worst
    out["roll1y"], out["roll3y"] = rolling(days, values, 1), rolling(days, values, 3)
    return out

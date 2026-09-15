#!/usr/bin/env python3
"""Returns of a ledger with external flows: TWR, the annualised figure, XIRR, drawdown dates.

An independent oracle for the previews. The Home Assistant app computes the same figures its
own way and the two are cross-checked later, so the maths here takes plain lists (dates, the
value at each day's close, each day's external flow) and uses the standard library only.
market_ref.py is used for two things only: the ledger it runs, and its window rule, so a
figure here covers exactly the days the page's `chg` covers.

Flows are signed from the portfolio's side: + money in (a contribution), - money out (a
withdrawal). Dividends are not external flows: they stay inside the ledger, in its cash.

Sources, read 2026-09-15:
- TWR, Portfolio Performance's daily formula
  (https://help.portfolio-performance.info/en/concepts/performance/time-weighted/):
      1 + r = (MVE + CFout) / (MVB + CFin)
  MVB is the previous day's MVE; CFin is counted "at the very start of the day", CFout "at
  the very end of the day, just before the daily valuation"; the days are linked
  geometrically, r = (1 + r1) x ... x (1 + rn) - 1. docs/19 section 6.1 names this formula.
- XIRR, Microsoft's definition
  (https://support.microsoft.com/en-us/office/xirr-function-de1242ec-6477-445b-b11b-a303ad9adc9d):
  the rate where sum P_i / (1 + rate)^((d_i - d_1) / 365) = 0, d_1 the first date, at least
  one negative and one positive payment.
- Annualised only over a year or more: GIPS 2020 for Firms 2.A.12, "Returns for periods of
  less than one year must not be annualized", as docs/19 section 4 quotes it. The year is
  market_ref's, days / 365.25, so that with no flows ANN equals the ledger's `cagr` exactly.
  Portfolio Performance annualises over 365 days; over the preview's 26.7 years the two
  differ in the fourth significant digit of the rate, and the app's choice is to be matched
  in the cross-check.

  python3 tools/market/preview_returns.py     # the preview portfolio's figures, with and without contributions
"""
import bisect
import datetime as dt
import math
import pathlib
import sys
from typing import List, Optional, Sequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import market_ref as M  # noqa: E402

ANN_MIN_YEARS = 1            # GIPS 2.A.12: never annualise under a year
XIRR_DAY_BASIS = 365         # Microsoft's XIRR: (d_i - d_1) / 365
XIRR_BRACKET = (-0.99, 100.0)  # rates searched, per year: -99 % .. +10 000 %
XIRR_STEPS = 200             # bisection halvings: the bracket's width / 2^200 is far under a double's step
Date = dt.date


# ── time-weighted ────────────────────────────────────────────────────────────
def twr_growth(values: Sequence[float], flows: Sequence[float]) -> float:
    """The linked growth factor (1 + r_1) x ... x (1 + r_n) from values[0] to values[-1].

    values[i] is day i's closing value, with a flow of that day already in or out; flows[i]
    is day i's external flow, + in, - out. flows[0] is inside the opening value and ignored.
    Day i's return is Portfolio Performance's
        1 + r_i = (values[i] + CFout_i) / (values[i-1] + CFin_i),
        CFin_i = flows[i] if > 0, CFout_i = -flows[i] if < 0.
    On days without a flow the product telescopes, values[k] / values[j], so it is taken
    stretch by stretch between flows: the same number with fewer divisions, and with no flow
    at all it is values[-1] / values[0], the very division market_ref.report() does for chg.
    """
    g, base = 1.0, values[0]
    for i in range(1, len(values)):
        cf = flows[i]
        if cf > 0:                                   # in at the start of day i: close the stretch before it
            g *= values[i - 1] / base
            base = values[i - 1] + cf
        elif cf < 0:                                 # out at the end of day i, before the valuation
            g *= (values[i] - cf) / base
            base = values[i]
    return g * (values[-1] / base)


def annualised(growth: float, start: Date, end: Date) -> Optional[float]:
    """growth^(1 / years) - 1 over a window of at least ANN_MIN_YEARS, else None.
    The expression and the year are market_ref.report()'s cagr."""
    if growth <= 0 or M.add_years(start, ANN_MIN_YEARS) > end:
        return None
    return growth ** (1.0 / M.years_between(start, end)) - 1.0


# ── money-weighted ───────────────────────────────────────────────────────────
def xnpv(rate: float, dates: Sequence[Date], amounts: Sequence[float]) -> float:
    d1 = dates[0]
    return sum(a / (1.0 + rate) ** ((d - d1).days / XIRR_DAY_BASIS) for d, a in zip(dates, amounts))


def xirr(dates: Sequence[Date], amounts: Sequence[float]) -> float:
    """The annual rate where xnpv is zero, from the investor's side (money put in negative, the
    value taken out positive), by bisection over XIRR_BRACKET: slower than Newton, but it
    cannot jump to a second root and gives the same bits on every run. Refuses without a
    sign change across the bracket."""
    order = sorted(range(len(dates)), key=lambda i: dates[i])
    ds, xs = [dates[i] for i in order], [amounts[i] for i in order]
    if not (any(a < 0 for a in xs) and any(a > 0 for a in xs)):
        raise ValueError("XIRR needs a negative and a positive payment")
    lo, hi = XIRR_BRACKET
    f_lo = xnpv(lo, ds, xs)
    if f_lo * xnpv(hi, ds, xs) > 0:
        raise ValueError("no XIRR inside %+.0f %% .. %+.0f %% a year" % (lo * 100, hi * 100))
    for _ in range(XIRR_STEPS):
        mid = (lo + hi) / 2.0
        if mid in (lo, hi):
            break
        f_mid = xnpv(mid, ds, xs)
        if (f_mid > 0) == (f_lo > 0):
            lo, f_lo = mid, f_mid
        else:
            hi = mid
    return (lo + hi) / 2.0


# ── drawdown with its dates ──────────────────────────────────────────────────
def drawdown(dates: Sequence[Date], values: Sequence[float]) -> dict:
    """The deepest fall from a previous peak, as market_ref.max_drawdown() walks it (the running
    peak; a strictly deeper ratio replaces the one kept, so the first of equal depths stays),
    with where it happened:
      peak      the last day at the running peak before the trough: the fall starts after it
      trough    the day of the deepest ratio
      recovery  the first day after the trough whose value is back at the peak's, else None
    No fall at all gives mdd 0.0 and no dates."""
    peak, peak_i = -math.inf, -1
    mdd, pk, tr = 0.0, None, None
    for i, v in enumerate(values):
        if v >= peak:
            peak, peak_i = v, i
        if peak > 0:
            r = v / peak - 1.0
            if r < mdd:
                mdd, pk, tr = r, peak_i, i
    out = {"mdd": mdd, "peak": None, "trough": None, "recovery": None}
    if tr is None:
        return out
    out["peak"], out["trough"] = dates[pk], dates[tr]
    for j in range(tr + 1, len(values)):
        if values[j] >= values[pk]:
            out["recovery"] = dates[j]
            break
    return out


# ── the ledger's figures for one window ──────────────────────────────────────
def ledger_flows(L: "M.Ledger") -> List[float]:
    """Day i's external flow: the contributions market_ref.simulate() added to the cash that day."""
    c = L.contrib
    return [c[0]] + [c[i] - c[i - 1] for i in range(1, len(c))]


def window_flows(dates: List[Date], flows: List[float], start: Date, end: Date) -> List[float]:
    """The flows aligned with market_ref.window_series(dates, ., start, end): its first point is
    the value carried to `start`, whose flows are already inside it, so that point's flow is 0."""
    i0 = bisect.bisect_right(dates, start) - 1
    if i0 < 0:
        i0 = bisect.bisect_left(dates, start)
        if i0 >= len(dates) or dates[i0] > end:
            return []
    return [0.0] + list(flows[i0 + 1:bisect.bisect_right(dates, end)])


def ledger_xirr(cfg: "M.Config", L: "M.Ledger") -> float:
    """Since inception: the capital out on the ledger's first day, each contribution out on the
    day it landed, the value back on the last day."""
    flows = ledger_flows(L)
    dates = [L.days[0]] + [d for d, f in zip(L.days, flows) if f] + [L.days[-1]]
    amounts = [-cfg.capital] + [-f for f in flows if f] + [L.v[-1]]
    return xirr(dates, amounts)


def window_figures(cfg: "M.Config", L: "M.Ledger", preset: str) -> dict:
    """PORTFOLIO's return figures for one window: `twr` over the window (equal to the payload's
    chg when nothing flows), `ann` its annualised figure from a year up, `xirr` since inception
    when contributions are on (else None), and the window's drawdown with its dates."""
    asof = L.days[-1]
    start = M.window_start(preset, asof, cfg.inception, L.days)
    wd, wv = M.window_series(L.days, L.v, start, asof)
    flows = ledger_flows(L)
    wf = window_flows(L.days, flows, start, asof)
    assert len(wf) == len(wv), (len(wf), len(wv))
    g = twr_growth(wv, wf)
    return {"from": wd[0], "to": wd[-1], "growth": g, "twr": g - 1.0, "ann": annualised(g, wd[0], wd[-1]),
            "xirr": ledger_xirr(cfg, L) if any(flows) else None, "flows": sum(wf[1:]),
            "drawdown": drawdown(wd, wv)}


def main():
    P = M.run_preview_portfolio()
    cfg, S = P["cfg"], P["samples"]
    contrib = M.Config(cfg.capital, cfg.currency, cfg.inception, cfg.positions, 1000.0, "year")
    days = P["days"]
    for label, c in (("no contributions", cfg), ("1 000 a year", contrib)):
        L = M.simulate(c, S["series"], S["fx"], days, "hold", True, M.calendar_years)
        print(f"{label}: value {L.v[-1]:.2f}, contributed {L.contrib[-1]:.2f}")
        for preset in M.PRESETS:
            w = window_figures(c, L, preset)
            dd = w["drawdown"]
            ann = f"{w['ann'] * 100:+.2f} %" if w["ann"] is not None else "   -   "
            xirr_s = f"{w['xirr'] * 100:+.3f} %" if w["xirr"] is not None else "-"
            print(f"  {preset:4} {w['from']}..{w['to']}  TWR {w['twr'] * 100:+9.3f} %  ANN {ann}  XIRR {xirr_s}  "
                  f"MDD {dd['mdd'] * 100:.2f} % {dd['peak']}>{dd['trough']} rec {dd['recovery']}")


if __name__ == "__main__":
    main()

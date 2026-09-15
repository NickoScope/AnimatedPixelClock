"""The trading-day timeline the ledger runs on.

The design (docs/18, "Timeline"): trading days of the portfolio currency's
calendar; each series carried forward from its last bar on or before the day;
nothing interpolated; a symbol exists from its first bar.

Two builders, a registry entry each:
  union      the union of the bar dates of every traded series from the inception
             (the reference's own rule, market_ref.timeline; with daily bars of
             US-listed funds this IS the NYSE calendar, holidays included)
  calendar   the sessions of one exchange_calendars calendar, when that package
             is installed; falls back to `union` and says so when it is not

FX series are never on the timeline (their "today" stamp is a day later than the
exchanges', market_ref's note).
"""
import datetime as dt
from typing import Dict, List, Optional

from ._ref import market_ref as M
from .registry import TIMELINES

Date = dt.date
CURRENCY_CALENDAR = {"EUR": "XETR", "USD": "XNYS"}      # used by the `calendar` builder only


def is_fx(s: "M.Series") -> bool:
    return s.sym.endswith("=X") or str(s.meta.get("instrumentType", "")).upper() == "CURRENCY"


def traded(series: Dict[str, "M.Series"]) -> List["M.Series"]:
    return [s for s in series.values() if not is_fx(s) and s.dates]


@TIMELINES.register("union")
def union_timeline(series: Dict[str, "M.Series"], start: Date, **_) -> List[Date]:
    return M.timeline(traded(series), start)


@TIMELINES.register("calendar")
def calendar_timeline(series: Dict[str, "M.Series"], start: Date, calendar: Optional[str] = None,
                      currency: str = "EUR", **_) -> List[Date]:
    try:
        import exchange_calendars as xcals      # optional; the add-on's python_packages
    except ImportError:
        return union_timeline(series, start)
    name = calendar or CURRENCY_CALENDAR.get(currency, "XNYS")
    end = max((s.last for s in traded(series)), default=start)
    if end < start:
        return []
    cal = xcals.get_calendar(name, start=str(start), end=str(end))
    return [d.date() for d in cal.sessions.to_pydatetime()]


def build(series: Dict[str, "M.Series"], start: Date, kind: str = "union", **kw) -> List[Date]:
    return TIMELINES.get(kind)(series, start, **kw)

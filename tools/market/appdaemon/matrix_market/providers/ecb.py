"""The ECB tables: EUR/USD before Yahoo's EURUSD=X series (2003-12), and the euro-area HICP for real terms.

EUR/USD goes through market_ref's own table, so the reference and the app have one FX path: series
market_ref.ECB_SERIES, EXR.M.USD.EUR.SP00.E, end of period - in the ECB code list CL_EXR_SUFFIX "E" is
end-of-period and "A" the month's average, and a monthly bar's close is the month's last rate (the previews
helper checked all 72 months 1999-01..2004-12 against the last daily rate of each). The app fetches the Data
Portal's csvdata answer for market_ref.ECB_START..ECB_END into the store under the saved sample's own name
(market_ref.ECB_SAMPLE), market_ref.load_ecb_monthly refuses an answer of any other series, and FxTable takes
market_ref.ecb_monthly_table(path) as its hook. Nothing is typed in. Before 1999 there is no euro: a
conversion there refuses (NoFxRate), as the design wants.
"""
import csv
import datetime as dt
import io
import pathlib
from typing import Callable, Dict, Optional

from .._ref import market_ref as M
from .base import ProviderError

ECB_URL = "https://data-api.ecb.europa.eu/service/data/{flow}/{key}"
TABLE_META = "ecb_eur_usd_meta"          # when the CSV was fetched; the CSV itself is <store>/<market_ref.ECB_SAMPLE>


def parse_csvdata(text: str, series: str) -> Dict[str, float]:
    """The portal's csvdata -> {"1999-01": value, ...} for `series`; other series' rows are ignored (the HICP)."""
    out: Dict[str, float] = {}
    for row in csv.DictReader(io.StringIO(text)):
        if row.get("KEY") not in (None, "", series):
            continue
        period, value = row.get("TIME_PERIOD"), row.get("OBS_VALUE")
        if not period or value in (None, ""):
            continue
        try:
            out[period] = float(value)
        except ValueError:
            continue
    return out


def _months(start: str, end: str):
    y, m = (int(x) for x in start.split("-"))
    ey, em = (int(x) for x in end.split("-"))
    while (y, m) <= (ey, em):
        yield y, m
        y, m = (y + (m == 12), m % 12 + 1)


class EcbRates:
    """The saved end-of-period table and the hook FxTable takes (market_ref.ecb_monthly_table)."""

    def __init__(self, path=None, fetched: Optional[str] = None):
        self.path = pathlib.Path(path) if path else None
        self.fetched = fetched
        have = self.path is not None and self.path.exists()
        self.table = M.load_ecb_monthly(self.path) if have else {}
        self.rate: Callable[[int, int], Optional[float]] = M.ecb_monthly_table(self.path) if have else (lambda y, m: None)

    def complete(self) -> bool:
        return all(ym in self.table for ym in _months(M.ECB_START, M.ECB_END))

    @classmethod
    def from_sample(cls) -> "EcbRates":
        return cls(M.SAMPLES / M.ECB_SAMPLE, "sample")

    @classmethod
    def from_store(cls, store) -> Optional["EcbRates"]:
        path = pathlib.Path(store.root) / M.ECB_SAMPLE
        if not path.exists():
            return None
        try:
            return cls(path, (store.load_table(TABLE_META) or {}).get("fetched"))
        except (ValueError, KeyError):
            return None                      # a file of another series: refetched by ensure()

    @classmethod
    def fetch(cls, http, store) -> "EcbRates":
        flow, key = M.ECB_SERIES.split(".", 1)
        text = http.get_text(ECB_URL.format(flow=flow, key=key),
                             {"startPeriod": M.ECB_START, "endPeriod": M.ECB_END, "format": "csvdata"})
        root = pathlib.Path(store.root)
        part = root / (M.ECB_SAMPLE + ".part")
        part.write_text(text, "utf-8")
        try:
            table = M.load_ecb_monthly(part)            # refuses a row of another series
        except (ValueError, KeyError) as e:
            part.unlink(missing_ok=True)
            raise ProviderError("ECB answer refused: %s" % str(e)[:80]) from None
        if not table:
            part.unlink(missing_ok=True)
            raise ProviderError("ECB answered without %s observations" % M.ECB_SERIES)
        if not store.writable:
            part.unlink(missing_ok=True)
            return cls(None)
        part.replace(root / M.ECB_SAMPLE)
        fetched = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        store.save_table(TABLE_META, {"v": 1, "series": M.ECB_SERIES, "fetched": fetched})
        return cls(root / M.ECB_SAMPLE, fetched)


def ensure(store, http=None) -> Optional[EcbRates]:
    """The table from the store, fetched once when missing or incomplete (and `http` is given)."""
    rates = EcbRates.from_store(store)
    if rates is not None and rates.complete():
        return rates
    if http is None:
        return rates
    return EcbRates.fetch(http, store)


# ── the euro-area HICP, for real terms (docs/19 §4 "Real vs nominal", §6.9) ──────────────
HICP_URL = "https://data-api.ecb.europa.eu/service/data/ICP/M.U2.N.000000.4.INX"
HICP_SERIES = "ICP.M.U2.N.000000.4.INX"     # "Euro area (changing composition) - HICP - Overall index", 2015 = 100,
HICP_TABLE = "ecb_hicp_ea_monthly"           # monthly, from 1999-01: probed 2026-09-15 from the Mac (HTTP 200)
HICP_MAX_AGE_DAYS = 35                       # a new print every month


class HicpIndex:
    """The monthly index, carried forward from the latest print for the days after it."""

    def __init__(self, table: Optional[Dict[str, float]] = None, fetched: Optional[str] = None):
        self.table = dict(table or {})
        self.fetched = fetched
        self._keys = sorted(self.table)

    def level(self, d: dt.date) -> Optional[float]:
        key = "%04d-%02d" % (d.year, d.month)
        if key in self.table:
            return self.table[key]
        earlier = [k for k in self._keys if k < key]
        return self.table[earlier[-1]] if earlier else None

    def factor(self, base: dt.date, d: dt.date) -> Optional[float]:
        """Nominal on `d` x factor = real in `base`'s money: level(base) / level(d)."""
        a, b = self.level(base), self.level(d)
        return a / b if a and b else None

    @classmethod
    def fetch(cls, http, start: str = "1999-01") -> "HicpIndex":
        text = http.get_text(HICP_URL, {"startPeriod": start, "format": "csvdata"})
        table = parse_csvdata(text, HICP_SERIES)
        if not table:
            from .base import ProviderError
            raise ProviderError("ECB answered without %s observations" % HICP_SERIES)
        return cls(table, dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))

    @classmethod
    def from_store(cls, store) -> Optional["HicpIndex"]:
        d = store.load_table(HICP_TABLE)
        if not d or d.get("series") != HICP_SERIES or not isinstance(d.get("rates"), dict):
            return None
        return cls({k: float(v) for k, v in d["rates"].items()}, d.get("fetched"))

    def save(self, store) -> None:
        store.save_table(HICP_TABLE, {"v": 1, "series": HICP_SERIES, "source": HICP_URL, "fetched": self.fetched, "rates": self.table})

    def age_days(self, now: Optional[dt.datetime] = None) -> Optional[float]:
        if not self.fetched:
            return None
        try:
            then = dt.datetime.strptime(self.fetched, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
        except ValueError:
            return None
        return ((now or dt.datetime.now(dt.timezone.utc)) - then).total_seconds() / 86400.0


def ensure_hicp(store, http=None) -> Optional[HicpIndex]:
    """The HICP table from the store, fetched when missing or older than a month (and `http` is given)."""
    idx = HicpIndex.from_store(store)
    age = idx.age_days() if idx else None
    if idx is not None and age is not None and age < HICP_MAX_AGE_DAYS:
        return idx
    if http is None:
        return idx
    fresh = HicpIndex.fetch(http)
    fresh.save(store)
    return fresh

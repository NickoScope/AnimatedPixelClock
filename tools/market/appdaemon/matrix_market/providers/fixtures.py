"""The saved Yahoo answers in tools/market/samples/ as a Provider: tests and `cli --offline`.

History: market_ref.sample_file's rule (the preview files, then yahoo_<SYM>_1mo.json).
Fund info: the saved quoteSummary answers. Quotes: the last two bars of the sample
(a DAY change only for the one daily sample, market_ref.quote's note). Intraday:
none saved (Yahoo answered 429 before a 5-minute sample was taken) -> NotFound.
"""
import datetime as dt
import json
import pathlib
from typing import Dict, Optional, Sequence

from .._ref import market_ref as M
from ..registry import PROVIDERS
from .base import FundInfo, Intraday, NotFound, Provider, Quote

Date = dt.date


@PROVIDERS.register("fixtures")
class FixtureProvider(Provider):
    name = "fixtures"

    def __init__(self, samples: Optional[pathlib.Path] = None, **_):
        self.samples = pathlib.Path(samples or M.SAMPLES)
        self._ter: Dict[str, Optional[float]] = {}
        for f in M.PREVIEW_TER_FILES:
            p = self.samples / f
            if p.exists():
                sym, ter = M.load_yahoo_ter(p)
                self._ter[sym] = ter

    def _file(self, symbol: str) -> pathlib.Path:
        for name in (M.PREVIEW_FILES.get(symbol), "yahoo_%s_1mo.json" % symbol.replace(".", ""),
                     "yahoo_%s.json" % symbol.replace(".", "").replace("^", "")):
            if name and (self.samples / name).exists():
                p = self.samples / name
                try:
                    d = json.loads(p.read_text("utf-8"))
                except (ValueError, OSError):
                    continue
                if "chart" in d:
                    return p
        for sym, mn, f in M.PREVIEW_INDICES:
            if sym == symbol and (self.samples / f).exists():
                return self.samples / f
        raise NotFound("%s: no sample" % symbol)

    def raw_chart(self, symbol: str, **params) -> dict:
        return json.loads(self._file(symbol).read_text("utf-8"))

    def history(self, symbol: str, start: Date, end: Optional[Date] = None, interval: str = "1d") -> "M.Series":
        s = M.load_yahoo_chart(self._file(symbol))
        if symbol in self._ter:
            s.ter = self._ter[symbol]
        return s

    def fund_info(self, symbol: str) -> FundInfo:
        if symbol not in self._ter:
            raise NotFound("%s: no quoteSummary sample" % symbol)
        return FundInfo(symbol, self._ter[symbol], "fixtures")

    def quotes(self, symbols: Sequence[str]) -> Dict[str, Quote]:
        out = {}
        for sym in symbols:
            try:
                s = M.load_yahoo_chart(self._file(sym))
            except NotFound:
                continue
            q = M.quote(s)
            ts = int(s.meta.get("regularMarketTime") or 0)
            out[sym] = Quote(sym, q["last"], q["prev"], ts, s.currency, None, None, s.meta)
        return out

    def intraday(self, symbol: str) -> Intraday:
        raise NotFound("%s: no intraday sample" % symbol)

    def session_meta(self, symbol: str) -> dict:
        return M.load_yahoo_chart(self._file(symbol)).meta

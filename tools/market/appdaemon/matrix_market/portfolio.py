"""Feature `portfolio`: both ledger modes on the stored history; portfolio/<mode>/<preset>, holdings/<mode>, stats/<mode>/<preset>.

Order 20: it takes `series`, `fx`, `asof` and the ECB tables from the context (the
history feature's), attaches the TERs (the portal's overrides first, then the
store's), splices proxies, applies the settings' Rules and benchmarks, runs
ledger.compute and leaves `result` in the context for the HA sensors.
A position without any stored history refuses the whole portfolio and says so
in status (`pfErr`): the panel shows DATA ERR rather than a wrong allocation.
Real terms need the HICP table; when it is missing the option is reported
unavailable (`realErr`) and the nominal figures go out.
"""
from typing import Dict, Optional

from . import ledger
from .config import TR_BENCH
from .features import Feature
from .providers import ecb as ecb_mod
from .registry import FEATURES


@FEATURES.register("portfolio")
class PortfolioFeature(Feature):
    name = "portfolio"
    order = 20

    def __init__(self, ctx):
        super().__init__(ctx)
        self.error = ""
        self.real_error = ""
        self.result: Optional[ledger.Result] = None

    def prepared_series(self) -> Dict[str, "ledger.M.Series"]:
        """The stored series with TERs attached and proxies spliced (copies for the spliced ones)."""
        s = self.settings
        series = dict(self.ctx.data.get("series") or {})
        entries = {sym: self.ctx.store.entry(sym) for sym in series}
        ledger.attach_ter(series, s.ter_overrides(), {k: e.get("ter") for k, e in entries.items()},
                          {k: e.get("terSrc") or "" for k, e in entries.items()})
        for sym, proxy in s.proxies().items():
            if sym in series and proxy in series:
                series[sym] = ledger.splice_proxy(series[sym], series[proxy])
        return series

    def compute(self) -> Optional[ledger.Result]:
        s = self.settings
        fx = self.ctx.data.get("fx")
        if not self.ctx.data.get("series") or fx is None:
            self.error = "no history"
            return None
        series = self.prepared_series()
        missing = [p["sym"] for p in s.positions() if p["sym"] not in series]
        if missing:
            self.error = "no history for " + ", ".join(missing[:4]) + (", .." if len(missing) > 4 else "")
            return None
        if not s.positions():
            self.error = "no positions"
            return None
        real = None
        self.real_error = ""
        if s["returns.real"] or s["contrib.index_inflation"]:
            hicp = self.ctx.data.get("hicp") or ecb_mod.HicpIndex.from_store(self.ctx.store)
            if hicp is None:
                self.real_error = "HICP unavailable"
                self.log("real terms asked for, the HICP table is missing: nominal figures published", level="WARNING")
            else:
                inception = ledger.Date.fromisoformat(s["portfolio.inception"])
                if s["returns.real"]:
                    real = lambda d, h=hicp, b=inception: h.factor(b, d)
        rules = s.rules()
        if s["contrib.index_inflation"] and not self.real_error:
            hicp = self.ctx.data.get("hicp") or ecb_mod.HicpIndex.from_store(self.ctx.store)
            inception = ledger.Date.fromisoformat(s["portfolio.inception"])
            rules.contrib_index = lambda d, h=hicp, b=inception: 1.0 / (h.factor(b, d) or 1.0)
        bench = s.benchmark()
        if bench and bench not in series:
            idx = [i["sym"] for i in s.indices() if i["sym"] in series]
            self.log("benchmark %s has no history yet; %s stands in" % (bench, idx[0] if idx else "nothing"), level="WARNING")
            bench = idx[0] if idx else None
        try:
            result = ledger.compute(s.ref_config(), series, fx, bench, rules=rules, blend=s.blend_legs() or None,
                                    extra_bench=[x["sym"] for x in (s.get("bench.extra") or [])] + [TR_BENCH],
                                    real=real, classes=s.asset_classes())
        except (KeyError, ValueError, ledger.M.NoFxRate) as e:
            self.error = "%s: %s" % (type(e).__name__, str(e)[:60])
            self.log(self.error, level="WARNING")
            return None
        self.error = ""
        self.bench_err = ", ".join(getattr(result, "bench_err", [])[:3])
        self.result = result
        self.ctx.data["result"] = result
        return result

    def contribute(self, snapshot, fetch: bool = True) -> None:
        result = self.compute()
        if result is None:
            return
        s = self.settings
        lines_on = {"bench": bool(s["lines.bench"]), "gross": bool(s["lines.gross"]), "tell": bool(s["bench.telltale"])}
        self.xirr_fail = 0
        for mode in result.modes:
            for preset in s.presets():
                pp = ledger.portfolio_payload(result, mode, preset, lines_on, s["returns.measure"], s["display.scale"], s["mdd_basis"])
                if pp.get("flows") and pp.get("xirr_ann") is None:
                    self.xirr_fail += 1              # the solver found no root: the panel shows XIRR --
                snapshot.add("portfolio/%s/%s" % (mode, preset), pp)
                snapshot.add("stats/%s/%s" % (mode, preset),
                             ledger.stats_payload(result, mode, preset, float(s["stats.risk_free_pct"]), s["mdd_basis"]))
            snapshot.add("holdings/%s" % mode, ledger.holdings_payload(result, mode))
        if self.xirr_fail:
            self.log("XIRR found no rate in %d portfolio windows: xirr_ann is null there" % self.xirr_fail, level="WARNING")
        self.ctx.emit("portfolio", result=result)

    def status(self) -> dict:
        out = {}
        if self.error:
            out["pfErr"] = self.error
        if self.real_error:
            out["realErr"] = self.real_error
        if getattr(self, "bench_err", ""):
            out["benchErr"] = self.bench_err
        if getattr(self, "xirr_fail", 0):
            out["xirrFail"] = self.xirr_fail
        return out

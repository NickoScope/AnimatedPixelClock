"""The laptop's door to the same code: fetch, compute, publish --dry-run, report, schema.

    cd tools/market/appdaemon
    python3 -m matrix_market.cli fetch                       # the watched symbols, paced 10 s, into --store
    python3 -m matrix_market.cli fetch --samples ../samples  # ..and the monthly samples the previews lack
    python3 -m matrix_market.cli report                      # the golden figures: HOLD and REBAL, MAX and 5Y
    python3 -m matrix_market.cli compute                     # every payload and its size, nothing published
    python3 -m matrix_market.cli publish --dry-run           # the payloads themselves
    python3 -m matrix_market.cli schema                      # the settings table (config.SCHEMA)

--store defaults to tools/market/store (gitignored). --config FILE.json lays a
`config` payload over the defaults, as the panel would. --offline uses the saved
samples (providers/fixtures.py) instead of Yahoo. A 429 stops a fetch and is
reported; the symbols already saved stay.
"""
import argparse
import datetime as dt
import json
import pathlib
import sys
import time
from typing import List, Optional

from . import SCHEMA_V, __version__, config
from . import ha_entities, history, live, portfolio, tape  # noqa: F401  (registrations)
from ._ref import market_ref as M
from .features import Context
from .providers.base import NotFound, ProviderError, RateLimited
from .providers.httpio import DEFAULT_UA
from .publisher import DryRunClient, Publisher, Topics
from .registry import FEATURES, PROVIDERS
from .store import Store

HERE = pathlib.Path(__file__).resolve().parent
MARKET_DIR = HERE.parents[1]                     # tools/market
DEFAULT_STORE = MARKET_DIR / "store"
OPENED: List[Store] = []


def log(msg, level="INFO"):
    print("[%s] %s" % (level, msg), file=sys.stderr)


def local_override(args):
    """The local override, as on Home Assistant: --local PATH, else <store>/local.json when it exists; --no-local
    for none (the tests and check_market.py always pass it, so their results never depend on the owner's file)."""
    if getattr(args, "no_local", False):
        return None
    path = pathlib.Path(args.local) if getattr(args, "local", None) else pathlib.Path(args.store) / config.LOCAL_FILE
    if getattr(args, "local", None) and not path.exists():
        sys.exit("--local %s: no such file" % path)
    return config.load_local(path)


def settings_from(args) -> config.Settings:
    local = local_override(args)
    s = config.from_args({}, local) if local else config.from_defaults()
    if local:
        log("local override: %d positions" % len(s.positions()))
    if args.config:
        payload = json.loads(pathlib.Path(args.config).read_text("utf-8"))
        s = config.from_panel(s, payload)
        for e in s.errors:
            log("config %s: %s (%s)" % (e["key"], e["code"], e["msg"]), "WARNING")
    return s


def build(args, features: List[str]):
    settings = settings_from(args)
    store = Store(args.store)
    OPENED.append(store)
    if args.offline:
        provider = PROVIDERS.get("fixtures")()
    else:
        provider = PROVIDERS.get("yahoo")(gap_s=args.gap, user_agent=args.user_agent, log=log)
    client = DryRunClient()
    publisher = Publisher(client, Topics("nickoscope_matrix", args.device), log)
    ctx = Context(settings, store, provider, publisher, log)
    ctx.data["device"] = args.device
    ctx.data["user_agent"] = args.user_agent
    feats = sorted([FEATURES.get(n)(ctx, args.device) if n == "ha_entities" else FEATURES.get(n)(ctx) for n in features],
                   key=lambda f: f.order)
    if args.offline:
        for f in feats:
            if f.name == "history":
                f.ecb_http = None            # offline: the ECB table only from the store
    return settings, store, provider, publisher, client, ctx, feats


def cmd_fetch(args) -> int:
    settings, store, provider, publisher, client, ctx, feats = build(args, ["history"])
    hist = feats[0]
    t0 = time.time()

    def progress(sym, how, detail):
        print("%-9s %-9s %s" % (sym, how, detail), flush=True)
    hist.progress = progress
    symbols = args.symbols or None
    if symbols:
        result = hist.fetch_symbols(symbols)
        if "429" not in result.values() and not args.no_ter:
            hist.fetch_ter([s for s in symbols if s in {p["sym"] for p in settings.positions()}], force=args.force_ter)
        hist.fetch_ecb()
    else:
        result = hist.fetch_all()
        if args.force_ter and "429" not in result.values():
            hist.fetch_ter([p["sym"] for p in settings.positions()], force=True)
    hit_429 = "429" in result.values()
    ecb = ctx.data.get("ecb")
    print("ECB 1999-2003 table: %s" % ("%d months, fetched %s" % (len(ecb.table), ecb.fetched) if ecb else "missing"))
    if args.samples and not hit_429:
        hit_429 = fetch_samples(provider, settings, pathlib.Path(args.samples), progress) or hit_429
    if args.samples and not hit_429 and not args.offline:
        hit_429 = fetch_live_samples(provider, settings, pathlib.Path(args.samples), progress) or hit_429
    n_ok = sum(1 for v in result.values() if v in ("new", "appended", "same", "replaced"))
    print("fetched %d/%d symbols in %.0f s, %d requests%s" % (n_ok, len([k for k in result if not k.startswith("TER:")]),
          time.time() - t0, getattr(provider, "requests", 0), "; STOPPED ON 429" if hit_429 else ""))
    return 2 if hit_429 else 0


def fetch_samples(provider, settings, samples: pathlib.Path, progress) -> bool:
    """The monthly samples the previews lack (fetch_samples.py's naming): yahoo_<SYM>_1mo.json."""
    period1 = int(dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc).timestamp())
    for sym in [p["sym"] for p in settings.positions()] + [i["sym"] for i in settings.indices()]:
        out = samples / ("yahoo_%s_1mo.json" % sym.replace(".", ""))
        if out.exists() or M.sample_file(sym):
            continue
        try:
            d = provider.raw_chart(sym, period1=period1, interval="1mo")
        except RateLimited as e:
            progress(sym, "429", "sample: %s" % e)
            return True
        except ProviderError as e:
            progress(sym, "error", "sample: %s" % type(e).__name__)
            continue
        r = d["chart"]["result"][0]
        if not r.get("timestamp"):
            progress(sym, "skip", "sample without bars")
            continue
        out.write_text(json.dumps(d, separators=(",", ":")), "utf-8")
        progress(sym, "sample", "%d monthly bars -> %s" % (len(r["timestamp"]), out.name))
    return False


def fetch_live_samples(provider, settings, samples: pathlib.Path, progress) -> bool:
    """One spark answer for the live symbols and one 1-minute chart, as fixtures for the live loop."""
    spark = samples / "yahoo_spark_1d_5m.json"
    if not spark.exists():
        try:
            provider.pacer.wait()
            provider.requests += 1
            d = provider.http.get_json("https://query1.finance.yahoo.com/v7/finance/spark",
                                       {"symbols": ",".join(settings.live_symbols()), "range": "1d", "interval": "5m"})
        except RateLimited as e:
            progress("spark", "429", str(e))
            return True
        except ProviderError as e:
            progress("spark", "error", type(e).__name__)
            d = None
        if d is not None:
            spark.write_text(json.dumps(d, separators=(",", ":")), "utf-8")
            progress("spark", "sample", "%d B -> %s" % (spark.stat().st_size, spark.name))
    intraday = samples / "yahoo_GSPC_1d_1m.json"
    if not intraday.exists():
        try:
            d = provider.raw_chart("^GSPC", interval="1m", range="1d", events="")
        except RateLimited as e:
            progress("^GSPC 1m", "429", str(e))
            return True
        except ProviderError as e:
            progress("^GSPC 1m", "error", type(e).__name__)
            return False
        intraday.write_text(json.dumps(d, separators=(",", ":")), "utf-8")
        progress("^GSPC 1m", "sample", "%d bars -> %s" % (len(d["chart"]["result"][0].get("timestamp") or []), intraday.name))
    return False


def run_generation(args, feature_names=("history", "portfolio", "tape", "ha_entities")):
    settings, store, provider, publisher, client, ctx, feats = build(args, list(feature_names))
    snap = publisher.new_snapshot(None)
    for f in feats:
        if f.name == "tape":
            continue
        f.contribute(snap, fetch=False)
    asof = ctx.data.get("asof")
    snap.asof = asof.isoformat() if asof else None
    status = {"v": SCHEMA_V, "asof": snap.asof, "state": "ok" if asof else "error", "err": "" if asof else "no data",
              "symbols": store.status_symbols(settings.status_symbols()), "ver": __version__}
    for f in feats:
        status.update(f.status())
    if settings.errors:
        status["cfgErr"] = [{"key": e["key"], "code": e["code"]} for e in settings.errors[:8]]
    sizes = publisher.publish_snapshot(snap, status)
    for f in feats:
        if f.name == "tape":
            f.tick(force=True)
        if f.name == "ha_entities":
            f.publish_all()
    return settings, ctx, feats, client, sizes, snap


def cmd_compute(args) -> int:
    settings, ctx, feats, client, sizes, snap = run_generation(args)
    worst = max(sizes.values()) if sizes else 0
    print("as of %s; %d payloads; largest %d B (limit 1900)" % (snap.asof, len(sizes), worst))
    for leaf in sorted(sizes, key=lambda k: -sizes[k])[:12]:
        print("  %5d B  %s" % (sizes[leaf], leaf))
    others = [(t, len(p)) for t, p, q, r in client.published if not t.split("/market/")[-1] in sizes]
    for t, n in others:
        print("  %5d B  %s" % (n, t))
    pf = next((f for f in feats if f.name == "portfolio"), None)
    if pf and pf.error:
        print("portfolio: %s" % pf.error)
    return 0 if worst <= 1900 and (not pf or not pf.error) else 1


def cmd_publish(args) -> int:
    settings, ctx, feats, client, sizes, snap = run_generation(args)
    for topic, payload, qos, retain in client.published:
        text = payload.decode("utf-8", errors="replace")
        if not args.full and len(text) > 300:
            try:
                d = json.loads(text)
                for k in ("pts", "px", "bench", "gross", "tell"):
                    if isinstance(d.get(k), str) and len(d[k]) > 40:
                        d[k] = d[k][:12] + "..(%d B)" % len(d[k])
                text = json.dumps(d, separators=(",", ":"))
            except ValueError:
                pass
        print("%s  [%d B%s]\n  %s" % (topic, len(payload), ", retained" if retain else "", text))
    print("\n%d messages, largest payload %d B" % (len(client.published), max((len(p) for _, p, _, _ in client.published), default=0)))
    return 0


def _pct(x) -> str:
    return "%+.2f %%" % (x * 100) if x is not None else "   -   "


def cmd_report(args) -> int:
    from . import ledger
    settings, store, provider, publisher, client, ctx, feats = build(args, ["history", "portfolio"])
    hist, pf = feats
    hist.load()
    result = pf.compute()
    if result is None:
        print("no portfolio: %s" % pf.error)
        return 1
    presets = [p.strip() for p in args.presets.split(",")]
    cfg = result.cfg
    print("matrix_market %s report, %s" % (__version__, dt.datetime.now().strftime("%Y-%m-%d %H:%M")))
    print("store %s; provider data %s; allocation: %s" % (args.store, "fixtures" if args.offline else "yahoo (daily bars)",
          "the local override (not for committing)" if local_override(args) else "the committed neutral example / --config"))
    print("capital %.2f %s from %s; contributions %s; %d positions" % (cfg.capital, cfg.currency, cfg.inception,
          "%.2f/%s" % (cfg.contrib_amount, cfg.contrib_every) if cfg.contrib_amount else "none", len(cfg.positions)))
    print("timeline %s .. %s, %d trading days; benchmark %s" % (result.days[0], result.days[-1], len(result.days), result.bench_sym))
    ecb = ctx.data.get("ecb")
    print("FX: EURUSD=X %s; ECB 1999-2003 table %s" % (
        "%s..%s" % (result.fx.dates[0], result.fx.dates[-1]) if result.fx.dates else "missing",
        "%d months" % len(ecb.table) if ecb else "missing"))
    print("\nTER per position (fraction/yr, source):")
    for p in cfg.positions:
        s = result.series[p.sym]
        e = store.entry(p.sym)
        print("  %-6s w %6.2f %%  bars %5s  %s..%s  ter %s (%s)" % (p.sym, p.w, e.get("bars"), e.get("first"), e.get("last"),
              "%.4f %%" % (s.ter * 100) if s.ter is not None else "none", e.get("terSrc") or "-"))
    rules = result.rules
    print("rules: dividends %s; REBAL = %s (%s; bands %g/%g checked %s); contrib_only %s; withdrawals %g/%g %%; cash yield %g %%; "
          "tax %g %%; costs %g + %g %%" % (rules.dividends, rules.rebal, rules.calendar, rules.band_abs_pts, rules.band_rel_pct,
                                          rules.band_check, rules.contrib_only, rules.withdraw_amount, rules.withdraw_pct,
                                          rules.cash_yield_pct, rules.tax_div_pct, rules.cost_fixed, rules.cost_pct))
    for mode in result.modes:
        L = result.ledger(mode)
        print("\n%s  entries: %s; rebalances %d; trades %d" % (mode.upper(), ", ".join("%s %s" % (k, v) for k, v in sorted(L.entries.items(), key=lambda kv: kv[1])),
                                                            len(L.rebal_dates), L.trades))
        for preset in presets:
            p = ledger.portfolio_payload(result, mode, preset)
            sp = ledger.stats_payload(result, mode, preset, float(settings["stats.risk_free_pct"]), settings["mdd_basis"])
            print("  %-4s %s..%s  value %12.2f  twr %s  ann %s  sinceStart %s  cagr %s  xirr_ann %s%s  fx_effect %s"
                  % (preset, p["from"], p["to"], p["value"], _pct(p["twr"]), _pct(p.get("ann")), _pct(p["sinceStart"]), _pct(p["cagr"]),
                     _pct(p.get("xirr_ann")), " (flows)" if p.get("flows") else "", _pct(p["fx_effect"])))
            md, me = sp["mdd"], sp["mddME"]
            print("       div %10.2f  ter~ %8.2f  cash %5.2f %%  MDD daily %6.2f %% (%s > %s > %s)  MDD month-end %6.2f %% (%s > %s > %s)"
                  % (p["div"], p["terDrag"], p["cash"] * 100, md["mdd"] * 100, md["peak"], md["trough"], md["recovery"] or "open",
                     me["mdd"] * 100, me["peak"], me["trough"], me["recovery"] or "open"))
            print("       vol %s  sharpe %s  sortino %s  calmar %s  best %s  worst %s  bench %s"
                  % (_pct(sp["vol"]), "%.2f" % sp["sharpe"] if sp["sharpe"] is not None else "-",
                     "%.2f" % sp["sortino"] if sp["sortino"] is not None else "-", "%.2f" % sp["calmar"] if sp["calmar"] is not None else "-",
                     "%d %s" % (sp["bestY"]["y"], _pct(sp["bestY"]["ret"])) if sp["bestY"] else "-",
                     "%d %s" % (sp["worstY"]["y"], _pct(sp["worstY"]["ret"])) if sp["worstY"] else "-",
                     ", ".join("%s %s" % (b["sym"], _pct(b["chg"])) for b in sp["bench"]) or "-"))
        h = ledger.holdings_payload(result, mode)
        sp = ledger.stats_payload(result, mode, presets[0], float(settings["stats.risk_free_pct"]), settings["mdd_basis"])
        contrib = dict(sp.get("contrib") or [])
        for r in h["rows"]:
            print("    %-6s %-11s tgt %6.2f now %6.2f  band %s..%s%s  ret %s  contrib(%s) %s  entry %s"
                  % (r["sym"], r.get("cls", "-"), r["tgt"], r["now"], r["lo"], r["hi"], " !" if r.get("out") else "  ", _pct(r["ret"]),
                     presets[0], _pct(contrib.get(r["sym"])), r["entry"]))
        print("    CASH   tgt %6.2f now %6.2f  classes %s" % (h["cash"]["tgt"], h["cash"]["now"], h.get("classes")))
    return 0


def lovelace_view(settings, currency: str = "EUR") -> dict:
    """The Lovelace sections view "Портфель" for the dashboard `stock-market`, from the effective settings:
    the discovered sensors (ha_entities.py) for the figures, a history graph of the value, the holdings as
    entities cards, the exchanges, the benchmarks. `python3 -m matrix_market.cli view > portfolio_view.json`;
    the committed tools/market/ha/portfolio_view.json is this on the neutral defaults."""
    from .ha_entities import object_id
    pos = settings.positions()
    cur = settings["portfolio.currency"]
    holdings_share = [{"entity": "sensor.matrix_market_holding_%s_share" % object_id(p["sym"]), "name": "%s (цель %g %%)" % (p["sym"], p["w"])} for p in pos]
    holdings_ret = [{"entity": "sensor.matrix_market_holding_%s_return" % object_id(p["sym"]), "name": p["sym"]} for p in pos]
    exchanges = [{"entity": "sensor.matrix_market_exchange_%s" % object_id(n), "name": n} for n in settings.get("tape.exchanges") or []]
    classes = sorted({p.get("asset_class") for p in pos if p.get("asset_class")} | {"cash"})
    return {
        "_comment": ["Generated by `python3 -m matrix_market.cli view` from the app's settings (the local override when present).",
                     "Add as a new view of the dashboard `stock-market` (Edit dashboard > Raw configuration editor > append under `views`).",
                     "Entities: the MQTT-discovered sensors of matrix_market (ha_entities.py). custom:easy-stock-card is the only custom card (installed).",
                     "Numbers are the hypothetical backtest of docs/18: no taxes, no commissions, TER as an estimate."],
        "title": "Портфель", "path": "portfolio", "icon": "mdi:briefcase-variant", "type": "sections", "max_columns": 3,
        "sections": [
            {"type": "grid", "cards": [
                {"type": "heading", "heading": "Портфель (backtest)", "heading_style": "title",
                 "badges": [{"type": "entity", "entity": "sensor.matrix_market_portfolio_mode", "show_state": True, "show_icon": False},
                            {"type": "entity", "entity": "sensor.matrix_market_portfolio_asof", "show_state": True, "show_icon": False}]},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_value", "name": "Стоимость, %s" % cur, "icon": "mdi:chart-line",
                 "color": "amber", "grid_options": {"columns": 12, "rows": 2}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_return", "name": "Доходность за окно (TWR)", "color": "green", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_twr_ann", "name": "За окно, годовых", "color": "green", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_since_start", "name": "С начала", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_cagr", "name": "CAGR", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_mdd", "name": "Макс. просадка", "color": "red", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_mdd_dates", "name": "Просадка: пик > дно > выход", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_div", "name": "Дивиденды (DIV)", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_ter_drag", "name": "TER drag ~", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_fx", "name": "Валютный эффект", "grid_options": {"columns": 6}},
                {"type": "tile", "entity": "sensor.matrix_market_portfolio_xirr", "name": "XIRR (годовых, при взносах)", "grid_options": {"columns": 6}},
                {"type": "gauge", "entity": "sensor.matrix_market_portfolio_cash", "name": "Кэш, %", "min": 0, "max": 20, "needle": True,
                 "segments": [{"from": 0, "color": "var(--success-color)"}, {"from": 5, "color": "var(--warning-color)"}, {"from": 10, "color": "var(--error-color)"}]}]},
            {"type": "grid", "cards": [
                {"type": "heading", "heading": "Стоимость во времени"},
                {"type": "history-graph", "hours_to_show": 720, "title": "Стоимость (с момента установки приложения)",
                 "entities": [{"entity": "sensor.matrix_market_portfolio_value", "name": "Портфель"}], "grid_options": {"columns": 12, "rows": 6}},
                {"type": "entities", "title": "Риск (по месячным доходностям, %s)" % settings["display.preset"],
                 "entities": [{"entity": "sensor.matrix_market_portfolio_vol", "name": "Волатильность, годовых"},
                              {"entity": "sensor.matrix_market_portfolio_sharpe", "name": "Sharpe"},
                              {"entity": "sensor.matrix_market_portfolio_sortino", "name": "Sortino"},
                              {"entity": "sensor.matrix_market_portfolio_calmar", "name": "Calmar"},
                              {"entity": "sensor.matrix_market_portfolio_mdd_month_end", "name": "Просадка по концам месяцев"},
                              {"entity": "sensor.matrix_market_portfolio_best_year", "name": "Лучший год"},
                              {"entity": "sensor.matrix_market_portfolio_worst_year", "name": "Худший год"}]},
                {"type": "entities", "title": "Бенчмарки за окно",
                 "entities": [{"entity": "sensor.matrix_market_benchmark_%d" % i, "name": "Бенчмарк %d" % i} for i in (1, 2, 3)]},
                {"type": "markdown", "title": "Что это",
                 "content": "Гипотетический бэктест: {{ states('sensor.matrix_market_portfolio_mode') }} (HOLD - 31 декабря кэш докупает позиции; REBAL - возврат к целевым весам), "
                            "%s %s с %s, дивиденды в кэш, TER как оценка (`TER drag ~`), без налогов и комиссий. Данные Yahoo Finance на "
                            "{{ states('sensor.matrix_market_portfolio_asof') }}; источник: **{{ states('sensor.matrix_market_status') }}** "
                            "({{ states('sensor.matrix_market_status_asof') }}). Настройки - в портале панели."
                            % ("{:,.0f}".format(settings["portfolio.capital"]).replace(",", " "), cur, settings["portfolio.inception"])}]},
            {"type": "grid", "cards": [
                {"type": "heading", "heading": "Позиции: цель → сейчас, доходность с входа"},
                {"type": "entities", "title": "Доля сейчас, %", "entities": holdings_share + [{"type": "divider"}, {"entity": "sensor.matrix_market_cash_share", "name": "CASH"}]},
                {"type": "entities", "title": "Доходность с входа, %", "entities": holdings_ret},
                {"type": "entities", "title": "По классам активов, %",
                 "entities": [{"entity": "sensor.matrix_market_class_%s" % c, "name": c.replace("_", " ")} for c in classes]}]},
            {"type": "grid", "cards": [
                {"type": "heading", "heading": "Биржи"},
                {"type": "entities", "entities": exchanges},
] + easy_stock_card(settings)},
        ],
    }


def easy_stock_card(settings) -> list:
    """The Easy Stock card for the symbol the panel shows (ticker, else the first ticker, position or index), or none.
    Easy Stock names its sensor after the config entry's title; for a fund added by its ticker that is
    sensor.<ticker in lower case> (docs/18: sensor.voo), for an index the title (sensor.s_p_500), so the id
    is only right for tickers entered as such."""
    from .ha_entities import object_id
    sym = (settings.get("ticker") or next((t["sym"] for t in settings.tickers()), None)
           or next((p["sym"] for p in settings.positions()), None) or next((i["sym"] for i in settings.indices()), None))
    if not sym:
        return []
    return [{"type": "custom:easy-stock-card", "entity": "sensor.%s" % object_id(sym), "name": "%s (Easy Stock, дневной график)" % sym, "range": "1d"}]


def cmd_view(args) -> int:
    settings = settings_from(args)
    print(json.dumps(lovelace_view(settings), ensure_ascii=False, indent=2))
    return 0


def cmd_schema(args) -> int:
    print(config.schema_table())
    print()
    print(config.schema_table(config.APP_SCHEMA))
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m matrix_market.cli", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--store", default=str(DEFAULT_STORE), help="the store directory (default tools/market/store)")
    ap.add_argument("--config", help="a config payload (JSON) laid over the defaults, as the panel's would be")
    ap.add_argument("--offline", action="store_true", help="the saved samples instead of Yahoo")
    ap.add_argument("--gap", type=float, default=10.0, help="seconds between Yahoo requests (default 10)")
    ap.add_argument("--user-agent", default=DEFAULT_UA, help="the User-Agent of every request (default: the one Yahoo answered on 2026-09-15)")
    ap.add_argument("--device", default="a1b2c3", help="the panel id in the topics (a placeholder by default)")
    ap.add_argument("--local", help="the local override JSON (default: <store>/local.json when it exists)")
    ap.add_argument("--no-local", action="store_true", help="ignore the local override: the neutral committed defaults")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="daily history (and TER, ECB) into the store; stops on 429")
    f.add_argument("symbols", nargs="*", help="only these symbols (default: every watched one)")
    f.add_argument("--samples", help="also save the monthly samples the previews lack into this directory")
    f.add_argument("--no-ter", action="store_true")
    f.add_argument("--force-ter", action="store_true", help="read the TER even if it is fresh")
    f.set_defaults(fn=cmd_fetch)
    c = sub.add_parser("compute", help="every payload from the store, sizes only")
    c.set_defaults(fn=cmd_compute)
    p = sub.add_parser("publish", help="print the payloads (dry run; nothing reaches a broker)")
    p.add_argument("--dry-run", action="store_true", default=True)
    p.add_argument("--full", action="store_true", help="print the lines whole")
    p.set_defaults(fn=cmd_publish)
    r = sub.add_parser("report", help="the golden figures")
    r.add_argument("--presets", default="MAX,5Y")
    r.set_defaults(fn=cmd_report)
    s = sub.add_parser("schema", help="the settings table")
    s.set_defaults(fn=cmd_schema)
    v = sub.add_parser("view", help="the Lovelace view (JSON) for the dashboard stock-market, from the settings")
    v.set_defaults(fn=cmd_view)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    finally:
        while OPENED:                      # let the store's lock go: the next command may be in the same process
            OPENED.pop().close()


if __name__ == "__main__":
    sys.exit(main())

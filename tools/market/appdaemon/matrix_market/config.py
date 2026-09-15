"""Settings: one declarative schema, and validation, defaults and merging generated from it.

A setting is one `Setting` row in SCHEMA: its dotted key in the payload
(`portfolio.capital`), its type (a name in TYPES), its default, its bounds, its
group (the portal's block), the config `v` that introduced it, and a line of
documentation. Nothing else knows the list of settings: defaults(), validate(),
merge() and the README's table all read SCHEMA. Adding a setting is adding a
row; adding a kind of value is adding a checker to TYPES.

Four layers, each validated with the same schema, the later winning per key:
built-in defaults (the neutral example: the four indices and the 60/40 pair) <
apps.yaml args < the local override <store_dir>/local.json (the real allocation
and the private wiring, never committed, never in the apps tree) < the panel's
retained `config` payload.

The panel's payload carries `v`. Rules (coordinator, 2026-09-15 10:15):
- a key the schema does not know is KEPT (returned in Settings.unknown, echoed
  in the store) and never refused, so a newer portal loses nothing;
- a value the schema refuses (wrong type, out of bounds, unknown enum member)
  is dropped for that key alone - the layer below keeps its value - and named
  in the errors list, which `status` publishes as `cfgErr`;
- a payload without an integer `v` is ignored whole (V_MISSING); a `v` newer
  than this app's is accepted with V_NEWER noted.

Bounds with a source: watch lists <= 8 and positions <= 16 are the design's
bus limits (docs/18, "MQTT contract"); the live refresh >= 60 s is the measured
update period of Yahoo's delayed quotes (docs/18, "Live quotes"); the history
gap >= 10 s is the pacing rule after the 429 episode (docs/18, "The previews").
The others are sanity limits, marked "sanity" in their doc string.
"""
import datetime as dt
import json
import math
import pathlib
import re
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import CONFIG_V, SCHEMA_V
from ._ref import market_ref as M
from .providers.httpio import DEFAULT_UA

Date = dt.date
SYMBOL_RE = re.compile(r"\^?[A-Z0-9][A-Z0-9.\-=]{0,11}")      # ^GSPC, EURUSD=X, IWDA.AS, BRK-B
NAME_RE = re.compile(r"[A-Z0-9.^=\-]{1,8}")                     # the panel's validName: alphabet(s, 1, 8, ".^=-"), capitals only
DEVICE_RE = re.compile(r"[0-9a-f]{6}")
TIME_RE = re.compile(r"([01]\d|2[0-3]):([0-5]\d)")
MAX_WATCH = 8
MAX_POSITIONS = 16
MAX_EXCHANGES = 8
INDEX_NAMES = {"^GSPC": "SPX", "^IXIC": "NDX", "^FCHI": "CAC", "^GDAXI": "DAX", "^FTSE": "FTSE", "^N225": "NKY",
               "^STOXX50E": "SX5E", "^DJI": "DJI", "^SP500TR": "SPTR"}
DEFAULT_EXCHANGES = ["NYSE", "NASDAQ", "LSE", "XETRA", "EURONEXT", "TOKYO"]     # docs/18, "Exchange open/closed"
FEATURE_NAMES = ["history", "portfolio", "live", "tape", "ha_entities"]


class Invalid(ValueError):
    """A refused value: a short code the panel can show, and the reason."""

    def __init__(self, code: str, msg: str):
        super().__init__(msg)
        self.code, self.msg = code, msg


@dataclass(frozen=True)
class Setting:
    key: str                  # dotted path in the payload and in apps.yaml
    type: str                 # a name in TYPES
    default: Any
    bounds: Any = None        # (lo, hi), the allowed names, or a callable giving them
    group: str = "general"    # the portal block: watch, portfolio, lines, display, tape, refresh, ha
    since: int = 1            # the config `v` that introduced the setting
    doc: str = ""


# ── the value checkers ───────────────────────────────────────────────────────
def _resolve(bounds):
    return bounds() if callable(bounds) else bounds


def _is_num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _number(v, b):
    if not _is_num(v):
        raise Invalid("TYPE", "a number is expected")
    lo, hi = b or (None, None)
    if (lo is not None and v < lo) or (hi is not None and v > hi):
        raise Invalid("RANGE", "must be between %s and %s" % (lo, hi))
    return float(v)


def _int(v, b):
    if isinstance(v, bool) or not isinstance(v, int):
        if _is_num(v) and float(v).is_integer():
            v = int(v)
        else:
            raise Invalid("TYPE", "an integer is expected")
    lo, hi = b or (None, None)
    if (lo is not None and v < lo) or (hi is not None and v > hi):
        raise Invalid("RANGE", "must be between %s and %s" % (lo, hi))
    return v


def _bool(v, b):
    if not isinstance(v, bool):
        raise Invalid("TYPE", "true or false is expected")
    return v


def _string(v, b):
    if not isinstance(v, str):
        raise Invalid("TYPE", "a string is expected")
    if b and len(v) > b:
        raise Invalid("RANGE", "at most %d characters" % b)
    return v


def _enum(v, b):
    allowed = _resolve(b)
    if v not in allowed:
        raise Invalid("ENUM", "one of %s is expected" % ", ".join(map(str, allowed)))
    return v


def _date(v, b):
    if v is None:
        return None
    if not isinstance(v, str):
        raise Invalid("TYPE", "an ISO date is expected")
    try:
        d = Date.fromisoformat(v)
    except ValueError:
        raise Invalid("TYPE", "an ISO date YYYY-MM-DD is expected") from None
    lo, hi = _resolve(b) or (None, None)
    if (lo is not None and d < lo) or (hi is not None and d > hi):
        raise Invalid("RANGE", "must be between %s and %s" % (lo, hi))
    return d.isoformat()


def _time(v, b):
    if not isinstance(v, str) or not TIME_RE.fullmatch(v):
        raise Invalid("TYPE", "a time HH:MM is expected")
    return v


def _tz(v, b):
    if v is None:
        return None
    if not isinstance(v, str):
        raise Invalid("TYPE", "an IANA time zone name is expected")
    try:
        from zoneinfo import ZoneInfo
        ZoneInfo(v)
    except Exception:
        raise Invalid("ENUM", "not an IANA time zone") from None
    return v


def _symbol(v, b):
    if v is None:
        return None
    if not isinstance(v, str) or not SYMBOL_RE.fullmatch(v.upper()):
        raise Invalid("TYPE", "a Yahoo symbol is expected (letters, digits, ^ . - =, up to 12)")
    return v.upper()


def _list(v, b, what):
    if not isinstance(v, list):
        raise Invalid("TYPE", "a list of %s is expected" % what)
    lo, hi = b or (0, None)
    if len(v) < lo or (hi is not None and len(v) > hi):
        raise Invalid("RANGE", "between %d and %s %s" % (lo, hi, what))
    return v


def _symbols(v, b):
    """Watch lists: ["^GSPC", ...] or [{"sym": "^GSPC", "name": "SPX"}, ...] -> the dict form, unique."""
    out, seen = [], set()
    for item in _list(v, b, "symbols"):
        if isinstance(item, dict):
            sym = _symbol(item.get("sym"), None)
            name = item.get("name")
            if name is not None and (not isinstance(name, str) or not NAME_RE.fullmatch(name)):
                raise Invalid("TYPE", "%s: a display name is 1-8 of A-Z 0-9 . ^ = - (the panel's validName)" % sym)
        else:
            sym, name = _symbol(item, None), None
        if sym is None:
            raise Invalid("TYPE", "a symbol is missing")
        if sym in seen:
            raise Invalid("DUP", "%s is listed twice" % sym)
        seen.add(sym)
        out.append({"sym": sym, "name": (name or INDEX_NAMES.get(sym) or sym.lstrip("^"))[:8]})
    return out


def _names(v, b):
    allowed = _resolve(b)
    out = []
    for item in _list(v, None, "names"):
        if not isinstance(item, str):
            raise Invalid("TYPE", "names are strings")
        if allowed is not None and item not in allowed:
            raise Invalid("ENUM", "%s is not one of %s" % (item, ", ".join(allowed)))
        if item not in out:
            out.append(item)
    return out


def _positions(v, b):
    """[{"sym": "VFINX", "w": 60, "entry": null, "asset_class": "equity"}, ...]: weights in percent, summing to at most 100."""
    out, seen, total = [], set(), 0.0
    for item in _list(v, b, "positions"):
        if not isinstance(item, dict):
            raise Invalid("TYPE", "a position is {sym, w, entry}")
        sym = _symbol(item.get("sym"), None)
        if sym is None:
            raise Invalid("TYPE", "a position without a symbol")
        if sym in seen:
            raise Invalid("DUP", "%s is held twice" % sym)
        w = _number(item.get("w"), (0.0, 100.0))
        if w <= 0:
            raise Invalid("RANGE", "%s: the weight must be above 0" % sym)
        entry = _date(item.get("entry"), None)
        cls = item.get("asset_class", ASSET_CLASS_DEFAULTS.get(sym))
        if cls is not None and cls not in ASSET_CLASSES:
            raise Invalid("ENUM", "%s: asset_class is one of %s" % (sym, ", ".join(ASSET_CLASSES)))
        proxy = _symbol(item.get("proxy"), None)
        if proxy == sym:
            raise Invalid("DUP", "%s: a position is not its own proxy" % sym)
        seen.add(sym)
        total += w
        row = {"sym": sym, "w": w, "entry": entry}
        if cls is not None:
            row["asset_class"] = cls
        if proxy:
            row["proxy"] = proxy
        out.append(row)
    if total > 100.0 + 1e-6:
        raise Invalid("RANGE", "weights sum to %.2f %%, above 100" % total)
    return out


def _blend(v, b):
    """bench.blend: a preset name ("60_40", "same_weights") or legs [{"sym", "w"}] summing to at most 100
    (the panel's T_WLIST); an empty list means the portfolio's own weights."""
    if isinstance(v, str):
        if v not in BLEND_PRESETS:
            raise Invalid("ENUM", "a blend preset is one of %s" % ", ".join(BLEND_PRESETS))
        return v
    legs, total = [], 0.0
    if isinstance(v, list) and not v:
        return "same_weights"
    for item in _list(v, (1, 4), "benchmark legs"):
        if not isinstance(item, dict):
            raise Invalid("TYPE", "a leg is {sym, w}")
        sym = _symbol(item.get("sym"), None)
        if sym is None:
            raise Invalid("TYPE", "a leg without a symbol")
        w = _number(item.get("w"), (0.0, 100.0))
        legs.append({"sym": sym, "w": w})
        total += w
    if total > 100.0 + 1e-6:
        raise Invalid("RANGE", "blend weights sum to %.2f %%, above 100" % total)
    return legs


def _ter_map(v, b):
    """{"VWCE.DE": 0.22}: expense ratios in PERCENT per year, as the portal shows them."""
    if not isinstance(v, dict):
        raise Invalid("TYPE", "a map symbol -> TER in percent is expected")
    out = {}
    for k, x in v.items():
        sym = _symbol(k, None)
        out[sym] = _number(x, b)
    return out


TYPES: Dict[str, Callable[[Any, Any], Any]] = {
    "number": _number, "int": _int, "bool": _bool, "string": _string, "enum": _enum, "date": _date,
    "time": _time, "tz": _tz, "symbol": _symbol, "symbols": _symbols, "names": _names,
    "positions": _positions, "ter_map": _ter_map, "blend": _blend,
}


# ── the schema ───────────────────────────────────────────────────────────────
def _today() -> Date:
    return dt.date.today()


def _inception_bounds():
    return (Date(1990, 1, 1), _today())


def _exchange_names():
    from .registry import EXCHANGES          # filled by tape.py when it is imported
    return EXCHANGES.names() if EXCHANGES.names() else None


ASSET_CLASSES = ("equity", "bond", "real_assets", "gold", "cash")
# The classes of the neutral example's funds; the panel sends asset_class per position, so any other
# symbol carries the class the portal gave it (none otherwise).
ASSET_CLASS_DEFAULTS = {"VFINX": "equity", "VBMFX": "bond"}
BLEND_PRESETS = ("60_40", "same_weights")
# The 60/40 preset's one constant (coordinator, 2026-09-15, settled): Vanguard 500 Index Investor 60 + Vanguard
# Total Bond Market Index Investor 40, both on adjclose, both with bars from 2000-01-01 (AGG 2003-09, BND 2007-04
# and VBTLX 2001-11 were rejected for not covering 2000). Mirrors Vanguard's Balanced Composite (docs/19).
BLEND_60_40 = [{"sym": "VFINX", "w": 60.0}, {"sym": "VBMFX", "w": 40.0}]
ALL_PRESETS = ("WTD", "MTD", "1M", "3M", "6M", "YTD", "1Y", "3Y", "5Y", "10Y", "MAX")   # the panel's order
EXTRA_PRESETS = ("1M", "3M", "6M", "MTD", "WTD")
TR_BENCH = "^SP500TR"                                                     # the default benchmark (owner, 2026-09-15 11:43)
# The committed defaults are a NEUTRAL example (privacy rule, 2026-09-15: this fork is public, the
# owner's allocation is not committed): the 60/40 pair of the blend preset as a two-fund portfolio.
# The real allocation lives in <store_dir>/local.json (never in the apps tree), merged over these by the app.
EXAMPLE_POSITIONS = [{"sym": "VFINX", "w": 60.0, "entry": None, "asset_class": "equity"},
                     {"sym": "VBMFX", "w": 40.0, "entry": None, "asset_class": "bond"}]
# The local override: JSON, in the store directory (/config/market/local.json in the add-on), NEVER in the
# apps tree - AppDaemon reads every .yaml and .toml under apps/ as app configuration and raises BadAppConfig
# on a key that is not an app (app_management.get_app_config_files / AllAppConfig, via deepwiki; audit M2).
LOCAL_FILE = "local.json"
DEFAULT_STORE_DIR = "/config/market"      # the add-on maps addon_config:rw to /config (its config.yaml `map`; audit M1)
LOCAL_WIRING_KEYS = ("device", "mqtt_host", "mqtt_port", "topic_base")   # the private wiring may live there too
DEFAULT_INDICES = [{"sym": s, "name": INDEX_NAMES[s]} for s in ("^GSPC", "^IXIC", "^FCHI", "^GDAXI")]

SCHEMA: List[Setting] = [
    # The keys, groups and bounds are the panel's (src/market/market_settings.cpp on wip/market-panel, the
    # reference contract, 2026-09-15); a v1 key is marked since 1, the inventory's since 2.
    # watch
    Setting("indices", "symbols", DEFAULT_INDICES, (1, MAX_WATCH), "watch", 1,
            "the MARKETS page's indices, {sym, name}; first = primary and the default benchmark; <= 8 (bus limit)"),
    Setting("tickers", "symbols", [], (0, MAX_WATCH), "watch", 1, "the TICKER page's symbols, {sym, name}; <= 8 (bus limit)"),
    Setting("ticker", "symbol", None, None, "watch", 1, "the symbol the panel shows on TICKER; its intraday line is polled"),
    Setting("presets", "names", list(M.PRESETS), lambda: list(ALL_PRESETS), "watch", 1,
            "the windows published: the six from YTD and the extras the panel switched on (WTD MTD 1M 3M 6M)"),
    # portfolio
    Setting("portfolio.capital", "number", 10000.0, (100.0, 1e9), "portfolio", 1, "the initial capital, in the portfolio currency"),
    Setting("portfolio.currency", "enum", "EUR", ("EUR", "USD"), "portfolio", 1, "the portfolio currency; the FX pair the design has is EURUSD=X"),
    Setting("portfolio.inception", "date", "2000-01-01", _inception_bounds, "portfolio", 1, "the inception date S; from 1990 (sanity) to today"),
    Setting("portfolio.contrib.amount", "number", 0.0, (0.0, 1e8), "portfolio", 1, "an extra contribution, 0 for none"),
    Setting("portfolio.contrib.every", "enum", "year", ("month", "quarter", "year"), "portfolio", 1, "the contribution period, from the inception's anniversary"),
    Setting("portfolio.rebalance", "bool", False, None, "portfolio", 1,
            "docs/18's v1 flag; the panel derives it from rebal.mode != hold, and so does the app (rebal.mode decides)"),
    Setting("portfolio.positions", "positions", EXAMPLE_POSITIONS, (0, MAX_POSITIONS), "portfolio", 1,
            "the allocation: {sym, w, asset_class, [entry], [proxy]}, weights in percent summing to at most 100; <= 16 (bus limit)"),
    Setting("rebal.mode", "enum", "hold", ("hold", "calendar", "bands", "calendar_or_bands"), "portfolio", 2,
            "the model the panel shows: hold, or REBAL's rule (calendar = the approved 31 December); REBAL is always computed"),
    # returns
    Setting("returns.measure", "enum", "twr", ("twr", "mwr"), "returns", 2, "the measure named in the payload; chg is always the TWR, xirr the MWR"),
    Setting("returns.real", "bool", False, None, "returns", 2, "real terms: deflate by the euro-area HICP (ECB ICP.M.U2.N.000000.4.INX); unavailable -> status"),
    Setting("dividends.mode", "enum", "sweep_yearend", ("sweep_yearend", "reinvest_paydate", "drop"), "returns", 2,
            "dividends to cash until 31 December (the owner's rule) | reinvested at the ex-date close (PV) | dropped"),
    # rebalancing
    Setting("rebal.calendar", "enum", "annual", ("annual", "semiannual", "quarterly", "monthly"), "rebal", 2, "the calendar rule's period ends"),
    Setting("rebal.bands.abs_pts", "number", 5.0, (0.0, 50.0), "rebal", 2, "Swedroe 5/25: +-points for targets of 20 % or more"),
    Setting("rebal.bands.rel_pct", "number", 25.0, (0.0, 100.0), "rebal", 2, "Swedroe 5/25: +-percent of the target below 20 %"),
    Setting("rebal.bands.check", "enum", "monthly", ("daily", "weekly", "monthly"), "rebal", 2, "how often the bands are tested"),
    Setting("rebal.contrib_only", "bool", False, None, "rebal", 2, "new cash buys the underweight positions the day it arrives"),
    Setting("rebal.reserve", "enum", "value_weight", ("value_weight", "capital_weight"), "rebal", 2,
            "what a REBAL keeps in cash for a fund not trading yet: V x w (value_weight, the owner's decision 2026-09-15 12:53) "
            "or (C + contributions) x w (capital_weight); app-only, the panel does not send it"),
    # cash and flows
    Setting("contrib.index_inflation", "bool", False, None, "flows", 2, "grow the contribution with the HICP (needs the HICP table)"),
    Setting("withdraw.amount", "number", 0.0, (0.0, 1e8), "flows", 2, "a periodic withdrawal, 0 for none"),
    Setting("withdraw.kind", "enum", "amount", ("amount", "pct"), "flows", 2, "an amount in the portfolio currency, or a percent of the balance"),
    Setting("withdraw.every", "enum", "year", ("month", "quarter", "year"), "flows", 2, "the withdrawal period, from the inception's anniversary"),
    Setting("cash.yield_pct", "number", 0.0, (0.0, 20.0), "flows", 2, "interest on the cash row, percent per year"),
    # fees and taxes (the owner's rule is TER only, so all 0)
    Setting("ter", "ter_map", {}, (0.0, 5.0), "fees", 1, "TER overrides in percent per year, for funds Yahoo lacks"),
    Setting("tax.div_withholding_pct", "number", 0.0, (0.0, 100.0), "fees", 2, "withholding on dividends, percent"),
    Setting("costs.per_trade_fixed", "number", 0.0, (0.0, 1000.0), "fees", 2, "a fixed cost per buy or sell, in the portfolio currency"),
    Setting("costs.per_trade_pct", "number", 0.0, (0.0, 5.0), "fees", 2, "a cost per trade in percent of the amount"),
    # currency
    Setting("display.fx_effect", "bool", False, None, "currency", 2, "the panel shows the FX part of the return; the app publishes `fx` regardless"),
    # benchmark
    Setting("bench.mode", "enum", "single", ("single", "blend"), "bench", 2, "one symbol's price index, or a blend of adjclose legs"),
    Setting("bench.symbol", "symbol", TR_BENCH, None, "bench", 2,
            "the benchmark on the panel, in the portfolio currency, scaled to the capital (owner, 11:43: ^SP500TR); null = the first index"),
    Setting("bench.blend", "blend", "same_weights", None, "bench", 2,
            "the blend: legs [{sym, w}] (the portal's presets fill them), or 60_40 (VFINX 60 + VBMFX 40, adjclose) | same_weights; [] = same_weights"),
    Setting("bench.extra", "symbols", [{"sym": TR_BENCH, "name": "SPTR"}], (0, 2), "bench", 2, "up to two more benchmarks for HA's sensors; ^SP500TR is always fetched"),
    Setting("bench.telltale", "bool", False, None, "bench", 2, "publish portfolio / benchmark as a line (Simba's telltale); it takes the benchmark line's slot"),
    # lines (the app's own: the panel decides what it draws with its panel-only display.lines.*)
    Setting("lines.bench", "bool", True, None, "lines", 1, "publish the benchmark line"),
    Setting("lines.gross", "bool", False, None, "lines", 1, "publish the gross-of-fees EST line (off by default, docs/18)"),
    # risk
    Setting("mdd_basis", "enum", "daily", ("daily", "month_end"), "risk", 2, "the drawdown basis of `mdd`; month_end is Portfolio Visualizer's"),
    Setting("stats.risk_free_pct", "number", 0.0, (0.0, 20.0), "risk", 2, "the risk-free rate for Sharpe and Sortino, percent per year"),
    # display
    Setting("display.preset", "enum", "1Y", lambda: list(ALL_PRESETS), "display", 1, "the window the HA return/MDD/stats sensors report"),
    Setting("display.tz", "tz", None, None, "display", 1, "the panel's local time zone for the tape's next-change times; null = AppDaemon's"),
    Setting("display.scale", "enum", "linear", ("linear", "log"), "display", 2, "log packs log10 values and flags the payload with scale: log"),
    # tape
    Setting("tape.exchanges", "names", list(DEFAULT_EXCHANGES), _exchange_names, "display", 1, "the exchanges on the tape, in order; <= 8"),
    # data
    Setting("live.poll_s", "int", 60, (30, 300), "data", 2, "the live poll period while an exchange is open; 60 is Yahoo's measured update period"),
    Setting("fetch.daily_at", "time", "07:00", None, "data", 2, "the daily fetch, local time (docs/18: 07:00)"),
    Setting("fetch.history_gap_s", "int", 10, (10, 600), "data", 2, "seconds between two history requests; >= 10 is the pacing rule (apps.yaml only)"),
    Setting("fetch.ter_days", "int", 30, (1, 365), "data", 2, "how often the TER is read again (docs/18: 30 days; apps.yaml only)"),
    Setting("fetch.keepalive_h", "int", 6, (1, 48), "data", 2, "republish everything this often (docs/18: 6 h; apps.yaml only)"),
    Setting("stale.session_min", "int", 20, (5, 120), "data", 2, "a quote older than this while the calendar says open is STALE (docs/18)"),
    Setting("stale.history_days", "int", M.STALE_TRADING_DAYS, (1, 10), "data", 2, "AS OF older than this many trading days -> stale (docs/18: 3)"),
    # alerts (HA binary sensors only, all off, no notifications)
    Setting("alert.day_move_pct", "number", 0.0, (0.0, 50.0), "alerts", 2, "a binary sensor when a watched symbol moves more than this in the day; 0 = off"),
    Setting("alert.drawdown_pct", "number", 0.0, (0.0, 90.0), "alerts", 2, "a binary sensor when the portfolio is more than this below its peak; 0 = off"),
    Setting("alert.out_of_band", "bool", False, None, "alerts", 2, "a binary sensor when any holding is outside its Swedroe band"),
    # ha
    Setting("ha.discovery", "bool", True, None, "ha", 1, "publish MQTT discovery for the Home Assistant sensors"),
    Setting("ha.prefix", "string", "homeassistant", 32, "ha", 1, "the MQTT discovery prefix"),
]
SCHEMA_BY_KEY: Dict[str, Setting] = {s.key: s for s in SCHEMA}

# apps.yaml's own arguments: the wiring, never sent by the panel.
APP_SCHEMA: List[Setting] = [
    Setting("device", "string", None, 12, "app", 1, "the panel's six lower-case hex digits; <panel-id> in the committed yaml (set it there, in !secret or in local.json)"),
    Setting("topic_base", "string", "nickoscope_matrix", 40, "app", 1, "the MQTT root"),
    Setting("mqtt_host", "string", None, 64, "app", 1, "the broker, by IP (the container has no mDNS); <your-broker> in the committed yaml"),
    Setting("mqtt_port", "int", 1883, (1, 65535), "app", 1, ""),
    Setting("mqtt_user", "string", None, 64, "app", 1, "!secret nicko_mqtt_user"),
    Setting("mqtt_pass", "string", None, 128, "app", 1, "!secret nicko_mqtt_pass"),
    Setting("store_dir", "string", DEFAULT_STORE_DIR, 255, "app", 1, "the store, inside the add-on's /config mount"),
    Setting("local_file", "string", None, 255, "app", 2, "the local override JSON (the real allocation, the private wiring); default <store_dir>/local.json, \"\" for none; never under apps/"),
    Setting("provider", "string", "yahoo", 16, "app", 1, "a name in PROVIDERS"),
    Setting("user_agent", "string", DEFAULT_UA, 200, "app", 2,
            "the User-Agent of every request; Yahoo answered 200 to this one and 429 to others (measured 2026-09-15, Yahoo can change it)"),
    Setting("features", "names", list(FEATURE_NAMES), None, "app", 1, "the features to wire, names in FEATURES"),
]


# ── nested dicts by dotted key ───────────────────────────────────────────────
def get_path(d: dict, key: str, default=None):
    node = d
    for part in key.split("."):
        if not isinstance(node, dict) or part not in node:
            return default
        node = node[part]
    return node


def set_path(d: dict, key: str, value) -> None:
    parts = key.split(".")
    node = d
    for part in parts[:-1]:
        node = node.setdefault(part, {})
        if not isinstance(node, dict):
            raise Invalid("TYPE", "%s is not a group" % key)
    node[parts[-1]] = value


def has_path(d: dict, key: str) -> bool:
    sentinel = object()
    return get_path(d, key, sentinel) is not sentinel


def _copy(v):
    if isinstance(v, dict):
        return {k: _copy(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_copy(x) for x in v]
    return v


def defaults(schema: List[Setting] = SCHEMA) -> dict:
    out: dict = {}
    for s in schema:
        set_path(out, s.key, _copy(s.default))
    return out


def known_paths(d: dict, prefix: str = "") -> List[str]:
    """Every leaf path in a nested dict; a group of the schema is descended, a leaf is not."""
    out = []
    for k, v in d.items():
        path = prefix + k
        if isinstance(v, dict) and path not in SCHEMA_BY_KEY and any(s.key.startswith(path + ".") for s in SCHEMA):
            out.extend(known_paths(v, path + "."))
        else:
            out.append(path)
    return out


def validate(payload: dict, base: Optional[dict] = None, schema: List[Setting] = SCHEMA,
             source: str = "config") -> Tuple[dict, List[dict], dict]:
    """(settings, errors, unknown): `payload`'s known keys checked and laid over `base`
    (or the defaults); a refused key keeps the base's value and lands in `errors` as
    {"key", "code", "msg"}; keys the schema does not know come back in `unknown`."""
    out = _copy(base) if base is not None else defaults(schema)
    errors: List[dict] = []
    unknown: dict = {}
    if not isinstance(payload, dict):
        return out, [{"key": "", "code": "TYPE", "msg": "%s is not an object" % source}], unknown
    by_key = {s.key: s for s in schema}
    for path in known_paths(payload):
        if path == "v" and source == "config":
            continue
        value = get_path(payload, path)
        s = by_key.get(path)
        if s is None:
            set_path(unknown, path, _copy(value))
            continue
        try:
            set_path(out, path, TYPES[s.type](value, s.bounds))
        except Invalid as e:
            errors.append({"key": path, "code": e.code, "msg": e.msg})
    return out, errors, unknown


def check_version(payload: dict) -> Optional[dict]:
    """None when the payload's `v` is usable, else the error that refuses or notes it."""
    v = payload.get("v") if isinstance(payload, dict) else None
    if isinstance(v, bool) or not isinstance(v, int):
        return {"key": "v", "code": "V_MISSING", "msg": "config carries no integer v; ignored"}
    if v < 1:
        return {"key": "v", "code": "V_BAD", "msg": "config v %d is not a version" % v}
    if v > CONFIG_V:
        return {"key": "v", "code": "V_NEWER", "msg": "config v %d is newer than this app's %d; known keys used" % (v, CONFIG_V)}
    return None


# ── the effective settings ───────────────────────────────────────────────────
class Settings:
    """The effective settings: a nested dict with every key present, the errors that
    produced it, and the unknown keys kept aside. Immutable by convention."""

    def __init__(self, data: dict, errors: Optional[List[dict]] = None, unknown: Optional[dict] = None, v: int = CONFIG_V):
        self.data, self.errors, self.unknown, self.v = data, list(errors or []), unknown or {}, v

    def get(self, key: str, default=None):
        return get_path(self.data, key, default)

    def __getitem__(self, key: str):
        sentinel = object()
        v = get_path(self.data, key, sentinel)
        if v is sentinel:
            raise KeyError(key)
        return v

    # the lists the features work from
    def indices(self) -> List[dict]:
        return list(self.get("indices") or [])

    def tickers(self) -> List[dict]:
        return list(self.get("tickers") or [])

    def positions(self) -> List[dict]:
        return list(self.get("portfolio.positions") or [])

    def presets(self) -> List[str]:
        """The windows published, in the panel's order: the list the panel sent (the six from YTD and
        the extras switched on), the six standard ones always."""
        wanted = set(self.get("presets") or M.PRESETS) | set(M.PRESETS)
        return [p for p in ALL_PRESETS if p in wanted]

    def benchmark(self) -> Optional[str]:
        """The panel's benchmark symbol (single mode, or the blend's first leg's name is not a symbol)."""
        b = self.get("bench.symbol")
        if b:
            return b
        idx = self.indices()
        return idx[0]["sym"] if idx else None

    def blend_legs(self) -> List[dict]:
        """The blend's legs [{sym, w}], presets resolved; [] when bench.mode is single."""
        if self.get("bench.mode") != "blend":
            return []
        b = self.get("bench.blend")
        if b == "60_40":
            return [dict(x) for x in BLEND_60_40]
        if b == "same_weights":
            return [{"sym": p["sym"], "w": p["w"]} for p in self.positions()]
        return [dict(x) for x in (b or [])]

    def bench_symbols(self) -> List[str]:
        """Every symbol the benchmarks need: the primary, the blend legs, the extras, ^SP500TR always."""
        out: List[str] = []
        for sym in [self.benchmark()] + [l["sym"] for l in self.blend_legs()] + [x["sym"] for x in (self.get("bench.extra") or [])] + [TR_BENCH]:
            if sym and sym not in out:
                out.append(sym)
        return out

    def rules(self) -> "M.Rules":
        """market_ref.Rules from the settings; with the defaults it is Rules() (= simulate()). REBAL's rule is
        rebal.mode, or the approved calendar when the panel shows HOLD (both modes are always computed)."""
        rebal = self["rebal.mode"]
        kind = self["withdraw.kind"]
        return M.Rules(dividends=self["dividends.mode"], rebal="calendar" if rebal == "hold" else rebal,
                       calendar=self["rebal.calendar"],
                       band_abs_pts=float(self["rebal.bands.abs_pts"]), band_rel_pct=float(self["rebal.bands.rel_pct"]),
                       band_check=self["rebal.bands.check"], contrib_only=bool(self["rebal.contrib_only"]),
                       withdraw_amount=float(self["withdraw.amount"]) if kind == "amount" else 0.0,
                       withdraw_pct=float(self["withdraw.amount"]) if kind == "pct" else 0.0,
                       withdraw_every=self["withdraw.every"], cash_yield_pct=float(self["cash.yield_pct"]),
                       tax_div_pct=float(self["tax.div_withholding_pct"]), cost_fixed=float(self["costs.per_trade_fixed"]),
                       cost_pct=float(self["costs.per_trade_pct"]), reserve=self["rebal.reserve"])

    def asset_classes(self) -> Dict[str, Optional[str]]:
        return {p["sym"]: p.get("asset_class") for p in self.positions()}

    def proxies(self) -> Dict[str, str]:
        return {p["sym"]: p["proxy"] for p in self.positions() if p.get("proxy")}

    def fx_symbols(self) -> List[str]:
        """The FX series the ledger needs: EURUSD=X whenever EUR and USD both appear (the design's one pair)."""
        return ["EURUSD=X"]

    def watched_symbols(self) -> List[str]:
        """Every symbol the daily fetch keeps: indices, tickers, positions, proxies, the benchmarks, FX. Ordered, unique."""
        out: List[str] = []
        for sym in ([i["sym"] for i in self.indices()] + [t["sym"] for t in self.tickers()]
                    + [p["sym"] for p in self.positions()] + list(self.proxies().values()) + self.bench_symbols()
                    + self.fx_symbols()):
            if sym and sym not in out:
                out.append(sym)
        return out

    def status_symbols(self) -> List[str]:
        """The order of status.symbols: the positions first (their err explains a DATA ERR on PORTFOLIO),
        then indices, tickers and the rest - the panel keeps the first 16 entries."""
        first = [p["sym"] for p in self.positions()]
        return first + [sym for sym in self.watched_symbols() if sym not in first]

    def live_symbols(self) -> List[str]:
        """What the live poll asks for: indices, tickers and the positions (no FX, no benchmark-only or proxy symbols)."""
        out: List[str] = []
        for sym in [i["sym"] for i in self.indices()] + [t["sym"] for t in self.tickers()] + [p["sym"] for p in self.positions()]:
            if sym not in out:
                out.append(sym)
        return out

    def mode(self) -> str:
        """The model the panel shows: rebal.mode decides (the panel derives portfolio.rebalance from it)."""
        return "hold" if self.get("rebal.mode", "hold") == "hold" else "rebal"

    def ter_overrides(self) -> Dict[str, float]:
        """Fractions per year (the portal's percent / 100)."""
        return {k: v / 100.0 for k, v in (self.get("ter") or {}).items()}

    def ref_config(self, ter_fraction: Optional[Dict[str, float]] = None) -> "M.Config":
        """The maths' Config. TERs are attached to the Series, not here."""
        pos = [M.Position(p["sym"], float(p["w"]), Date.fromisoformat(p["entry"]) if p.get("entry") else None)
               for p in self.positions()]
        return M.Config(float(self["portfolio.capital"]), self["portfolio.currency"],
                        Date.fromisoformat(self["portfolio.inception"]), pos,
                        float(self["portfolio.contrib.amount"]), self["portfolio.contrib.every"],
                        self.mode() == "rebal")

    def to_payload(self) -> dict:
        """The effective config as a `config` payload (the store keeps it; the CLI prints it)."""
        out = _copy(self.data)
        for path in known_paths(self.unknown):
            set_path(out, path, get_path(self.unknown, path))
        out["v"] = CONFIG_V
        return out


def from_defaults() -> Settings:
    return Settings(defaults(), [], {}, CONFIG_V)


APPDAEMON_KEYS = ("module", "class", "dependencies", "global_dependencies", "log", "disable", "plugin", "pin_app", "pin_thread")


def deep_merge(base: dict, over: dict) -> dict:
    out = _copy(base)
    for k, v in (over or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = _copy(v)
    return out


class LocalFileError(ValueError):
    """The local override exists but is not a JSON object."""


def load_local(path) -> Optional[dict]:
    """The local override (the real allocation, the private wiring): a JSON object with the settings keys of
    apps.yaml and, optionally, LOCAL_WIRING_KEYS. None when the file is absent; LocalFileError when it is
    not a JSON object (the caller logs the file's name, never its content)."""
    p = pathlib.Path(path)
    if not p.exists():
        return None
    try:
        d = json.loads(p.read_text("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise LocalFileError("%s is not readable JSON (%s)" % (p.name, type(e).__name__)) from None
    if not isinstance(d, dict):
        raise LocalFileError("%s is not a JSON object" % p.name)
    return {k: v for k, v in d.items() if k not in APPDAEMON_KEYS}


def merge_wiring(args: dict, local: Optional[dict]) -> dict:
    """apps.yaml's arguments with the local override's private wiring laid over them."""
    out = dict(args or {})
    for k in LOCAL_WIRING_KEYS:
        if local and k in local:
            out[k] = local[k]
    return out


def from_args(args: dict, local: Optional[dict] = None) -> Settings:
    """apps.yaml (and the local override laid over it): the settings keys over the defaults; the wiring
    keys of APP_SCHEMA are not settings."""
    wiring = {s.key for s in APP_SCHEMA}
    merged = deep_merge(args or {}, {k: v for k, v in (local or {}).items() if k not in LOCAL_WIRING_KEYS})
    payload = {k: v for k, v in merged.items() if k not in wiring and k not in APPDAEMON_KEYS}
    data, errors, unknown = validate(payload, None, SCHEMA, "apps.yaml")
    return Settings(data, errors, unknown, CONFIG_V)


# ── what a config change touches (the panel's audit rule, 2026-09-15) ────────
TICKER_KEYS = {"ticker"}
SCHEDULE_KEYS = {"live.poll_s", "stale.session_min", "stale.history_days", "fetch.daily_at", "fetch.keepalive_h",
                 "fetch.ter_days", "fetch.history_gap_s"}
LIGHT_KEYS = {"display.tz", "display.fx_effect", "ha.discovery", "ha.prefix"}     # no history, no maths: the features react


def changed_keys(a: dict, b: dict, prefix: str = "") -> set:
    """The dotted leaf paths whose values differ between two settings dicts."""
    out = set()
    for k in set(a) | set(b):
        path = prefix + k
        va, vb = a.get(k), b.get(k)
        if isinstance(va, dict) and isinstance(vb, dict) and path not in SCHEMA_BY_KEY:
            out |= changed_keys(va, vb, path + ".")
        elif va != vb:
            out.add(path)
    return out


def classes(changed: set) -> set:
    """Every class present among the changed keys - ticker, schedule, light, maths - since one save can carry
    several (audit MINOR 5): the app handles each of them."""
    out = set()
    if changed & TICKER_KEYS:
        out.add("ticker")
    if changed & SCHEDULE_KEYS:
        out.add("schedule")
    if changed & LIGHT_KEYS:
        out.add("light")
    if changed - TICKER_KEYS - SCHEDULE_KEYS - LIGHT_KEYS:
        out.add("maths")
    return out


def classify(changed: set) -> str:
    """none | ticker | schedule | light | maths - the heaviest class present, for the log."""
    present = classes(changed)
    return next((k for k in ("maths", "schedule", "light", "ticker") if k in present), "none")


def from_panel(base: Settings, payload: Any) -> Settings:
    """The panel's `config` over `base`. A payload without a usable `v` leaves `base` as it is
    (its error is still reported); a refused key keeps base's value."""
    ver = check_version(payload)
    if ver and ver["code"] != "V_NEWER":
        return Settings(_copy(base.data), [ver], _copy(base.unknown), base.v)
    data, errors, unknown = validate(payload, base.data, SCHEMA, "config")
    if ver:
        errors.insert(0, ver)
    merged_unknown = _copy(base.unknown)
    for path in known_paths(unknown):
        set_path(merged_unknown, path, get_path(unknown, path))
    return Settings(data, errors, merged_unknown, payload.get("v", CONFIG_V))


def app_args(args: dict, require_broker: bool = True) -> Tuple[dict, List[dict]]:
    """The wiring arguments checked against APP_SCHEMA; a missing device is fatal (ValueError)."""
    data, errors, _ = validate({k: v for k, v in (args or {}).items() if k in {s.key for s in APP_SCHEMA}}, None, APP_SCHEMA, "apps.yaml")
    dev = data.get("device")
    if not isinstance(dev, str) or not DEVICE_RE.fullmatch(dev):
        raise ValueError("matrix_market: device must be the panel's six lower-case hex digits "
                         "(the committed yaml has the placeholder <panel-id>: set it in apps.yaml, !secret or local.json)")
    host = data.get("mqtt_host")
    if require_broker and (not isinstance(host, str) or not host or "<" in host):
        raise ValueError("matrix_market: mqtt_host is missing or still the placeholder <your-broker>: "
                         "set it in apps.yaml, !secret or local.json")
    bad = [f for f in data.get("features") or [] if f not in FEATURE_NAMES]
    if bad:
        errors.append({"key": "features", "code": "ENUM", "msg": "unknown features left out: %s" % ", ".join(bad)})
        data["features"] = [f for f in data["features"] if f in FEATURE_NAMES]
    return data, errors


def schema_table(schema: List[Setting] = SCHEMA) -> str:
    """The README's settings table, generated so it cannot drift from the code."""
    rows = ["| Key | Type | Default | Bounds | Group | Since | What |", "|---|---|---|---|---|---|---|"]
    for s in schema:
        d = s.default
        if isinstance(d, list) and d and isinstance(d[0], dict):
            d = "%d entries (docs/18)" % len(d)
        b = s.bounds
        b = "at validation" if callable(b) else ("" if b is None else str(b))
        rows.append("| `%s` | %s | `%s` | %s | %s | %d | %s |" % (s.key, s.type, d, b, s.group, s.since, s.doc))
    return "\n".join(rows)

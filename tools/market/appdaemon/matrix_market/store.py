"""The store: one versioned JSON file per symbol, bars appended, a manifest.

    <store>/manifest.json            versions, per-symbol summary, fetched-at, errors
    <store>/sym/<escaped symbol>.json one symbol: dates, closes, dividends, splits, meta, TER
    <store>/ecb_EXR_M_USD_EUR_SP00_E.csv  the ECB end-of-period EUR/USD table (providers/ecb.py, market_ref's loader)
    <store>/config_last.json          the effective settings last applied

Append-only: a fetch that overlaps the stored bars is compared on the last
OVERLAP_BARS stored closes; when they agree the new bars after the stored last
date are appended. When they differ (a split or another corporate action has
changed past closes) NOTHING is written: save() answers "mismatch", and the
caller fetches the whole history again and saves that with full=True, which
replaces the file only when the full answer starts where the stored history
does (FULL_SLACK_DAYS). A partial answer never replaces a file (audit B1,
2026-09-15: 6 966 bars became 44 when a 60-day answer replaced them). The
caller also never hands in the bar of a session that is still open
(history.closed_bars). A file whose version is not STORE_V is refetched.

Plain JSON, standard library: ~6 700 daily bars with dates, closes and adjusted
closes are ~340 KB per symbol, which the design's "Parquet or CSV" would not
beat by enough to add pandas here.
"""
import datetime as dt
import json
import pathlib
import re
import tempfile
import time
from typing import Dict, Iterable, List, Optional, Tuple

from ._ref import market_ref as M

STORE_V = 1
OVERLAP_BARS = 30
FULL_SLACK_DAYS = 7      # a full answer may start this many days after the stored first bar (a holiday, a dropped hole)
META_KEEP = ("symbol", "currency", "exchangeName", "fullExchangeName", "instrumentType", "exchangeTimezoneName",
             "gmtoffset", "currentTradingPeriod", "regularMarketTime", "regularMarketPrice", "chartPreviousClose",
             "previousClose", "longName", "shortName", "dataGranularity", "range", "splits")
Date = dt.date


def escape(sym: str) -> str:
    """A file-name-safe symbol: ^GSPC -> _5eGSPC, EURUSD=X -> EURUSD_3dX, IWDA.AS stays."""
    return re.sub(r"[^A-Za-z0-9._-]", lambda m: "_%02x" % ord(m.group()), sym)


def _now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _atomic_write(path: pathlib.Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=path.name, suffix=".tmp")
    try:
        with open(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, separators=(",", ":"), ensure_ascii=False)
        pathlib.Path(tmp).replace(path)
    except Exception:
        pathlib.Path(tmp).unlink(missing_ok=True)
        raise


def trim_meta(meta: dict) -> dict:
    return {k: meta[k] for k in META_KEEP if k in meta}


LOCK_WAIT_S = 10.0      # how long a new Store waits for the previous app instance (a reload) to let go


class Store:
    """One writer per store directory (audit MINOR 7): an exclusive flock on <root>/.lock, held until close().
    A Store that cannot take it within `wait_s` reads but writes nothing (`writable` False, `refused_writes`
    counts), so an app reloaded while its predecessor's worker still runs cannot interleave manifest writes."""

    def __init__(self, root, lock: bool = True, wait_s: float = LOCK_WAIT_S):
        self.root = pathlib.Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "sym").mkdir(exist_ok=True)
        self._lock_fh = None
        self.refused_writes = 0
        self.writable = self._acquire(wait_s) if lock else True
        self.manifest = self._load_manifest()

    def _acquire(self, wait_s: float) -> bool:
        try:
            import fcntl
        except ImportError:                 # no flock on this system: one app per store is the maintainer's to keep
            return True
        fh = open(self.root / ".lock", "a+")
        deadline = time.monotonic() + max(0.0, wait_s)
        while True:
            try:
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                self._lock_fh = fh
                return True
            except OSError:
                if time.monotonic() >= deadline:
                    fh.close()
                    return False
                time.sleep(0.2)

    def close(self) -> None:
        if self._lock_fh is not None:
            try:
                import fcntl
                fcntl.flock(self._lock_fh.fileno(), fcntl.LOCK_UN)
            finally:
                self._lock_fh.close()
                self._lock_fh = None

    def _write(self, path: pathlib.Path, data: dict) -> None:
        if not self.writable:
            self.refused_writes += 1
            return
        _atomic_write(path, data)

    # ── manifest ─────────────────────────────────────────────────────────────
    def _load_manifest(self) -> dict:
        p = self.root / "manifest.json"
        if p.exists():
            try:
                m = json.loads(p.read_text("utf-8"))
                if isinstance(m, dict) and m.get("v") == STORE_V:
                    m.setdefault("symbols", {})
                    return m
            except (ValueError, OSError):
                pass
        return {"v": STORE_V, "symbols": {}, "created": _now_iso()}

    def write_manifest(self) -> None:
        self.manifest["updated"] = _now_iso()
        self._write(self.root / "manifest.json", self.manifest)

    def entry(self, sym: str) -> dict:
        return self.manifest["symbols"].setdefault(sym, {})

    def symbols(self) -> List[str]:
        return sorted(self.manifest["symbols"])

    # ── one symbol ───────────────────────────────────────────────────────────
    def path(self, sym: str) -> pathlib.Path:
        return self.root / "sym" / (escape(sym) + ".json")

    def load_raw(self, sym: str) -> Optional[dict]:
        p = self.path(sym)
        if not p.exists():
            return None
        try:
            d = json.loads(p.read_text("utf-8"))
        except (ValueError, OSError):
            return None
        if not isinstance(d, dict) or d.get("v") != STORE_V or d.get("sym") != sym:
            return None
        return d

    def load(self, sym: str) -> Optional["M.Series"]:
        """The stored Series with its TER attached, or None (missing, corrupt, another version)."""
        d = self.load_raw(sym)
        if d is None or not d.get("dates"):
            return None
        s = M.Series(sym, d["currency"], [Date.fromisoformat(x) for x in d["dates"]], [float(c) for c in d["closes"]],
                     [(Date.fromisoformat(x), float(a)) for x, a in d.get("dividends", [])],
                     d.get("ter"), dict(d.get("meta") or {}),
                     [float(a) for a in d["adj"]] if d.get("adj") and len(d["adj"]) == len(d["dates"]) else None)
        s.meta["_stored"] = {"fetched": d.get("fetched"), "source": d.get("source"), "terSrc": d.get("terSrc"),
                             "terAt": d.get("terAt"), "splits": d.get("splits") or []}
        return s

    def load_all(self, symbols: Iterable[str]) -> Dict[str, "M.Series"]:
        out = {}
        for sym in symbols:
            s = self.load(sym)
            if s is not None:
                out[sym] = s
        return out

    def save(self, sym: str, fresh: "M.Series", source: str = "yahoo", fetched: Optional[str] = None,
             full: bool = False) -> Tuple[str, int]:
        """Merge a fetched Series in. Returns (how, bars added):
          new       no file yet: written
          appended  the overlap agrees: the bars after the stored last date added
          same      the overlap agrees and nothing is new
          mismatch  past closes differ and `full` is False: NOTHING written; fetch the whole history again
          replaced  `full` is True: the file replaced by this answer
          short     `full` is True but the answer starts more than FULL_SLACK_DAYS after the stored first
                    bar: NOTHING written (a truncated answer must not cost the history)"""
        fetched = fetched or _now_iso()
        old = self.load_raw(sym)
        how, added = "new", len(fresh.dates)
        dates, closes = [d.isoformat() for d in fresh.dates], list(fresh.closes)
        adj = list(fresh.adj) if fresh.adj and len(fresh.adj) == len(fresh.dates) else None
        divs = [[d.isoformat(), a] for d, a in fresh.dividends]
        splits = list(fresh.meta.get("splits") or [])
        ter, ter_src, ter_at = fresh.ter, None, None
        if old is not None and old.get("dates"):
            ter = old.get("ter") if fresh.ter is None else fresh.ter
            ter_src, ter_at = old.get("terSrc"), old.get("terAt")
            if full:
                limit = (dt.date.fromisoformat(old["dates"][0]) + dt.timedelta(days=FULL_SLACK_DAYS)).isoformat()
                if not dates or dates[0] > limit:
                    return "short", 0
                how, added = "replaced", len(dates) - len(old["dates"])
            elif self._overlap_agrees(old, fresh):
                last = old["dates"][-1]
                new_i = [i for i, d in enumerate(dates) if d > last]
                merged_dates = old["dates"] + [dates[i] for i in new_i]
                merged_closes = old["closes"] + [closes[i] for i in new_i]
                old_adj = old.get("adj") if old.get("adj") and len(old["adj"]) == len(old["dates"]) else None
                merged_adj = (old_adj + [adj[i] for i in new_i]) if (old_adj is not None and adj is not None) else None
                # the stored last bar may have been a "today" bar: take the fresh close for it
                if dates and last in dates:
                    merged_closes[len(old["dates"]) - 1] = closes[dates.index(last)]
                    if merged_adj is not None:
                        merged_adj[len(old["dates"]) - 1] = adj[dates.index(last)]
                dates, closes, adj = merged_dates, merged_closes, merged_adj
                divs = self._merge_events(old.get("dividends", []), divs)
                splits = self._merge_events(old.get("splits", []), splits)
                how, added = ("appended" if new_i else "same"), len(new_i)
            else:
                return "mismatch", 0
        doc = {"v": STORE_V, "sym": sym, "currency": fresh.currency, "dates": dates, "closes": closes, "adj": adj,
               "dividends": divs, "splits": splits, "meta": trim_meta(fresh.meta), "ter": ter, "terSrc": ter_src,
               "terAt": ter_at, "fetched": fetched, "source": source}
        self._write(self.path(sym), doc)
        e = self.entry(sym)
        e.update({"bars": len(dates), "first": dates[0] if dates else None, "last": dates[-1] if dates else None,
                  "fetched": fetched, "source": source, "currency": fresh.currency, "how": how, "adj": adj is not None})
        e.pop("err", None)
        if how == "replaced":
            e["replaced"] = fetched
        self.write_manifest()
        return how, added

    @staticmethod
    def _overlap_agrees(old: dict, fresh: "M.Series") -> bool:
        """The last OVERLAP_BARS stored closes against the fresh ones on the same dates. A stored
        bar the fresh answer lacks is not a disagreement (Yahoo drops holes); a differing close is."""
        by_date = {d.isoformat(): c for d, c in zip(fresh.dates, fresh.closes)}
        checked = 0
        for d, c in zip(old["dates"][-OVERLAP_BARS:], old["closes"][-OVERLAP_BARS:]):
            if d in by_date:
                if abs(by_date[d] - c) > 1e-6 * max(1.0, abs(c)):
                    return False
                checked += 1
        return checked > 0 or not fresh.dates or fresh.dates[0].isoformat() > old["dates"][-1]

    @staticmethod
    def _merge_events(old: list, new: list) -> list:
        seen = {tuple(x) for x in old}
        out = [list(x) for x in old]
        for x in new:
            if tuple(x) not in seen:
                out.append(list(x))
                seen.add(tuple(x))
        return sorted(out)

    def set_ter(self, sym: str, ter: Optional[float], src: str, at: Optional[str] = None) -> None:
        d = self.load_raw(sym)
        at = at or _now_iso()
        if d is not None:
            d.update({"ter": ter, "terSrc": src, "terAt": at})
            self._write(self.path(sym), d)
        self.entry(sym).update({"ter": ter, "terSrc": src, "terAt": at})
        self.write_manifest()

    def ter_age_days(self, sym: str, now: Optional[dt.datetime] = None) -> Optional[float]:
        at = self.entry(sym).get("terAt")
        if not at:
            return None
        now = now or dt.datetime.now(dt.timezone.utc)
        try:
            then = dt.datetime.strptime(at, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
        except ValueError:
            return None
        return (now - then).total_seconds() / 86400.0

    def mark_error(self, sym: str, err: str) -> None:
        self.entry(sym)["err"] = err[:80]
        self.entry(sym)["errAt"] = _now_iso()
        self.write_manifest()

    def forget(self, sym: str) -> None:
        self.path(sym).unlink(missing_ok=True)
        self.manifest["symbols"].pop(sym, None)
        self.write_manifest()

    # ── side tables ──────────────────────────────────────────────────────────
    def load_table(self, name: str) -> Optional[dict]:
        p = self.root / (name + ".json")
        if not p.exists():
            return None
        try:
            d = json.loads(p.read_text("utf-8"))
        except (ValueError, OSError):
            return None
        return d if isinstance(d, dict) else None

    def save_table(self, name: str, data: dict) -> None:
        self._write(self.root / (name + ".json"), data)

    # ── the status payload's symbols block ───────────────────────────────────
    def status_symbols(self, symbols: Iterable[str]) -> Dict[str, dict]:
        out = {}
        for sym in symbols:
            e = self.manifest["symbols"].get(sym) or {}
            row = {"asof": e.get("last"), "bars": e.get("bars", 0)}
            if e.get("ter") is not None:
                row["ter"] = e["ter"]
                row["terSrc"] = e.get("terSrc")
            if e.get("err"):
                row["err"] = e["err"]
            out[sym] = row
        return out

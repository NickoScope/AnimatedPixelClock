#!/usr/bin/env python3
"""Fetch the saved answers the previews use, one request at a time.

Yahoo, the chart v8 URL of the saved samples (docs/18-stock-dashboard.md, "Data source"):
period1 = 2000-01-01, interval = 1mo, events = div,split, saved as yahoo_<symbol>_1mo.json
with ^, = and the dot dropped from the name (yahoo_SP500TR_1mo.json). A symbol that already
has a sample is skipped, an answer without bars is not saved, requests are PAUSE_S apart,
and 429 stops the run. A 429 is not only a rate limit: the User-Agent is part of the answer
(see UA). docs/18 paces history at one symbol every 10 s or slower.

ECB, the Data Portal's SDMX REST API: market_ref.ECB_SERIES from ECB_START to ECB_END as
CSV, saved as market_ref.ECB_SAMPLE; refused unless every row is that series.

  python3 tools/market/fetch_samples.py              # every missing fund of PREVIEW_PORTFOLIO
  python3 tools/market/fetch_samples.py ^SP500TR     # just these
  python3 tools/market/fetch_samples.py --ecb        # the ECB table
"""
import datetime as dt
import json
import pathlib
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import market_ref as M  # noqa: E402

PERIOD1 = int(dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc).timestamp())
# The User-Agent decides Yahoo's answer. Measured by the coordinator on 2026-09-15 12:20: Python's default
# and a full Safari 17 string get 429; this string gets 200, with or without an Accept header.
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36"
PAUSE_S = 10
ECB_URL = "https://data-api.ecb.europa.eu/service/data/{flow}/{key}?format=csvdata&startPeriod={start}&endPeriod={end}"


def sample_name(sym: str) -> str:
    return "yahoo_" + "".join(ch for ch in sym if ch not in "^=.") + "_1mo.json"


def fetch(sym: str) -> pathlib.Path:
    out = M.SAMPLES / sample_name(sym)
    if out.exists():
        print(f"{sym}: have {out.name}")
        return out
    q = urllib.parse.urlencode({"period1": PERIOD1, "period2": int(time.time()), "interval": "1mo", "events": "div,split"})
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{urllib.parse.quote(sym)}?{q}"
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
    d = json.loads(raw)
    res = d["chart"]["result"][0]
    if not res.get("timestamp"):
        raise ValueError(f"{sym}: an answer without bars, not saved")
    out.write_bytes(raw)
    first = dt.datetime.fromtimestamp(res["timestamp"][0], dt.timezone.utc).date()
    print(f"{sym}: {url}\n  {len(res['timestamp'])} bars from {first}, {len(res.get('events', {}).get('dividends', {}))} dividends, "
          f"{res['meta']['currency']}, {len(raw)} B -> {out.name}")
    return out


def fetch_ecb() -> pathlib.Path:
    out = M.SAMPLES / M.ECB_SAMPLE
    if out.exists():
        print(f"ECB: have {out.name}")
        return out
    flow, key = M.ECB_SERIES.split(".", 1)
    url = ECB_URL.format(flow=flow, key=key, start=M.ECB_START, end=M.ECB_END)
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "text/csv"})
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read()
    tmp = out.with_suffix(".part")
    tmp.write_bytes(raw)
    try:
        rates = M.load_ecb_monthly(tmp)                # refuses another series
    except (ValueError, KeyError):
        tmp.unlink()
        raise
    if not rates:
        tmp.unlink()
        raise ValueError("ECB: an answer without observations, not saved")
    tmp.rename(out)
    first, last = min(rates), max(rates)
    print(f"ECB: {url}\n  {len(rates)} months {first[0]}-{first[1]:02d}..{last[0]}-{last[1]:02d}, {len(raw)} B -> {out.name}")
    return out


def main(argv):
    if argv == ["--ecb"]:
        fetch_ecb()
        return 0
    syms = argv or [s for s, _ in M.PREVIEW_PORTFOLIO if not M.sample_file(s)]
    for i, sym in enumerate(syms):
        try:
            fetch(sym)
        except urllib.error.HTTPError as e:
            print(f"{sym}: HTTP {e.code}; stopping" if e.code == 429 else f"{sym}: HTTP {e.code}")
            if e.code == 429:
                return 1
        except (urllib.error.URLError, ValueError, KeyError) as e:
            print(f"{sym}: {e}")
        if i + 1 < len(syms):
            time.sleep(PAUSE_S)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

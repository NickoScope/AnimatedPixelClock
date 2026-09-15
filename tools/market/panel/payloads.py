"""The market payloads as the panel's decoder receives them, from the numbers the
previews were drawn from.

docs/18-stock-dashboard.md names the topics and fields; tools/market/market_ref.py
builds the index, ticker, portfolio and holdings payloads (the same functions
render.py draws from), and this file adds the ones it lacks - live, tape,
intraday, status - and turns each preview frame's context into a spec for
market_host_test's `layout` mode: the watch lists, the payloads by topic, the
view (preset, page, mode, the date, the tape's phase).

pack_pts() below is the packing docs/18 states, written again on purpose: the
HA app's encoder is another helper's; check_market_panel.py holds this copy
against market_ref.pack_pts on the same points.

  python3 tools/market/panel/payloads.py     # print every frame's spec size and the largest payload
"""
import base64
import json
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
MARKET = HERE.parent
sys.path.insert(0, str(MARKET))
import market_ref as M  # noqa: E402
import render as R      # noqa: E402

PAYLOAD_MAX = 1900   # the bus buffer less header and topic (market_model.h kPayloadMax)


def pack_pts(points, lo=None, hi=None):
    """128 uint16 values scaled between lo and hi, little-endian, base64 (docs/18).
    A flat line sits mid-scale."""
    lo = min(points) if lo is None else lo
    hi = max(points) if hi is None else hi
    if hi > lo:
        u = [int(round((v - lo) / (hi - lo) * 65535)) for v in points]
    else:
        u = [32767] * len(points)
    return lo, hi, base64.b64encode(struct.pack("<%dH" % len(u), *u)).decode()


def pack_u16(u16):
    return base64.b64encode(struct.pack("<%dH" % len(u16), *u16)).decode()


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), default=str)


def payload_bytes(obj):
    return len(compact(obj).encode())


def local_seconds(now):
    return now.hour * 3600 + now.minute * 60 + now.second


def tape_payload(tokens, ts):
    """`tokens` = [(exchange, state or None)]; None everywhere means the app has sent nothing."""
    if all(s is None for _, s in tokens):
        return None
    return {"v": 1, "ts": ts, "x": [{"n": n, "s": s or "CLOSED", "t": ""} for n, s in tokens]}


def live_payload(entries, ts, lag_s=None):
    """`entries` = [(quote dict from market_ref.quote, state)]; `lag_s` the quote's delay behind the clock."""
    q = {}
    for quote, state in entries:
        q[quote["sym"]] = {"last": quote["last"], "prev": quote["prev"], "day": quote["dayChg"], "state": state,
                           "asof": quote["date"].isoformat()}
        if lag_s is not None:
            q[quote["sym"]]["delay_s"] = lag_s
    return {"v": 1, "ts": ts, "q": q}


COLORS = {"green_red": 0, "blue_red": 1, "red_up": 2}   # market_layout.h Colors


def iso(d):
    return d.isoformat() if d else None


def intraday_payload(sym, session, ts):
    u16, n_valid = session
    return {"v": 1, "sym": sym, "ts": ts, "n": n_valid, "min": 0.0, "max": 1.0, "pts": pack_u16(u16)}


def spec_for(page_fn, ctx):
    """One preview frame's context (render.py build_frames) as a layout spec."""
    now = ctx["now"]
    ts = int(now.timestamp())
    view = {"preset": ctx["preset"], "today": now.date().isoformat(), "tapePhase": local_seconds(now) * R.MK_TAPE_FPS,
            "tapeNames": [n for n, _ in ctx["tape"]], "mode": ctx.get("mode", "hold"),
            "colors": COLORS[ctx.get("colors", "green_red")], "feedBadge": True}
    watch = {"indices": [], "tickers": []}
    payloads = []
    error = bool(ctx.get("error"))
    tape = None if error else tape_payload(ctx["tape"], ts)
    if tape:
        payloads.append({"topic": "tape", "json": tape})

    if page_fn is R.page_markets:
        view["page"] = "markets"
        live = []
        for idx in ctx["indices"]:
            p = idx["p"]
            watch["indices"].append({"sym": p["sym"], "name": idx["mn"]})
            if not error:
                payloads.append({"topic": f"index/{p['sym']}/{p['preset']}", "json": p})
                live.append((idx["q"], idx["state"]))
        if live:
            payloads.append({"topic": "live", "json": live_payload(live, ts, ctx.get("lag_s"))})
    elif page_fn is R.page_ticker:
        view["page"] = "ticker"
        view["tickerSym"] = ctx["sym"]
        watch["tickers"].append({"sym": ctx["sym"], "name": ctx["sym"]})
        if not error:
            p = ctx["p"]
            payloads.append({"topic": f"ticker/{p['sym']}/{p['preset']}", "json": dict(p, ann=ctx.get("ann"))})
            payloads.append({"topic": "live", "json": live_payload([(ctx["q"], ctx["state"])], ts, ctx.get("lag_s"))})
            if ctx.get("session"):
                payloads.append({"topic": f"intraday/{ctx['sym']}", "json": intraday_payload(ctx["sym"], ctx["session"], ts)})
    elif page_fn is R.page_portfolio:
        view["page"] = "portfolio"
        view["currency"] = ctx["cur"]
        view["showPx"] = ctx.get("show_px", True)
        view["showBench"] = ctx.get("show_bench", True)
        view["mddStop"] = ctx.get("stop") == "mdd"
        if not error:
            p, r = ctx["p"], ctx["r"]
            dd = r["drawdown"]
            # The figures the app adds (docs/18, 11:43), here from preview_returns.py as render.py draws them.
            j = dict(p, twr=r["twr"], ann=r["ann"], xirr_ann=r["xirr"], flows=r["xirr"] is not None,
                     mdd_peak=iso(dd["peak"]), mdd_trough=iso(dd["trough"]), mdd_recovery=iso(dd["recovery"]))
            payloads.append({"topic": f"portfolio/{p['mode']}/{p['preset']}", "json": j})
    elif page_fn is R.page_holdings:
        view["page"] = "holdings"
        view["hdPage"] = ctx.get("page", 0)
        if not error:
            payloads.append({"topic": f"holdings/{ctx['h']['mode']}", "json": ctx["h"]})
    else:
        raise ValueError("unknown page")
    return {"watch": watch, "view": view, "payloads": payloads}


def all_specs():
    """{frame name: (title, page name, spec)} for every preview frame."""
    frames, _ = R.build_frames()
    return {name: (title, page_fn.__name__[5:], spec_for(page_fn, ctx)) for name, title, page_fn, ctx in frames}


if __name__ == "__main__":
    biggest = (0, "")
    for name, (title, page, spec) in all_specs().items():
        sizes = [(payload_bytes(p["json"]), p["topic"]) for p in spec["payloads"]]
        top = max(sizes) if sizes else (0, "-")
        biggest = max(biggest, top)
        print(f"{name:28} {page:9} {len(spec['payloads']):2} payloads, largest {top[0]:5} B  {top[1]}")
    print(f"largest payload {biggest[0]} B ({biggest[1]}), limit {PAYLOAD_MAX}")

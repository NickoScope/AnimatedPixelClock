#!/usr/bin/env python3
"""Host checks for the market dashboard's panel side.

1. market_host_test against src/market/: decoders and refusals, number rules,
   dates, base64 lines, ingest by topic, the watch lists, the LittleFS record,
   the four pages through the recording canvas, the settings on the neutral
   defaults, the knob's settle (four detents, one publish), the page switches
   after an update; and a second build on the example local defaults header.
2. The number formatting of the firmware against render.py's fmt_* functions on
   the same values: every number the previews draw, and the edge cases.
3. payloads.pack_pts (the encoder written from docs/18) against market_ref.pack_pts.
4. Every preview frame through the panel's ingest and layout: the strings the
   firmware draws against tools/market/preview/frames.json (the frames that
   use payload data must match), and the raster against the 1:1 PNGs.
5. The layout budget: render.py's MK_* and LBL_* against market_layout.h, and
   render.layout_budget() run again with the firmware's constants, glyph
   widths and number formatters, against frames.json's _meta budget.
6. Every generated payload under the bus limit.
7. The portal's script parses (JavaScriptCore's jsc, when this is a Mac).

  python3 tools/market/panel/check_market_panel.py [--keep DIR]   # keep the specs and rasters
"""
import collections
import json
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import payloads    # noqa: E402
import probe       # noqa: E402
import render as R  # noqa: E402
import market_ref as M  # noqa: E402

JSC = pathlib.Path("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc")
PREVIEW = HERE.parent / "preview"
# Every frame must be reproduced string for string and pixel for pixel, except
# these, with why.
KNOWN_DIFF = {"ticker_err": "the currency in the heading: with nothing stored the firmware cannot know it"}


def step(title):
    print(title)


def fmt_lines(specs):
    """(kind, args, expected) for every number the frames draw, plus edge cases."""
    lines = []

    def add(kind, expected, *args):
        lines.append((kind, " ".join(str(a) for a in args), expected))

    for _, (_, page, spec) in specs.items():
        for p in spec["payloads"]:
            j = p["json"]
            if p["topic"].startswith(("index/", "ticker/")):
                add("value", R.fmt_value(j["last"], 6), j["last"], 6)
                add("value", R.fmt_value(j["last"], 7), j["last"], 7)
                add("pct", R.fmt_pct(j["chg"]), j["chg"])
                add("amount", R.fmt_amount(j["hi"], 7), j["hi"], 7)
                add("amount", R.fmt_amount(j["lo"], 7), j["lo"], 7)
                add("date", R.fmt_date(M.Date.fromisoformat(j["to"])), j["to"])
            elif p["topic"].startswith("portfolio/"):
                add("value", R.fmt_value(j["value"], 7), j["value"], 7)
                add("pct", R.fmt_pct(j["chg"]), j["chg"])
                add("pct", R.fmt_pct(j["sinceStart"]), j["sinceStart"])
                add("pct", R.fmt_pct(j["mdd"]), j["mdd"])
                if j["cagr"] is not None:
                    add("pct", R.fmt_pct(j["cagr"]), j["cagr"])
                add("amount", R.fmt_amount(j["div"]), j["div"], 4)
                add("amount", R.fmt_amount(j["terDrag"]), j["terDrag"], 4)
                add("share", R.fmt_share(j["cash"]), j["cash"])
                for k in ("twr", "ann", "xirr_ann"):
                    if j.get(k) is not None:
                        add("pct", R.fmt_pct(j[k]), j[k])
                for k in ("mdd_peak", "mdd_trough", "mdd_recovery"):
                    if j.get(k):
                        add("ym", R.fmt_ym(M.Date.fromisoformat(j[k])), j[k])
            elif p["topic"].startswith("holdings/"):
                for r in j["rows"]:
                    if r["ret"] is not None:
                        add("pct", R.fmt_pct(r["ret"]), r["ret"])
            elif p["topic"] == "live":
                for q in j["q"].values():
                    if q["day"] is not None:
                        add("pct", R.fmt_pct(q["day"]), q["day"])
    for v in (0, 0.5, 2.5, 3.5, 12.45, 99.95, 99.96, 100, 999.94, 999.95, 1000, 1234.5, 26186.4, 99999.5, 100000, 999999,
              1e6, 1234567, 1e9, -0.5, -130900.4, -999999, 5.55, 0.15, 0.25, 0.35, 676.5, 1.05,
              9949, 9950, 10000, 12345, 21663.12, 99499, 99500, 999499, 999500, 9949999, 9950000, 99499999, -21663, -999499):
        for slot in (4, 6, 7):
            add("value", R.fmt_value(v, slot), v, slot)
            add("amount", R.fmt_amount(v, slot), v, slot)
    for x in (0, 0.0004, -0.0004, 0.0005, 0.0015, 0.0025, 0.0045, 0.124, -0.0048, 0.9995, 0.9996, 1.0, 4.58, 12.45, 19.7, -0.567,
              0.99949, 0.001, -0.001, 0.05, 0.0949, 0.0951):
        add("pct", R.fmt_pct(x), x)
    add("pct", R.fmt_pct(None), "none")
    for x in (0, 0.0055, 0.0095, 0.0999, 0.1, 0.54, 0.999, 1.0, 0.00049):
        add("share", R.fmt_share(x), x)
    return lines


def notes_from(dump):
    """frames.json's fields, assembled from the layout dump's items by role."""
    by = collections.defaultdict(list)
    for it in dump["items"]:
        by[it["role"]].append(it)
    out = {}
    out["heading"] = " ".join(it["text"] for role in ("head.title", "head.tag", "head.cur") for it in by.get(role, []))
    if by.get("asof"):
        out["asof"] = by["asof"][0]["text"]
    if by.get("err"):
        out["error"] = by["err"][0]["text"] + " " + by["err.why"][0]["text"]
    if "tape" in dump["notes"]:
        out["tape"] = dump["notes"]["tape"]

    def rows(prefix):
        groups = collections.defaultdict(list)
        for role, items in by.items():
            if role.startswith(prefix):
                for it in items:
                    key = it["y"] - (R.MK_PICO_DROP if it["font"] == "pico" else 0)
                    groups[key].append(it)
        return [" ".join(it["text"] for it in sorted(items, key=lambda i: i["x"])) for _, items in sorted(groups.items())]

    label = "".join(it["text"] for it in by.get("live", []))
    if by.get("pri.mn"):
        out["primary"] = {"mn": by["pri.mn"][0]["text"], "last": by["pri.last"][0]["text"], "chg": by["pri.chg"][0]["text"], "label": label}
        out["rows"] = rows("row.")
    if by.get("big"):
        out["last"] = out["value"] = by["big"][0]["text"]
        out["chg"] = by["chg"][0]["text"]
        if label:
            out["label"] = label
        out["footer"] = "".join(it["text"] for it in by.get("foot", []))   # coloured pieces in draw order
        out["ann"] = "".join(it["text"] for it in by.get("ann", [])) or "-"
        if by.get("twr"):
            out["twr"] = by["twr"][0]["text"] + " " + by["chg"][0]["text"]
        if out["heading"].startswith("PORTFOLIO"):
            out["xirr"] = dump["notes"].get("xirr", "-")
        if "marks" in dump["notes"]:
            i0, i1, xa, xb = (int(x) for x in dump["notes"]["marks"].split(","))
            out["marks"] = {"columns": [xa, xb], "points": [i0, i1]}
    if by.get("hd.sym"):
        out["rows"] = rows("hd.")
        m = re.search(r"HOLDINGS (\d+/\d+)", out["heading"])
        out["page"] = m[1] if m else ""
    return out


def compare_notes(name, expected, got):
    """Which of frames.json's fields differ; the fields that do not apply are skipped."""
    fields = ["heading", "asof", "error", "tape", "rows", "page", "footer", "label", "chg", "twr", "ann", "xirr", "marks"]
    if "primary" in expected:
        fields.append("primary")
    fields.append("value" if "value" in expected else "last")
    bad = []
    for f in fields:
        if f not in expected:
            if f in got and f in ("asof", "error"):
                bad.append(f"{f}: drawn {got[f]!r}, not in the preview")
            continue
        e, g = expected[f], got.get(f)
        if f == "primary":   # render.py notes the window and the state too; the page draws these four
            e = {k: e[k] for k in ("mn", "last", "chg", "label")}
        if e != g:
            bad.append(f"{f}: preview {e!r}, firmware {g!r}")
    return bad


class Measure:
    """The firmware's glyph widths and number formatters, one query a line (market_host_test measure)."""

    def __init__(self):
        self.proc = subprocess.Popen([str(probe.build()), "measure"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.cache = {}

    def ask(self, q):
        if q not in self.cache:
            self.proc.stdin.write(q + "\n")
            self.proc.stdin.flush()
            self.cache[q] = self.proc.stdout.readline().rstrip("\n")
        return self.cache[q]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait()


def firmware_constants():
    """MK_* from market_layout.h: integers, integer arrays, strings."""
    text = (ROOT / "src/market/market_layout.h").read_text()
    out = {}
    for line in text.splitlines():
        if not line.startswith("static const"):
            continue
        code = line.split("//")[0]
        for name, val in re.findall(r'(MK_\w+)\[\]\s*=\s*"([^"]*)"', code):
            out[name] = val
        for name, vals in re.findall(r"(MK_\w+)\[\d*\]\s*=\s*\{([^}]*)\}", code):
            out[name] = [int(v) for v in vals.split(",")]
        for name, val in re.findall(r"(MK_\w+)\s*=\s*(-?\d+)\b", code):
            out[name] = int(val)
    return out


def budget_check(expected):
    """render.py's constants against the firmware's, then render.layout_budget() with the firmware's
    constants, Picopixel and 5x7 widths and fmt_value / fmt_amount. Returns (problems, budget, unused)."""
    fw = firmware_constants()
    problems, unused, patched = [], [], {}
    for name in sorted(n for n in dir(R) if n.startswith(("MK_", "LBL_"))):
        rv = getattr(R, name)
        fname = name if name.startswith("MK_") else "MK_" + name
        if fname not in fw:
            unused.append(name)
            continue
        fv = fw[fname]
        if (list(rv) if isinstance(rv, tuple) else rv) != fv:
            problems.append(f"{name}: render.py {rv!r}, market_layout.h {fv!r}")
        patched[name] = tuple(fv) if isinstance(rv, tuple) else fv
    meas = Measure()
    fns = {"wp": lambda s: int(meas.ask("p " + s)),
           "w5": lambda s, k=1, thin=False: int(meas.ask(f"5 {k} {1 if thin else 0} {s}")),
           "fmt_value": lambda v, slot: meas.ask(f"v {v!r} {slot}"),
           "fmt_amount": lambda v, slot=4: meas.ask(f"a {v!r} {slot}")}
    saved = {k: getattr(R, k) for k in list(patched) + list(fns)}
    try:
        for k, v in list(patched.items()) + list(fns.items()):
            setattr(R, k, v)
        try:
            got = R.layout_budget()
        except AssertionError as e:
            return problems + [f"layout_budget() with the firmware refused an overlap: {e}"], {}, unused
    finally:
        for k, v in saved.items():
            setattr(R, k, v)
        meas.close()
    if got != expected:
        problems.append("the budget with the firmware differs from frames.json's _meta budget")
    problems += [f"{k}: {v} blank columns" for k, v in got.items() if v < 1]
    return problems, got, unused


def pixel_diff(ppm_path, png_path):
    from PIL import Image
    a = Image.open(ppm_path).convert("RGB")
    b = Image.open(png_path).convert("RGB")
    if a.size != b.size:
        return 128 * 64
    pa, pb = a.load(), b.load()
    return sum(1 for y in range(64) for x in range(128) if pa[x, y] != pb[x, y])


def main():
    keep = None
    if "--keep" in sys.argv:
        keep = pathlib.Path(sys.argv[sys.argv.index("--keep") + 1])
        keep.mkdir(parents=True, exist_ok=True)
    bad = 0

    step("1. market model, layout, record, settings, knob settle, page switches")
    code, out, err = probe.run()
    print("\n".join("  " + line for line in (out + err).strip().splitlines()))
    bad += code != 0
    code, out, err = probe.run("local", local=True)
    print("\n".join("  " + line for line in (out + err).strip().splitlines()))
    bad += code != 0

    specs = payloads.all_specs()

    step("2. number formatting against render.py")
    lines = fmt_lines(specs)
    code, out, err = probe.run("fmt", stdin="".join(f"{k} {a}\n" for k, a, _ in lines))
    got = out.split("\n")
    wrong = [(k, a, e, g) for (k, a, e), g in zip(lines, got) if e != g]
    for k, a, e, g in wrong[:12]:
        print(f"  {k} {a}: render.py {e!r}, firmware {g!r}")
    print(f"  {len(lines)} values, {len(wrong)} differ")
    bad += bool(wrong) or code != 0

    step("3. pack_pts against market_ref.pack_pts")
    same = True
    for pts in ([1.0] * 128, [float(i) for i in range(128)], [7619.98 - i * 3.3 for i in range(128)], [-5.0, 2.0] * 64):
        same = same and payloads.pack_pts(pts) == M.pack_pts(pts)
    print(f"  {'identical' if same else 'DIFFER'}")
    bad += not same

    step("4. every preview frame through ingest and the layout")
    expected_all = json.loads((PREVIEW / "frames.json").read_text())
    counts = {"frames": 0, "match": 0}
    missing = sorted(set(expected_all) - {"_meta"} - set(specs))
    if missing:
        print(f"  frames.json lists frames render.py did not build: {missing}")
        bad += 1
    with tempfile.TemporaryDirectory() as tmp:
        outdir = keep or pathlib.Path(tmp)
        for name, (title, page, spec) in specs.items():
            spec_path = outdir / f"{name}.spec.json"
            ppm_path = outdir / f"{name}.ppm"
            spec_path.write_text(json.dumps(spec, default=str))
            code, out, err = probe.run("layout", str(spec_path), str(ppm_path))
            if code != 0:
                print(f"  {name:28} layout FAILED: {err.strip()}")
                bad += 1
                continue
            dump = json.loads(out)
            got = notes_from(dump)
            problems = compare_notes(name, expected_all[name], got)
            if dump["refused"]:
                problems.append("refused: " + "; ".join(dump["refused"]))
            px = pixel_diff(ppm_path, PREVIEW / f"{name}_128x64.png")
            must = name not in KNOWN_DIFF
            ok = not problems and (px == 0 or not must)
            verdict = "ok" if ok else ("UNEXPECTED" if must else "differs")
            print(f"  {name:31} {'strings match' if not problems else str(len(problems)) + ' differ':14} {px:4} px differ  {verdict}"
                  + ("" if must else f"  (known: {KNOWN_DIFF[name]})"))
            counts["frames"] += 1
            counts["match"] += not problems and px == 0
            for p in problems:
                print(f"      {p}")
            if must:
                bad += not ok

        print(f"  {counts['match']} of {counts['frames']} frames identical in strings and pixels")

    step("5. layout budget, with the firmware's constants, metrics and formatters")
    problems, got, missing_fw = budget_check(expected_all["_meta"].get("budget", {}))
    for name, v in sorted(got.items()):
        e = expected_all["_meta"]["budget"].get(name)
        print(f"  {v:3} blank columns (render.py {e})  {name}")
    if missing_fw:
        print(f"  render.py constants the firmware does not use: {', '.join(missing_fw)}")
    for p_ in problems:
        print(f"  {p_}")
    bad += bool(problems)

    step("6. payload sizes")
    biggest = max((payloads.payload_bytes(p["json"]), p["topic"], name) for name, (_, _, spec) in specs.items() for p in spec["payloads"])
    print(f"  largest {biggest[0]} B: {biggest[1]} in {biggest[2]}, limit {payloads.PAYLOAD_MAX}")
    bad += biggest[0] > payloads.PAYLOAD_MAX

    step("7. portal script")
    src = (ROOT / "src/web/web_panel_js.h").read_text()
    m = re.search(r'R"(\w*)\((.*)\)\1"', src, re.S)
    if not JSC.exists():
        print("  jsc not found: not checked")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            js = pathlib.Path(tmp, "panel.js")
            js.write_text(m[2])
            r = subprocess.run([str(JSC), "-e", 'try { new Function(read("%s")); print("parses"); } catch (e) { print("FAILS " + e); }' % js],
                               capture_output=True, text=True)
        print(f"  web_panel_js.h, {len(m[2]):,} B: {r.stdout.strip()}  ({'has' if 'api/market' in m[2] else 'NO'} market section)")
        bad += r.stdout.strip() != "parses"

    print(f"\n{'all passed' if not bad else str(bad) + ' FAILED'}")
    sys.exit(1 if bad else 0)


main()

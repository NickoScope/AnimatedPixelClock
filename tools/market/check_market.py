#!/usr/bin/env python3
"""Runs everything the market app can prove on a laptop: the reference's tests, the app's tests,
the payload sizes of the largest configuration, and a dry-run publish from the saved samples.

  python3 tools/market/check_market.py            # all of it; exit 0 when every check passes
  python3 tools/market/check_market.py --quick    # the two test suites only

Nothing here needs Home Assistant, a broker or the network.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APPD = HERE / "appdaemon"
PY = sys.executable


def run(title, cmd, cwd=None):
    print("== %s" % title, flush=True)
    r = subprocess.run(cmd, cwd=str(cwd or HERE), capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().splitlines()[-4:]
    for line in tail:
        print("   " + line)
    print("   -> %s" % ("ok" if r.returncode == 0 else "FAILED (exit %d)" % r.returncode))
    return r.returncode == 0, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    ok = True
    ok &= run("market_ref: the reference maths", [PY, "-m", "unittest", "test_market_ref"])[0]
    ok &= run("matrix_market: the app against the fakes", [PY, "test_matrix_market.py"], APPD)[0]
    if args.quick:
        return 0 if ok else 1
    tmp = tempfile.mkdtemp()
    cfg = pathlib.Path(tmp, "voo.json")
    cfg.write_text(json.dumps({"v": 2, "portfolio": {"positions": [{"sym": "VOO", "w": 100.0}]}}))
    common = [PY, "-m", "matrix_market.cli", "--offline", "--no-local", "--store", tmp, "--config", str(cfg)]
    ok &= run("fetch from the samples (offline)", common + ["fetch"], APPD)[0]
    good, out = run("compute: every payload's size from the samples", common + ["compute"], APPD)
    ok &= good
    good, out = run("publish --dry-run: the payloads printed, none reaches a broker", common + ["publish", "--dry-run"], APPD)
    ok &= good
    n = out.count("nickoscope_matrix/a1b2c3/market/")
    print("   %d retained market topics in the dry run" % n)
    ok &= n >= 80
    good, out = run("report: the golden figures on the samples", common + ["report"], APPD)
    ok &= good and "89678.85" in out
    store = HERE / "store"
    if (store / "manifest.json").exists():
        real = [PY, "-m", "matrix_market.cli", "--no-local", "--store", str(store)]
        good, out = run("report on the real store (tools/market/store)", real + ["report"], APPD)
        print("   (informational: depends on what the last fetch saved)")
    print("\nALL CHECKS PASSED" if ok else "\nSOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

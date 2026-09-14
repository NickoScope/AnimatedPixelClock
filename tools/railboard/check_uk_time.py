#!/usr/bin/env python3
"""Check src/railboard/uk_time.h against the tz database.

Compiles the header on the host with the system C++ compiler, feeds it UTC
instants, and compares every field with Python's zoneinfo for Europe/London.
The instants are every hour from 2000 to 2037, plus every minute for a day
either side of each change - the two moments the rule actually matters.

  python3 tools/railboard/check_uk_time.py
"""
import datetime as dt, pathlib, subprocess, sys, tempfile
from zoneinfo import ZoneInfo

ROOT = pathlib.Path(__file__).resolve().parents[2]
LONDON, UTC = ZoneInfo("Europe/London"), dt.timezone.utc

PROBE = r'''
#include <cstdio>
#include "uk_time.h"
int main() {
  long long t;
  while (std::scanf("%lld", &t) == 1) {
    const uktime::Civil c = uktime::london(t);
    std::printf("%lld %d %u %u %u %u %u %u %d\n", t, c.year, c.month, c.day,
                c.hour, c.minute, c.second, c.wday, c.bst ? 1 : 0);
  }
}
'''

def instants():
    start = int(dt.datetime(2000, 1, 1, tzinfo=UTC).timestamp())
    end = int(dt.datetime(2037, 12, 31, tzinfo=UTC).timestamp())
    seen = set(range(start, end, 3600))
    for year in range(2000, 2038):
        for month in (3, 10):
            last = max(dt.date(year, month, d) for d in range(25, 32)
                       if dt.date(year, month, d).weekday() == 6)
            change = int(dt.datetime(last.year, last.month, last.day, 1, tzinfo=UTC).timestamp())
            seen.update(range(change - 86400, change + 86400, 60))
            seen.update((change - 1, change, change + 1))
    return sorted(seen)

def main():
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = pathlib.Path(tmp, "probe.cpp"), pathlib.Path(tmp, "probe")
        src.write_text(PROBE)
        subprocess.run(["c++", "-std=c++11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/railboard"), str(src), "-o", str(exe)], check=True)
        ts = instants()
        out = subprocess.run([str(exe)], input="\n".join(map(str, ts)),
                             capture_output=True, text=True, check=True).stdout.split("\n")
    bad = 0
    for line in filter(None, out):
        t, y, mo, d, h, mi, s, wd, bst = map(int, line.split())
        ref = dt.datetime.fromtimestamp(t, LONDON)
        want = (ref.year, ref.month, ref.day, ref.hour, ref.minute, ref.second,
                (ref.weekday() + 1) % 7, int(ref.dst() != dt.timedelta(0)))
        if (y, mo, d, h, mi, s, wd, bst) != want:
            bad += 1
            if bad <= 10:
                print(f"  {t}: got {(y, mo, d, h, mi, s, wd, bst)} want {want}")
    print(f"{len(ts)} instants, {bad} differ from zoneinfo Europe/London")
    sys.exit(1 if bad else 0)

main()

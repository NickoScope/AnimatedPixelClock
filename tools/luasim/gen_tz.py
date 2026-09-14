#!/usr/bin/env python3
"""Write the IANA zone -> POSIX TZ table the world clock looks cities up in.

A city found by the portal's search arrives with an IANA zone name
("America/Los_Angeles"); the panel needs a POSIX TZ string it can evaluate
(src/worldclock/posix_tz.cpp). This writes src/worldclock/tzdb.h: every zone
under the geographic areas of this machine's tz database, with its string.

Source: the IANA tz database compiled on this machine, /usr/share/zoneinfo.
Its LICENSE: "all files in the tz code and data (including this LICENSE file)
are in the public domain". Every TZif file of version 2 or later ends in "a
POSIX-TZ-environment-variable-style string for use in handling instants after
the last transition time stored in the file" (tzfile(5)), and that footer is
what this takes - the method nayarsystems/posix_tz_db uses, whose gen-tz.py
reads the same directory. Its own table (MIT per GitHub, though the LICENSE
file still carries the "[year] [fullname]" template) was last regenerated for
tzdata 2025b; seven of its 461 strings differ from 2026c, among them
America/Vancouver and America/Edmonton, which 2026c moves to permanent
UTC-7 and UTC-6 from November 2026.

A footer holds only after the zone's last stored transition, so each one is
checked against the zone itself over WINDOW. Python's zoneinfo reads both: the
real file, and the footer alone wrapped in a TZif with no transitions, so
every instant falls to it. They must agree at the window's ends and a second
either side of every change of either, which leaves nowhere for a difference
to hide. A zone that disagrees gets the fixed offset it keeps longest in the
window instead, and the header lists it as approximate.

  python3 tools/luasim/gen_tz.py           regenerate tzdb.h
  python3 tools/luasim/gen_tz.py --check   offline: the header is sorted and consistent
"""
import io, pathlib, re, struct, sys, zoneinfo
from datetime import datetime, timezone

HERE     = pathlib.Path(__file__).resolve().parent
HEADER   = HERE.parent.parent / "src/worldclock/tzdb.h"
ZONEINFO = pathlib.Path("/usr/share/zoneinfo")
AREAS    = ("Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic",
            "Australia", "Europe", "Indian", "Pacific")
# Two years from when this table was made: what the panel will meet before
# the next firmware, not history. Fixed, so a rerun on the same tzdata writes
# the same header.
WINDOW = (int(datetime(2026, 9, 1, tzinfo=timezone.utc).timestamp()),
          int(datetime(2028, 9, 1, tzinfo=timezone.utc).timestamp()))
DAY = 86400


def load(path=HEADER):
    """{iana name: posix string} from the generated header, for gen_world.py and fx_parity.py."""
    text = path.read_text()
    def blob(name):
        body = re.search(r"static const char %s\[\] =\n(.*?);\n" % name, text, re.S).group(1)
        return re.findall(r'^\s*"(.*)\\0"$', body, re.M)
    names, posix = blob("kTzNames"), blob("kTzPosix")
    def offs(items):
        out, n = {}, 0
        for item in items:
            out[n], n = item, n + len(item) + 1
        return out
    noff, poff = offs(names), offs(posix)
    pairs = re.findall(r"\{(\d+), (\d+)\},", text.split("kTzIndex")[1])
    return {noff[int(a)]: poff[int(b)] for a, b in pairs}


def check():
    try:
        table = load()
        names = list(table)
        count = int(re.search(r"#define TZDB_COUNT (\d+)", HEADER.read_text()).group(1))
        problems = []
        if names != sorted(names): problems.append("names are not in strcmp order")
        if len(names) != count: problems.append(f"TZDB_COUNT {count} but {len(names)} entries")
    except Exception as e:                     # a hand edit that broke the layout
        problems = [f"cannot read {HEADER.name}: {e}"]
    for p in problems: print("gen_tz --check:", p)
    if not problems: print(f"gen_tz --check: {len(names)} zones, sorted")
    sys.exit(1 if problems else 0)


def footer_zone(s):
    """A ZoneInfo that is nothing but the POSIX string s."""
    block = b"TZif2" + bytes(15) + struct.pack(">6l", 0, 0, 0, 0, 1, 4) + struct.pack(">lBB", 0, 0, 0) + b"UTC\0"
    return zoneinfo.ZoneInfo.from_file(io.BytesIO(block + block + b"\n" + s.encode() + b"\n"), key=s)


def off(z, t):
    return int(datetime.fromtimestamp(t, timezone.utc).astimezone(z).utcoffset().total_seconds())


def changes(z, lo, hi):
    """Every second in (lo, hi) at which z's offset differs from the second before.
    Sampled daily and bisected; no zone changes twice within a day."""
    out, t, o = [], lo, off(z, lo)
    while t < hi:
        n = min(t + DAY, hi)
        on = off(z, n)
        if on != o:
            a, b = t, n
            while b - a > 1:
                m = (a + b) // 2
                if off(z, m) == o: a = m
                else: b = m
            out.append(b)
        t, o = n, on
    return out


def fixed(secs):
    sign, a = ("+" if secs >= 0 else "-"), abs(secs)
    h, m = a // 3600, a % 3600 // 60
    west = ("-" if secs > 0 else "") + (f"{h}:{m:02d}" if m else f"{h}")
    return (f"<{sign}{h:02d}{m:02d}>" if m else f"<{sign}{h:02d}>") + west


def main():
    version = (ZONEINFO / "+VERSION").read_text().strip()
    lo, hi = WINDOW
    table, approx = {}, []
    for area in AREAS:
        for path in sorted((ZONEINFO / area).rglob("*")):
            data = path.read_bytes() if path.is_file() else b""
            if data[:4] != b"TZif":
                continue
            name = str(path.relative_to(ZONEINFO))
            real = zoneinfo.ZoneInfo.from_file(io.BytesIO(data), key=name)
            foot = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode() if data[4:5] >= b"2" else ""
            ok = False
            if foot:
                fz = footer_zone(foot)
                pts = {lo, hi - 1}
                for c in changes(real, lo, hi) + changes(fz, lo, hi):
                    pts.update((c - 1, c))
                ok = all(off(real, t) == off(fz, t) for t in pts)
            if ok:
                table[name] = foot
                continue
            # The offset held longest in the window, from the real zone's own changes.
            edges = [lo] + changes(real, lo, hi) + [hi]
            held = {}
            for a, b in zip(edges, edges[1:]):
                held[off(real, a)] = held.get(off(real, a), 0) + b - a
            best = max(held, key=held.get)
            table[name] = fixed(best)
            others = ", ".join(f"{o / 3600:+g} h for {d // DAY} d" for o, d in held.items() if o != best)
            approx.append(f"{name:24s} {table[name]:10s} footer {foot or '(none)'!r}; also {others or 'nothing'}")

    names = sorted(table)                      # Python orders ASCII str as strcmp does
    posix = sorted(set(table.values()))
    noff, poff, n = {}, {}, 0
    for s in names: noff[s], n = n, n + len(s) + 1
    n = 0
    for s in posix: poff[s], n = n, n + len(s) + 1
    size_names = sum(len(s) + 1 for s in names)
    size_posix = sum(len(s) + 1 for s in posix)
    lit = lambda items: "".join(f'  "{s}\\0"\n' for s in items)
    HEADER.write_text(f"""#pragma once
// GENERATED by tools/luasim/gen_tz.py from tzdata {version} - do not edit.
//
// IANA zone name -> POSIX TZ string, for the world clock's cities: {len(names)} zones
// under {', '.join(AREAS[:5])},
// {', '.join(AREAS[5:])}.
// Names in strcmp order for a binary search; each distinct string stored once.
// {size_names} + {size_posix} bytes of text, {len(names) * 4} of index.
//
// Source: the IANA tz database, public domain; each string is its zone's TZif
// footer, checked against the zone itself from 2026-09-01 to 2028-09-01 UTC.
""" + ("""//
// Approximate: no footer matches these zones in that window, so each has the
// fixed offset it keeps longest there.
""" + "".join(f"//   {a}\n" for a in approx) if approx else "") + f"""
#include <stdint.h>

#define TZDB_VERSION "{version}"
#define TZDB_COUNT {len(names)}

static const char kTzPosix[] =
{lit(posix)};

static const char kTzNames[] =
{lit(names)};

// {{name, posix}}: byte offsets into the two blobs above.
static const uint16_t kTzIndex[TZDB_COUNT][2] = {{
""" + "".join(f"  {{{noff[s]}, {poff[table[s]]}}},\n" for s in names) + "};\n")
    print(f"{HEADER.name}: tzdata {version}, {len(names)} zones, {len(posix)} strings, "
          f"{size_names + size_posix + len(names) * 4} bytes, {len(approx)} approximate")
    for a in approx: print("  approximate:", a)
    assert load() == table, "the header does not read back as written"


if __name__ == "__main__":
    check() if "--check" in sys.argv else main()

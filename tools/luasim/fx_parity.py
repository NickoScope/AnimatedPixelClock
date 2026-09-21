#!/usr/bin/env python3
"""Hold the firmware's Lua effect runtime to luasim, pixel for pixel.

Builds the reference (luasim) and fxhost, which is src/lua/lua_fx.cpp,
lua_px.cpp and nslua_sandbox.cpp - the files the panel runs - compiled for
this machine against a stand-in display. Renders every script in scripts/
through both with the same clock and compares the raw frames byte for byte.
Then measures each script: exact instruction counts for the load and for a
frame, host draw time, Lua heap peak, and the C stack the host thread used.

Two checks for the world clock, whose page is native C++ rather than Lua:
wchost - src/worldclock/worldclock.cpp and posix_tz.cpp against a stand-in
display - renders the page under the same clocks as world_clock.lua, for
built-in and custom homes, compared after the panel's RGB565 step; and
posix_tz.cpp evaluates every string in tzdb.h against Python's zoneinfo.

  python3 tools/luasim/fx_parity.py           # parity and measurements
  python3 tools/luasim/fx_parity.py --quick   # parity only

Needs one PlatformIO build first, for Adafruit GFX's gfxfont.h and the font
(gen_font.py has the same requirement). Exit status 1 on any difference.
"""
import glob, os, pathlib, re, shutil, subprocess, sys, tempfile, time
from datetime import datetime, timezone

ROOT  = pathlib.Path(__file__).resolve().parents[2]
SIM   = ROOT / "tools/luasim"
BUILD = SIM / ".fxbuild"
LUA   = ROOT / "src/lua/vendor/lua"
W, H  = 128, 64
FRAME = W * H * 3
sys.path.insert(0, str(SIM))
import gen_tz                                   # the zone table and its zoneinfo reference

# (label, start, frames, extra flags). Every script sees every case, including
# the ones it ignores: a clock-free script must still match at any clock.
CASES = [
    ("12:34, 240 frames",       "12:34", 240, []),
    ("23:59, hour wraps",       "23:59",  90, []),
    ("day sweep, 96 frames",    "00:00",  96, ["--sweep"]),
    ("06:10, yday 0, UTC-5",    "06:10",  60, ["--yday", "0", "--utc", "-5"]),
]

# The world clock page against its prototype, under every clock above:
# (label, "HOME, CHANGED_AT" for the script, a city appended to its CITIES,
# wchost flags). The page's ids are 0.. built in and 100 for the custom slot;
# the script counts from 1 and appends. The long name is 61 of the 72 px, and
# Sydney's summer time spans the new year, which the 06:10 yday 0 case lands in.
WC_SCRIPT = SIM / "scripts/world_clock.lua"
WC_HOMES = [
    ("Cannes, just changed",       "1, 0",   None, ["--home", "0"]),
    ("New York, settled",          "3, nil", None, ["--home", "2", "--settled"]),
    ("custom long name, added",    "7, 0",   ("SAINT PETERSBURG", 59.94, 30.31, "Europe/Moscow"), ["--home", "100"]),
    ("custom southern, settled",   "7, nil", ("SYDNEY", -33.87, 151.21, "Australia/Sydney"), ["--home", "100", "--settled"]),
]

# posix_tz.cpp beyond the table: forms POSIX allows that no zone uses today,
# checked against zoneinfo like the table's own, and strings it must refuse.
# The zero-based day form is checked against this machine's C library instead:
# Python 3.9's zoneinfo starts "59" on 28 February 2026, where POSIX (XBD 8.3,
# "the zero-based Julian day") and macOS libc both start it on 1 March.
TZ_EXTRA  = ["AAA-2BBB,J60,J300/1", "<+0530>-5:30", "AAA3BBB2,M10.1.0,M3.3.0/-23"]
TZ_LIBC   = ["AAA-2BBB,59,300", "AAA-2BBB,0/1,365/23"]
TZ_REFUSE = ["", "CET", "CET-1CEST", "AB-1", "<AB>-1", "CET-25", "CET-1CEST,M13.1.0,M10.5.0",
             "CET-1CEST,M3.6.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0", "CET-1CEST,M3.5.0/168,M10.5.0",
             "CET-1CEST,M3.5.0,M10.5.0/3x", "CET-1 ", "CET-1CEST,J0,J300", "CET-1CEST,366,300"]

# worldClockFitName on names an IP lookup may hand back, expected by hand from
# Picopixel's advances (L A F R P G Y 4, N 5, W 6, I 2): the Welsh one fills
# 72 px exactly at 17 letters, and has no word break to prefer.
FIT_CASES = [
    ("Zürich", "ZURICH"), ("São Paulo", "SAO PAULO"), ("Łódź", "LODZ"), ("Straße", "STRASSE"),
    ("Москва", ""), ("  le   Cannet ", "LE CANNET"), ("Ho Chi Minh City", "HO CHI MINH CITY"),
    ("Petropavlovsk-Kamchatsky", "PETROPAVLOVSK"), ("Llanfairpwllgwyngyll", "LLANFAIRPWLLGWYNG"),
]

# The bench on the panel (src/lua/nslua_bench.cpp, phase 6b): kCompute took
# 64.1 ms per nslua_run, of which 2.54 ms is the fresh sandboxed state.
BENCH_COMPUTE = "local x = 0 for i = 1, 50000 do x = x + i % 7 end\nfunction draw() end\n"
BENCH_MS = 64.1 - 2.54


# Kept in step with platformio.ini by hand; the parity run is what would catch
# them drifting apart, because a script that loads on one and not the other
# stops being identical.
CCALLS = 28


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build():
    sh(["make", "-s", "-C", str(SIM), "luasim"])
    gfx = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/gfxfont.h"))
    if not gfx:
        sys.exit("fx_parity: build the firmware once so Adafruit GFX is downloaded")
    gfx_dir = str(pathlib.Path(gfx[0]).parent)
    BUILD.mkdir(exist_ok=True)
    objs = []
    for c in sorted(LUA.glob("*.c")):
        if c.name in ("lua.c", "luac.c"):
            continue
        o = BUILD / (c.stem + ".o")
        if not o.exists() or o.stat().st_mtime < c.stat().st_mtime:
            # The panel lowers Lua's C-call cap so an uploaded script's parser
            # recursion fits the effect task's 12 KB stack (platformio.ini, and
            # the reasoning in src/lua/lua_store.h). The host has to compile the
            # interpreter the same way or this check tests a different Lua.
            sh(["cc", "-O2", f"-DLUAI_MAXCCALLS={CCALLS}", "-I", str(LUA), "-c", str(c), "-o", str(o)])
        objs.append(str(o))
    srcs = [ROOT / "src/lua/lua_fx.cpp", ROOT / "src/lua/lua_px.cpp",
            ROOT / "src/lua/nslua_sandbox.cpp", SIM / "fxhost.cpp"]
    sh(["c++", "-std=gnu++11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", str(ROOT / "src/lua"), "-I", str(SIM / "host"), "-I", gfx_dir,
        *map(str, srcs), *objs, "-o", str(SIM / "fxhost"), "-lm", "-lpthread"])
    # -ffp-contract=off: the Lua VM multiplies and adds as separate operations,
    # and a host compiler allowed to fuse a*b+c in C++ would round differently.
    sh(["c++", "-std=gnu++11", "-O2", "-Wall", "-Wextra", "-ffp-contract=off",
        "-DWORLDCLOCK_ENABLED", "-DCONTROL_ENCODER_ENABLED", "-DWORLDCLOCK_HOST",
        "-I", str(ROOT / "src/worldclock"), "-I", str(SIM / "host"), "-I", gfx_dir,
        str(ROOT / "src/worldclock/worldclock.cpp"), str(ROOT / "src/worldclock/posix_tz.cpp"),
        str(SIM / "wchost.cpp"), "-o", str(SIM / "wchost"), "-lm"])


def render(tool, script, out, start, frames, flags):
    r = subprocess.run([str(SIM / tool), str(script), str(frames), str(out), "--start", start, *flags],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{tool} {script.name}: {r.stderr.strip()}")
    return r.stderr


def stats(stderr):
    out = {}
    for line in stderr.splitlines():
        if line.startswith("fxhost:"):
            toks = line.split()[1:]
            if toks[0] == "stack_used":
                out["stack_used"] = int(toks[1])
                continue
            out["name"] = toks[0]
            for k, v in zip(toks[1::2], toks[2::2]):
                out[k] = float(v)
    return out


def compare(a, b):
    """(frames that differ, pixels that differ, largest channel difference)."""
    da, db = a.read_bytes(), b.read_bytes()
    if len(da) != len(db):
        return (-1, -1, -1)
    frames = pixels = worst = 0
    for f in range(len(da) // FRAME):
        fa, fb = da[f * FRAME:(f + 1) * FRAME], db[f * FRAME:(f + 1) * FRAME]
        if fa == fb:
            continue
        frames += 1
        for i in range(0, FRAME, 3):
            if fa[i:i + 3] != fb[i:i + 3]:
                pixels += 1
                worst = max(worst, *(abs(fa[i + k] - fb[i + k]) for k in range(3)))
    return (frames, pixels, worst)


# RGB888 as the panel shows it: packed to RGB565 and spread back again, the
# way MatrixPanel_I2S_DMA's color565 and color565to888 do.
T5 = bytes((v & 0xF8) | ((v & 0xF8) >> 5) for v in range(256))
T6 = bytes((v & 0xFC) | ((v & 0xFC) >> 6) for v in range(256))


def via565(path):
    raw = path.read_bytes()
    out = bytearray(raw)
    out[0::3], out[1::3], out[2::3] = raw[0::3].translate(T5), raw[1::3].translate(T6), raw[2::3].translate(T5)
    path.write_bytes(bytes(out))


def zoneinfo_ref(s):
    z = gen_tz.footer_zone(s)
    return lambda t: gen_tz.off(z, t)


def libc_ref(s):
    def off(t):
        os.environ["TZ"] = s
        time.tzset()
        return time.localtime(t).tm_gmtoff
    return off


def tz_offsets(strings, lo, hi, ref=zoneinfo_ref):
    """("POSIX\tutc" lines, the reference's offsets): every 6 h, and a second
    either side of each change, found daily and bisected."""
    lines, want = [], []
    for s in strings:
        off = ref(s)
        pts, t, o = set(range(lo, hi, 6 * 3600)), lo, off(lo)
        while t < hi:
            n = min(t + 86400, hi)
            on = off(n)
            if on != o:
                a, b = t, n
                while b - a > 1:
                    m = (a + b) // 2
                    a, b = (m, b) if off(m) == o else (a, m)
                pts.update((b - 1, b))
            t, o = n, on
        for t in sorted(pts):
            lines.append(f"{s}\t{t}")
            want.append(str(off(t)))
    return lines, want


def tz_run(lines):
    r = subprocess.run([str(SIM / "wchost"), "--tz"], input="\n".join(lines) + "\n",
                       capture_output=True, text=True, check=True)
    return r.stdout.split("\n")[:-1]


def tz_check():
    lo = int(datetime(2026, 1, 1, tzinfo=timezone.utc).timestamp())
    hi = int(datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp())
    # Negative control: one Sunday's difference in a rule must show.
    a, want = tz_offsets(["EST5EDT,M3.3.0,M11.1.0"], lo, hi)
    got = tz_run([l.replace("M3.3.0", "M3.2.0") for l in a])
    if got == want:
        sys.exit("fx_parity: posix_tz cannot see a week's difference in a rule - not trusting it")
    strings = sorted(set(gen_tz.load().values())) + TZ_EXTRA
    lines, want = tz_offsets(strings, lo, hi)
    saved = os.environ.get("TZ")
    more, wmore = tz_offsets(TZ_LIBC, lo, hi, libc_ref)
    if saved is None:
        os.environ.pop("TZ", None)
    else:
        os.environ["TZ"] = saved
    time.tzset()
    strings += TZ_LIBC
    lines += more
    want += wmore
    lines += [f"{s}\t0" for s in TZ_REFUSE]
    want  += ["reject"] * len(TZ_REFUSE)
    got = tz_run(lines)
    bad = [(l, w, g) for l, w, g in zip(lines, want, got) if w != g]
    if len(got) != len(lines):
        bad.append(("lines", len(lines), len(got)))
    print(f"posix_tz.cpp against zoneinfo, 2026-2029: {len(strings)} strings at {len(lines) - len(TZ_REFUSE)} instants,"
          f" {len(TZ_REFUSE)} refusals - {'all agree' if not bad else f'{len(bad)} differ'}")
    for x in bad[:10]:
        print("   ", x)
    return len(bad)


def fit_check():
    r = subprocess.run([str(SIM / "wchost"), "--fit"], input="".join(f"{a}\n" for a, _ in FIT_CASES),
                       capture_output=True, text=True, check=True)
    bad = 0
    for (name, want), line in zip(FIT_CASES, r.stdout.splitlines()):
        got, px = line.rsplit("|", 1)
        ok = got == want and int(px) <= 72
        bad += not ok
        if not ok:
            print(f"    fit {name!r}: got {got!r} ({px} px), want {want!r}")
    print(f"worldClockFitName: {len(FIT_CASES) - bad}/{len(FIT_CASES)} names as expected")
    return bad


def wc_script(tmp, home, extra, table):
    s, n = re.subn(r"^local HOME, CHANGED_AT = .*$", f"local HOME, CHANGED_AT = {home}", WC_SCRIPT.read_text(), flags=re.M)
    if n != 1:
        sys.exit("fx_parity: world_clock.lua no longer has its HOME line")
    if extra:
        name, lat, lon, iana = extra
        s = s.replace("-- END WORLD MASK\n", "-- END WORLD MASK\n"
                      f'CITIES[#CITIES + 1] = {{name = "{name}", lat = {lat}, lon = {lon}, tz = "{table[iana]}"}}\n', 1)
    path = tmp / "wc.lua"
    path.write_text(s)
    return path


def wc_parity(tmp):
    table = gen_tz.load()
    def page(home, extra, flags, start, frames, cflags, out):
        script = wc_script(tmp, home, extra, table)
        wflags = list(flags) + (["--custom", f"{extra[0]}|{extra[1]}|{extra[2]}|{table[extra[3]]}"] if extra else [])
        render("wchost", script, out, start, frames, cflags + wflags)
        return script
    # Negative control: Cannes and New York must not look alike.
    page("1, nil", None, ["--home", "0", "--settled"], "12:34", 30, [], tmp / "a.raw")
    page("3, nil", None, ["--home", "2", "--settled"], "12:34", 30, [], tmp / "b.raw")
    if compare(tmp / "a.raw", tmp / "b.raw")[0] <= 0:
        sys.exit("fx_parity: the world clock page looks the same for Cannes and New York - not trusting it")
    print("world clock page (wchost) against world_clock.lua, after RGB565")
    bad = 0
    for label, home, extra, flags in WC_HOMES:
        for clabel, start, frames, cflags in CASES:
            ref, got = tmp / "wref.raw", tmp / "wgot.raw"
            script = page(home, extra, flags, start, frames, cflags, got)
            render("luasim", script, ref, start, frames, cflags)
            via565(ref)
            fr, px, worst = compare(ref, got)
            bad += fr != 0
            print(f"  {label:26s} {clabel:24s} {'identical' if fr == 0 else f'{fr} frames, {px} px, max {worst}':>22s}")
    return bad


def main():
    quick = "--quick" in sys.argv
    build()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="fxparity"))
    bad = 0
    try:
        scripts = sorted((SIM / "scripts").glob("*.lua"))
        # Negative control: the same script one minute apart must differ. If it
        # does not, the comparison is blind and "identical" below means nothing.
        tetris = SIM / "scripts/tetris_clock.lua"
        render("luasim", tetris, tmp / "a.raw", "12:34", 30, [])
        render("fxhost", tetris, tmp / "b.raw", "12:35", 30, [])
        if compare(tmp / "a.raw", tmp / "b.raw")[0] <= 0:
            sys.exit("fx_parity: the comparison cannot see a one-minute difference - not trusting it")
        print("negative control: 12:34 against 12:35 differs, so the comparison can see a change")
        print("parity against luasim (frames differing / pixels differing / worst channel delta)")
        for s in scripts:
            for label, start, frames, flags in CASES:
                ref, got = tmp / "ref.raw", tmp / "got.raw"
                render("luasim", s, ref, start, frames, flags)
                render("fxhost", s, got, start, frames, flags + ["--panel-limits"])
                fr, px, worst = compare(ref, got)
                ok = fr == 0
                bad += not ok
                print(f"  {s.name:18s} {label:24s} {'identical' if ok else f'{fr} frames, {px} px, max {worst}':>22s}")
        bad += wc_parity(tmp)
        bad += tz_check()
        bad += fit_check()
        if quick:
            return
        print("\nmeasurements, 12:34 case, 240 frames (instructions exact; times are this host's)")
        print(f"  {'script':14s} {'load instr':>11s} {'draw instr avg/max':>19s} {'host draw ms avg/max':>21s}"
              f" {'heap peak KB':>12s} {'incr: ms / KB':>13s} {'stack KB':>8s} {'fps':>4s} {'period':>6s}")
        calib = tmp / "calib.lua"
        calib.write_text(BENCH_COMPUTE)
        ns = BENCH_MS * 1e6 / stats(render("fxhost", calib, tmp / "c.raw", "12:34", 1, ["--exact"]))["load_instr"]
        # The harness itself (file I/O, printf, thread start) uses stack too:
        # measured on a script that does nothing, and taken off every figure.
        empty = tmp / "empty.lua"
        empty.write_text("function draw() end\n")
        base = stats(render("fxhost", empty, tmp / "e.raw", "12:34", 240, []))["stack_used"]
        for s in scripts:
            ex = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, ["--exact"]))
            tm = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, []))
            gn = stats(render("fxhost", s, tmp / "x.raw", "12:34", 240, ["--incremental"]))
            print(f"  {ex['name']:14s} {ex['load_instr']:11.0f} {ex['instr_avg']:9.0f}/{ex['instr_max']:<9.0f}"
                  f" {tm['draw_ms_avg']:10.3f}/{tm['draw_ms_max']:<10.3f} {tm['heap_peak'] / 1024:12.0f}"
                  f" {gn['draw_ms_avg']:6.3f}/{gn['heap_peak'] / 1024:<6.0f} {(tm['stack_used'] - base) / 1024:8.1f}"
                  f" {tm['fps']:4.0f} {tm['period']:6.0f}")
            print(f"  {'':14s} panel estimate at the bench's VM rate: load {ex['load_instr'] * ns / 1e6:.0f} ms,"
                  f" draw {ex['instr_avg'] * ns / 1e6:.1f}/{ex['instr_max'] * ns / 1e6:.1f} ms (VM only, px.* C work not included)")
        print(f"\n  bench calibration: {ns:.0f} ns per instruction on the panel (kCompute, phase 6b)")
        print(f"  stack KB is above the harness's own {base / 1024:.1f} KB, on this host's ABI - not Xtensa's")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        if bad:
            print(f"\n{bad} case(s) differ")
    sys.exit(1 if bad else 0)


main()

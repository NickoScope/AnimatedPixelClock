#!/usr/bin/env python3
"""The weather clock's firmware drawing against the previews the owner chose from.

src/clocks/weather_layout.h is compiled on the host with the real Adafruit GFX
library (.pio/libdeps/*/Adafruit GFX Library: Adafruit_GFX.cpp and glcdfont.c)
and draws every scene of tools/climate/render.py that today's screen and design
B cover. The check:

1. render.py's CL_B_* layout, indoor colours, house glyph and placeholders
   against weather_layout.h, name for name.
2. Each firmware frame pixel for pixel against render.py's drawing of the same
   scene, and render.py's drawing against the committed 1:1 PNG in preview/.
   b_split_absent is B with the sensor absent: it must be today_absent.
3. The strings the firmware drew (outdoor temperature, details row, indoor
   temperature and humidity, split or not, unit letter or not) against
   preview/frames.json.
4. The unit-letter edge: with three characters (-10 °C and below, 100 °F and
   above) the degree ring is drawn and the letter is not; with two, both are.
5. Nothing drawn off the panel.

What it cannot see: the DMA library's own overrides of drawPixel, fillRect and
the fast lines (the same pixels, faster), and the panel's colour depth and
brightness, which apply to every page alike.

  python3 tools/climate/check_weather_screen.py
"""
import glob
import json
import pathlib
import re
import subprocess
import sys
import tempfile

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import render as R  # noqa: E402

LAYOUT = ROOT / "src/clocks/weather_layout.h"
W, H = 128, 64


def rgb(v):
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def c565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def gfx_dir():
    # The panel's own env first: tools/flag_matrix.py installs a scratch env's
    # copy next to it while it runs, and a half-copied library would not build.
    own = ROOT / ".pio/libdeps/matrix-waveshare-rgb/Adafruit GFX Library"
    if (own / "Adafruit_GFX.cpp").is_file():
        return own
    hits = sorted(glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/Adafruit_GFX.cpp")))
    if not hits:
        sys.exit("check_weather_screen: build any env once so PlatformIO installs Adafruit GFX")
    return pathlib.Path(hits[0]).parent


def build(tmp):
    gfx, stub = gfx_dir(), HERE / "hoststub"
    common = ["-std=c++17", "-O2", "-DARDUINO=10819", "-isystem", str(stub), "-isystem", str(gfx)]
    obj = tmp / "gfx.o"
    subprocess.run(["c++", *common, "-w", "-c", str(gfx / "Adafruit_GFX.cpp"), "-o", str(obj)], check=True)
    exe = tmp / "weather_screen_host"
    subprocess.run(["c++", *common, "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src/clocks"),
                    str(HERE / "weather_screen_host.cpp"), str(obj), "-o", str(exe)], check=True)
    return exe


def constants(problems):
    hdr = LAYOUT.read_text()
    ints = {}
    for m in re.finditer(r"static const int16_t (CL_B_\w+) = ([^;]+);", hdr):
        ints[m[1]] = eval(m[2], {"__builtins__": {}}, dict(ints))
    py = {k: getattr(R, k) for k in dir(R) if k.startswith("CL_B_")}
    if ints != py:
        problems.append(f"CL_B_*: weather_layout.h {ints} != render.py {py}")
    cols = {m[1]: tuple(int(v) for v in m[2].split(","))
            for m in re.finditer(r"static const uint8_t (CL_\w+)\[3\] = \{([^}]*)\};", hdr)}
    want = {"CL_RULE", "CL_IN_GLYPH", "CL_IN_TEMP", "CL_IN_HUM", "CL_STALE"}
    if set(cols) != want:
        problems.append(f"colours in weather_layout.h: {sorted(cols)}, expected {sorted(want)}")
    for name, val in cols.items():
        if tuple(getattr(R, name, ())) != val:
            problems.append(f"{name}: weather_layout.h {val} != render.py {getattr(R, name, None)}")
    house = tuple(re.findall(r'"([.#]+)"', re.search(r"CL_HOUSE7\[7\] = \{(.*?)\};", hdr, re.S)[1]))
    if house != tuple(R.HOUSE7):
        problems.append("CL_HOUSE7 differs from render.py's HOUSE7")
    for name in ("IN_DASH_T", "IN_DASH_H"):
        m = re.search(r'static const char \*const ' + name + r' = "([^"]*)";', hdr)
        if not m or m[1] != getattr(R, name):
            problems.append(f"{name} differs from render.py")
    return len(ints) + len(cols) + 3


def scenes():
    """(frame the firmware draws, scene, B on, render.py's drawing, the preview it must equal)."""
    out = [("today_live", R.LIVE, False, R.today, "today_live"),
           ("today_absent", R.ABSENT, False, R.today, "today_absent"),
           ("today_worst", R.WORST, False, R.today, "today_worst"),
           ("b_split_live", R.LIVE, True, R.variant_b, "b_split_live"),
           ("b_split_stale", R.STALE, True, R.variant_b, "b_split_stale"),
           ("b_split_worst", R.WORST, True, R.variant_b, "b_split_worst"),
           ("b_split_absent", R.ABSENT, True, R.today, "today_absent")]
    out += [(f"b_split_{k}", sc, True, R.variant_b, f"b_split_{k}") for k, sc in R.EDGE.items()]
    return out


def spec(name, sc, split):
    swap = R._def["WDETAIL_SWAP_MS"]
    if sc.get("phase", 0) != (sc["now"] // swap) % 2:
        sys.exit(f"check_weather_screen: scene {name}: render.py's phase does not follow its clock")
    o, ind = sc["out"], sc["ind"]
    indoor = {"live": 1, "stale": 2}.get(ind["state"], 0) if split else 0   # absent: climate::weatherIndoor() gives None
    h, m = sc["time"]
    fields = [name, 1, h, m, 1, int(sc["h12"]), int(bool(sc.get("pm"))), int(sc["wifi"]), sc["now"],
              "%04x" % c565(R.COL_DIGIT), "%04x" % c565(R.COL_ICON), "%04x" % c565(R.COL_ACCENT),
              "%04x" % c565(R.COL_TEMP), 2, R.ICON_KINDS.index(o["kind"]), o["t"], o["hi"], o["lo"], o["rh"],
              o["rise"], o["set"], int(sc["F"]), indoor, ind.get("t", 0.0), ind.get("rh", 0.0)]
    return " ".join(str(v) for v in fields)


def run(exe, tmp, table):
    path = tmp / "scenes.txt"
    path.write_text("\n".join(spec(name, sc, split) for name, sc, split, _, _ in table) + "\n")
    r = subprocess.run([str(exe), str(path)], capture_output=True, text=True, check=True)
    lines = r.stdout.split("\n")
    frames, notes, i = {}, {}, 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("FRAME "):
            _, name, off = ln.split(" ")
            frames[name] = ([[int(row[k * 4:k * 4 + 4], 16) for k in range(W)] for row in lines[i + 1:i + 1 + H]],
                            int(off))
            i += 1 + H
            continue
        if ln.startswith("NOTES "):
            p = ln[len("NOTES "):].split("\t")
            notes[p[0]] = {"outdoor": p[1], "details": p[2], "inT": p[3], "inH": p[4],
                           "split": p[5] == "1", "unit": p[6] == "1"}
        i += 1
    return frames, notes


def unit_edge(name, raster, got, want_digits, fahrenheit, problems):
    """The degree ring and the letter beside the big temperature, read off the firmware's raster."""
    if got["outdoor"] != want_digits:
        problems.append(f"{name}: outdoor '{got['outdoor']}', the edge case needs '{want_digits}'")
        return
    y = R.WICON_Y + R.WTEMP_DY
    end_x = R.CL_B_TEMP_X + len(want_digits) * 18
    temp = c565(R.COL_TEMP)
    ring = R.Frame()
    ring.circle(end_x + 2, y + 1, 2, (255, 255, 255))
    letter = R.Frame()
    letter.text5(end_x + 7, y, "F" if fahrenheit else "C", (255, 255, 255))   # the letter this scene would draw
    lit = lambda fr: [(x, yy) for yy in range(H) for x in range(W) if fr.px[x, yy] != (0, 0, 0)]  # noqa: E731
    ring_ok = all(raster[yy][x] == temp for x, yy in lit(ring))
    letter_drawn = all(raster[yy][x] == temp for x, yy in lit(letter))
    want_letter = len(want_digits) < 3
    if not ring_ok:
        problems.append(f"{name}: the degree ring beside '{want_digits}' is not drawn")
    if letter_drawn != want_letter or got["unit"] != want_letter:
        problems.append(f"{name}: unit letter {'drawn' if letter_drawn else 'absent'} (layout says "
                        f"{got['unit']}), expected {'drawn' if want_letter else 'absent'} for '{want_digits}'")
    return f"{name:22s} '{want_digits}': degree ring drawn, letter {'drawn' if want_letter else 'absent'}"


def main():
    problems = []
    n_const = constants(problems)
    table = scenes()
    frames_json = json.loads((R.PREVIEW / "frames.json").read_text())
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        frames, notes = run(build(tmp), tmp, table)

    print(f"constants: {n_const} checked name for name against render.py")
    for name, sc, split, fn, expect in table:
        ref = R.draw(fn, sc)
        png = Image.open(R.PREVIEW / f"{expect}_128x64.png").convert("RGB")
        if list(png.getdata()) != list(ref.img.getdata()):
            problems.append(f"preview/{expect}_128x64.png is not what render.py draws now: rerun render.py")
        raster, off = frames[name]
        diffs = [(x, y, rgb(raster[y][x]), ref.px[x, y])
                 for y in range(H) for x in range(W) if rgb(raster[y][x]) != ref.px[x, y]]
        if off:
            problems.append(f"{name}: {off} pixel(s) drawn off the panel")
        if diffs:
            problems.append(f"{name}: {len(diffs)} pixel(s) differ from {expect}, first {diffs[:6]}")

        got, exp = notes[name], frames_json[expect]["notes"]
        drawn = {"outdoor": got["outdoor"], "details": got["details"].replace("\x18", "^").replace("\x19", "v")}
        if got["split"]:
            drawn["indoor"] = f"{got['inT']}° {got['inH']}"
            drawn["unit_letter"] = got["unit"]
        if drawn != exp:
            problems.append(f"{name}: strings {drawn} != frames.json {expect} {exp}")
        print(f"  {name:22s} {'identical' if not diffs else 'DIFFERS':9s} to {expect:22s} {drawn}")

    print("unit letter:")
    for name, digits, sc in (("b_split_worst", "104", R.WORST), ("b_split_edge_m10c", "-10", R.EDGE["edge_m10c"]),
                             ("b_split_edge_m9c", "-9", R.EDGE["edge_m9c"]), ("b_split_edge_99f", "99", R.EDGE["edge_99f"]),
                             ("b_split_edge_100f", "100", R.EDGE["edge_100f"])):
        line = unit_edge(name, frames[name][0], notes[name], digits, sc["F"], problems)
        if line:
            print("  " + line)

    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print(f"\nweather screen: {len(table)} frames pixel-identical to the previews, strings as frames.json")


if __name__ == "__main__":
    main()

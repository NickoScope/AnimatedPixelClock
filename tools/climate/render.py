#!/usr/bin/env python3
"""Host render of the weather clock (style 14) with the onboard sensor's indoor
reading added, 128x64: three designs for the owner to choose from before any
weather-screen code is written (owner, 2026-09-15 18:24: build the board's
temperature/humidity sensor and fit it beautifully into the weather screen).

"today" is the weather clock the panel draws. Its layout #defines are read out
of src/clocks/weather_layout.h and its default colours out of
src/config/settings.cpp, and every draw call is mirrored line by line: the
icons, the size-3 temperature with its drawCircle degree, the details row, the
no-WiFi icon (its calls executed from src/clocks/wifi_icon.h), AM/PM. The
variants change it only where their constants below say.

The owner chose B on 2026-09-15 19:10. The firmware draws it from
weather_layout.h, whose CL_B_* values, colours, house glyph and dashes equal the
ones below name for name. tools/climate/check_weather_screen.py runs that header
on the host with the real Adafruit GFX library and holds its frames to these,
pixel for pixel. The b_split_edge_* frames are the unit letter's edge: three
characters (-10 °C and below, 100 °F and above) keep the degree mark and drop
the letter.

Fonts are the panel's: the built-in 5x7 (tools/glcdfont.json, Adafruit GFX
drawChar at sizes 1-3, with its "c >= 176 -> c + 1" classic quirk) and
Picopixel (tools/picopixel.json: capitals, digits, . % -). Discs, circles and
lines follow Adafruit GFX's fillCircle / drawCircle / writeLine. Colours pass
through color565 as the panel keeps them.

Every variant comes in three frames: live, stale (the sensor stopped answering
or its CRC keeps failing), worst (12-hour clock, no WiFi icon, Fahrenheit, a
three-digit outside temperature, 100 %). When the sensor is absent every
variant falls back to today's screen, drawn once as today_absent. A layout
budget renders each design with the widest strings, every icon at every
animation phase, and refuses an overlap or ink off the panel.

  python3 tools/climate/render.py     # frames into preview/ at 6x and 1:1, contact_sheet.png, b_edges_sheet.png, frames.json
"""
import json
import math
import pathlib
import re
import struct
import sys
from contextlib import contextmanager

from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PREVIEW = HERE / "preview"
W, H = 128, 64
SCALE = 6

# ── today's weather clock, read from the firmware ────────────────────────────
WX = (ROOT / "src/clocks/weather_layout.h").read_text()
_def = {m[1]: int(m[2]) for m in re.finditer(r"#define\s+(W[A-Z_]+)\s+(\d+)", WX)}
WTIME_Y, WICON_X, WICON_Y = _def["WTIME_Y"], _def["WICON_X"], _def["WICON_Y"]
WICON_SIZE, WDETAIL_Y = _def["WICON_SIZE"], _def["WDETAIL_Y"]
WTEMP_X, WTEMP_DY = _def["WTEMP_X"], _def["WTEMP_DY"]
MERIDIEM_X, MERIDIEM_DY = _def["WMERIDIEM_X"], _def["WMERIDIEM_DY"]
WIFI_SRC = re.search(r"void drawNoWiFiIconOn\(G &g, int x, int y\)\s*\{(.*?)\n\}",
                     (ROOT / "src/clocks/wifi_icon.h").read_text(), re.S)[1]

SETTINGS = (ROOT / "src/config/settings.cpp").read_text()


def rgb565(v):
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def c565(c):
    r, g, b = c
    return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)


def slot(name):
    return rgb565(int(re.search(r"/\*\s*" + name + r"\s*\*/\s*0x([0-9A-Fa-f]{4})", SETTINGS)[1], 16))


COL_ICON, COL_ACCENT, COL_TEMP = slot("COL_WEATHER_ICON"), slot("COL_WEATHER_ACCENT"), slot("COL_WEATHER_TEMP")
COL_DIGIT = rgb565(int(re.findall(r"0x([0-9A-Fa-f]{4})",
                                  re.search(r"COL_DIGITS_S0\.\.S14\s*\*/(.*?)/\*", SETTINGS, re.S)[1])[14], 16))
WHITE = rgb565(0xFFFF)                           # DISPLAY_WHITE

# ── the variants' layout, the firmware copies this block ─────────────────────
# House colours, before color565 (railboard.cpp, market_layout.cpp).
CL_AMBER = (255, 150, 0)
CL_DIM = (110, 122, 128)
CL_RULE = (52, 60, 64)                           # flightboard.cpp's rule under the header
CL_IN_GLYPH = CL_AMBER                           # the house glyph: amber, the colour of a heading
CL_IN_TEMP = (255, 255, 255)                     # the indoor temperature: white, like the outdoor details row
CL_IN_HUM = CL_DIM                               # the indoor humidity: dim, the least important number on the screen
CL_STALE = CL_DIM                                # a stale reading: glyph and dashes, all dim

# A - the indoor line: the weather block moves up 5 rows (time 1 row) and a
# 5x7 line under the details row carries the house, temperature and humidity.
CL_A_TIME_Y = 1
CL_A_ICON_Y = 18
CL_A_TEMP_Y = CL_A_ICON_Y + WTEMP_DY
CL_A_DETAIL_Y = 47
CL_A_IN_Y = 56
CL_A_GAP_GLYPH = 4                               # house to temperature
CL_A_GAP_HUM = 7                                 # degree mark to humidity

# B - split: outdoor on the left (icon at the edge, the size-3 temperature
# after it), a dim rule, indoor in a column on the right. The details row is
# unchanged under both.
CL_B_ICON_X = 1                                  # 0 would put the fog's drifting lines off the panel
CL_B_TEMP_X = 28
CL_B_RULE_X = 89
CL_B_RULE_Y0, CL_B_RULE_Y1 = 26, 47                # the span of the big digits and their degree mark
CL_B_COL_X = 92                                  # the indoor column, x 92-127
CL_B_GLYPH_Y = 28
CL_B_IN_T_Y = 28                                 # 5x7, beside the house
CL_B_IN_H_Y = 39                                 # 5x7, under the temperature
CL_B_IN_TEXT_X = CL_B_COL_X + 9

# C - the badge: two Picopixel lines at the panel's right edge, under the
# unit letter and beside the big temperature. Nothing else moves. The house
# stands at a fixed column, so a three-digit outside temperature (ink to x 102)
# keeps one dark column from it whatever the number beside it; the humidity
# starts under the number.
CL_C_X = 104                                     # the house, x 104-108
CL_C_GAP_GLYPH = 1                               # the number from x 110; its widest (13 px) and the degree end at 126
CL_C_LINE1_TOP = 37                              # house + temperature, rows 37-41
CL_C_LINE2_TOP = 43                              # humidity, rows 43-47

# The indoor temperature: one decimal while it fits four characters, whole
# degrees beyond (-10 and below, 100 and above); humidity in whole percent (the
# sensor is +-2 %RH typical, datasheet Table 1).
IN_DASH_T, IN_DASH_H = "--.-", "--%"

# ── glyphs ───────────────────────────────────────────────────────────────────
GLCD = json.load(open(ROOT / "tools/glcdfont.json"))
PICO = json.load(open(ROOT / "tools/picopixel.json"))["glyphs"]
PICO_ASCENT = 4                                  # a capital's top is 4 rows above the baseline (yo = -4)

HOUSE7 = ("...#...",                             # for 5x7 lines: a solid house with a door
          "..###..",
          ".#####.",
          "#######",
          ".#####.",
          ".##.##.",
          ".##.##.")
HOUSE5 = ("..#..",                               # for Picopixel lines
          ".###.",
          "#####",
          "##.##",
          "##.##")


def f32(v):
    return struct.unpack("f", struct.pack("f", v))[0]


def c_round(v):                                  # roundf: half away from zero
    return int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)


class Frame:
    def __init__(self):
        self.img = Image.new("RGB", (W, H), (0, 0, 0))
        self.px = self.img.load()
        self.cur = None
        self.ink = {}
        self.pixels = {}
        self.off = set()
        self.notes = {}

    @contextmanager
    def part(self, name):
        prev, self.cur = self.cur, name
        try:
            yield
        finally:
            self.cur = prev

    def dot(self, x, y, c):
        if self.cur:
            b = self.ink.get(self.cur)
            self.ink[self.cur] = [x, y, x, y] if b is None else [min(b[0], x), min(b[1], y), max(b[2], x), max(b[3], y)]
            self.pixels.setdefault(self.cur, set()).add((x, y))
        if 0 <= x < W and 0 <= y < H:
            self.px[x, y] = c565(c)
        elif self.cur:
            self.off.add(self.cur)

    # Adafruit GFX, as the library draws
    def vline(self, x, y, h, c):
        for yy in range(y, y + h):
            self.dot(x, yy, c)

    def fill_rect(self, x, y, w, h, c):
        for i in range(x, x + w):
            self.vline(i, y, h, c)

    def circle(self, x0, y0, r, c):
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
        for p in ((x0, y0 + r), (x0, y0 - r), (x0 + r, y0), (x0 - r, y0)):
            self.dot(*p, c)
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            for dx, dy in ((x, y), (-x, y), (x, -y), (-x, -y), (y, x), (-y, x), (y, -x), (-y, -x)):
                self.dot(x0 + dx, y0 + dy, c)

    def fill_circle(self, x0, y0, r, c):
        self.vline(x0, y0 - r, 2 * r + 1, c)
        f, ddx, ddy, x, y, px, py, delta = 1 - r, 1, -2 * r, 0, r, 0, r, 1
        while x < y:
            if f >= 0:
                y -= 1
                ddy += 2
                f += ddy
            x += 1
            ddx += 2
            f += ddx
            if x < y + 1:
                self.vline(x0 + x, y0 - y, 2 * y + delta, c)
                self.vline(x0 - x, y0 - y, 2 * y + delta, c)
            if y != py:
                self.vline(x0 + py, y0 - px, 2 * px + delta, c)
                self.vline(x0 - py, y0 - px, 2 * px + delta, c)
                py = y
            px = x

    def line(self, x0, y0, x1, y1, c):
        steep = abs(y1 - y0) > abs(x1 - x0)
        if steep:
            x0, y0, x1, y1 = y0, x0, y1, x1
        if x0 > x1:
            x0, x1, y0, y1 = x1, x0, y1, y0
        dx, dy = x1 - x0, abs(y1 - y0)
        err, ystep = dx // 2, 1 if y0 < y1 else -1
        while x0 <= x1:
            self.dot(y0, x0, c) if steep else self.dot(x0, y0, c)
            err -= dy
            if err < 0:
                y0 += ystep
                err += dx
            x0 += 1

    def char5(self, x, y, ch, c, size):
        code = ord(ch)
        if code >= 176:                          # classic charset: GFX shifts these by one
            code += 1
        for i in range(5):
            bits = GLCD[code * 5 + i]
            for j in range(8):
                if (bits >> j) & 1:
                    if size == 1:
                        self.dot(x + i, y + j, c)
                    else:
                        self.fill_rect(x + i * size, y + j * size, size, size, c)

    def text5(self, x, y, s, c, size=1):
        for ch in s:
            self.char5(x, y, ch, c, size)
            x += 6 * size
        return x

    def num5(self, x, y, s, c):
        """A 5x7 number with a thin dot: the dot is drawn 2 columns left and advances
        3, as market_layout's numbers do, so 23.4 reads as one number."""
        for ch in s:
            if ch == ".":
                self.char5(x - 2, y, ch, c, 1)
                x += 3
            else:
                self.char5(x, y, ch, c, 1)
                x += 6
        return x

    def pico(self, x, top, s, c):
        base = top + PICO_ASCENT
        for ch in s:
            g = PICO.get(ch) or PICO[" "]
            for r in range(g["h"]):
                for cc in range(g["w"]):
                    if (g["rows"][r] >> cc) & 1:
                        self.dot(x + g["xo"] + cc, base + g["yo"] + r, c)
            x += g["adv"]
        return x

    def bitmap(self, x, y, rows, c):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch == "#":
                    self.dot(x + i, y + j, c)


def w_num5(s):
    return sum(3 if ch == "." else 6 for ch in s) - 1


def w_pico(s):
    x, lo, hi = 0, 1 << 15, -1
    for ch in s:
        g = PICO.get(ch) or PICO[" "]
        if g["w"] and g["h"]:
            lo, hi = min(lo, x + g["xo"]), max(hi, x + g["xo"] + g["w"] - 1)
        x += g["adv"]
    return hi - lo + 1 if hi >= lo else 0


# ── today's weather clock, mirrored from clock_weather.cpp ───────────────────
def cloud(f, cx, cy, c):
    f.fill_circle(cx - 6, cy + 2, 4, c)
    f.fill_circle(cx, cy - 1, 5, c)
    f.fill_circle(cx + 6, cy + 2, 4, c)
    f.fill_rect(cx - 6, cy + 2, 13, 4, c)


def icon(f, x, y, kind, now):
    body, accent = COL_ICON, COL_ACCENT
    cx, cy = x + WICON_SIZE // 2, y + WICON_SIZE // 2
    cosf = lambda a: f32(math.cos(f32(a)))       # noqa: E731
    sinf = lambda a: f32(math.sin(f32(a)))       # noqa: E731
    if kind == "sun":
        f.fill_circle(cx, cy, 6, body)
        pulse = (now // 400) % 2
        for i in range(8):
            a = i * (math.pi / 4.0)
            ln = 10 + (1 if i % 2 == pulse else -1)
            f.line(cx + int(cosf(a) * 8), cy + int(sinf(a) * 8), cx + int(cosf(a) * ln), cy + int(sinf(a) * ln), body)
    elif kind == "partcloud":
        f.fill_circle(cx - 4, cy - 4, 5, body)
        for i in range(4):
            a = i * (math.pi / 2.0) - math.pi / 4.0
            f.line(cx - 4 + int(cosf(a) * 6), cy - 4 + int(sinf(a) * 6),
                   cx - 4 + int(cosf(a) * 9), cy - 4 + int(sinf(a) * 9), body)
        cloud(f, cx + 3, cy + 4, accent)
    elif kind == "cloud":
        cloud(f, cx + (1 if (now // 700) % 2 else 0), cy - 1, body)
    elif kind == "fog":
        for i in range(4):
            shift = int((now // 150 + i * 4) % 8) - 4
            if i % 2:
                shift = -shift
            for xx in range(16):
                f.dot(x + 3 + shift + xx, y + 5 + i * 5, accent if i % 2 else body)
    elif kind == "rain":
        cloud(f, cx, cy - 5, body)
        for i in range(3):
            f.vline(cx - 6 + i * 6, cy + 2 + (now // 60 + i * 5) % 12, 3, accent)
    elif kind == "snow":
        cloud(f, cx, cy - 5, body)
        for i in range(3):
            fy = (now // 120 + i * 6) % 12
            fx = cx - 6 + i * 6 + (1 if (now // 240 + i) % 2 else -1)
            f.dot(fx, cy + 2 + fy, accent)
            f.dot(fx, cy + 3 + fy, accent)
    elif kind == "storm":
        cloud(f, cx, cy - 5, body)
        for i in range(2):
            f.vline(cx - 7 + i * 13, cy + 2 + (now // 60 + i * 7) % 10, 3, accent)
        if now % 1600 < 300:
            f.line(cx + 1, cy + 1, cx - 2, cy + 6, WHITE)
            f.line(cx - 2, cy + 6, cx + 2, cy + 6, WHITE)
            f.line(cx + 2, cy + 6, cx - 1, cy + 12, WHITE)


ICON_KINDS = ("sun", "partcloud", "cloud", "fog", "rain", "snow", "storm")


def outdoor_str(t_c, fahrenheit):
    return "%d" % c_round(t_c * 9.0 / 5.0 + 32.0 if fahrenheit else t_c)


def temperature(f, x, y, t_c, fahrenheit, unit=True):
    """drawTemperature(): size-3 digits, a radius-2 circle, the unit letter at size 1.
    `unit` False leaves the letter out (variant B, when three characters fill the slot)."""
    s = outdoor_str(t_c, fahrenheit)
    with f.part("temp"):
        f.text5(x, y, s, COL_TEMP, 3)
        end_x = x + len(s) * 18
        f.circle(end_x + 2, y + 1, 2, COL_TEMP)
    if unit:
        with f.part("unit"):
            f.text5(end_x + 7, y, "F" if fahrenheit else "C", COL_TEMP, 1)
    return s


def time_row(f, sc, y):
    h, m = sc["time"]
    with f.part("time"):
        f.text5((W - 5 * 12) // 2, y, "%02d:%02d" % (h, m), COL_DIGIT, 2)
    if sc["h12"]:
        with f.part("meridiem"):
            f.text5(MERIDIEM_X, y + MERIDIEM_DY, "PM" if sc.get("pm") else "AM", WHITE, 1)


def details_line(sc):
    o, fahr = sc["out"], sc["F"]
    if sc.get("phase", 0) == 0:
        hi, lo = (o["hi"], o["lo"]) if not fahr else (o["hi"] * 9 / 5 + 32, o["lo"] * 9 / 5 + 32)
        return "%d\x18 %d\x19  %d%% RH" % (c_round(hi), c_round(lo), o["rh"])
    return "\x18%s  \x19%s" % (o["rise"], o["set"])


def details_row(f, sc, y):
    line = details_line(sc)
    with f.part("details"):
        f.text5((W - len(line) * 6) // 2, y, line, WHITE, 1)
    f.notes["details"] = line.replace("\x18", "^").replace("\x19", "v")


def no_wifi(f, sc):
    if sc["wifi"]:
        return
    with f.part("nowifi"):
        for m in re.finditer(r"g\.(drawPixel|fillRect|drawLine)\(([^;]*)\);", WIFI_SRC):
            args = [eval(a.replace("DISPLAY_WHITE", "0"), {}, {"x": 0, "y": 0}) for a in m[2].split(",")]
            if m[1] == "drawPixel":
                f.dot(args[0], args[1], WHITE)
            elif m[1] == "fillRect":
                f.fill_rect(args[0], args[1], args[2], args[3], WHITE)
            else:
                f.line(args[0], args[1], args[2], args[3], WHITE)


def today(f, sc, time_y=WTIME_Y, icon_x=WICON_X, icon_y=WICON_Y, temp_x=WTEMP_X, temp_y=None,
          detail_y=WDETAIL_Y, unit=True):
    time_row(f, sc, time_y)
    with f.part("icon"):
        icon(f, icon_x, icon_y, sc["out"]["kind"], sc["now"])
    f.notes["outdoor"] = temperature(f, temp_x, icon_y + WTEMP_DY if temp_y is None else temp_y,
                                     sc["out"]["t"], sc["F"], unit)
    details_row(f, sc, detail_y)
    no_wifi(f, sc)


# ── the indoor reading ───────────────────────────────────────────────────────
def indoor_strings(sc):
    ind = sc["ind"]
    if ind["state"] != "live":
        return IN_DASH_T, IN_DASH_H
    t = ind["t"] * 9.0 / 5.0 + 32.0 if sc["F"] else ind["t"]
    ts = "%.1f" % t if -9.95 < t < 99.95 else "%d" % c_round(t)
    return ts, "%d%%" % c_round(ind["rh"])


def in_colours(sc):
    live = sc["ind"]["state"] == "live"
    return (CL_IN_GLYPH, CL_IN_TEMP, CL_IN_HUM) if live else (CL_STALE, CL_STALE, CL_STALE)


def degree5(f, x, top, c):
    """A degree mark beside a 5x7 number: drawCircle(r = 1) at the glyph's top, 3x3."""
    f.circle(x + 1, top + 1, 1, c)


def degree_pico(f, x, top, c):
    f.circle(x + 1, top + 1, 1, c)


def variant_a(f, sc):
    today(f, sc, time_y=CL_A_TIME_Y, icon_y=CL_A_ICON_Y, temp_y=CL_A_TEMP_Y, detail_y=CL_A_DETAIL_Y)
    ts, hs = indoor_strings(sc)
    cg, ct, ch = in_colours(sc)
    width = 7 + CL_A_GAP_GLYPH + w_num5(ts) + 1 + 3 + CL_A_GAP_HUM + (len(hs) * 6 - 1)
    x = (W - width) // 2
    with f.part("in_glyph"):
        f.bitmap(x, CL_A_IN_Y, HOUSE7, cg)
    x += 7 + CL_A_GAP_GLYPH
    with f.part("in_temp"):
        x = f.num5(x, CL_A_IN_Y, ts, ct) - 1
        degree5(f, x + 1, CL_A_IN_Y, ct)
    x += 1 + 3 + CL_A_GAP_HUM
    with f.part("in_hum"):
        f.text5(x, CL_A_IN_Y, hs, ch)
    f.notes["indoor"] = "%s° %s" % (ts, hs)


def variant_b(f, sc):
    three = len(outdoor_str(sc["out"]["t"], sc["F"])) >= 3
    today(f, sc, icon_x=CL_B_ICON_X, temp_x=CL_B_TEMP_X, unit=not three)
    with f.part("rule"):
        f.vline(CL_B_RULE_X, CL_B_RULE_Y0, CL_B_RULE_Y1 - CL_B_RULE_Y0 + 1, CL_RULE)
    ts, hs = indoor_strings(sc)
    cg, ct, ch = in_colours(sc)
    with f.part("in_glyph"):
        f.bitmap(CL_B_COL_X, CL_B_GLYPH_Y, HOUSE7, cg)
    with f.part("in_temp"):
        x = f.num5(CL_B_IN_TEXT_X, CL_B_IN_T_Y, ts, ct) - 1
        degree5(f, x + 1, CL_B_IN_T_Y, ct)
    with f.part("in_hum"):
        f.text5(CL_B_IN_TEXT_X, CL_B_IN_H_Y, hs, ch)
    f.notes["indoor"] = "%s° %s" % (ts, hs)
    f.notes["unit_letter"] = not three


def variant_c(f, sc):
    today(f, sc)
    ts, hs = indoor_strings(sc)
    cg, ct, ch = in_colours(sc)
    with f.part("in_glyph"):
        f.bitmap(CL_C_X, CL_C_LINE1_TOP, HOUSE5, cg)
    x = CL_C_X + 5 + CL_C_GAP_GLYPH
    with f.part("in_temp"):
        f.pico(x, CL_C_LINE1_TOP, ts, ct)
        degree_pico(f, x + w_pico(ts) + 1, CL_C_LINE1_TOP, ct)
    with f.part("in_hum"):
        f.pico(x, CL_C_LINE2_TOP, hs, ch)
    f.notes["indoor"] = "%s° %s" % (ts, hs)


VARIANTS = {"a_line": variant_a, "b_split": variant_b, "c_badge": variant_c}

# ── scenes ───────────────────────────────────────────────────────────────────
# Typical September afternoon weather; indoor 23.4 °C, 45 % (the brief's values).
LIVE = dict(time=(14, 27), h12=False, pm=True, wifi=True, F=False, now=2000, phase=0,
            out=dict(kind="partcloud", t=17.2, hi=19.4, lo=11.2, rh=62, rise="07:12", set="19:48"),
            ind=dict(state="live", t=23.4, rh=45.0))
STALE = dict(LIVE, ind=dict(state="stale"))
ABSENT = dict(LIVE, ind=dict(state="absent"))
# Synthetic, for width: 12-hour clock with PM, no WiFi, Fahrenheit, 104 °F outside with 100 %.
WORST = dict(time=(12, 47), h12=True, pm=True, wifi=False, F=True, now=2000, phase=0,
             out=dict(kind="sun", t=40.2, hi=41.1, lo=27.2, rh=100, rise="07:12", set="19:48"),
             ind=dict(state="live", t=27.0, rh=100.0))

SCENES = {"live": LIVE, "stale": STALE, "worst": WORST}
# B's unit-letter edge, synthetic, on either side of three characters (WORST is
# the third case, 104 °F). The details row follows the outside temperature.
EDGE = {"edge_m9c": dict(LIVE, out=dict(LIVE["out"], kind="snow", t=-9.4, hi=-6.0, lo=-12.4, rh=86)),
        "edge_m10c": dict(LIVE, out=dict(LIVE["out"], kind="snow", t=-9.6, hi=-6.0, lo=-12.4, rh=86)),
        "edge_99f": dict(LIVE, F=True, out=dict(LIVE["out"], kind="sun", t=37.2, hi=38.3, lo=24.1, rh=31)),
        "edge_100f": dict(LIVE, F=True, out=dict(LIVE["out"], kind="sun", t=37.8, hi=38.9, lo=24.1, rh=31))}


def draw(fn, sc):
    f = Frame()
    fn(f, sc)
    return f


# ── layout budget ────────────────────────────────────────────────────────────
def halo(pixels):
    """The pixels and their eight neighbours: two parts must keep one dark pixel between them."""
    return {(x + dx, y + dy) for x, y in pixels for dx in (-1, 0, 1) for dy in (-1, 0, 1)}


BUDGET_OUT_C = (-40.0, -17.8, -9.6, 8.0, 31.0, 40.2, 50.0)      # "-40", "-18", "-10", "8", "31", 104 °F / "40", 122 °F / "50"
BUDGET_IN = (("live", -9.9, 100.0), ("live", 37.7, 8.0), ("live", 23.4, 45.0), ("stale", 0.0, 0.0))


def budget():
    """Each design over every combination that moves its parts (unit, outside and
    indoor temperature widths, humidity 8-100 %, 12/24 h, the no-WiFi icon, both
    details phases), with the icon standing for every kind at every animation
    phase: no two parts may touch or come within one pixel, and nothing may be
    drawn off the panel."""
    report, bad = {}, []
    icon_pos = {"today": (WICON_X, WICON_Y), "a_line": (WICON_X, CL_A_ICON_Y),
                "b_split": (CL_B_ICON_X, WICON_Y), "c_badge": (WICON_X, WICON_Y)}
    for name, fn in [("today", today)] + list(VARIANTS.items()):
        icon_px = set()
        for kind in ICON_KINDS:
            for now in range(0, 9600, 20):
                g = Frame()
                with g.part("icon"):
                    icon(g, icon_pos[name][0], icon_pos[name][1], kind, now)
                icon_px |= g.pixels["icon"]
                if g.off:
                    bad.append(f"{name}: icon '{kind}' off the panel at {now} ms")
        cases = 0
        for fahr in (False, True):
            for out_t in BUDGET_OUT_C:
                for in_state, in_t, in_rh in BUDGET_IN:
                    for h12 in (False, True):
                        for wifi in (False, True):
                            for phase in (0, 1):
                                sc = dict(WORST, F=fahr, h12=h12, wifi=wifi, phase=phase,
                                          out=dict(WORST["out"], t=out_t, hi=50.0, lo=-40.0),
                                          ind=dict(state=in_state, t=in_t, rh=in_rh))
                                f = draw(fn, sc)
                                cases += 1
                                parts = dict(f.pixels, icon=icon_px)
                                if f.off:
                                    bad.append(f"{name}: off the panel: {sorted(f.off)}")
                                keys = sorted(parts)
                                for i, a in enumerate(keys):
                                    ha = halo(parts[a])
                                    for b in keys[i + 1:]:
                                        if ha & parts[b]:
                                            bad.append(f"{name}: {a} touches {b} (F={fahr} out={out_t} "
                                                       f"in={in_state} {in_t} {in_rh} h12={h12} wifi={wifi} phase={phase})")
        report[name] = {"cases": cases}
    return report, sorted(set(bad))


# ── output ───────────────────────────────────────────────────────────────────
def save(f, name):
    f.img.save(PREVIEW / f"{name}_128x64.png")
    f.img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(PREVIEW / f"{name}.png")


def contact_sheet(frames):
    rows = [("today", ["today_live", "today_absent", "today_worst"]),
            ("A  indoor line", ["a_line_live", "a_line_stale", "a_line_worst"]),
            ("B  split", ["b_split_live", "b_split_stale", "b_split_worst"]),
            ("C  badge", ["c_badge_live", "c_badge_stale", "c_badge_worst"])]
    cols = ["live", "stale  (today: absent, all three fall back to it)", "worst case"]
    k, pad, lab = 4, 16, 18
    cw, chh = W * k, H * k
    sheet = Image.new("RGB", (pad + 3 * (cw + pad) + 120, pad + lab + len(rows) * (chh + pad)), (22, 22, 22))
    d = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for c, t in enumerate(cols):
        d.text((120 + pad + c * (cw + pad), pad // 2), t, fill=(200, 200, 200), font=font)
    for r, (title, names) in enumerate(rows):
        y = pad + lab + r * (chh + pad)
        d.text((pad // 2, y + chh // 2 - 6), title, fill=(230, 180, 90), font=font)
        for c, n in enumerate(names):
            sheet.paste(frames[n].img.resize((cw, chh), Image.NEAREST), (120 + pad + c * (cw + pad), y))
    sheet.save(PREVIEW / "contact_sheet.png")


def edges_sheet(frames):
    names = ("b_split_edge_m9c", "b_split_edge_m10c", "b_split_edge_99f", "b_split_edge_100f")
    labels = ("-9 C: letter kept", "-10 C: letter dropped", "99 F: letter kept", "100 F: letter dropped")
    k, pad, lab = 4, 16, 18
    cw, chh = W * k, H * k
    sheet = Image.new("RGB", (pad + 2 * (cw + pad), pad + 2 * (lab + chh + pad)), (22, 22, 22))
    d = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for i, (n, t) in enumerate(zip(names, labels)):
        x, y = pad + (i % 2) * (cw + pad), pad + (i // 2) * (lab + chh + pad)
        d.text((x, y), t, fill=(230, 180, 90), font=font)
        sheet.paste(frames[n].img.resize((cw, chh), Image.NEAREST), (x, y + lab))
    sheet.save(PREVIEW / "b_edges_sheet.png")


def main():
    PREVIEW.mkdir(exist_ok=True)
    frames = {"today_live": draw(today, LIVE), "today_absent": draw(today, ABSENT), "today_worst": draw(today, WORST)}
    for vname, fn in VARIANTS.items():
        for sname, sc in SCENES.items():
            frames[f"{vname}_{sname}"] = draw(fn, sc)
    for ename, sc in EDGE.items():                   # B only: the unit letter's edge
        frames[f"b_split_{ename}"] = draw(variant_b, sc)
    for name, f in frames.items():
        save(f, name)
    contact_sheet(frames)
    edges_sheet(frames)
    report, bad = budget()
    meta = {"_meta": {"budget": report, "problems": bad,
                      "colours_before_565": {"amber": CL_AMBER, "dim": CL_DIM, "rule": CL_RULE,
                                             "in_glyph": CL_IN_GLYPH, "in_temp": CL_IN_TEMP, "in_hum": CL_IN_HUM}}}
    meta.update({n: {"notes": f.notes, "boxes": f.ink} for n, f in frames.items()})
    (PREVIEW / "frames.json").write_text(json.dumps(meta, indent=1, sort_keys=True) + "\n")
    for n, f in frames.items():
        print(f"  {n:20s} {f.notes}")
    if bad:
        print("\nlayout budget: " + str(len(bad)) + " problem(s)")
        for b in bad:
            print("  " + b)
        sys.exit(1)
    print(f"\nlayout budget: clean, {sum(r['cases'] for r in report.values())} cases")


if __name__ == "__main__":
    main()

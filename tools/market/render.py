#!/usr/bin/env python3
"""Host render of the market pages, 128x64: MARKETS, TICKER, PORTFOLIO, HOLDINGS.

Every number on a page comes out of market_ref.py on the saved Yahoo answers in
samples/; nothing is typed. Every layout constant is named once, in the MK_ block
below, and the firmware copies that block. The fonts are the panel's own: the
built-in 5x7 (tools/glcdfont.json, drawn as Adafruit GFX drawChar does, 2x for
the one key number) and Picopixel (tools/picopixel.json) for the tape, the
secondary rows and the footer. Colours are quantised as color565 keeps them, so
the PNG shows what the panel shows.

The tape (owner, 2026-09-15 09:19): rows 0-6 carry the exchanges and their
state, scrolling 1 px per frame at 20 fps; the page lives in rows 8-63. The
exchange hours come from the samples' meta.currentTradingPeriod (NYSE Arca,
Nasdaq, XETRA, Euronext Paris); LSE and TSE have no source here and are left
off. LIVE / D / CLOSE (owner, 2026-09-15 11:43): while the instrument's exchange
is OPEN the last price carries the day change against the previous close,
behind a green LIVE when the quote is less than MK_LIVE_MAX_S behind the clock,
else behind a dim delay badge, D and the whole minutes (D15); when the exchange
is not open the label reads CLOSE and the as-of date. The window change keeps
its own spot.

The six decisions of 11:43, drawn: that freshness badge; the benchmark
^SP500TR in the portfolio currency when its sample is saved (until then the
approved ^GSPC price line, and frames.json says which); PORTFOLIO's window
figure is the TWR, labelled, with XIRR since inception in the footer when
contributions are on (both from preview_returns.py); ANN from a one-year
window on PORTFOLIO and TICKER; the drawdown stop, a click, with its dates and
marks; one frame in the blue/red scheme. layout_budget() fills the new slots
with their widest strings and refuses an overlap.

Synthetic frames are named SYNTH* or ABCDEFGH / GROWTH8X and say so in the
README: they exist to fit worst-case strings and the 15-row pagination, and
their series are arithmetic transforms of the real samples. The intraday
session strip is synthetic everywhere: no 5-minute sample exists (Yahoo
answered 429 to the probe on 2026-09-15 09:30); its shape is the last 78 daily
^GSPC returns damped 8x, bent between the previous close and the last.

  python3 tools/market/render.py     # every frame into preview/ at 6x and 1:1, contact_sheet.png, frames.json
"""
import datetime as dt
import json
import math
import pathlib
import sys
from zoneinfo import ZoneInfo

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import market_ref as M  # noqa: E402
import preview_returns as PR  # noqa: E402

PREVIEW = HERE / "preview"
PARIS = ZoneInfo("Europe/Paris")
W, H = 128, 64

# ── layout, the firmware copies this block ───────────────────────────────────
MK_TAPE_H = 7            # the exchange tape: rows 0-6, Picopixel
MK_Y_TAPE = 1            # Picopixel top inside the tape: glyph rows 1-5
MK_Y_TAPE_RULE = 7       # one dark row between the tape and the page
MK_TAPE_SEP = "   "      # between exchanges on the tape
MK_TAPE_FPS = 20         # the tape moves 1 px per frame at this rate
MK_PAGE_TOP = 8          # the page: rows 8-63, 56 px
MK_X_LEFT = 1
MK_X_RIGHT = 127         # inclusive: right-aligned text ends on this column
MK_GAP = 3
MK_ADV = 6               # 5x7 advance
MK_THIN_ADV = 3          # in a number, a space (the thousands separator) or a dot advances half a cell
MK_BIG = 2               # the key number's scale: 10x14 glyphs, advance 12
MK_PICO_ASCENT = 4       # Picopixel: the baseline is 4 rows under the top (GFX yo = -4 for capitals)
MK_PICO_DROP = 2         # Picopixel top below a 5x7 row's top, so both sit on one baseline
MK_Y_HEAD = 8            # 5x7 heading, rows 8-14
MK_Y_BLOCK = 16          # the key number, rows 16-29; the 5x7 line beside its top half is at 16
MK_Y_BLOCK2 = 23         # the 5x7 line beside its bottom half, rows 23-29
MK_BIG_CHARS = 7         # TICKER, PORTFOLIO: the 2x number's slot in characters, a sign included (x 1-82)
MK_Y_FOOT = 58           # Picopixel footer, rows 58-62
MK_Y_ERR = 30            # DATA ERR, 5x7, centred
MK_Y_ERR_WHY = 41        # its reason, Picopixel, centred
MK_TICKS = (32, 64, 96)  # three time ticks, at the quarters of a full-width chart
MK_TICK_H = 2
MK_LIVE_MAX_S = 120      # LIVE only while the quote is less than this many seconds behind the clock (owner, 11:43)
MK_DELAY_BADGE = "D"     # else this badge and the whole minutes behind, dim: D15
# MARKETS
MK_Y_MKT_SYM = 16        # the primary's mnemonic and window change, rows 16-22
MK_Y_MKT_BIG = 24        # the primary's last at 2x, rows 24-37
MK_X_MKT_CHG_R = 76      # the primary's window change ends here, left of the sparkline
MK_BIG_CHARS_MKT = 6     # the 2x slot on MARKETS: x 1-70, before the sparkline
MK_X_SPARK, MK_Y_SPARK, MK_SPARK_W, MK_SPARK_H = 79, 16, 48, 16   # rows 16-31, x 79-126
MK_Y_MKT_LIVE = 33       # LIVE / CLOSE under the sparkline, rows 33-37
MK_Y_MKT_ROW0 = 41       # three secondary rows, Picopixel: 41, 49, 57
MK_MKT_ROW_H = 8
MK_X_MKT_VAL_R = 88      # secondary rows: the last ends here, the change at MK_X_RIGHT
# TICKER
MK_Y_TK_CHART, MK_TK_CHART_H = 31, 17     # the window chart, rows 31-47
MK_Y_TK_SESS, MK_TK_SESS_H = 49, 8        # today's session, rows 49-56
MK_SESSION_MIN = 390                      # the strip spans the regular session, 09:30-16:00 New York = 390 min
# PORTFOLIO
MK_Y_PF_CHART, MK_PF_CHART_H = 31, 26     # rows 31-56
MK_BIG_CHARS_PF = 6      # PORTFOLIO's 2x slot, x 1-70, so TWR and a four-digit change fit beside it (TICKER keeps MK_BIG_CHARS)
MK_Y_MDD_BAR = MK_Y_PF_CHART + MK_PF_CHART_H   # row 57, under the chart: the drawdown's bracket
MK_MDD_TICK_H = 3        # the bracket's end ticks, rows 55-57, at the peak's and the trough's columns
# HOLDINGS
MK_Y_HD_ROW0, MK_HD_ROW_H, MK_HD_ROWS = 17, 9, 4   # rows at 17, 26, 35, 44
MK_X_HD_MID_R = 84                        # TGT>NOW ends here; the return at MK_X_RIGHT

# the words of the six decisions (owner, 2026-09-15 11:43)
LBL_TWR, LBL_ANN, LBL_XIRR, LBL_MDD, LBL_REC, LBL_NO_REC = "TWR", "ANN", "XIRR", "MDD", "REC", "NO REC"

# ── colours, before color565 ─────────────────────────────────────────────────
COL_BLACK = (0, 0, 0)
COL_AMBER = (255, 150, 0)        # the panel's amber (railboard.cpp, media_page.cpp)
COL_WHITE = (255, 255, 255)
COL_DIM = (110, 122, 128)        # the design's dim
COL_GREEN = (60, 200, 90)        # softened: never 0/255
COL_RED = (230, 70, 60)
COL_BLUE = (60, 150, 255)        # the blue/red scheme's up colour, as the firmware's registry has it (market_layout.cpp C_BLUE)
COL_RULE = (36, 40, 44)          # structural, not semantic: the row under the tape
STATE_COL = {"OPEN": COL_GREEN, "PRE": COL_AMBER, "POST": COL_AMBER, "CLOSED": COL_DIM}
SCHEMES = {"green_red": (COL_GREEN, COL_RED), "blue_red": (COL_BLUE, COL_RED)}   # display.colors: (up, down), signed changes only


def c565(c):
    r, g, b = c
    return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)


# ── fonts ────────────────────────────────────────────────────────────────────
GLCD = json.load(open(ROOT / "tools/glcdfont.json"))
PICO = json.load(open(ROOT / "tools/picopixel.json"))["glyphs"]


def w5(s, k=1, thin=False):
    """Ink width of a 5x7 string: 6 per glyph less the final blank column; in a number
    (`thin`) a space or a dot takes half a cell."""
    if not s:
        return 0
    return sum(MK_THIN_ADV if thin and ch in " ." else MK_ADV for ch in s) * k - k


def glyph(ch):
    return PICO.get(ch) or PICO["?" if "?" in PICO else " "]


def wp(s):
    """Ink width of a Picopixel string, as Adafruit_GFX::getTextBounds gives it."""
    x, lo, hi = 0, 1 << 15, -1
    for ch in s:
        g = glyph(ch)
        if g["w"] and g["h"]:
            lo, hi = min(lo, x + g["xo"]), max(hi, x + g["xo"] + g["w"] - 1)
        x += g["adv"]
    return hi - lo + 1 if hi >= lo else 0


def adv(s):
    return sum(glyph(ch)["adv"] for ch in s)


class Frame:
    def __init__(self):
        self.img = Image.new("RGB", (W, H), c565(COL_BLACK))
        self.px = self.img.load()
        self.notes = {}

    def dot(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[x, y] = c565(c)

    def put5(self, x, top, s, c, k=1, thin=False):
        """Adafruit GFX drawChar for the classic font: 5 columns, bit j = row j. With
        `thin`, a space advances half a cell and a dot is drawn 2 columns left and
        advances half a cell, so `7 620` and `699.3` read as one number."""
        for ch in s:
            if thin and ch == " ":
                x += MK_THIN_ADV * k
                continue
            dx0 = -2 * k if thin and ch == "." else 0
            o = (ord(ch) if ord(ch) < 256 else ord("?")) * 5
            for i in range(5):
                bits = GLCD[o + i]
                for j in range(8):
                    if (bits >> j) & 1:
                        for dx in range(k):
                            for dy in range(k):
                                self.dot(x + dx0 + i * k + dx, top + j * k + dy, c)
            x += (MK_THIN_ADV if thin and ch == "." else MK_ADV) * k
        return x - k                        # the column after the string's ink

    def put5_right(self, right, top, s, c, k=1, thin=False):
        x = right - w5(s, k, thin) + 1
        self.put5(x, top, s, c, k, thin)
        return x

    def putp(self, x, top, s, c):
        base = top + MK_PICO_ASCENT
        for ch in s:
            g = glyph(ch)
            for r in range(g["h"]):
                row = g["rows"][r]
                for cc in range(g["w"]):
                    if (row >> cc) & 1:
                        self.dot(x + g["xo"] + cc, base + g["yo"] + r, c)
            x += g["adv"]
        return x - adv(s) + wp(s)           # the column after the ink

    def putp_right(self, right, top, s, c):
        x = right - wp(s) + 1
        self.putp(x, top, s, c)
        return x

    def putp_segs(self, x, top, segs):
        """Coloured pieces of one Picopixel string; spacing as if drawn in one go."""
        for s, c in segs:
            self.putp(x, top, s, c)
            x += adv(s)
        return x

    def putp_segs_right(self, right, top, segs):
        full = "".join(s for s, _ in segs)
        return self.putp_segs(right - wp(full) + 1, top, segs)

    def hline(self, y, x0, x1, c):
        for x in range(x0, x1 + 1):
            self.dot(x, y, c)

    def line(self, x0, y0, x1, y1, c):           # Adafruit_GFX::writeLine
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


# ── numbers and words, the design's rules ────────────────────────────────────
MON = ("JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")


def thousands(n):
    """A space as the thousands separator: the design's own rows read `26 186`, and a
    comma is a decimal mark to a EUR reader."""
    return f"{n:,d}".replace(",", " ")


def fmt_value(v, slot):
    """A price or value that fits `slot` characters: one decimal under 1 000, a
    separated integer above, then K and M with one decimal. A sign only when negative."""
    sign, a = ("-" if v < 0 else ""), abs(v)
    s = f"{a:.1f}" if a < 1000 else thousands(int(round(a)))
    if len(s) + len(sign) <= slot:
        return sign + s
    for unit, div in (("K", 1e3), ("M", 1e6), ("B", 1e9)):
        s = f"{a / div:.1f}{unit}"
        if len(s) + len(sign) <= slot:
            break
    return sign + s


def fmt_amount(v, slot=4):
    """A sum of money in the footer: whole units, then K / M with one decimal, or whole K / M when the
    decimal does not fit. (Before 2026-09-15 the decimal was never dropped: 12 345 in four characters
    fell through to `0.0M`; now `12K`. No approved frame had such a value.)"""
    sign, a = ("-" if v < 0 else ""), abs(v)
    s = thousands(int(round(a)))
    if len(s) + len(sign) <= slot:
        return sign + s
    for unit, div in (("K", 1e3), ("M", 1e6)):
        for s in (f"{a / div:.1f}{unit}", f"{a / div:.0f}{unit}"):
            if len(s) + len(sign) <= slot:
                return sign + s
    return sign + s


def pct_round(x):
    """The percent as printed: one decimal under 100, none above."""
    p = x * 100.0
    return round(p, 1) if abs(p) < 100 else float(round(p))


def fmt_pct(x):
    """A change: a sign always, except zero, which has none and is not green."""
    if x is None:
        return "--"
    p = pct_round(x)
    if p == 0:
        return "0.0%"
    if abs(p) < 100:
        return f"{p:+.1f}%"
    return ("+" if p > 0 else "-") + thousands(int(abs(p))) + "%"


def pct_col(x, colors="green_red"):
    if x is None or pct_round(x) == 0:
        return COL_DIM
    up, down = SCHEMES[colors]
    return up if x > 0 else down


def fmt_share(x):
    """An unsigned share: `0.5%`, `54%`."""
    p = x * 100.0
    return f"{p:.1f}%" if p < 10 else f"{p:.0f}%"


def fmt_date(d):
    return f"{d.day:02d} {MON[d.month - 1]}"


def fmt_ym(d):
    """The drawdown's months: YY-MM."""
    return f"{d.year % 100:02d}-{d.month:02d}"


# ── shared pieces ────────────────────────────────────────────────────────────
def draw_tape(f, tokens, seconds):
    """`tokens` = [(exchange, state)], state None when the app has none. Scrolled by
    the frame clock: MK_TAPE_FPS px per second, cycling over the tape's length."""
    segs = []
    for name, state in tokens:
        segs += [(name + " ", COL_DIM), (state or "--", STATE_COL.get(state, COL_DIM)), (MK_TAPE_SEP, COL_DIM)]
    cycle = adv("".join(s for s, _ in segs))
    x = -((seconds * MK_TAPE_FPS) % cycle)
    while x < W:
        f.putp_segs(x, MK_Y_TAPE, segs)
        x += cycle
    f.hline(MK_Y_TAPE_RULE, 0, W - 1, COL_RULE)
    f.notes["tape"] = "  ".join(f"{n} {s or '--'}" for n, s in tokens)


def head(f, title, tag=None, cur=None):
    x = f.put5(MK_X_LEFT, MK_Y_HEAD, title, COL_AMBER)
    y = MK_Y_HEAD + MK_PICO_DROP
    if tag:
        x = f.putp(x + MK_GAP, y, tag, COL_AMBER)
    if cur:
        x = f.putp(x + MK_GAP, y, cur, COL_DIM)
    f.notes["heading"] = " ".join(s for s in (title, tag, cur) if s)
    return x


def asof_text(ctx, label=True):
    if ctx["stale"]:
        return "STALE " + fmt_date(ctx["asof"]), COL_AMBER
    return ("AS OF " if label else "") + fmt_date(ctx["asof"]), COL_DIM


def asof_slot(f, top, ctx, right=MK_X_RIGHT, label=True):
    if ctx.get("error"):                  # DATA ERR: nothing is stored, so there is no date
        return right
    s, c = asof_text(ctx, label)
    f.notes["asof"] = s
    return f.putp_right(right, top, s, c)


def feed_label(lag_s):
    """LIVE while the quote is less than MK_LIVE_MAX_S behind the clock, else the delay badge and the
    whole minutes behind."""
    if lag_s < MK_LIVE_MAX_S:
        return "LIVE", COL_GREEN
    return f"{MK_DELAY_BADGE}{int(lag_s // 60)}", COL_DIM


def live_segs(state, q, asof, lag_s=0, colors="green_red"):
    if state == "OPEN":
        word, col = feed_label(lag_s)
        return [(word + " ", col), (fmt_pct(q["dayChg"]), pct_col(q["dayChg"], colors))]
    return [("CLOSE ", COL_DIM), (fmt_date(asof), COL_DIM)]


def ann_segs(ann, colors):
    """`ANN +8.6%`: the word dim, the value in its sign colour; nothing under a one-year window."""
    return [(LBL_ANN + " ", COL_DIM), (fmt_pct(ann), pct_col(ann, colors))] if ann is not None else []


def draw_error(f, ctx):
    f.put5((W - w5("DATA ERR")) // 2, MK_Y_ERR, "DATA ERR", COL_RED)
    why = ctx.get("err", "NOTHING STORED")
    f.putp((W - wp(why)) // 2, MK_Y_ERR_WHY, why, COL_DIM)
    f.notes["error"] = "DATA ERR " + why


def curve_xy(x0, y0, w, h, u16, n_valid=None):
    n = len(u16)
    m = n if n_valid is None else max(0, min(n, n_valid))
    out = []
    for i in range(m):
        x = x0 + ((2 * i * (w - 1) + (n - 1)) // (2 * (n - 1)) if n > 1 else 0)
        y = y0 + (h - 1) - (u16[i] * (h - 1) + 32767) // 65535
        out.append((x, y))
    return out


def draw_curve(f, x0, y0, w, h, u16, col, n_valid=None, dotted=False):
    """128 uint16 points across w columns, the panel's own mapping. A dotted line
    keeps every other column and does not join the dots."""
    pts = curve_xy(x0, y0, w, h, u16, n_valid)
    if not pts:
        return
    if dotted:
        for x, y in pts:
            if (x - x0) % 2 == 0:
                f.dot(x, y, col)
        return
    f.dot(*pts[0], col)
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        f.line(ax, ay, bx, by, col)


def draw_ticks(f, x0, y0, w, h):
    for tx in MK_TICKS:
        for k in range(MK_TICK_H):
            f.dot(x0 + tx * (w - 1) // (W - 1), y0 + h - 1 - k, COL_DIM)


def slot_index(frm, to, day, n=M.POINTS):
    """The downsampled point that first shows `day`: market_ref.downsample() gives point i the last
    bar at or before (i + 1) of n equal steps of the window, so a bar at offset o days of a span of
    S days is first shown by point ceil(o n / S) - 1."""
    span = (to - frm).days
    if span <= 0:
        return n - 1
    o = min(max((day - frm).days, 0), span)
    return max(0, -(-o * n // span) - 1)


# ── the pages ────────────────────────────────────────────────────────────────
def page_markets(ctx):
    """Heading MARKETS + window tag, AS OF at the right; the primary index: mnemonic,
    window change, last at 2x, a 48x16 sparkline, LIVE / CLOSE; three secondary rows."""
    f = Frame()
    colors = ctx.get("colors", "green_red")
    draw_tape(f, ctx["tape"], ctx["seconds"])
    head(f, "MARKETS", ctx["preset"])
    asof_slot(f, MK_Y_HEAD + MK_PICO_DROP, ctx)
    if ctx.get("error"):
        draw_error(f, ctx)
        return f
    pri, rest = ctx["indices"][0], ctx["indices"][1:]
    p = pri["p"]
    f.put5(MK_X_LEFT, MK_Y_MKT_SYM, pri["mn"], COL_WHITE)
    f.put5_right(MK_X_MKT_CHG_R, MK_Y_MKT_SYM, fmt_pct(p["chg"]), pct_col(p["chg"], colors), thin=True)
    big = fmt_value(p["last"], MK_BIG_CHARS_MKT)
    f.put5(MK_X_LEFT, MK_Y_MKT_BIG, big, COL_WHITE, MK_BIG, thin=True)
    draw_curve(f, MK_X_SPARK, MK_Y_SPARK, MK_SPARK_W, MK_SPARK_H, M.unpack_pts(p["pts"]), COL_WHITE)
    lag_s = ctx.get("lag_s", LAG_REALTIME_S)
    segs = live_segs(pri["state"], pri["q"], ctx["asof"], lag_s, colors)
    f.putp_segs_right(MK_X_RIGHT, MK_Y_MKT_LIVE, segs)
    f.notes["primary"] = {"mn": pri["mn"], "last": big, "chg": fmt_pct(p["chg"]), "label": "".join(s for s, _ in segs),
                          "window": f"{p['from']}..{p['to']}", "state": pri["state"],
                          "lag_s": lag_s if pri["state"] == "OPEN" else None}
    rows = []
    for k, idx in enumerate(rest[:3]):
        y = MK_Y_MKT_ROW0 + k * MK_MKT_ROW_H
        q = idx["p"]
        val, chg = fmt_value(q["last"], 7), fmt_pct(q["chg"])
        f.putp(MK_X_LEFT, y, idx["mn"], COL_DIM)
        f.putp_right(MK_X_MKT_VAL_R, y, val, COL_DIM)
        f.putp_right(MK_X_RIGHT, y, chg, pct_col(q["chg"], colors))
        rows.append(f"{idx['mn']} {val} {chg}")
    f.notes["rows"] = rows
    return f


def page_ticker(ctx):
    """Heading: symbol, window tag, currency, and ANN at the right from a one-year window. The last at
    2x; the window change on the top line, LIVE / D / CLOSE on the bottom line; the window chart with
    three ticks; the session strip; footer HI / LO and AS OF."""
    f = Frame()
    colors = ctx.get("colors", "green_red")
    draw_tape(f, ctx["tape"], ctx["seconds"])
    x_head = head(f, ctx["sym"], ctx["preset"], ctx["cur"])
    if ctx.get("error"):
        draw_error(f, ctx)
        return f
    p = ctx["p"]
    ann = ann_segs(ctx.get("ann"), colors)
    if ann:
        x_ann = MK_X_RIGHT - wp("".join(s for s, _ in ann)) + 1
        assert x_ann - x_head >= MK_GAP, (ctx["sym"], x_head, x_ann)
        f.putp_segs_right(MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, ann)
    big = fmt_value(p["last"], MK_BIG_CHARS)
    f.put5(MK_X_LEFT, MK_Y_BLOCK, big, COL_WHITE, MK_BIG, thin=True)
    f.put5_right(MK_X_RIGHT, MK_Y_BLOCK, fmt_pct(p["chg"]), pct_col(p["chg"], colors), thin=True)
    lag_s = ctx.get("lag_s", LAG_REALTIME_S)
    segs = live_segs(ctx["state"], ctx["q"], ctx["asof"], lag_s, colors)
    f.putp_segs_right(MK_X_RIGHT, MK_Y_BLOCK2 + MK_PICO_DROP, segs)
    draw_ticks(f, 0, MK_Y_TK_CHART, W, MK_TK_CHART_H)
    draw_curve(f, 0, MK_Y_TK_CHART, W, MK_TK_CHART_H, M.unpack_pts(p["pts"]), COL_WHITE)
    draw_ticks(f, 0, MK_Y_TK_SESS, W, MK_TK_SESS_H)
    if ctx.get("session"):
        u16, n_valid = ctx["session"]
        draw_curve(f, 0, MK_Y_TK_SESS, W, MK_TK_SESS_H, u16, COL_DIM, n_valid)
    foot = f"HI {fmt_amount(p['hi'], 7)}  LO {fmt_amount(p['lo'], 7)}"
    f.putp(MK_X_LEFT, MK_Y_FOOT, foot, COL_DIM)
    asof_slot(f, MK_Y_FOOT, ctx)
    f.notes.update({"last": big, "chg": fmt_pct(p["chg"]), "label": "".join(s for s, _ in segs), "footer": foot,
                    "window": f"{p['from']}..{p['to']}", "state": ctx["state"],
                    "lag_s": lag_s if ctx["state"] == "OPEN" else None,
                    "ann": "".join(s for s, _ in ann) if ann else "-", "colors": colors,
                    "session": ("synthetic, %d of 128 slots" % ctx["session"][1]) if ctx.get("session") else "none"})
    return f


def page_portfolio(ctx):
    """Heading PORTFOLIO + currency, mode and window tag at the right. The value at 2x, the window's
    TWR beside it with its label, and under that ANN from a one-year window; the value line bright,
    the no-dividend line dim, the benchmark dim and dotted; footer DIV, TER~, CASH and the as-of date
    (bare: the footer has no room for the AS OF label). With contributions the footer is XIRR since
    inception and DIV: TER~ and CASH give their room (a four-digit XIRR and six-figure amounts overflow
    the footer by 8 columns with TER~ kept; layout_budget() measures it). STALE keeps XIRR alone.

    The drawdown stop (ctx["stop"] == "mdd", a click): the footer is MDD with the peak and trough
    months and the recovery month or NO REC; the fall from the peak's point to the trough's is red on
    the value line, with a red bracket under the chart between their columns; the as-of date, pushed
    out of the footer, takes ANN's place under the TWR."""
    f = Frame()
    colors = ctx.get("colors", "green_red")
    draw_tape(f, ctx["tape"], ctx["seconds"])
    head(f, "PORTFOLIO", None, ctx["cur"])
    f.putp_right(MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, f"{ctx['mode'].upper()}  {ctx['preset']}", COL_AMBER)
    if ctx.get("error"):
        draw_error(f, ctx)
        return f
    p, r = ctx["p"], ctx["r"]
    mdd_stop = ctx.get("stop") == "mdd"
    big = fmt_value(p["value"], MK_BIG_CHARS_PF)
    f.put5(MK_X_LEFT, MK_Y_BLOCK, big, COL_WHITE, MK_BIG, thin=True)
    twr = fmt_pct(r["twr"])
    x_chg = f.put5_right(MK_X_RIGHT, MK_Y_BLOCK, twr, pct_col(r["twr"], colors), thin=True)
    f.putp_right(x_chg - MK_GAP - 1, MK_Y_BLOCK + MK_PICO_DROP, LBL_TWR, COL_DIM)
    ann = [] if mdd_stop else ann_segs(r["ann"], colors)
    if mdd_stop:
        asof_slot(f, MK_Y_BLOCK2 + MK_PICO_DROP, ctx, label=False)
    elif ann:
        f.putp_segs_right(MK_X_RIGHT, MK_Y_BLOCK2 + MK_PICO_DROP, ann)
    draw_ticks(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H)
    if "bench" in p and ctx.get("show_bench", True):
        draw_curve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, M.unpack_pts(p["bench"]), COL_DIM, dotted=True)
    if ctx.get("show_px", True):
        draw_curve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, M.unpack_pts(p["px"]), COL_DIM)
    u16 = M.unpack_pts(p["pts"])
    draw_curve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, u16, COL_WHITE)
    dd = r["drawdown"]
    extra = {}
    if mdd_stop:
        down = SCHEMES[colors][1]
        if dd["trough"] is not None:
            frm, to = dt.date.fromisoformat(p["from"]), dt.date.fromisoformat(p["to"])
            i0, i1 = slot_index(frm, to, dd["peak"]), slot_index(frm, to, dd["trough"])
            pts = curve_xy(0, MK_Y_PF_CHART, W, MK_PF_CHART_H, u16)
            f.dot(*pts[i0], down)
            for (ax, ay), (bx, by) in zip(pts[i0:i1], pts[i0 + 1:i1 + 1]):
                f.line(ax, ay, bx, by, down)
            xa, xb = pts[i0][0], pts[i1][0]
            f.hline(MK_Y_MDD_BAR, xa, xb, down)
            for x in (xa, xb):
                for k in range(MK_MDD_TICK_H):
                    f.dot(x, MK_Y_MDD_BAR - k, down)
            rec = f"{LBL_REC} {fmt_ym(dd['recovery'])}" if dd["recovery"] else LBL_NO_REC
            segs = [(LBL_MDD + " ", COL_DIM), (fmt_pct(dd["mdd"]), pct_col(dd["mdd"], colors)),
                    (f"  {fmt_ym(dd['peak'])}>{fmt_ym(dd['trough'])}  {rec}", COL_DIM)]
            extra["marks"] = {"points": [i0, i1], "columns": [xa, xb]}
        else:
            segs = [(LBL_MDD + " ", COL_DIM), (fmt_pct(0.0), COL_DIM)]
        foot = "".join(s for s, _ in segs)
        assert MK_X_LEFT + wp(foot) - 1 <= MK_X_RIGHT, foot
        f.putp_segs(MK_X_LEFT, MK_Y_FOOT, segs)
    else:
        div, ter = f"DIV {fmt_amount(p['div'])}", f"TER~ {fmt_amount(p['terDrag'])}"
        if r["xirr"] is not None:
            segs = [(LBL_XIRR + " ", COL_DIM), (fmt_pct(r["xirr"]), pct_col(r["xirr"], colors)),
                    ("" if ctx["stale"] else "  " + div, COL_DIM)]
        else:
            items = [div, ter, f"CASH {fmt_share(p['cash'])}"]
            segs = [("  ".join(items[:2] if ctx["stale"] else items), COL_DIM)]   # STALE dd MON needs the CASH item's room
        foot = "".join(s for s, _ in segs)
        f.putp_segs(MK_X_LEFT, MK_Y_FOOT, segs)
        x_date = asof_slot(f, MK_Y_FOOT, ctx, label=False)
        assert x_date - (MK_X_LEFT + wp(foot)) >= MK_GAP, (foot, x_date)
    f.notes.update({"value": big, "chg": twr, "twr": f"{LBL_TWR} {twr}", "footer": foot,
                    "ann": "".join(s for s, _ in ann) if ann else "-",
                    "xirr": fmt_pct(r["xirr"]) if r["xirr"] is not None else "-",
                    "window": f"{p['from']}..{p['to']}",
                    "sinceStart": fmt_pct(p["sinceStart"]), "cagr": fmt_pct(p["cagr"]) if p["cagr"] is not None else "-",
                    "mdd": fmt_pct(p["mdd"]), "mdd_dates": {k: str(dd[k]) if dd[k] else None for k in ("peak", "trough", "recovery")},
                    "lines": [k for k in ("pts", "px", "bench") if k in p], "bench": ctx.get("bench_note", "-"),
                    "stop": ctx.get("stop", "-"), "colors": colors})
    f.notes.update(extra)
    return f


def page_holdings(ctx):
    """Heading HOLDINGS n/m + window tag, the mode at the right. Rows: symbol (5x7),
    target > now (Picopixel), return since entry (5x7, signed); CASH last. Footer:
    the column legend at the left, AS OF at the right."""
    f = Frame()
    draw_tape(f, ctx["tape"], ctx["seconds"])
    h = ctx["h"]
    rows = [(r["sym"], r["tgt"], r["now"], r["ret"]) for r in h["rows"]] + [("CASH", h["cash"]["tgt"], h["cash"]["now"], None)]
    pages = max(1, math.ceil(len(rows) / MK_HD_ROWS))
    page = min(ctx.get("page", 0), pages - 1)
    colors = ctx.get("colors", "green_red")
    head(f, f"HOLDINGS {page + 1}/{pages}", ctx["preset"])
    f.putp_right(MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, ctx["mode"].upper(), COL_AMBER)
    if ctx.get("error"):
        draw_error(f, ctx)
        return f
    drawn = []
    for k, (sym, tgt, now, ret) in enumerate(rows[page * MK_HD_ROWS:(page + 1) * MK_HD_ROWS]):
        y = MK_Y_HD_ROW0 + k * MK_HD_ROW_H
        mid = f"{int(round(tgt))}>{int(round(now))}%"
        f.put5(MK_X_LEFT, y, sym, COL_WHITE if sym != "CASH" else COL_DIM)
        f.putp_right(MK_X_HD_MID_R, y + MK_PICO_DROP, mid, COL_DIM)
        if ret is not None:
            f.put5_right(MK_X_RIGHT, y, fmt_pct(ret), pct_col(ret, colors), thin=True)
        drawn.append(f"{sym} {mid} {fmt_pct(ret) if ret is not None else ''}".rstrip())
    f.putp(MK_X_LEFT, MK_Y_FOOT, "TGT>NOW", COL_DIM)
    asof_slot(f, MK_Y_FOOT, ctx)
    f.notes.update({"rows": drawn, "page": f"{page + 1}/{pages}"})
    return f


# ── the frames ───────────────────────────────────────────────────────────────
INCEPTION = dt.date(2000, 1, 1)
T_OPEN = dt.datetime(2026, 9, 14, 18, 0, tzinfo=PARIS)      # 12:00 New York: NYSE and Nasdaq OPEN, XETRA in its POST session
T_POST = dt.datetime(2026, 9, 14, 22, 30, tzinfo=PARIS)     # 16:30 New York: POST; Europe closed
T_CLOSED = dt.datetime(2026, 9, 15, 8, 40, tzinfo=PARIS)    # when the samples were saved: XETRA in PRE, the rest closed
T_STALE = dt.datetime(2026, 9, 21, 8, 40, tzinfo=PARIS)     # five trading days after the as-of
T_EU_OPEN = dt.datetime(2026, 9, 14, 11, 0, tzinfo=PARIS)   # XETRA and Euronext OPEN, NYSE and Nasdaq PRE

# Quote lags behind the clock. A real-time feed; and Yahoo's delay for ^GDAXI, ^FCHI and ^FTSE, 902-906 s on
# every probe on 2026-09-15 09:19-09:21 CEST (docs/18, "Live quotes"). Both ends make 15 whole minutes; the
# smaller is drawn. Yahoo's US delay is not measured yet (docs/18: at 15:35 CEST), so the ^GSPC frames come
# in both states and which is the default waits for that measurement.
LAG_REALTIME_S = 0
LAG_YAHOO_EU_S = (902, 906)
CONTRIB_EXAMPLE = 1000.0    # the contributions frame: this much a year, in the portfolio currency, on the inception's anniversary
BENCH_NOTES = {True: "^SP500TR total return, in EUR, growth of 10 000 from the inception",
               False: "^GSPC price, USD, from 10 000 (the approved line): no ^SP500TR sample, Yahoo answered 429 on 2026-09-15"}


def meta_day_change(meta):
    """The day change a monthly sample cannot give, from Yahoo's own meta: fulldayChange over the
    previous close, fulldayPrice - fulldayChange. On ^GSPC it matches the daily sample's last two
    closes to 1e-11."""
    c, price = meta.get("fulldayChange"), meta.get("fulldayPrice")
    return c / (price - c) if c is not None and price is not None and price != c else None


def window_ann(s, preset, asof):
    """ANN of a price series over the page's window (index_payload's window): growth last / first."""
    end = min(asof, s.last)
    wd, wv = M.window_series(s.dates, s.closes, M.window_start(preset, end, INCEPTION, s.dates), end)
    return PR.annualised(wv[-1] / wv[0], wd[0], wd[-1]) if wv and wv[0] else None


def synthetic_session(daily, prev, last, n_bars=MK_SESSION_MIN // 5):
    """SYNTHETIC. No 5-minute sample exists (Yahoo: 429 on 2026-09-15 09:30). The shape
    replays the last `n_bars` daily returns of `daily`, damped 8x, one per 5-minute bar,
    then bent so the path starts at `prev` and ends at `last`. A stand-in for the layout."""
    c = daily.closes
    rets = [c[i] / c[i - 1] - 1.0 for i in range(len(c) - n_bars, len(c))]
    p = [prev]
    for r in rets:
        p.append(p[-1] * (1.0 + r / 8.0))
    k = last / p[-1]
    return [v * k ** (i / n_bars) for i, v in enumerate(p)]


def session_strip(path, elapsed_min):
    """The 128 slots of the session strip: slot j covers up to minute 390 (j+1) / 128 and
    shows the last 5-minute bar in it; slots after `elapsed_min` are not drawn yet."""
    vals, n_valid = [], 0
    for j in range(M.POINTS):
        minute = MK_SESSION_MIN * (j + 1) / M.POINTS
        vals.append(path[min(len(path) - 1, int(minute // 5))])
        n_valid += minute <= elapsed_min
    lo, hi, b64 = M.pack_pts(vals[:n_valid] if n_valid else vals)
    u16 = M.unpack_pts(b64) + [0] * (M.POINTS - n_valid) if n_valid else [0] * M.POINTS
    return u16, n_valid


def elapsed_minutes(session, now_utc):
    local = now_utc + dt.timedelta(seconds=session["gmtoffset"])
    t = local.hour * 3600 + local.minute * 60
    a, b = session["regular"]
    return max(0, min(b, t) - a) // 60 if M.exchange_state(session, now_utc) == "OPEN" else MK_SESSION_MIN


def scaled(base, sym, cur, power, sign=1.0):
    """A synthetic series: the base's normalised path raised to `power`, times its first
    close, optionally negated. Dividends stay as they are (per share)."""
    c0 = base.closes[0]
    return M.Series(sym, cur, list(base.dates), [sign * c0 * (c / c0) ** power for c in base.closes],
                    list(base.dividends) if sign > 0 else [], base.ter, base.meta)


def build_frames():
    S = M.load_samples()
    series, fx, daily = S["series"], S["fx"], S["daily"]
    indices = S["indices"]
    asof = M.as_of([s for _, _, s in indices])
    tape_src = [("NYSE", series["VOO"]), ("NASDAQ", series["AAPL"]), ("XETRA", series["^GDAXI"]), ("EURONEXT", series["^FCHI"])]
    sessions = {n: M.session_from_meta(s.meta) for n, s in tape_src}

    def base(now, stale_asof=asof, error=False):
        utc = now.astimezone(dt.timezone.utc)
        tape = [(n, None if error else M.exchange_state(ses, utc)) for n, ses in sessions.items()]
        return {"tape": tape, "seconds": now.hour * 3600 + now.minute * 60 + now.second, "now": now,
                "asof": stale_asof, "stale": M.is_stale(stale_asof, now.date()), "error": error}

    def idx_ctx(sym, mn, s, preset, now, live_series=None):
        utc = now.astimezone(dt.timezone.utc)
        q = M.quote(live_series or s)
        if live_series is None:                  # a monthly sample's last two bars are a month apart
            q["dayChg"] = meta_day_change(s.meta)
        return {"mn": mn, "p": M.index_payload(s, preset, asof, INCEPTION, mn), "q": q,
                "state": M.exchange_state(M.session_from_meta(s.meta), utc)}

    def markets(preset, now, primary=0, error=False, lag_s=LAG_REALTIME_S, colors="green_red"):
        ctx = base(now, error=error)
        order = indices[primary:] + indices[:primary]
        ctx.update({"preset": preset, "lag_s": lag_s, "colors": colors})
        ctx["indices"] = [idx_ctx(sym, mn, s, preset, now, daily if sym == "^GSPC" else None) for sym, mn, s in order]
        return ctx

    def ticker(s, preset, now, live_series=None, error=False, session=True, lag_s=LAG_REALTIME_S, colors="green_red"):
        ctx = base(now, stale_asof=min(asof, s.last), error=error)
        utc = now.astimezone(dt.timezone.utc)
        ses = M.session_from_meta(s.meta)
        q = M.quote(live_series or s)
        ctx.update({"sym": s.sym, "cur": s.currency, "preset": preset, "p": M.index_payload(s, preset, asof, INCEPTION),
                    "q": q, "state": M.exchange_state(ses, utc), "lag_s": lag_s, "colors": colors})
        if ctx["p"]:
            ctx["ann"] = window_ann(s, preset, asof)
        if session and ctx["p"]:
            # the previous close of a monthly instrument is unknown: bend the synthetic path by the S&P's day change
            prev = q["prev"] if live_series else s.closes[-1] / (1.0 + M.quote(daily)["dayChg"])
            ctx["session"] = session_strip(synthetic_session(daily, prev, s.closes[-1]), elapsed_minutes(ses, utc))
        return ctx

    assert len({feed_label(lag)[0] for lag in LAG_YAHOO_EU_S}) == 1, LAG_YAHOO_EU_S

    # the real portfolio: VOO alone (market_ref.PREVIEW_PORTFOLIO)
    P = M.run_preview_portfolio(samples=S)
    cfg = P["cfg"]
    have_tr = P["bench_tr"] is not None
    bench, bench_note = (P["bench_tr"] if have_tr else P["bench"]), BENCH_NOTES[have_tr]
    # the same portfolio with CONTRIB_EXAMPLE a year, for TWR against XIRR
    contrib_cfg = M.Config(cfg.capital, cfg.currency, cfg.inception, cfg.positions, CONTRIB_EXAMPLE, "year")
    contrib_runs = {m: M.simulate(contrib_cfg, series, fx, P["days"], "hold", m == "hold", M.calendar_years)
                    for m in ("hold", "hold_nodiv")}

    def portfolio(runs, c, bench, mode, preset, now, bench_note="-", **extra):
        ctx = base(now)
        L = runs[mode]
        ctx.update({"mode": mode, "cur": c.currency, "preset": preset, "bench_note": bench_note if bench else "-",
                    "p": M.portfolio_payload(c, L, runs[mode + "_nodiv"], bench, preset),
                    "r": PR.window_figures(c, L, preset)})
        if not any(PR.ledger_flows(L)):          # nothing flows: the TWR is the payload's chg itself
            assert ctx["r"]["twr"] == ctx["p"]["chg"], (mode, preset)
        ctx.update(extra)
        return ctx

    def holdings(runs, c, ser, mode, preset, now, page=0):
        ctx = base(now)
        ctx.update({"mode": mode, "preset": preset, "page": page, "h": M.holdings_payload(c, runs[mode], ser, fx)})
        return ctx

    # SYNTH01X..SYNTH14X: an example allocation (market_ref.EXAMPLE_ALLOCATION) on transformed
    # real series, to fit 8-character symbols, a four-digit return and the 15-row pagination.
    bases = [("^FCHI", "EUR"), ("^GDAXI", "EUR"), ("VOO", "USD"), ("^GSPC", "EUR"), ("^IXIC", "EUR"), ("AAPL", "USD")]
    synth = {}
    for k, (name, _) in enumerate(M.EXAMPLE_ALLOCATION):
        bsym, cur = bases[k % len(bases)]
        power = 1.6 if bsym == "^IXIC" and k == 4 else 0.7 + 0.1 * (k % 7)
        synth[name] = scaled(series[bsym], name, cur, power)
    synth_cfg = M.Config(10000.0, "EUR", INCEPTION, [M.Position(name, w) for name, w in M.EXAMPLE_ALLOCATION])
    synth_days = M.timeline(list(synth.values()), INCEPTION)
    synth_runs = {m: M.simulate(synth_cfg, synth, fx, synth_days, m.split("_")[0], not m.endswith("_nodiv"), M.calendar_years)
                  for m in ("hold", "hold_nodiv", "rebal", "rebal_nodiv")}
    if have_tr:
        synth_bench = M.benchmark_in(S["bench_tr"], synth_days, 10000.0, INCEPTION, fx, "EUR")
    else:
        synth_bench = M.benchmark(indices[0][2], synth_days, 10000.0, INCEPTION)
    # a position without dividends: the S&P path as an EUR fund
    nodiv = {"SYNTH00X": scaled(series["^GSPC"], "SYNTH00X", "EUR", 1.0)}
    nodiv_cfg = M.Config(10000.0, "EUR", INCEPTION, [M.Position("SYNTH00X", 100.0)])
    nodiv_days = M.timeline(list(nodiv.values()), INCEPTION)
    nodiv_runs = {m: M.simulate(nodiv_cfg, nodiv, fx, nodiv_days, "hold", m == "hold", M.calendar_years) for m in ("hold", "hold_nodiv")}
    # worst-case tickers
    neg = scaled(series["^IXIC"], "ABCDEFGH", "USD", 1.0, sign=-5.0)                       # a six-figure negative value
    ix = series["^IXIC"]
    growth = scaled(ix, "GROWTH8X", "USD", math.log(13.45) / math.log(ix.closes[-1] / ix.closes[0]))   # ends at +1 245 %

    frames = [
        ("markets_max_open", "MARKETS, MAX, 12:00 New York: SPX LIVE", page_markets, markets("MAX", T_OPEN)),
        ("markets_max_open_d15", "MARKETS, MAX, 12:00 New York: SPX behind the delay badge (a variant: the US lag is not measured)",
         page_markets, markets("MAX", T_OPEN, lag_s=LAG_YAHOO_EU_S[0])),
        ("markets_5y_closed", "MARKETS, 5Y, 08:40 Paris: CLOSE, XETRA in PRE", page_markets, markets("5Y", T_CLOSED)),
        ("markets_1y_dax", "MARKETS, 1Y, DAX primary, 22:30 Paris: NYSE POST", page_markets, markets("1Y", T_POST, primary=3)),
        ("markets_1y_dax_d15", "MARKETS, 1Y, DAX primary, 11:00 Paris: Yahoo's measured European lag, D15", page_markets,
         markets("1Y", T_EU_OPEN, primary=3, lag_s=LAG_YAHOO_EU_S[0])),
        ("markets_stale", "MARKETS, YTD, a week later: STALE", page_markets, markets("YTD", T_STALE)),
        ("markets_err", "MARKETS: DATA ERR, nothing stored", page_markets, markets("MAX", T_CLOSED, error=True)),
        ("ticker_voo_max", "TICKER VOO, MAX, CLOSE", page_ticker, ticker(series["VOO"], "MAX", T_CLOSED)),
        ("ticker_voo_5y", "TICKER VOO, 5Y, 22:30 Paris: POST", page_ticker, ticker(series["VOO"], "5Y", T_POST)),
        ("ticker_voo_ytd", "TICKER VOO, YTD, CLOSE", page_ticker, ticker(series["VOO"], "YTD", T_CLOSED)),
        ("ticker_gspc_1y_open", "TICKER ^GSPC (daily sample), 1Y, LIVE at 12:00 New York", page_ticker, ticker(daily, "1Y", T_OPEN, live_series=daily)),
        ("ticker_gspc_1y_open_d15", "TICKER ^GSPC, 1Y, 12:00 New York: D15 (a variant: the US lag is not measured)", page_ticker,
         ticker(daily, "1Y", T_OPEN, live_series=daily, lag_s=LAG_YAHOO_EU_S[0])),
        ("ticker_gspc_1y_open_bluered", "TICKER ^GSPC, 1Y, LIVE: the blue/red colour scheme", page_ticker,
         ticker(daily, "1Y", T_OPEN, live_series=daily, colors="blue_red")),
        ("ticker_aapl_short", "worst case: short history (AAPL, 5 years weekly) on MAX", page_ticker, ticker(series["AAPL"], "MAX", T_CLOSED)),
        ("ticker_worst_neg", "worst case: 8-char symbol, six-figure negative (synthetic)", page_ticker, ticker(neg, "MAX", T_CLOSED, session=False)),
        ("ticker_worst_pct", "worst case: +1 245 % (synthetic)", page_ticker, ticker(growth, "MAX", T_POST, session=False)),
        ("ticker_err", "TICKER: DATA ERR", page_ticker, dict(ticker(series["VOO"], "MAX", T_CLOSED, error=True), state="CLOSED")),
        ("portfolio_hold_max", "PORTFOLIO HOLD MAX: " + ", ".join(f"{p.sym} {p.w:g}" for p in cfg.positions), page_portfolio,
         portfolio(P["runs"], cfg, bench, "hold", "MAX", T_CLOSED, bench_note)),
        ("portfolio_hold_max_mdd", "PORTFOLIO HOLD MAX, the drawdown stop (a click): MDD with its dates", page_portfolio,
         portfolio(P["runs"], cfg, bench, "hold", "MAX", T_CLOSED, bench_note, stop="mdd")),
        ("portfolio_rebal_max", "PORTFOLIO REBAL MAX", page_portfolio, portfolio(P["runs"], cfg, bench, "rebal", "MAX", T_CLOSED, bench_note)),
        ("portfolio_hold_5y", "PORTFOLIO HOLD 5Y", page_portfolio, portfolio(P["runs"], cfg, bench, "hold", "5Y", T_CLOSED, bench_note)),
        ("portfolio_rebal_5y", "PORTFOLIO REBAL 5Y", page_portfolio, portfolio(P["runs"], cfg, bench, "rebal", "5Y", T_CLOSED, bench_note)),
        ("portfolio_contrib_max", f"PORTFOLIO HOLD MAX, VOO with {thousands(int(CONTRIB_EXAMPLE))} EUR a year: TWR, ANN, XIRR", page_portfolio,
         portfolio(contrib_runs, contrib_cfg, bench, "hold", "MAX", T_CLOSED, bench_note)),
        ("portfolio_synth14_hold_max", "PORTFOLIO HOLD MAX, 14 synthetic positions, an example allocation", page_portfolio,
         portfolio(synth_runs, synth_cfg, synth_bench, "hold", "MAX", T_CLOSED, BENCH_NOTES[have_tr])),
        ("portfolio_synth14_hold_max_mdd", "PORTFOLIO HOLD MAX, the example allocation, the drawdown stop", page_portfolio,
         portfolio(synth_runs, synth_cfg, synth_bench, "hold", "MAX", T_CLOSED, BENCH_NOTES[have_tr], stop="mdd")),
        ("portfolio_synth14_rebal_5y", "PORTFOLIO REBAL 5Y, the same 14", page_portfolio,
         portfolio(synth_runs, synth_cfg, synth_bench, "rebal", "5Y", T_POST, BENCH_NOTES[have_tr])),
        ("portfolio_nodiv", "worst case: a position without dividends (DIV 0, the PX line hidden under V)", page_portfolio,
         portfolio(nodiv_runs, nodiv_cfg, None, "hold", "10Y", T_CLOSED)),
        ("portfolio_stale", "PORTFOLIO HOLD MAX, a week later: STALE", page_portfolio, portfolio(P["runs"], cfg, bench, "hold", "MAX", T_STALE, bench_note)),
        ("holdings_hold", "HOLDINGS HOLD, the real portfolio", page_holdings, holdings(P["runs"], cfg, series, "hold", "MAX", T_CLOSED)),
        ("holdings_synth14_p1", "HOLDINGS 1/4, 14 synthetic positions + CASH", page_holdings, holdings(synth_runs, synth_cfg, synth, "rebal", "MAX", T_CLOSED, page=0)),
        ("holdings_synth14_p4", "HOLDINGS 4/4, the last page with CASH", page_holdings, holdings(synth_runs, synth_cfg, synth, "hold", "MAX", T_CLOSED, page=3)),
        ("holdings_synth14_p2_stale", "HOLDINGS 2/4, STALE", page_holdings, holdings(synth_runs, synth_cfg, synth, "hold", "5Y", T_STALE, page=1)),
    ]
    return frames, {"asof": asof.isoformat(), "config": [(p.sym, p.w) for p in cfg.positions],
                    "synth_bases": {name: bases[k % len(bases)][0] for k, (name, _) in enumerate(M.EXAMPLE_ALLOCATION)},
                    "example_allocation": list(M.EXAMPLE_ALLOCATION),
                    "listing": {s.sym: s.first.isoformat() for s in series.values()},
                    "bench": bench_note, "contrib_example": CONTRIB_EXAMPLE,
                    "lags_s": {"realtime": LAG_REALTIME_S, "yahoo_eu_measured": list(LAG_YAHOO_EU_S)}}


def layout_budget():
    """The slots the six decisions added, filled with their widest strings: the blank columns left
    between neighbours (1 or more: they do not touch). An overlap raises. Kept in frames.json's _meta."""
    capital = max("ABCDEFGHIJKLMNOPQRSTUVWXYZ", key=lambda ch: (glyph(ch)["adv"], wp(ch)))
    digit = max("0123456789", key=lambda ch: (glyph(ch)["adv"], wp(ch)))
    date = f"{digit * 2} {max(MON, key=wp)}"
    ym = f"{digit * 2}-{digit * 2}"
    pct5 = max(("-99.9%", "-100%", "+8 888%"), key=lambda s: w5(s, thin=True))            # 5x7 digits share a width
    pctp = max(("-99.9%", "-100%", f"+{digit} {digit * 3}%", f"-{digit * 2}.{digit}%"), key=wp)
    tag = max(M.PRESETS + ("WTD", "MTD", "1M", "3M", "6M"), key=wp)
    amount = max({fmt_amount(v) for v in (999, 9_949, 99_499, 999_499, 9_949_999, 99_499_999)}, key=wp)

    def slot_end(chars):                             # the last column of a 2x slot of `chars` characters
        return MK_X_LEFT + chars * MK_ADV * MK_BIG - MK_BIG - 1

    def left_of(s, pico=True):                       # the first column of `s` right-aligned at MK_X_RIGHT
        return MK_X_RIGHT - (wp(s) if pico else w5(s, thin=True)) + 1

    big_pf = max(w5(fmt_value(v, MK_BIG_CHARS_PF), MK_BIG, True) for v in (-999.94, 99_999, -99_999, 999_949, -999_949, 9.9e6, -9.9e6))
    assert MK_X_LEFT + big_pf - 1 <= slot_end(MK_BIG_CHARS_PF), big_pf
    out = {
        "PORTFOLIO row 16: 2x slot | TWR and the change": left_of(pct5, False) - MK_GAP - wp(LBL_TWR) - slot_end(MK_BIG_CHARS_PF) - 1,
        "PORTFOLIO row 25: 2x slot | ANN": left_of(f"{LBL_ANN} {pctp}") - slot_end(MK_BIG_CHARS_PF) - 1,
        "PORTFOLIO drawdown stop, row 25: 2x slot | STALE date": left_of(f"STALE {date}") - slot_end(MK_BIG_CHARS_PF) - 1,
        "PORTFOLIO drawdown stop, footer: MDD .. REC | the right edge":
            MK_X_RIGHT - (MK_X_LEFT + wp(f"{LBL_MDD} {pctp}  {ym}>{ym}  {LBL_REC} {ym}") - 1),
        "PORTFOLIO contributions, footer: XIRR DIV | date":
            left_of(date) - (MK_X_LEFT + wp(f"{LBL_XIRR} {pctp}  DIV {amount}") - 1) - 1,
        "PORTFOLIO contributions, STALE footer: XIRR | STALE date":
            left_of(f"STALE {date}") - (MK_X_LEFT + wp(f"{LBL_XIRR} {pctp}") - 1) - 1,
        "PORTFOLIO footer as approved: DIV TER~ CASH | date":
            left_of(date) - (MK_X_LEFT + wp(f"DIV {amount}  TER~ {amount}  CASH {digit * 2}%") - 1) - 1,
        "TICKER heading: 8-character symbol, tag, currency | ANN":
            left_of(f"{LBL_ANN} {pctp}") - (MK_X_LEFT + w5("W" * 8) + MK_GAP + wp(tag) + MK_GAP + wp(capital * 3)),
        "TICKER row 25: 2x slot | badge D + 3 digits and the day change":
            left_of(f"{MK_DELAY_BADGE}{digit * 3} {pctp}") - slot_end(MK_BIG_CHARS) - 1,
        "MARKETS row 33: 2x slot | badge D + 3 digits and the day change":
            left_of(f"{MK_DELAY_BADGE}{digit * 3} {pctp}") - slot_end(MK_BIG_CHARS_MKT) - 1,
    }
    bad = {k: v for k, v in out.items() if v < 1}
    assert not bad, bad
    return out


def contact_sheet(images, cols=3, scale=4, label_h=14, margin=8):
    rows = math.ceil(len(images) / cols)
    cw, ch = W * scale + margin, H * scale + label_h + margin
    sheet = Image.new("RGB", (cols * cw + margin, rows * ch + margin), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, (name, title, img) in enumerate(images):
        x, y = margin + (i % cols) * cw, margin + (i // cols) * ch
        d.text((x, y), f"{name}  -  {title}"[:100], fill=(200, 200, 200))
        sheet.paste(img.resize((W * scale, H * scale), Image.NEAREST), (x, y + label_h))
    return sheet


def main():
    PREVIEW.mkdir(exist_ok=True)
    budget = layout_budget()
    frames, meta = build_frames()
    meta["budget"] = budget
    images, notes = [], {"_meta": meta}
    for name, title, page, ctx in frames:
        f = page(ctx)
        f.img.save(PREVIEW / f"{name}_128x64.png")
        f.img.resize((W * 6, H * 6), Image.NEAREST).save(PREVIEW / f"{name}.png")
        notes[name] = dict(f.notes, title=title, now=ctx["now"].isoformat())
        images.append((name, title, f.img))
        print(f"{name:28} {title}")
    contact_sheet(images).save(PREVIEW / "contact_sheet.png")
    (PREVIEW / "frames.json").write_text(json.dumps(notes, indent=1, sort_keys=True, default=str) + "\n")
    for k, v in budget.items():
        print(f"  budget {v:3d} blank columns  {k}")
    print(f"{len(frames)} frames, contact_sheet.png, frames.json -> {PREVIEW.relative_to(ROOT)}/")


if __name__ == "__main__":
    main()

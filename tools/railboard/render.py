#!/usr/bin/env python3
"""Host render of the rail board page, from sample payloads to PNG.

Every layout number, colour and word comes out of src/railboard/railboard.cpp
and the defaults out of railboard.h, so the render moves when the firmware does.
The drawing is a line-by-line mirror of the draw* functions there.

The fonts are the firmware's own, decoded with Adafruit GFX's drawChar bit order:
Picopixel's bitmaps from src/fonts/picopixel_fb.h (the corrected U) with the
glyph table from the Adafruit GFX library PlatformIO installed, and the built-in
5x7 font from tools/glcdfont.json. tools/picopixel.json is not used: it holds
capitals and digits only, and this board is mixed case.

London time comes from the tz database (zoneinfo), which check_uk_time.py holds
the firmware's own conversion to. Heap and JSON peak exist only on the device,
so diagnostics shows them as dashes.

  python3 tools/railboard/render.py      # every scene into tools/railboard/preview/
"""
import datetime as dt, glob, json, pathlib, re, sys
from zoneinfo import ZoneInfo
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SAMPLES, PREVIEW = HERE / "samples", HERE / "preview"
SRC = (ROOT / "src/railboard/railboard.cpp").read_text()
HDR = (ROOT / "src/railboard/railboard.h").read_text()

# ── constants, read from the firmware ────────────────────────────────────────
K = {m[1]: int(m[2]) for m in re.finditer(
    r"static const (?:u?int\d+_t|long)\s+(RB_\w+)\s*=\s*(-?\d+)\s*;", SRC)}
COL = {m[1]: tuple(int(v) for v in m[2].split(",")) for m in re.finditer(
    r"static const uint8_t (RB_COL_\w+)\[3\]\s*=\s*\{([^}]*)\};", SRC)}
ARR = {m[1]: re.findall(r'"([^"]*)"', m[2]) for m in re.finditer(
    r"static const char \*const (RB_\w+)\[\]\s*=\s*\{(.*?)\};", SRC, re.S)}
STR = {m[1]: m[2] for m in re.finditer(
    r'static const char \*const (RB_\w+)\s*=\s*"([^"]*)";', SRC)}
DEF = {m[1]: m[2].strip('"') for m in re.finditer(r'#define (RB_\w+)\s+("[^"]*"|\d+)', HDR)}
MODEL = (ROOT / "src/railboard/rb_model.h").read_text()   # the sizes moved there with the direct fetch
MAX_SVC = int(re.search(r"#define RB_MAX_SVC\s+(\d+)", MODEL)[1])
NAME_LEN = int(re.search(r"#define RB_NAME_LEN\s+(\d+)", MODEL)[1])
STN_LEN = int(re.search(r"#define RB_STN_LEN\s+(\d+)", MODEL)[1])

ASC, PITCH, BIG_ADV = K["RB_ASCENT"], K["RB_PITCH"], K["RB_BIG_ADV"]
X_LEFT, X_RIGHT, GAP = K["RB_X_LEFT"], K["RB_X_RIGHT"], K["RB_GAP"]
Y_TITLE, Y_HEAD1, Y_HEAD2 = K["RB_Y_TITLE"], K["RB_Y_HEAD1"], K["RB_Y_HEAD2"]
Y_ROW0, ROWS_PAGE, Y_CLOCK, Y_FOOT = K["RB_Y_ROW0"], K["RB_ROWS_PAGE"], K["RB_Y_CLOCK"], K["RB_Y_FOOT"]
X_DEST, X_EXPT, X_PLAT_R, X_TIME_R = K["RB_X_DEST"], K["RB_X_EXPT"], K["RB_X_PLAT_R"], K["RB_X_TIME_R"]
X_VALUE, GRACE = K["RB_X_VALUE"], K["RB_GRACE_S"]
WHITE, AMBER, RED, DIM, TEXT = (COL["RB_COL_" + n] for n in ("WHITE", "AMBER", "RED", "DIM", "TEXT"))
ST_KEYS, ST_WORDS, TITLES, DIAG_DIR = ARR["RB_ST_KEYS"], ARR["RB_ST_WORDS"], ARR["RB_TITLES"], ARR["RB_DIAG_DIR"]
H_PLAT, H_EXPT, H_TIME, H_DEST, H_FROM = (STR["RB_H_" + n] for n in ("PLAT", "EXPT", "TIME", "DEST", "FROM"))
CONTINUED, PAGE, OF = STR["RB_CONTINUED"], STR["RB_PAGE"], STR["RB_OF"]
STALE, EMPTY, WAITING = STR["RB_STALE"], STR["RB_EMPTY"], STR["RB_WAITING"]

# ── fonts ────────────────────────────────────────────────────────────────────
def load_picopixel():
    heads = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/Fonts/Picopixel.h"))
    if not heads:
        sys.exit("render.py: build any env once so PlatformIO installs Adafruit GFX (its Picopixel.h holds the glyph table)")
    table = re.search(r"PicopixelGlyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", open(heads[0]).read(), re.S)[1]
    glyphs = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", table)
    fb = (ROOT / "src/fonts/picopixel_fb.h").read_text()
    bits = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})",
            re.search(r"PicopixelFBBitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", fb, re.S)[1])]
    font = {}
    for i, g in enumerate(glyphs):
        off, w, h, adv, xo, yo = map(int, g)
        px, k = [], 0
        for r in range(h):                       # drawChar: MSB first, one stream per glyph
            for c in range(w):
                if bits[off + k // 8] & (0x80 >> (k % 8)):
                    px.append((c, r))
                k += 1
        font[chr(0x20 + i)] = dict(w=w, h=h, adv=adv, xo=xo, yo=yo, px=px)
    return font

PICO = load_picopixel()
GLCD = json.load(open(ROOT / "tools/glcdfont.json"))
LONDON = ZoneInfo("Europe/London")

def text_w(s):
    """Adafruit_GFX::getTextBounds width for a custom font: charBounds() mirrored."""
    x, lo, hi = 0, 1 << 15, -1
    for ch in s:
        g = PICO.get(ch)
        if g is None:
            continue
        if g["w"] and g["h"]:
            lo, hi = min(lo, x + g["xo"]), max(hi, x + g["xo"] + g["w"] - 1)
        x += g["adv"]
    return hi - lo + 1 if hi >= lo else 0

def big_w(s):
    return len(s) * BIG_ADV - 1 if s else 0

class Frame:
    def __init__(self, level):
        self.img = Image.new("RGB", (128, 64), (0, 0, 0))
        self.px, self.level = self.img.load(), level

    def col(self, c):
        r, g, b = (v * self.level // 100 for v in c)
        return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)   # what color565 keeps

    def dot(self, x, y, c):
        if 0 <= x < 128 and 0 <= y < 64:
            self.px[x, y] = c

    def put(self, x, top, s, c):
        base = top + ASC
        for ch in s:
            g = PICO.get(ch)
            if g is None:
                continue
            for cx, cy in g["px"]:
                self.dot(x + g["xo"] + cx, base + g["yo"] + cy, c)
            x += g["adv"]

    def put_right(self, right, top, s, c):
        self.put(right - text_w(s), top, s, c)

    def put_big(self, x, top, s, c):
        for ch in s:
            o = ord(ch) * 5
            for i in range(5):
                bits = GLCD[o + i]
                for j in range(8):
                    if (bits >> j) & 1:
                        self.dot(x + i, top + j, c)
            x += BIG_ADV

# ── the page ─────────────────────────────────────────────────────────────────
def civil(utc):
    return dt.datetime.fromtimestamp(utc, LONDON)

def hhmm(utc):
    return f"{civil(utc):%H:%M}"

def fit_name(name, room):
    out = name[:STN_LEN - 1]
    if text_w(out) <= room:
        return out
    if out.lower().startswith("london ") and len(out) > 7 and " " not in out[7:]:
        out = out[7:]
        if text_w(out) <= room:
            return out
    words = out
    while words and text_w(words) > room:
        if " " not in words:
            words = ""
            break
        words = words[:words.rfind(" ")]
        sp = words.rfind(" ")
        if sp >= 0 and (len(words[sp + 1:]) <= 2 or words[sp + 1:].lower() == "and"):
            words = words[:sp]
    if words:
        return words
    out = out.split(" ")[0]
    while len(out) > 1 and text_w(out) > room:
        out = out[:-1]
    return out

def ingest_board(b):
    """railboard.cpp ingestBoard(): mixed case kept, bounded fields, at most RB_MAX_SVC."""
    if not b:
        return None
    out = []
    for o in b["s"][:MAX_SVC]:
        if not o.get("t"):
            continue
        st = o.get("st", "")
        out.append({"t": o["t"], "x": o.get("x", 0), "st": st if st in ST_KEYS else "nr",
                    "d": max(0, min(255, o.get("d", 0))),
                    "p": o.get("p", "")[:3], "n": o.get("n", "")[:NAME_LEN - 1]})
    return {"s": out, "ts": b.get("ts", 0), "stn": b.get("stn", "")[:STN_LEN - 1],
            "rt": b.get("rt", "").upper()[:23]}

def is_late(sv):
    return sv["st"] == "late" or (sv["st"] == "arr" and sv["d"] > 0)

def has_time(sv):
    return bool(sv["x"]) and is_late(sv)

def is_stale(b, now, stale_s):
    return not b or not b["ts"] or now - b["ts"] > stale_s

def is_gone(sv, now):
    when = sv["x"] or sv["t"]
    return now > when + (2 * GRACE if sv["st"] == "arr" else GRACE)

def ago(prefix, s):
    if s < 120:
        return f"{prefix}{s} S AGO"
    if s < 7200:
        return f"{prefix}{s // 60} MIN AGO"
    return f"{prefix}{s // 3600} H AGO"

def draw_headings(f, which, sc):
    white = f.col(WHITE)
    f.put_big(X_LEFT, Y_TITLE, TITLES[which], white)
    if which == 1:
        f.put_right(X_TIME_R, Y_HEAD1, H_TIME, white)
    f.put_right(X_PLAT_R, Y_HEAD1, H_PLAT, white)
    f.put(X_EXPT, Y_HEAD1, H_EXPT, white)
    if which == 0:
        f.put(X_LEFT, Y_HEAD2, H_TIME, white)
        f.put(X_DEST, Y_HEAD2, H_DEST, white)
        used = X_DEST + text_w(H_DEST)
    else:
        f.put(X_LEFT, Y_HEAD2, H_FROM, white)
        used = X_LEFT + text_w(H_FROM)
    b0, b1 = sc["boards"]
    stn = (b0 and b0["stn"]) or (b1 and b1["stn"]) or sc["crs"]
    f.put_right(X_RIGHT, Y_HEAD2, fit_name(stn, X_RIGHT - used - 2 * GAP), f.col(DIM))

def draw_service(f, which, top, sv):
    canc = sv["st"] == "canc"
    amber = f.col(AMBER)
    expt = hhmm(sv["x"]) if has_time(sv) else ST_WORDS[ST_KEYS.index(sv["st"])]
    f.put(X_EXPT, top, expt, f.col(RED) if canc else amber)
    plat_left = X_PLAT_R
    if not canc and sv["p"]:
        plat_left = X_PLAT_R - text_w(sv["p"])
        f.put(plat_left, top, sv["p"], amber)
    tm = hhmm(sv["t"])
    if which == 0:
        f.put(X_LEFT, top, tm, amber)
        f.put(X_DEST, top, fit_name(sv["n"], plat_left - GAP - X_DEST), amber)
    else:
        x_time = X_TIME_R - text_w(tm)
        f.put(x_time, top, tm, amber)
        f.put(X_LEFT, top, fit_name(sv["n"], x_time - GAP - X_LEFT), amber)

def draw_board(f, which, sc, page_half):
    b, cfg, now = sc["boards"][which], sc["cfg"], sc["now"]
    amber = f.col(AMBER)
    if b is None:
        f.put(X_LEFT, Y_ROW0, f"{WAITING} {sc['crs']}" if sc["connected"] else sc["mqtt"], amber)
        f.put(X_LEFT, Y_FOOT, STALE, amber)
        return
    idx = [i for i, sv in enumerate(b["s"]) if not is_gone(sv, now)][:cfg["rows"]]
    n = len(idx)
    pages = 2 if n > ROWS_PAGE else 1
    page = 1 if (pages > 1 and page_half) else 0
    stale = is_stale(b, now, cfg["stale_s"])
    if n == 0:
        if not stale:
            f.put(X_LEFT, Y_ROW0, EMPTY, amber)
    elif page == 0:
        for r in range(min(n, ROWS_PAGE)):
            draw_service(f, which, Y_ROW0 + r * PITCH, b["s"][idx[r]])
    else:
        f.put(X_LEFT, Y_ROW0, CONTINUED, amber)
        r, k = 1, ROWS_PAGE
        while k < n and r < ROWS_PAGE:
            draw_service(f, which, Y_ROW0 + r * PITCH, b["s"][idx[k]])
            r, k = r + 1, k + 1
    f.put(X_LEFT, Y_FOOT, STALE if stale else f"{PAGE} {page + 1} {OF} {pages}", amber)

def draw_clock(f, now):
    clk = f"{civil(now):%H:%M:%S}"
    f.put_big(X_RIGHT - big_w(clk), Y_CLOCK, clk, f.col(AMBER))

def diag_line(f, line, label, value, value_col, right):
    top = 1 + line * PITCH
    f.put(X_LEFT, top, label, f.col(DIM))
    end = X_VALUE
    if value:
        f.put(X_VALUE, top, value, value_col)
        end += text_w(value)
    if right and X_RIGHT - text_w(right) >= end + GAP:
        f.put_right(X_RIGHT, top, right, f.col(DIM))

def draw_diag(f, sc):
    now, boards, ha = sc["now"], sc["boards"], sc["status"]
    white = f.col(WHITE)
    f.put(X_LEFT, 1, f"RAIL BOARD {sc['crs']}", f.col(AMBER))
    f.put_right(X_RIGHT, 1, "DIAGNOSTICS", f.col(DIM))
    last = max((b["ts"] for b in boards if b), default=0)
    if last:
        c = civil(last)
        diag_line(f, 1, "UPDATED", f"{c:%H:%M:%S} {'BST' if c.dst() else 'GMT'}", white,
                  ago("", now - last) if now >= last else "")
    else:
        diag_line(f, 1, "UPDATED", "NEVER", white, "")
    for d in (0, 1):
        b = boards[d]
        if b:   # the host has no receipt time; in steady state it is the fetch time
            diag_line(f, 2 + d, DIAG_DIR[d], f"{len(b['s'])} SERVICES", white, ago("RX ", now - b["ts"]))
        else:
            diag_line(f, 2 + d, DIAG_DIR[d], "NOTHING YET", white, "")
    if not ha:
        diag_line(f, 4, "HA", "NO STATUS", f.col(DIM), "")
    else:
        err, code, retry = ha["err"].upper(), ha["code"], ha["retry"]
        v = f"OK {code}" if not err else (f"{err} {code} RETRY {retry} S" if retry else (f"{err} HTTP {code}" if code else err))
        diag_line(f, 4, "HA", v, f.col(TEXT if not err else (AMBER if err == "RATE" else RED)),
                  hhmm(ha["at"]) if ha["at"] else "")
    rt = next((b["rt"] for b in boards if b and b["rt"]), "")
    rt = rt[14:] if rt.startswith("REALTIME_DATA_") else rt
    rl = ha["rl"] if ha else ""
    # matrix-waveshare-rgb builds RAILBOARD_DIRECT_ENABLED: line 5 is the panel's own
    # fetch, the realtime status moves after it, and the quota goes to line 8.
    direct = sc.get("direct", "DIRECT OK 200")
    fine = direct.startswith("DIRECT OK") or direct == "HA ONLY"
    diag_line(f, 5, "RTT", direct, f.col(TEXT if fine else AMBER), rt)
    diag_line(f, 6, "MQTT", "CONNECTED", f.col(TEXT), "REFUSED 0")
    diag_line(f, 7, "HEAP", "--", white, "JSON --")
    diag_line(f, 8, "SELECT", f"{sc['crs']} SENT", f.col(TEXT), f"LEFT {sc.get('left', rl)}" if (sc.get('left') or rl) else "NTP OK")

def render(sc):
    f = Frame(sc["cfg"]["level"])
    if sc["view"] == "diag":
        draw_diag(f, sc)
    else:
        which = 0 if sc["view"] == "dep" else 1
        draw_headings(f, which, sc)
        draw_board(f, which, sc, sc["page_half"])
        draw_clock(f, sc["now"])
    return f.img

# ── output ───────────────────────────────────────────────────────────────────
S = 8   # each LED is 7 px with a 1 px dark gap, so the preview reads like the panel

def leds(img):
    out = Image.new("RGB", (128 * S, 64 * S), (14, 14, 14))
    src, dst = img.load(), out.load()
    for y in range(64):
        for x in range(128):
            c = src[x, y]
            for yy in range(S - 1):
                for xx in range(S - 1):
                    dst[x * S + xx, y * S + yy] = c
    return out

def load(name):
    return json.load(open(SAMPLES / name)) if name else None

NOW = load("normal_departures.json")["ts"] + 7    # 14:05:14 BST, Monday 14 September 2026

SCENES = [
    # name,               view,   departures,                  arrivals,                status,            now,       extra
    ("departures",        "dep",  "normal_departures.json",    "normal_arrivals.json",  "status_ok.json",  NOW,       {}),
    ("arrivals",          "arr",  "normal_departures.json",    "normal_arrivals.json",  "status_ok.json",  NOW,       {}),
    ("departures_page2",  "dep",  "normal_departures.json",    "normal_arrivals.json",  "status_ok.json",  NOW + 5,   {"page_half": True}),
    ("delay",             "dep",  "delay_departures.json",     "cancel_arrivals.json",  "status_ok.json",  NOW,       {}),
    ("cancellation",      "arr",  "delay_departures.json",     "cancel_arrivals.json",  "status_ok.json",  NOW,       {}),
    ("stale",             "dep",  "normal_departures.json",    "normal_arrivals.json",  "status_net.json", NOW + 600, {}),
    ("long_name",         "dep",  "long_name_departures.json", None,                    "status_ok.json",  NOW,       {"crs": "LRD"}),
    ("diagnostics",       "diag", "normal_departures.json",    "normal_arrivals.json",  "status_net.json", NOW + 600, {}),
    ("waiting",           "dep",  None,                        None,                    None,              NOW,       {}),
]
RAW = ("departures", "arrivals")   # also written 1:1, 128 x 64

def main():
    PREVIEW.mkdir(exist_ok=True)
    cfg = load("config_small.json")
    for name, view, dep, arr, status, now, extra in SCENES:
        sc = {"view": view, "boards": [ingest_board(load(dep)), ingest_board(load(arr))],
              "status": load(status), "cfg": cfg, "now": now, "crs": extra.get("crs", DEF["RB_CRS"]),
              "connected": True, "mqtt": "CONNECTING", "page_half": extra.get("page_half", False)}
        img = render(sc)
        leds(img).save(PREVIEW / f"{name}.png")
        print((PREVIEW / f"{name}.png").relative_to(ROOT))
        if name in RAW:
            img.save(PREVIEW / f"{name}_128x64.png")
            print((PREVIEW / f"{name}_128x64.png").relative_to(ROOT))

main()

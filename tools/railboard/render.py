#!/usr/bin/env python3
"""Host render of the rail board page, from sample payloads to PNG.

Every layout number, colour and word comes out of src/railboard/railboard.cpp
and the defaults out of railboard.h, so the render moves when the firmware does.
The drawing is a line-by-line mirror of the draw* functions there. London time
comes from the tz database (zoneinfo), which check_uk_time.py holds the
firmware's own conversion to.

Two things the host cannot know are drawn as dashes in the diagnostics view:
heap and JSON peak, which only the device can measure.

  python3 tools/railboard/render.py      # every scene into tools/railboard/preview/
"""
import datetime as dt, json, pathlib, re
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
NAME_CAP = int(re.search(r"#define RB_NAME_LEN\s+(\d+)", SRC)[1])

W, ASC, PITCH = K["RB_W"], K["RB_ASCENT"], K["RB_PITCH"]
Y_HEAD, Y_TITLE, Y_ROW0, Y_FOOT = K["RB_Y_HEAD"], K["RB_Y_TITLE"], K["RB_Y_ROW0"], K["RB_Y_FOOT"]
X_STATUS, X_VALUE, GAP = K["RB_X_STATUS"], K["RB_X_VALUE"], K["RB_GAP"]
GUTTER = K["RB_GUTTER"]
L_ADV, L_PITCH, L_ROW0 = K["RB_L_ADV"], K["RB_L_PITCH"], K["RB_L_ROW0"]
ROWS_SMALL, ROWS_LARGE, GRACE = K["RB_ROWS_SMALL"], K["RB_ROWS_LARGE"], K["RB_GRACE_S"]
TEXT, WHITE, AMBER, RED, DIM = (COL["RB_COL_" + n] for n in ("TEXT", "WHITE", "AMBER", "RED", "DIM"))
ST_KEYS, ST_WORDS, TITLES, DIAG_DIR = ARR["RB_ST_KEYS"], ARR["RB_ST_WORDS"], ARR["RB_TITLES"], ARR["RB_DIAG_DIR"]
DAYS, MONTHS = ARR["RB_DAYS"], ARR["RB_MONTHS"]
CANC_L, STALE, EMPTY = STR["RB_CANC_LARGE"], STR["RB_STALE"], STR["RB_EMPTY"]
ARR_AT = STR["RB_ARR_AT"]

# ── fonts ────────────────────────────────────────────────────────────────────
PICO = json.load(open(ROOT / "tools/picopixel.json"))["glyphs"]
PICO = {c: dict(g) for c, g in PICO.items()}
# The firmware draws PicopixelFB, whose U has a flat bottom (src/fonts/picopixel_fb.h).
PICO["U"]["rows"] = PICO["U"]["rows"][:4] + [7]
GLCD = json.load(open(ROOT / "tools/glcdfont.json"))
LONDON = ZoneInfo("Europe/London")

def text_w(s):
    """Adafruit_GFX::getTextBounds width for a custom font, charBounds() mirrored."""
    x, minx, maxx = 0, 1 << 15, -1
    for ch in s:
        g = PICO.get(ch)
        if g is None:
            continue
        x1 = x + g["xo"]
        x2 = x1 + g["w"] - 1
        minx, maxx = min(minx, x1), max(maxx, x2)
        x += g["adv"]
    return maxx - minx + 1 if maxx >= minx else 0

def large_w(s):
    return len(s) * L_ADV - 1 if s else 0

class Frame:
    def __init__(self, level):
        self.img = Image.new("RGB", (2 * W, 64), (0, 0, 0))
        self.px, self.level = self.img.load(), level

    def col(self, c):
        r, g, b = (v * self.level // 100 for v in c)
        return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)   # what color565 keeps

    def dot(self, x, y, c):
        if 0 <= x < 2 * W and 0 <= y < 64:
            self.px[x, y] = c

    def put(self, x, top, s, c):
        base = top + ASC
        for ch in s:
            g = PICO.get(ch)
            if g is None:
                continue
            for r in range(g["h"]):
                for k in range(g["w"]):
                    if (g["rows"][r] >> k) & 1:
                        self.dot(x + g["xo"] + k, base + g["yo"] + r, c)
            x += g["adv"]

    def put_right(self, right, top, s, c):
        self.put(right - text_w(s), top, s, c)

    def put_large(self, x, top, s, c):
        for ch in s:
            o = ord(ch) * 5
            for i in range(5):
                bits = GLCD[o + i]
                for j in range(8):
                    if (bits >> j) & 1:
                        self.dot(x + i, top + j, c)
            x += L_ADV

# ── the page ─────────────────────────────────────────────────────────────────
def civil(utc):
    return dt.datetime.fromtimestamp(utc, LONDON)

def hhmm(utc):
    return f"{civil(utc):%H:%M}"

def fit_name(name, room, large, cap=NAME_CAP + 8):
    width = large_w if large else text_w
    out = name[:cap - 1]
    if width(out) <= room:
        return out
    if out.startswith("LONDON ") and len(out) > 7 and " " not in out[7:]:
        out = out[7:]
        if width(out) <= room:
            return out
    while width(out) > room:
        if " " not in out:
            while len(out) > 1 and width(out) > room:
                out = out[:-1]
            return out
        out = out[:out.rfind(" ")]
        sp = out.rfind(" ")
        if sp >= 0 and (len(out[sp + 1:]) <= 2 or out[sp + 1:] == "AND"):
            out = out[:sp]
    return out

def ingest_board(b):
    """railboard.cpp ingestBoard(): upper case, bounded fields, at most 8."""
    if not b:
        return None
    out = []
    for o in b["s"][:8]:
        if not o.get("t"):
            continue
        st = o.get("st", "")
        out.append({"t": o["t"], "x": o.get("x", 0),
                    "st": st if st in ST_KEYS else "nr",
                    "d": max(0, min(255, o.get("d", 0))),
                    "p": o.get("p", "").upper()[:3], "o": o.get("o", "").upper()[:3],
                    "n": o.get("n", "").upper()[:NAME_CAP - 1]})
    return {"s": out, "ts": b.get("ts", 0), "stn": b.get("stn", "").upper()[:19],
            "rt": b.get("rt", "").upper()[:23]}

def is_late(sv):
    return sv["st"] == "late" or (sv["st"] == "arr" and sv["d"] > 0)

def has_time(sv):
    return bool(sv["x"]) and is_late(sv)

def ago(prefix, s):
    if s < 120:
        return f"{prefix}{s} S AGO"
    if s < 7200:
        return f"{prefix}{s // 60} MIN AGO"
    return f"{prefix}{s // 3600} H AGO"

def is_stale(b, now, stale_s):
    if not b or not b["ts"]:
        return True
    return now - b["ts"] > stale_s

def is_gone(sv, now):
    when = sv["x"] or sv["t"]
    return now > when + (2 * GRACE if sv["st"] == "arr" else GRACE)

def draw_small(f, x0, top, sv):
    canc = sv["st"] == "canc"
    body = f.col(RED if canc else TEXT)
    right = x0 + W - GUTTER
    x_plat = right - text_w(sv["p"]) if (not canc and sv["p"]) else right
    f.put(x0, top, hhmm(sv["t"]), body)
    if has_time(sv):
        at = hhmm(sv["x"])
        word = f"{ST_WORDS[ST_KEYS.index('late')] if sv['st'] == 'late' else ARR_AT} {at}"
        if x0 + X_STATUS + text_w(word) + GAP > x_plat:
            word = at
    else:
        word = ST_WORDS[ST_KEYS.index(sv["st"])]
    f.put(x0 + X_STATUS, top, word, body if canc else f.col(AMBER if is_late(sv) else TEXT))
    if x_plat < right:
        f.put(x_plat, top, sv["p"], f.col(WHITE))
    top2, room = top + PITCH, W - GUTTER
    if sv["o"]:
        f.put_right(right, top2, sv["o"], f.col(DIM))
        room -= text_w(sv["o"]) + GAP
    f.put(x0, top2, fit_name(sv["n"], room, False), body)

def draw_large(f, x0, top, sv):
    canc = sv["st"] == "canc"
    c1 = f.col(RED if canc else (AMBER if is_late(sv) else TEXT))
    right = x0 + W - GUTTER
    f.put_large(x0, top, hhmm(sv["x"] if has_time(sv) else sv["t"]), c1)
    if canc:
        f.put_large(right - large_w(CANC_L), top, CANC_L, c1)
    elif sv["p"]:
        f.put_large(right - large_w(sv["p"]), top, sv["p"], f.col(WHITE))
    f.put_large(x0, top + L_PITCH, fit_name(sv["n"], W - GUTTER, True), f.col(RED if canc else TEXT))

def station(sc):
    for b in sc["boards"]:
        if b and b["stn"]:
            return b["stn"]
    return DEF["RB_STATION"]

def draw_list(f, x0, which, sc):
    b, cfg, now = sc["boards"][which], sc["cfg"], sc["now"]
    if b is None:
        f.put(x0, Y_ROW0, sc["mqtt"], f.col(DIM))
        f.put(x0, Y_FOOT, STALE, f.col(AMBER))
        return
    stale = is_stale(b, now, cfg["stale_s"])
    if not b["s"] and not stale:
        f.put(x0, Y_ROW0, EMPTY, f.col(DIM))
    large = cfg["font"] == "large"
    rows = min(cfg["rows"], ROWS_LARGE if large else ROWS_SMALL)
    shown = 0
    for sv in b["s"]:
        if shown >= rows:
            break
        if is_gone(sv, now):
            continue
        if large:
            draw_large(f, x0, L_ROW0 + shown * 2 * L_PITCH, sv)
        else:
            draw_small(f, x0, Y_ROW0 + shown * 2 * PITCH, sv)
        shown += 1
    if stale:
        f.put(x0, Y_FOOT, STALE, f.col(AMBER))

def draw_header_wide(f, sc):
    c = civil(sc["now"])
    clk, date = f"{c:%H:%M:%S}", f"{DAYS[(c.weekday() + 1) % 7]} {c.day} {MONTHS[c.month - 1]}"
    x_clk = 2 * W - text_w(clk)
    f.put(x_clk, Y_HEAD, clk, f.col(WHITE))
    x_date = x_clk - 2 * GAP - text_w(date)
    stn = fit_name(station(sc), x_clk - GAP, False, cap=20)
    f.put(0, Y_HEAD, stn, f.col(WHITE))
    if x_date >= text_w(stn) + 2 * GAP:
        f.put(x_date, Y_HEAD, date, f.col(DIM))
    f.put(0, Y_TITLE, TITLES[0], f.col(AMBER))
    f.put(W, Y_TITLE, TITLES[1], f.col(AMBER))

def draw_header_narrow(f, which, sc):
    c = civil(sc["now"])
    clk, date = f"{c:%H:%M}", f"{c.day} {MONTHS[c.month - 1]}"
    f.put_right(W, Y_HEAD, clk, f.col(WHITE))
    f.put(0, Y_HEAD, fit_name(station(sc), W - text_w(clk) - GAP, False, cap=20), f.col(WHITE))
    f.put(0, Y_TITLE, TITLES[which], f.col(AMBER))
    if text_w(TITLES[which]) + GAP + text_w(date) <= W:
        f.put_right(W, Y_TITLE, date, f.col(DIM))

def diag_line(f, line, label, value, value_col, right):
    top = Y_HEAD + line * PITCH
    f.put(0, top, label, f.col(DIM))
    end = X_VALUE
    if value:
        f.put(X_VALUE, top, value, value_col)
        end += text_w(value)
    if right and 2 * W - text_w(right) >= end + GAP:
        f.put_right(2 * W, top, right, f.col(DIM))

def draw_diag(f, sc):
    now, boards, ha = sc["now"], sc["boards"], sc["status"]
    white = f.col(WHITE)
    f.put(0, Y_HEAD, "RAIL BOARD " + DEF["RB_CRS"], f.col(AMBER))
    f.put_right(2 * W, Y_HEAD, "DIAGNOSTICS", f.col(DIM))
    last = max((b["ts"] for b in boards if b), default=0)
    if last:
        c = civil(last)
        v = f"{c:%H:%M:%S} {'BST' if c.dst() else 'GMT'}"
        r = ago("", now - last) if now >= last else ""
    else:
        v, r = "NEVER", ""
    diag_line(f, 1, "UPDATED", v, white, r)
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
        if not err:
            v = f"OK {code}"
        elif retry:
            v = f"{err} {code} RETRY {retry} S"
        elif code:
            v = f"{err} HTTP {code}"
        else:
            v = err
        colour = TEXT if not err else (AMBER if err == "RATE" else RED)
        diag_line(f, 4, "HA", v, f.col(colour), hhmm(ha["at"]) if ha["at"] else "")
    rt = next((b["rt"] for b in boards if b and b["rt"]), "")
    rt = rt[14:] if rt.startswith("REALTIME_DATA_") else rt
    rl = ha["rl"] if ha else ""
    diag_line(f, 5, "RTT", rt or "-", f.col(TEXT if rt == "OK" else AMBER), f"LEFT TODAY {rl}" if rl else "")
    diag_line(f, 6, "MQTT", "CONNECTED", f.col(TEXT), "REFUSED 0")
    diag_line(f, 7, "HEAP", "--", white, "JSON --")
    diag_line(f, 8, "LONDON", f"{'BST' if civil(now).dst() else 'GMT'}  NTP OK", f.col(TEXT), "")

def render(sc, which=0):
    f = Frame(sc["cfg"]["level"])
    if sc.get("diag") or sc["cfg"]["diag"]:
        draw_diag(f, sc)
    elif sc["cfg"]["panels"] >= 2:
        draw_header_wide(f, sc)
        draw_list(f, 0, 0, sc)
        draw_list(f, W, 1, sc)
    else:
        draw_header_narrow(f, which, sc)
        draw_list(f, 0, which, sc)
    return f.img

# ── output ───────────────────────────────────────────────────────────────────
S = 8   # each LED is 7 px with a 1 px dark gap, so the preview reads like the panel

def leds(img):
    w, h = img.size
    out = Image.new("RGB", (w * S, h * S), (14, 14, 14))
    src, dst = img.load(), out.load()
    for y in range(h):
        for x in range(w):
            c = src[x, y]
            for yy in range(S - 1):
                for xx in range(S - 1):
                    dst[x * S + xx, y * S + yy] = c
    return out

def load(name):
    return json.load(open(SAMPLES / name)) if name else None

NOW = load("normal_departures.json")["ts"] + 7    # 14:05:14 BST, Monday 14 September 2026

SCENES = [
    # name,        departures,                  arrivals,                  status,            config,                   now,        extra
    ("normal",      "normal_departures.json",    "normal_arrivals.json",    "status_ok.json",  "config_two_panels.json", NOW,        {}),
    ("disrupted",   "disrupted_departures.json", "disrupted_arrivals.json", "status_ok.json",  "config_two_panels.json", NOW,        {}),
    ("stale",       "normal_departures.json",    "normal_arrivals.json",    "status_net.json", "config_two_panels.json", NOW + 600,  {}),
    ("large",       "disrupted_departures.json", "disrupted_arrivals.json", "status_ok.json",  "config_large.json",      NOW,        {}),
    ("one_panel",   "disrupted_departures.json", "disrupted_arrivals.json", "status_ok.json",  "config_one_panel.json",  NOW,        {"frames": (0, 1)}),
    ("diagnostics", "normal_departures.json",    "normal_arrivals.json",    "status_net.json", "config_two_panels.json", NOW + 600,  {"diag": True}),
    ("waiting",     None,                        None,                      None,              "config_two_panels.json", NOW,        {"mqtt": "CONNECTING"}),
]

def main():
    PREVIEW.mkdir(exist_ok=True)
    for name, dep, arr, status, cfg, now, extra in SCENES:
        sc = {"boards": [ingest_board(load(dep)), ingest_board(load(arr))],
              "status": load(status), "cfg": load(cfg), "now": now,
              "mqtt": extra.get("mqtt", "NO DATA"), "diag": extra.get("diag", False)}
        if "frames" in extra:   # one panel: the two lists it alternates between, side by side
            frames = [leds(render(sc, w).crop((0, 0, W, 64))) for w in extra["frames"]]
            out = Image.new("RGB", (frames[0].width * 2 + 4 * S, frames[0].height), (40, 40, 40))
            out.paste(frames[0], (0, 0))
            out.paste(frames[1], (frames[0].width + 4 * S, 0))
        else:
            out = leds(render(sc))
        path = PREVIEW / f"{name}.png"
        out.save(path)
        print(path.relative_to(ROOT))

main()

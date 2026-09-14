#!/usr/bin/env python3
"""Host render of the media player page, from sample payloads to PNG.

Every layout number, colour and word comes out of src/media/media_page.cpp and
the timings out of media.h, so the render moves when the firmware does. The
drawing mirrors the draw* functions there line by line, and every string goes
through the firmware's own transliteration (media_model.cpp, compiled on the
host by tools/media/probe.py) - not a Python copy of it.

The fonts are the firmware's own, decoded as tools/railboard/render.py decodes
them: Picopixel's bitmaps from src/fonts/picopixel_fb.h with the glyph table
from the Adafruit GFX library PlatformIO installed, and the built-in 5x7 font
from tools/glcdfont.json. Circles and lines follow Adafruit_GFX.cpp's
drawCircle and writeLine. The header clock is drawn in Europe/Paris, Home
Assistant's zone on the owner's instance; the panel draws its own zone.

  python3 tools/media/render.py      # every scene into tools/media/preview/
"""
import datetime as dt
import functools
import glob
import json
import pathlib
import re
import sys
from zoneinfo import ZoneInfo

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import probe  # noqa: E402

SAMPLES, PREVIEW = HERE / "samples", HERE / "preview"
SRC = (ROOT / "src/media/media_page.cpp").read_text()
HDR = (ROOT / "src/media/media.h").read_text()
MODEL = (ROOT / "src/media/media_model.h").read_text()

# ── constants, read from the firmware ────────────────────────────────────────
K = {m[1]: int(m[2]) for m in re.finditer(r"static const u?int\d+_t\s+(MP_\w+)\s*=\s*(-?\d+)\s*;", SRC)}
COL = {m[1]: tuple(int(v) for v in m[2].split(",")) for m in re.finditer(r"static const uint8_t MP_COL_(\w+)\[3\]\s*=\s*\{([^}]*)\};", SRC)}
W = {m[1]: m[2] for m in re.finditer(r'static const char \*const MP_(\w+)\s*=\s*"([^"]*)";', SRC)}
D = {m[1]: int(m[2]) for m in re.finditer(r"#define (MEDIA_\w+)\s+(\d+)", HDR)}
SZ = {m[1]: int(m[2]) for m in re.finditer(r"static const (?:size_t|uint32_t|uint8_t)\s+(k\w+)\s*=\s*(\d+);", MODEL)}
G = {k[3:]: v for k, v in K.items()}          # MP_Y_HEAD -> Y_HEAD
PARIS = ZoneInfo("Europe/Paris")


# ── fonts ────────────────────────────────────────────────────────────────────
def load_picopixel():
    heads = glob.glob(str(ROOT / ".pio/libdeps/*/Adafruit GFX Library/Fonts/Picopixel.h"))
    if not heads:
        sys.exit("render.py: build any env once so PlatformIO installs Adafruit GFX (its Picopixel.h holds the glyph table)")
    table = re.search(r"PicopixelGlyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", open(heads[0]).read(), re.S)[1]
    glyphs = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", table)
    fb = (ROOT / "src/fonts/picopixel_fb.h").read_text()
    bits = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", re.search(r"PicopixelFBBitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", fb, re.S)[1])]
    font = {}
    for i, g in enumerate(glyphs):
        off, w, h, adv, xo, yo = map(int, g)
        px, k = [], 0
        for r in range(h):
            for c in range(w):
                if bits[off + k // 8] & (0x80 >> (k % 8)):
                    px.append((c, r))
                k += 1
        font[chr(0x20 + i)] = dict(w=w, h=h, adv=adv, xo=xo, yo=yo, px=px)
    return font


PICO = load_picopixel()
GLCD = json.load(open(ROOT / "tools/glcdfont.json"))


def text_w(s):
    """Adafruit_GFX::getTextBounds width for a custom font."""
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
    return len(s) * G["BIG_ADV"] - 1 if s else 0


@functools.lru_cache(maxsize=None)
def tl(s):
    return probe.translit([s])[0] if s else ""


def bound(s, cap):
    """copyUtf8: control characters are spaces, cut to cap - 1 bytes at a whole code point."""
    s = "".join(" " if (ord(ch) < 0x20 or 0x7F <= ord(ch) < 0xA0) else ch for ch in (s or ""))
    raw = s.encode("utf-8")
    return raw[:cap - 1].decode("utf-8", errors="ignore") if len(raw) > cap - 1 else s


def ingest_state(d):
    """stateFrom() on an already valid payload: the defaults and the bounds the panel applies."""
    if d is None:
        return None
    player = d.get("player", "")
    return {
        "ts": d["ts"], "player": player,
        "name": bound(d.get("name") or (player.split(".", 1)[1] if player else ""), SZ["kNameLen"]),
        "src": d.get("src") or "HA", "err": d.get("err", ""),
        "st": d.get("st") or "unavailable", "kind": d.get("kind") or "music",
        "title": bound(d.get("title"), SZ["kTitleLen"]), "artist": bound(d.get("artist"), SZ["kArtistLen"]),
        "album": bound(d.get("album"), SZ["kAlbumLen"]),
        "vol": d.get("vol", -1) if d.get("vol") is not None else -1, "muted": bool(d.get("muted", False)),
        "pos": d.get("pos", 0), "pos_at": d.get("pos_at", 0), "dur": d.get("dur", 0),
    }


def position_at(np, now):
    p = np["pos"]
    if np["st"] == "playing" and np["pos_at"] and now > np["pos_at"]:
        p += now - np["pos_at"]
    if np["dur"] and p > np["dur"]:
        p = np["dur"]
    return min(p, SZ["kMaxSeconds"])


def format_clock(s):
    return "%d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60) if s >= 3600 else "%d:%02d" % (s // 60, s % 60)


# ── the panel ────────────────────────────────────────────────────────────────
class Frame:
    def __init__(self):
        self.img = Image.new("RGB", (128, 64), (0, 0, 0))
        self.px = self.img.load()

    @staticmethod
    def c565(rgb):
        r, g, b = rgb
        return ((r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3)

    def dot(self, x, y, c):
        if 0 <= x < 128 and 0 <= y < 64:
            self.px[x, y] = c

    def fill(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.dot(xx, yy, c)

    def rect(self, x, y, w, h, c):
        self.fill(x, y, w, 1, c); self.fill(x, y + h - 1, w, 1, c)
        self.fill(x, y, 1, h, c); self.fill(x + w - 1, y, 1, h, c)

    def circle(self, x0, y0, r, c):              # Adafruit_GFX::drawCircle
        f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
        for px, py in ((x0, y0 + r), (x0, y0 - r), (x0 + r, y0), (x0 - r, y0)):
            self.dot(px, py, c)
        while x < y:
            if f >= 0:
                y -= 1; ddy += 2; f += ddy
            x += 1; ddx += 2; f += ddx
            for px, py in ((x0 + x, y0 + y), (x0 - x, y0 + y), (x0 + x, y0 - y), (x0 - x, y0 - y),
                           (x0 + y, y0 + x), (x0 - y, y0 + x), (x0 + y, y0 - x), (x0 - y, y0 - x)):
                self.dot(px, py, c)

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
                y0 += ystep; err += dx
            x0 += 1

    def pico(self, x, top, s, c):
        base = top + G["ASCENT"]
        for ch in s:
            g = PICO.get(ch)
            if g is None:
                continue
            for cx, cy in g["px"]:
                self.dot(x + g["xo"] + cx, base + g["yo"] + cy, c)
            x += g["adv"]

    def big(self, x, top, s, c):
        for ch in s:
            o = ord(ch) * 5
            for i in range(5):
                bits = GLCD[o + i]
                for j in range(8):
                    if (bits >> j) & 1:
                        self.dot(x + i, top + j, c)
            x += G["BIG_ADV"]


class Page:
    """media_page.cpp, with the scene standing in for the panel's state."""

    def __init__(self, sc):
        self.sc, self.f, self.level = sc, Frame(), 100
        self.np = ingest_state(sc.get("state"))

    # helpers
    def col(self, name):
        return Frame.c565(tuple(v * self.level // 100 for v in COL[name]))

    def full(self, name):
        return Frame.c565(COL[name])

    def put(self, x, top, s, c): self.f.pico(x, top, s, c)
    def put_right(self, right, top, s, c): self.put(right - text_w(s), top, s, c)
    def put_centre(self, top, s, c): self.put(int((128 - text_w(s)) / 2), top, s, c)
    def put_big(self, x, top, s, c): self.f.big(x, top, s, c)

    @staticmethod
    def fit_cut(s, room):
        while s and text_w(s) > room:
            s = s[:-1]
        return s.rstrip(" ")

    def entered(self): return self.sc.get("entered", False)
    def ms(self): return self.sc.get("ms", 100000)
    def stale(self): return self.np is None or self.sc["now"] - self.np["ts"] > D["MEDIA_STALE_S"]
    def vol_shown(self):
        return self.sc["vol_pending"] if self.sc.get("vol_pending") is not None else (self.np["vol"] if self.np else -1)
    def favs(self): return self.sc.get("favs") or []
    def mode(self): return self.sc.get("mode", "none")
    def overlay_on(self):
        o = self.sc.get("overlay_ms")
        return o is not None and self.ms() - o < D["MEDIA_OVERLAY_MS"]

    def marquee(self, text, w, room):
        if w <= room:
            return 0
        travel = w - room
        move = travel * 1000 // G["SCROLL_PX_S"]
        cycle = G["SCROLL_HOLD_MS"] + move + G["SCROLL_END_MS"]
        t = (self.ms() - self.sc.get("scroll_at", self.ms())) % cycle
        if t < G["SCROLL_HOLD_MS"]:
            return 0
        if t < G["SCROLL_HOLD_MS"] + move:
            return (t - G["SCROLL_HOLD_MS"]) * G["SCROLL_PX_S"] // 1000
        return travel

    def big_line(self, top, left, text, c):
        off = self.marquee(text, big_w(text), G["X_RIGHT"] - left)
        self.put_big(left - off, top, text, c)
        if off:
            self.f.fill(0, top, left, 8, (0, 0, 0))
            self.f.fill(G["X_RIGHT"], top, 128 - G["X_RIGHT"], 8, (0, 0, 0))

    def tag(self, x, text, bg):
        w = text_w(text) + 4
        self.f.fill(x, G["Y_HEAD"] - 1, w, 7, bg)
        self.put(x + 2, G["Y_HEAD"], text, (0, 0, 0))
        return x + w

    def state_icon(self, x, top, st):
        if st == "playing":
            c = self.col("GREEN")
            for i in range(3):
                self.f.fill(x + 1 + i, top + i, 1, 5 - 2 * i, c)
        elif st == "paused":
            c = self.col("AMBER")
            self.f.fill(x, top, 2, 5, c); self.f.fill(x + 3, top, 2, 5, c)
        elif st == "idle":
            self.f.rect(x, top, 5, 5, self.col("DIM"))
        elif st == "off":
            self.f.circle(x + 2, top + 2, 2, self.col("DIM"))
        else:
            c = self.col("RED")
            self.f.line(x, top, x + 4, top + 4, c); self.f.line(x, top + 4, x + 4, top, c)

    def head_right(self, st, stale):
        right = G["X_RIGHT_IN"] if self.entered() else G["X_RIGHT"]
        if stale:
            w = text_w(W["W_STALE"]) + 4
            self.tag(right - w, W["W_STALE"], self.full("RED"))
            return right - w
        clk = dt.datetime.fromtimestamp(self.sc["now"], PARIS).strftime("%H:%M")
        cx = right - text_w(clk)
        self.put(cx, G["Y_HEAD"], clk, self.col("DIM"))
        if st is None:
            return cx
        ix = cx - G["GAP"] - G["ICON_W"]
        self.state_icon(ix, G["Y_HEAD"], st)
        return ix

    def progress(self, stale):
        n, top = self.np, G["Y_PROGRESS"]
        if n["kind"] == "radio" or not n["dur"]:
            if n["st"] != "playing":
                return
            red = self.col("RED")
            self.f.fill(G["X_LEFT"], top + 1, 3, 3, red)
            self.put(G["X_LEFT"] + 5, top, W["W_LIVE"], red)
            return
        pos = n["pos"] if stale else position_at(n, self.sc["now"])
        el, tot = format_clock(pos), format_clock(n["dur"])
        self.put(G["X_LEFT"], top, el, self.col("DIM"))
        self.put_right(G["X_RIGHT"], top, tot, self.col("DIM"))
        x0, x1 = G["X_LEFT"] + text_w(el) + G["GAP"], G["X_RIGHT"] - text_w(tot) - G["GAP"]
        if x1 <= x0:
            return
        self.f.fill(x0, top + 1, x1 - x0, 3, self.col("TRACK"))
        done = (x1 - x0) * pos // n["dur"]
        if done > 0:
            self.f.fill(x0, top + 1, done, 3, self.col("GREEN" if n["st"] == "playing" else "AMBER"))

    def volume_row(self):
        n, top, active = self.np, G["Y_VOLUME"], self.mode() == "volume"
        self.put(G["X_LEFT"], top, W["W_VOL"], self.col("AMBER" if active else "DIM"))
        v = self.vol_shown()
        self.f.fill(G["X_VOL_BAR"], top + 1, G["VOL_BAR_W"], 3, self.col("TRACK"))
        if v > 0:
            self.f.fill(G["X_VOL_BAR"], top + 1, G["VOL_BAR_W"] * v // 100, 3,
                        self.col("DIM" if n["muted"] else ("AMBER" if active else "ARTIST")))
        val = W["W_MUTED"] if n["muted"] else ("--" if v < 0 else str(v))
        tx = G["X_VOL_BAR"] + G["VOL_BAR_W"] + G["GAP"]
        self.put(tx, top, val, self.col("RED" if n["muted"] else ("AMBER" if active else "DIM")))
        if n["err"] and G["X_RIGHT"] - text_w(n["err"]) >= tx + text_w(val) + G["GAP"]:
            self.put_right(G["X_RIGHT"], top, n["err"], self.col("RED"))

    def hint(self, stale):
        top, ms = G["Y_HINT"], self.ms()
        if stale:
            a = max(0, self.sc["now"] - self.np["ts"])
            t = "%s %d S" % (W["H_NOUPDATE"], a) if a < 120 else ("%s %d MIN" % (W["H_NOUPDATE"], a // 60) if a < 7200
                                                               else "%s %d H" % (W["H_NOUPDATE"], a // 3600))
            self.put(G["X_LEFT"], top, t, self.full("RED"))
            return
        shown = self.sc.get("track_shown_ms")
        if self.sc.get("track_steps") or (shown is not None and ms - shown < 1200):
            self.put(G["X_LEFT"], top, W["H_NEXT"] if self.sc.get("track_dir", 1) > 0 else W["H_PREV"], self.col("AMBER"))
            return
        m = self.mode()
        h = W["H_VOLUME"] if m == "volume" else ((W["H_TUNE"] if self.favs() else W["H_TRACK"]) if m == "tune" else W["H_BROWSE"])
        self.put(G["X_LEFT"], top, h, self.col("AMBER" if m != "none" else "DIM"))

    def now_playing(self):
        n = self.np
        stale = self.stale()
        self.level = G["STALE_LEVEL"] if stale else 100
        tagcol = {"radio": "RADIO", "tts": "TTS"}.get(n["kind"], "MUSIC")
        x = self.tag(0, n["src"], self.col(tagcol)) + G["GAP"]
        right = self.head_right(n["st"], stale) - G["GAP"]
        self.put(x, G["Y_HEAD"], self.fit_cut(tl(n["name"]), right - x), self.col("DIM"))
        self.put(G["X_LEFT"], G["Y_ARTIST"], self.fit_cut(tl(n["artist"]), G["X_RIGHT"] - G["X_LEFT"]), self.col("ARTIST"))
        title = tl(n["title"])
        if title:
            nt = self.sc.get("new_track_ms")
            fresh = nt is not None and self.ms() - nt < G["NEW_TRACK_MS"]
            self.big_line(G["Y_TITLE"], G["X_LEFT"], title, self.col("AMBER" if fresh else "WHITE"))
        else:
            self.put_big(G["X_LEFT"], G["Y_TITLE"], W["W_OFF"] if n["st"] == "off" else W["W_IDLE"], self.col("DIM"))
        self.put(G["X_LEFT"], G["Y_ALBUM"], self.fit_cut(tl(n["album"]), G["X_RIGHT"] - G["X_LEFT"]), self.col("DIM"))
        self.progress(stale)
        self.volume_row()
        self.hint(stale)

    @staticmethod
    def dial_x(k, n):
        if n < 2:
            return (G["X_DIAL0"] + G["X_DIAL1"]) // 2
        return G["X_DIAL0"] + k * (G["X_DIAL1"] - G["X_DIAL0"]) // (n - 1)

    def tune(self):
        f, i = self.favs(), self.sc.get("tune_idx", 0)
        n = len(f)
        self.level = 100
        x = self.tag(0, W["W_TUNE"], self.col("RADIO")) + G["GAP"]
        self.put_right(G["X_RIGHT_IN"] if self.entered() else G["X_RIGHT"], G["Y_HEAD"], "%d/%d" % (i + 1, n), self.col("DIM"))
        self.put(x, G["Y_HEAD"], W["W_FAVS"], self.col("DIM"))
        if n > 1:
            self.put(G["X_TUNE_NAME"], G["Y_TUNE_PREV"], self.fit_cut(tl(f[(i + n - 1) % n]["name"]), G["X_RIGHT"] - G["X_TUNE_NAME"]), self.col("DIM"))
        self.big_line(G["Y_TUNE_CUR"], G["X_TUNE_NAME"], tl(f[i]["name"]), self.col("AMBER"))
        self.put_big(G["X_LEFT"], G["Y_TUNE_CUR"], ">", self.col("AMBER"))
        if n > 2:
            self.put(G["X_TUNE_NAME"], G["Y_TUNE_NEXT"], self.fit_cut(tl(f[(i + 1) % n]["name"]), G["X_RIGHT"] - G["X_TUNE_NAME"]), self.col("DIM"))
        played = next((k for k, fav in enumerate(f) if fav["id"] == self.sc.get("played_id")), -1)
        for k in range(n):
            self.f.fill(self.dial_x(k, n), G["Y_DIAL"], 1, 3, self.col("GREEN" if k == played else "DIM"))
        self.f.fill(self.dial_x(i, n), G["Y_DIAL"] - 2, 1, 7, self.col("AMBER"))
        moved, sent, ms, rest = self.sc.get("tune_moved_ms"), self.sc.get("tune_sent_ms"), self.ms(), D["MEDIA_TUNE_REST_MS"]
        if moved is not None:
            left = max(0, rest - (ms - moved))
            w = (G["X_DIAL1"] - G["X_DIAL0"] + 1) * left // rest
            if w > 0:
                self.f.fill(G["X_DIAL0"], G["Y_WAIT"], w, 2, self.col("AMBER"))
            self.put(G["X_LEFT"], G["Y_HINT"], "%s %d.%d S" % (W["H_STARTS"], left // 1000, left // 100 % 10), self.col("AMBER"))
        elif sent is not None and ms - sent < G["SENT_SHOW_MS"]:
            ok = self.sc.get("tune_ok", True)
            self.put(G["X_LEFT"], G["Y_HINT"], W["H_SENT"] if ok else W["H_NOTSENT"], self.col("GREEN" if ok else "RED"))
        else:
            self.put(G["X_LEFT"], G["Y_HINT"], W["H_TUNE"], self.col("DIM"))

    def screen(self, big, line1, line2):
        self.level = 100
        self.tag(0, W["W_MEDIA"], self.col("DIM"))
        self.head_right(None, False)
        self.put_big(int((128 - big_w(big)) / 2), G["Y_BIG"], big, self.col("WHITE"))
        for text, top in ((line1, G["Y_LINE1"]), (line2, G["Y_LINE2"])):
            if text:
                self.put_centre(top, self.fit_cut(tl(text), G["X_RIGHT"] - G["X_LEFT"]), self.col("DIM"))
        if self.sc.get("dev"):
            self.put(G["X_LEFT"], G["Y_HINT"], "%s %s" % (W["L_DEV"], self.sc["dev"]), self.col("DIM"))

    def volume_overlay(self):
        self.level = 100
        v, amber = self.vol_shown(), self.col("AMBER")
        muted = self.np["muted"]
        self.f.fill(0, G["Y_OV0"], 128, G["Y_OV1"] - G["Y_OV0"] + 1, (0, 0, 0))
        self.f.fill(0, G["Y_OV0"], 128, 1, amber)
        self.f.fill(0, G["Y_OV1"], 128, 1, amber)
        self.put(G["X_OV0"], G["Y_OV0"] + 4, W["W_VOLUME"], amber)
        if muted:
            self.put(G["X_OV0"] + text_w(W["W_VOLUME"]) + 6, G["Y_OV0"] + 4, W["W_MUTED"], self.col("RED"))
        t = "--" if v < 0 else str(v)
        self.put_big(G["X_OV1"] - big_w(t), G["Y_OV0"] + 3, t, amber)
        w = G["X_OV1"] - G["X_OV0"]
        self.f.fill(G["X_OV0"], G["Y_OV_BAR"], w, 6, self.col("TRACK"))
        if v > 0:
            self.f.fill(G["X_OV0"], G["Y_OV_BAR"], w * v // 100, 6, self.col("DIM") if muted else amber)

    def render(self):
        sc, n = self.sc, self.np
        if not sc.get("connected", True):
            self.screen(W["W_OFFLINE"], sc.get("mqtt_status", "CONNECTING"), W["L_BROKER"])
        elif sc.get("bridge") == "offline":
            self.screen(W["W_OFFLINE"], W["L_APP"], W["L_APP_OFF"])
        elif n is None:
            self.screen(W["W_WAITING"], W["L_FOR_HA"], "")
        elif self.mode() == "tune" and self.favs():
            self.tune()
        elif not n["player"]:
            self.screen(W["W_NOPLAYER"], W["L_NONE"], W["L_CHOOSE"])
        elif n["st"] == "unavailable":
            self.screen(W["W_NOPLAYER"], n["name"], W["L_UNAVAIL"])
        else:
            self.now_playing()
        if sc.get("connected", True) and n is not None and self.overlay_on():
            self.volume_overlay()
        return self.f.img


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


MUSIC = load("state_music.json")
T = MUSIC["ts"] + 20                  # 2026-09-14 19:39:00 UTC, 21:39 in Paris
FAVS = load("favs.json")["list"]
HOLD = G["SCROLL_HOLD_MS"]

SCENES = [
    # name,                          state,                    extra
    ("now_playing",                  "state_music.json",       {}),
    ("now_playing_cyrillic",         "state_cyrillic.json",    {"scroll_at": 100000}),
    ("now_playing_cyrillic_scrolled", "state_cyrillic.json",   {"scroll_at": 100000 - HOLD - 700}),
    ("new_track",                    "state_cyrillic.json",    {"scroll_at": 100000, "new_track_ms": 100000 - 800}),
    ("radio_live",                   "state_radio.json",       {}),
    ("paused_with_error",            "state_paused.json",      {}),
    ("player_off",                   "state_off.json",         {}),
    ("tune_countdown",               "state_music.json",       {"mode": "tune", "entered": True, "favs": FAVS, "tune_idx": 3,
                                                                "tune_moved_ms": 100000 - 400, "played_id": "library://radio/5"}),
    ("tune_long_name",               "state_music.json",       {"mode": "tune", "entered": True, "favs": FAVS, "tune_idx": 5,
                                                                "scroll_at": 100000 - HOLD - 1500, "tune_sent_ms": 100000 - 900,
                                                                "played_id": "library://radio/10"}),
    ("track_mode_no_favourites",     "state_cyrillic.json",    {"mode": "tune", "entered": True, "scroll_at": 100000,
                                                                "track_steps": 1, "track_dir": 1}),
    ("volume_band",                  "state_music.json",       {"mode": "volume", "entered": True, "overlay_ms": 100000 - 300, "vol_pending": 56}),
    ("volume_mode",                  "state_music.json",       {"mode": "volume", "entered": True, "vol_pending": 56}),
    ("stale",                        "state_music.json",       {"now": MUSIC["ts"] + 420}),
    ("no_player",                    "state_none.json",        {}),
    ("unavailable",                  "state_unavailable.json", {}),
    ("offline",                      None,                     {"connected": False, "mqtt_status": "CONNECTING"}),
    ("app_offline",                  "state_music.json",       {"bridge": "offline"}),
    ("waiting",                      None,                     {}),
]
RAW = ("now_playing", "now_playing_cyrillic")   # also written 1:1, 128 x 64


def main():
    PREVIEW.mkdir(exist_ok=True)
    for name, state, extra in SCENES:
        sc = {"now": T, "ms": 100000, "dev": "a1b2c3", "state": load(state), **extra}
        img = Page(sc).render()
        leds(img).save(PREVIEW / f"{name}.png")
        print((PREVIEW / f"{name}.png").relative_to(ROOT))
        if name in RAW:
            img.save(PREVIEW / f"{name}_128x64.png")
            print((PREVIEW / f"{name}_128x64.png").relative_to(ROOT))


main()

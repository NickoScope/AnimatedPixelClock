#!/usr/bin/env python3
"""Real golf courses for GOLF CLOCK.

Each course is two files in tools/luasim/golf/:
- `<id>.card.json`: the official scorecard (par, length, stroke index per
  hole), with its source;
- `<id>.json`: the eighteen holes already laid out in screen space, the tee
  on the left and the cup on the right, the way golf_clock.lua draws a hole.

`build` packs every course into golf_clock.lua (between the COURSES markers)
and writes one script per course with that course fixed, in
tools/luasim/scripts (and gallery/ with --gallery).

    python3 tools/luasim/golf_courses.py osm old_course --relation 16704569
    python3 tools/luasim/golf_courses.py plans pestovo --dir /path/to/plans
    python3 tools/luasim/golf_courses.py build

Two ways to lay out a course:
- `osm`: the course is mapped in OpenStreetMap with golf=hole ways (tee to
  cup, dogleg points between), greens, fairways, bunkers, water and woods.
  Everything on screen is where the map has it. Data (c) OpenStreetMap
  contributors, ODbL: the script says so in its header.
- `plans`: the club's own hole plans (a picture per hole, tee at the bottom,
  green at the top). Water, sand and grass are told apart by colour; the
  pictures themselves never enter the repository, only the positions taken
  from them.

The screen: x 0..127, the tee at x 8 and the cup at x 114; play between y 9
and 55 (the score bar above, the time and messages below). One scale along
the hole and across it, so a par 3 is seen closer than a par 5, as a camera
would.
"""
import argparse
import json
import math
import pathlib
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
GOLF = HERE / "golf"
REPO = HERE.parent.parent
MASTER = HERE / "scripts" / "golf_course.lua"   # golf_clock.lua stays the published one until this is approved
GALLERY = REPO / "gallery"

TEE_X, CUP_X = 8, 114
Y_MIN, Y_MAX, Y_MID = 9, 55, 32
SPAN = CUP_X - TEE_X
MAX_BUNKERS, MAX_WATER, MAX_TREES = 10, 45, 26

# The courses built into golf_clock.lua, in the order a rotating copy plays them.
ORDER = ["old_course", "pestovo"]


# ---------------------------------------------------------------- geometry
def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


class Frame:
    """Map metres to the screen for one hole: A the tee, B the cup."""

    def __init__(self, a, b, line):
        self.a = a
        dx, dy = b[0] - a[0], b[1] - a[1]
        self.L = math.hypot(dx, dy) or 1.0
        self.e = (dx / self.L, dy / self.L)
        self.n = (-self.e[1], self.e[0])          # the player's left
        self.s = SPAN / self.L                    # px per metre
        vs = [self.uv(p)[1] for p in line]
        self.v0 = (min(vs) + max(vs)) / 2

    def uv(self, p):
        x, y = p[0] - self.a[0], p[1] - self.a[1]
        return (x * self.e[0] + y * self.e[1]) / self.L, x * self.n[0] + y * self.n[1]

    def screen(self, p):
        u, v = self.uv(p)
        return TEE_X + u * SPAN, Y_MID - (v - self.v0) * self.s

    def world(self, sx, sy):
        u = (sx - TEE_X) / SPAN
        v = self.v0 - (sy - Y_MID) / self.s
        return (self.a[0] + u * self.L * self.e[0] + v * self.n[0],
                self.a[1] + u * self.L * self.e[1] + v * self.n[1])


def on_screen(x, y, pad=0):
    return -pad <= x <= 127 + pad and Y_MIN - pad <= y <= Y_MAX + pad


def area_centroid(ring):
    a = cx = cy = 0.0
    for i in range(len(ring) - 1):
        x0, y0 = ring[i]
        x1, y1 = ring[i + 1]
        k = x0 * y1 - x1 * y0
        a += k
        cx += (x0 + x1) * k
        cy += (y0 + y1) * k
    if abs(a) < 1e-9:
        xs, ys = zip(*ring)
        return 0.0, (sum(xs) / len(xs), sum(ys) / len(ys))
    return abs(a) / 2, (cx / (3 * a), cy / (3 * a))


def inside(rings, p):
    """Even-odd over every ring: works for a multipolygon's outers and inners."""
    x, y = p
    c = False
    for ring in rings:
        for i in range(len(ring) - 1):
            x0, y0 = ring[i]
            x1, y1 = ring[i + 1]
            if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                c = not c
    return c


def seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = 0.0 if dx == dy == 0 else clamp(((p[0] - ax) * dx + (p[1] - ay) * dy) / (dx * dx + dy * dy), 0, 1)
    return math.hypot(p[0] - ax - t * dx, p[1] - ay - t * dy)


def line_dist(p, pts):
    return min(seg_dist(p, pts[i], pts[i + 1]) for i in range(len(pts) - 1))


# ---------------------------------------------------------------- OpenStreetMap
MIRRORS = ["https://overpass-api.de/api/interpreter",
           "https://overpass.private.coffee/api/interpreter",
           "https://maps.mail.ru/osm/tools/overpass/api/interpreter"]
UA = "AnimatedPixelClock golf_courses.py (github.com/NickoScope/AnimatedPixelClock)"


def overpass(query):
    last = None
    for url in MIRRORS:
        for _ in range(2):
            try:
                req = urllib.request.Request(url, data=urllib.parse.urlencode({"data": query}).encode(),
                                             headers={"User-Agent": UA})
                with urllib.request.urlopen(req, timeout=180) as r:
                    d = json.loads(r.read().decode())
                if d.get("elements"):
                    return d
                last = "empty answer"
            except Exception as e:                      # a busy server: the next one
                last = e
            time.sleep(8)
    sys.exit(f"Overpass failed on every mirror: {last}")


def osm_rings(e, proj):
    """A way's or a relation's rings in metres."""
    if e["type"] == "way":
        g = [proj(p) for p in e.get("geometry", [])]
        return [g] if len(g) > 2 else []
    rings = []
    for m in e.get("members", []):
        g = [proj(p) for p in m.get("geometry", []) or []]
        if len(g) > 2:
            if g[0] != g[-1]:
                g.append(g[0])
            rings.append(g)
    return rings


def cmd_osm(args):
    card = json.loads((GOLF / f"{args.id}.card.json").read_text())
    if args.json:
        d = json.loads(pathlib.Path(args.json).read_text())
    else:
        bb = json.loads(urllib.request.urlopen(urllib.request.Request(
            f"https://nominatim.openstreetmap.org/lookup?osm_ids=R{args.relation}&format=json",
            headers={"User-Agent": UA}), timeout=30).read())[0]["boundingbox"]
        s, n, w, e = (float(x) for x in bb)
        pad = 0.002
        box = f"{s - pad},{w - pad},{n + pad},{e + pad}"
        d = overpass(f'[out:json][timeout:170];(nwr["golf"]({box});nwr["natural"~"^(water|wood)$"]({box});'
                     f'nwr["landuse"="forest"]({box});way["waterway"]({box}););out geom;')
    els = [e for e in d["elements"] if "tags" in e]
    lat0 = sum(p["lat"] for e in els for p in e.get("geometry", [])[:1]) / max(1, sum(1 for e in els if e.get("geometry")))
    kx, ky = 111320.0 * math.cos(math.radians(lat0)), 110540.0

    def proj(p):
        return (p["lon"] * kx, p["lat"] * ky)

    def kind(e):
        t = e["tags"]
        if t.get("golf") in ("hole", "green", "fairway", "bunker", "tee", "water_hazard", "lateral_water_hazard"):
            return "water" if "water_hazard" in t["golf"] else t["golf"]
        if t.get("natural") == "water":
            return "water"
        if t.get("natural") == "wood" or t.get("landuse") == "forest":
            return "wood"
        if t.get("waterway") in ("river", "canal", "stream", "ditch"):
            return "ww_" + t["waterway"]
        return None

    feats = {}
    for e in els:
        k = kind(e)
        if k:
            feats.setdefault(k, []).append(e)
    holes = {}
    for e in feats.get("hole", []):
        ref = "".join(ch for ch in e["tags"].get("ref", "") if ch.isdigit())
        if ref:
            holes[int(ref)] = [proj(p) for p in e["geometry"]]
    if sorted(holes) != list(range(1, 19)):
        sys.exit(f"holes mapped: {sorted(holes)} - need 1..18")

    polys = {k: [(osm_rings(e, proj), e) for e in feats.get(k, [])] for k in ("green", "fairway", "bunker", "water", "wood")}
    lines = [([proj(p) for p in e["geometry"]], {"ww_river": 3, "ww_canal": 2, "ww_stream": 1, "ww_ditch": 1}[k])
             for k in ("ww_river", "ww_canal", "ww_stream", "ww_ditch") for e in feats.get(k, [])]
    out = []
    for n in range(1, 19):
        line = holes[n]
        fr = Frame(line[0], line[-1], line)
        cl = [fr.screen(p) for p in line]
        cl = [(x, clamp(y, Y_MIN + 2, Y_MAX - 2)) for x, y in cl]
        h = {"n": n, "cl": cl}
        # the green: the polygon holding the cup, or the nearest one
        cup = line[-1]
        best = None
        for rings, _ in polys["green"]:
            if not rings:
                continue
            a, c = area_centroid(rings[0])
            dd = 0 if inside(rings, cup) else math.dist(c, cup)
            if best is None or dd < best[0]:
                best = (dd, a, c)
        if best and best[0] < 40:
            gx, gy = fr.screen(best[2])
            h["green"] = [gx, clamp(gy, Y_MIN, Y_MAX), clamp(round(math.sqrt(best[1] / math.pi) * fr.s), 3, 9)]
        else:
            h["green"] = [cl[-1][0], cl[-1][1], 6]
        # the fairway: the polygon the centre line runs through most
        samples = [(line[i][0] + (line[i + 1][0] - line[i][0]) * t, line[i][1] + (line[i + 1][1] - line[i][1]) * t)
                   for i in range(len(line) - 1) for t in [k / 20 for k in range(20)]]
        fbest = None
        for rings, _ in polys["fairway"]:
            if not rings:
                continue
            hits = sum(1 for p in samples if inside(rings, p))
            if hits and (fbest is None or hits > fbest[0]):
                fbest = (hits, rings)
        h["fw"] = [0, 0, 0]
        if fbest:
            ring = fbest[1][0]
            us = [clamp(fr.uv(p)[0], 0, 1) for p in ring]
            u0, u1 = min(us), max(us)
            a, _ = area_centroid(ring)
            hw = a / max(1.0, (u1 - u0) * fr.L) / 2
            h["fw"] = [clamp(round(hw * fr.s), 2, 7), round(u0 * 100), round(u1 * 100)]
        # bunkers on screen, nearest the line first
        bunkers = []
        for rings, _ in polys["bunker"]:
            if not rings:
                continue
            a, c = area_centroid(rings[0])
            x, y = fr.screen(c)
            if on_screen(x, y):
                bunkers.append((line_dist((x, y), cl), [x, y, clamp(round(math.sqrt(a / math.pi) * fr.s), 2, 4)]))
        h["bunkers"] = [b for _, b in sorted(bunkers)[:MAX_BUNKERS]]
        # water: a grid over the screen tested against every water polygon,
        # and the rivers and canals drawn as lines
        wpolys = [r for r, _ in polys["water"] if r]
        for step, rad in ((5, 3), (7, 4), (9, 5)):
            water = []
            for sx in range(2, 128, step):
                for sy in range(Y_MIN + 1, Y_MAX, step):
                    p = fr.world(sx, sy)
                    if any(inside(r, p) for r in wpolys):
                        water.append([sx, sy, rad])
            for pts, wr in lines:
                sp = [fr.screen(p) for p in pts]
                for i in range(len(sp) - 1):
                    (x0, y0), (x1, y1) = sp[i], sp[i + 1]
                    k = max(1, int(math.hypot(x1 - x0, y1 - y0) / max(3, step - 1)))
                    for j in range(k + 1):
                        x, y = x0 + (x1 - x0) * j / k, y0 + (y1 - y0) * j / k
                        if on_screen(x, y) and not any(abs(x - w[0]) < 3 and abs(y - w[1]) < 3 for w in water):
                            water.append([x, y, max(1, min(wr, rad))])
            if len(water) <= MAX_WATER:
                break
        h["water"] = water[:MAX_WATER]
        # trees: the mapped woods, sampled, never on the fairway or the green
        wood = [r for r, _ in polys["wood"] if r]
        fw = h["fw"]
        trees = []
        for sx in range(3, 127, 7):
            for sy in range(Y_MIN + 2, Y_MAX - 1, 7):
                jx, jy = sx + (sy * 3) % 4, sy + (sx * 5) % 3
                p = fr.world(jx, jy)
                if not any(inside(r, p) for r in wood):
                    continue
                if line_dist((jx, jy), cl) < fw[0] + 3 and fw[1] - 3 <= (jx - TEE_X) / SPAN * 100 <= fw[2] + 3:
                    continue
                g = h["green"]
                if math.hypot(jx - g[0], jy - g[1]) < g[2] + 3:
                    continue
                trees.append([jx, jy, 2 + (jx + jy) % 2])
        if len(trees) > MAX_TREES:
            k = len(trees) / MAX_TREES
            trees = [trees[int(i * k)] for i in range(MAX_TREES)]
        h["trees"] = trees
        out.append(h)
    save(args.id, card, out, "OpenStreetMap (golf=hole, green, fairway, bunker, water, woods), "
         "data (c) OpenStreetMap contributors, ODbL 1.0, https://www.openstreetmap.org/copyright")


# ---------------------------------------------------------------- the club's plans
def plan_class(p):
    r, g, b, a = p
    if a < 128:
        return 0                                      # outside the course
    if b > r + 15 and b >= g - 10 and b > 50:
        return 1                                      # water
    if r >= 200 and r - g >= 16 and b < 140:
        return 2                                      # sand
    if g > 150 and r > 100 and b < 110 and g >= r:
        return 3                                      # short grass
    if g < 75 and r < 45 and b < 40:
        return 4                                      # dark: a tree or a shadow
    return 5                                          # rough


def components(mask, w, h):
    seen = bytearray(w * h)
    out = []
    for i in range(w * h):
        if mask[i] and not seen[i]:
            stack, pts = [i], []
            seen[i] = 1
            while stack:
                j = stack.pop()
                pts.append(j)
                x, y = j % w, j // w
                for k in (j - 1 if x else -1, j + 1 if x < w - 1 else -1, j - w, j + w):
                    if 0 <= k < w * h and mask[k] and not seen[k]:
                        seen[k] = 1
                        stack.append(k)
            out.append(pts)
    return out


def cmd_plans(args):
    from PIL import Image, ImageFilter
    card = json.loads((GOLF / f"{args.id}.card.json").read_text())
    out = []
    for n in range(1, 19):
        im = Image.open(pathlib.Path(args.dir) / f"{n}.png").convert("RGBA")
        w, hgt = im.size
        px = im.load()
        cls = [plan_class(px[x, y]) for y in range(hgt) for x in range(w)]
        rows = [y for y in range(hgt) if any(cls[y * w + x] for x in range(w))]
        top, bot = rows[0], rows[-1]
        # the centre of play, row by row, bottom (tee) to top: the short grass
        # where a row has some (a lake beside the fairway must stay beside
        # it), the middle of the course shape where it has none
        raw = {}
        for y in range(top, bot + 1):
            gx_ = [x for x in range(w) if cls[y * w + x] == 3]
            xs = [x for x in range(w) if cls[y * w + x] != 0]
            if len(gx_) >= 6:
                raw[y] = sum(gx_) / len(gx_)
            elif xs:
                raw[y] = (min(xs) + max(xs)) / 2
        cent = {}
        for y in raw:
            near = [raw[k] for k in range(y - 10, y + 11) if k in raw]
            cent[y] = sum(near) / len(near)
        # the green: the biggest short-grass patch in the top fifth
        grass = bytearray(1 if (c == 3 and y < top + (bot - top) * 0.3) else 0
                          for y in range(hgt) for x in range(w) for c in [cls[y * w + x]])
        comps = [c for c in components(grass, w, hgt) if len(c) > 40]
        if comps:
            # the green is the top end of the topmost patch: its first rows
            gc = min(comps, key=lambda c: min(j // w for j in c))
            y0 = min(j // w for j in gc)
            head = [j for j in gc if j // w < y0 + (bot - top) * 0.07]
            gx = sum(j % w for j in head) / len(head)
            gy = sum(j // w for j in head) / len(head)
        else:
            gx, gy = cent[top + 10], top + 20
        # the tee: the light stripes of the tee boxes at the back
        ty0 = bot - (bot - top) * 0.14
        tee = [(x, y) for y in range(int(ty0), bot + 1) for x in range(w) if cls[y * w + x] == 3]
        if tee:
            tx, ty = sum(x for x, _ in tee) / len(tee), sum(y for _, y in tee) / len(tee)
        else:
            ty = bot - (bot - top) * 0.06
            tx = cent[int(ty)]
        # the line: tee, two points on the centre of the shape, the cup
        # five points between the tee and the cup, so a dogleg bends, not kinks
        ys = [ty - (ty - gy) * k for k in (0.2, 0.4, 0.6, 0.8)]
        pts = [(tx, ty)] + [(cent[int(y)], y) for y in ys if int(y) in cent] + [(gx, gy)]
        # the picture to the screen: along the hole from the tee to the cup,
        # across it one scale, centred like the OSM holes
        L = math.hypot(gx - tx, gy - ty)
        e = ((gx - tx) / L, (gy - ty) / L)
        nl = (e[1], -e[0])                                 # the player's left in the picture (y down)
        s = SPAN / L

        def uv(p):
            x, y = p[0] - tx, p[1] - ty
            return (x * e[0] + y * e[1]) / L, x * nl[0] + y * nl[1]

        vs = [uv(p)[1] for p in pts]
        v0 = (min(vs) + max(vs)) / 2

        def scr(p):
            u, v = uv(p)
            return TEE_X + u * SPAN, Y_MID - (v - v0) * s

        cl = [scr(p) for p in pts]
        cl = [(x, clamp(y, Y_MIN + 2, Y_MAX - 2)) for x, y in cl]
        hh = {"n": n, "cl": cl}
        g = scr((gx, gy))
        hh["green"] = [g[0], clamp(g[1], Y_MIN, Y_MAX), 7 if card["par"][n - 1] == 3 else 6]
        # fairway: the short grass along the middle, its width and extent
        widths, us = [], []
        for y in range(int(gy + (ty - gy) * 0.15), int(ty - (ty - gy) * 0.18)):
            xs = [x for x in range(w) if cls[y * w + x] == 3]
            if len(xs) > 6:
                widths.append(len(xs))
                us.append(uv((cent[y], y))[0])
        if widths and len(widths) > (ty - gy) * 0.15 and card["par"][n - 1] > 3:
            widths.sort()
            hw = widths[len(widths) // 2] / 2
            hh["fw"] = [clamp(round(hw * s), 2, 7), round(clamp(min(us), 0, 1) * 100), round(clamp(max(us), 0, 1) * 100)]
        else:
            hh["fw"] = [0, 0, 0]
        # sand: every patch big enough to be a bunker
        # the sand is speckled in the pictures: close the gaps before counting
        smask = Image.new("L", (w, hgt))
        smask.putdata([255 if c == 2 else 0 for c in cls])
        smask = smask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(3))
        sand = bytearray(1 if v else 0 for v in smask.getdata())
        bunkers = []
        for c in components(sand, w, hgt):
            if len(c) < 30:
                continue
            x = sum(j % w for j in c) / len(c)
            y = sum(j // w for j in c) / len(c)
            sx, sy = scr((x, y))
            if on_screen(sx, sy):
                bunkers.append((line_dist((sx, sy), cl), [sx, sy, clamp(round(math.sqrt(len(c) / math.pi) * s), 2, 4)]))
        hh["bunkers"] = [b for _, b in sorted(bunkers)[:MAX_BUNKERS]]

        def grid(want, step, rad, cap, jitter=False):
            res = []
            for sx in range(2, 128, step):
                for sy in range(Y_MIN + 1, Y_MAX, step):
                    jx, jy = (sx + (sy * 3) % 4, sy + (sx * 5) % 3) if jitter else (sx, sy)
                    # the screen back to the picture
                    u = (jx - TEE_X) / SPAN
                    v = v0 - (jy - Y_MID) / s
                    x = tx + u * L * e[0] + v * nl[0]
                    y = ty + u * L * e[1] + v * nl[1]
                    xi, yi = int(x), int(y)
                    if 0 <= xi < w and 0 <= yi < hgt:
                        # a small neighbourhood votes, so a speckle is not a lake
                        hits = sum(1 for dx in (-2, 0, 2) for dy in (-2, 0, 2)
                                   if 0 <= xi + dx < w and 0 <= yi + dy < hgt and cls[(yi + dy) * w + xi + dx] == want)
                        if hits >= 6:
                            res.append([jx, jy, rad if not jitter else 2 + (jx + jy) % 2])
            return res

        for step, rad in ((5, 3), (7, 4), (9, 5)):
            water = grid(1, step, rad, MAX_WATER)
            if len(water) <= MAX_WATER:
                break
        hh["water"] = water[:MAX_WATER]
        trees = [t for t in grid(4, 7, 3, MAX_TREES, jitter=True)
                 if math.hypot(t[0] - hh["green"][0], t[1] - hh["green"][1]) > hh["green"][2] + 3]
        if len(trees) > MAX_TREES:
            k = len(trees) / MAX_TREES
            trees = [trees[int(i * k)] for i in range(MAX_TREES)]
        hh["trees"] = trees
        out.append(hh)
    save(args.id, card, out, args.source or "the club's hole plans")


# ---------------------------------------------------------------- save and build
def save(cid, card, holes, layout_source):
    for h in holes:
        n = h["n"] - 1
        h["par"], h["len"], h["hcp"] = card["par"][n], card["len"][n], card["hcp"][n]
        for k in ("cl", "bunkers", "water", "trees"):
            h[k] = [[round(v) for v in p] for p in h[k]]
        h["green"] = [round(v) for v in h["green"]]
    doc = {"id": card["id"], "name": card["name"], "sub": card["sub"],
           "card": card["source"], "layout": layout_source, "holes": holes}
    path = GOLF / f"{cid}.json"
    path.write_text(json.dumps(doc, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")
    print("wrote", path.relative_to(REPO), "-",
          sum(len(h["bunkers"]) for h in holes), "bunkers,",
          sum(len(h["water"]) for h in holes), "water,",
          sum(len(h["trees"]) for h in holes), "trees")


B64 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$"


def enc(v):
    return B64[clamp(int(round(v)), 0, 63)]


def enc_pt(p):
    return enc(p[0] / 2) + enc(p[1])


def enc_c(c):
    return enc(c[0] / 2) + enc(c[1]) + enc(c[2])


def water_near(h):
    """The pond or river nearest the line between a fifth of the way and the
    green, worked out here once instead of on the panel every round: the same
    sums golf_course.lua made in make_hole, on the same quantised points.
    Returns (f in hundredths, the lateral offset in tenths of a pixel) or (0, 0)."""
    pts = [(clamp(int(round(p[0] / 2)), 0, 63) * 2, clamp(int(round(p[1])), 0, 63)) for p in h["cl"]]
    cum = [0.0]
    for i in range(1, len(pts)):
        cum.append(cum[-1] + math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]))
    total = cum[-1]

    def along(f, off):
        f = clamp(f, 0, 1)
        d = f * total
        i = 0
        while i < len(pts) - 2 and cum[i + 1] < d:
            i += 1
        (ax, ay), (bx, by) = pts[i], pts[i + 1]
        seg = cum[i + 1] - cum[i]
        k = (d - cum[i]) / seg if seg > 0 else 0
        dx, dy = bx - ax, by - ay
        n = math.hypot(dx, dy) or 1
        return ax + dx * k - dy / n * off, ay + dy * k + dx / n * off

    best = None
    for w in h["water"]:
        wx, wy, wr = clamp(int(round(w[0] / 2)), 0, 63) * 2, clamp(int(round(w[1])), 0, 63), clamp(int(round(w[2])), 0, 63)
        for i in range(4, 20):
            x, y = along(i / 20, 0)
            d = math.hypot(wx - x, wy - y) - wr
            if d <= 4 and (best is None or d < best[0]):
                best = (d, i / 20, wx, wy)
    if not best:
        return 0, 0
    x, y = along(best[1], 0)
    x2, y2 = along(best[1], 1)
    woff = clamp((best[2] - x) * (x2 - x) + (best[3] - y) * (y2 - y), -8, 8)
    return int(round(best[1] * 100)), int(round(woff * 10))


def lua_course(doc):
    rows = []
    for h in doc["holes"]:
        fw = h["fw"]
        wf, woff = water_near(h)
        s = (f'{h["par"]},{h["len"]},{h["hcp"]},{fw[0]},{fw[1]},{fw[2]},{wf},{woff}|'
             + "".join(enc_pt(p) for p in h["cl"]) + "|" + enc_c(h["green"]) + "|"
             + "".join(enc_c(c) for c in h["bunkers"]) + "|"
             + "".join(enc_c(c) for c in h["water"]) + "|"
             + "".join(enc_c(c) for c in h["trees"]))
        rows.append(f'  "{s}",')
    return (f'COURSES[#COURSES + 1] = {{id = "{doc["id"]}", name = "{doc["name"]}", sub = "{doc["sub"]}", holes = {{\n'
            + "\n".join(rows) + "\n}}\n")


BEGIN, END = "-- COURSES BEGIN (tools/luasim/golf_courses.py build)", "-- COURSES END"


def variant_name(cid):
    return f"golf_{cid}"


def cmd_build(args):
    docs = [json.loads((GOLF / f"{cid}.json").read_text()) for cid in ORDER]
    src = MASTER.read_text(encoding="utf-8")
    a, b = src.index(BEGIN), src.index(END)
    block = BEGIN + "\n" + "".join(lua_course(d) for d in docs)
    src = src[:a] + block + src[b:]
    MASTER.write_text(src, encoding="utf-8")
    print("wrote", MASTER.relative_to(REPO), len(src.encode()), "B")
    # the gallery copies only when asked: a course is tried here first
    gal = GALLERY / MASTER.name
    if args.gallery:
        gal.write_text(src, encoding="utf-8")
    # one script per course: the same game with the course fixed
    lines = src.splitlines(keepends=True)
    for d in docs:
        name = variant_name(d["id"])
        title = f'-- {name.upper()} - GOLF CLOCK on {d["name"].title() if d["name"].isascii() else d["name"]} ({d["sub"]}), '
        head = ["-- @upload-only\n",
                title + "eighteen real holes in two minutes.\n",
                f'-- Generated by tools/luasim/golf_courses.py build from golf_clock.lua; edit that, not this.\n',
                f'COURSE = "{d["id"]}"\n']
        body = "".join(head + lines[2:])
        for p in [HERE / "scripts" / f"{name}.lua"] + ([GALLERY / f"{name}.lua"] if args.gallery else []):
            p.write_text(body, encoding="utf-8")
        print("wrote", name + ".lua", len(body.encode()), "B")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    o = sub.add_parser("osm")
    o.add_argument("id")
    o.add_argument("--relation", type=int, help="the course's OSM relation id")
    o.add_argument("--json", help="an Overpass answer saved earlier, instead of fetching")
    p = sub.add_parser("plans")
    p.add_argument("id")
    p.add_argument("--dir", required=True, help="the plans, 1.png .. 18.png")
    p.add_argument("--source", help="where the plans came from, for the header")
    bl = sub.add_parser("build")
    bl.add_argument("--gallery", action="store_true", help="also write the copies in gallery/")
    args = ap.parse_args()
    {"osm": cmd_osm, "plans": cmd_plans, "build": cmd_build}[args.cmd](args)


if __name__ == "__main__":
    main()

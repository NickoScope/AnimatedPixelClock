"""The eight visualizer effects the owner picked on 2026-09-15 (styles 7-14),
as the reference src/viz/wow/ is held to pixel for pixel
(tools/audiofx/host/compare_wow.py).

Each effect takes the frame the onboard DSP produces 50 times a second
(tools/audiofx/dsp.py, src/audio/audio_dsp.h), or one derived from a PC packet
(src/viz/wow/viz_frame.cpp): band bytes, smoothed levels, peak holds,
bass/mid/treble, beats with a strength, the waveform.

Written so that C++ can repeat it exactly:
- scalars are Python floats, IEEE double; the host comparison builds C++ with double
- buffers that are float32 here are float in C++, and a Python scalar mixed
  into them is float32 first (NumPy 2 promotes weakly)
- no NumPy maths on scalars, no np.interp, no Mersenne Twister: XorShift32
- round() is half to even, like C's nearbyint(); % and // keep Python's signs
- the rotation, the grid offset and the hue wrap, so float on the panel stays exact

House style: black ground, bright but never blown out. No channel above 235.
"""
import math

import numpy as np

from gfx import BLACK, WHITE, H, W, RgbCanvas, rgb565

CAP = 235
SEED = 2463534242


class XorShift32:
    """Marsaglia's xorshift32; XorShift32 in src/viz/wow/wow_internal.h is its twin."""

    def __init__(self, seed=SEED):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return x

    def uniform(self, a, b):
        return a + (b - a) * (self.next() / 4294967296.0)


def col(r, g, b, k=1.0):
    return rgb565(min(CAP, max(0, int(r * k))), min(CAP, max(0, int(g * k))), min(CAP, max(0, int(b * k))))


def hsv(h, s, v):
    h = (h % 1.0) * 6.0
    i = int(h)
    f = h - i
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i % 6]
    return r * CAP, g * CAP, b * CAP


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inferno(t):
    """Black -> purple -> red -> orange -> pale yellow, capped at CAP."""
    stops = [(0.0, (0, 0, 0)), (0.18, (40, 8, 70)), (0.40, (140, 20, 90)), (0.62, (220, 60, 30)),
             (0.82, (235, 150, 20)), (1.0, (235, 225, 150))]
    t = min(1.0, max(0.0, t))
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0))
    return stops[-1][1]


class Wow:
    name = "wow"
    title = ""

    def __init__(self, react=1.0):
        self.rng = XorShift32()
        self.react = react              # settings.vizBeatFx / 100
        self.f = None
        self.beat_env = 0.0
        self.hue = 0.0
        self.hue_target = 0.0

    def update(self, frame):
        """Called once per frame, in order, so no beat is missed."""
        self.f = frame
        if frame["beat"] and self.react > 0.0:
            self.beat_env = max(self.beat_env, (0.55 + 0.45 * frame["strength"]) * self.react)
            self.hue_target += 0.07 * self.react
            if self.hue_target >= 64.0:     # exact in float and double; hsv() reads only the fraction
                self.hue_target -= 64.0
                self.hue -= 64.0
            self.on_beat(frame)

    def on_beat(self, frame):
        pass

    def step(self, dt):
        self.beat_env *= math.exp(-dt / 0.18)
        self.hue += (self.hue_target - self.hue) * (1.0 - math.exp(-dt / 0.25))

    def clock(self, cv, text):
        """drawVizClock() in src/viz/visualizer.cpp, as on every style."""
        cv.fill_rect(W - 34, 0, 34, 10, BLACK)
        cv.text(W - 31, 1, text, WHITE)


class PrismEQ(Wow):
    name, title = "w1_prism_eq", "Prism EQ (7)"

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        base, top = 59, 54
        gain = 0.80 + 0.20 * self.beat_env
        if self.beat_env > 0.05:
            cv.hline(0, base + 1, W, col(150, 70, 235, self.beat_env))
        for i in range(32):
            h = int(f["level"][i] * top)
            x = i * 4
            hue = 0.83 - i / 32.0 * 0.62 + self.hue
            for y in range(h):
                t = y / float(top)                        # colour follows the absolute height,
                lit = 0.45 + 0.55 * (y + 1) / float(h)    # light the bar's own top
                r, g, b = hsv(hue + t * 0.10, 0.95 - 0.45 * t, lit * gain)
                cv.hline(x, base - y, 3, col(r, g, b))
                if y < 4:   # floor reflection
                    cv.hline(x, base + 1 + y, 3, col(r, g, b, 0.30 - 0.07 * y))
            if h > 0:
                r, g, b = hsv(hue + 0.05, 0.30, gain)
                cv.hline(x, base - h, 3, col(r, g, b))
            pk = int(f["peak"][i] * top)
            if pk > 1:
                r, g, b = hsv(hue, 0.20, 0.95)
                cv.hline(x, base - pk - 1, 3, col(r, g, b))


class NeonMirrorPlus(Wow):
    name, title = "w2_neon_mirror_plus", "Neon Mirror+ (8)"

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        horizon, reach = 36, 26
        warm = min(1.0, self.beat_env * 0.8)
        top_a, top_b = mix((30, 210, 235), (235, 190, 60), warm), mix((235, 40, 200), (150, 60, 235), warm)
        for i in range(32):
            h = int(f["level"][i] * reach)
            pk = int(f["peak"][i] * reach)
            for side in (1, -1):
                x = 64 + 2 * i if side > 0 else 63 - 2 * i
                gx = x + side
                for y in range(1, h + 1):
                    c = mix(top_a, top_b, y / float(reach))
                    cv.pixel(x, horizon - y, col(*c))
                    cv.pixel(gx, horizon - y, col(*c, k=0.18))
                    cv.pixel(x, horizon + y, col(*c, k=0.45))
                if pk > 1:
                    cv.pixel(x, horizon - pk - 1, col(225, 235, 235))
                    cv.pixel(x, horizon + pk + 1, col(235, 120, 210, k=0.6))
        cv.hline(0, horizon, W, col(90, 30, 140, 0.45 + 0.55 * self.beat_env))


class Spectrogram(Wow):
    name, title = "w3_spectrogram", "Spectrogram (9)"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.idx = np.zeros((H, W), np.int32)       # 128x64 bytes of colour index in PSRAM on the panel
        self.ticks = [0] * W
        self.lut = [col(*inferno(i / 63.0)) for i in range(64)]

    def update(self, frame):
        super().update(frame)
        fp = [int(v) / 255.0 for v in frame["bands8"]]   # raw bytes: crisp in time
        column = [0] * H
        for r in range(H):          # linear between band centres 1, 3, ..., 63, flat beyond: np.interp's rule
            x = r + 0.5
            if x <= 1.0:
                v = fp[0]
            elif x >= 63.0:
                v = fp[31]
            else:
                j = int((x - 1.0) / 2.0)
                t = (x - (2 * j + 1)) / 2.0
                v = fp[j] + (fp[j + 1] - fp[j]) * t
            column[H - 1 - r] = int(min(63.0, max(0.0, v * 63.0)))   # bass at the bottom
        steps = frame.get("steps", 1)
        for s in range(steps):      # a PC packet stands for two DSP frames: same scroll speed
            self.idx[:, :-1] = self.idx[:, 1:]
            self.idx[:, -1] = column
            self.ticks = self.ticks[1:] + [1 if (frame["beat"] and s == steps - 1) else 0]

    def render(self, cv, now, dt):
        self.step(dt)
        for y in range(H):
            row = self.idx[y]
            for x in range(W):
                if row[x]:
                    cv.px[y, x] = self.lut[row[x]]
        for x in range(W):
            if self.ticks[x]:
                cv.vline(x, 0, 3, col(235, 200, 90))


class RadialBloom(Wow):
    name, title = "w4_radial_bloom", "Radial Bloom (10)"
    MAX_RINGS = 16

    def __init__(self, react=1.0):
        super().__init__(react)
        self.rot = 0.0
        self.rings = []

    def on_beat(self, frame):
        self.rings.append([6.0 + frame["bass"] * 6.0, 0.5 + 0.5 * frame["strength"]])
        self.rings = self.rings[-self.MAX_RINGS:]

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        self.rot = (self.rot + dt * (0.12 + f["treble"] * 0.5)) % (2 * math.pi)
        cx, cy = 64, 33
        r0 = 5.0 + f["bass"] * 7.0
        for ring in self.rings:
            ring[0] += 55.0 * dt
            ring[1] *= math.exp(-dt / 0.35)
            cv.circle(cx, cy, int(ring[0]), col(200, 90, 235, ring[1]))
        self.rings = [r for r in self.rings if r[1] > 0.04 and r[0] < 80]
        cv.fill_circle(cx, cy, max(1, int(r0) - 2), col(60, 20, 90, 0.5 + 0.5 * self.beat_env))
        for j in range(64):
            lobe = ((j + 0.5) / 16.0) % 2.0          # four mirrored lobes: bass top and bottom, treble at the sides
            b = min(31, int((lobe if lobe < 1.0 else 2.0 - lobe) * 32))
            a = self.rot + 2 * math.pi * j / 64.0 - math.pi / 2
            length = f["level"][b] * 21.0
            ca, sa = math.cos(a), math.sin(a)
            c = mix((150, 60, 235), (235, 70, 160), b / 31.0) if b < 20 else mix((235, 70, 160), (235, 180, 60), (b - 20) / 11.0)
            k = 0.55 + 0.45 * f["level"][b]
            x0, y0 = cx + ca * r0, cy + sa * r0
            x1, y1 = cx + ca * (r0 + length), cy + sa * (r0 + length)
            cv.line(round(x0), round(y0), round(x1), round(y1), col(*c, k=k))
            pk = r0 + f["peak"][b] * 21.0 + 1
            cv.pixel(round(cx + ca * pk), round(cy + sa * pk), col(235, 225, 210, 0.9))


class BeatParticles(Wow):
    name, title = "w5_beat_particles", "Beat Particles (11)"
    KEEP = 180

    def __init__(self, react=1.0):
        super().__init__(react)
        self.canvas = RgbCanvas()                   # 128x64x3 float32 in PSRAM on the panel
        self.parts = []
        self.stars = [[self.rng.uniform(0, W), self.rng.uniform(0, H), self.rng.uniform(0.2, 1.0)] for _ in range(50)]

    def on_beat(self, frame):
        n = 18 + int(40 * frame["strength"])
        groups = (frame["bass"], frame["mid"], frame["treble"])
        palette = [(235, 50, 130), (40, 210, 235), (235, 190, 70)][groups.index(max(groups))]
        for _ in range(n):
            a = -math.pi / 2 + self.rng.uniform(-1.05, 1.05)
            v = self.rng.uniform(45, 95) * (0.6 + frame["strength"])
            self.parts.append([64.0, 62.0, math.cos(a) * v, math.sin(a) * v, 1.0, palette])
        self.parts = self.parts[-self.KEEP:]

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        c = self.canvas
        c.fade(0.80)
        for s in self.stars:
            s[1] += dt * (4 + 20 * f["bass"]) * s[2]
            if s[1] >= H:
                s[0], s[1] = self.rng.uniform(0, W), 0.0
            c.add(s[0], s[1], 30 * s[2], 30 * s[2], 50 * s[2])
        for p in self.parts:
            p[3] += 60.0 * dt
            p[0] += p[2] * dt
            p[1] += p[3] * dt
            p[4] *= math.exp(-dt / 0.9)
            r, g, b = p[5]
            c.add(p[0], p[1], r * p[4], g * p[4], b * p[4])
        self.parts = [p for p in self.parts if p[4] > 0.05 and -4 < p[0] < W + 4 and p[1] < H + 2]
        width = int(f["bass"] * 60)
        for x in range(64 - width, 64 + width):
            c.add(x, 63, 120, 30, 90)
        c.blit(cv)


class ScopeAfterglow(Wow):
    name, title = "w6_scope_afterglow", "Scope Afterglow (12)"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.glow = np.zeros((H, W), np.float32)    # 128x64 float32 in PSRAM on the panel

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        self.glow *= math.exp(-dt / 0.09)
        cy, half = 36, 26
        prev = None
        for i in range(128):
            d = (int(f["wave"][i]) - 128) / 128.0
            y = int(round(cy - max(-1.0, min(1.0, d)) * half))
            if prev is not None:
                x0, y0 = prev
                steps = max(1, abs(y - y0))
                for s in range(steps + 1):
                    yy = y0 + (y - y0) * s // steps
                    if 0 <= yy < H:
                        self.glow[yy, i] = 1.0
                        if self.beat_env > 0.2:
                            for dy in (-1, 1):
                                if 0 <= yy + dy < H:
                                    self.glow[yy + dy, i] = max(self.glow[yy + dy, i], 0.5 * self.beat_env)
            prev = (i, y)
        grid = col(20, 70, 40, 0.5 + 0.5 * self.beat_env)
        for x in range(0, W, 16):
            for y in range(12, H, 3):
                cv.pixel(x, y, grid)
        for x in range(0, W, 3):
            cv.pixel(x, cy, grid)
        ys, xs = np.nonzero(self.glow > 0.04)
        for y, x in zip(ys, xs):
            g = float(self.glow[y, x])
            dev = abs(y - cy) / float(half)
            base = mix((40, 235, 120), (235, 190, 60), dev)
            core = mix(base, (190, 235, 200), max(0.0, g - 0.7) / 0.3) if g > 0.7 else base
            cv.pixel(x, y, col(*core, k=g))


class TwinVU(Wow):
    name, title = "w7_twin_vu", "Twin VU (13)"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.pos = [0.0, 0.0]
        self.vel = [0.0, 0.0]
        self.ghosts = [[], []]
        self.norm = [0.3, 0.3]

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        raw = [f["bass"], f["mid"] * 0.6 + f["treble"] * 0.4]
        targets = []
        for m in range(2):   # each meter ranges itself over the last few seconds
            self.norm[m] = max(self.norm[m] * math.exp(-dt / 4.0), raw[m], 0.15)
            targets.append(raw[m] / self.norm[m] * 0.92)
        for m in range(2):
            acc = 140.0 * (min(1.05, targets[m]) - self.pos[m]) - 16.0 * self.vel[m]   # VU-like, slight overshoot
            self.vel[m] += acc * dt
            self.pos[m] = min(1.08, max(-0.02, self.pos[m] + self.vel[m] * dt))
            self.ghosts[m] = ([self.pos[m]] + self.ghosts[m])[:6]
        for m, ox in enumerate((0, 64)):
            px, py, rad = ox + 32, 61, 44
            for k in range(21):
                a = math.radians(-50 + k * 5)
                red = k >= 16
                x, y = px + math.sin(a) * rad, py - math.cos(a) * rad
                cv.pixel(round(x), round(y), col(200, 40, 30) if red else col(150, 100, 30))
                if k % 5 == 0:
                    x2, y2 = px + math.sin(a) * (rad - 3), py - math.cos(a) * (rad - 3)
                    cv.line(round(x), round(y), round(x2), round(y2), col(150, 100, 30))
            for g, v in reversed(list(enumerate(self.ghosts[m]))):
                a = math.radians(-50 + 100 * v)
                k = 1.0 if g == 0 else 0.35 * (1 - g / 6.0)
                cv.line(px, py, round(px + math.sin(a) * (rad - 4)), round(py - math.cos(a) * (rad - 4)),
                        col(235, 225, 200, k))
            cv.fill_circle(px, py, 3, col(90, 60, 30, 0.6 + 0.4 * self.beat_env))
            cv.text(ox + 3, 54, "LO" if m == 0 else "HI", col(150, 100, 30))
            hot = self.pos[m] > 0.95 or f["clipping"]
            cv.fill_circle(ox + 58, 58, 2, col(235, 40, 30) if hot else col(50, 12, 8))


class Synthwave(Wow):
    name, title = "w8_synthwave_grid", "Synthwave Grid (14)"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.offset = 0.0

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        horizon = 30
        energy = (f["bass"] + f["mid"] + f["treble"]) / 3.0
        self.offset = (self.offset + dt * (0.7 + energy * 2.0 + self.beat_env * 2.5)) % 1.0
        for y in range(horizon):
            cv.hline(0, y, W, col(18, 4, 30, y / float(horizon)))
        sr = 13 + int(f["bass"] * 5)
        for dy in range(-sr, 1):
            half = int(math.sqrt(max(0, sr * sr - dy * dy)))
            y = horizon + dy
            if dy > -sr * 0.55 and (dy % 3 == 0):
                continue                               # the sun's stripes
            c = mix((235, 200, 60), (235, 50, 140), (dy + sr) / float(sr))
            cv.hline(64 - half, y, 2 * half + 1, col(*c, k=0.85 + 0.15 * self.beat_env))
        ridge = []
        for x in range(W):
            band = 31 - min(31, int(abs(x - 63.5) * 32 / 64.0))
            ridge.append(horizon - int(f["level"][band] * 20))
        for x in range(W):
            cv.vline(x, ridge[x] + 1, horizon - ridge[x], col(22, 6, 38))   # the mountains stand in front of the sun
            cv.pixel(x, ridge[x], col(40, 220, 235, 0.6 + 0.4 * f["level"][31 - min(31, int(abs(x - 63.5) * 32 / 64.0))]))
        grid_k = 0.45 + 0.55 * self.beat_env
        frac = self.offset % 1.0
        for k in range(1, 14):
            z = k - frac
            if z <= 0.15:
                continue
            y = horizon + int(round(34.0 / z))
            if horizon < y < H:
                cv.hline(0, y, W, col(200, 40, 180, grid_k * min(1.0, 3.0 / z)))
        for j in range(-8, 9):
            cv.line(64 + j * 3, horizon + 1, 64 + j * 26, H - 1, col(160, 30, 150, grid_k))
        cv.hline(0, horizon, W, col(235, 60, 180, 0.7))


ALL = [PrismEQ, NeonMirrorPlus, Spectrogram, RadialBloom, BeatParticles, ScopeAfterglow, TwinVU, Synthwave]
STYLE = {cls.name: 7 + i for i, cls in enumerate(ALL)}   # settings.vizStyle

"""The six visualizer styles that ship today, ported line by line from
src/viz/visualizer.cpp, starfield.cpp and oscilloscope.cpp, so a preview shows
what the panel would draw from the same packets.

Input is exactly what vizIngest() stores: 32 band bytes, 128 waveform bytes,
the packet time and a serial. Defaults are the firmware's: vizShowClock on,
the colour slots from src/config/settings.cpp, the scope options from
applyScopeDefaults().
"""
import math
import random

from gfx import BLACK, WHITE, H, W, lround, rgb565, rgbc, unpack

VIZ_BANDS = 32
VIZ_WAVE_POINTS = 128
VIZ_BAR_W = 3
VIZ_MAX_H = 56.0
VIZ_SMOOTH = 0.35
VIZ_PEAK_GRAVITY = 60.0
WATERFALL_ROWS = 26
SCOPE_TRAIL_MAX = 4
COL_VIZ_LOW, COL_VIZ_MID, COL_VIZ_PEAK = 0x07E0, 0xFFE0, 0xF800
COL_SCOPE_GRID, COL_SCOPE_TRACE, COL_SCOPE_PEAK = 0x07E0, 0xFFE0, 0xF800

STYLES = {0: "classic_eq", 1: "neon_mirror", 2: "phosphor_waterfall", 3: "purple_led_stage",
          5: "starfield_overdrive", 6: "oscilloscope"}


class Settings:
    vizShowClock = True
    scopeGrid = True
    scopeFill = False
    scopeFlat = False
    scopeTrail = 3
    scopeGain = 100
    clock = "18:27"


class Starfield:
    def __init__(self, seed=1):
        self.rng = random.Random(seed)
        self.stars = [[0.0, 0.0, 0.0] for _ in range(96)]
        self.drive = self.boost = self.cooldown = self.phase = 0.0
        self.previousBass = [0.0] * 8
        self.fluxAverage = self.packetAge = 0.0
        self.seenPacket = 0
        self.trailBaseline = [0.0] * 32

    def spawn(self, s):
        s[0] = self.rng.randrange(-1000, 1001) / 1000.0
        s[1] = self.rng.randrange(-1000, 1001) / 1000.0
        if abs(s[0]) + abs(s[1]) < 0.08:
            s[0] = 0.12
        s[2] = self.rng.randrange(650, 1001) / 1000.0

    def draw(self, cv, levels, raw, serial, dt, reset, show_clock):
        bass = sum(levels[i] / 8.0 for i in range(8))
        mids = sum(levels[i] / 16.0 for i in range(8, 24))
        if reset:
            for s in self.stars:
                self.spawn(s)
                s[2] = self.rng.randrange(100, 1001) / 1000.0
            for i in range(8):
                self.previousBass[i] = raw[i] / 255.0
            self.seenPacket = serial
            self.fluxAverage = self.packetAge = 0.0
            self.drive = bass
            self.boost = self.cooldown = self.phase = 0.0
            self.trailBaseline = list(levels)
        self.cooldown = max(0.0, self.cooldown - dt)
        self.boost *= math.exp(-dt * 12.0)
        self.packetAge += dt
        if serial != self.seenPacket:
            flux = rawBass = oldBass = 0.0
            for i in range(8):
                value = raw[i] / 255.0
                flux += max(0.0, value - self.previousBass[i]) / 8.0
                rawBass += value / 8.0
                oldBass += self.previousBass[i] / 8.0
                self.previousBass[i] = value
            threshold = max(0.035, self.fluxAverage * 1.8)
            if (self.packetAge < 0.25 and rawBass > 0.12 and rawBass > oldBass + 0.025
                    and flux > threshold and self.cooldown <= 0.0):
                self.boost = min(1.0, 0.40 + (flux - threshold) * 4.0)
                self.cooldown = 0.16
            if self.packetAge >= 0.25:
                self.fluxAverage = 0.0
            else:
                self.fluxAverage += (flux - self.fluxAverage) * (1.0 - math.exp(-self.packetAge * 2.0))
            self.packetAge = 0.0
            self.seenPacket = serial
        self.drive += (bass - self.drive) * (1.0 - math.exp(-dt * 6.0))
        self.phase = math.fmod(self.phase + dt * 0.22, 6.2831853)
        cx = 63.5 + math.sin(self.phase) * (2.0 + mids * 4.0)
        cy = (36.0 if show_clock else 31.5) + math.cos(self.phase * 2) * 2.0
        speed = 0.18 + self.drive * 0.65 + self.boost * 1.60
        accents = [0.0] * 32
        trailSmooth = 1.0 - math.exp(-dt * 3.0)
        for i in range(32):
            self.trailBaseline[i] += (levels[i] - self.trailBaseline[i]) * trailSmooth
            accents[i] = min(1.0, max(0.0, levels[i] - self.trailBaseline[i]) * 3.0)
        for i in range(96):
            s = self.stars[i]
            s[2] -= speed * dt
            if s[2] <= 0.06:
                self.spawn(s)
                continue
            px = cx + s[0] / s[2] * 44.0
            py = cy + s[1] / s[2] * 26.0
            if px < 0 or px >= W or py < 0 or py >= H:
                self.spawn(s)
                continue
            level = levels[i % 32]
            depth = 1.0 - s[2]
            accent = accents[i % 32]
            light = min(1.0, 0.16 + depth * 0.36 + level * 0.28 + accent * 0.10 + self.boost * 0.15)
            if i % 32 < 10:
                r, g, b = 190, 110, 255
            elif i % 32 < 24:
                r, g, b = 100, 220, 255
            else:
                r, g, b = 255, 215, 145
            r, g, b = int(r * light), int(g * light), int(b * light)
            dx, dy = px - cx, py - cy
            distance = math.sqrt(dx * dx + dy * dy)
            length = 2.0 + depth * 1.5 + level * 1.5 + accent + self.boost * 12.0
            length = min(length, distance * 0.8)
            fraction = length / distance if distance > 0.001 else 0.0
            tx, ty = px - dx * fraction, py - dy * fraction
            x, y = int(px), int(py)
            mx, my = int((tx + px) * 0.5), int((ty + py) * 0.5)
            cv.line(int(tx), int(ty), mx, my, rgb565(r // 4, g // 4, b // 4))
            cv.line(mx, my, x, y, rgb565(r // 2, g // 2, b // 2))
            white = 0.5 + self.boost * 0.4
            cv.pixel(x, y, rgb565(r + int((255 - r) * white), g + int((255 - g) * white), b + int((255 - b) * white)))
            if depth > 0.60 and level > 0.70:
                dim = rgb565(r // 3, g // 3, b // 3)
                cv.pixel(x - 1, y, dim)
                cv.pixel(x + 1, y, dim)
                cv.pixel(x, y - 1, dim)
                cv.pixel(x, y + 1, dim)


class Scope:
    def __init__(self):
        self.trail = [[128] * VIZ_WAVE_POINTS for _ in range(SCOPE_TRAIL_MAX)]
        self.previousWave = [128] * VIZ_WAVE_POINTS
        self.trailUsed = [False] * SCOPE_TRAIL_MAX
        self.trailHead = 0
        self.seenSerial = 0
        self.everSeen = False

    @staticmethod
    def trace(cv, wave, cy, half, gain, light, p, st):
        prevX = prevY = 0
        centre = lround(cy)
        for i in range(VIZ_WAVE_POINTS):
            deflect = max(-1.0, min(1.0, (int(wave[i]) - 128) / 128.0 * gain))
            y = lround(cy - deflect * half)
            hot = 0.0 if st.scopeFlat else abs(deflect)
            color = rgbc((p[0] + (p[3] - p[0]) * hot) * light, (p[1] + (p[4] - p[1]) * hot) * light,
                         (p[2] + (p[5] - p[2]) * hot) * light)
            if st.scopeFill:
                body = rgbc((p[0] + (p[3] - p[0]) * hot) * light * 0.45, (p[1] + (p[4] - p[1]) * hot) * light * 0.45,
                            (p[2] + (p[5] - p[2]) * hot) * light * 0.45)
                cv.line(i, centre, i, y, body)
            if i > 0:
                cv.line(prevX, prevY, i, y, color)
            else:
                cv.pixel(i, y, color)
            prevX, prevY = i, y

    def draw(self, cv, wave, serial, stale, dt, reset, st):
        if reset or wave is None or stale:
            self.trailUsed = [False] * SCOPE_TRAIL_MAX
            self.trailHead = 0
            self.seenSerial = serial
            self.everSeen = False
        cy = 36.5 if st.vizShowClock else 31.5
        half = 26.5 if st.vizShowClock else 31.5
        top, bottom, centre = lround(cy - half), lround(cy + half), lround(cy)
        gr, gg, gb = unpack(COL_SCOPE_GRID)
        p = unpack(COL_SCOPE_TRACE) + unpack(COL_SCOPE_PEAK)
        if st.scopeGrid:
            grid = rgbc(gr / 5, gg / 5, gb / 5)
            axis = rgbc(gr / 3, gg / 3, gb / 3)
            for x in range(16, W, 16):
                for y in range(top, bottom + 1, 2):
                    cv.pixel(x, y, grid)
            for step in (-2, -1, 1, 2):
                y = centre + lround(step * half / 2.5)
                for x in range(0, W, 2):
                    cv.pixel(x, y, grid)
            for x in range(W):
                cv.pixel(x, centre, axis)
            for x in range(4, W, 4):
                cv.pixel(x, centre - 1, axis)
                cv.pixel(x, centre + 1, axis)
        if wave is None:
            cv.text(4, centre - 20, "Update PC companion", rgbc(p[3], p[4], p[5]))
            cv.text(4, centre + 12, "for the waveform", rgbc(p[3], p[4], p[5]))
            return
        if stale:
            return
        depth = 3 if st.scopeTrail > SCOPE_TRAIL_MAX else st.scopeTrail
        if serial != self.seenSerial or not self.everSeen:
            if self.everSeen and depth > 0:
                self.trailHead = (self.trailHead + SCOPE_TRAIL_MAX - 1) % SCOPE_TRAIL_MAX
                self.trail[self.trailHead] = list(self.previousWave)
                self.trailUsed[self.trailHead] = True
            self.previousWave = list(wave)
            self.seenSerial = serial
            self.everSeen = True
        gain = st.scopeGain / 100.0
        for age in range(depth - 1, -1, -1):
            slot = (self.trailHead + age) % SCOPE_TRAIL_MAX
            if self.trailUsed[slot]:
                self.trace(cv, self.trail[slot], cy, half, gain, 0.38 - age * 0.10, p, st)
        self.trace(cv, wave, cy, half, gain, 1.0, p, st)


class Visualizer:
    """displayVisualizer() and the state vizIngest() keeps."""

    def __init__(self, style, settings=None, seed=1):
        self.style = style
        self.st = settings or Settings()
        self.bands = [0] * VIZ_BANDS
        self.wave = [128] * VIZ_WAVE_POINTS
        self.wave_ever = False
        self.wave_serial = 0
        self.last_rx = None
        self.packet_serial = 0
        self.barH = [0.0] * VIZ_BANDS
        self.peakY = [0.0] * VIZ_BANDS
        self.peakVel = [0.0] * VIZ_BANDS
        self.last_frame = 0
        self.waterfall = [[0] * VIZ_BANDS for _ in range(WATERFALL_ROWS)]
        self.wf_head = 0
        self.last_wf_row = 0
        self.last_style = 255
        self.star = Starfield(seed)
        self.scope = Scope()

    def ingest(self, bands8, wave, now_ms):
        self.bands = [int(v) for v in bands8]
        if wave is not None:
            self.wave = [int(v) for v in wave]
            self.wave_ever = True
            self.wave_serial += 1
        else:
            self.wave_ever = False
        self.last_rx = int(now_ms)
        self.packet_serial += 1

    def recent(self, now, max_age):
        return self.last_rx is not None and now - self.last_rx <= max_age

    def render(self, cv, now):
        now = int(now)
        st = self.st
        reset = self.style != self.last_style or now - self.last_frame > 250
        if reset:
            self.waterfall = [[0] * VIZ_BANDS for _ in range(WATERFALL_ROWS)]
            self.wf_head = 0
            self.last_wf_row = now - 40
            self.last_style = self.style
        dt = min(0.1, (now - self.last_frame) / 1000.0)
        self.last_frame = now
        stale = not self.recent(now, 2000)
        lowZone, midZone = 28, 45
        smooth = VIZ_SMOOTH if self.style == 0 else 1.0 - math.exp(-26.0 * dt)
        for i in range(VIZ_BANDS):
            target = 0.0 if stale else self.bands[i] * (VIZ_MAX_H / 255.0)
            self.barH[i] += (target - self.barH[i]) * smooth
            if self.barH[i] >= self.peakY[i]:
                self.peakY[i] = self.barH[i]
                self.peakVel[i] = 0.0
            else:
                self.peakVel[i] += VIZ_PEAK_GRAVITY * dt
                self.peakY[i] -= self.peakVel[i] * dt
                if self.peakY[i] < 0:
                    self.peakY[i] = 0.0
            if self.style != 0:
                continue
            h = int(self.barH[i])
            x = i * 4
            if h > 0:
                hLow = min(h, lowZone)
                cv.fill_rect(x, H - hLow, VIZ_BAR_W, hLow, COL_VIZ_LOW)
                if h > lowZone:
                    hMid = min(h, midZone) - lowZone
                    cv.fill_rect(x, H - lowZone - hMid, VIZ_BAR_W, hMid, COL_VIZ_MID)
                if h > midZone:
                    cv.fill_rect(x, H - h, VIZ_BAR_W, h - midZone, COL_VIZ_PEAK)
            if self.peakY[i] > 1:
                cv.hline(x, H - 1 - int(self.peakY[i]), VIZ_BAR_W, COL_VIZ_PEAK)
        if self.style == 1:
            self.neon_mirror(cv)
        elif self.style == 2:
            self.phosphor_waterfall(cv, now, stale)
        elif self.style == 3:
            self.purple_stage(cv, now)
        elif self.style == 5:
            levels = [b / VIZ_MAX_H for b in self.barH]
            self.star.draw(cv, levels, self.bands, self.packet_serial, dt, reset, st.vizShowClock)
        elif self.style == 6:
            self.scope.draw(cv, self.wave if self.wave_ever else None, self.wave_serial, stale, dt, reset, st)
        if stale:
            cv.text(25, 28, "No audio data...", WHITE)
        if st.vizShowClock:
            cv.fill_rect(W - 34, 0, 34, 10, BLACK)
            cv.text(W - 31, 1, st.clock, WHITE)

    def neon_mirror(self, cv):
        st = self.st
        horizon = 36 if st.vizShowClock else 32
        if st.vizShowClock:
            cv.hline(0, horizon, W, rgb565(45, 12, 70))
        for i in range(VIZ_BANDS):
            h = int(self.barH[i] * (24.0 / VIZ_MAX_H))
            peak = int(self.peakY[i] * (24.0 / VIZ_MAX_H))
            x = i * 4
            for y in range(1, h + 1):
                if y % 3 == 0:
                    continue
                mix = y * 255 // 24
                cv.hline(x, horizon - y, VIZ_BAR_W, rgb565(40 + mix * 215 // 255, 235 - mix * 185 // 255, 255))
                cv.hline(x, horizon + y, VIZ_BAR_W, rgb565(100 + mix * 100 // 255, 20 + mix * 30 // 255, 160))
            if peak > 1:
                cv.hline(x, horizon - peak, VIZ_BAR_W, rgb565(210, 255, 255))
                cv.hline(x, horizon + peak, VIZ_BAR_W, rgb565(255, 100, 210))

    def phosphor_waterfall(self, cv, now, stale):
        st = self.st
        steps = (now - self.last_wf_row) // 40
        if steps > 0:
            self.last_wf_row = now - (now - self.last_wf_row) % 40
            for _ in range(min(steps, WATERFALL_ROWS)):
                self.wf_head = (self.wf_head + WATERFALL_ROWS - 1) % WATERFALL_ROWS
                self.waterfall[self.wf_head] = [0 if stale else int(b * (255.0 / VIZ_MAX_H)) & 0xFF for b in self.barH]
        if st.vizShowClock:
            cv.hline(0, 10, W, rgb565(0, 65, 34))
        top = 12 if st.vizShowClock else 0
        for row in range(WATERFALL_ROWS):
            fade = 255 - row * 7
            y = top + row * (H - top) // WATERFALL_ROWS
            nextY = top + (row + 1) * (H - top) // WATERFALL_ROWS
            for i in range(VIZ_BANDS):
                level = self.waterfall[(self.wf_head + row) % WATERFALL_ROWS][i]
                if level < 5:
                    continue
                if level < 128:
                    r, g, b = 0, level * 2, level // 2
                elif level < 208:
                    r, g, b = (level - 128) * 2, 255, 64 + (level - 128)
                else:
                    r, g, b = 255, 225 - (level - 208), 80 - (level - 208)
                cv.fill_rect(i * 4, y, VIZ_BAR_W, nextY - y, rgb565(r * fade // 255, g * fade // 255, b * fade // 255))

    def purple_stage(self, cv, now):
        bass = sum(self.barH[i] / (6.0 * VIZ_MAX_H) for i in range(6))
        treble = sum(self.barH[i] / (8.0 * VIZ_MAX_H) for i in range(24, VIZ_BANDS))
        phase = (now % 60000) * (6.2831853 / 3000.0)
        for i in range(VIZ_BANDS):
            level = self.barH[i] / VIZ_MAX_H
            side = (i - 15.5) / 15.5
            center = 7.5 + side * side * 3.5 + math.sin(i * 0.28 - phase) * (1.0 + bass * 2.5)
            reach = 1.0 + level * 6.0 + bass * 2.0
            for row in range(H // 4):
                distance = abs(row - center)
                glow = max(0.0, 1.0 - distance / reach)
                rim = max(0.0, 1.0 - abs(distance - reach) * 1.4)
                light = min(1.0, glow * (0.2 + level * 0.65) + rim * bass * 0.4)
                hot = int(glow * glow * treble * 160.0)
                r = 12 + int(light * 210.0)
                g = 2 + int(light * 24.0) + hot
                b = 24 + int(light * 200.0)
                x, y = i * 4 + 1, row * 4 + 1
                halo = rgb565(r // 4, g // 4, b // 4)
                cv.hline(x - 1, y, 4, halo)
                cv.vline(x, y - 1, 4, halo)
                cv.fill_rect(x, y, 2, 2, rgb565(r, g, b))

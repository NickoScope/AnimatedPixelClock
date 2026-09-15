"""Audio-reactive Matrix Rain: three variants for visualizer style 2, replacing
Phosphor Waterfall. Previews only; the owner picks one.

Reused from the Matrix Rain clock style (src/clocks/clock_matrix.cpp) so the
panel stays consistent: the 21 x 8 grid of 6 x 8 px cells with a 1 px margin,
its charset (digits, caps and dense symbols from the GFX 5x7 font, standing in
for katakana), its fading trail built as a 32-level ramp of COL_MATRIX_RAIN,
the white-green head of COL_MATRIX_HEAD, glyphs that change as the head enters a
row and mutate inside the trail. Both colours are capped at 235 (house style):
the clock's defaults are 0x07E8 and 0xDFFB, whose green is 255.

Each reads the VizFrame the wow effects read (effects_wow.py), so it works from
the microphones' 20 ms frames and from the PC's 40 ms packets through
PcFrameDeriver. Written like effects_wow.py for a later pixel-identical C++
port: xorshift32, integer choices, floor instead of int() for negatives.
"""
import math

from effects_wow import Wow
from gfx import BLACK, W, rgb565

COLS, ROWS, CW, CH, XOFF = 21, 8, 6, 8, 1           # clock_matrix.cpp MX_COLS, MX_ROWS, MX_CELL_W/H, MX_X_OFF
CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$+-*/=%#&<>@"   # MX_CHARSET
FADE = 32                                           # MX_FADE_LEVELS
RAIN5 = (0, 58, 8)                                  # COL_MATRIX_RAIN 0x07E8 as r5/g6/b5, green 63 -> 58 (235)
HEAD = rgb565(216, 235, 216)                        # COL_MATRIX_HEAD 0xDFFB, green 255 -> 235
MUTATE_RATE = 1.2                                   # MX_MUTATE_RATE, mutations per visible cell per second


def fade_lut(pct=100):
    """mxBuildFade(): level i of 32 is (i + 1) / 32 of the rain colour, scaled by pct."""
    r, g, b = RAIN5
    return [((r * (i + 1) * pct // (FADE * 100)) << 11) | ((g * (i + 1) * pct // (FADE * 100)) << 5)
            | (b * (i + 1) * pct // (FADE * 100)) for i in range(FADE)]


def col_level(f, c):
    """Column c of 21 covers bands c*32//21 .. (c+1)*32//21: bass on the left, as the EQ styles."""
    lo = c * 32 // COLS
    hi = max(lo + 1, (c + 1) * 32 // COLS)
    return max(f["level"][lo:hi])


class MatrixBase(Wow):
    BASE_SPEED = 3.0        # rows/s in silence: a slow drizzle, never a black screen

    def __init__(self, react=1.0):
        super().__init__(react)
        self.lut = fade_lut()
        self.chars = [[self.rand_char() for _ in range(ROWS)] for _ in range(COLS)]
        # per column: active, head row (fractional), speed, trail rows, respawn seconds
        self.cols = [[False, 0.0, 0.0, 3, self.rng.uniform(0.0, 1.5)] for _ in range(COLS)]

    def rand_char(self):
        return CHARSET[self.rng.next() % len(CHARSET)]

    def spawn(self, c, head0):
        col = self.cols[c]
        col[0], col[1] = True, head0
        col[2] = self.BASE_SPEED * self.rng.uniform(0.7, 1.3)
        col[3] = 3 + self.rng.next() % 5                # 3-7 rows, as the clock
        col[4] = 0.0

    def advance(self, c, speed, dt, chance, respawn_lo, respawn_hi):
        """updateMatrixAnimation()'s column step: new glyphs where the head enters, mutation in the trail."""
        col = self.cols[c]
        prev = math.floor(col[1])
        col[1] += speed * dt
        head = math.floor(col[1])
        for r in range(max(0, prev + 1), min(head, ROWS - 1) + 1):
            self.chars[c][r] = self.rand_char()
        if head - col[3] >= ROWS:
            col[0] = False
            col[4] = self.rng.uniform(respawn_lo, respawn_hi)
            return
        for k in range(1, col[3] + 1):
            r = head - k
            if 0 <= r < ROWS and self.rng.next() % 1000 < chance:
                self.chars[c][r] = self.rand_char()

    def draw_rain(self, cv, lut, head_color, boost=0):
        """drawMatrixRain(): the head in its own colour, the trail faded off the fractional head."""
        for c, col in enumerate(self.cols):
            if not col[0]:
                continue
            head = math.floor(col[1])
            x = XOFF + c * CW
            for k in range(col[3] + 1):
                r = head - k
                if not 0 <= r < ROWS:
                    continue
                if k == 0:
                    color = head_color
                else:
                    t = max(0.0, 1.0 - (col[1] - r) / (col[3] + 1))
                    color = lut[min(FADE - 1, int(t * (FADE - 1)) + boost)]
                cv.text(x, r * CH, self.chars[c][r], color)


class SpectrumRain(MatrixBase):
    """Every column is a band: loud bands rain fast, often and with long trails. A beat glitches
    every visible glyph for 120 ms and drops a new head in every loud column."""
    name, title = "m1_spectrum_rain", "Spectrum Rain"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.glitch = 0.0

    def on_beat(self, f):
        self.glitch = 0.12
        for c, col in enumerate(self.cols):
            if not col[0] and col_level(f, c) > 0.25:
                self.spawn(c, 0.0)

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        self.glitch = max(0.0, self.glitch - dt)
        chance = 1000 if self.glitch > 0.0 else int(MUTATE_RATE * (1.0 + 4.0 * f["treble"]) * dt * 1000.0)
        for c, col in enumerate(self.cols):
            lvl = col_level(f, c)
            if not col[0]:
                col[4] -= dt * (1.0 + 8.0 * lvl)          # loud bands respawn sooner
                if col[4] <= 0.0:
                    self.spawn(c, 0.0)
                continue
            col[3] = min(7, 3 + int(lvl * 6.0))           # and grow longer trails
            self.advance(c, col[2] * (1.0 + 2.5 * lvl + 1.5 * f["bass"]), dt, chance, 0.6, 2.5)
        self.draw_rain(cv, self.lut, HEAD, boost=int(self.beat_env * 10.0))


class BassCurtain(MatrixBase):
    """The whole rain breathes with the bass: it races on a kick and drifts in the quiet. A beat
    drops a curtain of new heads across every idle column; treble lights random trail glyphs."""
    name, title = "m2_bass_curtain", "Bass Curtain"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.drive = 0.0

    def on_beat(self, f):
        for c, col in enumerate(self.cols):
            if not col[0]:
                self.spawn(c, -self.rng.uniform(0.0, 2.5))   # staggered, so the curtain has a ragged edge

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        self.drive += (f["bass"] - self.drive) * (1.0 - math.exp(-dt / 0.15))
        energy = (f["bass"] + f["mid"] + f["treble"]) / 3.0
        chance = int(MUTATE_RATE * dt * 1000.0)
        for c, col in enumerate(self.cols):
            if not col[0]:
                col[4] -= dt * (0.3 + 3.0 * energy)
                if col[4] <= 0.0:
                    self.spawn(c, 0.0)
                continue
            self.advance(c, col[2] * (1.0 + 4.0 * self.drive), dt, chance, 0.8, 3.0)
        self.draw_rain(cv, self.lut, HEAD, boost=int(self.beat_env * 8.0))
        for _ in range(int(f["treble"] * 14.0)):           # sparkles
            c = self.rng.next() % COLS
            col = self.cols[c]
            if not col[0]:
                continue
            r = math.floor(col[1]) - 1 - self.rng.next() % col[3]
            if 0 <= r < ROWS:
                cv.text(XOFF + c * CW, r * CH, self.chars[c][r], HEAD)


class CodeEQ(MatrixBase):
    """A dim rain behind a bar of bright glyphs rising from the bottom of each column to its band's
    level, headed by a white glyph with a held peak above. A beat tears a glitch line across."""
    name, title = "m3_code_eq", "Code EQ"

    def __init__(self, react=1.0):
        super().__init__(react)
        self.dim = fade_lut(45)
        self.stack = [[self.rand_char() for _ in range(ROWS)] for _ in range(COLS)]
        self.glitch = 0.0
        self.glitch_row = 0

    def on_beat(self, f):
        self.glitch = 0.10
        self.glitch_row = self.rng.next() % ROWS

    def render(self, cv, now, dt):
        f = self.f
        self.step(dt)
        self.glitch = max(0.0, self.glitch - dt)
        chance = int(MUTATE_RATE * dt * 1000.0)
        for c, col in enumerate(self.cols):
            if not col[0]:
                col[4] -= dt
                if col[4] <= 0.0:
                    self.spawn(c, 0.0)
                continue
            self.advance(c, col[2] * (1.0 + 2.0 * f["bass"]), dt, chance, 1.0, 3.5)
        self.draw_rain(cv, self.dim, self.dim[FADE - 1])
        stack_chance = int(MUTATE_RATE * (2.0 + 8.0 * f["treble"]) * dt * 1000.0)
        for c in range(COLS):
            x = XOFF + c * CW
            hgt = math.floor(col_level(f, c) * ROWS + 0.5)
            for i in range(hgt):
                r = ROWS - 1 - i
                if self.rng.next() % 1000 < stack_chance:
                    self.stack[c][r] = self.rand_char()
                cv.fill_rect(x, r * CH, CW, CH, BLACK)
                color = HEAD if i == hgt - 1 else self.lut[min(FADE - 1, 12 + i * 19 // max(1, hgt - 1))]
                cv.text(x, r * CH, self.stack[c][r], color)
            pk = math.floor(f["peak"][c * 32 // COLS] * ROWS + 0.5)
            if hgt < pk <= ROWS:
                r = ROWS - pk
                cv.fill_rect(x, r * CH, CW, CH, BLACK)
                cv.text(x, r * CH, self.stack[c][r], rgb565(120, 200, 140))
        if self.glitch > 0.0:
            y = self.glitch_row * CH
            for c in range(COLS):
                cv.fill_rect(XOFF + c * CW, y, CW, CH, BLACK)
                cv.text(XOFF + c * CW, y, self.rand_char(), HEAD if self.rng.next() % 3 else self.lut[20])


ALL = [SpectrumRain, BassCurtain, CodeEQ]

# PSRAM per variant, as the C++ state would lay out (floats, uint8 glyph codes):
# 21 columns x 16 B + 21 x 8 glyphs + the Wow scalars ~24 B.
PSRAM_BYTES = {"m1_spectrum_rain": 21 * 16 + 21 * 8 + 24 + 4,
               "m2_bass_curtain": 21 * 16 + 21 * 8 + 24 + 4,
               "m3_code_eq": 21 * 16 + 21 * 8 * 2 + 24 + 8}
_ = W

/*
 * Visualizer style 2: Code EQ, the owner's pick of 2026-09-15 to replace
 * Phosphor Waterfall. CodeEQ and MatrixBase in tools/audiofx/effects_matrix.py,
 * line by line, with the Matrix Rain clock style's grid, charset, fade and
 * colours (src/clocks/clock_matrix.cpp). Where Python reads two random numbers in
 * one call, this reads them into locals in the same order.
 */
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include "wow_internal.h"

namespace wow {
namespace {

constexpr int COLS = kMxCols, ROWS = kMxRows, CW = 6, CH = 8, XOFF = 1;   // MX_CELL_W/H, MX_X_OFF
constexpr char CHARSET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$+-*/=%#&<>@";   // MX_CHARSET
constexpr int CHARSET_LEN = sizeof(CHARSET) - 1;
constexpr real BASE_SPEED = R(3.0);
constexpr real MUTATE_RATE = R(1.2);

inline char randChar(State &s) { return CHARSET[s.rng.next() % CHARSET_LEN]; }

inline void glyph(Canvas &cv, int x, int y, char ch, uint16_t c) {
  const char str[2] = {ch, 0};
  cv.text(x, y, str, c);
}

// col_level(): column c covers bands c*32//21 .. (c+1)*32//21
inline real colLevel(const VizFrame &f, int c) {
  const int lo = c * 32 / COLS;
  const int hi = std::max(lo + 1, (c + 1) * 32 / COLS);
  float m = f.level[lo];
  for (int b = lo + 1; b < hi; b++)
    if (f.level[b] > m) m = f.level[b];
  return R(m);
}

void spawn(State &s, int c, real head0) {
  RainCol &col = s.eq.cols[c];
  col.active = 1;
  col.head = head0;
  col.speed = BASE_SPEED * s.rng.uniform(R(0.7), R(1.3));
  col.trail = (uint8_t)(3 + s.rng.next() % 5);
  col.respawn = R(0.0);
}

void advance(State &s, int c, real speed, real dt, int chance, real respawnLo, real respawnHi) {
  RainCol &col = s.eq.cols[c];
  const int prev = (int)std::floor(col.head);
  col.head += speed * dt;
  const int head = (int)std::floor(col.head);
  for (int r = std::max(0, prev + 1); r <= std::min(head, ROWS - 1); r++) s.eq.chars[c][r] = randChar(s);
  if (head - col.trail >= ROWS) {
    col.active = 0;
    col.respawn = s.rng.uniform(respawnLo, respawnHi);
    return;
  }
  for (int k = 1; k <= col.trail; k++) {
    const int r = head - k;
    if (0 <= r && r < ROWS && (int)(s.rng.next() % 1000) < chance) s.eq.chars[c][r] = randChar(s);
  }
}

void drawRain(State &s, Canvas &cv, const uint16_t *lut, uint16_t headColor) {
  for (int c = 0; c < COLS; c++) {
    const RainCol &col = s.eq.cols[c];
    if (!col.active) continue;
    const int head = (int)std::floor(col.head);
    const int x = XOFF + c * CW;
    for (int k = 0; k <= col.trail; k++) {
      const int r = head - k;
      if (r < 0 || r >= ROWS) continue;
      uint16_t color;
      if (k == 0) {
        color = headColor;
      } else {
        const real t = std::max(R(0.0), R(1.0) - (col.head - r) / R(col.trail + 1));
        color = lut[std::min(kFade - 1, (int)(t * (kFade - 1)))];
      }
      glyph(cv, x, r * CH, s.eq.chars[c][r], color);
    }
  }
}

}  // namespace

// fade_lut(pct): level i of 32 is (i + 1) / 32 of the rain colour (0x07E8, green capped at 58 = 235).
void buildFade(uint16_t *lut, int pct) {
  constexpr int R5 = 0, G6 = 58, B5 = 8;
  for (int i = 0; i < kFade; i++)
    lut[i] = (uint16_t)(((R5 * (i + 1) * pct / (kFade * 100)) << 11) | ((G6 * (i + 1) * pct / (kFade * 100)) << 5) |
                        (B5 * (i + 1) * pct / (kFade * 100)));
}

// MatrixBase.__init__ then CodeEQ.__init__, in their order of random draws.
void resetCodeEq(State &s) {
  CodeEqState &e = s.eq;
  for (int c = 0; c < COLS; c++)
    for (int r = 0; r < ROWS; r++) e.chars[c][r] = randChar(s);
  for (int c = 0; c < COLS; c++) {
    e.cols[c].active = 0;
    e.cols[c].head = R(0.0);
    e.cols[c].speed = R(0.0);
    e.cols[c].trail = 3;
    e.cols[c].respawn = s.rng.uniform(R(0.0), R(1.5));
  }
  for (int c = 0; c < COLS; c++)
    for (int r = 0; r < ROWS; r++) e.stack[c][r] = randChar(s);
  e.glitch = R(0.0);
  e.glitchRow = 0;
}

void beatCodeEq(State &s) {
  s.eq.glitch = R(0.10);
  s.eq.glitchRow = (int)(s.rng.next() % ROWS);
}

void renderCodeEq(State &s, Canvas &cv, real dt) {
  const VizFrame &f = s.f;
  CodeEqState &e = s.eq;
  step(s, dt);
  e.glitch = std::max(R(0.0), e.glitch - dt);
  const int chance = (int)(MUTATE_RATE * dt * R(1000.0));
  for (int c = 0; c < COLS; c++) {
    RainCol &col = e.cols[c];
    if (!col.active) {
      col.respawn -= dt;
      if (col.respawn <= R(0.0)) spawn(s, c, R(0.0));
      continue;
    }
    advance(s, c, col.speed * (R(1.0) + R(2.0) * R(f.bass)), dt, chance, R(1.0), R(3.5));
  }
  drawRain(s, cv, e.dim, e.dim[kFade - 1]);
  const int stackChance = (int)(MUTATE_RATE * (R(2.0) + R(8.0) * R(f.treble)) * dt * R(1000.0));
  const uint16_t head = rgb565(216, 235, 216), peakColor = rgb565(120, 200, 140);
  for (int c = 0; c < COLS; c++) {
    const int x = XOFF + c * CW;
    const int hgt = (int)std::floor(colLevel(f, c) * ROWS + R(0.5));
    for (int i = 0; i < hgt; i++) {
      const int r = ROWS - 1 - i;
      if ((int)(s.rng.next() % 1000) < stackChance) e.stack[c][r] = randChar(s);
      cv.fillRect(x, r * CH, CW, CH, 0);
      const uint16_t color = i == hgt - 1 ? head : e.lut[std::min(kFade - 1, 12 + i * 19 / std::max(1, hgt - 1))];
      glyph(cv, x, r * CH, e.stack[c][r], color);
    }
    const int pk = (int)std::floor(R(f.peak[c * 32 / COLS]) * ROWS + R(0.5));
    if (hgt < pk && pk <= ROWS) {
      const int r = ROWS - pk;
      cv.fillRect(x, r * CH, CW, CH, 0);
      glyph(cv, x, r * CH, e.stack[c][r], peakColor);
    }
  }
  if (e.glitch > R(0.0)) {
    const int y = e.glitchRow * CH;
    for (int c = 0; c < COLS; c++) {
      cv.fillRect(XOFF + c * CW, y, CW, CH, 0);
      const char ch = randChar(s);                                  // Python: rand_char() first,
      const uint16_t color = (s.rng.next() % 3) ? head : e.lut[20];  // then the colour's draw
      glyph(cv, XOFF + c * CW, y, ch, color);
    }
  }
}

}  // namespace wow

#endif

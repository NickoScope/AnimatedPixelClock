#pragma once
// The system font on an Adafruit GFX surface: every print() is UTF-8.
//
// SysTextGfx<Base> sits between Adafruit GFX (or anything built on it) and the
// class that draws: the panel is MatrixDisplay : SysTextGfx<MatrixPanel_I2S_DMA>
// (src/display/matrix_display.h), and the host test puts it over a GFX canvas
// (tools/fonts/check_sysfont.py), so what is tested is the code the panel runs.
//
// Print hands a string over whole (Print::write(buffer, size)), so a sequence
// is never split between calls. ASCII goes to Adafruit GFX unchanged. A
// well-formed code point above it is drawn from the system tables
// (src/fonts/sys_text.h) in the current font, the classic 5x7 or PicopixelFB,
// with Adafruit's own rules for wrap, advance, text size and background. A byte
// that starts no well-formed sequence goes to Adafruit GFX as the byte it
// always was, so a screen that prints CP437 signs as raw bytes (print((char)247)
// for a degree) draws exactly what it drew before. A single write(uint8_t) is
// untouched for the same reason.
//
// getTextBounds() and textWidth() measure letters, not bytes.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ARDUINO)
#include <WString.h>
#endif

#include "sys_text.h"

template <class Base>
class SysTextGfx : public Base {
public:
  using Base::Base;
  using Base::write;
  using Base::getTextBounds;   // keeps the __FlashStringHelper overload reachable

  size_t write(const uint8_t *buf, size_t n) override {
    size_t i = 0;
    while (i < n) {
      const uint8_t c = buf[i];
      uint32_t cp = c;
      const unsigned len = c < 0x80 ? 1 : utf8Decode(buf + i, n - i, &cp);
      if (c < 0x80 || len == 0) {
        Base::write(c);
        i++;
        continue;
      }
      writeCodePoint(cp);
      i += len;
    }
    return n;
  }

  // The same box Adafruit's getTextBounds() gives for ASCII, with a Cyrillic
  // or Latin-1 letter measured as it is drawn.
  void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1,
                     uint16_t *w, uint16_t *h) {
    *x1 = x;
    *y1 = y;
    *w = *h = 0;
    int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;   // as Adafruit_GFX::getTextBounds
    const uint8_t *p = (const uint8_t *)str;
    const size_t n = strlen(str);
    for (size_t i = 0; i < n;) {
      uint32_t cp = p[i];
      const unsigned len = p[i] < 0x80 ? 1 : utf8Decode(p + i, n - i, &cp);
      if (p[i] < 0x80 || len == 0) {
        this->charBounds(p[i], &x, &y, &minx, &miny, &maxx, &maxy);
        i++;
        continue;
      }
      codePointBounds(cp, &x, &y, &minx, &miny, &maxx, &maxy);
      i += len;
    }
    if (maxx >= minx) { *x1 = minx; *w = maxx - minx + 1; }
    if (maxy >= miny) { *y1 = miny; *h = maxy - miny + 1; }
  }
#if defined(ARDUINO)
  void getTextBounds(const String &str, int16_t x, int16_t y, int16_t *x1, int16_t *y1,
                     uint16_t *w, uint16_t *h) {
    getTextBounds(str.c_str(), x, y, x1, y1, w, h);
  }
#endif

  // Width of getTextBounds() at the current font and text size, for centring
  // and marquees instead of strlen() * 6. As Adafruit measures, the classic
  // font's box includes each cell's blank sixth column: 6 a letter, so a
  // centring that wants the ink subtracts 1. sysTextWidth() is the ink.
  int16_t textWidth(const char *str) {
    int16_t x1, y1;
    uint16_t w, h;
    getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
  }

private:
  // PicopixelFB is defined in its header, so every file that includes it has
  // its own copy (nine in the firmware) and a pointer comparison would miss
  // most of them. It is recognised by what it is; the last answer is kept.
  bool smallFont() {
    const GFXfont *f = this->gfxFont;
    if (!f) return false;
    if (f == fontSeen_) return fontSeenSmall_;
    fontSeen_ = f;
    fontSeenSmall_ = f->first == PicopixelFB.first && f->last == PicopixelFB.last &&
                     f->yAdvance == PicopixelFB.yAdvance &&
                     memcmp(f->bitmap, PicopixelFBBitmaps, sizeof(PicopixelFBBitmaps)) == 0 &&
                     memcmp(f->glyph, PicopixelFB.glyph,
                            (PicopixelFB.last - PicopixelFB.first + 1) * sizeof(GFXglyph)) == 0;
    return fontSeenSmall_;
  }
  const GFXfont *fontSeen_ = nullptr;
  bool fontSeenSmall_ = false;

  // One code point above ASCII, at the cursor, as Adafruit_GFX::write() would
  // place a glyph of the same font: the same wrap, the same advance.
  void writeCodePoint(uint32_t cp) {
    if (!this->gfxFont) {
      if (this->wrap && (this->cursor_x + this->textsize_x * 6 > this->_width)) {
        this->cursor_x = 0;
        this->cursor_y += this->textsize_y * 8;
      }
      drawClassic(this->cursor_x, this->cursor_y, sysClassicGlyph(cp));
      this->cursor_x += this->textsize_x * 6;
      return;
    }
    if (!smallFont()) {            // no other font has these letters
      Base::write('?');
      return;
    }
    const PxfbGlyph gl = pxfbGlyph(cp, false);
    const GFXglyph *g = gl.g;
    const int16_t sx = this->textsize_x, sy = this->textsize_y;
    if (this->wrap && (this->cursor_x + sx * (g->xOffset + g->width) > this->_width)) {
      this->cursor_x = 0;
      this->cursor_y += sy * (int16_t)PicopixelFB.yAdvance;
    }
    int bit = 0, bo = g->bitmapOffset;
    for (int gy = 0; gy < g->height; gy++)
      for (int gx = 0; gx < g->width; gx++) {
        if (!(bit & 7)) bo++;
        if ((gl.bitmap[bo - 1] >> (7 - (bit & 7))) & 1) {
          const int16_t px = this->cursor_x + (g->xOffset + gx) * sx;
          const int16_t py = this->cursor_y + (g->yOffset + gy) * sy;
          if (sx == 1 && sy == 1) this->drawPixel(px, py, this->textcolor);
          else this->fillRect(px, py, sx, sy, this->textcolor);
        }
        bit++;
      }
    this->cursor_x += g->xAdvance * sx;
  }

  // Adafruit's classic drawChar for 5 columns of our own: the same clipping,
  // the same background rule (drawn when it differs from the text colour).
  void drawClassic(int16_t x, int16_t y, const uint8_t *cols) {
    const uint16_t fg = this->textcolor, bg = this->textbgcolor;
    const uint8_t sx = this->textsize_x, sy = this->textsize_y;
    if (x >= this->_width || y >= this->_height || x + 6 * sx - 1 < 0 || y + 8 * sy - 1 < 0) return;
    for (int8_t i = 0; i < 5; i++) {
      uint8_t line = cols[i];
      for (int8_t j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) {
          if (sx == 1 && sy == 1) this->drawPixel(x + i, y + j, fg);
          else this->fillRect(x + i * sx, y + j * sy, sx, sy, fg);
        } else if (bg != fg) {
          if (sx == 1 && sy == 1) this->drawPixel(x + i, y + j, bg);
          else this->fillRect(x + i * sx, y + j * sy, sx, sy, bg);
        }
      }
    }
    if (bg != fg) {
      if (sx == 1 && sy == 1) this->drawFastVLine(x + 5, y, 8, bg);
      else this->fillRect(x + 5 * sx, y, sx, 8 * sy, bg);
    }
  }

  // Adafruit's charBounds() for a code point it cannot index.
  void codePointBounds(uint32_t cp, int16_t *x, int16_t *y, int16_t *minx, int16_t *miny,
                       int16_t *maxx, int16_t *maxy) {
    if (!this->gfxFont || !smallFont()) {   // classic: every glyph has 'A''s box
      this->charBounds(this->gfxFont ? '?' : 'A', x, y, minx, miny, maxx, maxy);
      return;
    }
    const GFXglyph *g = pxfbGlyph(cp, false).g;
    const int16_t tsx = this->textsize_x, tsy = this->textsize_y;
    if (this->wrap && (*x + ((int16_t)g->xOffset + g->width) * tsx > this->_width)) {
      *x = 0;
      *y += tsy * (int16_t)PicopixelFB.yAdvance;
    }
    const int16_t x1 = *x + g->xOffset * tsx, y1 = *y + g->yOffset * tsy;
    const int16_t x2 = x1 + g->width * tsx - 1, y2 = y1 + g->height * tsy - 1;
    if (x1 < *minx) *minx = x1;
    if (y1 < *miny) *miny = y1;
    if (x2 > *maxx) *maxx = x2;
    if (y2 > *maxy) *maxy = y2;
    *x += g->xAdvance * tsx;
  }
};

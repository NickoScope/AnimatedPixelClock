#pragma once
// Text in the panel's small font, Latin and Cyrillic: px.text and px.width in
// Lua, and the world clock's home name. One decoder and one glyph router, so
// no two pages can disagree about a string. (The flight board still sums its
// ASCII widths itself, flightboardNameWidth, to match the print() it draws with.)
//
// Why not display.print(): Adafruit GFX's print() writes one byte at a time
// (Adafruit_GFX::write(uint8_t)) and reads the font's first/last with
// pgm_read_byte, so a code point above 255 cannot reach any font through it.
// A second GFXfont for Cyrillic (PicopixelCyr in picopixel_fb.h) is therefore
// drawn here, with the same bit order as drawChar.
//
// ASCII is drawn exactly as before this file existed: the same glyphs, the
// same case folding where the caller asked for it, a space for control
// characters and DEL. What is new is only what lies above 0x7F: Cyrillic
// U+0400..U+045F from PicopixelCyr (case folded: the font has capitals only),
// and the MISSING box for everything else, including broken UTF-8, where the
// old loops silently drew a space per byte.
//
// Needs no Adafruit_GFX class, only the font structs, so the host builds in
// tools/luasim compile it unchanged.

#include "picopixel_fb.h"
#include "utf8_next.h"

struct PxfbGlyph {
  const GFXglyph *g;
  const uint8_t  *bitmap;
};

// `foldLatin`: a..z drawn as A..Z, as px.text has always done. The world clock
// keeps Picopixel's own lowercase, so it passes false.
static inline PxfbGlyph pxfbGlyph(uint32_t cp, bool foldLatin) {
  PxfbGlyph r;
  if (cp < 0x80) {
    if (foldLatin && cp >= 'a' && cp <= 'z') cp -= 32;
    if (cp < PicopixelFB.first || cp > PicopixelFB.last) cp = ' ';
    r.g = &PicopixelFB.glyph[cp - PicopixelFB.first];
    r.bitmap = PicopixelFB.bitmap;
  } else if (cp >= PicopixelCyr.first && cp <= PicopixelCyr.last) {
    r.g = &PicopixelCyrGlyphs[cp - PicopixelCyr.first];
    r.bitmap = PicopixelCyrBitmaps;
  } else {
    r.g = &PicopixelMissingGlyph;
    r.bitmap = PicopixelCyrBitmaps;
  }
  return r;
}

// Width in pixels, the sum of the advances.
static inline int pxfbWidth(const char *s, bool foldLatin) {
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p;)
    w += pxfbGlyph(utf8Next(&p), foldLatin).g->xAdvance;
  return w;
}

// Draws `s` with its first glyph's origin at x and the font's baseline at
// `base`, calling put(x, y) for every lit pixel. Stops before a glyph that
// would start at or past `stopX` (the Lua canvas passes its width). Returns
// the x after the last glyph drawn.
template <typename Put>
static inline int pxfbDraw(const char *s, int x, int base, bool foldLatin, int stopX, Put put) {
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    if (x >= stopX) break;
    const PxfbGlyph gl = pxfbGlyph(utf8Next(&p), foldLatin);
    int bit = 0, bo = gl.g->bitmapOffset;
    for (int gy = 0; gy < gl.g->height; gy++)
      for (int gx = 0; gx < gl.g->width; gx++) {
        if (!(bit & 7)) bo++;
        if ((gl.bitmap[bo - 1] >> (7 - (bit & 7))) & 1)
          put(x + gx + gl.g->xOffset, base + gy + gl.g->yOffset);
        bit++;
      }
    x += gl.g->xAdvance;
  }
  return x;
}

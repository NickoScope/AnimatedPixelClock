#pragma once
// The system font: Latin and Cyrillic, capitals and lowercase, in both of the
// panel's fonts, for every screen.
//
//   classic  the 5x7 font every print() uses with no font set (6 x 8 cells)
//   small    PicopixelFB, set with display.setFont(&PicopixelFB)
//
// Text is UTF-8. On the display, print() does all of this by itself
// (MatrixDisplay::write, src/display/matrix_display.h): ASCII goes through
// Adafruit GFX exactly as before, a Cyrillic or Latin-1 code point is drawn
// from the tables here, anything else is the solid MISSING box, and a byte that
// is not UTF-8 at all is drawn as the byte it always was, so the authors'
// CP437 signs (print((char)247) for a degree) are untouched. getTextBounds() and
// sysTextWidth() count letters, not bytes.
//
// Lua reaches both fonts through px.text / px.width (src/lua/lua_px.cpp), and
// the simulator (tools/luasim) compiles this same file, so a script and the
// panel cannot disagree about a string.
//
// Sources: tools/fonts/mksysfont.py (classic_font.h) and tools/fonts/mkcyr.py
// (the Cyrillic of picopixel_fb.h); the X11 glyphs they draw from are in
// tools/fonts/x11/.

#include <stddef.h>
#include <stdint.h>

#include "classic_font.h"
#include "pxfb_text.h"
#include "utf8_next.h"

enum SysFont : uint8_t { SYS_FONT_CLASSIC = 0, SYS_FONT_SMALL = 1 };

// ── cutting text without cutting a letter ──────────────────────────────────
// A byte count cuts a Cyrillic letter in half, and half a letter is drawn as
// a broken byte. These keep cuts on letter boundaries.

// Drops the last letter of s (length n, in place); returns the new length.
static inline size_t utf8DropLast(char *s, size_t n) {
  if (!n) return 0;
  size_t i = n - 1;
  while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
  s[i] = '\0';
  return i;
}

// After a copy cut to a buffer's size: drops a sequence left without its end.
static inline void utf8TrimPartial(char *s) {
  size_t n = 0;
  while (s[n]) n++;
  size_t i = n;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;   // back over continuation bytes
  if (!i) return;
  const unsigned lead = (unsigned char)s[i - 1];
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (lead >= 0xC0 && n - (i - 1) < need) s[i - 1] = '\0';
}

// The classic font's 5 columns for a code point: ASCII, Latin-1, Cyrillic, or
// the MISSING box. Bit 0 of each column is the top row.
static inline const uint8_t *sysClassicGlyph(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) return kClassicAscii[cp - 0x20];
  if (cp >= 0xA0 && cp <= 0xFF) return kClassicLatin1[cp - 0xA0];
  if (cp >= kClassicCyrFirst && cp <= kClassicCyrLast) return kClassicCyr[cp - kClassicCyrFirst];
  return kClassicMissing;
}

// Letters in a UTF-8 string (a broken byte counts as one, as it draws as one).
static inline int sysTextLetters(const char *s) {
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; n++) utf8Next(&p);
  return n;
}

// Width in pixels at size 1, from the first lit column to the last: what a
// caller centring a string wants. The classic font is 6 a letter with the last
// column blank; the small font sums its advances less the last blank column.
static inline int sysTextWidth(const char *s, SysFont f) {
  if (!s || !*s) return 0;
  if (f == SYS_FONT_CLASSIC) return sysTextLetters(s) * 6 - 1;
  return pxfbWidth(s, false) - 1;
}

// Draws `s` at size 1, calling put(x, y) for every lit pixel. Classic: (x, y)
// is the top left of the first cell, as setCursor for the classic font.
// Small: y is the baseline, as setCursor for a GFXfont. Returns the x after
// the text. No wrapping: the display's print() wraps, this is for canvases.
template <typename Put>
static inline int sysTextDraw(const char *s, int x, int y, SysFont f, Put put) {
  if (f == SYS_FONT_SMALL) return pxfbDraw(s, x, y, false, 1 << 14, put);
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    const uint8_t *cols = sysClassicGlyph(utf8Next(&p));
    for (int cx = 0; cx < 5; cx++)
      for (int cy = 0; cy < 8; cy++)
        if ((cols[cx] >> cy) & 1) put(x + cx, y + cy);
    x += 6;
  }
  return x;
}

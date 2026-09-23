// Picopixel metrics for the market pages, from the GFX glyph table itself, so
// the layout (market_layout.cpp) places right-aligned text the same way on the
// panel and in the host test. PicopixelFB (src/fonts/picopixel_fb.h) reuses
// the stock glyph table unchanged, only its 'U' bitmap differs, so the metrics
// are the shipped font's.
//
// On the host, tools/market/panel/hoststub/Adafruit_GFX.h stands in for the
// Arduino header the font file includes: it defines PROGMEM away and brings in
// gfxfont.h, nothing else.

#include "market_model.h"

#include "../fonts/pxfb_text.h"   // UTF-8 letters, as print() now draws them (src/fonts/sys_text.h)

namespace market {

// A letter's metrics: ASCII from the stock table as always (a control
// character as '?'), Cyrillic from PicopixelCyr, anything else the MISSING box.
static const GFXglyph *glyphOf(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F) cp = '?';
  return pxfbGlyph(cp, false).g;
}

int16_t picoAdv(const char *s) {
  int16_t x = 0;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;)
    x = (int16_t)(x + glyphOf(utf8Next(&p))->xAdvance);
  return x;
}

// As Adafruit_GFX::getTextBounds: the span from the first ink column to the
// last; a blank glyph (the space) advances and adds nothing.
int16_t picoInk(const char *s) {
  int16_t x = 0, lo = INT16_MAX, hi = -1;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;) {
    const GFXglyph *g = glyphOf(utf8Next(&p));
    if (g->width && g->height) {
      const int16_t a = (int16_t)(x + g->xOffset), b = (int16_t)(a + g->width - 1);
      if (a < lo) lo = a;
      if (b > hi) hi = b;
    }
    x = (int16_t)(x + g->xAdvance);
  }
  return hi >= lo ? (int16_t)(hi - lo + 1) : 0;
}

}  // namespace market

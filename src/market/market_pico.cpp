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

#if defined(ARDUINO)
#include "../fonts/picopixel_fb.h"
#else
#include <Fonts/Picopixel.h>
#endif

namespace market {

static const GFXglyph *glyphOf(char c) {
  if (c < 0x20 || c > 0x7E) c = '?';
  return &PicopixelGlyphs[c - 0x20];
}

int16_t picoAdv(const char *s) {
  int16_t x = 0;
  for (; s && *s; s++) x = (int16_t)(x + glyphOf(*s)->xAdvance);
  return x;
}

// As Adafruit_GFX::getTextBounds: the span from the first ink column to the
// last; a blank glyph (the space) advances and adds nothing.
int16_t picoInk(const char *s) {
  int16_t x = 0, lo = INT16_MAX, hi = -1;
  for (; s && *s; s++) {
    const GFXglyph *g = glyphOf(*s);
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

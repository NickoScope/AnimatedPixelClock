// Host test of the system font (src/fonts/sys_print.h, sys_text.h): the
// panel's print() path over the real Adafruit GFX (its own Adafruit_GFX.cpp and
// glcdfont.c), drawing into a GFXcanvas16 instead of the HUB75 panel.
//
// Built and run by tools/fonts/check_sysfont.py.
//
// What must hold:
//   - every Cyrillic letter, capital and lowercase, and the Latin-1 the classic
//     font has, lands on the canvas exactly as the tables say, in both fonts,
//     at text size 1 and 2, with and without a background colour;
//   - ASCII, and bytes that are not UTF-8 (the authors' CP437 signs), draw
//     exactly as plain Adafruit GFX draws them;
//   - widths count letters; wrapping moves a Cyrillic letter as it would a
//     Latin one;
//   - text cut to a buffer or to a width never keeps half a letter;
//   - negative control: plain Adafruit GFX does not draw the Cyrillic right,
//     so the comparison can see the difference.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Adafruit_GFX.h"
#include "Fonts/Org_01.h"
#include "sys_print.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using SysCanvas = SysTextGfx<GFXcanvas16>;
static const int W = 128, H = 64;
static const uint16_t FG = 0xFFFF, BG = 0x1234;

static std::vector<uint16_t> pixels(GFXcanvas16 &c) {
  return std::vector<uint16_t>(c.getBuffer(), c.getBuffer() + W * H);
}

// The reference: the tables drawn straight, no Adafruit involved.
static std::vector<uint16_t> reference(const char *s, SysFont f, int x, int y, int size, bool bg) {
  std::vector<uint16_t> px(W * H, 0);
  auto set = [&](int X, int Y, uint16_t v) { if (X >= 0 && X < W && Y >= 0 && Y < H) px[Y * W + X] = v; };
  if (bg && f == SYS_FONT_CLASSIC) {                     // the classic font paints its whole cell
    const int n = sysTextLetters(s);
    for (int i = 0; i < n; i++)
      for (int X = 0; X < 6 * size; X++)
        for (int Y = 0; Y < 8 * size; Y++) set(x + i * 6 * size + X, y + Y, BG);
  }
  sysTextDraw(s, 0, 0, f, [&](int X, int Y) {
    for (int a = 0; a < size; a++)
      for (int b = 0; b < size; b++) set(x + X * size + a, y + Y * size + b, FG);
  });
  return px;
}

static std::vector<uint16_t> printed(const char *s, SysFont f, int x, int y, int size, bool bg) {
  SysCanvas c(W, H);
  c.fillScreen(0);
  c.setTextWrap(false);
  c.setFont(f == SYS_FONT_SMALL ? &PicopixelFB : nullptr);
  c.setTextSize(size);
  if (bg) c.setTextColor(FG, BG); else c.setTextColor(FG);
  c.setCursor(x, y);
  c.print(s);
  return pixels(c);
}

static const char *kUpper = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
static const char *kLower = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя";

static void cyrillicMatchesTables() {
  // 33 letters at 6 px do not fit 128 at once: draw them in rows of 16.
  for (const char *alpha : {kUpper, kLower}) {
    const unsigned char *p = (const unsigned char *)alpha;
    std::string chunk;
    int n = 0;
    while (*p) {
      const unsigned char *q = p;
      utf8Next(&p);
      chunk.append((const char *)q, p - q);
      if (++n == 16 || !*p) {
        for (SysFont f : {SYS_FONT_CLASSIC, SYS_FONT_SMALL}) {
          const int y = f == SYS_FONT_SMALL ? 10 : 2;    // small: y is the baseline
          CHECK(printed(chunk.c_str(), f, 3, y, 1, false) == reference(chunk.c_str(), f, 3, y, 1, false));
        }
        CHECK(printed(chunk.c_str(), SYS_FONT_CLASSIC, 1, 3, 1, true) ==
              reference(chunk.c_str(), SYS_FONT_CLASSIC, 1, 3, 1, true));
        chunk.clear();
        n = 0;
      }
    }
  }
  // Size 2 (a big title), classic and small.
  CHECK(printed("ЖЁЛТЫЙ щи", SYS_FONT_CLASSIC, 0, 0, 2, false) ==
        reference("ЖЁЛТЫЙ щи", SYS_FONT_CLASSIC, 0, 0, 2, false));
  CHECK(printed("Жёлтый", SYS_FONT_SMALL, 0, 20, 2, false) == reference("Жёлтый", SYS_FONT_SMALL, 0, 20, 2, false));
  // Latin-1 the classic font has: the degree sign, the accents.
  CHECK(printed("21°C café ±", SYS_FONT_CLASSIC, 2, 2, 1, false) ==
        reference("21°C café ±", SYS_FONT_CLASSIC, 2, 2, 1, false));
}

static void asciiAndOldBytesUnchanged() {
  const char *samples[] = {"Hello, World 12:34", "The quick brown fox", "~!@#$%^&*()_+{}|:<>?",
                           "25\xF7" "C",             // the authors' degree: a byte, not UTF-8
                           "\xDB bars \xB1",        // CP437 blocks that start no UTF-8 sequence
                           "tail \xD0"};             // a lead byte with nothing after it
  for (const char *s : samples)
    for (int f = 0; f < 2; f++)
      for (int size = 1; size <= 2; size++) {
        GFXcanvas16 plain(W, H);
        plain.fillScreen(0);
        plain.setTextWrap(false);
        plain.setFont(f ? &PicopixelFB : nullptr);
        plain.setTextSize(size);
        plain.setTextColor(FG, BG);
        plain.setCursor(1, f ? 10 : 1);
        plain.print(s);
        SysCanvas sys(W, H);
        sys.fillScreen(0);
        sys.setTextWrap(false);
        sys.setFont(f ? &PicopixelFB : nullptr);
        sys.setTextSize(size);
        sys.setTextColor(FG, BG);
        sys.setCursor(1, f ? 10 : 1);
        sys.print(s);
        CHECK(pixels(plain) == pixels(sys));
        int16_t a1, b1, a2, b2;
        uint16_t w1, h1, w2, h2;
        plain.getTextBounds(s, 1, 10, &a1, &b1, &w1, &h1);
        sys.getTextBounds(s, 1, 10, &a2, &b2, &w2, &h2);
        CHECK(a1 == a2 && b1 == b2 && w1 == w2 && h1 == h2);
      }
  // Bytes that do form UTF-8 are read as UTF-8, even if a screen meant CP437:
  // DB B0 is U+06F0, which the panel has no glyph for. No screen in src/
  // prints raw bytes above 0x7F to the panel (checked 2026-09-23).
  CHECK(printed("\xDB\xB0", SYS_FONT_CLASSIC, 0, 0, 1, false) == reference("\xDB\xB0", SYS_FONT_CLASSIC, 0, 0, 1, false));
  // A single byte written on its own keeps its old meaning too.
  GFXcanvas16 plain(W, H);
  SysCanvas sys(W, H);
  plain.fillScreen(0); sys.fillScreen(0);
  plain.setTextColor(FG); sys.setTextColor(FG);
  plain.setCursor(0, 0); sys.setCursor(0, 0);
  plain.print((char)247); sys.print((char)247);
  CHECK(pixels(plain) == pixels(sys));
}

static void widthsCountLetters() {
  SysCanvas c(W, H);
  c.setTextWrap(false);
  c.setFont(nullptr);
  c.setTextSize(1);
  // Classic: 6 a letter, the last column blank - as Adafruit measures ASCII.
  CHECK(c.textWidth("Привет") == c.textWidth("ABCDEF"));
  CHECK(c.textWidth("Привет") == sysTextWidth("Привет", SYS_FONT_CLASSIC) + 1);
  c.setTextSize(2);
  CHECK(c.textWidth("Щука") == c.textWidth("ABCD"));
  CHECK(c.textWidth("Щука") == 2 * 4 * 6);
  c.setTextSize(1);
  c.setFont(&PicopixelFB);
  // Small: the ink of what is drawn, compared with the pixels themselves.
  const char *s = "Съешь же ещё";
  int16_t bx, by; uint16_t bw, bh;
  c.getTextBounds(s, 5, 20, &bx, &by, &bw, &bh);
  std::vector<uint16_t> px = printed(s, SYS_FONT_SMALL, 5, 20, 1, false);
  int lo = W, hi = -1, top = H, bot = -1;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (px[y * W + x]) { if (x < lo) lo = x; if (x > hi) hi = x; if (y < top) top = y; if (y > bot) bot = y; }
  CHECK(bx <= lo && bx + bw - 1 >= hi);                     // the box holds every lit pixel
  CHECK(by <= top && by + bh - 1 >= bot);
  CHECK(bw <= (hi - lo + 1) + 1);                           // and is not a byte count wide
  CHECK(sysTextLetters("ёЁ") == 2 && sysTextLetters("a\xD0") == 2);
}

static void wrapMovesLetters() {
  // 22 letters at 6 px: the 22nd starts at 126 and does not fit 128, so it
  // goes to the next row, exactly where the 22nd Latin letter goes.
  std::string cyr, lat;
  for (int i = 0; i < 22; i++) { cyr += "Ж"; lat += "M"; }
  SysCanvas a(W, H), b(W, H);
  for (SysCanvas *c : {&a, &b}) { c->fillScreen(0); c->setTextWrap(true); c->setTextColor(FG); c->setCursor(0, 0); }
  a.print(cyr.c_str());
  b.print(lat.c_str());
  CHECK(a.getCursorX() == b.getCursorX() && a.getCursorY() == b.getCursorY());
  CHECK(a.getCursorY() == 8);
}

static void cutsKeepWholeLetters() {
  char buf[16];
  strncpy(buf, "ПриветМир", 7);   // 3.5 letters
  buf[7] = '\0';
  utf8TrimPartial(buf);
  CHECK(!strcmp(buf, "При"));
  char w[32] = "Щука";
  size_t n = strlen(w);
  n = utf8DropLast(w, n);
  CHECK(!strcmp(w, "Щук") && n == strlen("Щук"));
  n = utf8DropLast(w, n); n = utf8DropLast(w, n); n = utf8DropLast(w, n);
  CHECK(n == 0 && w[0] == '\0');
  char ascii[] = "abc";
  utf8TrimPartial(ascii);
  CHECK(!strcmp(ascii, "abc"));
}

static void negativeControl() {
  // Plain Adafruit GFX, as the panel was before: the Cyrillic comes out wrong.
  GFXcanvas16 plain(W, H);
  plain.fillScreen(0);
  plain.setTextWrap(false);
  plain.setTextColor(FG);
  plain.setCursor(3, 2);
  plain.print("АБВГДЕЁЖЗИЙКЛМНО");
  CHECK(pixels(plain) != reference("АБВГДЕЁЖЗИЙКЛМНО", SYS_FONT_CLASSIC, 3, 2, 1, false));
}

static void boundsAndFallbacks() {
  // Bounds that start off the canvas: the same box as Adafruit's (audit 2026-09-23).
  for (int f = 0; f < 2; f++) {
    GFXcanvas16 plain(W, H);
    SysCanvas sys(W, H);
    for (GFXcanvas16 *c : {(GFXcanvas16 *)&plain, (GFXcanvas16 *)&sys}) c->setFont(f ? &PicopixelFB : nullptr);
    struct { int16_t x, y; } at[] = {{130, 0}, {0, 70}, {-20, 5}, {200, 90}};
    for (auto a : at) {
      int16_t a1, b1, a2, b2;
      uint16_t w1, h1, w2, h2;
      plain.getTextBounds("AB y", a.x, a.y, &a1, &b1, &w1, &h1);
      sys.getTextBounds("AB y", a.x, a.y, &a2, &b2, &w2, &h2);
      CHECK(a1 == a2 && b1 == b2 && w1 == w2 && h1 == h2);
    }
  }
  // Picopixel wraps a Cyrillic letter where it would wrap a Latin one of the same width.
  {
    SysCanvas a(W, H), b(W, H);
    for (SysCanvas *c : {&a, &b}) { c->setFont(&PicopixelFB); c->setTextWrap(true); c->setCursor(0, 10); }
    std::string cyr, lat;
    for (int i = 0; i < 40; i++) { cyr += "Н"; lat += "H"; }   // same glyph, 4 px: the 33rd does not fit
    a.print(cyr.c_str());
    b.print(lat.c_str());
    CHECK(a.getCursorX() == b.getCursorX() && a.getCursorY() == b.getCursorY() && a.getCursorY() > 10);
  }
  // A font with no Cyrillic draws '?', as a font with no glyph always did.
  {
    SysCanvas a(W, H), b(W, H);
    for (SysCanvas *c : {&a, &b}) { c->fillScreen(0); c->setFont(&Org_01); c->setTextColor(FG); c->setCursor(2, 10); }
    a.print("Я");
    b.print("?");
    CHECK(pixels(a) == pixels(b));
  }
  // The classic font off the left edge (a marquee) draws the part that is on.
  CHECK(printed("ЖЖЖЖ", SYS_FONT_CLASSIC, -7, 0, 1, false) == reference("ЖЖЖЖ", SYS_FONT_CLASSIC, -7, 0, 1, false));
}

int main() {
  boundsAndFallbacks();
  cyrillicMatchesTables();
  asciiAndOldBytesUnchanged();
  widthsCountLetters();
  wrapMovesLetters();
  cutsKeepWholeLetters();
  negativeControl();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}

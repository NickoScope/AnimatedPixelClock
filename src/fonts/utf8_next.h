#pragma once
// One UTF-8 code point at a time, strictly. Plain C, so the firmware and the
// simulator (tools/luasim/luasim.c) share this exact text: if they decoded
// differently, fx_parity would compare two different strings.
//
// Well-formed sequences are the ones of the Unicode Standard's table of
// well-formed UTF-8 byte sequences (the same set RFC 3629 gives as ABNF):
//
//     00..7F
//     C2..DF  80..BF
//     E0      A0..BF  80..BF
//     E1..EC  80..BF  80..BF
//     ED      80..9F  80..BF        (no surrogates)
//     EE..EF  80..BF  80..BF
//     F0      90..BF  80..BF  80..BF
//     F1..F3  80..BF  80..BF  80..BF
//     F4      80..8F  80..BF  80..BF (nothing above U+10FFFF)
//
// Anything else is ill-formed and becomes U+FFFD, one per maximal subpart:
// the lead byte and as many following bytes as could still have been part of
// a well-formed sequence. That is the substitution the Unicode Standard
// recommends, and what Python's decode(errors="replace") does, which is how
// tools/fonts/check_utf8.py tests this on the host. C0, C1 and F5..FF can
// start nothing, so overlong forms of ASCII never reach the font.
//
// Stops at the terminating NUL: a continuation byte is never read past it,
// because NUL is not 80..BF.

#include <stddef.h>
#include <stdint.h>

#define UTF8_REPLACEMENT 0xFFFDu

static inline uint32_t utf8Next(const unsigned char **ps) {
  const unsigned char *p = *ps;
  const unsigned c = p[0];
  if (c < 0x80) { *ps = p + 1; return c; }

  unsigned need, lo = 0x80, hi = 0xBF;
  uint32_t cp;
  if      (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F;
                                     if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07;
                                     if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
  else { *ps = p + 1; return UTF8_REPLACEMENT; }   // 80..C1, F5..FF: no sequence starts here

  for (unsigned i = 1; i <= need; i++) {
    const unsigned b = p[i];
    if (b < lo || b > hi) { *ps = p + i; return UTF8_REPLACEMENT; }   // the maximal subpart ends here
    cp = (cp << 6) | (b & 0x3F);
    lo = 0x80; hi = 0xBF;                          // only the second byte has a narrower range
  }
  *ps = p + need + 1;
  return cp;
}

// The same table over a buffer that need not end in NUL, for Print's
// write(buffer, size): the length of the well-formed sequence at p (1..4), with
// its code point in *cp, or 0 when none starts there. The caller decides what a
// byte that starts nothing means; the display keeps what it always drew for it.
static inline unsigned utf8Decode(const unsigned char *p, size_t n, uint32_t *cp) {
  if (!n) return 0;
  const unsigned c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  unsigned need, lo = 0x80, hi = 0xBF;
  uint32_t v;
  if      (c >= 0xC2 && c <= 0xDF) { need = 1; v = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; v = c & 0x0F;
                                     if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; v = c & 0x07;
                                     if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
  else return 0;
  if (n < need + 1) return 0;
  for (unsigned i = 1; i <= need; i++) {
    const unsigned b = p[i];
    if (b < lo || b > hi) return 0;
    v = (v << 6) | (b & 0x3F);
    lo = 0x80; hi = 0xBF;
  }
  *cp = v;
  return need + 1;
}

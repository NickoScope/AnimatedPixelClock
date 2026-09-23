#pragma once
// What a place name on a page may hold: the letters the page's font draws as
// capitals, Latin and Cyrillic (the system font, src/fonts/sys_text.h), digits,
// space and . - '. The world clock's cities and the flight board's airports
// check their names against this, on the panel and in the host tests.

#include <stdint.h>

#include "utf8_next.h"

static inline bool nameCharOk(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == ' ' || cp == '.' ||
         cp == '-' || cp == '\'' || (cp >= 0x0410 && cp <= 0x042F) || cp == 0x0401;
}

// Every letter of a NUL-terminated UTF-8 name is allowed (broken UTF-8 is not).
static inline bool nameCharsOk(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p;)
    if (!nameCharOk(utf8Next(&p))) return false;
  return true;
}

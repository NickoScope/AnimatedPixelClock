#pragma once
// Just enough Arduino for Adafruit GFX's own Adafruit_GFX.cpp to build on the
// host, so tools/fonts/check_sysfont.py tests the panel's print() path with the
// real library underneath. Nothing here draws.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef radians
#define radians(deg) ((deg) * 0.017453292519943295)
#endif
class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))
class String {
 public:
  String(const char *s = "") : s_(s) {}
  const char *c_str() const { return s_; }
  unsigned length() const { return (unsigned)strlen(s_); }
 private:
  const char *s_;
};
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *b, size_t n) {
    size_t k = 0;
    while (n--) k += write(*b++);
    return k;
  }
  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t print(const char *s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
};

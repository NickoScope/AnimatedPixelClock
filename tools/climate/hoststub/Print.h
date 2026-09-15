#pragma once
// Host stand-in for the Arduino core's Print, as far as Adafruit GFX and the
// weather screen's layout use it: print() of a string reaches the subclass's
// write(uint8_t) one character at a time.

#include <cstddef>
#include <cstdint>
#include <cstring>

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *buffer, size_t size) {
    size_t n = 0;
    while (size--) n += write(*buffer++);
    return n;
  }
  size_t write(const char *str) { return str ? write((const uint8_t *)str, strlen(str)) : 0; }
  size_t print(const char *s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
};

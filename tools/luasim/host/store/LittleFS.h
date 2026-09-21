#pragma once
#include <cstddef>
#include <cstdint>
struct File {
  operator bool() const { return false; }
  bool isDirectory() const { return false; }
  File openNextFile() { return File(); }
  const char *name() const { return ""; }
  size_t size() const { return 0; }
  void close() {}
  size_t read(uint8_t*, size_t) { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
};
struct FSStub {
  bool begin(bool) { return false; }
  bool exists(const char*) { return false; }
  bool mkdir(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  File open(const char*, const char* = "r") { return File(); }
  size_t totalBytes() { return 0; }
  size_t usedBytes() { return 0; }
};
static FSStub LittleFS;

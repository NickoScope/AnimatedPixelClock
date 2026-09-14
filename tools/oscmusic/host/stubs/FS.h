// Host stand-in for fs::File: a stdio file. readDelayUs() slows every read, so
// the stream test can play a slow card.
#pragma once
#include <atomic>
#include <thread>

#include <Arduino.h>

inline std::atomic<long>& readDelayUs() {
  static std::atomic<long> us{0};
  return us;
}

class File {
 public:
  File() {}
  explicit File(const char* path) {
    f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET); }
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() { close(); }
  explicit operator bool() const { return f != nullptr; }
  bool seek(uint32_t pos) { return f && fseek(f, (long)pos, SEEK_SET) == 0; }
  size_t read(uint8_t* b, size_t n) {
    if (long us = readDelayUs().load()) std::this_thread::sleep_for(std::chrono::microseconds(us));
    return f ? fread(b, 1, n, f) : 0;
  }
  size_t size() const { return sz; }
  void close() { if (f) fclose(f); f = nullptr; }

 private:
  FILE* f = nullptr;
  size_t sz = 0;
};

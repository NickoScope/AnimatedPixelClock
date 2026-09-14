// Host stand-in for the few Arduino pieces anim_store.cpp and clip_stream.cpp
// use, so tools/oscmusic/test_clip_maker.py can compile them with c++.
#pragma once
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  std::string s;
  String(const char* c = "") : s(c) {}
  String(const std::string& x) : s(x) {}
  String operator+(const char* c) const { return String(s + c); }
  const char* c_str() const { return s.c_str(); }
};

struct SerialStub {
  void println(const char*) {}
  template <class... A> void printf(const char*, A...) {}
};
extern SerialStub Serial;

inline unsigned long micros() {
  using namespace std::chrono;
  return (unsigned long)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
inline unsigned long millis() { return micros() / 1000; }

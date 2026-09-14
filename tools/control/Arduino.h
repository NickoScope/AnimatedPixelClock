#pragma once
#include <cstdint>
#include <cstdio>
#define INPUT_PULLUP 5
#define INPUT_PULLDOWN 9
#define LOW 0
#define HIGH 1
extern int g_pin[64];
extern uint32_t g_ms;
inline int digitalRead(int p) { return g_pin[p]; }
inline void pinMode(int, int) {}
inline uint32_t millis() { return g_ms; }
struct SerialShim { template<class... A> void printf(const char *f, A... a) { std::printf(f, a...); } };
extern SerialShim Serial;

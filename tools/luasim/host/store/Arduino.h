#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cstdlib>
struct SerialStub { void println(const char*){} void printf(const char*,...){} };
static SerialStub Serial;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x)  ((void)0)

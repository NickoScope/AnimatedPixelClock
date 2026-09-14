// Host stand-in: anim_store.cpp mounts LittleFS, the validator never touches it.
#pragma once
#include <FS.h>

struct LittleFSStub {
  bool begin(bool) { return true; }
  bool exists(const char*) { return true; }
  bool mkdir(const char*) { return true; }
  bool remove(const char*) { return true; }
  size_t totalBytes() { return 0; }
  size_t usedBytes() { return 0; }
};
extern LittleFSStub LittleFS;

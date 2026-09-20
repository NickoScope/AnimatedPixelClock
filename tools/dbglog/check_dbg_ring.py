#!/usr/bin/env python3
"""The remote log's ring, on the host. src/debug/dbg_ring.h."""
import subprocess, sys, tempfile, os, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = r'''
#include "debug/dbg_ring.h"
#include <cstdio>
#include <cstring>
#include <string>
static int failed = 0, checks = 0;
static void is(bool got, bool want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s\n", what); }
}
static void eq(uint32_t got, uint32_t want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s: got %u want %u\n", what, got, want); }
}
static void put(DbgRing *r, const char *s) { dbgRingWrite(r, s, (uint32_t)strlen(s)); }
static std::string readAll(const DbgRing *r, uint32_t since, uint32_t *from) {
  char out[4096]; const uint32_t n = dbgRingRead(r, since, out, sizeof out, from);
  return std::string(out, n);
}
int main() {
  char store[16];
  DbgRing r; dbgRingInit(&r, store, sizeof store);
  uint32_t from = 0;

  // Empty
  eq(r.seq, 0, "starts at zero"); eq(dbgRingOldest(&r), 0, "oldest is zero");
  eq((uint32_t)readAll(&r, 0, &from).size(), 0, "nothing to read");

  // A line that fits
  put(&r, "hello");
  eq(r.seq, 5, "five written"); eq(r.kept, 5, "five kept"); eq(r.dropped, 0, "nothing dropped");
  is(readAll(&r, 0, &from) == "hello", true, "reads back");
  eq(from, 0, "from the start");
  eq((uint32_t)readAll(&r, 5, &from).size(), 0, "a caught-up reader gets nothing");
  eq(from, 5, "and keeps its place");

  // Fill exactly
  put(&r, "abcdefghijk");             // 5 + 11 = 16 = cap
  eq(r.kept, 16, "exactly full"); eq(r.dropped, 0, "still nothing dropped");
  is(readAll(&r, 0, &from) == "helloabcdefghijk", true, "the whole ring reads back");

  // Overflow: the oldest go
  put(&r, "XY");
  eq(r.seq, 18, "eighteen written"); eq(r.kept, 16, "still holds a ringful");
  eq(r.dropped, 2, "two dropped"); eq(dbgRingOldest(&r), 2, "oldest moved");
  is(readAll(&r, 0, &from) == "lloabcdefghijkXY", true, "the newest sixteen");
  eq(from, 2, "a reader behind the window is told where it really starts");

  // A write larger than the ring keeps its tail
  put(&r, "0123456789ABCDEFGHIJ");    // 20 bytes into a 16-byte ring
  eq(r.kept, 16, "a ringful again");
  is(readAll(&r, 0, &from) == "456789ABCDEFGHIJ", true, "the tail of an oversized write");
  eq(from, r.seq - 16, "and the cursor says so");

  // A reader in the middle of the window
  const uint32_t mid = r.seq - 4;
  is(readAll(&r, mid, &from) == "GHIJ", true, "reads from the middle");
  eq(from, mid, "with no gap reported");

  // A reader from the future is pulled back rather than reading rubbish
  readAll(&r, r.seq + 1000, &from);
  eq(from, dbgRingOldest(&r), "a nonsense cursor lands on the oldest byte");

  // A short read is bounded by the caller's buffer
  char small[3]; uint32_t f2 = 0;
  eq(dbgRingRead(&r, dbgRingOldest(&r), small, sizeof small, &f2), 3, "bounded by the reader's room");

  // The log turned off: writes are counted, nothing is stored, nothing crashes
  DbgRing off; dbgRingInit(&off, nullptr, 0);
  put(&off, "ignored");
  eq(off.seq, 0, "an off log does nothing"); eq(off.kept, 0, "and keeps nothing");
  char o2[8]; uint32_t f3 = 0;
  eq(dbgRingRead(&off, 0, o2, sizeof o2, &f3), 0, "and reads nothing");

  // A negative control: dropping nothing on overflow would be the bug.
  DbgRing t2; char s2[4]; dbgRingInit(&t2, s2, sizeof s2);
  put(&t2, "12345678");
  is(t2.dropped > 0, true, "overflow must report dropped bytes");
  eq(t2.kept, 4, "and keep only a ringful");

  printf("%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, "t.cpp"); open(c, "w").write(SRC)
    b = os.path.join(d, "t")
    for std in ("c++11", "c++17"):
        if subprocess.run(["c++", "-std=" + std, "-Wall", "-Wextra", "-Werror",
                           "-fsanitize=address,undefined", "-I", str(ROOT / "src"), c, "-o", b]).returncode:
            sys.exit("compile failed at " + std)
        if subprocess.run([b]).returncode: sys.exit(1)
        print("[%s] above" % std)
print("dbg ring: ok")

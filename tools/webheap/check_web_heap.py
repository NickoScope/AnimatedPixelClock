#!/usr/bin/env python3
"""The portal's back-off rule, on the host. src/web/web_heap_backoff.h."""
import subprocess, sys, tempfile, os, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = r'''
#include "web/web_heap_backoff.h"
#include <cstdio>
static int failed = 0, checks = 0;
static void is(bool got, bool want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want); }
}
static void eq(uint32_t got, uint32_t want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s: got %u want %u\n", what, got, want); }
}
int main() {
  // The age arithmetic, including the millis() wrap - the half that lives on the
  // panel and used to be unreachable from here.
  eq(webHeapFailAgeMs(5000, 1000), 4000, "plain elapsed");
  eq(webHeapFailAgeMs(0, 0), 0, "same instant");
  eq(webHeapFailAgeMs(0x00000100UL, 0xFFFFF000UL), 0x1100UL, "across the wrap");
  eq(webHeapFailAgeMs(0, 1), ALLOC_FAIL_WIFI_NEVER - 1, "one tick before the wrap, clamped");
  eq(webHeapFailAgeMs(0x7FFFFFFEUL, 0), 0x7FFFFFFEUL, "just under the clamp");
  eq(webHeapFailAgeMs(0x7FFFFFFFUL, 0), ALLOC_FAIL_WIFI_NEVER - 1, "at the clamp");
  eq(webHeapFailAgeMs(0xFFFFFFFFUL, 0), ALLOC_FAIL_WIFI_NEVER - 1, "far past it");
  eq(webHeapFailAgeMs(0x00000100UL, 0xFFFFFF00UL), 0x200UL, "a short age across the wrap");
  is(webHeapBackoffActive(webHeapFailAgeMs(0x00000100UL, 0xFFFFFF00UL), 0), true,
     "a fresh failure across the wrap still refuses");
  is(webHeapBackoffActive(webHeapFailAgeMs(0x00000100UL, 0xFFFFF000UL), 0), false,
     "a stale one across the wrap does not");

  // Fresh failure: hold back.
  is(webHeapBackoffActive(0, 0), true, "just failed");
  is(webHeapBackoffActive(1, 0), true, "1 ms ago");
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS - 1, 0), true, "just inside the window");
  // The window closes, and stays closed.
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS, 0), false, "exactly at the window");
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS + 1, 0), false, "just past it");
  is(webHeapBackoffActive(60000, 0), false, "a minute ago");
  // Never failed reads as calm, and the sentinel is far outside the window.
  is(webHeapBackoffActive(ALLOC_FAIL_WIFI_NEVER, 0), false, "never failed");
  is(ALLOC_FAIL_WIFI_NEVER > WEB_HEAP_BACKOFF_MS, true, "sentinel outside the window");
  // A negative control: a window of zero would refuse nothing, which is the bug
  // this rule exists to prevent, so the constant must not be zero.
  is(WEB_HEAP_BACKOFF_MS > 0, true, "the window is not zero");
  // The escape hatch: a panel that keeps starving must not refuse for ever.
  is(webHeapBackoffActive(0, WEB_HEAP_REFUSE_STREAK - 1), true, "still refusing one short of the streak");
  is(webHeapBackoffActive(0, WEB_HEAP_REFUSE_STREAK), false, "the streak lets one through");
  is(webHeapBackoffActive(0, WEB_HEAP_REFUSE_STREAK + 99), false, "and stays open past it");
  is(WEB_HEAP_REFUSE_STREAK > 0, true, "the streak is not zero");
  // A negative control for the escape hatch: without it a fresh failure would
  // refuse at any streak, which is the silent-portal bug it exists to prevent.
  is(0 < WEB_HEAP_BACKOFF_MS && !webHeapBackoffActive(0, WEB_HEAP_REFUSE_STREAK), true,
     "fresh failure plus a full streak does not refuse");
  printf("%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, "t.cpp"); open(c, "w").write(SRC)
    b = os.path.join(d, "t")
    for std in ("c++11", "c++17"):
        r = subprocess.run(["c++", "-std=" + std, "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "src"), c, "-o", b])
        if r.returncode: sys.exit("compile failed at " + std)
        r = subprocess.run([b]); print("[%s] above" % std)
        if r.returncode: sys.exit(1)
print("web heap back-off: ok")

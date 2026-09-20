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
int main() {
  // Fresh failure: hold back.
  is(webHeapBackoffActive(0), true, "just failed");
  is(webHeapBackoffActive(1), true, "1 ms ago");
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS - 1), true, "just inside the window");
  // The window closes, and stays closed.
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS), false, "exactly at the window");
  is(webHeapBackoffActive(WEB_HEAP_BACKOFF_MS + 1), false, "just past it");
  is(webHeapBackoffActive(60000), false, "a minute ago");
  // Never failed reads as calm, and the sentinel is far outside the window.
  is(webHeapBackoffActive(ALLOC_FAIL_WIFI_NEVER), false, "never failed");
  is(ALLOC_FAIL_WIFI_NEVER > WEB_HEAP_BACKOFF_MS, true, "sentinel outside the window");
  // A negative control: a window of zero would refuse nothing, which is the bug
  // this rule exists to prevent, so the constant must not be zero.
  is(WEB_HEAP_BACKOFF_MS > 0, true, "the window is not zero");
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

#!/usr/bin/env python3
"""The portal's back-off rule, on the host. src/web/web_heap_backoff.h."""
import subprocess, sys, tempfile, os, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = r'''
#include "web/web_heap_backoff.h"
#include "net/net_reserve.h"
#include "network/net_turns.h"
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
  // The streak belongs to one episode: a refusal older than the window is gone.
  eq(webHeapStreakNow(5, 0), 5, "a refusal just now keeps the streak");
  eq(webHeapStreakNow(5, WEB_HEAP_BACKOFF_MS - 1), 5, "still inside the window");
  eq(webHeapStreakNow(5, WEB_HEAP_BACKOFF_MS), 0, "at the window, a new episode");
  eq(webHeapStreakNow(5, 3600000), 0, "an hour later, certainly new");
  eq(webHeapStreakNow(0, 0), 0, "nothing to drop");
  // The case the dead flag could not see: nobody asked for a blob during the
  // calm, so only the clock can tell the episodes apart.
  eq(webHeapStreakNow(WEB_HEAP_REFUSE_STREAK, WEB_HEAP_BACKOFF_MS * 100), 0,
     "a full streak does not survive a quiet spell");
  // The radio's reserve: held from boot, given up while the radio is failing,
  // taken back only after a good quiet spell.
  is(netReserveWanted(ALLOC_FAIL_WIFI_NEVER), true, "held from boot, the radio never failed");
  is(netReserveWanted(0), false, "given up the instant the radio fails");
  is(netReserveWanted(NET_RESERVE_REARM_MS - 1), false, "still given up just before the re-arm");
  is(netReserveWanted(NET_RESERVE_REARM_MS), true, "taken back at the re-arm");
  is(netReserveWanted(3600000), true, "an hour of quiet, certainly held");
  // It must outlast a burst: the measured bursts ran about eleven seconds, so a
  // gap inside one episode must not hand the space back mid-episode.
  is(NET_RESERVE_REARM_MS > 11000UL, true, "the re-arm outlasts a measured burst");
  // And it must cover both measured failures at once: 1,626 B for the wifi
  // task's DMA buffer and 3,072 B for the reconnect.
  // The reserve is off (NET_RESERVE_BYTES 0): taking it at boot cost more than
  // it bought - it cut the largest free block from 25,588 B to 11,252 B and
  // stopped the rail board fetching. What the test holds to is the rule that
  // survives either setting: whenever it IS on, it must cover both measured
  // allocations at once - 1,626 B for the Wi-Fi task and 3,072 B for the
  // reconnect - and when it is off, nothing is ever taken.
  is(NET_RESERVE_BYTES == 0 || NET_RESERVE_BYTES >= 1626 + 3072, true,
     "off, or big enough for both measured allocations");
  is(NET_RESERVE_BYTES == 0, true, "and it is off today: see net_reserve.h for what it cost");
  // A negative control: a reserve released on the same signal the portal keeps
  // sending on would be pointless, so the two must agree that fresh is fresh.
  is(!netReserveWanted(0) && webHeapBackoffActive(0, 0), true, "both react to the same fresh failure");
  // Taking turns: a fetch stands aside for someone at the portal, but not for ever.
  is(netTurnYield(0, 0), true, "a request just now: stand aside");
  is(netTurnYield(NET_TURN_QUIET_MS - 1, 0), true, "still inside the quiet window");
  is(netTurnYield(NET_TURN_QUIET_MS, 0), false, "quiet long enough: go");
  is(netTurnYield(NET_HTTP_NEVER, 0), false, "nobody has ever asked: go");
  is(netTurnYield(0, NET_TURN_MAX_YIELDS - 1), true, "one short of the limit, still yielding");
  is(netTurnYield(0, NET_TURN_MAX_YIELDS), false, "at the limit the data wins");
  is(netTurnYield(0, NET_TURN_MAX_YIELDS + 500), false, "and past it");
  // The two guards are independent: quiet alone is enough, and so is the count.
  is(netTurnYield(NET_TURN_QUIET_MS, NET_TURN_MAX_YIELDS), false, "both clear");
  // A negative control: without the limit an open portal would starve the fetch
  // for ever, which is the failure this counter exists to prevent.
  is(NET_TURN_MAX_YIELDS > 0 && NET_TURN_QUIET_MS > 0, true, "neither guard is disabled");
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

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
// ---- webHeapTooTight: whether the portal answers at all -------------------
// Added 2026-09-21. This function had ZERO coverage: 51 checks in this file and
// not one of them touched the line that decides whether the web portal serves a
// page or returns 503. It went unnoticed until the reserve it applies outlived
// the thing it was protecting.
static const uint32_t BIG = 1u << 20;  // DMA pool not the constraint in these checks
static void checkTooTightDma() {
  // 2026-09-22, the four steps measured while the portal opened, as (general
  // largest, DMA largest). The old guard read only the first and let all four
  // through; the radio failed at the last.
  is(webHeapTooTight(11252, 11252, true), false, "portal opens: first request is served");
  is(webHeapTooTight(7668, 5108, true), true, "second request refused: DMA pool too short for a response");
  is(webHeapTooTight(7668, 3572, true), true, "third refused");
  is(webHeapTooTight(7668, 1396, true), true, "the step that starved the radio is refused");
  // At rest after recovery, 2026-09-22: DMA largest 11,252-12,788 B.
  is(webHeapTooTight(18420, 12788, true), false, "a panel at rest serves the portal");
  // The line itself: the radio's buffer plus one response's send buffer.
  eq(WEB_HEAP_KEEP_DMA, 1626u + 5760u, "DMA line = radio 1,626 + TCP send buffer 5,760");
  is(webHeapTooTight(BIG, WEB_HEAP_KEEP_DMA - 1, true), true, "one below the DMA line is too tight");
  is(webHeapTooTight(BIG, WEB_HEAP_KEEP_DMA, true), false, "exactly the DMA line is enough");
  is(webHeapTooTight(BIG, WEB_HEAP_KEEP_DMA - 1, false), true, "broker down does not relax the DMA line");
}
static void checkTooTight() {
  // Broker up: no module starts a fetch task any more, so the only thing that
  // must be left room is the radio's 1,626 B DMA buffer.
  is(webHeapTooTight(0, BIG, true), true, "broker up: nothing free is too tight");
  is(webHeapTooTight(1625, BIG, true), true, "broker up: one below the radio's need is too tight");
  is(webHeapTooTight(1626, BIG, true), false, "broker up: exactly the radio's need is enough");
  is(webHeapTooTight(8692, BIG, true), false,
     "broker up: the measured unlucky boot (8,692 B) still serves the portal");
  is(webHeapTooTight(24564, BIG, true), false, "broker up: a good boot is fine");

  // Broker down: every module creates its own task again, so the old combined
  // line is the right one.
  is(webHeapTooTight(8692, BIG, false), true,
     "broker down: 8,692 B must refuse - the rail board's 10 KB task could not start");
  is(webHeapTooTight(11865, BIG, false), true, "broker down: one below the sum is too tight");
  is(webHeapTooTight(11866, BIG, false), false, "broker down: exactly the sum is enough");
  is(webHeapTooTight(24564, BIG, false), false, "broker down: a good boot is fine");

  // Named for what it now takes: not "the broker is running" but "every
  // consumer is on it". The audit of 2026-09-21 pointed out that these ten
  // checks tested the function and not what was passed to it, and that the
  // caller was passing nbUp() - true even when one caller's mailbox had failed
  // and that module was still creating a 9 KB task. The checks could not have
  // caught it, and saying so here is the only honest fix available to a unit
  // test: the predicate is the caller's to get right.
  //
  // The relationship, which is the part that would rot silently if either
  // constant moved: all-migrated can only ever ALLOW more, never less.
  for (uint32_t b = 0; b < 30000; b += 97)
    if (webHeapTooTight(b, BIG, true) && !webHeapTooTight(b, BIG, false)) {
      is(false, true, "broker up is never stricter than broker down");
      return;
    }
  is(true, true, "broker up is never stricter than broker down, across the range");
}

int main() {
  checkTooTight();
  checkTooTightDma();
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
  // yieldingSinceMs 0 means "not waiting yet".
  is(netTurnYield(0, 0, 1000), true, "a request just now: stand aside");
  is(netTurnYield(NET_TURN_QUIET_MS - 1, 0, 1000), true, "still inside the quiet window");
  is(netTurnYield(NET_TURN_QUIET_MS, 0, 1000), false, "quiet long enough: go");
  is(netTurnYield(NET_HTTP_NEVER, 0, 1000), false, "nobody has ever asked: go");
  // The ceiling is TIME, not passes. This is the bug the first version had: it
  // counted calls, and the starters are called on every pass of loop(), so the
  // ceiling was spent in milliseconds and the mechanism did nothing.
  is(netTurnYield(0, 1000, 1000 + NET_TURN_MAX_YIELD_MS - 1), true, "one tick short of the ceiling");
  is(netTurnYield(0, 1000, 1000 + NET_TURN_MAX_YIELD_MS), false, "at the ceiling the data wins");
  is(netTurnYield(0, 1000, 1000 + NET_TURN_MAX_YIELD_MS * 10), false, "and long past it");
  // Ten thousand calls inside the window still yield - a counter would not.
  { bool all = true;
    for (uint32_t k = 0; k < 10000; k++) if (!netTurnYield(0, 1000, 1000 + k % 100)) all = false;
    is(all, true, "ten thousand passes inside the window all yield"); }
  // Across the millis() wrap.
  is(netTurnYield(0, 0xFFFFFF00UL, 0x00000100UL), true, "waiting across the wrap, still inside");
  is(netTurnYield(0, 0xFFFFFF00UL, 0x00000100UL + NET_TURN_MAX_YIELD_MS), false, "and past it across the wrap");
  // Quiet wins over the ceiling either way round.
  is(netTurnYield(NET_TURN_QUIET_MS, 1000, 1000), false, "quiet beats a fresh wait");
  // Negative control: neither guard may be disabled.
  is(NET_TURN_MAX_YIELD_MS > 0 && NET_TURN_QUIET_MS > 0, true, "neither guard is disabled");
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

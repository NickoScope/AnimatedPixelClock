#!/usr/bin/env python3
"""The broker's queue rules, on the host. src/net/nb_queue.h."""
import subprocess, sys, tempfile, os, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = r'''
#include "net/nb_queue.h"
#include "net/nb_sink.h"
#include <cstdio>
static int failed = 0, checks = 0;
static void eq(uint32_t got, uint32_t want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s: got %u want %u\n", what, got, want); }
}
static void is(bool got, bool want, const char *what) {
  checks++;
  if (got != want) { failed++; printf("FAIL %s\n", what); }
}
// ---- nb_sink.h: the mailbox's bounds arithmetic --------------------------
// Added 2026-09-21 because the audit pointed out that the mailbox rework had
// shipped with no host test at all - and this is the part of it where an
// off-by-one is a memcpy past the end of a PSRAM buffer, which on this chip
// does not fault, it quietly corrupts whatever is next.
static void checkSink() {
  bool t;
  // No room for even a terminator.
  t = false; is(nbSinkTake(0, 0, 10, &t) == 0 && t, true, "cap 0 takes nothing and says so");
  t = false; is(nbSinkTake(1, 0, 10, &t) == 0 && t, true, "cap 1 is terminator only");
  // cap 2 holds exactly one byte.
  t = false; is(nbSinkTake(2, 0, 1, &t) == 1 && !t, true, "cap 2 takes one byte cleanly");
  t = false; is(nbSinkTake(2, 0, 5, &t) == 1 && t, true, "cap 2 takes one of five, truncates");
  t = false; is(nbSinkTake(2, 1, 1, &t) == 0 && t, true, "cap 2 already full takes nothing");
  // Exactly filling the usable length is NOT truncation - the boundary that
  // matters, because getting it wrong either loses a byte or writes one too many.
  t = false; is(nbSinkTake(100, 0, 99, &t) == 99 && !t, true, "exactly cap-1 is not truncation");
  t = false; is(nbSinkTake(100, 0, 100, &t) == 99 && t, true, "cap bytes truncates by one");
  t = false; is(nbSinkTake(100, 99, 1, &t) == 0 && t, true, "full at cap-1 takes nothing more");
  // Two blocks across the boundary - the shape a socket actually delivers.
  t = false;
  uint32_t len = 0;
  len += nbSinkTake(100, len, 60, &t);
  is(len == 60 && !t, true, "first block of 60 fits");
  len += nbSinkTake(100, len, 60, &t);
  is(len == 99 && t, true, "second block fills to cap-1 and truncates");
  // A len past the end - the case that would wrap (cap-1)-len to about four
  // billion and run the memcpy off the end of the buffer. The audit of
  // 2026-09-21 pointed out that the old sweep stopped exactly at the
  // precondition, so the one input that mattered was never offered.
  t = false; is(nbSinkTake(10, 10, 5, &t) == 0 && t, true, "len == cap takes nothing");
  t = false; is(nbSinkTake(10, 99, 5, &t) == 0 && t, true, "len far past cap takes nothing");
  t = false; is(nbSinkTake(10, 9, 5, &t) == 0 && t, true, "len == cap-1 is already full");
  // size 0 is not truncation: nothing was dropped.
  t = false; is(nbSinkTake(10, 0, 0, &t) == 0 && !t, true, "size 0 is not truncation");
  t = false; is(nbSinkTake(0, 0, 0, &t) == 0 && !t, true, "size 0 on cap 0 is not truncation");
  // The invariant the memcpy depends on, now swept PAST the precondition too.
  bool everOver = false;
  for (uint32_t cap = 0; cap < 40 && !everOver; cap++)
    for (uint32_t l = 0; l < cap + 4 && !everOver; l++)
      for (uint32_t sz = 0; sz < 50; sz++) {
        bool tt = false;
        const uint32_t n = nbSinkTake(cap, l, sz, &tt);
        if (n && (uint64_t)l + n > (uint64_t)cap - 1) { everOver = true; break; }
      }
  is(everOver, false, "room never exceeded for any cap/len/size, len past the end included");
}

int main() {
  checkSink();
  NbQueue q; nbInit(&q);

  // Empty
  is(nbBusy(&q), false, "empty: nothing on air");
  eq(nbWaiting(&q), 0, "empty: nobody waiting");
  eq(nbPick(&q, 100), NB_CALLER_COUNT, "empty: nobody to pick");

  // First come, first served among scheduled refreshes
  is(nbSubmit(&q, NB_WEATHER, false, 100), true, "weather queues");
  is(nbSubmit(&q, NB_RAIL, false, 200), true, "rail queues");
  eq(nbWaiting(&q), 2, "two waiting");
  eq(nbPick(&q, 300), NB_WEATHER, "the one that waited longest goes first");

  // While one is on the wire, nobody else is picked
  nbStart(&q, NB_WEATHER);
  is(nbBusy(&q), true, "on air");
  eq(nbPick(&q, 400), NB_CALLER_COUNT, "nobody is picked while one is on the wire");
  eq(nbWaiting(&q), 1, "rail still waiting");

  // A caller cannot have two of its own in flight
  is(nbSubmit(&q, NB_WEATHER, false, 410), false, "the one on air cannot queue itself again");

  // ...but it may queue again once its turn has ended
  nbFinish(&q);
  is(nbBusy(&q), false, "finished");
  eq(nbPick(&q, 500), NB_RAIL, "rail's turn now");
  is(nbSubmit(&q, NB_WEATHER, false, 510), true, "weather may queue again");

  // A person waiting jumps the scheduled ones
  is(nbSubmit(&q, NB_FLIGHT, true, 520), true, "an interactive request queues");
  eq(nbPick(&q, 530), NB_FLIGHT, "interactive goes first, however long the others waited");

  // A later scheduled request from the same caller does NOT clear the flag:
  // the person is still waiting for the thing they changed.
  is(nbSubmit(&q, NB_FLIGHT, false, 540), true, "same caller queues again");
  eq(nbPick(&q, 550), NB_FLIGHT, "still first: the interactive flag survives");

  // Two interactive: the older one first
  nbInit(&q);
  nbSubmit(&q, NB_RAIL, true, 100);
  nbSubmit(&q, NB_FLIGHT, true, 200);
  eq(nbPick(&q, 300), NB_RAIL, "between two interactive, the older");

  // Replacing one's own pending request keeps one slot, not two
  nbInit(&q);
  nbSubmit(&q, NB_RAIL, false, 100);
  nbSubmit(&q, NB_RAIL, false, 200);
  eq(nbWaiting(&q), 1, "a caller occupies one slot however often it asks");

  // The wire time is measured from queueing, and survives the millis() wrap
  nbInit(&q);
  nbSubmit(&q, NB_WEATHER, false, 0xFFFFFF00UL);
  nbStart(&q, NB_WEATHER);
  eq(nbOnAirMs(&q, 0x00000100UL), 0x200UL, "on-air time across the wrap");
  eq(nbOnAirMs(&q, 0xFFFFFF00UL), 0, "on-air time at the instant it started");

  // Picking across the wrap prefers the one that has waited longer
  nbInit(&q);
  nbSubmit(&q, NB_WEATHER, false, 0xFFFFFF00UL);   // queued before the wrap
  nbSubmit(&q, NB_RAIL, false, 0x00000050UL);      // queued after it
  eq(nbPick(&q, 0x00000100UL), NB_WEATHER, "the older one wins across the wrap");

  // A finished slot is free and carries no flag into its next turn
  nbInit(&q);
  nbSubmit(&q, NB_RAIL, true, 100); nbStart(&q, NB_RAIL); nbFinish(&q);
  nbSubmit(&q, NB_RAIL, false, 200); nbSubmit(&q, NB_WEATHER, false, 150);
  eq(nbPick(&q, 300), NB_WEATHER, "an old interactive flag does not haunt the next turn");

  // A bad caller id changes nothing
  nbInit(&q);
  is(nbSubmit(&q, NB_CALLER_COUNT, false, 100), false, "a caller out of range is refused");
  eq(nbWaiting(&q), 0, "and queues nothing");

  // Negative control: without the interactive rule the person would wait behind
  // a refresh, which is the whole reason the rule exists.
  nbInit(&q);
  nbSubmit(&q, NB_WEATHER, false, 100);
  nbSubmit(&q, NB_RAIL, true, 900);
  is(nbPick(&q, 1000) == NB_RAIL, true, "the person beats the older refresh");

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
print("nb queue: ok")

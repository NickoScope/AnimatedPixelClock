#!/usr/bin/env python3
"""The broker's queue rules, on the host. src/net/nb_queue.h."""
import subprocess, sys, tempfile, os, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = r'''
#include "net/nb_queue.h"
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
int main() {
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

// Host test for src/ir/ir_map.h and src/ir/ir_console.h: the learned map, the
// state machine that turns frames into detents and a button level, and the
// serial console's grammar. No board, no Arduino, no receiver library.
//
// The timings come from Vishay application note 80071 rev 2.3, THE NEC CODE: a
// held key repeats in a 108 ms time slot. Every case below is written in those
// terms rather than in round numbers, so a change to the constants that breaks
// the reason for them fails here.
//
// Built and run by tools/ir/check_ir.py.

#include <cstdio>
#include <cstring>

#include "ir_console.h"
#include "ir_map.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using ir::Command;
using ir::Decoder;
using ir::Frame;
using ir::Outcome;

static Frame code(uint64_t v, uint8_t proto = 3) { return Frame{proto, v, false, false}; }
static Frame repeat() { return Frame{0, 0, true, false}; }
static Frame noise() { return Frame{0, 0, false, true}; }

// A decoder with the three knob slots already learned, as the portal would
// leave it.
static void teach(Decoder &d) {
  d.reset();
  d.map().bind(ir::kCcw, 3, 0x11);
  d.map().bind(ir::kCw, 3, 0x22);
  d.map().bind(ir::kOk, 3, 0x33);
}

// ── the map ─────────────────────────────────────────────────────────────────
static void mapBasics() {
  Decoder d;
  teach(d);
  CHECK(d.map().boundCount() == 3);
  CHECK(d.map().match(3, 0x22) == ir::kCw);
  CHECK(d.map().match(3, 0x99) < 0);        // a button nobody taught
  CHECK(d.map().match(4, 0x22) < 0);        // same code, another protocol, is another button
  CHECK(d.map().clear(ir::kCw));
  CHECK(d.map().match(3, 0x22) < 0);
  CHECK(d.map().boundCount() == 2);
  CHECK(!d.map().clear(ir::kSlotCount));    // out of range refuses rather than writing past the end
}

// ── one code lives in one slot ──────────────────────────────────────────────
static void learningMovesACode() {
  Decoder d;
  teach(d);
  d.learnArm(ir::kBrightUp, 1000);
  const Outcome o = d.frame(1100, code(0x22));       // the button already bound to CW
  CHECK(o.kind == Outcome::kLearned);
  CHECK(o.slot == ir::kBrightUp);
  CHECK(o.movedFrom == ir::kCw);                     // taken away, so it cannot fire two things
  CHECK(!d.map().bound(ir::kCw));
  CHECK(d.map().match(3, 0x22) == ir::kBrightUp);
  CHECK(d.learnActive(1100) < 0);                    // one code closes the window
}

static void learningConsumesTheFrame() {
  Decoder d;
  teach(d);
  d.learnArm(ir::kBrightUp, 1000);
  d.frame(1100, code(0x33));                         // the OK button, taught into another slot
  CHECK(d.takeRotate() == 0);
  CHECK(!d.okDown(1100));                            // the frame taught; it did not also act
}

static void learnWindowCloses() {
  Decoder d;
  teach(d);
  d.learnArm(ir::kAux, 1000);
  CHECK(d.learnActive(1000 + ir::kLearnWindowMs - 1) == ir::kAux);
  CHECK(d.learnRemainMs(1000 + ir::kLearnWindowMs - 1) == 1);
  CHECK(d.learnActive(1000 + ir::kLearnWindowMs) < 0);
  const Outcome o = d.frame(1000 + ir::kLearnWindowMs + 10, code(0x44));
  CHECK(o.kind == Outcome::kNothing);                // too late: an unlearned code does nothing
  CHECK(!d.map().bound(ir::kAux));
}

static void learnRefusesNoiseAndRepeats() {
  Decoder d;
  teach(d);
  d.learnArm(ir::kAux, 1000);
  CHECK(d.frame(1100, noise()).kind == Outcome::kNothing);
  CHECK(d.frame(1150, repeat()).kind == Outcome::kNothing);
  CHECK(d.frame(1200, Frame{3, 0, false, false}).kind == Outcome::kNothing);   // a zero code is not a button
  CHECK(d.learnActive(1200) == ir::kAux);            // the window is still open for a real one
  CHECK(d.frame(1250, code(0x55)).kind == Outcome::kLearned);
}

// ── rotation ────────────────────────────────────────────────────────────────
static void detentsAccumulateAndAreDrainedOnce() {
  Decoder d;
  teach(d);
  d.frame(1000, code(0x22));
  d.frame(1108, code(0x22));      // one NEC time slot later
  CHECK(d.takeRotate() == 2);
  CHECK(d.takeRotate() == 0);     // drained: the seam must never see them twice
  d.frame(1300, code(0x11));
  CHECK(d.takeRotate() == -1);
}

static void theQueueHasACeiling() {
  Decoder d;
  teach(d);
  for (int i = 0; i < 20; i++) d.frame(1000 + i * 108, code(0x22));
  CHECK(d.takeRotate() == ir::kRotAccMax);   // a starved sampling task loses the excess, not the display
  for (int i = 0; i < 20; i++) d.frame(4000 + i * 108, code(0x11));
  CHECK(d.takeRotate() == -ir::kRotAccMax);
}

// ── the button is a level ───────────────────────────────────────────────────
static void heldButtonSurvivesOneDroppedRepeat() {
  Decoder d;
  teach(d);
  d.frame(1000, code(0x33));
  CHECK(d.okDown(1000));
  CHECK(d.okDown(1000 + 2 * ir::kNecRepeatMs));        // 216 ms: one repeat was lost
  CHECK(!d.okDown(1000 + ir::kHoldMs));                // and not two
  // A repeat frame carries no code: it extends the slot being held.
  d.frame(1000 + ir::kNecRepeatMs, repeat());
  CHECK(d.okDown(1000 + ir::kNecRepeatMs + ir::kHoldMs - 1));
}

static void aStaleRepeatExtendsNothing() {
  Decoder d;
  teach(d);
  d.frame(1000, code(0x33));
  // Later than kRepeatFreshMs after the last real frame: the remote let go and
  // somebody else's repeat arrived. It must not revive the button.
  const Outcome o = d.frame(1000 + ir::kRepeatFreshMs + 1, repeat());
  CHECK(o.kind == Outcome::kNothing);
  CHECK(!d.okDown(1000 + ir::kRepeatFreshMs + 1 + ir::kHoldMs));
  CHECK(d.ignored() == 1);
}

static void noiseAndStrangersAreIgnored() {
  Decoder d;
  teach(d);
  CHECK(d.frame(1000, noise()).kind == Outcome::kNothing);
  CHECK(d.frame(1100, code(0x99)).kind == Outcome::kNothing);   // another household remote
  CHECK(d.takeRotate() == 0);
  CHECK(!d.okDown(1100));
  CHECK(d.frames() == 2 && d.ignored() == 2);
}

// ── millis() wraps ──────────────────────────────────────────────────────────
// The panel runs for months; millis() wraps at 49.7 days. Unsigned comparisons
// across that wrap held the button down for weeks in the module this was ported
// from (NickoScope32 v33.48.0, audit HIGH-1).
static void clockWrap() {
  const uint32_t nearEnd = 0xFFFFFF00u;
  Decoder d;
  teach(d);
  d.frame(nearEnd, code(0x33));
  CHECK(d.okDown(nearEnd + 100));                 // still held, across the wrap
  CHECK(!d.okDown(nearEnd + ir::kHoldMs));        // and released on time, not in seven weeks
  d.learnArm(ir::kAux, nearEnd);
  CHECK(d.learnActive(nearEnd + ir::kLearnWindowMs - 1) == ir::kAux);
  CHECK(d.learnActive(nearEnd + ir::kLearnWindowMs) < 0);
}

// ── the simulator takes the same path ───────────────────────────────────────
static void simulationIsIndistinguishable() {
  Decoder d;
  d.reset();                       // nothing learned at all: a fresh panel
  CHECK(d.simulate(1000, ir::kCw, 0).kind == Outcome::kRotate);
  CHECK(d.takeRotate() == 1);
  CHECK(d.simulate(1100, ir::kOk, 0).kind == Outcome::kButton);
  CHECK(d.okDown(1100 + ir::kHoldMs - 1));
  CHECK(!d.okDown(1100 + ir::kHoldMs));
  // A hold long enough for the encoder's own long-press threshold (1 s).
  d.simulate(2000, ir::kOk, 1200);
  CHECK(d.okDown(2000 + 1100));
  CHECK(!d.okDown(2000 + 1200));
  CHECK(!d.simulate(1000, ir::kSlotCount, 0).kind);     // out of range does nothing
  CHECK(d.simulate(3000, ir::kPower, 0).kind == Outcome::kReserved);
}

static void countersAndLiveJournal() {
  Decoder d;
  teach(d);
  d.frame(1000, code(0x22));
  d.frame(1108, repeat());
  uint8_t proto = 0; uint64_t value = 0; bool rpt = true; uint32_t age = 99;
  CHECK(d.lastSeen(&proto, &value, &rpt, &age, 1200));
  CHECK(value == 0x22 && proto == 3);     // a repeat carries no code, so the value stands
  CHECK(rpt && age == 92);
  CHECK(d.lastHitSlot(1200, 5000) == ir::kCw);
  CHECK(d.lastHitSlot(1200, 50) < 0);     // "recently" is the caller's window
  CHECK(d.hits(ir::kCw) == 2);
}

// ── the console's grammar ───────────────────────────────────────────────────
static Command parse(const char *line, bool expectOurs = true) {
  Command c{Command::kNone, 0, 0, nullptr};
  const bool ours = ir::parseCommand(line, &c);
  CHECK(ours == expectOurs);
  return c;
}

static void consoleBasics() {
  CHECK(parse("ir").kind == Command::kStatus);
  CHECK(parse("  IR  ").kind == Command::kStatus);
  CHECK(parse("ir status").kind == Command::kStatus);
  CHECK(parse("ir help").kind == Command::kHelp);
  CHECK(parse("ir ?").kind == Command::kHelp);
  CHECK(parse("ir cancel").kind == Command::kCancel);
  // Not ours: every other line on the port is left alone.
  CHECK(parse("", false).kind == Command::kNone);
  CHECK(parse("hello", false).kind == Command::kNone);
  CHECK(parse("iron ok", false).kind == Command::kNone);
}

static void consoleRotation() {
  Command c = parse("ir cw");
  CHECK(c.kind == Command::kRepeat && c.slot == ir::kCw && c.arg == 1);
  c = parse("ir CCW 3");
  CHECK(c.kind == Command::kRepeat && c.slot == ir::kCcw && c.arg == 3);
  c = parse("ir cw 900");
  CHECK(c.kind == Command::kRepeat && c.arg == 64);       // a typo cannot spin the display for a minute
  CHECK(parse("ir cw 0").kind == Command::kError);
  CHECK(parse("ir cw two").kind == Command::kError);
}

static void consoleButtonAndSim() {
  Command c = parse("ir ok");
  CHECK(c.kind == Command::kSim && c.slot == ir::kOk && c.arg == 0);
  c = parse("ir ok 1200");
  CHECK(c.kind == Command::kSim && c.arg == 1200);
  c = parse("ir sim bright_up");
  CHECK(c.kind == Command::kSim && c.slot == ir::kBrightUp && c.arg == 0);
  c = parse("ir sim 2 500");
  CHECK(c.kind == Command::kSim && c.slot == ir::kOk && c.arg == 500);
  CHECK(parse("ir sim").kind == Command::kError);
  CHECK(parse("ir sim nope").kind == Command::kError);
  CHECK(parse("ir sim 99").kind == Command::kError);      // a number past the table is not a slot
  CHECK(parse("ir ok soon").kind == Command::kError);
  c = parse("ir ok 99999999999");                         // clamped, not wrapped into a short click
  CHECK(c.kind == Command::kSim && c.arg == 0xFFFFFFFFu);
}

static void consoleLearnAndClear() {
  Command c = parse("ir learn ok");
  CHECK(c.kind == Command::kLearn && c.slot == ir::kOk);
  CHECK(parse("ir learn").kind == Command::kError);
  c = parse("ir clear CW");
  CHECK(c.kind == Command::kClear && c.slot == ir::kCw);
  CHECK(parse("ir clear all").kind == Command::kClearAll);
  CHECK(parse("ir clear").kind == Command::kError);
  CHECK(parse("ir wibble").kind == Command::kError);
}

static void consoleSurvivesLongWords() {
  // A word longer than the parser's buffer is truncated, never overrun.
  char line[300];
  std::snprintf(line, sizeof(line), "ir sim %.*s", 250, "okokokokokokokokokokokokokokokokokokokokokokokokok");
  CHECK(parse(line).kind == Command::kError);
  CHECK(ir::parseCommand(nullptr, nullptr) == false);
}

// ── the tables stay in step ─────────────────────────────────────────────────
static void everySlotHasAName() {
  for (uint8_t i = 0; i < ir::kSlotCount; i++) {
    CHECK(std::strcmp(ir::slotName(i), "?") != 0);
    CHECK(std::strcmp(ir::slotHint(i), "?") != 0);
    uint8_t back = 0xFF;
    CHECK(ir::detail::slot(ir::slotName(i), &back) == false || back == i);   // names parse back, case aside
  }
  CHECK(std::strcmp(ir::slotName(ir::kSlotCount), "?") == 0);
  CHECK(ir::slotDrivesKnob(ir::kCcw) && ir::slotDrivesKnob(ir::kCw) && ir::slotDrivesKnob(ir::kOk));
  CHECK(!ir::slotDrivesKnob(ir::kPower));
}

int main() {
  mapBasics();
  learningMovesACode();
  learningConsumesTheFrame();
  learnWindowCloses();
  learnRefusesNoiseAndRepeats();
  detentsAccumulateAndAreDrainedOnce();
  theQueueHasACeiling();
  heldButtonSurvivesOneDroppedRepeat();
  aStaleRepeatExtendsNothing();
  noiseAndStrangersAreIgnored();
  clockWrap();
  simulationIsIndistinguishable();
  countersAndLiveJournal();
  consoleBasics();
  consoleRotation();
  consoleButtonAndSim();
  consoleLearnAndClear();
  consoleSurvivesLongWords();
  everySlotHasAName();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}

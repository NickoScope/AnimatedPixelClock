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

// Buttons by their place in the default layout (owner, 2026-09-23), 0-based as
// the code counts them; people see 1..10.
enum : uint8_t { B_CCW = 0, B_CW, B_OK, B_LONG, B_POWER, B_BUP, B_BDOWN, B_HOME, B_CAR, B_MEDIA };

static Frame code(uint64_t v, uint8_t proto = 3) { return Frame{proto, v, false, false}; }
static Frame repeat() { return Frame{0, 0, true, false}; }
static Frame noise() { return Frame{0, 0, false, true}; }

// A decoder with the three knob slots already learned, as the portal would
// leave it.
static void teach(Decoder &d) {
  d.reset();
  d.map().bind(B_CCW, 3, 0x11);
  d.map().bind(B_CW, 3, 0x22);
  d.map().bind(B_OK, 3, 0x33);
}

// ── the map ─────────────────────────────────────────────────────────────────
static void mapBasics() {
  Decoder d;
  teach(d);
  CHECK(d.map().boundCount() == 3);
  CHECK(d.map().match(3, 0x22) == B_CW);
  CHECK(d.map().match(3, 0x99) < 0);        // a button nobody taught
  CHECK(d.map().match(4, 0x22) < 0);        // same code, another protocol, is another button
  CHECK(d.map().clear(B_CW));
  CHECK(d.map().match(3, 0x22) < 0);
  CHECK(d.map().boundCount() == 2);
  CHECK(!d.map().clear(ir::kSlotCount));    // out of range refuses rather than writing past the end
}

// ── one code lives in one slot ──────────────────────────────────────────────
static void learningMovesACode() {
  Decoder d;
  teach(d);
  d.learnArm(B_BUP, 1000);
  const Outcome o = d.frame(1100, code(0x22));       // the button already bound to CW
  CHECK(o.kind == Outcome::kLearned);
  CHECK(o.slot == B_BUP);
  CHECK(o.movedFrom == B_CW);                     // taken away, so it cannot fire two things
  CHECK(!d.map().bound(B_CW));
  CHECK(d.map().match(3, 0x22) == B_BUP);
  CHECK(d.learnActive(1100) < 0);                    // one code closes the window
}

static void learningConsumesTheFrame() {
  Decoder d;
  teach(d);
  d.learnArm(B_BUP, 1000);
  d.frame(1100, code(0x33));                         // the OK button, taught into another slot
  CHECK(d.takeRotate() == 0);
  CHECK(!d.okDown(1100));                            // the frame taught; it did not also act
}

static void learnWindowCloses() {
  Decoder d;
  teach(d);
  d.learnArm(B_MEDIA, 1000);
  CHECK(d.learnActive(1000 + ir::kLearnWindowMs - 1) == B_MEDIA);
  CHECK(d.learnRemainMs(1000 + ir::kLearnWindowMs - 1) == 1);
  CHECK(d.learnActive(1000 + ir::kLearnWindowMs) < 0);
  const Outcome o = d.frame(1000 + ir::kLearnWindowMs + 10, code(0x44));
  CHECK(o.kind == Outcome::kNothing);                // too late: an unlearned code does nothing
  CHECK(!d.map().bound(B_MEDIA));
}

static void learnRefusesNoiseAndRepeats() {
  Decoder d;
  teach(d);
  d.learnArm(B_MEDIA, 1000);
  CHECK(d.frame(1100, noise()).kind == Outcome::kNothing);
  CHECK(d.frame(1150, repeat()).kind == Outcome::kNothing);
  CHECK(d.frame(1200, Frame{3, 0, false, false}).kind == Outcome::kNothing);   // a zero code is not a button
  CHECK(d.learnActive(1200) == B_MEDIA);            // the window is still open for a real one
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
  d.learnArm(B_MEDIA, nearEnd);
  CHECK(d.learnActive(nearEnd + ir::kLearnWindowMs - 1) == B_MEDIA);
  CHECK(d.learnActive(nearEnd + ir::kLearnWindowMs) < 0);
}

// ── the three bugs the audit of 2026-09-16 found, at 25.5 days of uptime ────
// millis() is 32 bits: 24.85 days is where a signed difference against a raw
// timestamp changes sign. All three below passed the old tests, which only ever
// looked at the wrap itself (0xFFFFFF00) and never at a deadline left behind.
static const uint32_t kDays25 = 2204496000u;   // 25.5 days in milliseconds

static void aButtonNobodyPressedIsNotHeld() {
  Decoder d;
  teach(d);
  // Nothing has ever touched the remote. The old code compared a signed
  // difference against a deadline of zero and called the button held from day
  // 24.85 onwards - on any panel, with no remote in the room, and the encoder's
  // seam fed that straight into the knob's own switch.
  CHECK(!d.okDown(kDays25));
  CHECK(!d.okDown(kDays25 + 1000));
  // And a press that really happened a month ago is over, not held.
  d.frame(1000, code(0x33));
  CHECK(!d.okDown(kDays25));
}

static void aHoldIsClamped() {
  Decoder d;
  teach(d);
  // /api/ir/sim takes hold from a query string: one GET with a huge number used
  // to pin the button down for weeks.
  d.simulate(1000, B_OK, 2000000000u);
  CHECK(d.okDown(1000 + ir::kSimHoldMaxMs - 1));
  CHECK(!d.okDown(1000 + ir::kSimHoldMaxMs));
  CHECK(!d.okDown(1000 + 60000));
}

static void anAncientSlotIsNotRevived() {
  Decoder d;
  teach(d);
  d.frame(1000, code(0x22));          // a detent, a month ago
  CHECK(d.takeRotate() == 1);
  // A repeat frame carries no code. Freshness is an age, so it is unsigned: a
  // signed difference against a month-old timestamp read as "recent" and turned
  // this into a detent out of nowhere.
  const Outcome o = d.frame(kDays25, repeat());
  CHECK(o.kind == Outcome::kNothing);
  CHECK(d.takeRotate() == 0);
}

static void anAbandonedLearnWindowCloses() {
  Decoder d;
  teach(d);
  d.learnArm(B_MEDIA, 1000);
  CHECK(d.learnActive(kDays25) < 0);        // armed and forgotten: it is not open
  CHECK(d.learnRemainMs(kDays25) == 0);
  CHECK(d.frame(kDays25, code(0x77)).kind == Outcome::kNothing);
  CHECK(!d.map().bound(B_MEDIA));
}

// ── the simulator takes the same path ───────────────────────────────────────
static void simulationIsIndistinguishable() {
  Decoder d;
  d.reset();                       // nothing learned at all: a fresh panel
  CHECK(d.simulate(1000, B_CW, 0).kind == Outcome::kRotate);
  CHECK(d.takeRotate() == 1);
  CHECK(d.simulate(1100, B_OK, 0).kind == Outcome::kButton);
  CHECK(d.okDown(1100 + ir::kHoldMs - 1));
  CHECK(!d.okDown(1100 + ir::kHoldMs));
  // A hold long enough for the encoder's own long-press threshold (1 s).
  d.simulate(2000, B_OK, 1200);
  CHECK(d.okDown(2000 + 1100));
  CHECK(!d.okDown(2000 + 1200));
  CHECK(1200 <= ir::kSimHoldMaxMs);    // the long-press test must stay inside the ceiling
  CHECK(!d.simulate(1000, ir::kSlotCount, 0).kind);     // out of range does nothing
  const Outcome pw = d.simulate(3000, B_POWER, 0);        // an action: run in loop(), by ir_actions.cpp
  CHECK(pw.kind == Outcome::kAction && pw.fn == ir::kFnPower);
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
  CHECK(d.lastHitSlot(1200, 5000) == B_CW);
  CHECK(d.lastHitSlot(1200, 50) < 0);     // "recently" is the caller's window
  CHECK(d.hits(B_CW) == 2);
}

// ── the console's grammar ───────────────────────────────────────────────────
static Command parse(const char *line, bool expectOurs = true) {
  Command c{Command::kNone, 0, 0, nullptr, ir::kFnNone};
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
  CHECK(c.kind == Command::kRepeatFn && c.fn == ir::kFnCw && c.arg == 1);
  c = parse("ir CCW 3");
  CHECK(c.kind == Command::kRepeatFn && c.fn == ir::kFnCcw && c.arg == 3);
  c = parse("ir cw 900");
  CHECK(c.kind == Command::kRepeatFn && c.arg == 64);     // a typo cannot spin the display for a minute
  CHECK(parse("ir cw 0").kind == Command::kError);
  CHECK(parse("ir cw two").kind == Command::kError);
}

static void consoleButtonAndDo() {
  Command c = parse("ir ok");
  CHECK(c.kind == Command::kDo && c.fn == ir::kFnOk && c.arg == 0);
  c = parse("ir ok 1200");
  CHECK(c.kind == Command::kDo && c.arg == 1200);
  c = parse("ir do bright_up");
  CHECK(c.kind == Command::kDo && c.fn == ir::kFnBrightUp && c.arg == 0);
  c = parse("ir do PAGE 20");
  CHECK(c.kind == Command::kDo && c.fn == ir::kFnPage && c.arg == 20);
  CHECK(parse("ir do page 999").kind == Command::kError);
  CHECK(parse("ir do").kind == Command::kError);
  CHECK(parse("ir do nope").kind == Command::kError);
  c = parse("ir press 3 500");
  CHECK(c.kind == Command::kPress && c.slot == B_OK && c.arg == 500);
  c = parse("ir sim 10");                                 // the old word still presses a button
  CHECK(c.kind == Command::kPress && c.slot == B_MEDIA);
  CHECK(parse("ir press").kind == Command::kError);
  CHECK(parse("ir press 0").kind == Command::kError);     // buttons are 1..10
  CHECK(parse("ir press 11").kind == Command::kError);
  CHECK(parse("ir press ok").kind == Command::kError);    // a function is not a button
  CHECK(parse("ir ok soon").kind == Command::kError);
  c = parse("ir ok 99999999999");                         // clamped, not wrapped into a short click
  CHECK(c.kind == Command::kDo && c.arg == 0xFFFFFFFFu);
}

static void consoleFunctions() {
  Command c = parse("ir fn 5 vol_up");
  CHECK(c.kind == Command::kSetFn && c.slot == 4 && c.fn == ir::kFnVolUp);
  c = parse("ir fn 10 page 3");
  CHECK(c.kind == Command::kSetFn && c.slot == 9 && c.fn == ir::kFnPage && c.arg == 3);
  CHECK(parse("ir fn 10 page").kind == Command::kError);   // which page, then
  CHECK(parse("ir fn 11 home").kind == Command::kError);
  CHECK(parse("ir fn 1 wibble").kind == Command::kError);
  CHECK(parse("ir fn").kind == Command::kError);
}

static void consoleLearnAndClear() {
  Command c = parse("ir learn 3");
  CHECK(c.kind == Command::kLearn && c.slot == B_OK);
  CHECK(parse("ir learn").kind == Command::kError);
  CHECK(parse("ir learn ok").kind == Command::kError);
  c = parse("ir clear 2");
  CHECK(c.kind == Command::kClear && c.slot == B_CW);
  CHECK(parse("ir clear all").kind == Command::kClearAll);
  CHECK(parse("ir clear").kind == Command::kError);
  CHECK(parse("ir wibble").kind == Command::kError);
}

static void consoleSurvivesLongWords() {
  // A word longer than the parser's buffer is truncated, never overrun.
  char line[300];
  std::snprintf(line, sizeof(line), "ir do %.*s", 250, "okokokokokokokokokokokokokokokokokokokokokokokokok");
  CHECK(parse(line).kind == Command::kError);
  CHECK(ir::parseCommand(nullptr, nullptr) == false);
}

// ── the buttons, their functions, and the approved layout ───────────────────
static void theDefaultLayoutIsTheApprovedOne() {
  Decoder d;
  d.reset();
  const uint8_t want[ir::kSlotCount] = {ir::kFnCcw, ir::kFnCw, ir::kFnOk, ir::kFnLong, ir::kFnPower,
                                        ir::kFnBrightUp, ir::kFnBrightDown, ir::kFnHome,
                                        ir::kFnCarousel, ir::kFnMediaToggle};
  for (uint8_t i = 0; i < ir::kSlotCount; i++) CHECK(d.map().fn(i) == want[i]);
  CHECK(d.map().fn(ir::kSlotCount) == ir::kFnNone);
  ir::Map fresh;                                    // a map made without reset() has it too
  CHECK(fresh.fn(0) == ir::kFnCcw && fresh.fn(9) == ir::kFnMediaToggle);
}

static void aFunctionIsChosenAndKept() {
  Decoder d;
  teach(d);
  CHECK(d.map().setFn(B_CW, ir::kFnVolUp, 0));
  const Outcome o = d.frame(1000, code(0x22));      // the code learned into button 2
  CHECK(o.kind == Outcome::kAction && o.fn == ir::kFnVolUp && o.slot == B_CW);
  CHECK(d.takeRotate() == 0);                       // no longer a knob turn
  CHECK(d.map().setFn(B_CW, ir::kFnPage, 7));
  const Outcome pg = d.frame(2000, code(0x22));
  CHECK(pg.kind == Outcome::kAction && pg.fn == ir::kFnPage && pg.arg == 7);
  CHECK(d.map().setFn(B_CW, ir::kFnHome, 7) && d.map().arg(B_CW) == 0);   // an argument only for page
  CHECK(!d.map().setFn(ir::kSlotCount, ir::kFnHome, 0));
  CHECK(!d.map().setFn(0, ir::kFnCount, 0));
  CHECK(d.map().clear(B_CW) && d.map().fn(B_CW) == ir::kFnHome);   // forgetting a code keeps the function
  d.map().reset();
  CHECK(d.map().fn(B_CW) == ir::kFnCw);            // reset is the layout, not "nothing"
}

static void noActionIsReportedNotRun() {
  Decoder d;
  teach(d);
  d.map().setFn(B_OK, ir::kFnNone, 0);
  CHECK(d.frame(1000, code(0x33)).kind == Outcome::kReserved);
  CHECK(!d.okDown(1000));
}

static void aHeldActionRepeatsAtItsOwnPace() {
  Decoder d;
  teach(d);
  d.map().setFn(B_CW, ir::kFnBrightUp, 0);
  int fired = 0;
  uint32_t t = 1000;
  if (d.frame(t, code(0x22)).kind == Outcome::kAction) fired++;       // the press fires at once
  // Then NEC repeats every 108 ms for two seconds.
  for (t += ir::kNecRepeatMs; t <= 3000; t += ir::kNecRepeatMs)
    if (d.frame(t, repeat()).kind == Outcome::kAction) fired++;
  // The first repeat that may fire is after kActionDelayMs, then one every
  // kActionRepeatMs at most - four or five in the next 1.5 s, not fourteen.
  CHECK(fired >= 1 + 4 && fired <= 1 + 6);
  // A function that does not repeat fires once however long it is held.
  d.map().setFn(B_CW, ir::kFnPower, 0);
  fired = 0;
  t = 10000;
  if (d.frame(t, code(0x22)).kind == Outcome::kAction) fired++;
  for (t += ir::kNecRepeatMs; t <= 12000; t += ir::kNecRepeatMs)
    if (d.frame(t, repeat()).kind == Outcome::kAction) fired++;
  CHECK(fired == 1);
}

static void aLongPressButtonHoldsPastTheThreshold() {
  Decoder d;
  teach(d);
  d.map().setFn(B_CW, ir::kFnLong, 0);
  d.frame(1000, code(0x22));
  CHECK(d.okDown(1000 + 1000));                    // past src/control's 1 s long-press threshold
  CHECK(!d.okDown(1000 + ir::kLongSpanMs));
  CHECK(ir::kLongSpanMs > 1000 + 20);              // threshold plus the switch debounce
  d.frame(5000, code(0x22));
  d.frame(5000 + ir::kNecRepeatMs, repeat());      // its repeats change nothing
  CHECK(!d.okDown(5000 + ir::kLongSpanMs));
}

static void simulatingAFunctionNeedsNoButton() {
  Decoder d;
  d.reset();
  CHECK(d.simulateFn(1000, ir::kFnCw, 0, 0).kind == Outcome::kRotate);
  CHECK(d.takeRotate() == 1);
  const Outcome o = d.simulateFn(1000, ir::kFnPage, 12, 0);
  CHECK(o.kind == Outcome::kAction && o.arg == 12 && o.slot == -1);
  CHECK(d.simulateFn(1000, ir::kFnOk, 0, 1200).kind == Outcome::kButton);
  CHECK(d.okDown(1000 + 1100) && !d.okDown(1000 + 1200));
  CHECK(!d.simulateFn(1000, ir::kFnCount, 0, 0).kind);
  CHECK(d.hits(0) == 0);                            // no button was pressed
}

// ── the tables stay in step ─────────────────────────────────────────────────
static void everyFunctionHasANameThatParsesBack() {
  for (uint8_t f = 0; f < ir::kFnCount; f++) {
    CHECK(std::strcmp(ir::fnInfo(f).name, "?") != 0);
    CHECK(std::strcmp(ir::fnInfo(f).label, "?") != 0);
    uint8_t back = 0xFF;
    CHECK(ir::fnByName(ir::fnInfo(f).name, &back) && back == f);
  }
  CHECK(std::strcmp(ir::fnInfo(ir::kFnCount).name, "?") == 0);
  uint8_t x;
  CHECK(ir::fnByName("BRIGHT_UP", &x) && x == ir::kFnBrightUp);   // case does not matter
  CHECK(!ir::fnByName("", &x) && !ir::fnByName(nullptr, &x));
  CHECK(ir::fnDrivesKnob(ir::kFnCw) && ir::fnDrivesKnob(ir::kFnLong) && !ir::fnDrivesKnob(ir::kFnPower));
  CHECK(ir::slotByNumber("1", &x) && x == 0 && ir::slotByNumber("10", &x) && x == 9);
  CHECK(!ir::slotByNumber("0", &x) && !ir::slotByNumber("11", &x) && !ir::slotByNumber("99999999999", &x));
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
  aButtonNobodyPressedIsNotHeld();
  aHoldIsClamped();
  anAncientSlotIsNotRevived();
  anAbandonedLearnWindowCloses();
  simulationIsIndistinguishable();
  countersAndLiveJournal();
  consoleBasics();
  consoleRotation();
  consoleButtonAndDo();
  consoleFunctions();
  consoleLearnAndClear();
  consoleSurvivesLongWords();
  theDefaultLayoutIsTheApprovedOne();
  aFunctionIsChosenAndKept();
  noActionIsReportedNotRun();
  aHeldActionRepeatsAtItsOwnPace();
  aLongPressButtonHoldsPastTheThreshold();
  simulatingAFunctionNeedsNoButton();
  everyFunctionHasANameThatParsesBack();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}

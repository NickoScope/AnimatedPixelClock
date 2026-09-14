// Drive the real control.cpp with simulated knob signals, one sample per ms.
#include <cstdio>
#include <string>
#include <vector>
#include "Arduino.h"
#include "control.h"
int g_pin[64]; uint32_t g_ms = 0; SerialShim Serial;
// Board wiring: A/B active high (common on 3V3), so contact closed = pin 1.
// Logical level 1 = contact open, as the decoder sees it. Switch: pressed = 0.
static void setLogical(int ab) { g_pin[45] = !(ab & 1); g_pin[46] = !((ab >> 1) & 1); }
static int cw = 0, ccw = 0, press = 0, lng = 0;
static void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { g_ms++; controlLoop(); for (CtrlEvent e = controlTake(); e != CTRL_NONE; e = controlTake()) { if (e == CTRL_CW) cw++; else if (e == CTRL_CCW) ccw++; else if (e == CTRL_PRESS) press++; else lng++; } } }
static void reset() { cw = ccw = press = lng = 0; }
// Gray sequence forward: 11 -> 10 -> 00 -> 01 -> 11 (A leads).
static const int FWD[4] = {0b11, 0b10, 0b00, 0b01};
static void turn(int startIdx, int transitions, int dir, uint32_t msPerState, bool bounce) {
  int i = startIdx;
  for (int k = 0; k < transitions; k++) {
    int next = (i + (dir > 0 ? 1 : 3)) % 4;
    if (bounce) { setLogical(FWD[next]); run(1); setLogical(FWD[i]); run(1); setLogical(FWD[next]); run(1); setLogical(FWD[i]); run(1); }
    setLogical(FWD[next]); run(msPerState); i = next;
  }
}
static int fails = 0;
static void expect(const char *name, int gotCw, int gotCcw, int wantCw, int wantCcw) {
  bool ok = gotCw == wantCw && gotCcw == wantCcw;
  std::printf("  %-52s CW %2d CCW %2d  (want %d/%d)  %s\n", name, gotCw, gotCcw, wantCw, wantCcw, ok ? "ok" : "FAIL");
  fails += !ok;
}
int main() {
  g_pin[0] = 1;                       // BOOT released
  setLogical(0b11);
  controlBegin();
  run(300);
  std::printf("full-detent knob (rests only at 11)\n");
  reset(); for (int d = 0; d < 10; d++) { turn(0, 4, +1, 15, false); run(120); } expect("10 slow clicks clockwise", cw, ccw, 10, 0);
  reset(); for (int d = 0; d < 10; d++) { turn(0, 4, -1, 15, false); run(120); } expect("10 slow clicks anticlockwise", cw, ccw, 0, 10);
  reset(); for (int d = 0; d < 10; d++) { turn(0, 4, +1, 15, true); run(120); } expect("10 clicks clockwise with contact bounce", cw, ccw, 10, 0);
  reset(); for (int d = 0; d < 20; d++) { turn(0, 4, +1, 3, false); run(8); } expect("20 fast clicks clockwise (12 ms per click)", cw, ccw, 20, 0);
  reset(); turn(0, 2, +1, 20, false); turn(2, 2, -1, 20, false); run(200); expect("half a click and back (no step)", cw, ccw, 0, 0);

  std::printf("half-detent knob (rests at 11 and at 00)\n");
  setLogical(0b11); run(300);
  reset(); int idx = 0;
  for (int d = 0; d < 10; d++) { turn(idx, 2, +1, 15, false); idx = (idx + 2) % 4; run(400); }
  std::printf("  (detent mode now detected from a 00 rest)\n");
  expect("10 slow clicks clockwise, first one delivered once 00 is learned", cw, ccw, 10, 0);
  reset(); for (int d = 0; d < 10; d++) { turn(idx, 2, -1, 15, false); idx = (idx + 2) % 4; run(400); } expect("10 slow clicks anticlockwise", cw, ccw, 0, 10);
  reset(); for (int d = 0; d < 20; d++) { turn(idx, 2, +1, 4, false); idx = (idx + 2) % 4; run(12); } expect("20 fast clicks clockwise (20 ms per click)", cw, ccw, 20, 0);

  std::printf("glitches (nobody at the knob)\n");
  setLogical(0b11); run(300); idx = 0;
  reset(); for (int k = 0; k < 300; k++) { setLogical(0b10); run(1); setLogical(0b11); run(20); } expect("300 one-millisecond spikes on A", cw, ccw, 0, 0);
  reset(); for (int k = 0; k < 100; k++) { for (int s = 1; s <= 4; s++) { setLogical(FWD[s % 4]); run(1); } run(30); } expect("100 one-millisecond fake clicks, a valid sequence", cw, ccw, 0, 0);
  reset(); for (int k = 0; k < 200; k++) { setLogical(0b10); run(1); setLogical(0b00); run(1); setLogical(0b11); run(50); } expect("200 coupled pulses 11-10-00-11, 1 ms a state", cw, ccw, 0, 0);
  reset(); for (int k = 0; k < 200; k++) { setLogical(0b00); run(1); setLogical(0b11); run(9); } expect("200 pulses on both lines at once", cw, ccw, 0, 0);
  reset(); { unsigned x = 12345; for (int k = 0; k < 3000; k++) { x = x * 1103515245u + 12345u; setLogical(((x >> 16) & 7) == 0 ? (int)((x >> 20) & 3) : 0b11); run(1); } setLogical(0b11); run(300); } expect("3 s of sparse random one-millisecond hits", cw, ccw, 0, 0);
  reset(); for (int d = 0; d < 20; d++) { turn(0, 4, +1, 3, false); run(8); } expect("still counts 20 fast clicks at 3 ms a state", cw, ccw, 20, 0);

  std::printf("switch\n");
  reset(); for (int k = 0; k < 3; k++) { g_pin[0] = 0; run(150); g_pin[0] = 1; run(300); } std::printf("  3 clicks of 150 ms: PRESS %d LONG %d (want 3/0) %s\n", press, lng, (press == 3 && lng == 0) ? "ok" : "FAIL"); fails += !(press == 3 && lng == 0);
  reset(); g_pin[0] = 0; run(1500); g_pin[0] = 1; run(300); std::printf("  one 1.5 s hold: PRESS %d LONG %d (want 0/1) %s\n", press, lng, (press == 0 && lng == 1) ? "ok" : "FAIL"); fails += !(press == 0 && lng == 1);
  reset(); for (int k = 0; k < 3; k++) { for (int b = 0; b < 4; b++) { g_pin[0] = b % 2; run(2); } g_pin[0] = 0; run(150); g_pin[0] = 1; run(300); } std::printf("  3 bouncy clicks: PRESS %d LONG %d (want 3/0) %s\n", press, lng, (press == 3 && lng == 0) ? "ok" : "FAIL"); fails += !(press == 3 && lng == 0);
  std::printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
  return fails ? 1 : 0;
}

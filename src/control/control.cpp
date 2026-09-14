#include "control.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <Arduino.h>

// ---------------------------------------------------------------- pins
// From the vendor schematic (reference-drawings/controller in the knowledge
// base), not from counting what the firmware happens not to use.
//
// The expansion header U8 is four pins: IO45, IO46, GND, 3V3. That is the whole
// budget. Everything else on this board is committed, and two pins that look
// free in a firmware grep are not: the vendor pin table assigns IO10 to RTC_INT
// and IO13 to IMU_INT, and neither reaches the header anyway.
//
// Both header pins are strapping pins, which is survivable here:
//   IO45 selects VDD_SPI voltage - but this module has in-package flash and
//        PSRAM with VDD_SPI fixed at 1.8 V by the VDD_SPI_FORCE eFuse, and the
//        datasheet is explicit that the strap then no longer affects it.
//        Read off the board on 2026-09-14: VDD_SPI_FORCE = True.
//   IO46 gates ROM message printing at boot, and with GPIO0 picks the boot
//        mode; normal boot ignores it. A knob only ever pulls it to ground.
//
// The switch has nowhere to go on the header, so it shares GPIO0 with the BOOT
// button - which means a wire to the button pad, not a header pin. The cost is
// unchanged: a knob held through reset lands in download mode and recovers on
// the next one, and the same gesture flashes the board without opening the case.
#if defined(BOARD_WAVESHARE_RGB_MATRIX)
  #define CTRL_PIN_A   45     // header U8 pin 1
  #define CTRL_PIN_B   46     // header U8 pin 2
  #define CTRL_PIN_SW   0     // BOOT button pad, shared
  // The board pulls IO45 and IO46 down with 10 kOhm (schematic R59, R60; the
  // pull-up positions R57, R58 are not fitted). A weak internal pull-up cannot
  // win against that - on the bench both lines read 0 at rest and closing a
  // contact to GND changed nothing. So the knob's common goes to 3V3, a closed
  // contact reads HIGH, and A and B are inverted on reading, which leaves the
  // flagship's decoder, rest state 0b11 included, exactly as it was.
  #define CTRL_AB_ACTIVE_HIGH 1
#else
  #error "CONTROL_ENCODER_ENABLED: no pin map for this board"
#endif

// The decoder and the switch are the NickoScope32 S3 bridge's, ported as they
// are (flagship main.cpp, initEnc/readEnc/readSw and the v33.0.5 dispatch):
// years on the owner's own KY-040 knobs, where a hand-written transition-table
// decoder on this board did not work on the first try. Kept 1:1 on purpose.
//
//   - Forbes table over a 4-bit history of B and A: +-1 per transition.
//   - REST-POSITION GATE: a detent is dispatched only when A and B are back at
//     rest (both HIGH through the pull-ups, 0b11). That folds the "click down"
//     and "click up" halves of one detent into one step, whatever the part.
//   - 80 ms after a dispatch the pins are not decoded at all, and the history
//     is re-synced from the pins, so bounce cannot build a phantom step.
//   - The switch: three low samples start a press; released under 500 ms it is
//     a click, held past 1000 ms it is a long press (fired once, while held).
static const uint32_t kEncDebounceMs    = 80;
static const uint32_t kSwShortMaxMs     = 500;
static const uint32_t kSwLongMs         = 1000;

#ifndef CTRL_AB_ACTIVE_HIGH
#define CTRL_AB_ACTIVE_HIGH 0      // common to GND, internal pull-ups: the flagship's wiring
#endif

// CTRL_REVERSE 1 if clockwise walks backwards - instead of swapping A and B.
#ifndef CTRL_REVERSE
#define CTRL_REVERSE 0
#endif

static const int kEncTab[16] = {0,-1,0,0, 1,0,0,0, 0,0,0,1, 0,0,-1,0};

static int      s_encDir = 0;
static int      s_lastEnc = 0;
static uint32_t s_lastDispatchMs = 0;

static int      s_swHist = 0;
static uint32_t s_swPressStart = 0;
static bool     s_swLongDone = false;

// A tiny ring: a fast twist can outrun one frame, and dropping detents makes a
// knob feel broken in a way users never forgive.
static CtrlEvent s_q[16];
static uint8_t   s_qHead = 0, s_qTail = 0;

// CTRL_DEBUG: pin levels, transitions and every event on serial - for telling
// wiring from firmware when a knob does nothing. Build with
// PLATFORMIO_BUILD_FLAGS=-DCTRL_DEBUG; never in a release.
#if defined(CTRL_DEBUG)
static uint32_t s_dbgIdleAt = 0;
static int      s_dbgPrevAB = -1;
#endif

static void push(CtrlEvent e) {
#if defined(CTRL_DEBUG)
  Serial.printf("[ctrl] event %s\n", e == CTRL_CW ? "CW" : e == CTRL_CCW ? "CCW" : e == CTRL_PRESS ? "PRESS" : "LONG");
#endif
  const uint8_t n = (uint8_t)((s_qHead + 1) % 16);
  if (n == s_qTail) return;         // full: drop the newest, keep the order
  s_q[s_qHead] = e;
  s_qHead = n;
}

// Logical contact levels: 1 = open, as with the flagship's pull-ups, whichever
// way this board is wired.
static inline int pinA() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_A) : digitalRead(CTRL_PIN_A); }
static inline int pinB() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_B) : digitalRead(CTRL_PIN_B); }

static inline int encAB() {        // B is bit 1, A is bit 0 - as in the flagship
  return (pinB() << 1) | pinA();
}

void controlBegin() {
#if CTRL_AB_ACTIVE_HIGH
  pinMode(CTRL_PIN_A,  INPUT_PULLDOWN);   // agrees with the board's own 10 k
  pinMode(CTRL_PIN_B,  INPUT_PULLDOWN);
#else
  pinMode(CTRL_PIN_A,  INPUT_PULLUP);
  pinMode(CTRL_PIN_B,  INPUT_PULLUP);
#endif
  pinMode(CTRL_PIN_SW, INPUT_PULLUP);      // BOOT: pressed pulls GPIO0 to GND
  s_lastEnc = encAB() * 5;         // old = new
  s_encDir = 0;
  s_qHead = s_qTail = 0;
#if defined(CTRL_DEBUG)
  Serial.printf("[ctrl] begin: A(IO%d)=%d B(IO%d)=%d SW(IO%d)=%d\n",
                CTRL_PIN_A, digitalRead(CTRL_PIN_A), CTRL_PIN_B, digitalRead(CTRL_PIN_B),
                CTRL_PIN_SW, digitalRead(CTRL_PIN_SW));
#endif
}

void controlLoop() {
  const uint32_t now = millis();

#if defined(CTRL_DEBUG)
  const int ab = encAB();
  if (ab != s_dbgPrevAB) {
    Serial.printf("[ctrl] AB %d%d\n", (ab >> 1) & 1, ab & 1);
    s_dbgPrevAB = ab;
  }
  if (now - s_dbgIdleAt > 5000) {
    s_dbgIdleAt = now;
    Serial.printf("[ctrl] levels A=%d B=%d SW=%d\n", digitalRead(CTRL_PIN_A),
                  digitalRead(CTRL_PIN_B), digitalRead(CTRL_PIN_SW));
  }
#endif

  // ---- rotation: debounce gate, decode, rest-position dispatch
  if ((now - s_lastDispatchMs) < kEncDebounceMs) {
    s_encDir = 0;
    s_lastEnc = encAB() * 5;
  } else {
    s_lastEnc = (s_lastEnc >> 2) | (pinB() << 3) | (pinA() << 2);
    s_encDir += kEncTab[s_lastEnc & 0x0F];
  }
  if (s_encDir != 0 && encAB() == 0b11) {
    s_lastDispatchMs = now;
    const bool forward = (s_encDir > 0) != (CTRL_REVERSE != 0);
    s_encDir = 0;
    s_lastEnc = 0b11 * 5;
    push(forward ? CTRL_CW : CTRL_CCW);
  }

  // ---- switch: the flagship's readSw, minus its AP-mode gesture
  if (digitalRead(CTRL_PIN_SW) == LOW) {
    s_swHist++;
    if (s_swHist == 3 && s_swPressStart == 0) s_swPressStart = now;
    if (s_swPressStart && (now - s_swPressStart) > kSwLongMs && !s_swLongDone) {
      s_swLongDone = true;
      push(CTRL_LONG);
    }
  } else {
    if (s_swHist > 0 && s_swPressStart && !s_swLongDone) {
      if ((now - s_swPressStart) < kSwShortMaxMs) push(CTRL_PRESS);
    }
    s_swHist = 0;
    s_swPressStart = 0;
    s_swLongDone = false;
  }
}

CtrlEvent controlTake() {
  if (s_qTail == s_qHead) return CTRL_NONE;
  const CtrlEvent e = s_q[s_qTail];
  s_qTail = (uint8_t)((s_qTail + 1) % 16);
  return e;
}

bool     controlHeld()   { return s_swPressStart != 0; }
uint32_t controlHeldMs() { return s_swPressStart ? (millis() - s_swPressStart) : 0; }

#endif  // CONTROL_ENCODER_ENABLED

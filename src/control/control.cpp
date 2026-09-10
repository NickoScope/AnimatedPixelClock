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
//        UNVERIFIED on hardware: read the eFuse with esptool before trusting it.
//   IO46 gates ROM message printing at boot. Cosmetic; it does not stop a boot.
//
// The switch has nowhere to go on the header, so it shares GPIO0 with the BOOT
// button - which means a wire to the button pad, not a header pin. The cost is
// unchanged: a knob held through reset lands in download mode and recovers on
// the next one, and the same gesture flashes the board without opening the case.
#if defined(BOARD_WAVESHARE_RGB_MATRIX)
  #define CTRL_PIN_A   45     // header U8 pin 1
  #define CTRL_PIN_B   46     // header U8 pin 2
  #define CTRL_PIN_SW   0     // BOOT button pad, shared
#else
  #error "CONTROL_ENCODER_ENABLED: no pin map for this board"
#endif

static const uint32_t CTRL_LONG_MS     = 700;
static const uint32_t CTRL_DEBOUNCE_MS = 25;

// Quadrature decoding by transition table. A full detent on an EC11 is four
// state changes; the table scores each transition +1, -1 or 0, and only a
// complete +4 or -4 counts. Illegal transitions - which is what contact bounce
// looks like - score 0 and cancel themselves out, so no RC filtering is needed.
static const int8_t kQuadrature[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0,
};

static uint8_t  s_prevAB   = 0;
static int8_t   s_accum    = 0;
static bool     s_swDown   = false;
static bool     s_longSent = false;
static uint32_t s_swEdge   = 0;
static uint32_t s_swDownAt = 0;

// A tiny ring: a fast twist can outrun one frame, and dropping detents makes a
// knob feel broken in a way users never forgive.
static CtrlEvent s_q[16];
static uint8_t   s_qHead = 0, s_qTail = 0;

static void push(CtrlEvent e) {
  const uint8_t n = (uint8_t)((s_qHead + 1) % 16);
  if (n == s_qTail) return;         // full: drop the newest, keep the order
  s_q[s_qHead] = e;
  s_qHead = n;
}

void controlBegin() {
  pinMode(CTRL_PIN_A,  INPUT_PULLUP);
  pinMode(CTRL_PIN_B,  INPUT_PULLUP);
  pinMode(CTRL_PIN_SW, INPUT_PULLUP);
  s_prevAB = (uint8_t)((digitalRead(CTRL_PIN_A) << 1) | digitalRead(CTRL_PIN_B));
  s_accum = 0;
  s_qHead = s_qTail = 0;
}

void controlLoop() {
  const uint8_t ab = (uint8_t)((digitalRead(CTRL_PIN_A) << 1) | digitalRead(CTRL_PIN_B));
  if (ab != s_prevAB) {
    s_accum += kQuadrature[(s_prevAB << 2) | ab];
    s_prevAB = ab;
    if (s_accum >= 4)      { push(CTRL_CW);  s_accum = 0; }
    else if (s_accum <= -4){ push(CTRL_CCW); s_accum = 0; }
  }

  const uint32_t now = millis();
  const bool down = (digitalRead(CTRL_PIN_SW) == LOW);
  if (down != s_swDown && (now - s_swEdge) > CTRL_DEBOUNCE_MS) {
    s_swEdge = now;
    s_swDown = down;
    if (down) { s_swDownAt = now; s_longSent = false; }
    else if (!s_longSent) push(CTRL_PRESS);   // long press already fired its own
  }
  // Fires on the threshold, not on release: the page can act while the knob is
  // still held, which is what makes a long press feel like it worked.
  if (s_swDown && !s_longSent && (now - s_swDownAt) >= CTRL_LONG_MS) {
    s_longSent = true;
    push(CTRL_LONG);
  }
}

CtrlEvent controlTake() {
  if (s_qTail == s_qHead) return CTRL_NONE;
  const CtrlEvent e = s_q[s_qTail];
  s_qTail = (uint8_t)((s_qTail + 1) % 16);
  return e;
}

bool     controlHeld()   { return s_swDown; }
uint32_t controlHeldMs() { return s_swDown ? (millis() - s_swDownAt) : 0; }

#endif  // CONTROL_ENCODER_ENABLED

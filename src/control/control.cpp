#include "control.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <Arduino.h>
#include <esp_timer.h>

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

// The decoder is the NickoScope32 S3 bridge's (flagship main.cpp readEnc and
// the v33.0.5 rest-position dispatch): the Forbes table over a 4-bit history,
// and a step only when A and B are back at a detent. Three things changed after
// it ran on this board and missed clicks - which the owner says the flagship
// suffered from too:
//
//   - It is sampled every millisecond by an esp_timer, not once per loop().
//     loop() renders, talks MQTT and serves the web page, and one pass can take
//     tens of milliseconds; a quadrature state shorter than that was never seen,
//     and the step it belonged to was lost.
//   - A detent is not always at A=B=1. Knobs with a detent every half cycle also
//     rest at A=B=0, and a gate that accepts only 11 drops every other click. A
//     knob seen resting at 00 switches the gate to both states, once, for good.
//   - The flagship's 80 ms lock-out after a step, needed at loop speed, drops
//     every step of a turn faster than about 12 clicks a second. At 1 kHz the
//     table and the rest gate reject bounce by themselves, so it is
//     CTRL_ENC_LOCKOUT_MS, 10 ms by default.
//
// The switch is sampled by the same timer: pressed after CTRL_SW_DEBOUNCE_MS of
// steady LOW, a click if released within 500 ms, a long press once held for a
// second - the flagship's thresholds.

// CTRL_REVERSE 1 if clockwise walks backwards - instead of swapping A and B.
#ifndef CTRL_REVERSE
#define CTRL_REVERSE 0
#endif
#ifndef CTRL_ENC_LOCKOUT_MS
#define CTRL_ENC_LOCKOUT_MS 10
#endif
#ifndef CTRL_SW_DEBOUNCE_MS
#define CTRL_SW_DEBOUNCE_MS 20
#endif
// -1 find out from the knob (default), 0 detents at 11 only, 1 at 11 and 00.
#ifndef CTRL_ENC_HALF_DETENT
#define CTRL_ENC_HALF_DETENT -1
#endif

static const uint64_t kSampleUs     = 1000;
static const uint32_t kSwShortMaxMs = 500;
static const uint32_t kSwLongMs     = 1000;
static const uint32_t kRestSeenMs   = 250;   // 00 held this long is a detent, not a passing state

static const int kEncTab[16] = {0,-1,0,0, 1,0,0,0, 0,0,0,1, 0,0,-1,0};

// Single producer (the sampling task) and single consumer (loop()): the head is
// written only by the producer, the tail only by the consumer.
static CtrlEvent        s_q[16];
static volatile uint8_t s_qHead = 0;
static volatile uint8_t s_qTail = 0;

static bool     s_timerOk = false;
static int      s_encDir = 0;
static int      s_lastEnc = 0;
static uint32_t s_lastStepMs = 0;
static int8_t   s_halfDetent = CTRL_ENC_HALF_DETENT;
static int      s_prevAB = -1;
static uint32_t s_abSinceMs = 0;

static bool     s_swRaw = false;         // last sample, true = pressed
static uint32_t s_swRawSinceMs = 0;
static volatile bool s_swDown = false;   // debounced
static uint32_t s_swDownAtMs = 0;
static bool     s_swLongSent = false;

// CTRL_DEBUG: pin levels, the detent mode, transition and step counts, and every
// event, on serial. Build with PLATFORMIO_BUILD_FLAGS=-DCTRL_DEBUG; never in a
// release.
#if defined(CTRL_DEBUG)
static volatile uint32_t s_dbgTransitions = 0;
static volatile uint32_t s_dbgSteps = 0;
static uint32_t s_dbgAt = 0;
#endif

// Logical contact levels: 1 = open, as with the flagship's pull-ups, whichever
// way this board is wired.
static inline int pinA() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_A) : digitalRead(CTRL_PIN_A); }
static inline int pinB() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_B) : digitalRead(CTRL_PIN_B); }
static inline int encAB() { return (pinB() << 1) | pinA(); }   // B bit 1, A bit 0

static void push(CtrlEvent e) {
  const uint8_t n = (uint8_t)((s_qHead + 1) % 16);
  if (n == s_qTail) return;         // full: drop the newest, keep the order
  s_q[s_qHead] = e;
  s_qHead = n;
}

static void sampleTick(void *) {
  const uint32_t now = millis();

  // ---- rotation
  const int ab = encAB();
  if (ab != s_prevAB) {
    s_prevAB = ab;
    s_abSinceMs = now;
#if defined(CTRL_DEBUG)
    s_dbgTransitions = s_dbgTransitions + 1;
#endif
  }
  if (s_halfDetent < 0 && ab == 0b00 && (now - s_abSinceMs) >= kRestSeenMs) s_halfDetent = 1;

  if ((now - s_lastStepMs) < CTRL_ENC_LOCKOUT_MS) {
    s_encDir = 0;
    s_lastEnc = ab * 5;             // old = new
  } else {
    s_lastEnc = (s_lastEnc >> 2) | (ab << 2);
    s_encDir += kEncTab[s_lastEnc & 0x0F];
  }
  const bool atDetent = (ab == 0b11) || (s_halfDetent == 1 && ab == 0b00);
  if (s_encDir != 0 && atDetent) {
    s_lastStepMs = now;
    const bool forward = (s_encDir > 0) != (CTRL_REVERSE != 0);
    s_encDir = 0;
    s_lastEnc = ab * 5;
    push(forward ? CTRL_CW : CTRL_CCW);
#if defined(CTRL_DEBUG)
    s_dbgSteps = s_dbgSteps + 1;
#endif
  }

  // ---- switch
  const bool raw = (digitalRead(CTRL_PIN_SW) == LOW);
  if (raw != s_swRaw) { s_swRaw = raw; s_swRawSinceMs = now; }
  if (raw != s_swDown && (now - s_swRawSinceMs) >= CTRL_SW_DEBOUNCE_MS) {
    s_swDown = raw;
    if (raw) { s_swDownAtMs = now; s_swLongSent = false; }
    else if (!s_swLongSent && (now - s_swDownAtMs) < kSwShortMaxMs) push(CTRL_PRESS);
  }
  if (s_swDown && !s_swLongSent && (now - s_swDownAtMs) >= kSwLongMs) {
    s_swLongSent = true;
    push(CTRL_LONG);
  }
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
  s_prevAB = encAB();
  s_abSinceMs = millis();
  s_lastEnc = s_prevAB * 5;
  s_encDir = 0;
  s_qHead = s_qTail = 0;

  esp_timer_create_args_t args = {};
  args.callback = sampleTick;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "ctrl";
  esp_timer_handle_t timer = nullptr;
  s_timerOk = esp_timer_create(&args, &timer) == ESP_OK &&
              esp_timer_start_periodic(timer, kSampleUs) == ESP_OK;
#if defined(CTRL_DEBUG)
  Serial.printf("[ctrl] begin: A(IO%d)=%d B(IO%d)=%d SW(IO%d)=%d, sampling %s, lockout %d ms\n",
                CTRL_PIN_A, digitalRead(CTRL_PIN_A), CTRL_PIN_B, digitalRead(CTRL_PIN_B),
                CTRL_PIN_SW, digitalRead(CTRL_PIN_SW), s_timerOk ? "1 kHz timer" : "loop (timer failed)",
                CTRL_ENC_LOCKOUT_MS);
#endif
}

void controlLoop() {
  if (!s_timerOk) sampleTick(nullptr);    // no timer: sample at loop speed, as before
#if defined(CTRL_DEBUG)
  if (millis() - s_dbgAt > 5000) {
    s_dbgAt = millis();
    Serial.printf("[ctrl] levels A=%d B=%d SW=%d | detents %s | transitions %u steps %u\n",
                  digitalRead(CTRL_PIN_A), digitalRead(CTRL_PIN_B), digitalRead(CTRL_PIN_SW),
                  s_halfDetent == 1 ? "11+00" : s_halfDetent == 0 ? "11" : "11 (watching for 00)",
                  (unsigned)s_dbgTransitions, (unsigned)s_dbgSteps);
  }
#endif
}

CtrlEvent controlTake() {
  if (s_qTail == s_qHead) return CTRL_NONE;
  const CtrlEvent e = s_q[s_qTail];
  s_qTail = (uint8_t)((s_qTail + 1) % 16);
#if defined(CTRL_DEBUG)
  Serial.printf("[ctrl] event %s\n", e == CTRL_CW ? "CW" : e == CTRL_CCW ? "CCW" : e == CTRL_PRESS ? "PRESS" : "LONG");
#endif
  return e;
}

bool     controlHeld()   { return s_swDown; }
uint32_t controlHeldMs() { return s_swDown ? (millis() - s_swDownAtMs) : 0; }

#endif  // CONTROL_ENCODER_ENABLED

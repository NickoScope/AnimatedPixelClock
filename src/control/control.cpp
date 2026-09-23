#include "control.h"

#if defined(CONTROL_ENCODER_ENABLED)

#include <Arduino.h>
#include <esp_timer.h>

#if defined(IR_ENABLED)
#include "../ir/ir.h"   // the remote produces detents and a button level; see below
#endif

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

// CTRL_REVERSE, CTRL_ENC_LOCKOUT_MS, CTRL_SW_DEBOUNCE_MS and CTRL_ENC_HALF_DETENT
// are defaults now, defined in control.h; the values in force are the s_cfg*
// fields below.

static const uint64_t kSampleUs     = 1000;
static const uint32_t kSwShortMaxMs = 500;
static const uint32_t kSwLongMs     = 1000;
static const uint32_t kRestSeenMs   = 250;   // 00 held this long is a detent, not a passing state

static const int kEncTab[16] = {0,-1,0,0, 1,0,0,0, 0,0,0,1, 0,0,-1,0};

// Glitch filter. On the bench, with nobody at the knob, a 15 s burst produced
// 18 clockwise steps from 54 A/B changes - three changes a step where a real
// click of this knob makes two, and the lines sit next to a HUB75 panel
// switching amps. The A/B pair now counts as a new state only once
// CTRL_ENC_STABLE_MS consecutive 1 kHz samples agree on it. The pair, not each
// pin: a coupled pulse that walks both lines through a valid sequence in a
// millisecond a state would pass a per-pin filter, because each pin in a
// quadrature sequence holds for two states. A real contact state lasts several
// milliseconds even on a fast turn.
#ifndef CTRL_ENC_STABLE_MS
#define CTRL_ENC_STABLE_MS 2
#endif
static int     s_abShown = 0b11;
static int     s_abCand  = 0b11;
static uint8_t s_abRun   = 0;
static inline int filteredAB(int raw) {
  if (raw == s_abShown) { s_abCand = raw; s_abRun = 0; return s_abShown; }
  if (raw != s_abCand)  { s_abCand = raw; s_abRun = 1; }
  else if (s_abRun < 255) s_abRun++;
  if (s_abRun >= CTRL_ENC_STABLE_MS) { s_abShown = raw; s_abRun = 0; }
  return s_abShown;
}

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

// Run-time settings. Written only by loop() through controlConfigure(), read by
// the sampling task. Each is one byte or one word, so a read sees the old value
// or the new one, never a mix; nothing depends on two of them agreeing.
// s_halfDetent is the timer's own state, so a new detent mode is handed over by
// generation: the fields are stored first and the generation last, and the
// timer copies the mode in when it sees a generation it has not seen.
static volatile bool     s_cfgReverse    = CTRL_REVERSE != 0;
static volatile uint16_t s_cfgLockoutMs  = CTRL_ENC_LOCKOUT_MS;
static volatile uint16_t s_cfgDebounceMs = CTRL_SW_DEBOUNCE_MS;
#if defined(IR_RX_ENABLED) && (IR_PIN == CTRL_PIN_SW)
// GPIO0 carries the BOOT button, the knob's switch and the IR receiver at once:
// all three only ever pull it low. See controlConfigure().
#define CTRL_SW_DEBOUNCE_MIN_MS 12
static_assert(CTRL_SW_DEBOUNCE_MS >= CTRL_SW_DEBOUNCE_MIN_MS,
              "the default switch debounce would read IR marks as presses");
#endif
static volatile int8_t   s_cfgDetent     = CTRL_ENC_HALF_DETENT;
static volatile uint8_t  s_cfgGen        = 0;
static uint8_t           s_cfgSeenGen    = 0;   // sampling task only

// Knob tester counters: written by the sampling task only.
static volatile uint32_t s_nCw = 0, s_nCcw = 0, s_nPress = 0, s_nLong = 0;
static volatile uint8_t  s_lastEvent = CTRL_NONE;
static volatile uint32_t s_lastEventMs = 0;

// CTRL_DEBUG: pin levels, the detent mode, transition and step counts, and every
// event, on serial. Build with PLATFORMIO_BUILD_FLAGS=-DCTRL_DEBUG; never in a
// release.
#if defined(CTRL_DEBUG)
static volatile uint32_t s_dbgTransitions = 0;
static volatile uint32_t s_dbgSteps = 0;
static uint32_t s_dbgAt = 0;
// Every raw A/B change before the filter, with its time, so the width of a
// phantom pulse is measured rather than guessed.
static uint32_t          s_rawMs[64];
static uint8_t           s_rawAb[64];
static volatile uint8_t  s_rawHead = 0;
static uint8_t           s_rawTail = 0;
static int               s_rawPrev = -1;
#endif

// Logical contact levels: 1 = open, as with the flagship's pull-ups, whichever
// way this board is wired.
static inline int pinA() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_A) : digitalRead(CTRL_PIN_A); }
static inline int pinB() { return CTRL_AB_ACTIVE_HIGH ? !digitalRead(CTRL_PIN_B) : digitalRead(CTRL_PIN_B); }
static inline int encAB() { return (pinB() << 1) | pinA(); }   // B bit 1, A bit 0

static void push(CtrlEvent e) {
  switch (e) {
  case CTRL_CW:    s_nCw = s_nCw + 1; break;
  case CTRL_CCW:   s_nCcw = s_nCcw + 1; break;
  case CTRL_PRESS: s_nPress = s_nPress + 1; break;
  case CTRL_LONG:  s_nLong = s_nLong + 1; break;
  default: break;
  }
  s_lastEvent = e;
  s_lastEventMs = millis();
  if (!s_lastEventMs) s_lastEventMs = 1;   // 0 means none yet
  const uint8_t n = (uint8_t)((s_qHead + 1) % 16);
  if (n == s_qTail) return;         // full: drop the newest, keep the order
  s_q[s_qHead] = e;
  s_qHead = n;
}

static void sampleTick(void *) {
  const uint32_t now = millis();
  if (s_cfgGen != s_cfgSeenGen) {           // the portal changed the detent mode
    s_cfgSeenGen = s_cfgGen;
    s_halfDetent = s_cfgDetent;
  }
  const uint16_t lockoutMs  = s_cfgLockoutMs;
  const uint16_t debounceMs = s_cfgDebounceMs;

  // ---- rotation
  const int rawAB = encAB();
#if defined(CTRL_DEBUG)
  if (rawAB != s_rawPrev) {
    s_rawPrev = rawAB;
    const uint8_t n = (uint8_t)((s_rawHead + 1) % 64);
    if (n != s_rawTail) { s_rawMs[s_rawHead] = now; s_rawAb[s_rawHead] = (uint8_t)rawAB; s_rawHead = n; }
  }
#endif
  const int ab = filteredAB(rawAB);
  if (ab != s_prevAB) {
    s_prevAB = ab;
    s_abSinceMs = now;
#if defined(CTRL_DEBUG)
    s_dbgTransitions = s_dbgTransitions + 1;
#endif
  }
  if (s_halfDetent < 0 && ab == 0b00 && (now - s_abSinceMs) >= kRestSeenMs) s_halfDetent = 1;

  if ((now - s_lastStepMs) < lockoutMs) {
    s_encDir = 0;
    s_lastEnc = ab * 5;             // old = new
  } else {
    s_lastEnc = (s_lastEnc >> 2) | (ab << 2);
    s_encDir += kEncTab[s_lastEnc & 0x0F];
  }
#if defined(IR_ENABLED)
  // The remote's detents are drained here, inside the sampling task, so that
  // the task stays the only writer of the event queue - loop() pushing events
  // of its own would make two producers of a queue built for one. The knob's
  // reverse setting is not applied: it exists to fix how one knob is wired, and
  // the remote's left is left. src/ir/ir.h.
  for (int8_t irRot = irTakeRotate(); irRot != 0; irRot += (irRot > 0 ? -1 : 1))
    push(irRot > 0 ? CTRL_CW : CTRL_CCW);
#endif
  const bool atDetent = (ab == 0b11) || (s_halfDetent == 1 && ab == 0b00);
  if (s_encDir != 0 && atDetent) {
    s_lastStepMs = now;
    const bool forward = (s_encDir > 0) != s_cfgReverse;
    s_encDir = 0;
    s_lastEnc = ab * 5;
    push(forward ? CTRL_CW : CTRL_CCW);
#if defined(CTRL_DEBUG)
    s_dbgSteps = s_dbgSteps + 1;
#endif
  }

  // ---- switch
  // The remote's button is a level, exactly like the switch's, so the debounce,
  // the 500 ms click and the 1 s long press below decide what it was - one
  // implementation of the gesture rather than two that drift.
#if defined(IR_ENABLED)
  const bool raw = (digitalRead(CTRL_PIN_SW) == LOW) || irOkDown(now);
#else
  const bool raw = (digitalRead(CTRL_PIN_SW) == LOW);
#endif
  if (raw != s_swRaw) { s_swRaw = raw; s_swRawSinceMs = now; }
  if (raw != s_swDown && (now - s_swRawSinceMs) >= debounceMs) {
    if (raw) { s_swDownAtMs = now; s_swLongSent = false; }
    else if (!s_swLongSent && (now - s_swDownAtMs) < kSwShortMaxMs) push(CTRL_PRESS);
    s_swDown = raw;                 // last: a reader never pairs it with an old press time
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
  s_abShown = s_abCand = s_prevAB;
  s_abRun = 0;
  s_abSinceMs = millis();
  s_lastEnc = s_prevAB * 5;
  s_encDir = 0;
  s_qHead = s_qTail = 0;

  esp_timer_create_args_t args = {};
  args.callback = sampleTick;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "ctrl";
  // A periodic esp_timer that was held up (WiFi, a flash write) otherwise runs
  // the missed callbacks back to back, and the 2 ms filter would see two
  // samples microseconds apart as two milliseconds.
  args.skip_unhandled_events = true;
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
  while (s_rawTail != s_rawHead) {
    Serial.printf("[ctrl] raw %u ms ab=%d%d\n", (unsigned)s_rawMs[s_rawTail],
                  (s_rawAb[s_rawTail] >> 1) & 1, s_rawAb[s_rawTail] & 1);
    s_rawTail = (uint8_t)((s_rawTail + 1) % 64);
  }
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

void controlConfigure(bool reverse, uint16_t lockoutMs, uint16_t debounceMs, int8_t detent) {
  s_cfgReverse    = reverse;
  s_cfgLockoutMs  = lockoutMs;
#if defined(IR_RX_ENABLED) && (IR_PIN == CTRL_PIN_SW)
  // The receiver shares this line. A press is told from IR by duration: the
  // longest IR mark is NEC's 9 ms leader (Vishay 80071), so a debounce under
  // it would read a remote's frame as the button. 12 ms is our margin.
  if (debounceMs < CTRL_SW_DEBOUNCE_MIN_MS) debounceMs = CTRL_SW_DEBOUNCE_MIN_MS;
#endif
  s_cfgDebounceMs = debounceMs;
  if (detent != s_cfgDetent) {
    s_cfgDetent = detent;
    s_cfgGen    = (uint8_t)(s_cfgGen + 1);   // last: the timer acts on it
  }
}

void controlStats(CtrlStats *out) {
  if (!out) return;
  out->cw        = s_nCw;
  out->ccw       = s_nCcw;
  out->press     = s_nPress;
  out->longPress = s_nLong;
  out->last      = s_lastEvent;
  out->lastMs    = s_lastEventMs;
  out->detent    = s_halfDetent;
  out->timer     = s_timerOk;
}

bool     controlHeld()   { return s_swDown; }
uint32_t controlHeldMs() { return s_swDown ? (millis() - s_swDownAtMs) : 0; }

#endif  // CONTROL_ENCODER_ENABLED

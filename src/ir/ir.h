#pragma once
// The infrared remote, as a second knob.
//
// The panel is driven by one EC11 encoder today (src/control). The owner's plan
// is to retire the knob and drive the same gestures from a remote, so this
// module does not invent a control scheme of its own: it produces exactly what
// the knob produces - detents and a button level - and hands them to the
// encoder's own state machine. Single, long press, the browse/enter logic and
// every page's behaviour then follow by construction rather than by a second
// implementation that drifts.
//
// Ported from NickoScope32's ADD-79 ir_remote.{h,cpp} (v33.55.0), which is
// where the shape comes from: learned codes rather than hard-coded ones, NVS so
// a remote survives a reflash, and a simulator that injects a slot at exactly
// the level a real frame reaches. What is ours: the rules live in ir_map.h as
// plain C++ and are tested on the host, the way climate_model.h and
// presence_model.h are; the receiver is a separate flag, so a build can carry
// the module with no library and no heap; and the seam is the encoder's
// sampling task, not loop(), so the event queue keeps its single producer.
//
// src/ir/README.md has the hardware, the pin and the serial console.

#include <ArduinoJson.h>
#include <stdint.h>

#include "ir_map.h"   // the slots and the rules, in every build that includes us

#if defined(IR_RX_ENABLED) && !defined(IR_ENABLED)
#error "IR_RX_ENABLED needs IR_ENABLED: the receiver feeds this module's decoder"
#endif

// The receiver's pin: GPIO0, the BOOT line (owner's decision 2026-09-23, so
// IO45 and IO46 stay free for later). The vendor schematic's reset/boot
// circuit pulls it up with R8 10 kOhm and leaves C9 across the button unfitted:
// an open-collector receiver needs no resistor of its own there, and the line
// carries no capacitance to smear the pulses. It idles high, which is also
// the normal-boot level of this strapping pin. The BOOT button and the knob's
// switch share the line; src/control tells a press from IR by duration.
// Knowledge base, docs/24-ir-remote.md.
#ifndef IR_PIN
#define IR_PIN 0
#endif

#if defined(IR_ENABLED)

// ── lifecycle ───────────────────────────────────────────────────────────────
void irBegin();    // in setup(), after loadSettings(): the learned map, then the receiver
void irLoop();     // every loop() pass: frames, the learn window, the serial console. No allocation.

// ── the seam into the encoder (src/control/control.cpp) ─────────────────────
// Both are called from the encoder's 1 kHz sampling task, so that the task
// stays the only writer of the event queue. Each is a handful of instructions
// and takes the module's spinlock, never a mutex: a lock held across a task
// switch inside that callback would delay every knob sample.
int8_t irTakeRotate();          // detents accumulated since the last call, -3..+3, 0 = none
bool   irOkDown(uint32_t nowMs);  // the button's level, held while repeat frames keep arriving

// ── the portal and /api/info ────────────────────────────────────────────────
void irInfoJson(JsonObject out);      // /api/info's "ir"
void irSettingsChanged();             // after the portal or an import changed one of ours

// The learn service, driven by the portal (and by the serial console). All of
// it runs in loop(): NVS writes and the map are touched nowhere else.
bool     irLearnArm(uint8_t slot);    // false for a slot this build does not have
void     irLearnCancel();
int      irLearnActiveSlot();         // -1 when no window is open
uint32_t irLearnRemainMs();
bool     irClearSlot(uint8_t slot);
void     irClearAll();

// Inject a slot at the level a decoded frame reaches, so the whole chain -
// seam, encoder state machine, pages - is exercised with no receiver soldered
// and no codes learned. holdMs applies to the button only: 0 is a click, and
// anything past the encoder's long-press threshold is a long press.
bool irSimulate(uint8_t slot, uint32_t holdMs);

#endif  // IR_ENABLED

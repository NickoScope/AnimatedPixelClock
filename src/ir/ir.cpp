#include "ir.h"

#if defined(IR_ENABLED)

#include <Arduino.h>
#include <Preferences.h>

#include "../config/config.h"
#include "ir_console.h"

#if defined(IR_RX_ENABLED)
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#endif

// ── the pin ─────────────────────────────────────────────────────────────────
// This board has no free GPIO: the expansion header U8 is IO45, IO46, GND and
// 3V3, and the knob holds both pins (src/control/control.cpp says where that
// comes from). So the remote's receiver takes the knob's place rather than
// sitting next to it - which is the point of the module, and why the two must
// not be built for the same pin. A build that tries is refused below.
//
// The board pulls IO45 and IO46 down with 10 kOhm (schematic R59, R60), and a
// Vishay receiver's OUT is an open collector with a 30 kOhm pull-up inside it
// (datasheet 82459 rev 2.4, block diagram, read 2026-09-16). Those two in
// series leave an idle line at 3.3 x 10 / (30 + 10) = 0.83 V, and the ESP32-S3
// wants 0.75 x VDD = 2.48 V to read a one: without help the receiver would look
// permanently busy. An external 2.2 kOhm from OUT to 3V3 puts the idle line at
// 2.74 V and draws 1.6 mA when the receiver pulls down, inside its 5 mA rating.
// CALCULATED, NOT MEASURED - src/ir/README.md carries it as the first thing to
// check on the bench when the receiver is soldered.
//
// And IO45 and IO46 are strapping pins. IO45 selects VDD_SPI's voltage, so a
// pull-up to 3V3 on it would be fatal on a module whose flash and PSRAM run at
// 3.3 V - it would force them to 1.8 V and the board would not boot. This
// module is safe because VDD_SPI is fixed at 1.8 V by the VDD_SPI_FORCE eFuse
// (read off this board on 2026-09-14, and esptool reports "Embedded PSRAM 16MB
// (AP_1v8)"), which is exactly why the knob may already pull them about. Check
// the fuse with espefuse summary before soldering anything to IO45 on another
// board. IO46 gates ROM messages at boot and, with GPIO0, picks the boot mode.
// IR_PIN is in ir.h (GPIO0 by default). The comment above is the history of
// the IO45 plan and still holds for anyone building IR_PIN=45.
#if defined(IR_RX_ENABLED) && defined(CONTROL_ENCODER_ENABLED) && defined(BOARD_WAVESHARE_RGB_MATRIX)
#if (IR_PIN == 45) || (IR_PIN == 46)
#error "IR_RX_ENABLED and CONTROL_ENCODER_ENABLED want the same pin (IO45/IO46 carry the knob). Keep IR_PIN on GPIO0, where it shares the line with the knob's switch by design."
#endif
#endif

// ── the decoder's state ─────────────────────────────────────────────────────
// Two tasks reach it. loop() writes: irLoop(), the portal's handlers, the
// serial console. The encoder's 1 kHz sampling task reads, through the seam.
// A spinlock covers every entry point, the way src/presence does; the rules in
// ir_map.h hold no lock of their own, so the host tests still build them on a
// Mac. Nothing slow ever runs inside the lock - NVS writes happen after it is
// released, from what the decoder reported.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ir::Decoder  s_dec;
static bool         s_begun = false;

#if defined(IR_RX_ENABLED)
// 256 raw samples, not the library's 1024. With save_buffer there are two
// buffers of uint16, so 1024 would cost about 4 KB of internal heap - and
// internal heap is the scarce resource on this board (docs/22 §12.3: the panel
// idles at ~35 KB and a portal page load spikes ~20 KB). A NEC frame is about
// 68 samples, so 256 is a factor of three of headroom and costs ~1 KB. An air
// conditioner's remote would overflow it; those frames are dropped, and the
// counter below says so.
static IRrecv s_recv(IR_PIN, 256, 15 /* ms of silence ends a frame */, true /* stable copy */);
static uint32_t s_overflows = 0;
static bool     s_rxOn = false;
#endif

// ── NVS ─────────────────────────────────────────────────────────────────────
// Its own namespace, so nothing here can disturb the settings blob. Opened for
// the length of one operation and closed again: a handle held open is a handle
// that outlives a crash half-written.
static const char *kNs = "irmap";

static void keyFor(char *out, size_t n, char kind, uint8_t slot) {
  snprintf(out, n, "%c%u", kind, (unsigned)slot);   // v0..v7, p0..p7
}

static void mapLoad() {
  Preferences p;
  if (!p.begin(kNs, true)) return;   // no namespace yet: nothing learned, which is not an error
  char k[8];
  for (uint8_t i = 0; i < ir::kSlotCount; i++) {
    keyFor(k, sizeof(k), 'v', i);
    if (!p.isKey(k)) continue;
    const uint64_t v = p.getULong64(k, 0);
    keyFor(k, sizeof(k), 'p', i);
    const uint8_t proto = p.getUChar(k, 0);
    if (v) {
      portENTER_CRITICAL(&s_mux);
      s_dec.map().bind(i, proto, v);
      portEXIT_CRITICAL(&s_mux);
    }
  }
  p.end();
}

static void mapSaveSlot(uint8_t slot, uint8_t proto, uint64_t value) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  char k[8];
  keyFor(k, sizeof(k), 'v', slot); p.putULong64(k, value);
  keyFor(k, sizeof(k), 'p', slot); p.putUChar(k, proto);
  p.end();
}

static void mapClearSlot(uint8_t slot) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  char k[8];
  // isKey() first: removing a key that is not there rewrites the whole page for
  // nothing, and on this board that was measured at a visible display freeze
  // (src/config/settings.cpp, the same fix).
  keyFor(k, sizeof(k), 'v', slot); if (p.isKey(k)) p.remove(k);
  keyFor(k, sizeof(k), 'p', slot); if (p.isKey(k)) p.remove(k);
  p.end();
}

// ── the protocol's name ─────────────────────────────────────────────────────
// Without the receiver library there is no table of names, and a build without
// it never learns a code anyway, so the number is the honest answer.
static void protoName(uint8_t proto, char *out, size_t n) {
#if defined(IR_RX_ENABLED)
  const String s = typeToString((decode_type_t)proto, false);
  snprintf(out, n, "%s", s.c_str());
#else
  snprintf(out, n, "%u", (unsigned)proto);
#endif
}

// ── what a frame or a command did, on serial ────────────────────────────────
static void report(const ir::Outcome &o) {
  switch (o.kind) {
    case ir::Outcome::kLearned:
      if (o.movedFrom >= 0)
        Serial.printf("[ir] learned %s (taken from %s)\n", ir::slotName((uint8_t)o.slot),
                      ir::slotName((uint8_t)o.movedFrom));
      else
        Serial.printf("[ir] learned %s\n", ir::slotName((uint8_t)o.slot));
      break;
    case ir::Outcome::kReserved:
      Serial.printf("[ir] %s: learned, but nothing is wired to it yet\n", ir::slotName((uint8_t)o.slot));
      break;
    default:
      break;
  }
}

// Applies one decoded frame or one simulated slot, then does the slow work
// (NVS, serial) outside the lock.
static ir::Outcome apply(uint32_t nowMs, const ir::Frame *f, int16_t simSlot, uint32_t holdMs) {
  ir::Outcome o{ir::Outcome::kNothing, -1, -1};
  uint8_t proto = 0;
  uint64_t value = 0;
  portENTER_CRITICAL(&s_mux);
  o = f ? s_dec.frame(nowMs, *f) : s_dec.simulate(nowMs, (uint8_t)simSlot, holdMs);
  if (o.kind == ir::Outcome::kLearned && o.slot >= 0) {
    proto = s_dec.map().proto((uint8_t)o.slot);
    value = s_dec.map().value((uint8_t)o.slot);
  }
  portEXIT_CRITICAL(&s_mux);

  if (o.kind == ir::Outcome::kLearned && o.slot >= 0) {
    if (o.movedFrom >= 0) mapClearSlot((uint8_t)o.movedFrom);
    mapSaveSlot((uint8_t)o.slot, proto, value);
  }
  report(o);
  return o;
}

// ── the serial console ──────────────────────────────────────────────────────
static char    s_line[96];
static uint8_t s_len = 0;

static void printHelp() {
  Serial.println(F("[ir] ir                  what this module knows"));
  Serial.println(F("[ir] ir cw [n] | ccw [n] n detents, as the knob's"));
  Serial.println(F("[ir] ir ok [ms]          the button; ms holds it (1200 = long press)"));
  Serial.println(F("[ir] ir sim <slot> [ms]  any slot, the path a real frame takes"));
  Serial.println(F("[ir] ir learn <slot>     open the window, then press the remote"));
  Serial.println(F("[ir] ir cancel           close it"));
  Serial.println(F("[ir] ir clear <slot>|all forget learned codes"));
  Serial.print(F("[ir] slots:"));
  for (uint8_t i = 0; i < ir::kSlotCount; i++) Serial.printf(" %u=%s", (unsigned)i, ir::slotName(i));
  Serial.println();
}

static void printStatus() {
  const uint32_t now = millis();
  uint8_t  bound;
  uint32_t frames, ignored;
  int      learn, hit;
  uint32_t learnLeft;
  bool     seen;
  uint8_t  sProto = 0;
  uint64_t sValue = 0;
  bool     sRepeat = false;
  uint32_t sAge = 0;
  portENTER_CRITICAL(&s_mux);
  bound = s_dec.map().boundCount();
  frames = s_dec.frames();
  ignored = s_dec.ignored();
  learn = s_dec.learnActive(now);
  learnLeft = s_dec.learnRemainMs(now);
  hit = s_dec.lastHitSlot(now, 5000);
  seen = s_dec.lastSeen(&sProto, &sValue, &sRepeat, &sAge, now);
  portEXIT_CRITICAL(&s_mux);

#if defined(IR_RX_ENABLED)
  Serial.printf("[ir] receiver on IO%d, %s\n", IR_PIN, s_rxOn ? "listening" : "FAILED TO START");
#else
  Serial.printf("[ir] no receiver in this build (IR_RX_ENABLED off); pin would be IO%d\n", IR_PIN);
#endif
  Serial.printf("[ir] %u of %u slots learned, %lu frames, %lu ignored\n", (unsigned)bound,
                (unsigned)ir::kSlotCount, (unsigned long)frames, (unsigned long)ignored);
  if (learn >= 0)
    Serial.printf("[ir] learning %s - press the button on the remote (%lu ms left)\n",
                  ir::slotName((uint8_t)learn), (unsigned long)learnLeft);
  if (hit >= 0) Serial.printf("[ir] last slot: %s\n", ir::slotName((uint8_t)hit));
  if (seen) {
    char pn[16];
    protoName(sProto, pn, sizeof(pn));
    Serial.printf("[ir] last frame: %s 0x%llX%s, %lu ms ago\n", pn, (unsigned long long)sValue,
                  sRepeat ? " (repeat)" : "", (unsigned long)sAge);
  }
  for (uint8_t i = 0; i < ir::kSlotCount; i++) {
    bool b;
    uint64_t v;
    uint8_t p;
    uint32_t h;
    portENTER_CRITICAL(&s_mux);
    b = s_dec.map().bound(i);
    v = s_dec.map().value(i);
    p = s_dec.map().proto(i);
    h = s_dec.hits(i);
    portEXIT_CRITICAL(&s_mux);
    char pn[16];
    protoName(p, pn, sizeof(pn));
    Serial.printf("[ir]   %u %-11s %s%s  hits %lu\n", (unsigned)i, ir::slotName(i),
                  b ? pn : "-", b ? "" : "", (unsigned long)h);
    if (b) Serial.printf("[ir]     code 0x%llX\n", (unsigned long long)v);
  }
}

static void runCommand(const ir::Command &c) {
  const uint32_t now = millis();
  switch (c.kind) {
    case ir::Command::kHelp: printHelp(); break;
    case ir::Command::kStatus: printStatus(); break;
    case ir::Command::kSim:
      Serial.printf("[ir] sim %s%s\n", ir::slotName(c.slot), c.arg ? " (held)" : "");
      apply(now, nullptr, (int16_t)c.slot, c.arg);
      break;
    case ir::Command::kRepeat:
      Serial.printf("[ir] sim %s x%lu\n", ir::slotName(c.slot), (unsigned long)c.arg);
      // One detent per call, as a real remote sends one frame per press: the
      // seam drains at 1 kHz, so a burst arrives as a burst of detents and the
      // accumulator's ceiling is exercised rather than bypassed.
      for (uint32_t i = 0; i < c.arg; i++) apply(now, nullptr, (int16_t)c.slot, 0);
      break;
    case ir::Command::kLearn:
      portENTER_CRITICAL(&s_mux);
      s_dec.learnArm(c.slot, now);
      portEXIT_CRITICAL(&s_mux);
      Serial.printf("[ir] learning %s - press its button on the remote within %lu s\n",
                    ir::slotName(c.slot), (unsigned long)(ir::kLearnWindowMs / 1000));
#if !defined(IR_RX_ENABLED)
      Serial.println(F("[ir] (this build has no receiver, so nothing will arrive)"));
#endif
      break;
    case ir::Command::kCancel:
      portENTER_CRITICAL(&s_mux);
      s_dec.learnCancel();
      portEXIT_CRITICAL(&s_mux);
      Serial.println(F("[ir] learn window closed"));
      break;
    case ir::Command::kClear:
      portENTER_CRITICAL(&s_mux);
      s_dec.map().clear(c.slot);
      portEXIT_CRITICAL(&s_mux);
      mapClearSlot(c.slot);
      Serial.printf("[ir] %s forgotten\n", ir::slotName(c.slot));
      break;
    case ir::Command::kClearAll:
      irClearAll();
      Serial.println(F("[ir] every code forgotten"));
      break;
    case ir::Command::kError:
      Serial.printf("[ir] %s\n", c.error ? c.error : "no");
      break;
    default:
      break;
  }
}

static void consolePoll() {
  while (Serial.available()) {
    const int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\n' || ch == '\r') {
      if (!s_len) continue;
      s_line[s_len] = 0;
      s_len = 0;
      ir::Command c;
      if (ir::parseCommand(s_line, &c)) runCommand(c);
      continue;
    }
    // A line longer than the buffer is cut, not overrun; the tail becomes the
    // next line, which parses as "not ours" and is dropped.
    if (s_len + 1 < sizeof(s_line)) s_line[s_len++] = (char)ch;
  }
}

// ── lifecycle ───────────────────────────────────────────────────────────────
void irBegin() {
  portENTER_CRITICAL(&s_mux);
  s_dec.reset();
  portEXIT_CRITICAL(&s_mux);
  mapLoad();
  s_begun = true;

  uint8_t bound;
  portENTER_CRITICAL(&s_mux);
  bound = s_dec.map().boundCount();
  portEXIT_CRITICAL(&s_mux);

#if defined(IR_RX_ENABLED)
  if (settings.irEnabled) {
    s_recv.setUnknownThreshold(12);   // shorter bursts are noise, not a protocol
    s_recv.enableIRIn();
    s_rxOn = true;
  }
  Serial.printf("[ir] IO%d, %u/%u slots learned, receiver %s. Type `ir help` on this port.\n", IR_PIN,
                (unsigned)bound, (unsigned)ir::kSlotCount, s_rxOn ? "listening" : "off");
#else
  Serial.printf("[ir] no receiver in this build, %u/%u slots learned. Type `ir help` on this port.\n",
                (unsigned)bound, (unsigned)ir::kSlotCount);
#endif
}

void irLoop() {
  if (!s_begun) return;
  consolePoll();

#if defined(IR_RX_ENABLED)
  if (!s_rxOn) return;
  decode_results r;
  // A pass can bring several frames; drain them all, or a held button walks
  // further behind with every loop().
  while (s_recv.decode(&r)) {
    if (r.overflow) {
      s_overflows++;
    } else {
      ir::Frame f;
      f.proto = (uint8_t)r.decode_type;
      f.value = r.value;
      f.repeat = r.repeat;
      f.unknown = (r.decode_type == decode_type_t::UNKNOWN);
      apply(millis(), &f, -1, 0);
    }
    s_recv.resume();
  }
#endif
}

// ── the seam (the encoder's sampling task) ──────────────────────────────────
int8_t irTakeRotate() {
  int8_t v;
  portENTER_CRITICAL(&s_mux);
  v = s_dec.takeRotate();
  portEXIT_CRITICAL(&s_mux);
  return v;
}

bool irOkDown(uint32_t nowMs) {
  bool v;
  portENTER_CRITICAL(&s_mux);
  v = s_dec.okDown(nowMs);
  portEXIT_CRITICAL(&s_mux);
  return v;
}

// ── the learn service and the simulator ─────────────────────────────────────
bool irLearnArm(uint8_t slot) {
  if (slot >= ir::kSlotCount) return false;
  bool ok;
  portENTER_CRITICAL(&s_mux);
  ok = s_dec.learnArm(slot, millis());
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void irLearnCancel() {
  portENTER_CRITICAL(&s_mux);
  s_dec.learnCancel();
  portEXIT_CRITICAL(&s_mux);
}

int irLearnActiveSlot() {
  int s;
  portENTER_CRITICAL(&s_mux);
  s = s_dec.learnActive(millis());
  portEXIT_CRITICAL(&s_mux);
  return s;
}

uint32_t irLearnRemainMs() {
  uint32_t v;
  portENTER_CRITICAL(&s_mux);
  v = s_dec.learnRemainMs(millis());
  portEXIT_CRITICAL(&s_mux);
  return v;
}

bool irClearSlot(uint8_t slot) {
  if (slot >= ir::kSlotCount) return false;
  portENTER_CRITICAL(&s_mux);
  s_dec.map().clear(slot);
  portEXIT_CRITICAL(&s_mux);
  mapClearSlot(slot);
  return true;
}

void irClearAll() {
  portENTER_CRITICAL(&s_mux);
  s_dec.map().reset();
  portEXIT_CRITICAL(&s_mux);
  Preferences p;
  if (p.begin(kNs, false)) { p.clear(); p.end(); }
}

bool irSimulate(uint8_t slot, uint32_t holdMs) {
  if (slot >= ir::kSlotCount) return false;
  apply(millis(), nullptr, (int16_t)slot, holdMs);
  return true;
}

void irSettingsChanged() {
#if defined(IR_RX_ENABLED)
  if (settings.irEnabled && !s_rxOn) {
    s_recv.setUnknownThreshold(12);
    s_recv.enableIRIn();
    s_rxOn = true;
    Serial.println(F("[ir] receiver on"));
  } else if (!settings.irEnabled && s_rxOn) {
    s_recv.disableIRIn();   // gives the timer and the ISR back
    s_rxOn = false;
    Serial.println(F("[ir] receiver off"));
  }
#endif
}

void irInfoJson(JsonObject out) {
  const uint32_t now = millis();
  out["pin"] = IR_PIN;
#if defined(IR_RX_ENABLED)
  out["receiver"] = s_rxOn ? "listening" : "off";
  out["overflows"] = s_overflows;
#else
  out["receiver"] = "not built";
#endif
  out["enabled"] = settings.irEnabled;

  uint8_t  bound;
  uint32_t frames, ignored;
  int      learn, hit;
  uint32_t learnLeft;
  portENTER_CRITICAL(&s_mux);
  bound = s_dec.map().boundCount();
  frames = s_dec.frames();
  ignored = s_dec.ignored();
  learn = s_dec.learnActive(now);
  learnLeft = s_dec.learnRemainMs(now);
  hit = s_dec.lastHitSlot(now, 5000);
  portEXIT_CRITICAL(&s_mux);

  out["bound"] = bound;
  out["slots"] = (uint8_t)ir::kSlotCount;
  out["frames"] = frames;
  out["ignored"] = ignored;
  if (learn >= 0) {
    out["learning"] = ir::slotName((uint8_t)learn);
    out["learnMs"] = learnLeft;
  }
  if (hit >= 0) out["lastSlot"] = ir::slotName((uint8_t)hit);

  uint8_t  sProto = 0;
  uint64_t sValue = 0;
  bool     sRepeat = false, seen;
  uint32_t sAge = 0;
  portENTER_CRITICAL(&s_mux);
  seen = s_dec.lastSeen(&sProto, &sValue, &sRepeat, &sAge, now);
  portEXIT_CRITICAL(&s_mux);
  if (seen) {
    char pn[16], hex[20];
    protoName(sProto, pn, sizeof(pn));
    snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)sValue);
    out["lastProto"] = pn;      // copied into the document: both are stack buffers
    out["lastCode"] = hex;
    out["lastRepeat"] = sRepeat;
    out["lastAgeMs"] = sAge;
  }

  JsonArray arr = out["map"].to<JsonArray>();
  for (uint8_t i = 0; i < ir::kSlotCount; i++) {
    bool b;
    uint64_t v;
    uint8_t p;
    uint32_t h;
    portENTER_CRITICAL(&s_mux);
    b = s_dec.map().bound(i);
    v = s_dec.map().value(i);
    p = s_dec.map().proto(i);
    h = s_dec.hits(i);
    portEXIT_CRITICAL(&s_mux);
    JsonObject o = arr.add<JsonObject>();
    o["name"] = ir::slotName(i);
    o["hint"] = ir::slotHint(i);
    o["knob"] = ir::slotDrivesKnob(i);
    o["bound"] = b;
    o["hits"] = h;
    if (b) {
      char pn[16], hex[20];
      protoName(p, pn, sizeof(pn));
      snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)v);
      o["proto"] = pn;
      o["code"] = hex;
    }
  }
}

#endif  // IR_ENABLED

#pragma once
// The serial console's grammar: one line in, one command out.
//
// The panel has no command line - nothing in loop() has ever read Serial (the
// Improv window in src/network closes before loop() begins). This module brings
// its own, because the remote has to be testable before a receiver is soldered:
// typing `ir ok` into the serial monitor must move the display exactly as the
// knob's button does.
//
// Plain C++ so tools/ir/check_ir.py can grind the grammar on the host: the
// parser is where a test costs nothing and a mistake costs a reflash.
//
//   ir                      what the module knows: pin, receiver, learned slots
//   ir help                 the lines below
//   ir cw [n]               n detents clockwise (default 1, at most kRotAccMax)
//   ir ccw [n]              n detents anticlockwise
//   ir ok [ms]              the button: a click, or held for ms (1200 = long press)
//   ir sim <slot> [ms]      any slot by name or number, same path as a real frame
//   ir learn <slot>         open the learn window and press the button on the remote
//   ir cancel               close it
//   ir clear <slot>         forget one code
//   ir clear all            forget every code
//
// A slot is a name from ir_map.h (case-insensitive: ok, OK, Ok) or its number.

#include <stdint.h>

#include "ir_map.h"

namespace ir {

struct Command {
  enum Kind : uint8_t {
    kNone = 0,   // not addressed to us - the line starts with something else
    kHelp,
    kStatus,
    kSim,        // slot + arg(hold ms) - `cw`, `ccw` and `ok` arrive as this too
    kRepeat,     // slot + arg(count): `ir cw 3`
    kLearn,      // slot
    kCancel,
    kClear,      // slot
    kClearAll,
    kError,      // ours, but wrong: `error` says how
  } kind;
  uint8_t     slot;
  uint32_t    arg;
  const char *error;   // a literal, only when kind == kError
};

namespace detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// The next word, lowercased into `out`. Returns the length; advances `p`.
inline uint8_t word(const char *&p, char *out, uint8_t cap) {
  while (*p && isSpace(*p)) p++;
  uint8_t n = 0;
  while (*p && !isSpace(*p)) {
    if (n + 1 < cap) out[n++] = lower(*p);
    p++;   // a word longer than the buffer is truncated, not overrun
  }
  out[n] = 0;
  return n;
}

inline bool sameLower(const char *a, const char *bUpper) {
  for (;; a++, bUpper++) {
    if (*a != lower(*bUpper)) return false;
    if (!*a) return true;
  }
}

// A decimal number, with no sign and no overflow: anything that does not fit is
// clamped rather than wrapped, so `ir ok 99999999999` is a long hold and not a
// short one.
inline bool number(const char *w, uint32_t *out) {
  if (!*w) return false;
  uint32_t v = 0;
  for (const char *p = w; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    if (v > 42949672u) { v = 0xFFFFFFFFu; break; }
    v = v * 10 + (uint32_t)(*p - '0');
  }
  *out = v;
  return true;
}

// A slot by name ("ok", "bright_up") or by number ("2").
inline bool slot(const char *w, uint8_t *out) {
  uint32_t n = 0;
  if (number(w, &n)) {
    if (n >= kSlotCount) return false;
    *out = (uint8_t)n;
    return true;
  }
  return slotByName(w, out);
}

}  // namespace detail

// Parses one line. Returns false when the line is not addressed to this module
// (so anything else on the serial port is left alone); true with kind == kError
// when it is ours and wrong.
inline bool parseCommand(const char *line, Command *out) {
  if (!line || !out) return false;
  Command c{Command::kNone, 0, 0, nullptr};
  const char *p = line;
  char w[24];

  if (!detail::word(p, w, sizeof(w))) return false;
  if (!detail::sameLower(w, "IR")) return false;

  const uint8_t n = detail::word(p, w, sizeof(w));
  if (!n || detail::sameLower(w, "STATUS")) { c.kind = Command::kStatus; *out = c; return true; }
  if (detail::sameLower(w, "HELP") || w[0] == '?') { c.kind = Command::kHelp; *out = c; return true; }
  if (detail::sameLower(w, "CANCEL")) { c.kind = Command::kCancel; *out = c; return true; }

  // `ir cw [n]` / `ir ccw [n]`: the shorthand, because a detent count is what
  // one actually wants to type when walking the pages.
  const bool cw = detail::sameLower(w, "CW"), ccw = detail::sameLower(w, "CCW");
  if (cw || ccw) {
    c.kind = Command::kRepeat;
    c.slot = cw ? kCw : kCcw;
    c.arg = 1;
    char a[24];
    if (detail::word(p, a, sizeof(a))) {
      if (!detail::number(a, &c.arg) || c.arg == 0) { c.kind = Command::kError; c.error = "count must be 1 or more"; }
      else if (c.arg > 64) c.arg = 64;   // a typo should not spin the display for a minute
    }
    *out = c;
    return true;
  }

  if (detail::sameLower(w, "OK")) {
    c.kind = Command::kSim;
    c.slot = kOk;
    char a[24];
    if (detail::word(p, a, sizeof(a)) && !detail::number(a, &c.arg)) {
      c.kind = Command::kError; c.error = "hold time must be a number of milliseconds";
    }
    *out = c;
    return true;
  }

  if (detail::sameLower(w, "SIM") || detail::sameLower(w, "LEARN") || detail::sameLower(w, "CLEAR")) {
    const bool sim = detail::sameLower(w, "SIM"), learn = detail::sameLower(w, "LEARN");
    char a[24];
    if (!detail::word(p, a, sizeof(a))) {
      c.kind = Command::kError;
      c.error = sim ? "which slot? try `ir sim ok`" : (learn ? "which slot? try `ir learn ok`"
                                                             : "which slot? or `ir clear all`");
      *out = c;
      return true;
    }
    if (!sim && !learn && detail::sameLower(a, "ALL")) { c.kind = Command::kClearAll; *out = c; return true; }
    if (!detail::slot(a, &c.slot)) {
      c.kind = Command::kError;
      c.error = "no such slot - `ir help` lists them";
      *out = c;
      return true;
    }
    c.kind = sim ? Command::kSim : (learn ? Command::kLearn : Command::kClear);
    if (sim) {
      char b[24];
      if (detail::word(p, b, sizeof(b)) && !detail::number(b, &c.arg)) {
        c.kind = Command::kError; c.error = "hold time must be a number of milliseconds";
      }
    }
    *out = c;
    return true;
  }

  c.kind = Command::kError;
  c.error = "unknown word - `ir help` lists the commands";
  *out = c;
  return true;
}

}  // namespace ir

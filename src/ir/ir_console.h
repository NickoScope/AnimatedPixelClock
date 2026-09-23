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
    kPress,      // slot (a button, 0-based) + arg(hold ms): `ir press 3 [ms]`
    kDo,         // fn + arg (page, or hold ms for ok): `ir do bright_up`, `ir ok 1200`
    kRepeatFn,   // fn + arg(count): `ir cw 3`
    kLearn,      // slot
    kCancel,
    kClear,      // slot
    kClearAll,
    kSetFn,      // slot + fn + arg(page): `ir fn 5 page 20`
    kError,      // ours, but wrong: `error` says how
  } kind;
  uint8_t     slot;
  uint32_t    arg;
  const char *error;   // a literal, only when kind == kError
  uint8_t     fn;      // zero (kFnNone) when left out of a brace initializer
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
}  // namespace detail

inline bool parseCommand(const char *line, Command *out) {
  if (!line || !out) return false;
  Command c{Command::kNone, 0, 0, nullptr, kFnNone};
  const char *p = line;
  char w[24];
  auto fail = [&](const char *why) { c.kind = Command::kError; c.error = why; *out = c; return true; };

  if (!detail::word(p, w, sizeof(w))) return false;
  if (!detail::sameLower(w, "IR")) return false;

  const uint8_t n = detail::word(p, w, sizeof(w));
  if (!n || detail::sameLower(w, "STATUS")) { c.kind = Command::kStatus; *out = c; return true; }
  if (detail::sameLower(w, "HELP") || w[0] == '?') { c.kind = Command::kHelp; *out = c; return true; }
  if (detail::sameLower(w, "CANCEL")) { c.kind = Command::kCancel; *out = c; return true; }

  // The knob's three, by their old names: `ir cw 3`, `ir ccw`, `ir ok 1200`.
  const bool cw = detail::sameLower(w, "CW"), ccw = detail::sameLower(w, "CCW");
  if (cw || ccw) {
    c.kind = Command::kRepeatFn;
    c.fn = cw ? kFnCw : kFnCcw;
    c.arg = 1;
    char a[24];
    if (detail::word(p, a, sizeof(a))) {
      if (!detail::number(a, &c.arg) || c.arg == 0) return fail("count must be 1 or more");
      if (c.arg > 64) c.arg = 64;   // a typo should not spin the display for a minute
    }
    *out = c;
    return true;
  }
  if (detail::sameLower(w, "OK")) {
    c.kind = Command::kDo;
    c.fn = kFnOk;
    char a[24];
    if (detail::word(p, a, sizeof(a)) && !detail::number(a, &c.arg))
      return fail("hold time must be a number of milliseconds");
    *out = c;
    return true;
  }

  // `ir do <function> [page | hold ms]` - a function, no button needed.
  if (detail::sameLower(w, "DO")) {
    char a[24];
    if (!detail::word(p, a, sizeof(a))) return fail("which function? `ir help` lists them");
    if (!fnByName(a, &c.fn)) return fail("no such function - `ir help` lists them");
    c.kind = Command::kDo;
    char b[24];
    if (detail::word(p, b, sizeof(b)) && !detail::number(b, &c.arg))
      return fail(c.fn == kFnPage ? "the page must be a number" : "hold time must be a number of milliseconds");
    if (c.fn == kFnPage && c.arg > 255) return fail("the page must be 0..255");
    *out = c;
    return true;
  }

  // `ir fn <button> <function> [page]`
  if (detail::sameLower(w, "FN")) {
    char a[24], b[24];
    if (!detail::word(p, a, sizeof(a))) return fail("which button? 1..10");
    if (!slotByNumber(a, &c.slot)) return fail("buttons are 1..10");
    if (!detail::word(p, b, sizeof(b))) return fail("which function? `ir help` lists them");
    if (!fnByName(b, &c.fn)) return fail("no such function - `ir help` lists them");
    c.kind = Command::kSetFn;
    char e[24];
    if (c.fn == kFnPage) {
      if (!detail::word(p, e, sizeof(e)) || !detail::number(e, &c.arg) || c.arg > 255)
        return fail("which page? a number, as /api/panel lists them");
    }
    *out = c;
    return true;
  }

  // `ir press|sim <button> [ms]`, `ir learn <button>`, `ir clear <button>|all`
  const bool press = detail::sameLower(w, "PRESS") || detail::sameLower(w, "SIM");
  const bool learn = detail::sameLower(w, "LEARN");
  if (press || learn || detail::sameLower(w, "CLEAR")) {
    char a[24];
    if (!detail::word(p, a, sizeof(a)))
      return fail(press ? "which button? try `ir press 3`" : (learn ? "which button? try `ir learn 3`"
                                                                     : "which button? or `ir clear all`"));
    if (!press && !learn && detail::sameLower(a, "ALL")) { c.kind = Command::kClearAll; *out = c; return true; }
    if (!slotByNumber(a, &c.slot)) return fail("buttons are 1..10");
    c.kind = press ? Command::kPress : (learn ? Command::kLearn : Command::kClear);
    if (press) {
      char b[24];
      if (detail::word(p, b, sizeof(b)) && !detail::number(b, &c.arg))
        return fail("hold time must be a number of milliseconds");
    }
    *out = c;
    return true;
  }

  return fail("unknown word - `ir help` lists the commands");
}

}  // namespace ir

#!/usr/bin/env python3
"""src/fonts/utf8_next.h against Python's own UTF-8 decoder, on the host.

Python's decode(errors="replace") substitutes U+FFFD per maximal subpart, the
practice the Unicode Standard recommends, so the two must agree on every input:
the edge cases below, then random byte strings weighted towards the bytes that
matter (leads, continuations, the forbidden ones).

    python3 tools/fonts/check_utf8.py      # exit 1 on any difference
"""
import os, pathlib, random, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
C = r'''
#include "fonts/utf8_next.h"
#include <stdio.h>
#include <string.h>
int main(void) {
  static unsigned char buf[4096];
  char line[9000];
  while (fgets(line, sizeof line, stdin)) {
    size_t n = 0;
    for (char *h = line; h[0] && h[1] && h[0] != '\n'; h += 2) {
      unsigned v; sscanf(h, "%2x", &v); buf[n++] = (unsigned char)v;
    }
    buf[n] = 0;
    const unsigned char *p = buf;
    while (*p) printf("%lx ", (unsigned long)utf8Next(&p));
    printf("\n");
  }
  return 0;
}
'''

EDGES = [
    b"", b"A", b"hello", "Привет".encode(), "ёЁ".encode(), "€".encode(), "😀".encode(),
    b"\x80", b"\xbf", b"\xc0\x80", b"\xc1\xbf", b"\xc2", b"\xc2\x7f", b"\xc2\x80", b"\xdf\xbf",
    b"\xe0\x80\x80", b"\xe0\x9f\xbf", b"\xe0\xa0\x80", b"\xe0\xa0", b"\xed\x9f\xbf", b"\xed\xa0\x80",
    b"\xef\xbf\xbf", b"\xf0\x8f\xbf\xbf", b"\xf0\x90\x80\x80", b"\xf4\x8f\xbf\xbf", b"\xf4\x90\x80\x80",
    b"\xf5\x80\x80\x80", b"\xff", b"\xfe\xff", b"\xf0\x90\x80", b"\xf0\x90", b"\xe1\x80", b"a\xe1\x80b",
    b"\xd0\xd0\x9f", "WiFi: Кухня".encode(),
]


def expect(bs):
    return [ord(ch) for ch in bs.decode("utf-8", errors="replace")]


def main():
    rnd = random.Random(20260923)
    pool = list(range(0x01, 0x80, 17)) + list(range(0x80, 0xC0, 3)) + [0xC0, 0xC1, 0xC2, 0xD0, 0xD1, 0xDF,
            0xE0, 0xE1, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF4, 0xF5, 0xFF]
    cases = list(EDGES)
    for _ in range(200000):
        cases.append(bytes(rnd.choice(pool) for _ in range(rnd.randint(1, 8))))
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d, "t.c"); src.write_text(C)
        exe = pathlib.Path(d, "t")
        subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "src"),
                        str(src), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], input="\n".join(c.hex() for c in cases) + "\n",
                             capture_output=True, text=True, check=True).stdout.splitlines()
    bad = 0
    for c, line in zip(cases, out):
        got = [int(x, 16) for x in line.split()]
        want = expect(c)
        if got != want:
            bad += 1
            if bad <= 10:
                print(f"FAIL {c.hex()}: got {[hex(v) for v in got]} want {[hex(v) for v in want]}")
    if len(out) != len(cases):
        print(f"FAIL: {len(out)} results for {len(cases)} cases"); bad += 1
    print(f"utf8_next: {len(cases)} inputs, {bad} differences from Python")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

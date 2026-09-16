#!/usr/bin/env python3
"""Host test of the Music Assistant player's protocol and its rules.

src/maplayer/snap_proto.h and src/maplayer/maplayer_model.h are plain C++ - no
Arduino, no sockets, no I2S - so they are compiled here with the host compiler
and held to the Snapcast binary protocol as its own specification defines it
(badaix/snapcast, doc/binary_protocol.md, read in full): the protocol is little
endian, the base message is 26 bytes, a client joins with Hello then receives
Server Settings and a Codec Header before any Wire Chunk may be played, and for
codec "pcm" the codec header is a RIFF WAVE carrying the sample rate, bit depth
and channel count.

That specification is the whole reason this file exists. No stream has ever run
on the panel, so the parser cannot be checked against hardware; it is checked
against the document instead. The cases that matter are the ones a real stream
produces: a wire chunk truncated across two socket reads, a WAVE header whose
fmt chunk is not first, and a JSON key that merely starts with the key being
looked for.

The module's own rules are tested here too: the 48 kHz the shared bit clock
forces on us (docs/25 §4), the stereo-to-mono downmix the ES8311 needs because
it takes the left channel only, and the heap gate that must refuse to start a
stream on the heap this panel actually has (docs/22 §12.1).

  python3 tools/maplayer/check_maplayer.py
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    # Both standards, and not for tidiness. The firmware builds as gnu++11 and
    # this test used to build only at C++17, where a class with default member
    # initialisers is an aggregate again - so an Appender that brace-initialised
    # fine here failed to compile in the firmware, after the test had reported
    # 136 checks passed. A host test that passes while the firmware will not
    # build is worse than no host test, so both are built here.
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for std in ("c++11", "c++17"):
            exe = pathlib.Path(tmp) / f"maplayer_host_test_{std.replace('+', 'x')}"
            subprocess.run(["c++", f"-std={std}", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "src/maplayer"),
                            str(HERE / "maplayer_host_test.cpp"), "-o", str(exe)], check=True)
            print(f"--- {std}")
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            failed += r.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

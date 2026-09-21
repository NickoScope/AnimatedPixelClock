#!/usr/bin/env python3
"""What the published flasher images were built from, and whether that is still true.

The web flasher at docs/firmware/latest serves real binaries to real boards.
On 2026-09-21 it came to serve one labelled v2.5.0 while the panel ran a
DIFFERENT binary also labelled v2.5.0 - twelve slots and an upload fix had
landed in between. Two binaries under one version number is the defect; a
published image merely being older than main is not, as long as it says so.

So `release.py` stamps `docs/firmware/latest/SOURCE.sha` with the version it
packaged and a hash of everything that goes into a firmware binary, and this
compares that stamp against the tree:

    python3 tools/firmware_stamp.py --write     # release.py calls this
    python3 tools/firmware_stamp.py --check     # the pre-commit hook calls this

`--check` FAILS on exactly one thing: the sources have changed while
FIRMWARE_VERSION has not, so a rebuild would produce different bytes under the
same name. It only WARNS when the published images are simply behind, because
that is normal between a change and a release, and a hook that refuses every
commit until you have run a three-board build is a hook people disable.

The hash covers src/, platformio.ini and the partition tables named in it -
everything that changes the image without changing its version. It deliberately
does NOT cover the library versions, which are pinned in platformio.ini and
therefore already in it.
"""

import argparse
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAMP = ROOT / "docs" / "firmware" / "latest" / "SOURCE.sha"
CONFIG_H = ROOT / "src" / "config" / "config.h"


def version():
    m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', CONFIG_H.read_text(encoding="utf-8"))
    if not m:
        sys.exit("firmware_stamp: FIRMWARE_VERSION not found in src/config/config.h")
    return "v" + m.group(1).strip().removeprefix("v")


def inputs():
    """Every file whose content can change a firmware binary, sorted."""
    files = sorted(p for p in (ROOT / "src").rglob("*")
                   if p.is_file() and p.suffix in
                   {".c", ".cpp", ".h", ".hpp", ".inc", ".S", ".s"})
    files.append(ROOT / "platformio.ini")
    # The partition tables named in platformio.ini live with the platform, not
    # here, when they are stock; a project-local one is what matters.
    for name in sorted(set(re.findall(r"board_build\.partitions\s*=\s*(\S+)",
                                      (ROOT / "platformio.ini").read_text(encoding="utf-8")))):
        for cand in (ROOT / name, ROOT / "partitions" / name):
            if cand.is_file():
                files.append(cand)
    return [f for f in files if f.is_file()]


def digest():
    h = hashlib.sha256()
    for f in inputs():
        h.update(str(f.relative_to(ROOT)).encode())
        h.update(b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def read_stamp():
    if not STAMP.exists():
        return None, None
    parts = STAMP.read_text(encoding="utf-8").split()
    return (parts[0], parts[1]) if len(parts) >= 2 else (None, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="stamp the published images with this tree (release.py)")
    ap.add_argument("--check", action="store_true",
                    help="fail if a rebuild would change the bytes without changing the version")
    a = ap.parse_args()

    now, ver = digest(), version()

    if a.write:
        STAMP.parent.mkdir(parents=True, exist_ok=True)
        STAMP.write_text(f"{ver} {now}\n", encoding="utf-8", newline="\n")
        print(f"  stamped {STAMP.relative_to(ROOT)}: {ver} {now[:16]}...")
        return 0

    if not a.check:
        print(f"{ver} {now}")
        return 0

    was_ver, was_hash = read_stamp()
    if was_hash is None:
        print("firmware_stamp: docs/firmware/latest carries no SOURCE.sha yet.")
        print("                run  python3 release.py  once to stamp it.")
        return 0                      # nothing to compare against; do not block

    if now == was_hash:
        return 0                      # the page serves exactly this tree

    if ver == was_ver:
        print()
        print("pre-commit: the firmware sources have changed but FIRMWARE_VERSION has not.")
        print(f"            docs/firmware/latest serves {was_ver}, and a rebuild of this tree")
        print(f"            would produce DIFFERENT bytes under that same name - which is how")
        print("            a person ends up comparing versions and being told they match when")
        print("            they do not. It happened on 2026-09-21.")
        print()
        print(f"            Bump FIRMWARE_VERSION in src/config/config.h (it is {ver} now),")
        print("            then `python3 release.py` when you are ready to publish.")
        print()
        return 1

    print(f"  note: the flasher still serves {was_ver}; this tree is {ver}.")
    print("        `python3 release.py` republishes all three boards when you want it to.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

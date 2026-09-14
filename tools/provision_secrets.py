#!/usr/bin/env python3
"""Fill provision_secrets.ini from the NickoScope32 flagship's own secret headers.

Meant to be run by the owner. It reads two local header files and writes one
gitignored file; nothing goes over the network. It prints which values it found
and how long they are - never the values themselves.

  python3 tools/provision_secrets.py import "<a NickoScope32_ESP32S3_v... folder>"
  python3 tools/provision_secrets.py check
"""
import os
import pathlib
import re
import stat
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = pathlib.Path(os.environ.get("PROVISION_SECRETS_OUT", ROOT / "provision_secrets.ini"))

# (build flag, header inside the flagship, #define there, kind)
SOURCES = [
    ("PROV_MQTT_HOST", "src/net/ha_mqtt_secrets.h", "MQTT_HOST", "str"),
    ("PROV_MQTT_PORT", "src/net/ha_mqtt_secrets.h", "MQTT_PORT", "int"),
    ("PROV_MQTT_USER", "src/net/ha_mqtt_secrets.h", "MQTT_USER", "str"),
    ("PROV_MQTT_PASS", "src/net/ha_mqtt_secrets.h", "MQTT_PASS", "str"),
    ("PROV_AIS_KEY",   "src/secrets_yacht_radar.h", "AISSTREAM_API_KEY", "str"),
]
REQUIRED = {"PROV_MQTT_HOST", "PROV_MQTT_PORT", "PROV_MQTT_USER", "PROV_MQTT_PASS"}
# Template placeholders from provision_secrets.example.ini: set, but not real.
PLACEHOLDERS = {"192.168.x.x", "mqtt-user", "mqtt-password", "aisstream-key"}

# What would break a -D flag in platformio.ini: whitespace splits the flag, a
# quote or backslash breaks the escaping, ; or # after a space starts an ini
# comment, and $ starts interpolation. Such a value is refused, not mangled.
UNSAFE = re.compile(r"[\s\"\;#$']")


def read_define(path, name, kind):
    text = path.read_text(errors="replace")
    if kind == "int":
        m = re.search(r"^\s*#define\s+%s\s+(\d+)\b" % name, text, re.M)
    else:
        m = re.search(r'^\s*#define\s+%s\s+"([^"]*)"' % name, text, re.M)
    return m.group(1) if m else None


def do_import(flagship):
    base = pathlib.Path(flagship).expanduser()
    lines, problems = [], 0
    for flag, rel, name, kind in SOURCES:
        f = base / rel
        if not f.is_file():
            print(f"  {flag:15s} missing file {rel}")
            problems += 1
            continue
        v = read_define(f, name, kind)
        if not v:
            print(f"  {flag:15s} {name} not found in {rel}")
            problems += 1
            continue
        if kind == "str" and UNSAFE.search(v):
            print(f"  {flag:15s} has a space, quote, backslash, ; # or $ - put this one in by hand")
            problems += 1
            continue
        lines.append(f"\t-D{flag}={v}" if kind == "int" else f'\t-D{flag}=\\"{v}\\"')
        print(f"  {flag:15s} found, {len(v)} chars")
    if problems:
        print(f"\n{problems} problem(s) - nothing written.")
        sys.exit(1)
    OUT.write_text("; Written by tools/provision_secrets.py import. Gitignored: never commit.\n"
                   "[env:provision]\nbuild_flags =\n"
                   "\t${env:matrix-waveshare-rgb.build_flags}\n" + "\n".join(lines) + "\n")
    os.chmod(OUT, stat.S_IRUSR | stat.S_IWUSR)
    print(f"\nwrote {OUT.name}, readable by you only\n")
    check()


def check():
    if not OUT.is_file():
        print(f"{OUT.name}: missing")
        sys.exit(1)
    text = OUT.read_text()
    good = True
    for flag, _, _, _ in SOURCES:
        m = re.search(r'^\s*-D%s=(?:\\"(.*)\\"|(\d+))\s*$' % flag, text, re.M)
        if not m:
            print(f"  {flag:15s} not set{'' if flag not in REQUIRED else '  <- needed'}")
            good = good and flag not in REQUIRED
            continue
        v = m.group(1) if m.group(1) is not None else m.group(2)
        if v in PLACEHOLDERS:
            print(f"  {flag:15s} still the template placeholder")
            good = good and flag not in REQUIRED
            continue
        print(f"  {flag:15s} set, {len(v)} chars")
    ignored = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", str(OUT)],
                             capture_output=True).returncode == 0
    print(f"  git ignores it: {'yes' if ignored else 'NO - do not commit it'}; "
          f"permissions {oct(OUT.stat().st_mode & 0o777)}")
    print("\nready for: pio run -e provision -t upload" if good else "\nnot ready")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "import":
        do_import(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "check":
        check()
    else:
        print(__doc__)
        sys.exit(2)

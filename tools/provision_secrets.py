#!/usr/bin/env python3
"""Fill provision_secrets.ini, the gitignored build flags env:provision writes into NVS.

Meant to be run by the owner. It prints which values it found and how long
they are - never the values themselves.

  python3 tools/provision_secrets.py import "<a NickoScope32_ESP32S3_v... folder>"
  python3 tools/provision_secrets.py rtt-from-ha [--host nickohome] [--kind refresh|access|auto]
  python3 tools/provision_secrets.py rtt-prompt  [--kind refresh|access|auto]
  python3 tools/provision_secrets.py aeroapi-from-ha [--host nickohome]
  python3 tools/provision_secrets.py aeroapi-prompt
  python3 tools/provision_secrets.py check

import       MQTT broker and AIS key from the flagship's secret headers (local files)
rtt-from-ha  the Realtime Trains token from Home Assistant's secrets.yaml, over ssh:
             ssh <host> 'sudo grep ^rtt_bearer: /config/secrets.yaml'
             The line's value is read from ssh's output inside this process; "Bearer "
             and the quotes are removed. ssh and sudo prompts still reach the terminal.
rtt-prompt   the same token, typed or pasted at a prompt that does not echo
aeroapi-from-ha  the FlightAware AeroAPI key from Home Assistant's secrets.yaml, over ssh:
             ssh <host> 'sudo grep ^aeroapi_key: /config/secrets.yaml'
             (the name Home Assistant's REST sensors use: x-apikey: !secret aeroapi_key)
aeroapi-prompt   the same key, typed or pasted at a prompt that does not echo
check        what the file holds, as set / length only

Each command changes only its own lines and keeps the rest of the file. The
file is written 0600 through a temporary file, so a failed write leaves the old
one intact. --kind refresh is right for a token from api-portal.rtt.io that
must be exchanged at /api/get_access_token; auto lets the firmware find out.
"""
import argparse
import getpass
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
RTT_TOKEN, RTT_KIND = "PROV_RTT_TOKEN", "PROV_RTT_KIND"
AERO_KEY = "PROV_AEROAPI_KEY"
# Template placeholders from provision_secrets.example.ini: set, but not real.
PLACEHOLDERS = {"192.168.x.x", "mqtt-user", "mqtt-password", "aisstream-key", "rtt-token", "aeroapi-key"}

# What would break a -D flag in platformio.ini: whitespace splits the flag, a
# quote or backslash breaks the escaping, ; or # after a space starts an ini
# comment, and $ starts interpolation. Such a value is refused, not mangled.
UNSAFE = re.compile(r"[\s\"\\;#$']")

HEADER = "; Written by tools/provision_secrets.py. Gitignored: never commit."


def read_define(path, name, kind):
    text = path.read_text(errors="replace")
    if kind == "int":
        m = re.search(r"^\s*#define\s+%s\s+(\d+)\b" % name, text, re.M)
    else:
        m = re.search(r'^\s*#define\s+%s\s+"([^"]*)"' % name, text, re.M)
    return m.group(1) if m else None


def str_flag(flag, value):
    return f'\t-D{flag}=\\"{value}\\"'


def write_defines(updates):
    """Set or remove -D lines in [env:provision] build_flags; keep everything else.

    `updates` maps a flag to its whole line, or to None to remove the flag.
    """
    lines = OUT.read_text().splitlines() if OUT.is_file() else [HEADER]
    flag_re = re.compile(r"^\s*-D(%s)=" % "|".join(map(re.escape, updates)))
    lines = [l for l in lines if not flag_re.match(l)]

    if "[env:provision]" not in (l.strip() for l in lines):
        lines += ["[env:provision]", "build_flags =", "\t${env:matrix-waveshare-rgb.build_flags}"]
    sec = next(i for i, l in enumerate(lines) if l.strip() == "[env:provision]")
    nxt = next((i for i in range(sec + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
    bf = next((i for i in range(sec + 1, nxt) if re.match(r"\s*build_flags\s*=", lines[i])), None)
    if bf is None:
        lines[sec + 1:sec + 1] = ["build_flags =", "\t${env:matrix-waveshare-rgb.build_flags}"]
        bf = sec + 1
    end = bf + 1
    while end < len(lines) and lines[end][:1] in ("\t", " ") and lines[end].strip():
        end += 1
    lines[end:end] = [line for line in updates.values() if line]

    # Created 0600 from the first byte, then moved over the old file in one step.
    tmp = OUT.with_name(OUT.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
    os.replace(tmp, OUT)


def do_import(flagship):
    base = pathlib.Path(flagship).expanduser()
    updates, problems = {}, 0
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
        updates[flag] = f"\t-D{flag}={v}" if kind == "int" else str_flag(flag, v)
        print(f"  {flag:15s} found, {len(v)} chars")
    if problems:
        print(f"\n{problems} problem(s) - nothing written.")
        sys.exit(1)
    write_defines(updates)
    print(f"\nwrote {OUT.name}, readable by you only\n")
    check()


def yaml_scalar(raw):
    """The value of a one-line YAML scalar: quoted, or plain with an optional # comment."""
    v = raw.strip()
    if v[:1] in ("'", '"'):
        end = v.find(v[0], 1)
        if end < 0:
            raise ValueError("the quoted value has no closing quote")
        return v[1:end]
    return re.split(r"\s+#", v, maxsplit=1)[0].strip()


def bare_token(value):
    v = value.strip()
    if v.startswith("Bearer "):
        v = v[len("Bearer "):].strip()
    return v


def store_token(token, kind, source):
    if not token:
        print(f"  {RTT_TOKEN:15s} empty in {source} - nothing written")
        sys.exit(1)
    if UNSAFE.search(token):
        print(f"  {RTT_TOKEN:15s} has a space, quote, backslash, ; # or $ - nothing written")
        sys.exit(1)
    if not 16 <= len(token) <= 2000:
        print(f"  {RTT_TOKEN:15s} is {len(token)} chars, outside 16-2000 - nothing written")
        sys.exit(1)
    updates = {RTT_TOKEN: str_flag(RTT_TOKEN, token),
               RTT_KIND: str_flag(RTT_KIND, kind) if kind in ("refresh", "access") else None}
    print(f"  {RTT_TOKEN:15s} found, {len(token)} chars")
    print(f"  {RTT_KIND:15s} {kind if kind in ('refresh', 'access') else 'auto (not written)'}")
    write_defines(updates)
    print(f"\nwrote {OUT.name}, readable by you only\n")
    check()


def secret_from_ha(host, name):
    """One `name:` line of Home Assistant's secrets.yaml, read over ssh; its value is never shown."""
    cmd = ["ssh", host, f"sudo grep ^{name}: /config/secrets.yaml"]
    print(f"asking {host} for the {name} line; its value is not shown")
    # stdout is captured and never printed; stderr goes to the terminal, so an
    # ssh or sudo prompt or error is visible. grep writes matches to stdout only.
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=None)
    out = r.stdout.decode("utf-8", "replace")
    if r.returncode == 1 and not out:
        print(f"  no {name}: line in /config/secrets.yaml")
        sys.exit(1)
    if r.returncode != 0:
        print(f"  ssh or sudo failed, exit code {r.returncode}")
        sys.exit(1)
    found = [l for l in out.splitlines() if l.startswith(f"{name}:")]
    if len(found) != 1:
        print(f"  expected one {name}: line, found {len(found)} - nothing written")
        sys.exit(1)
    try:
        return yaml_scalar(found[0].split(":", 1)[1])
    except ValueError as e:
        print(f"  {e} - nothing written")
        sys.exit(1)


def rtt_from_ha(host, kind):
    store_token(bare_token(secret_from_ha(host, "rtt_bearer")), kind, "Home Assistant's secrets.yaml")


def store_aero(key, source):
    key = key.strip()
    if not key:
        print(f"  {AERO_KEY:15s} empty in {source} - nothing written")
        sys.exit(1)
    if UNSAFE.search(key):
        print(f"  {AERO_KEY:15s} has a space, quote, backslash, ; # or $ - nothing written")
        sys.exit(1)
    # The firmware reads at most 255 characters (aero_direct.cpp kKeyMax). The
    # key's real length is not documented; 16 only refuses an obvious mistake.
    if not 16 <= len(key) <= 255:
        print(f"  {AERO_KEY:15s} is {len(key)} chars, outside 16-255 - nothing written")
        sys.exit(1)
    print(f"  {AERO_KEY:15s} found, {len(key)} chars")
    write_defines({AERO_KEY: str_flag(AERO_KEY, key)})
    print(f"\nwrote {OUT.name}, readable by you only\n")
    check()


def aeroapi_from_ha(host):
    store_aero(secret_from_ha(host, "aeroapi_key"), "Home Assistant's secrets.yaml")


def aeroapi_prompt():
    store_aero(getpass.getpass("FlightAware AeroAPI key (input is not shown): "), "the prompt")


def rtt_prompt(kind):
    value = getpass.getpass("Realtime Trains token (input is not shown): ")
    store_token(bare_token(value), kind, "the prompt")


def check():
    if not OUT.is_file():
        print(f"{OUT.name}: missing")
        sys.exit(1)
    text = OUT.read_text()
    good, mqtt_seen = True, False
    for flag, _, _, _ in SOURCES:
        m = re.search(r'^\s*-D%s=(?:\\"(.*)\\"|(\d+))\s*$' % flag, text, re.M)
        if not m:
            print(f"  {flag:15s} not set{'' if flag not in REQUIRED else '  <- needed with the broker'}")
            good = good and flag not in REQUIRED
            continue
        mqtt_seen = mqtt_seen or flag in REQUIRED
        v = m.group(1) if m.group(1) is not None else m.group(2)
        if v in PLACEHOLDERS:
            print(f"  {flag:15s} still the template placeholder")
            good = good and flag not in REQUIRED
            continue
        print(f"  {flag:15s} set, {len(v)} chars")
    m = re.search(r'^\s*-D%s=\\"(.*)\\"\s*$' % RTT_TOKEN, text, re.M)
    rtt_ok = bool(m) and m.group(1) not in PLACEHOLDERS
    if not m:
        print(f"  {RTT_TOKEN:15s} not set  (the rail board then fetches through Home Assistant only)")
    elif not rtt_ok:
        print(f"  {RTT_TOKEN:15s} still the template placeholder")
    else:
        print(f"  {RTT_TOKEN:15s} set, {len(m.group(1))} chars")
    k = re.search(r'^\s*-D%s=\\"(refresh|access)\\"\s*$' % RTT_KIND, text, re.M)
    print(f"  {RTT_KIND:15s} {k.group(1) if k else 'not set (auto)'}")
    m = re.search(r'^\s*-D%s=\\"(.*)\\"\s*$' % AERO_KEY, text, re.M)
    aero_ok = bool(m) and m.group(1) not in PLACEHOLDERS
    if not m:
        print(f"  {AERO_KEY:15s} not set  (the flight board then uses Home Assistant's MQTT boards)")
    elif not aero_ok:
        print(f"  {AERO_KEY:15s} still the template placeholder")
    else:
        print(f"  {AERO_KEY:15s} set, {len(m.group(1))} chars")
    # A file with only a token or a key is complete: NVS keeps the broker it has.
    if not mqtt_seen and (rtt_ok or aero_ok):
        good = True
    ignored = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", str(OUT)],
                             capture_output=True).returncode == 0
    print(f"  git ignores it: {'yes' if ignored else 'NO - do not commit it'}; "
          f"permissions {oct(OUT.stat().st_mode & 0o777)}")
    print("\nready for: pio run -e provision -t upload" if good else "\nnot ready")
    sys.exit(0 if good else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("import")
    p.add_argument("flagship")
    for name in ("rtt-from-ha", "rtt-prompt"):
        p = sub.add_parser(name)
        p.add_argument("--kind", choices=("refresh", "access", "auto"), default="auto")
        if name == "rtt-from-ha":
            p.add_argument("--host", default="nickohome")
    p = sub.add_parser("aeroapi-from-ha")
    p.add_argument("--host", default="nickohome")
    sub.add_parser("aeroapi-prompt")
    sub.add_parser("check")
    a = ap.parse_args()
    if a.cmd == "import":
        do_import(a.flagship)
    elif a.cmd == "rtt-from-ha":
        rtt_from_ha(a.host, a.kind)
    elif a.cmd == "rtt-prompt":
        rtt_prompt(a.kind)
    elif a.cmd == "aeroapi-from-ha":
        aeroapi_from_ha(a.host)
    elif a.cmd == "aeroapi-prompt":
        aeroapi_prompt()
    else:
        check()


if __name__ == "__main__":
    main()

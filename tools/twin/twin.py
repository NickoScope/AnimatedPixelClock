#!/usr/bin/env python3
"""twin.py - the virtual panel: the real firmware image on an emulated ESP32-S3 and HUB75 panel.

The engine is esp32sim (Rust, MIT) with the models this project added: octal flash, LCD_CAM
i8080 + GDMA, an empty SD slot, the HUB75 panel, the IR receiver and the knob
(docs/40-virtual-twin.md in the knowledge base). The twin's flash chip is a file, so what the
firmware stores - Wi-Fi, settings, the remote's learned codes, Lua effects - stays between runs,
as on the panel.

  twin.py run [--web PORT] [--seconds S] [--png FILE] [--fresh] [-- extra esp32sim flags]
      boot the twin. --web serves the panel page at http://127.0.0.1:PORT/panel.html (real time);
      without it the run is headless and as fast as the Mac allows. --fresh starts from an erased
      chip written with the image, like a new board.
  twin.py flash IMAGE [--at OFFSET]
      write an image into the twin's flash, as esptool write_flash would: a full image
      (firmware-vX-waveshare.bin) at 0x0, an app image (firmware.bin, OTA_ONLY_...) at 0x10000.
  twin.py wifi SSID PASSWORD
      provision Wi-Fi at the next boot the way a person does: an Improv-Serial packet on the USB
      console, which the firmware's own Improv code takes. The virtual AP uses the same pair.
  twin.py erase
      forget everything (a new chip): the next run starts from the image again.
  twin.py run --lan ...
      the twin on the home network with its own address from the router (esp32sim --net bridge
      through the socket_vmnet daemon; README "В домашней сети"). Refuses unless the daemon's
      socket answers, the network is the one `lan-setup` recorded, and the name starts with TWIN-.
  twin.py lan-setup
      at home, once: record this network's gateway (IP and MAC) in ~/twin/state/lan.txt, the
      home guard for --lan. The file stays outside every repository.

Paths (override with the environment): TWIN_HOME (~/twin) holds the engine, the ROM, the flash
file and the default image.
"""
import argparse, os, shutil, struct, subprocess, sys

HOME = os.path.expanduser(os.environ.get("TWIN_HOME", "~/twin"))
ENGINE = os.path.join(HOME, "esp32sim")
EXE = os.path.join(ENGINE, "target", "release", "esp32sim")
ROM = os.path.join(HOME, "rom", "esp32s3_rev0_rom.elf")
STATE = os.path.join(HOME, "state")
FLASH = os.path.join(STATE, "flash.bin")
WIFI = os.path.join(STATE, "wifi.txt")
IMAGE = os.environ.get("TWIN_IMAGE", os.path.join(HOME, "fw", "v2.7.3", "merged.bin"))
ELF = os.environ.get("TWIN_ELF", os.path.join(HOME, "fw", "v2.7.3", "firmware.elf"))
EFUSE = os.path.join(HOME, "efuse-opi.txt")
FLASH_MB = 32
# The module's flash answers as a Macronix octal part. IDF 4.4.7 accepts any 0xC2 0x8x ID
# (spi_flash_oct_flash_init.c, s_probe_mxic_chip); c28039 is Macronix's MX25UM25645G (256 Mbit).
# Not read from our module: esptool flash_id on the panel would settle it.
FLASH_ID = "c28039"
# The twin's own MAC (the owner, 2026-09-29: its own address, never the panel's 90:E5:B1:D2:0E:2C).
# Locally administered (bit 1 of the first byte set, IEEE 802 §8.4), so it can belong to no
# vendor's device; 54 57 49 4E is "TWIN" in ASCII.
MAC = "02:54:57:49:4E:01"
# The bridge (ADR-TWIN-02): socket_vmnet v1.2.2 in bridged mode on en0, installed once by the owner.
LAN_SOCKET = os.environ.get("TWIN_LAN_SOCKET", "/var/run/socket_vmnet.bridged.en0")
LAN_IFACE = "en0"
LAN_STATE = os.path.join(STATE, "lan.txt")
LAN_INSTALL = """\
The bridge daemon is not there. Once, with sudo (ADR-TWIN-02 §3; tools/twin/README.md "В домашней сети"):

  cd ~/Downloads
  curl -OSL https://github.com/lima-vm/socket_vmnet/releases/download/v1.2.2/socket_vmnet-1.2.2-arm64.tar.gz
  gh attestation verify --owner=lima-vm socket_vmnet-1.2.2-arm64.tar.gz
  sudo tar Cxzvf / socket_vmnet-1.2.2-arm64.tar.gz opt/socket_vmnet
  sudo mkdir -p /var/log/socket_vmnet
  sudo cp /opt/socket_vmnet/share/doc/socket_vmnet/launchd/io.github.lima-vm.socket_vmnet.bridged.en0.plist /Library/LaunchDaemons/
  sudo launchctl bootstrap system /Library/LaunchDaemons/io.github.lima-vm.socket_vmnet.bridged.en0.plist
  sudo launchctl enable system/io.github.lima-vm.socket_vmnet.bridged.en0
  sudo launchctl kickstart -kp system/io.github.lima-vm.socket_vmnet.bridged.en0

If it is installed but stopped: sudo launchctl kickstart -k system/io.github.lima-vm.socket_vmnet.bridged.en0
"""


def improv_hex(ssid: str, password: str) -> str:
    """The Improv-Serial "send Wi-Fi settings" packet (Improv WiFi Library, parseImprovSerial)."""
    s, p = ssid.encode(), password.encode()
    rpc = bytes([len(s)]) + s + bytes([len(p)]) + p
    data = bytes([0x01, len(rpc)]) + rpc
    pkt = b"IMPROV" + bytes([1, 3, len(data)]) + data
    return (pkt + bytes([sum(pkt) & 0xFF])).hex()


def nvs_string(flash, namespace, key):
    """A string from the firmware's NVS in the flash file, as ESP-IDF lays it out: 4 KiB pages,
    a 32-byte header (state, sequence), a 32-byte table of 2-bit entry states (0b10 written), then
    126 entries of 32 bytes: ns, type (0x01 u8, 0x21 string), span, chunk, crc, key[16], data[8];
    a string's bytes follow in the next span-1 entries (ESP-IDF nvs_constants.h, nvs_types.hpp,
    nvs.h). The partition comes from the table at 0x8000. None if there is no such value."""
    with open(flash, "rb") as f:
        f.seek(0x8000); table = f.read(0xC00)
        nvs = None
        for i in range(0, len(table), 32):
            e = table[i:i + 32]
            if e[:2] != b"\xaa\x50": break
            if e[2] == 1 and e[3] == 2: nvs = struct.unpack("<II", e[4:12])
        if not nvs: return None
        f.seek(nvs[0]); data = f.read(nvs[1])
    found, ns_index = [], None
    for want_ns in (True, False):
        for page in range(0, len(data), 4096):
            state, seq = struct.unpack("<II", data[page:page + 8])
            if state not in (0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8): continue   # active, full, freeing
            bitmap = int.from_bytes(data[page + 32:page + 64], "little")
            i = 0
            while i < 126:
                e = data[page + 64 + 32 * i:page + 96 + 32 * i]
                if (bitmap >> (2 * i)) & 3 != 0b10: i += 1; continue
                ns, typ, span, name = e[0], e[1], max(e[2], 1), e[8:24].split(b"\0")[0].decode(errors="replace")
                if want_ns and ns == 0 and typ == 0x01 and name == namespace: ns_index = e[24]
                if not want_ns and ns == ns_index and typ == 0x21 and name == key:
                    size = struct.unpack("<H", e[24:26])[0]
                    start = page + 64 + 32 * (i + 1)
                    found.append((seq, data[start:start + size].split(b"\0")[0].decode(errors="replace")))
                i += span
        if ns_index is None: return None
    return max(found)[1] if found else None


def default_gateway():
    """(interface, gateway IP, gateway MAC) of the default route, from route(8) and arp(8)."""
    route = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True).stdout
    fields = dict(l.strip().split(": ", 1) for l in route.splitlines() if ": " in l)
    ip, iface = fields.get("gateway"), fields.get("interface")
    mac = None
    if ip:
        words = subprocess.run(["arp", "-n", ip], capture_output=True, text=True).stdout.split()
        if "at" in words:
            octets = words[words.index("at") + 1].split(":")
            if len(octets) == 6 and all(o and len(o) <= 2 for o in octets):
                mac = ":".join(f"{int(o, 16):02x}" for o in octets)
    return iface, ip, mac


def lan_checks():
    """What must hold before the twin goes on the LAN (ADR-TWIN-02 §4.2). Exits with the reason."""
    import socket
    if not os.path.exists(LAN_SOCKET):
        sys.exit(f"{LAN_SOCKET}: no such socket\n\n{LAN_INSTALL}")
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(2); s.connect(LAN_SOCKET); s.close()
    except OSError as e:
        sys.exit(f"{LAN_SOCKET}: {e}\n\n{LAN_INSTALL}")
    # The home guard: the daemon bridges whatever network en0 is on, so the twin goes out only on
    # the one recorded at home.
    if not os.path.exists(LAN_STATE):
        sys.exit(f"no {LAN_STATE}: run `twin.py lan-setup` once at home first")
    want = dict(l.split("=", 1) for l in open(LAN_STATE).read().split() if "=" in l)
    iface, ip, mac = default_gateway()
    here = {"interface": iface, "gateway_ip": ip, "gateway_mac": mac}
    wrong = [k for k in ("interface", "gateway_ip", "gateway_mac") if want.get(k) != here[k]]
    if wrong:
        sys.exit(f"not the home network ({', '.join(wrong)} differ from {LAN_STATE}): the twin stays off this LAN")
    name = nvs_string(FLASH, "pcmonitor", "deviceName") if os.path.exists(FLASH) else None
    if not (name or "").startswith("TWIN-"):
        sys.exit(f"the twin's name is {name or 'the default (none stored)'}, not TWIN-...: rename it first in a normal run, "
                 f"so the router and HA can tell it from the panel:\n  curl -X POST -d '{{\"name\":\"TWIN-NickoScopeMatrix-64x128-01\"}}' "
                 f"http://127.0.0.1:8080/api/rename   (then restart the twin)")
    print(f"LAN: {LAN_SOCKET}, home network ({ip} on {iface}), name {name}", file=sys.stderr)


def cmd_lan_setup(a, _):
    iface, ip, mac = default_gateway()
    if not (ip and mac):
        sys.exit(f"no default gateway with a known MAC (route: {ip}, arp: {mac}); ping the router once and retry")
    if iface != LAN_IFACE:
        sys.exit(f"the default route goes through {iface}, not {LAN_IFACE}, the interface the daemon bridges")
    os.makedirs(STATE, exist_ok=True)
    with open(os.open(LAN_STATE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        f.write("# twin.py lan-setup: the home network, the guard for run --lan. Not in any repository.\n")
        f.write(f"interface={iface}\ngateway_ip={ip}\ngateway_mac={mac}\n")
    print(f"recorded the home network: gateway {ip} ({mac}) on {iface} -> {LAN_STATE}")


def cmd_run(a, extra):
    os.makedirs(STATE, exist_ok=True)
    if a.lan:                                   # before --fresh could touch the flash
        if a.fresh:
            sys.exit("--lan with --fresh: a new chip has no TWIN- name yet; name it in a normal run first")
        if not os.path.exists(WIFI):
            sys.exit("--lan needs the virtual AP: twin.py wifi SSID PASSWORD first")
        lan_checks()
    if a.fresh and os.path.exists(FLASH):
        os.remove(FLASH)
    args = [EXE, "--board", "panel", "--boot", "rom", "--rom", ROM, "--flash-image", IMAGE,
            "--flash-mb", str(FLASH_MB), "--flash-id", FLASH_ID, "--psram-mb", "16",
            "--efuse-regs", EFUSE, "--elf", ELF, "--console", "usb", "--no-dump",
            "--flash-persist", FLASH, "--mac", a.mac]
    ssid = pw = None
    if os.path.exists(WIFI):
        ssid, pw = open(WIFI).read().splitlines()[:2]
        args += ["--wifi", f"ssid={ssid},psk={pw}"]
        if a.lan:
            # its own address from the router; nothing forwarded to 127.0.0.1
            args += ["--net", f"bridge:{LAN_SOCKET}"]
        else:
            # The portal, the API and the UDP port, from the Mac only (127.0.0.1), as on the LAN.
            args += ["--hostfwd", f"tcp:{a.http}-80", "--hostfwd", f"udp:{a.udp}-4210"]
        if a.provision:
            args += ["--serial-hex", improv_hex(ssid, pw)]
    if a.web:
        args += ["--web", str(a.web), "--web-dir", os.path.join(ENGINE, "web")]
        portal = f"http://<the IP Address line>/ (discover.py --mac {a.mac})" if a.lan else f"http://127.0.0.1:{a.http}/"
        print(f"the panel: http://127.0.0.1:{a.web}/panel.html   its portal: {portal}", file=sys.stderr)
    if a.seconds:
        args += ["--max-seconds", str(a.seconds)]
    if a.png:
        args += ["--tft-png", a.png]
    if a.cpi:
        args += ["--cpi", a.cpi]
    if a.web and a.open:
        # Open the page once the engine's web server answers (macOS `open`).
        import threading, time, urllib.request
        def opener():
            for _ in range(60):
                try:
                    urllib.request.urlopen(f"http://127.0.0.1:{a.web}/panel.html", timeout=1); break
                except Exception:
                    time.sleep(0.5)
            subprocess.run(["open", f"http://127.0.0.1:{a.web}/panel.html"])
        threading.Thread(target=opener, daemon=True).start()
        sys.exit(subprocess.call(args + extra))
    os.execv(EXE, args + extra)


def cmd_flash(a, _):
    data = open(a.image, "rb").read()
    at = int(a.at, 0) if a.at else (0x0 if data[:1] == b"\xe9" and len(data) > 0x10000 and data[0x8000:0x8002] == b"\xaa\x50" else 0x10000)
    if not os.path.exists(FLASH):
        sys.exit("no twin flash yet: run the twin once (it creates it), or pass --fresh to run")
    size = os.path.getsize(FLASH)
    if at + len(data) > size:
        sys.exit(f"{a.image}: {len(data)} bytes at {at:#x} do not fit the {size >> 20} MB flash")
    with open(FLASH, "r+b") as f:
        f.seek(at)
        f.write(data)
    print(f"wrote {len(data)} bytes at {at:#x} into {FLASH}")


def cmd_wifi(a, _):
    os.makedirs(STATE, exist_ok=True)
    with open(WIFI, "w") as f:
        f.write(f"{a.ssid}\n{a.password}\n")
    print(f"the virtual AP is '{a.ssid}'; add --provision to the next run to hand it over by Improv")


def cmd_erase(a, _):
    if os.path.exists(FLASH):
        os.remove(FLASH)
    print("the twin's flash is erased; the next run writes the image into a new chip")


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--web", type=int)
    r.add_argument("--seconds", type=float)
    r.add_argument("--png")
    r.add_argument("--open", action="store_true", help="with --web, open the panel page in the browser")
    r.add_argument("--mac", default=MAC, help=f"the twin's station MAC (default {MAC})")
    r.add_argument("--cpi", default="2.45", help="cycles per instruction; 2.45 matches the panel's Lua draw times "
                   "(calibrate.py: 10 scenes, validate.py: 3 held out, within about 16%%); 1 = the engine's full speed")
    r.add_argument("--fresh", action="store_true")
    r.add_argument("--provision", action="store_true", help="send the Wi-Fi pair over Improv at boot")
    r.add_argument("--http", type=int, default=8080, help="the twin's port 80 on 127.0.0.1 (default 8080)")
    r.add_argument("--udp", type=int, default=4210, help="the twin's UDP 4210 on 127.0.0.1")
    r.add_argument("--lan", action="store_true", help="on the home network with its own IP (socket_vmnet bridge); "
                   "no ports on 127.0.0.1 then, the portal is at the twin's own address")
    f = sub.add_parser("flash")
    f.add_argument("image")
    f.add_argument("--at")
    w = sub.add_parser("wifi")
    w.add_argument("ssid")
    w.add_argument("password")
    sub.add_parser("erase")
    sub.add_parser("lan-setup")
    a = ap.parse_args(argv)
    {"run": cmd_run, "flash": cmd_flash, "wifi": cmd_wifi, "erase": cmd_erase, "lan-setup": cmd_lan_setup}[a.cmd](a, extra)


if __name__ == "__main__":
    main()

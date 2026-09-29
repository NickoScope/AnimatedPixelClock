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

Paths (override with the environment): TWIN_HOME (~/twin) holds the engine, the ROM, the flash
file and the default image.
"""
import argparse, os, shutil, subprocess, sys

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


def improv_hex(ssid: str, password: str) -> str:
    """The Improv-Serial "send Wi-Fi settings" packet (Improv WiFi Library, parseImprovSerial)."""
    s, p = ssid.encode(), password.encode()
    rpc = bytes([len(s)]) + s + bytes([len(p)]) + p
    data = bytes([0x01, len(rpc)]) + rpc
    pkt = b"IMPROV" + bytes([1, 3, len(data)]) + data
    return (pkt + bytes([sum(pkt) & 0xFF])).hex()


def cmd_run(a, extra):
    os.makedirs(STATE, exist_ok=True)
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
        # The portal, the API and the UDP port, from the Mac only (127.0.0.1), as on the LAN.
        args += ["--hostfwd", f"tcp:{a.http}-80", "--hostfwd", f"udp:{a.udp}-4210"]
        if a.provision:
            args += ["--serial-hex", improv_hex(ssid, pw)]
    if a.web:
        args += ["--web", str(a.web), "--web-dir", os.path.join(ENGINE, "web")]
        print(f"the panel: http://127.0.0.1:{a.web}/panel.html   its portal: http://127.0.0.1:{a.http}/", file=sys.stderr)
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
    f = sub.add_parser("flash")
    f.add_argument("image")
    f.add_argument("--at")
    w = sub.add_parser("wifi")
    w.add_argument("ssid")
    w.add_argument("password")
    sub.add_parser("erase")
    a = ap.parse_args(argv)
    {"run": cmd_run, "flash": cmd_flash, "wifi": cmd_wifi, "erase": cmd_erase}[a.cmd](a, extra)


if __name__ == "__main__":
    main()

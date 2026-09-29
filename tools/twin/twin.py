#!/usr/bin/env python3
"""twin.py - the virtual panel: the real firmware image on an emulated ESP32-S3 and HUB75 panel.

The engine is esp32sim (Rust, MIT) with the models this project added: octal flash, LCD_CAM
i8080 + GDMA, an empty SD slot, the HUB75 panel, the IR receiver and the knob
(docs/40-virtual-twin.md in the knowledge base). The twin's flash chip is a file, so what the
firmware stores - Wi-Fi, settings, the remote's learned codes, Lua effects - stays between runs,
as on the panel.

  twin.py run [--web PORT] [--seconds S] [--png FILE] [--fresh | --blank] [-- extra esp32sim flags]
      boot the twin. --web serves the panel page at http://127.0.0.1:PORT/panel.html (real time)
      and the project's web flasher at http://127.0.0.1:PORT/flasher/index.html, a copy of docs/
      that flashes the twin over its USB-Serial/JTAG (tools/twin/flasher/); without --web the run
      is headless and as fast as the Mac allows. --fresh starts from an erased chip written with
      the image, like a new board; --blank from an erased chip with nothing on it, as it comes
      from the factory, for the flasher to write. The flasher offers docs/firmware/latest, or
      --flasher-image FILE (a merged image) under --flasher-version VER.
  twin.py flash IMAGE [--at OFFSET]
      write an image into the twin's flash, as `pio run -t upload` would: a full image
      (firmware-vX-waveshare.bin) at 0x0; an app image (firmware.bin, OTA_ONLY_...) at 0x10000 with
      otadata set back to app0 (boot_app0.bin), so it boots even after an OTA put the twin on app1.
      Anything else (bootloader.bin, partitions.bin) needs --at, as esptool always does.
  twin.py wifi SSID PASSWORD
      provision Wi-Fi at the next boot the way a person does: an Improv-Serial packet on the USB
      console, which the firmware's own Improv code takes. The virtual AP uses the same pair; an
      empty PASSWORD ("") makes it an open network. Neither may hold a ',' (the engine's --wifi).
  twin.py erase
      forget everything (a new chip): the next run starts from the image again.
  twin.py verify [IMAGE]
      compare the twin's flash with a merged image (default: the one the flasher offers,
      docs/firmware/latest) outside the image's data partitions, e.g. after flashing it from the
      web flasher. Differences in data partitions (nvs, otadata, spiffs) are listed, not judged:
      the firmware writes them. Exit status 1 if anything else differs.

Paths (override with the environment): TWIN_HOME (~/twin) holds the engine, the ROM, the flash
file and the default image; TWIN_ENGINE (TWIN_HOME/esp32sim) is the engine's checkout.
"""
import argparse, binascii, os, re, shutil, subprocess, sys

HOME = os.path.expanduser(os.environ.get("TWIN_HOME", "~/twin"))
ENGINE = os.path.expanduser(os.environ.get("TWIN_ENGINE", os.path.join(HOME, "esp32sim")))
EXE = os.path.join(ENGINE, "target", "release", "esp32sim")
ROM = os.path.join(HOME, "rom", "esp32s3_rev0_rom.elf")
STATE = os.path.join(HOME, "state")
FLASH = os.path.join(STATE, "flash.bin")
WIFI = os.path.join(STATE, "wifi.txt")
IMAGE = os.environ.get("TWIN_IMAGE", os.path.join(HOME, "fw", "v2.7.3", "merged.bin"))
ELF = os.environ.get("TWIN_ELF", os.path.join(HOME, "fw", "v2.7.3", "firmware.elf"))
EFUSE = os.path.join(HOME, "efuse-opi.txt")
# The engine's pages and the flasher's copy, made again by every run with --web (build_web).
WEB = os.path.join(STATE, "web")
HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "docs")   # the public flasher page
FLASHER_SRC = os.path.join(HERE, "flasher")                           # twin-serial.js, selftest.html
FLASH_MB = 32
# The module's flash answers as a Macronix octal part. IDF 4.4.7 accepts any 0xC2 0x8x ID
# (spi_flash_oct_flash_init.c, s_probe_mxic_chip); c28039 is Macronix's MX25UM25645G (256 Mbit).
# Not read from our module: esptool flash_id on the panel would settle it.
FLASH_ID = "c28039"
# The twin's own MAC (the owner, 2026-09-29: its own address, never the panel's 90:E5:B1:D2:0E:2C).
# Locally administered (bit 1 of the first byte set, IEEE 802 §8.4), so it can belong to no
# vendor's device; 54 57 49 4E is "TWIN" in ASCII.
MAC = "02:54:57:49:4E:01"
# Where an app image goes: ESP32_APP_OFFSET, app0 in this build's partition table (tools/flash/README.md).
APP0 = 0x10000
# esp_app_desc_t.magic_word, ESP_APP_DESC_MAGIC_WORD (IDF 4.4 esp_app_format.h), at 0x20 of an app
# image: after the 24-byte image header and the first 8-byte segment header
# (bootloader_common_get_partition_description). A bootloader image has no app descriptor.
APP_DESC_MAGIC = (0xABCD5432).to_bytes(4, "little")


def _boot_app0() -> bytes:
    """otadata as `pio run -t upload` writes it at 0xe000 (tools/flash/README.md:62, 70, 77):
    framework-arduinoespressif32 tools/partitions/boot_app0.bin, sha256 f94c5d78..., the same 8 KiB
    as merged.bin's 0xe000. Entry 0: ota_seq 1, label and state blank, crc = crc32_le(UINT32_MAX,
    seq) (IDF 4.4 bootloader_common_ota_select_crc); entry 1, the second sector: ota_seq 0, crc
    blank, so not valid. The bootloader therefore starts ota_{(1 - 1) % 2} = app0."""
    seq = (1).to_bytes(4, "little")
    e0 = seq + b"\xff" * 24 + binascii.crc32(seq, 0xFFFFFFFF).to_bytes(4, "little")
    return e0 + b"\xff" * (0x1000 - 32) + b"\0" * 4 + b"\xff" * (0x1000 - 4)


BOOT_APP0 = _boot_app0()


def partitions(head: bytes):
    """The partition table at 0x8000: esp_partition_info_t, 32 bytes each (magic AA 50, type, subtype,
    offset, size, label[16], flags; IDF 4.4 esp_flash_partitions.h), up to 0xC00 bytes
    (ESP_PARTITION_TABLE_MAX_LEN), ended by the MD5 entry (EB EB) or blank flash.
    Returns (type, subtype, offset, size, label) tuples."""
    out = []
    for i in range(0x8000, 0x8C00, 32):
        e = head[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        out.append((e[2], e[3], int.from_bytes(e[4:8], "little"), int.from_bytes(e[8:12], "little"),
                    e[12:28].split(b"\0")[0].decode("ascii", "replace")))
    return out


def boot_slot(otadata: bytes, apps: int) -> int:
    """The OTA slot the IDF 4.4 bootloader starts (bootloader_utility.c): of the two otadata entries
    (one per 4 KiB sector), the valid one - ota_seq not blank, ota_state not INVALID (3) or ABORTED
    (4), crc right (bootloader_common_ota_select_valid) - with the highest ota_seq picks
    ota_{(seq - 1) % apps}. With neither valid it tries ota_0 (this layout has no factory app)."""
    best = None
    for k in (0, 0x1000):
        e = otadata[k:k + 32]
        seq, state, crc = (int.from_bytes(e[o:o + 4], "little") for o in (0, 24, 28))
        if seq != 0xFFFFFFFF and state not in (3, 4) and crc == binascii.crc32(e[:4], 0xFFFFFFFF):
            best = seq if best is None else max(best, seq)
    return 0 if best is None else (best - 1) % apps


def wifi_spec(ssid: str, pw: str) -> str:
    """The engine's --wifi value for the virtual AP. ApConfig::parse (esp32sim esp-soc/src/wifi.rs)
    splits it at ',' with no escaping, so a ',' in either cannot get through; refused here rather
    than the engine exiting on it. An empty password leaves psk out, an open AP: WiFi.begin(ssid, "")
    joins an open network (Arduino-ESP32 2.0.17 WiFiSTA.cpp wifi_sta_config sets the password and
    WPA2 only for a non-empty one), while psk= would advertise WPA2 with an empty key."""
    for what, v in (("SSID", ssid), ("password", pw)):
        if "," in v:
            sys.exit(f"the {what} holds a ',', which the engine's --wifi cannot carry (esp-soc/src/wifi.rs "
                     "splits the spec at ','); choose one without")
    return f"ssid={ssid}" + (f",psk={pw}" if pw else "")


def improv_hex(ssid: str, password: str) -> str:
    """The Improv-Serial "send Wi-Fi settings" packet (Improv WiFi Library, parseImprovSerial)."""
    s, p = ssid.encode(), password.encode()
    rpc = bytes([len(s)]) + s + bytes([len(p)]) + p
    data = bytes([0x01, len(rpc)]) + rpc
    pkt = b"IMPROV" + bytes([1, 3, len(data)]) + data
    return (pkt + bytes([sum(pkt) & 0xFF])).hex()


# ---- the web flasher's copy for the twin ----------------------------------------------------------
# ESP Web Tools pinned to the version the shim was written against (twin-serial.js); docs/index.html
# asks unpkg for @10, whatever 10.x is current.
EWT_VERSION = "10.4.0"
# docs/flasher.js:15 and :38: the image is firmware/latest/AnimatedPixelClock-<firmware>-<VERSION>-Full.bin
FIRMWARE_ID = "waveshare"
GENERATED = ".twin-generated"
BANNER = ('<div role="note" style="background:#1d1b16;color:#f4f0e7;font:14px/1.45 system-ui,-apple-system,sans-serif;'
          'padding:8px 16px;text-align:center">Копия прошивальщика для виртуального двойника: Install → '
          '«Двойник» прошивает эмулятор на этом Mac, «Плата по USB» — настоящую плату. '
          '<a href="../panel.html" style="color:#f0b429">Панель двойника</a></div>')
# (anchor, replacement): each anchor must occur exactly once in docs/index.html, so a change to the
# public page that moves one stops the build instead of leaving a half-made copy.
FLASHER_EDITS = (
    # first in <head>, a classic script: it must run before ESP Web Tools' module (twin-serial.js)
    ('<meta charset="utf-8">', '<meta charset="utf-8">\n  <script src="twin-serial.js"></script>'),
    ("https://unpkg.com/esp-web-tools@10/", f"https://unpkg.com/esp-web-tools@{EWT_VERSION}/"),
    ("<title>", "<title>Двойник · "),
    ("<body>", "<body>\n" + BANNER),
)


def flasher_index(html: str) -> str:
    """docs/index.html as the twin serves it: the navigator.serial shim, ESP Web Tools pinned, a
    title and a note that say whose page it is. Nothing else changes; flasher.js is copied as is."""
    for old, new in FLASHER_EDITS:
        n = html.count(old)
        if n != 1:
            raise ValueError(f"docs/index.html: {old!r} occurs {n} times, not once; the twin's copy of the page "
                             "would be wrong, so update FLASHER_EDITS in tools/twin/twin.py")
        html = html.replace(old, new)
    return html


def flasher_firmware(image=None, version=None):
    """(image, version) the twin's flasher offers: the release in docs/firmware/latest, or IMAGE under
    VERSION. The page writes it at 0x0 (docs/flasher.js:53), so it has to be a merged image."""
    if image is None:
        if version is not None:
            sys.exit("--flasher-version goes with --flasher-image")
        latest = os.path.join(DOCS, "firmware", "latest")
        with open(os.path.join(latest, "VERSION")) as f:
            version = f.read().strip()
        image = os.path.join(latest, f"AnimatedPixelClock-{FIRMWARE_ID}-{version}-Full.bin")
        if not os.path.exists(image):
            sys.exit(f"{image} is missing: docs/firmware/latest has VERSION {version} but not its image")
        return image, version
    if not version:
        sys.exit("--flasher-image needs --flasher-version (the name the page shows and builds the file name from)")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        sys.exit(f"--flasher-version {version!r}: letters, digits, '.', '_' and '-' only (it becomes a file name)")
    with open(image, "rb") as f:
        head = f.read(0x8002)
    if head[:1] != b"\xe9" or head[0x8000:0x8002] != b"\xaa\x50":
        sys.exit(f"{image}: not a merged image (bootloader at 0x0, partition table at 0x8000); the page writes it at 0x0")
    return image, version


def build_web(dest, image=None, version=None):
    """Make DEST: the engine's web/ (panel.html and the rest), and flasher/ with the page from docs/
    (flasher_index), flasher.js, styles.css, img/, the shim, the self-test page and the firmware
    in firmware/latest/. Made again on every run; DEST must be absent or made by this function.
    Returns the firmware version offered."""
    image, version = flasher_firmware(image, version)
    if os.path.lexists(dest):
        if not os.path.exists(os.path.join(dest, GENERATED)):
            sys.exit(f"{dest} exists and was not made by twin.py; move it away (it is rebuilt on every run)")
        shutil.rmtree(dest)
    shutil.copytree(os.path.join(ENGINE, "web"), dest)
    with open(os.path.join(dest, GENERATED), "w") as f:
        f.write("made by tools/twin/twin.py on every run with --web; edits here are lost\n")
    fl = os.path.join(dest, "flasher")
    fw = os.path.join(fl, "firmware", "latest")
    os.makedirs(fw)
    with open(os.path.join(DOCS, "index.html"), encoding="utf-8") as f:
        page = flasher_index(f.read())
    with open(os.path.join(fl, "index.html"), "w", encoding="utf-8") as f:
        f.write(page)
    for name in ("flasher.js", "styles.css"):
        shutil.copy2(os.path.join(DOCS, name), fl)
    shutil.copytree(os.path.join(DOCS, "img"), os.path.join(fl, "img"))
    for name in ("twin-serial.js", "selftest.html"):
        shutil.copy2(os.path.join(FLASHER_SRC, name), fl)
    shutil.copy2(image, os.path.join(fw, f"AnimatedPixelClock-{FIRMWARE_ID}-{version}-Full.bin"))
    with open(os.path.join(fw, "VERSION"), "w") as f:
        f.write(version + "\n")
    return version


def cmd_run(a, extra):
    os.makedirs(STATE, exist_ok=True)
    if (a.flasher_image or a.flasher_version) and not a.web:
        sys.exit("--flasher-image and --flasher-version go with --web (the flasher page is served there)")
    if (a.fresh or a.blank) and os.path.exists(FLASH):
        # The old chip is kept, never just deleted: it holds what the firmware stored (Wi-Fi, settings,
        # the remote's codes, Lua effects, and on the owner's twin the broker credentials).
        import time
        keep = os.path.join(STATE, "backup", time.strftime("flash-%Y%m%d-%H%M%S.bin"))
        os.makedirs(os.path.dirname(keep), exist_ok=True)
        os.replace(FLASH, keep)
        print(f"the old flash chip is kept as {keep}", file=sys.stderr)
    # --blank: no image, so the flash file starts erased (0xFF) and the ROM finds nothing to boot
    image = [] if a.blank else ["--flash-image", IMAGE]
    args = [EXE, "--board", "panel", "--boot", "rom", "--rom", ROM, *image,
            "--flash-mb", str(FLASH_MB), "--flash-id", FLASH_ID, "--psram-mb", "16",
            "--efuse-regs", EFUSE, "--elf", ELF, "--console", "usb", "--no-dump",
            "--flash-persist", FLASH, "--mac", a.mac]
    ssid = pw = None
    if os.path.exists(WIFI):
        ssid, pw = (open(WIFI).read().splitlines() + ["", ""])[:2]
        args += ["--wifi", wifi_spec(ssid, pw)]   # checked again: wifi.txt may have been edited by hand
        # The portal, the API and the UDP port, from the Mac only (127.0.0.1), as on the LAN.
        args += ["--hostfwd", f"tcp:{a.http}-80", "--hostfwd", f"udp:{a.udp}-4210"]
        if a.provision:
            args += ["--serial-hex", improv_hex(ssid, pw)]
    if a.web:
        version = build_web(WEB, a.flasher_image, a.flasher_version)
        args += ["--web", str(a.web), "--web-dir", WEB]
        print(f"the panel: http://127.0.0.1:{a.web}/panel.html   its portal: http://127.0.0.1:{a.http}/\n"
              f"the flasher ({version}): http://127.0.0.1:{a.web}/flasher/index.html", file=sys.stderr)
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
    if a.at:
        at = int(a.at, 0)
    elif data[:1] == b"\xe9" and len(data) > 0x10000 and data[0x8000:0x8002] == b"\xaa\x50":
        at = 0x0                  # a merged image: bootloader, partition table, otadata and app
    elif data[:1] == b"\xe9" and data[0x20:0x24] == APP_DESC_MAGIC:
        at = APP0                 # an app image
    else:
        sys.exit(f"{a.image}: neither a merged image nor an app image, so give its address with --at, as esptool "
                 "needs: in this build's merged.bin the bootloader is at 0x0 and the partition table at 0x8000")
    if not os.path.exists(FLASH):
        sys.exit("no twin flash yet: run the twin once (it creates it), or pass --fresh to run")
    size = os.path.getsize(FLASH)
    if at + len(data) > size:
        sys.exit(f"{a.image}: {len(data)} bytes at {at:#x} do not fit the {size >> 20} MB flash")
    with open(FLASH, "r+b") as f:
        f.seek(at)
        f.write(data)
        print(f"wrote {len(data)} bytes at {at:#x} into {FLASH}")
        f.seek(0)
        table = partitions(f.read(0x8C00))
        apps = sorted((p for p in table if p[0] == 0 and 0x10 <= p[1] <= 0x1F), key=lambda p: p[1])   # ota_0..
        ota = next((p for p in table if p[0] == 1 and p[1] == 0), None)
        slot = next((i for i, p in enumerate(apps) if p[2] == at), None)
        if ota is None or slot is None:
            return
        if slot == 0 and ota[3] >= len(BOOT_APP0):
            # pio upload writes boot_app0.bin with every app: after an OTA (app1 selected) an app
            # written to app0 alone would never run (tools/flash/README.md:77).
            f.seek(ota[2])
            f.write(BOOT_APP0)
            print(f"otadata at {ota[2]:#x} set to boot {apps[0][4]} (boot_app0.bin, as pio upload writes it)")
            return
        f.seek(ota[2])
        active = boot_slot(f.read(0x2000), len(apps))
        if active != slot:
            print(f"note: otadata still starts {apps[active][4]}; this image in {apps[slot][4]} runs only once "
                  "an OTA selects it", file=sys.stderr)


def cmd_wifi(a, _):
    wifi_spec(a.ssid, a.password)   # refuse before writing what the engine cannot take
    os.makedirs(STATE, exist_ok=True)
    with open(WIFI, "w") as f:
        f.write(f"{a.ssid}\n{a.password}\n")
    print(f"the virtual AP is '{a.ssid}'; add --provision to the next run to hand it over by Improv")


def cmd_erase(a, _):
    if os.path.exists(FLASH):
        os.remove(FLASH)
    print("the twin's flash is erased; the next run writes the image into a new chip")


def differing(a: bytes, b: bytes, block=4096):
    """[start, end) ranges where A and B differ, over the shorter of the two."""
    n, out, i = min(len(a), len(b)), [], 0
    while i < n:
        j = min(n, i + block)
        if a[i:j] == b[i:j]:
            i = j
            continue
        for k in range(i, j):
            if a[k] != b[k]:
                if out and out[-1][1] == k:
                    out[-1][1] = k + 1
                else:
                    out.append([k, k + 1])
        i = j
    return [tuple(r) for r in out]


def cmd_verify(a, _):
    image = a.image or flasher_firmware()[0]
    with open(image, "rb") as f:
        img = f.read()
    if not os.path.exists(FLASH):
        sys.exit("no twin flash yet: run the twin once")
    with open(FLASH, "rb") as f:
        flash = f.read(len(img))
    table = partitions(img)
    if not table:
        sys.exit(f"{image}: no partition table at 0x8000, so not a merged image")

    def home(off):
        return next((p for p in table if p[2] <= off < p[2] + p[3]), None)
    bad, data = [], {}
    for lo, hi in differing(img, flash):
        p = home(lo)
        if p is not None and p[0] == 1 and hi <= p[2] + p[3]:   # a data partition (type 1)
            data[p[4]] = data.get(p[4], 0) + hi - lo
        else:
            bad.append((lo, hi, p[4] if p else "no partition (bootloader, table)"))
    print(f"{FLASH} against {image} ({len(img)} bytes)")
    for label, n in data.items():
        print(f"  {label}: {n} bytes differ (a data partition: the firmware's own)")
    for lo, hi, where in bad:
        print(f"  DIFFERENT {lo:#x}..{hi:#x} ({hi - lo} bytes) in {where}")
    print("the same outside the data partitions" if not bad else f"{len(bad)} range(s) differ outside the data partitions")
    sys.exit(1 if bad else 0)


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
    r.add_argument("--cpi", default="2.45", help="cycles per instruction; 2.45 fits the panel's Lua draw times "
                   "(calibrate.py: the 10 fitted scenes then run -12%% to +30%% against the panel's times, "
                   "slowest on text; validate.py: 3 held-out effects within about 16%%); 1 = the engine's full speed")
    r.add_argument("--fresh", action="store_true", help="a new chip written with the image")
    r.add_argument("--blank", action="store_true", help="a new chip with nothing on it, for the web flasher")
    r.add_argument("--flasher-image", help="with --web: the merged image the flasher offers (default docs/firmware/latest)")
    r.add_argument("--flasher-version", help="with --flasher-image: its version, as the page shows it")
    r.add_argument("--provision", action="store_true", help="send the Wi-Fi pair over Improv at boot")
    r.add_argument("--http", type=int, default=8080, help="the twin's port 80 on 127.0.0.1 (default 8080)")
    r.add_argument("--udp", type=int, default=4210, help="the twin's UDP 4210 on 127.0.0.1")
    f = sub.add_parser("flash")
    f.add_argument("image")
    f.add_argument("--at")
    w = sub.add_parser("wifi")
    w.add_argument("ssid")
    w.add_argument("password", help='"" for an open network')
    sub.add_parser("erase")
    v = sub.add_parser("verify")
    v.add_argument("image", nargs="?", help="a merged image (default: the flasher's, docs/firmware/latest)")
    a = ap.parse_args(argv)
    {"run": cmd_run, "flash": cmd_flash, "wifi": cmd_wifi, "erase": cmd_erase, "verify": cmd_verify}[a.cmd](a, extra)


if __name__ == "__main__":
    main()

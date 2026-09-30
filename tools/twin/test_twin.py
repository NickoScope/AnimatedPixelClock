#!/usr/bin/env python3
"""Offline tests of twin.py and calibrate.py: synthetic flash files in a temporary directory, the
engine never started (os.execv and subprocess.Popen are replaced).

  python3 -m unittest tools/twin/test_twin.py -v

Where ~/twin/fw/v2.7.3/merged.bin exists, the otadata and partition checks also run on it.
"""
import binascii
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import twin as T  # noqa: E402
import calibrate as C  # noqa: E402

def put(path, data):
    with open(path, "w" if isinstance(data, str) else "wb") as f:
        f.write(data)


def get(path, n=-1):
    with open(path, "rb") as f:
        return f.read(n)


MERGED = os.path.expanduser("~/twin/fw/v2.7.3/merged.bin")
BOOT_APP0_SHA = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"   # framework boot_app0.bin
APP1 = 0x490000


def entry(type_, sub, off, size, label):
    return b"\xaa\x50" + bytes([type_, sub]) + off.to_bytes(4, "little") + size.to_bytes(4, "little") \
        + label.encode().ljust(16, b"\0") + b"\0" * 4


def ota_entry(seq, state=0xFFFFFFFF, crc=None):
    s = seq.to_bytes(4, "little")
    crc = binascii.crc32(s, 0xFFFFFFFF) if crc is None else crc
    return s + b"\xff" * 20 + state.to_bytes(4, "little") + crc.to_bytes(4, "little")


def otadata(e0, e1=b""):
    return e0.ljust(0x1000, b"\xff") + e1.ljust(0x1000, b"\xff")


# The v2.7.3 layout (merged.bin at 0x8000): nvs, otadata at 0xe000, app0 at 0x10000, app1 at 0x490000.
TABLE = (entry(1, 0x02, 0x9000, 0x5000, "nvs") + entry(1, 0x00, 0xE000, 0x2000, "otadata")
         + entry(0, 0x10, 0x10000, 0x480000, "app0") + entry(0, 0x11, APP1, 0x480000, "app1"))
AFTER_OTA = otadata(ota_entry(1), ota_entry(2, state=2))   # ~/twin/state/flash-ota.bin: seq 2 VALID
APP = b"\xe9\x06\x02\x2f" + b"\0" * 28 + (0xABCD5432).to_bytes(4, "little") + b"app" * 3000
BOOTLOADER = b"\xe9\x03\x02\x2f" + b"\0" * 28 + b"\xff" * 4 + b"boot" * 3000
PARTITIONS = TABLE + b"\xeb\xeb" + b"\xff" * 30


class TwinFlash(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.flash = os.path.join(self.tmp.name, "flash.bin")
        img = bytearray(b"\xff" * 0x920000)
        img[0x8000:0x8000 + len(TABLE)] = TABLE
        img[0xE000:0x10000] = AFTER_OTA
        put(self.flash, img)
        mock.patch.object(T, "FLASH", self.flash).start()
        self.addCleanup(mock.patch.stopall)
        self.addCleanup(self.tmp.cleanup)

    def flash_file(self, data, name, at=None):
        path = os.path.join(self.tmp.name, name)
        put(path, data)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            T.cmd_flash(mock.Mock(image=path, at=at), [])
        return out.getvalue(), err.getvalue()

    def read(self, off, n):
        with open(self.flash, "rb") as f:
            f.seek(off)
            return f.read(n)

    # finding 10: an app written to app0 after an OTA must also get otadata back on app0
    def test_boot_app0_is_the_framework_file(self):
        self.assertEqual(hashlib.sha256(T.BOOT_APP0).hexdigest(), BOOT_APP0_SHA)
        self.assertEqual(T.BOOT_APP0[28:32], (0x4743989A).to_bytes(4, "little"))   # crc32_le(UINT32_MAX, 1)
        self.assertEqual(T.boot_slot(T.BOOT_APP0, 2), 0)

    @unittest.skipUnless(os.path.exists(MERGED), "no merged.bin")
    def test_boot_app0_is_what_merged_bin_carries(self):
        m = get(MERGED, 0x10000)
        self.assertEqual(m[0xE000:0x10000], T.BOOT_APP0)
        self.assertEqual([p[:4] for p in T.partitions(m) if p[4] in ("otadata", "app0", "app1")],
                         [(1, 0, 0xE000, 0x2000), (0, 0x10, 0x10000, 0x480000), (0, 0x11, APP1, 0x480000)])

    def test_boot_slot_follows_the_idf_rules(self):
        self.assertEqual(T.boot_slot(AFTER_OTA, 2), 1)                                       # highest valid seq
        self.assertEqual(T.boot_slot(otadata(ota_entry(1), ota_entry(2, state=3)), 2), 0)    # INVALID ignored
        self.assertEqual(T.boot_slot(otadata(ota_entry(1), ota_entry(2, state=4)), 2), 0)    # ABORTED ignored
        self.assertEqual(T.boot_slot(otadata(ota_entry(1), ota_entry(2, crc=0)), 2), 0)      # bad crc ignored
        self.assertEqual(T.boot_slot(otadata(ota_entry(3)), 2), 0)                           # (3 - 1) % 2
        self.assertEqual(T.boot_slot(b"\xff" * 0x2000, 2), 0)                                # blank: ota_0

    def test_app_after_ota_boots_app0(self):
        self.assertEqual(T.boot_slot(self.read(0xE000, 0x2000), 2), 1)
        out, _ = self.flash_file(APP, "firmware.bin")
        self.assertIn("at 0x10000", out)
        self.assertIn("otadata at 0xe000 set to boot app0", out)
        self.assertEqual(self.read(0x10000, len(APP)), APP)
        self.assertEqual(self.read(0xE000, 0x2000), T.BOOT_APP0)
        self.assertEqual(T.boot_slot(self.read(0xE000, 0x2000), 2), 0)

    def test_app_at_0x10000_given_with_at_also_resets_otadata(self):
        self.flash_file(APP, "firmware.bin", at="0x10000")
        self.assertEqual(self.read(0xE000, 0x2000), T.BOOT_APP0)

    def test_app1_not_selected_is_reported(self):
        self.flash_file(APP, "firmware.bin")                        # otadata now on app0
        _, err = self.flash_file(APP, "firmware.bin", at=hex(APP1))
        self.assertIn("otadata still starts app0", err)
        self.assertEqual(self.read(0xE000, 0x2000), T.BOOT_APP0)     # never rewritten for app1

    def test_merged_image_goes_to_0(self):
        merged = bytearray(b"\xff" * 0x20000)
        merged[:len(BOOTLOADER)] = BOOTLOADER
        merged[0x8000:0x8000 + len(TABLE)] = TABLE
        merged[0xE000:0x10000] = T.BOOT_APP0
        merged[0x10000:0x10000 + len(APP)] = APP
        out, _ = self.flash_file(bytes(merged), "merged.bin")
        self.assertIn("at 0x0 ", out)
        self.assertEqual(T.boot_slot(self.read(0xE000, 0x2000), 2), 0)

    # finding 11: without --at, only a merged image or an app image has an address
    def test_bootloader_and_partition_table_need_at(self):
        before = get(self.flash)
        for data, name in ((BOOTLOADER, "bootloader.bin"), (PARTITIONS, "partitions.bin"), (b"\0" * 100, "x.bin")):
            with self.assertRaises(SystemExit) as e:
                self.flash_file(data, name)
            self.assertIn("--at", str(e.exception))
        self.assertEqual(get(self.flash), before)

    def test_bootloader_with_at_goes_where_asked(self):
        self.flash_file(BOOTLOADER, "bootloader.bin", at="0x0")
        self.assertEqual(self.read(0, len(BOOTLOADER)), BOOTLOADER)


def with_elf(elf: bytes) -> bytes:
    """APP built from ELF: its descriptor carries the ELF's SHA-256 (app_elf_sha256 at 0xB0)."""
    a = bytearray(APP)
    a[0xB0:0xD0] = hashlib.sha256(elf).digest()
    return bytes(a)


class TwinSymbols(unittest.TestCase):
    # The engine's symbols are those of the firmware the chip boots, found by the SHA-256 the build
    # wrote into the app's descriptor; a rebuilt ELF of the same version is not taken.
    def test_the_elf_of_the_slot_that_boots(self):
        with tempfile.TemporaryDirectory() as d:
            fw, elfs = os.path.join(d, "fw"), {"v1": b"\x7fELF one", "v2": b"\x7fELF two"}
            for name, elf in elfs.items():
                os.makedirs(os.path.join(fw, name))
                put(os.path.join(fw, name, "firmware.elf"), elf)
            img = bytearray(b"\xff" * 0x920000)
            img[0x8000:0x8000 + len(TABLE)] = TABLE
            img[0x10000:0x10000 + len(APP)] = with_elf(elfs["v1"])
            img[APP1:APP1 + len(APP)] = with_elf(elfs["v2"])
            flash = os.path.join(d, "flash.bin")
            img[0xE000:0x10000] = otadata(ota_entry(1))                    # app0 boots
            put(flash, img)
            self.assertEqual(T.symbols_for(flash, fw), os.path.join(fw, "v1", "firmware.elf"))
            img[0xE000:0x10000] = AFTER_OTA                                # an OTA moved it to app1
            put(flash, img)
            self.assertEqual(T.symbols_for(flash, fw), os.path.join(fw, "v2", "firmware.elf"))
            put(os.path.join(fw, "v2", "firmware.elf"), b"\x7fELF rebuilt")  # not the build that runs
            self.assertIsNone(T.symbols_for(flash, fw))

    def test_no_descriptor_no_symbols(self):
        self.assertIsNone(T.app_elf_sha(BOOTLOADER))
        self.assertEqual(T.app_elf_sha(with_elf(b"x")), hashlib.sha256(b"x").digest())
        with tempfile.TemporaryDirectory() as d:
            blank = os.path.join(d, "blank.bin")
            put(blank, b"\xff" * 0x9000)
            self.assertIsNone(T.booted_elf_sha(blank))                     # no partition table
            self.assertIsNone(T.symbols_for(os.path.join(d, "missing.bin"), d))

    def test_the_release_carries_its_elf_sha(self):
        image = T.flasher_firmware()[0]
        sha = T.booted_elf_sha(image)
        self.assertIsNotNone(sha)
        elf = T.symbols_for(image)
        if elf is None:
            self.skipTest(f"the release's firmware.elf is not under {T.FW}")
        self.assertEqual(hashlib.sha256(get(elf)).digest(), sha)


class TwinWifi(unittest.TestCase):
    # finding 12: the engine's --wifi is split at ',' and psk= means WPA2
    def test_spec(self):
        self.assertEqual(T.wifi_spec("NickoTwin", "twin-demo-2026"), "ssid=NickoTwin,psk=twin-demo-2026")
        self.assertEqual(T.wifi_spec("Guest", ""), "ssid=Guest")                  # an open AP
        self.assertEqual(T.wifi_spec("Home", "a=b"), "ssid=Home,psk=a=b")         # split_once: '=' is fine
        for ssid, pw in (("Home", "pa,ss"), ("Ho,me", "password")):
            with self.assertRaises(SystemExit):
                T.wifi_spec(ssid, pw)

    def test_wifi_refuses_before_writing_and_run_uses_the_spec(self):
        with tempfile.TemporaryDirectory() as d:
            wifi = os.path.join(d, "wifi.txt")
            with mock.patch.object(T, "STATE", d), mock.patch.object(T, "WIFI", wifi), \
                    mock.patch.object(T, "FLASH", os.path.join(d, "flash.bin")), \
                    mock.patch.object(T, "FW", os.path.join(d, "fw")), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    T.cmd_wifi(mock.Mock(ssid="Home", password="pa,ss"), [])
                self.assertFalse(os.path.exists(wifi))
                with contextlib.redirect_stdout(io.StringIO()):
                    T.cmd_wifi(mock.Mock(ssid="Guest", password=""), [])
                a = mock.Mock(fresh=False, blank=False, lan=False, provision=True, web=None, seconds=None, png=None, cpi="2.45",
                              open=False, http=8080, udp=4210, mac=T.MAC, flasher_image=None, flasher_version=None)
                with mock.patch.object(T.os, "execv") as execv:
                    T.cmd_run(a, [])
                args = execv.call_args[0][1]
                self.assertEqual(args[args.index("--wifi") + 1], "ssid=Guest")
                self.assertEqual(args[args.index("--serial-hex") + 1], T.improv_hex("Guest", ""))
                put(wifi, "Home\npa,ss\n")                                # edited by hand
                with mock.patch.object(T.os, "execv") as execv, self.assertRaises(SystemExit):
                    T.cmd_run(a, [])
                execv.assert_not_called()


DOCS_INDEX = os.path.join(T.DOCS, "index.html")


def tree_digest(root):
    """sha256 of every file under ROOT, by relative path."""
    out = {}
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root)] = hashlib.sha256(get(p)).hexdigest()
    return out


LATEST = os.path.join(T.DOCS, "firmware", "latest")


def esp_image(segments, digest=True, chip=9):
    """An ESP32 image as esptool 4.9.0 writes one (bin_image.py ESP32FirmwareImage.save): the header
    (magic, segment count, flash mode, size/freq, entry), the extended header (WP pin, drive settings,
    chip id, revisions, reserved, the digest flag), the segments, padding with the checksum byte in the
    last of 16 (align_file_position), and the SHA-256 digest of all that."""
    out = bytearray(b"\xe9" + bytes([len(segments), 2, 0x2F]) + (0x40375000).to_bytes(4, "little"))
    out += bytes([0xEE, 0, 0, 0]) + chip.to_bytes(2, "little") + b"\0" * 9 + bytes([1 if digest else 0])
    checksum = 0xEF
    for addr, data in segments:
        out += addr.to_bytes(4, "little") + len(data).to_bytes(4, "little") + data
        for b in data:
            checksum ^= b
    out += b"\0" * (15 - len(out) % 16) + bytes([checksum])
    if digest:
        out += hashlib.sha256(out).digest()
    return bytes(out)


def merged(boot, table, ota, app, app_at=0x10000):
    """A merged image as release.py merge_segments makes one: the parts at their offsets, 0xFF between."""
    img = bytearray(b"\xff" * (app_at + len(app)))
    for at, part in ((0, boot), (0x8000, table), (0xE000, ota), (app_at, app)):
        img[at:at + len(part)] = part
    return bytes(img)


class TwinFlasherPage(unittest.TestCase):
    """The twin's copy of the web flasher (twin.py build_web): docs/ is only read."""

    # docs/index.html since main c9acf5f: ESP Web Tools pinned to 10.4.0 (before it: @10/)
    PAGE = ('<!doctype html>\n<html>\n<head>\n  <meta charset="utf-8">\n  <title>Flasher</title>\n'
            '  <script type="module" src="https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"></script>\n'
            '</head>\n<body>\n<p>x</p>\n</body>\n</html>\n')
    BOOT = esp_image([(0x403C9000, b"boot" * 1000), (0x3FCE3000, b"data" * 17)])
    APP_IMAGE = esp_image([(0x3C000020, APP[0x20:0x120]), (0x42000020, b"text" * 5000)])
    PARTS = (BOOT, PARTITIONS.ljust(0xC00, b"\xff"), T.BOOT_APP0, APP_IMAGE)

    def test_the_four_edits(self):
        out = T.flasher_index(self.PAGE)
        self.assertIn('<meta charset="utf-8">\n  <script src="twin-serial.js"></script>\n'
                      '  <script src="twin-lang.js"></script>', out)
        self.assertIn("esp-web-tools@10.4.0/dist/web/install-button.js", out)
        self.assertEqual(out.count("esp-web-tools@"), 1)
        self.assertIn("<title>Twin · Flasher</title>", out)          # English until the note's script runs
        self.assertIn("<body>\n" + T.BANNER, out)
        # the shim and the language are classic scripts ahead of the ESP Web Tools module
        self.assertLess(out.index('src="twin-serial.js"'), out.index('type="module"'))
        self.assertLess(out.index('src="twin-lang.js"'), out.index('type="module"'))

    def test_esp_web_tools_is_the_shims_whatever_10_x_the_page_asks_for(self):
        # @10/ before main c9acf5f, @10.4.0/ since; another 10.x as well: the shim is checked against 10.4.0
        for asked in ("@10/", "@10.4.0/", "@10.5/", "@10.12.3/"):
            out = T.flasher_index(self.PAGE.replace("@10.4.0/", asked))
            self.assertIn("https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module", out)
            self.assertEqual(out.count("esp-web-tools@"), 1)
        # another major version, a version that is not one, two scripts: the build stops
        for asked in ("@11/", "@11.0.0/", "@1/", "@10.4.0.1/", "@latest/", "@10.4.0-beta/"):
            with self.assertRaises(ValueError, msg=asked):
                T.flasher_index(self.PAGE.replace("@10.4.0/", asked))
        with self.assertRaises(ValueError):
            T.flasher_index(self.PAGE.replace("</head>", '<script src="https://unpkg.com/esp-web-tools@10/x.js"></script></head>'))

    def test_the_note_speaks_both_languages(self):
        en, ru = T.BANNER_TEXT["en"], T.BANNER_TEXT["ru"]
        self.assertEqual(set(en), set(ru))
        # the Russian words are the proofread ones the note had before it learned English
        self.assertEqual(ru["note"], "Копия прошивальщика для виртуального двойника: Install → «Двойник» прошивает "
                                     "эмулятор на этом Mac, «Плата по USB» — настоящую плату.")
        self.assertEqual((ru["panel"], ru["title"]), ("Панель двойника", "Двойник · "))
        # as the page starts: English, EN pressed, the panel link in English; the script carries both
        self.assertIn(f'<span data-twin="note">{en["note"]}</span>', T.BANNER)
        self.assertIn(f'href="../panel.html?lang=en" style="color:#f0b429;white-space:nowrap">{en["panel"]}</a>', T.BANNER)
        self.assertIn('data-lang="en" lang="en" title="English" aria-pressed="true"', T.BANNER)
        self.assertIn('data-lang="ru" lang="ru" title="Русский" aria-pressed="false"', T.BANNER)
        self.assertIn(">EN</button>", T.BANNER)
        self.assertIn(">RU</button>", T.BANNER)
        self.assertIn("'../panel.html?lang=' + lang", T.BANNER)
        self.assertIn(ru["note"], T.BANNER)
        self.assertEqual(T.BANNER.count("<script>"), 1)
        self.assertEqual(T.BANNER.count("</script>"), 1)                 # nothing in the texts ends it early
        # no anchor of a later edit inside an earlier replacement
        for i, (_, new) in enumerate(T.FLASHER_EDITS):
            for old, _ in T.FLASHER_EDITS[i + 1:]:
                if isinstance(old, re.Pattern):
                    self.assertIsNone(old.search(new))
                else:
                    self.assertNotIn(old, new)

    def test_the_chooser_names_what_the_note_names(self):
        with open(os.path.join(T.FLASHER_SRC, "twin-serial.js"), encoding="utf-8") as f:
            js = f.read()
        for lang, twin, board in (("en", "Twin", "Board over USB"), ("ru", "Двойник", "Плата по USB")):
            note = T.BANNER_TEXT[lang]["note"]
            self.assertIn(twin, note)
            self.assertIn(board, note)
            self.assertIn(f"twin: '{twin} — ", js)
            self.assertIn(f"native: '{board}…'", js)

    def test_an_anchor_missing_or_twice_stops_the_build(self):
        for page in (self.PAGE.replace("<title>", "<title lang=en>"),
                     self.PAGE.replace("@10.4.0/", "@11.0.0/"),
                     self.PAGE.replace("<body>", "<body><body>"),
                     self.PAGE + '<meta charset="utf-8">'):
            with self.assertRaises(ValueError):
                T.flasher_index(page)

    @unittest.skipUnless(os.path.exists(DOCS_INDEX), "no docs/index.html")
    def test_the_public_page_takes_the_edits(self):
        out = T.flasher_index(get(DOCS_INDEX).decode("utf-8"))
        self.assertEqual(out.count("twin-serial.js"), 1)
        self.assertEqual(out.count("twin-lang.js"), 1)
        self.assertEqual(out.count("esp-web-tools@10.4.0/"), 1)
        self.assertLess(out.index("twin-serial.js"), out.index("esp-web-tools@"))
        self.assertLess(out.index("twin-lang.js"), out.index("esp-web-tools@"))
        self.assertEqual(out.count("<title>Twin · "), 1)

    def make_engine(self, d):
        engine = os.path.join(d, "engine")
        os.makedirs(os.path.join(engine, "web", "assets"))
        put(os.path.join(engine, "web", "panel.html"), "<html>panel</html>")
        put(os.path.join(engine, "web", "assets", "a.png"), b"\x89PNG")
        return engine

    @unittest.skipUnless(os.path.exists(os.path.join(T.DOCS, "firmware", "latest", "VERSION")), "no docs/firmware/latest")
    def test_build_web_from_the_release(self):
        with tempfile.TemporaryDirectory() as d:
            before = tree_digest(T.DOCS)
            dest = os.path.join(d, "web")
            with mock.patch.object(T, "ENGINE", self.make_engine(d)):
                version = T.build_web(dest)
                T.build_web(dest)                                   # made again over its own output
            self.assertEqual(tree_digest(T.DOCS), before)           # docs/ untouched
            self.assertEqual(version, get(os.path.join(T.DOCS, "firmware", "latest", "VERSION")).decode().strip())
            fl = os.path.join(dest, "flasher")
            self.assertEqual(get(os.path.join(dest, "panel.html")), b"<html>panel</html>")
            self.assertTrue(os.path.exists(os.path.join(dest, T.GENERATED)))
            for name in ("flasher.js", "styles.css"):
                self.assertEqual(get(os.path.join(fl, name)), get(os.path.join(T.DOCS, name)))
            for name in ("twin-serial.js", "twin-lang.js", "selftest.html"):
                self.assertEqual(get(os.path.join(fl, name)), get(os.path.join(T.FLASHER_SRC, name)))
            self.assertEqual(tree_digest(os.path.join(fl, "img")), tree_digest(os.path.join(T.DOCS, "img")))
            fw = os.path.join(fl, "firmware", "latest")
            names = [f"AnimatedPixelClock-waveshare-{version}-bootloader.bin", f"AnimatedPixelClock-waveshare-{version}-partitions.bin",
                     f"AnimatedPixelClock-waveshare-{version}-otadata.bin", f"OTA_ONLY_firmware-{version}-waveshare.bin"]
            # the files the page asks for (docs/flasher.js buildManifest), as the release has them; no Full.bin
            self.assertEqual(sorted(os.listdir(fw)), sorted(names + ["SHA256SUMS.txt", "VERSION"]))
            for name in names:
                self.assertEqual(get(os.path.join(fw, name)), get(os.path.join(LATEST, name)), name)
            sums = dict(reversed(line.split("  ")) for line in get(os.path.join(fw, "SHA256SUMS.txt")).decode().splitlines())
            self.assertEqual(sorted(sums), sorted(names))
            release = T.release_sums(LATEST)
            for name in names:
                self.assertEqual(sums[name], release[name])
            self.assertEqual(get(os.path.join(fw, "VERSION")).decode().strip(), version)
            self.assertEqual(get(os.path.join(fl, "index.html")).decode("utf-8"), T.flasher_index(get(DOCS_INDEX).decode("utf-8")))
            self.assertEqual(get(os.path.join(dest, "screens.json")), get(T.SCREENS))   # beside panel.html

    def test_build_web_never_removes_a_directory_it_did_not_make(self):
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "web")
            os.makedirs(dest)
            put(os.path.join(dest, "mine.txt"), "keep")
            with mock.patch.object(T, "ENGINE", self.make_engine(d)), self.assertRaises(SystemExit):
                T.build_web(dest)
            self.assertEqual(get(os.path.join(dest, "mine.txt")), b"keep")

    def test_another_image(self):
        with tempfile.TemporaryDirectory() as d:
            img = merged(*self.PARTS)
            good = os.path.join(d, "merged.bin")
            put(good, img)
            self.assertEqual(T.flasher_firmware(good, "v9.9.9-test"), (good, "v9.9.9-test"))
            dest = os.path.join(d, "web")
            with mock.patch.object(T, "ENGINE", self.make_engine(d)):
                T.build_web(dest, good, "v9.9.9-test")
            self.assertEqual(get(os.path.join(dest, "screens.json")), get(T.SCREENS))
            latest = os.path.join(dest, "flasher", "firmware", "latest")
            names = ["AnimatedPixelClock-waveshare-v9.9.9-test-bootloader.bin", "AnimatedPixelClock-waveshare-v9.9.9-test-partitions.bin",
                     "AnimatedPixelClock-waveshare-v9.9.9-test-otadata.bin", "OTA_ONLY_firmware-v9.9.9-test-waveshare.bin"]
            self.assertEqual(sorted(os.listdir(latest)), sorted(names + ["SHA256SUMS.txt", "VERSION"]))
            for name, part in zip(names, self.PARTS):
                self.assertEqual(get(os.path.join(latest, name)), part, name)
            self.assertEqual(get(os.path.join(latest, "VERSION")), b"v9.9.9-test\n")
            self.assertIn(f"{hashlib.sha256(self.APP_IMAGE).hexdigest()}  OTA_ONLY_firmware-v9.9.9-test-waveshare.bin\n",
                          get(os.path.join(latest, "SHA256SUMS.txt")).decode())
            app = os.path.join(d, "firmware.bin")
            put(app, APP)
            for image, version in ((good, None), (good, "v1/../x"), (good, "v 1"), (app, "v1"), (None, "v1")):
                with self.assertRaises(SystemExit):
                    T.flasher_firmware(image, version)

    def test_image_length_is_esptools(self):
        # the checksum byte closes a 16-byte block, a whole block of padding when the segments end on one
        for n in range(0, 40):
            for digest in (True, False):
                image = esp_image([(0x40370000, bytes(range(n % 256)) * 1)] * 2, digest=digest)
                self.assertEqual(T.image_length(image + b"\xff" * 64), len(image), (n, digest))
                self.assertEqual(len(image) % 16, 0)
                self.assertIsNone(T.image_length(image[:-1]))                   # runs past the end
        self.assertEqual(T.image_length(b"\xff" * 16 + self.APP_IMAGE, 16), len(self.APP_IMAGE))
        self.assertIsNone(T.image_length(esp_image([(0, b"x")], chip=0)))     # an ESP32, not an ESP32-S3
        self.assertIsNone(T.image_length(APP))                                  # no chip id
        self.assertIsNone(T.image_length(b"\xff" * 64))

    def test_merged_parts_refuses_another_layout(self):
        self.assertEqual(T.merged_parts(merged(*self.PARTS), "x"), list(self.PARTS))
        other_ota = TABLE.replace((0xE000).to_bytes(4, "little"), (0xD000).to_bytes(4, "little"))
        big_boot = esp_image([(0x403C9000, b"b" * 0x8000)])
        for img in (merged(self.PARTS[0], other_ota, *self.PARTS[2:]),              # otadata elsewhere
                    merged(b"\xe9" + b"\0" * 100, *self.PARTS[1:]),               # no bootloader image
                    merged(big_boot, *self.PARTS[1:])[:0x8000] + merged(*self.PARTS)[0x8000:],   # into the table
                    merged(*self.PARTS)[:-1],                                         # the app cut short
                    merged(*self.PARTS[:3], esp_image([(0, b"a" * 0x480001)]))):      # bigger than app0
            with self.assertRaises(SystemExit):
                T.merged_parts(img, "x")

    @unittest.skipUnless(os.path.exists(os.path.join(LATEST, "VERSION")), "no docs/firmware/latest")
    def test_the_release_parts_are_its_full_image_cut(self):
        # what the page writes (the release's part files) is what a new chip of the engine starts from
        version = get(os.path.join(LATEST, "VERSION")).decode().strip()
        full = get(os.path.join(LATEST, f"AnimatedPixelClock-waveshare-{version}-Full.bin"))
        v, parts = T.flasher_parts()
        self.assertEqual(v, version)
        self.assertEqual([p[0] for p in T.FLASHER_PARTS], [0x0, 0x8000, 0xE000, T.APP0])
        for (offset, _, _), (name, data) in zip(T.FLASHER_PARTS, parts):
            self.assertEqual(data, get(os.path.join(LATEST, name)), name)
            self.assertEqual(full[offset:offset + len(data)], data, name)
        # and nothing else of Full.bin is lost: between the parts it is blank
        covered = bytearray(len(full))
        for (offset, _, _), (_, data) in zip(T.FLASHER_PARTS, parts):
            covered[offset:offset + len(data)] = b"\1" * len(data)
        self.assertEqual({b for b, c in zip(full, covered) if not c}, {0xFF})

    @unittest.skipUnless(os.path.exists(os.path.join(LATEST, "VERSION")), "no docs/firmware/latest")
    def test_a_release_file_not_its_checksum_stops_the_build_before_the_copy_changes(self):
        with tempfile.TemporaryDirectory() as d:
            docs = os.path.join(d, "docs")
            shutil.copytree(T.DOCS, docs, ignore=shutil.ignore_patterns("oceanarium", "*.md", "*.svg"))
            latest = os.path.join(docs, "firmware", "latest")
            version = get(os.path.join(latest, "VERSION")).decode().strip()
            dest = os.path.join(d, "web")
            with mock.patch.object(T, "DOCS", docs), mock.patch.object(T, "ENGINE", self.make_engine(d)):
                T.build_web(dest)
                before = tree_digest(dest)
                ota = os.path.join(latest, f"AnimatedPixelClock-waveshare-{version}-otadata.bin")
                good = get(ota)
                bad = bytearray(good)
                bad[5] ^= 1
                put(ota, bytes(bad))
                with self.assertRaises(SystemExit) as e:                    # not its checksum
                    T.build_web(dest)
                self.assertIn("not the checksum in SHA256SUMS.txt", str(e.exception.code))
                sums = get(os.path.join(latest, "SHA256SUMS.txt")).decode()
                put(os.path.join(latest, "SHA256SUMS.txt"),
                    sums.replace(hashlib.sha256(good).hexdigest(), hashlib.sha256(bytes(bad)).hexdigest()))
                with self.assertRaises(SystemExit) as e:                    # its checksum, not Full.bin's part
                    T.build_web(dest)
                self.assertIn("the release's files disagree", str(e.exception.code))
                put(os.path.join(latest, "SHA256SUMS.txt"), sums)
                put(ota, good)
                os.remove(os.path.join(latest, f"OTA_ONLY_firmware-{version}-waveshare.bin"))
                with self.assertRaises(SystemExit) as e:                    # a part missing
                    T.build_web(dest)
                self.assertIn("is missing", str(e.exception.code))
                self.assertEqual(tree_digest(dest), before)                 # the copy made before is untouched
                full = os.path.join(latest, f"AnimatedPixelClock-waveshare-{version}-Full.bin")
                put(full, get(full)[:-1] + b"\0")
                with self.assertRaises(SystemExit):                         # a new chip's image: its checksum too
                    T.flasher_firmware()

    def test_flasher_js_asks_for_the_parts_the_twin_serves(self):
        js = get(os.path.join(T.DOCS, "flasher.js")).decode("utf-8")
        T.flasher_js_check(js)
        for changed in (js.replace("part('otadata')", "part('ota_data')"),                       # another name
                        js.replace("offset: 0xE000", "offset: 0xD000"),                          # another offset
                        js.replace("{ path: part('otadata'), offset: 0xE000 },", ""),            # a part fewer
                        js.replace("{ path: part('bootloader'), offset: 0x0 },",                 # a part more
                                   "{ path: part('bootloader'), offset: 0x0 },\n{ path: part('nvs'), offset: 0x9000 },"),
                        js.replace("firmware: 'waveshare',", "firmware: 'waveshare-s3',"),        # another firmware id
                        js.replace("-${label}.bin", "_${label}.bin"),                              # names made otherwise
                        js.replace("`firmware/latest/${name}`", "`firmware/${name}`")):           # another directory
            with self.assertRaises(ValueError):
                T.flasher_js_check(changed)

    def run_args(self, d, **kw):
        flash = os.path.join(d, "flash.bin")
        a = dict(fresh=False, blank=False, lan=False, provision=False, web=None, seconds=None, png=None, cpi="2.45", open=False,
                 http=8080, udp=4210, mac=T.MAC, flasher_image=None, flasher_version=None)
        a.update(kw)
        with mock.patch.object(T, "STATE", d), mock.patch.object(T, "FLASH", flash), mock.patch.object(T, "FW", os.path.join(d, "fw")), \
                mock.patch.object(T, "WIFI", os.path.join(d, "wifi.txt")), mock.patch.object(T, "WEB", os.path.join(d, "web")), \
                mock.patch.object(T, "build_web", return_value="v2.7.3") as build, \
                mock.patch.object(T.os, "execv") as execv, contextlib.redirect_stderr(io.StringIO()) as err:
            T.cmd_run(mock.Mock(**a), [])
        return execv.call_args[0][1], build, err.getvalue()

    def test_run_with_web_serves_the_generated_pages(self):
        with tempfile.TemporaryDirectory() as d:
            args, build, err = self.run_args(d, web=18790)
            build.assert_called_once_with(os.path.join(d, "web"), None, None)
            self.assertEqual(args[args.index("--web-dir") + 1], os.path.join(d, "web"))
            self.assertIn("http://127.0.0.1:18790/flasher/index.html", err)
            self.assertIn("--flash-image", args)
            args, build, _ = self.run_args(d)
            build.assert_not_called()
            self.assertNotIn("--web-dir", args)

    def test_blank_is_an_empty_new_chip(self):
        with tempfile.TemporaryDirectory() as d:
            put(os.path.join(d, "flash.bin"), b"\0" * 16)
            args, _, _ = self.run_args(d, blank=True, web=18790)
            self.assertFalse(os.path.exists(os.path.join(d, "flash.bin")))
            self.assertNotIn("--flash-image", args)
            self.assertEqual(args[args.index("--flash-persist") + 1], os.path.join(d, "flash.bin"))

    def test_a_new_chip_starts_from_the_release(self):
        with tempfile.TemporaryDirectory() as d:
            args, _, err = self.run_args(d)
            self.assertEqual(args[args.index("--flash-image") + 1], T.flasher_firmware()[0])
            self.assertNotIn("--elf", args)                                # no ELF under d/fw
            self.assertIn("no symbols", err)

    def test_run_takes_the_elf_of_the_chip(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "fw", "v9"))
            put(os.path.join(d, "fw", "v9", "firmware.elf"), b"\x7fELF nine")
            img = bytearray(b"\xff" * 0x20000)
            img[0x8000:0x8000 + len(TABLE)] = TABLE
            img[0x10000:0x10000 + len(APP)] = with_elf(b"\x7fELF nine")
            put(os.path.join(d, "flash.bin"), img)
            args, _, err = self.run_args(d)
            self.assertEqual(args[args.index("--elf") + 1], os.path.join(d, "fw", "v9", "firmware.elf"))
            self.assertNotIn("no symbols", err)
            with mock.patch.object(T, "ELF", "/given.elf"):                # TWIN_ELF wins
                args, _, _ = self.run_args(d)
            self.assertEqual(args[args.index("--elf") + 1], "/given.elf")

    def test_flasher_image_needs_web(self):
        with tempfile.TemporaryDirectory() as d, self.assertRaises(SystemExit):
            self.run_args(d, flasher_image="x.bin", flasher_version="v1")

    def test_verify_judges_only_what_is_not_data(self):
        with tempfile.TemporaryDirectory() as d:
            img = bytearray(b"\xff" * 0x20000)
            img[0] = 0xE9
            img[0x8000:0x8000 + len(TABLE)] = TABLE
            img[0x10000:0x10000 + len(APP)] = APP
            image, flash = os.path.join(d, "merged.bin"), os.path.join(d, "flash.bin")
            put(image, bytes(img))
            chip = bytearray(img) + b"\xff" * 0x10000
            chip[0x9000:0x9010] = b"\0" * 16                        # nvs written by the firmware
            put(flash, bytes(chip))
            with mock.patch.object(T, "FLASH", flash):
                with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit) as e:
                    T.cmd_verify(mock.Mock(image=image), [])
                self.assertEqual(e.exception.code, 0)
                self.assertIn("nvs: 16 bytes differ", out.getvalue())
                chip[0x10100] ^= 1                                    # one bit of the app
                chip[0x1000] = 0                                      # and the bootloader
                put(flash, bytes(chip))
                with contextlib.redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit) as e:
                    T.cmd_verify(mock.Mock(image=image), [])
                self.assertEqual(e.exception.code, 1)
                self.assertIn("DIFFERENT 0x10100..0x10101 (1 bytes) in app0", out.getvalue())
                self.assertIn("DIFFERENT 0x1000..0x1001 (1 bytes) in no partition (bootloader, table)", out.getvalue())

    def test_differing_ranges(self):
        a = bytes(10000)
        b = bytearray(a)
        b[5] = b[6] = 1
        b[4096] = 1
        b[9999] = 1
        self.assertEqual(T.differing(a, bytes(b)), [(5, 7), (4096, 4097), (9999, 10000)])
        self.assertEqual(T.differing(a, a), [])


REPO = os.path.dirname(os.path.dirname(HERE))
# The header of a script, as the panel page reads it (gallery/README.md, "What a screen says about
# itself"): the lines from the top up to the first one that is neither blank nor a `--` comment, and in
# them `-- @<tag>.<lang> <text>`.
HEADER_TAG = re.compile(r"^--\s*@(name|about|control|function)\.([a-z]{2})\s+(.*\S)\s*$")
CONTROL = re.compile(r"^(knob press(?: x[2-9])?): (.+)$")


def header_tags(text):
    tags = []
    for line in text.splitlines():
        s = line.strip()
        if s and not s.startswith("--"):
            break
        m = HEADER_TAG.match(s)
        if m:
            tags.append(m.groups())
    return tags


class TwinScreenHelp(unittest.TestCase):
    """tools/twin/screens.json (the built-in screens) and the gallery scripts' own headers (the Lua
    effects): what the panel page's "This screen" block shows."""

    # the functions of the ten buttons on the page's remote (panel.html BUTTONS, the owner's learned remote)
    REMOTE_FNS = {"power", "home", "carousel", "ccw", "ok", "cw", "bright_down", "long", "bright_up", "media_toggle"}

    @classmethod
    def setUpClass(cls):
        with open(T.SCREENS, encoding="utf-8") as f:
            cls.doc = json.load(f)

    def check_control(self, c, where):
        self.assertTrue(c.get("on") or c.get("remote"), where)
        if "on" in c:
            self.assertEqual(set(c["on"]), {"en", "ru"}, where)
            self.assertTrue(all(c["on"].values()), where)
        for lang in ("en", "ru"):
            self.assertTrue(c[lang].strip(), where)
        self.assertTrue(c["source"].strip(), where)          # every line from the firmware's code
        for fn in c.get("remote", []):
            self.assertIn(fn, self.REMOTE_FNS, where)

    def test_every_screen_in_both_languages_with_its_source(self):
        ids = [s["id"] for s in self.doc["screens"]]
        self.assertEqual(len(ids), len(set(ids)))
        for s in self.doc["screens"]:
            for field in ("name", "what"):
                self.assertEqual(set(s[field]), {"en", "ru"}, s["id"])
                self.assertTrue(all(s[field].values()), s["id"])
            self.assertEqual(len(s["functions"]["en"]), len(s["functions"]["ru"]), s["id"])
            self.assertTrue(s["source"].strip(), s["id"])
            self.assertIsInstance(s["match"], dict, s["id"])
            for c in s["controls"]:
                self.check_control(c, s["id"])
        for c in self.doc["global_controls"]:
            self.check_control(c, "global")
        for name, e in self.doc["effects"].items():
            self.assertEqual(name, name.upper())                 # as the panel names an effect
            for c in e["controls"]:
                self.check_control(c, name)
            self.assertEqual(len(e["functions"]["en"]), len(e["functions"]["ru"]), name)

    def test_the_first_match_is_the_right_one(self):
        # the page takes the first screen whose match fits now: a catch-all never hides a narrower one
        def first(now):
            for s in self.doc["screens"]:
                if all(now.get(k) == v for k, v in s["match"].items()):
                    return s["id"]
        clock = {"key": "clock", "mode": "clock", "style": 0}
        self.assertEqual(first(clock), "clock")
        self.assertEqual(first(dict(clock, style=14)), "weather")
        self.assertEqual(first(dict(clock, mode="ambient", style=14)), "ambient")
        self.assertEqual(first(dict(clock, mode="ambient", clip=True)), "clip")
        self.assertEqual(first(dict(clock, mode="viz")), "viz")
        self.assertEqual(first(dict(clock, mode="metrics")), "metrics")
        self.assertEqual(first(dict(clock, notify=True)), "notify")
        self.assertEqual(first({"key": "market", "name": "PORTFOLIO"}), "portfolio")
        self.assertEqual(first({"key": "lua", "name": "LASER CLOCK"}), "lua")
        for key in ("world", "flights", "trains", "yachts", "media", "cards"):
            self.assertEqual(first({"key": key}), key)
        # a Lua page's button line is the one a script's own "knob press" replaces
        lua = next(s for s in self.doc["screens"] if s["id"] == "lua")
        self.assertEqual([c.get("button", False) for c in lua["controls"]].count(True), 1)

    @unittest.skipUnless(os.path.exists(os.path.join(T.ENGINE, "web", "panel.html")), "no engine")
    def test_the_remote_is_the_pages(self):
        with open(os.path.join(T.ENGINE, "web", "panel.html"), encoding="utf-8") as f:
            page = f.read()
        self.assertEqual(set(re.findall(r"fn: '([a-z_]+)'", page)), self.REMOTE_FNS)

    def test_every_gallery_script_says_what_it_is(self):
        gallery = os.path.join(REPO, "gallery")
        luasim = os.path.join(REPO, "tools", "luasim", "scripts")
        scripts = [os.path.join(gallery, n) for n in sorted(os.listdir(gallery)) if n.endswith(".lua")]
        self.assertGreater(len(scripts), 20)
        scripts += [os.path.join(luasim, n) for n in sorted(os.listdir(luasim)) if n.endswith(".lua")]
        tagged = 0
        for path in scripts:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            tags = header_tags(text)
            # a tag anywhere else is a mistake: the page would never read it
            self.assertEqual(len(tags), len(re.findall(r"(?m)^--\s*@(?:name|about|control|function)\.", text)), path)
            if not tags and not path.startswith(gallery):
                continue                                      # luasim's test and demo scripts
            tagged += 1
            by = {}
            for tag, lang, value in tags:
                by.setdefault((tag, lang), []).append(value)
            for tag in ("name", "about"):
                for lang in ("en", "ru"):
                    self.assertIn((tag, lang), by, f"{path}: no @{tag}.{lang}")
            self.assertEqual(len(by[("name", "en")]), 1, path)
            controls = {lang: [CONTROL.match(v) for v in by.get(("control", lang), [])] for lang in ("en", "ru")}
            for lang, ms in controls.items():
                self.assertTrue(all(ms), f"{path}: @control.{lang} is '<knob press[ xN]>: <text>'")
            # the same actions in both languages, in the same order
            self.assertEqual([m.group(1) for m in controls["en"]], [m.group(1) for m in controls["ru"]], path)
            self.assertTrue(controls["en"], f"{path}: say what the knob press does, even if nothing")
            # a script that never reads its button says so
            if 'rawget(px, "button")' not in text and "px.button(" not in text:
                self.assertEqual([m.group(2) for m in controls["en"]],
                                 ["Nothing: this effect does not use the button."], path)
            self.assertEqual(len(by.get(("function", "en"), [])), len(by.get(("function", "ru"), [])), path)
        self.assertGreater(tagged, 25)


class Calibrate(unittest.TestCase):
    # finding 13: a fractional CPI, and errors that land in the results
    def test_flags(self):
        self.assertEqual(C.flags("cpi2.45"), ["--cpi", "2.45"])
        self.assertEqual(C.flags("cpi2"), ["--cpi", "2"])
        self.assertEqual(C.flags("cpi1"), [])
        self.assertEqual(C.flags("cpi1.0"), [])
        for bad in ("cpi0.5", "cpi257", "cpi2_45", "cpix", "cpi", "cpi-2", "bogus"):
            with self.assertRaises(SystemExit):
                C.flags(bad)

    def test_twin_gets_the_fractional_cpi_and_the_open_ap(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "state"))
            put(os.path.join(d, "state", "flash.bin"), b"\xff" * 16)
            put(os.path.join(d, "state", "wifi.txt"), "Guest\n\n")
            with mock.patch.object(C, "HOME", d), mock.patch.object(C.subprocess, "Popen") as popen:
                C.Twin("cpi2.45", 18080, d)
            args = popen.call_args[0][0]
            self.assertEqual(args[args.index("--cpi") + 1], "2.45")
            self.assertEqual(args[args.index("--wifi") + 1], "ssid=Guest")
            self.assertEqual(args[args.index("--flash-image") + 1], T.flasher_firmware()[0])   # not a hard-coded build

    def test_run_records_errors_instead_of_dying(self):
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()):
            results = {}
            with mock.patch.object(C, "Twin", side_effect=FileNotFoundError("state/flash.bin")) as twin:
                C.run("cpi2.45", 18080, d, results)
                C.run("cpibad", 18081, d, results)
            self.assertEqual(twin.call_count, 1)                   # a bad name never starts an emulator
            for name in ("cpi2.45", "cpibad"):
                self.assertIn("error", results[name])
                self.assertEqual(json.loads(get(os.path.join(d, f"{name}.json")))["config"], name)
            self.assertEqual(results["cpi2.45"]["flags"], ["--cpi", "2.45"])

    def test_main_stops_on_a_bad_name_before_any_thread(self):
        with mock.patch.object(sys, "argv", ["calibrate.py", "--configs", "cpi2.45,cpi2_45", "--out", tempfile.gettempdir()]), \
                mock.patch.object(C.threading, "Thread") as thread, self.assertRaises(SystemExit):
            C.main()
        thread.assert_not_called()


if __name__ == "__main__":
    unittest.main()

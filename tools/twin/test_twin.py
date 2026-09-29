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
                    mock.patch.object(T, "FLASH", os.path.join(d, "flash.bin")):
                with self.assertRaises(SystemExit):
                    T.cmd_wifi(mock.Mock(ssid="Home", password="pa,ss"), [])
                self.assertFalse(os.path.exists(wifi))
                with contextlib.redirect_stdout(io.StringIO()):
                    T.cmd_wifi(mock.Mock(ssid="Guest", password=""), [])
                a = mock.Mock(fresh=False, blank=False, provision=True, web=None, seconds=None, png=None, cpi="2.45",
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


class TwinFlasherPage(unittest.TestCase):
    """The twin's copy of the web flasher (twin.py build_web): docs/ is only read."""

    PAGE = ('<!doctype html>\n<html>\n<head>\n  <meta charset="utf-8">\n  <title>Flasher</title>\n'
            '  <script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>\n'
            '</head>\n<body>\n<p>x</p>\n</body>\n</html>\n')

    def test_the_four_edits(self):
        out = T.flasher_index(self.PAGE)
        self.assertIn('<meta charset="utf-8">\n  <script src="twin-serial.js"></script>', out)
        self.assertIn("esp-web-tools@10.4.0/dist/web/install-button.js", out)
        self.assertNotIn("esp-web-tools@10/", out)
        self.assertIn("<title>Двойник · Flasher</title>", out)
        self.assertIn("<body>\n" + T.BANNER, out)
        # the shim is a classic script ahead of the ESP Web Tools module
        self.assertLess(out.index('src="twin-serial.js"'), out.index('type="module"'))

    def test_an_anchor_missing_or_twice_stops_the_build(self):
        for page in (self.PAGE.replace("<title>", "<title lang=en>"),
                     self.PAGE.replace("@10/", "@11/"),
                     self.PAGE.replace("<body>", "<body><body>"),
                     self.PAGE + '<meta charset="utf-8">'):
            with self.assertRaises(ValueError):
                T.flasher_index(page)

    @unittest.skipUnless(os.path.exists(DOCS_INDEX), "no docs/index.html")
    def test_the_public_page_takes_the_edits(self):
        out = T.flasher_index(get(DOCS_INDEX).decode("utf-8"))
        self.assertEqual(out.count("twin-serial.js"), 1)
        self.assertEqual(out.count("esp-web-tools@10.4.0/"), 1)
        self.assertLess(out.index("twin-serial.js"), out.index("esp-web-tools@"))

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
            for name in ("twin-serial.js", "selftest.html"):
                self.assertEqual(get(os.path.join(fl, name)), get(os.path.join(T.FLASHER_SRC, name)))
            self.assertEqual(tree_digest(os.path.join(fl, "img")), tree_digest(os.path.join(T.DOCS, "img")))
            bin_name = f"AnimatedPixelClock-{T.FIRMWARE_ID}-{version}-Full.bin"
            self.assertEqual(get(os.path.join(fl, "firmware", "latest", bin_name)),
                             get(os.path.join(T.DOCS, "firmware", "latest", bin_name)))
            self.assertEqual(get(os.path.join(fl, "firmware", "latest", "VERSION")).decode().strip(), version)
            self.assertEqual(get(os.path.join(fl, "index.html")).decode("utf-8"), T.flasher_index(get(DOCS_INDEX).decode("utf-8")))

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
            img = bytearray(b"\xff" * 0x9000)
            img[0] = 0xE9
            img[0x8000:0x8000 + len(TABLE)] = TABLE
            good = os.path.join(d, "merged.bin")
            put(good, bytes(img))
            self.assertEqual(T.flasher_firmware(good, "v9.9.9-test"), (good, "v9.9.9-test"))
            dest = os.path.join(d, "web")
            with mock.patch.object(T, "ENGINE", self.make_engine(d)):
                T.build_web(dest, good, "v9.9.9-test")
            latest = os.path.join(dest, "flasher", "firmware", "latest")
            self.assertEqual(get(os.path.join(latest, "AnimatedPixelClock-waveshare-v9.9.9-test-Full.bin")), bytes(img))
            self.assertEqual(get(os.path.join(latest, "VERSION")), b"v9.9.9-test\n")
            app = os.path.join(d, "firmware.bin")
            put(app, APP)
            for image, version in ((good, None), (good, "v1/../x"), (good, "v 1"), (app, "v1"), (None, "v1")):
                with self.assertRaises(SystemExit):
                    T.flasher_firmware(image, version)

    def run_args(self, d, **kw):
        flash = os.path.join(d, "flash.bin")
        a = dict(fresh=False, blank=False, provision=False, web=None, seconds=None, png=None, cpi="2.45", open=False,
                 http=8080, udp=4210, mac=T.MAC, flasher_image=None, flasher_version=None)
        a.update(kw)
        with mock.patch.object(T, "STATE", d), mock.patch.object(T, "FLASH", flash), \
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

    def test_flasher_image_needs_web(self):
        with tempfile.TemporaryDirectory() as d, self.assertRaises(SystemExit):
            self.run_args(d, flasher_image="x.bin", flasher_version="v1")


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

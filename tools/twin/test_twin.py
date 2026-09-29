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
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import twin as T  # noqa: E402

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
                a = mock.Mock(fresh=False, provision=True, web=None, seconds=None, png=None, cpi="2.45",
                              open=False, http=8080, udp=4210, mac=T.MAC)
                with mock.patch.object(T.os, "execv") as execv:
                    T.cmd_run(a, [])
                args = execv.call_args[0][1]
                self.assertEqual(args[args.index("--wifi") + 1], "ssid=Guest")
                self.assertEqual(args[args.index("--serial-hex") + 1], T.improv_hex("Guest", ""))
                put(wifi, "Home\npa,ss\n")                                # edited by hand
                with mock.patch.object(T.os, "execv") as execv, self.assertRaises(SystemExit):
                    T.cmd_run(a, [])
                execv.assert_not_called()


if __name__ == "__main__":
    unittest.main()

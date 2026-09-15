#!/usr/bin/env python3
"""Offline tests of repartition_32mb.py: a synthetic default_16MB.csv flash dump, a fake
esptool/pio that act on it in memory, and no subprocess at all.

  uv run --with littlefs-python==0.19.0 python -m unittest tools/flash/test_repartition_32mb.py -v
"""
import contextlib
import io
import json
import pathlib
import posixpath
import random
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import repartition_32mb as R  # noqa: E402

import littlefs  # noqa: E402

FRAMEWORK = R.PIO_HOME / "packages" / "framework-arduinoespressif32"
ENV = "matrix-waveshare-rgb"
PORT = "/dev/fake-panel-port"          # never opened: every command goes to FakeBench
KEEP = ["--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]
PIO_ARGS = ["--flash_mode", "dout", "--flash_freq", "80m", "--flash_size", "32MB"]

rng = random.Random(20260915)
FILES = {
    "/anim/a.pca": rng.randbytes(300 * 1024),
    "/anim/b.pca": rng.randbytes(1024 * 1024),
    "/icons/sun": rng.randbytes(1234),
    "/market/last.bin": rng.randbytes(87 * 1024),
}
ATTRS = [("/anim/a.pca", "t", b"\x10\x32\x54\x66"), ("/market/last.bin", "t", b"\x01\x02\x03\x04"),
         ("/icons", "t", b"\xaa\xbb\xcc\xdd")]
EMPTY_DIRS = ["/empty"]


def make_fs(size, files, attrs=(), dirs=()):
    ctx = littlefs.UserContext(buffer=bytearray(b"\xff" * size))
    fs = littlefs.LittleFS(context=ctx, mount=False, **{**R.LFS_CFG, "block_count": size // R.BLOCK_SIZE})
    fs.format()
    fs.mount()
    for d in dirs:
        fs.makedirs(d, exist_ok=True)
    for path, data in files.items():
        fs.makedirs(posixpath.dirname(path), exist_ok=True)
        with fs.open(path, "wb") as f:
            f.write(data)
    for path, typ, value in attrs:
        fs.setattr(path, typ, value)
    fs.unmount()
    return bytes(ctx.buffer)


def gen():
    return R.Ctx(mock.Mock(framework_dir=str(FRAMEWORK)), None).gen


def table_bin(csv_name):
    text = (FRAMEWORK / "tools" / "partitions" / csv_name).read_text()
    return gen().PartitionTable.from_csv(text).to_binary()


def make_flash(fs_image, csv_name=R.OLD_CSV):
    flash = bytearray(b"\xff" * R.FLASH_SIZE)
    flash[0:4] = b"\xe9\x03\x03\x4f"                      # a bootloader-looking header
    pt = table_bin(csv_name)
    flash[R.PT_OFFSET:R.PT_OFFSET + len(pt)] = pt
    flash[R.NVS_OFFSET:R.NVS_OFFSET + 16] = b"NVS-PLACEHOLDER!"
    flash[0x10000:0x10000 + 1024] = b"\x00" * 1024       # zeros where an app would be
    flash[R.OLD_FS_OFFSET:R.OLD_FS_OFFSET + R.OLD_FS_SIZE] = fs_image
    return flash


class FakeBench:
    """esptool and pio as seen by the tool, acting on an in-memory 32 MB flash."""

    def __init__(self, flash, tools):
        self.flash = flash
        self.tools = tools
        self.calls = []
        self.detected = "32MB"
        self.read_hook = None          # (addr, size, data) -> data
        self.write_hook = None         # (addr, data) -> data
        self.config = [[f"env:{ENV}", [
            ["extends", ["env:matrix-base"]], ["board", "esp32-s3-devkitc-1"],
            ["board_build.arduino.memory_type", "opi_opi"], ["board_upload.flash_size", "32MB"],
            ["board_upload.maximum_size", "33554432"], ["board_build.flash_size", "32MB"],
            ["board_build.partitions", R.NEW_CSV], ["platform", "espressif32@6.12.0"]]]]

    def __call__(self, cmd, cwd=None):
        self.calls.append((list(cmd), cwd))
        if cmd[:2] == [self.tools["python"], self.tools["esptool"]]:
            return self.esptool(cmd[2:])
        if cmd[0] == self.tools["pio"]:
            return self.pio(cmd[1:], cwd)
        raise AssertionError(f"unexpected command {cmd}")

    @staticmethod
    def positional(rest):
        out, i = [], 0
        while i < len(rest):
            if rest[i] in ("--flash_mode", "--flash_freq", "--flash_size"):
                i += 2
            elif rest[i].startswith("-"):
                i += 1
            else:
                out.append(rest[i])
                i += 1
        return out

    def esptool(self, argv):
        i = argv.index("--after") + 2
        op, rest = argv[i], argv[i + 1:]
        if op == "flash_id":
            return 0, f"Manufacturer: c8\nDevice: 4039\nDetected flash size: {self.detected}\n"
        if op == "read_flash":
            addr, size, fn = int(rest[0], 16), int(rest[1], 16), rest[2]
            data = bytes(self.flash[addr:addr + size])
            if self.read_hook:
                data = self.read_hook(addr, size, data)
            pathlib.Path(fn).write_bytes(data)
            return 0, f"Read {size} bytes at {addr:#010x}\n"
        pos = self.positional(rest)
        pairs = [(int(pos[k], 16), pathlib.Path(pos[k + 1]).read_bytes()) for k in range(0, len(pos), 2)]
        if op == "write_flash":
            for addr, data in pairs:
                if self.write_hook:
                    data = self.write_hook(addr, data)
                self.flash[addr:addr + len(data)] = data
            return 0, "Hash of data verified.\n"
        if op == "verify_flash":
            ok = all(bytes(self.flash[a:a + len(d)]) == d for a, d in pairs)
            return (0, "-- verify OK (digest matched)\n") if ok else (2, "-- verify FAILED (digest mismatch)\n")
        raise AssertionError(f"unexpected esptool op {argv}")

    def pio(self, argv, cwd):
        if argv == ["project", "config", "--json-output"]:
            return 0, json.dumps(self.config)
        if argv == ["run", "-e", ENV]:
            return 0, "========== [SUCCESS] ==========\n"
        if argv[:5] == ["run", "-e", ENV, "-t", "upload"]:
            build = pathlib.Path(cwd) / ".pio" / "build" / ENV
            for addr, path in ((0x0, build / "bootloader.bin"), (0x8000, build / "partitions.bin"),
                               (0xE000, FRAMEWORK / "tools" / "partitions" / "boot_app0.bin"),
                               (0x10000, build / "firmware.bin")):
                data = path.read_bytes()
                self.flash[addr:addr + len(data)] = data
            return 0, "========== [SUCCESS] ==========\n"
        raise AssertionError(f"unexpected pio call {argv}")


@unittest.skipUnless((FRAMEWORK / "tools" / "gen_esp32part.py").is_file(), "framework-arduinoespressif32 missing")
class RepartitionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old_fs = make_fs(R.OLD_FS_SIZE, FILES, ATTRS, EMPTY_DIRS)
        cls.old_flash = make_flash(cls.old_fs)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name).resolve()     # /var -> /private/var on macOS
        self.d = self.root / "backup"
        self.project = self.root / "project"
        bindir = self.root / "bin"
        bindir.mkdir()
        self.tools = {}
        for name in ("python", "esptool", "pio"):
            p = bindir / name
            p.write_text("# placeholder: never executed\n")
            self.tools[name] = str(p)
        self.build = self.project / ".pio" / "build" / ENV
        self.build.mkdir(parents=True)
        (self.build / "partitions.bin").write_bytes(table_bin(R.NEW_CSV))
        (self.build / "bootloader.bin").write_bytes(b"\xe9\x03\x03\x5f" + rng.randbytes(14060))
        (self.build / "firmware.bin").write_bytes(b"\xe9\x05\x03\x5f" + rng.randbytes(2 * 1024 * 1024))
        self.bench = FakeBench(bytearray(self.old_flash), self.tools)
        self.guards = [mock.patch.object(subprocess, n, side_effect=AssertionError(f"subprocess.{n} called"))
                       for n in ("Popen", "run", "call", "check_call", "check_output")]
        for g in self.guards:
            g.start()

    def tearDown(self):
        for g in self.guards:
            g.stop()
        self.tmp.cleanup()

    # ── helpers ──
    def cli(self, *argv):
        self.bench.calls.clear()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = R.main([str(a) for a in argv], runner=self.bench)
        self.out = buf.getvalue()
        return rc

    def ok(self, *argv):
        self.assertEqual(self.cli(*argv), 0, self.out)

    def hw(self):
        return ["--port", PORT, "--python", self.tools["python"], "--esptool", self.tools["esptool"],
                "--framework-dir", FRAMEWORK]

    def fw(self):
        return ["--framework-dir", FRAMEWORK]

    def esp(self, before, after, *op):
        return [self.tools["python"], self.tools["esptool"], "--chip", "esp32s3", "--port", PORT,
                "--baud", "921600", "--before", before, "--after", after, *[str(x) for x in op]]

    def commands(self):
        return [c for c, _ in self.bench.calls]

    def backup(self):
        self.ok("backup", "--out", self.d, *self.hw())

    def build_app(self):
        self.ok("build-app", "--in", self.d, "--env", ENV, "--pio", self.tools["pio"],
                "--project-dir", self.project, *self.fw())

    def final_read(self):
        self.ok("final-read", "--in", self.d, *self.hw())

    def extract(self, *extra):
        self.ok("extract", "--in", self.d, *self.fw(), *extra)

    def build_fs(self):
        self.ok("build-fs", "--in", self.d, *self.fw())

    def ready(self):
        self.backup()
        self.build_app()
        self.final_read()
        self.extract()
        self.build_fs()

    def flash_app_args(self):
        return ["flash-app", "--in", self.d, "--env", ENV, *self.hw(), "--pio", self.tools["pio"], "--yes-i-mean-it"]

    def flash_fs_args(self):
        return ["flash-fs", "--in", self.d, *self.hw(), "--yes-i-mean-it"]

    def mount_image(self, image, block_count=0):
        fs = littlefs.LittleFS(context=littlefs.UserContext(buffer=bytearray(image)), mount=False,
                               **{**R.LFS_CFG, "block_count": block_count})
        fs.mount()
        return fs

    def write_hand_files(self, flash=None):
        hand = self.root / "hand"
        hand.mkdir(exist_ok=True)
        flash = bytes(flash if flash is not None else self.old_flash)
        (hand / "flash_full.bin").write_bytes(flash)
        (hand / "verify_app1_1mb.bin").write_bytes(flash[0x650000:0x750000])
        return hand

    # ── backup ──
    def test_backup_commands_and_manifest(self):
        self.backup()
        d = self.d
        self.assertEqual(self.commands(), [
            self.esp("default_reset", "no_reset", "flash_id"),
            self.esp("default_reset", "no_reset", "read_flash", "0x8000", "0xc00", d / "partition_table_before.bin"),
            self.esp("default_reset", "no_reset", "read_flash", "0x0", "0x2000000", d / "flash_full.bin"),
            self.esp("default_reset", "no_reset", "read_flash", "0xc90000", "0x360000", d / "spiffs_reread.bin"),
            self.esp("default_reset", "no_reset", "verify_flash", *KEEP, "0x0", d / "flash_full.bin"),
        ])
        man = json.loads((d / "manifest.json").read_text())
        self.assertEqual(man["full"]["sha256"], R.sha256_bytes(self.old_flash))
        self.assertEqual(man["old_fs"]["sha256"], R.sha256_bytes(self.old_fs))
        self.assertTrue(all(r["match"] for r in man["read_back"]))
        self.assertEqual(d.stat().st_mode & 0o777, 0o700)

    def test_backup_refuses_new_table(self):
        self.bench.flash = make_flash(self.old_fs, R.NEW_CSV)
        self.assertEqual(self.cli("backup", "--out", self.d, *self.hw()), 2)
        self.assertIn("already carries large_littlefs_32MB.csv", self.out)
        self.assertEqual(len(self.bench.calls), 2)          # flash_id + table, never the dump
        self.assertFalse((self.d / "manifest.json").exists())

    def test_backup_refuses_16mb_chip(self):
        self.bench.detected = "16MB"
        self.assertEqual(self.cli("backup", "--out", self.d, *self.hw()), 2)
        self.assertEqual(len(self.bench.calls), 1)

    def test_backup_second_read_mismatch(self):
        def flip(addr, size, data):
            if addr == R.OLD_FS_OFFSET and size == R.OLD_FS_SIZE:
                data = bytearray(data)
                data[4096 * 720 + 5] ^= 0x01
            return bytes(data)
        self.bench.read_hook = flip
        self.assertEqual(self.cli("backup", "--out", self.d, *self.hw()), 1)
        self.assertIn("blocks [720]", self.out)
        self.assertFalse((self.d / "manifest.json").exists())

    def test_backup_refuses_existing_backup(self):
        self.backup()
        self.assertEqual(self.cli("backup", "--out", self.d, *self.hw()), 2)
        self.assertEqual(self.bench.calls, [])

    # ── adopt ──
    def test_adopt_hand_dump_with_reread(self):
        hand = self.write_hand_files()
        work = hand / "work"
        self.ok("adopt", "--dump", hand / "flash_full.bin", "--out", work,
                "--reread", "0x650000", hand / "verify_app1_1mb.bin", *self.fw())
        self.assertIn("nvs       type 0x01 subtype 0x02 0x00009000 0x00005000", self.out)
        self.assertEqual(work.stat().st_mode & 0o777, 0o700)
        man = json.loads((work / "manifest.json").read_text())
        self.assertEqual(man["full"]["file"], str(hand / "flash_full.bin"))      # referenced, not copied
        self.assertEqual(man["read_back"][0]["offset"], "0x650000")
        self.assertFalse((work / "flash_full.bin").exists())
        self.assertEqual(self.bench.calls, [])
        self.d = work
        self.build_app()
        self.final_read()                                    # needs a read-back dump: it has one
        self.extract()
        self.build_fs()
        self.ok(*self.flash_app_args())

    def test_adopt_without_reread_is_offline_only(self):
        hand = self.write_hand_files()
        self.d = hand / "work"
        self.ok("adopt", "--dump", hand / "flash_full.bin", "--out", self.d, *self.fw())
        self.extract("--source", "dump")
        self.build_fs()
        self.assertEqual(self.cli("final-read", "--in", self.d, *self.hw()), 2)
        self.assertIn("never read back", self.out)
        self.assertEqual(self.bench.calls, [])

    def test_adopt_refuses_short_new_or_bad_reread(self):
        work = self.root / "hand" / "work"
        hand = self.write_hand_files(self.old_flash[:0x1000000])
        self.assertEqual(self.cli("adopt", "--dump", hand / "flash_full.bin", "--out", work, *self.fw()), 2)
        self.assertIn("not a whole 32 MB dump", self.out)
        hand = self.write_hand_files(make_flash(self.old_fs, R.NEW_CSV))
        self.assertEqual(self.cli("adopt", "--dump", hand / "flash_full.bin", "--out", work, *self.fw()), 2)
        hand = self.write_hand_files()
        bad = bytearray((hand / "verify_app1_1mb.bin").read_bytes())
        bad[777] ^= 0x01
        (hand / "verify_app1_1mb.bin").write_bytes(bad)
        self.assertEqual(self.cli("adopt", "--dump", hand / "flash_full.bin", "--out", work,
                                  "--reread", "0x650000", hand / "verify_app1_1mb.bin", *self.fw()), 1)
        self.assertFalse((work / "manifest.json").exists())

    def test_adopt_final_hand_read(self):
        changed = bytearray(self.old_flash)
        changed[R.OLD_FS_OFFSET + 4096 * 5] ^= 0xFF          # the "firmware" wrote after the dump
        hand = self.write_hand_files()
        spiffs = hand / "verify_spiffs_old.bin"
        spiffs.write_bytes(bytes(changed[R.OLD_FS_OFFSET:R.OLD_FS_OFFSET + R.OLD_FS_SIZE]))
        self.d = hand / "work"
        self.ok("adopt", "--dump", hand / "flash_full.bin", "--out", self.d, "--final", spiffs, *self.fw())
        meta = json.loads((self.d / "final_read.json").read_text())
        self.assertEqual(meta["blocks_differing_from_dump"], [5])
        self.assertEqual(meta["how"], "adopted hand read")

    # ── build-app ──
    def test_build_app_commands_and_record(self):
        self.backup()
        self.build_app()
        proj = str(self.project)
        self.assertEqual(self.bench.calls, [
            ([self.tools["pio"], "project", "config", "--json-output"], proj),
            ([self.tools["pio"], "run", "-e", ENV], proj),
        ])
        ab = json.loads((self.d / "app_build.json").read_text())
        self.assertEqual(ab["build_dir"], str(self.build))
        self.assertEqual(ab["artifacts"]["firmware.bin"], R.sha256_file(self.build / "firmware.bin"))

    def test_build_app_refusals(self):
        self.backup()
        args = ["build-app", "--in", self.d, "--env", ENV, "--pio", self.tools["pio"],
                "--project-dir", self.project, *self.fw()]
        good_fw = (self.build / "firmware.bin").read_bytes()
        (self.build / "firmware.bin").write_bytes(b"\xe9\x05\x03\x5f" + b"\x00" * (0x480000 - 3))
        self.assertEqual(self.cli(*args), 2)
        self.assertIn("more than app0", self.out)
        (self.build / "firmware.bin").write_bytes(good_fw)
        (self.build / "partitions.bin").write_bytes(table_bin(R.OLD_CSV))
        self.assertEqual(self.cli(*args), 2)
        (self.build / "partitions.bin").write_bytes(table_bin(R.NEW_CSV))
        (self.build / "bootloader.bin").write_bytes(b"\xe9\x03\x03\x4f" + b"\x00" * 100)
        self.assertEqual(self.cli(*args), 2)
        self.assertIn("not 32MB", self.out)
        self.bench.config[0][1][3] = ["board_upload.flash_size", "16MB"]
        self.assertEqual(self.cli(*args), 2)
        self.assertIn("board_upload.flash_size = 16MB", self.out)
        self.assertEqual(len(self.bench.calls), 1)          # refused before pio run
        self.assertFalse((self.d / "app_build.json").exists())

    # ── final-read ──
    def test_final_read_commands_and_record(self):
        self.backup()
        self.bench.flash[R.OLD_FS_OFFSET + 4096 * 720 + 9] ^= 0x40   # the firmware ran after backup
        self.bench.flash[R.OLD_FS_OFFSET + 4096 * 721 + 9] ^= 0x40
        self.final_read()
        d = self.d
        self.assertEqual(self.commands(), [
            self.esp("default_reset", "no_reset", "read_flash", "0xc90000", "0x360000", d / "spiffs_final.bin"),
            self.esp("default_reset", "no_reset", "verify_flash", *KEEP, "0xc90000", d / "spiffs_final.bin"),
        ])
        meta = json.loads((d / "final_read.json").read_text())
        self.assertEqual(meta["blocks_differing_from_dump"], [720, 721])

    def test_final_read_refuses_without_backup(self):
        self.assertEqual(self.cli("final-read", "--in", self.d, *self.hw()), 2)
        self.assertEqual(self.bench.calls, [])

    # ── extract ──
    def test_extract_copies_every_file_from_final_read(self):
        self.backup()
        self.final_read()
        self.extract()
        for path, data in FILES.items():
            self.assertEqual((self.d / "fs" / path.lstrip("/")).read_bytes(), data, path)
            self.assertIn(f"{len(data):>9}  {path}", self.out)
        self.assertTrue((self.d / "fs" / "empty").is_dir())
        fsman = json.loads((self.d / "fs_manifest.json").read_text())
        self.assertEqual(fsman["source"]["kind"], "final")
        attrs = {r["path"]: r["attrs"] for r in fsman["files"] + fsman["dirs"]}
        self.assertEqual(attrs["/anim/a.pca"], {"0x74": "10325466"})
        self.assertEqual(attrs["/icons"], {"0x74": "aabbccdd"})
        self.assertEqual(fsman["old_fs_stat"]["block_count"], 864)
        self.assertEqual(self.bench.calls, [])              # offline

    def test_extract_refuses_without_final_read(self):
        self.backup()
        self.assertEqual(self.cli("extract", "--in", self.d, *self.fw()), 2)
        self.assertIn("final_read.json", self.out)

    def test_extract_replace(self):
        self.backup()
        self.final_read()
        self.extract()
        self.assertEqual(self.cli("extract", "--in", self.d, *self.fw()), 2)
        self.assertIn("--replace", self.out)
        (self.d / "fs" / "stale").write_bytes(b"x")
        self.extract("--replace")
        self.assertFalse((self.d / "fs" / "stale").exists())

    def test_extract_refuses_unmountable_region(self):
        self.backup()
        self.bench.flash[R.OLD_FS_OFFSET:R.OLD_FS_OFFSET + R.OLD_FS_SIZE] = b"\xff" * R.OLD_FS_SIZE
        self.final_read()
        final_sha = R.sha256_file(self.d / "spiffs_final.bin")
        self.assertEqual(self.cli("extract", "--in", self.d, *self.fw()), 1)
        self.assertIn("does not mount", self.out)
        self.assertIn("Nothing was formatted", self.out)
        self.assertEqual(R.sha256_file(self.d / "spiffs_final.bin"), final_sha)
        self.assertFalse((self.d / "fs").exists())
        self.assertFalse((self.d / "fs_manifest.json").exists())

    def test_extract_refuses_case_collision(self):
        self.bench.flash = make_flash(make_fs(R.OLD_FS_SIZE, {"/icons/Sun": b"1", "/icons/sun": b"2"}))
        self.backup()
        self.final_read()
        self.assertEqual(self.cli("extract", "--in", self.d, *self.fw()), 2)
        self.assertIn("case-insensitive", self.out)
        self.assertFalse((self.d / "fs_manifest.json").exists())

    def test_extract_refuses_changed_inputs(self):
        self.backup()
        self.final_read()
        with open(self.d / "spiffs_final.bin", "r+b") as f:
            f.seek(100)
            f.write(b"\x00")
        self.assertEqual(self.cli("extract", "--in", self.d, *self.fw()), 1)
        with open(self.d / "flash_full.bin", "r+b") as f:
            f.seek(R.OLD_FS_OFFSET + 100)
            f.write(b"\x00")
        self.assertEqual(self.cli("extract", "--in", self.d, "--source", "dump", *self.fw()), 1)
        self.assertIn("changed since it was recorded", self.out)

    # ── build-fs ──
    def test_build_fs_round_trip_and_parameters(self):
        self.backup()
        self.final_read()
        self.extract()
        self.build_fs()
        image = (self.d / "littlefs_32mb.bin").read_bytes()
        self.assertEqual(len(image), 0x16E0000)
        fs = self.mount_image(image)                         # block_count 0, as esp_littlefs mounts
        st = fs.fs_stat()
        self.assertEqual((st.block_size, st.block_count, st.disk_version, st.name_max, st.file_max, st.attr_max),
                         (4096, 0x16E0000 // 4096, 0x00020001, 255, 0x7FFFFFFF, 1022))
        for path, data in FILES.items():
            with fs.open(path, "rb") as f:
                self.assertEqual(f.read(), data, path)
        for path, typ, value in ATTRS:
            self.assertEqual(fs.getattr(path, typ), value)
        self.assertEqual(fs.stat("/empty").type, 2)
        meta = json.loads((self.d / "littlefs_32mb.json").read_text())
        self.assertEqual(meta["sha256"], R.sha256_bytes(image))

    def test_build_fs_refuses_modified_host_file(self):
        self.backup()
        self.final_read()
        self.extract()
        p = self.d / "fs" / "anim" / "b.pca"
        data = bytearray(p.read_bytes())
        data[500] ^= 0xFF
        p.write_bytes(data)
        self.assertEqual(self.cli("build-fs", "--in", self.d, *self.fw()), 1)
        self.assertFalse((self.d / "littlefs_32mb.bin").exists())

    def test_build_fs_refuses_unlisted_file_but_ignores_ds_store(self):
        self.backup()
        self.final_read()
        self.extract()
        (self.d / "fs" / ".DS_Store").write_bytes(b"finder")
        self.build_fs()
        (self.d / "fs" / "anim" / "extra.pca").write_bytes(b"x")
        self.assertEqual(self.cli("build-fs", "--in", self.d, *self.fw()), 2)
        self.assertIn("/anim/extra.pca", self.out)

    # ── write steps: guards ──
    def test_write_steps_need_yes_i_mean_it(self):
        self.ready()
        for argv in (self.flash_app_args()[:-1], self.flash_fs_args()[:-1],
                     ["restore-old", "--in", self.d, *self.hw()]):
            self.assertEqual(self.cli(*argv), 2, argv[0])
            self.assertIn("--yes-i-mean-it", self.out)
            self.assertEqual(self.bench.calls, [], argv[0])

    def test_write_steps_need_port(self):
        for step in ("flash-fs", "restore-old"):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                R.main([step, "--in", str(self.d), "--yes-i-mean-it"], runner=self.bench)
        self.assertEqual(self.bench.calls, [])

    def test_flash_app_refuses_before_build_fs(self):
        self.backup()
        self.build_app()
        self.final_read()
        self.extract()
        self.assertEqual(self.cli(*self.flash_app_args()), 2)
        self.assertIn("littlefs_32mb.bin", self.out)
        self.assertEqual(self.bench.calls, [])

    # ── flash-app ──
    def test_flash_app_commands(self):
        self.ready()
        nvs_before = bytes(self.bench.flash[R.NVS_OFFSET:R.NVS_OFFSET + R.NVS_SIZE])
        self.ok(*self.flash_app_args())
        d, proj = self.d, str(self.project)
        self.assertEqual(self.bench.calls, [
            ([self.tools["pio"], "project", "config", "--json-output"], proj),
            (self.esp("default_reset", "no_reset", "read_flash", "0x8000", "0xc00",
                      d / "partition_table_before_flash_app.bin"), None),
            (self.esp("default_reset", "no_reset", "verify_flash", *KEEP, "0xc90000", d / "spiffs_final.bin"), None),
            ([self.tools["pio"], "run", "-e", ENV, "-t", "upload", "--upload-port", PORT], proj),
        ])
        self.assertEqual(bytes(self.bench.flash[R.PT_OFFSET:R.PT_OFFSET + R.PT_LEN]), table_bin(R.NEW_CSV))
        self.assertEqual(bytes(self.bench.flash[R.NVS_OFFSET:R.NVS_OFFSET + R.NVS_SIZE]), nvs_before)

    def test_flash_app_refuses_if_panel_booted_since_final_read(self):
        self.ready()
        self.bench.flash[R.OLD_FS_OFFSET + 4096 * 721] ^= 0x01
        self.assertEqual(self.cli(*self.flash_app_args()), 1)
        self.assertIn("no longer the final read", self.out)
        self.assertNotIn("upload", [c[3] if len(c) > 3 else "" for c in self.commands()])
        self.assertEqual(bytes(self.bench.flash[R.PT_OFFSET:R.PT_OFFSET + R.PT_LEN]), table_bin(R.OLD_CSV))

    def test_flash_app_refuses_build_changed_since_build_app(self):
        self.ready()
        (self.build / "firmware.bin").write_bytes(b"\xe9\x05\x03\x5f" + rng.randbytes(1000))
        self.assertEqual(self.cli(*self.flash_app_args()), 2)
        self.assertIn("changed since build-app", self.out)
        self.assertEqual(len(self.bench.calls), 1)          # config only, the panel untouched

    def test_flash_app_refuses_image_from_dump(self):
        self.backup()
        self.build_app()
        self.final_read()
        self.extract("--source", "dump")
        self.build_fs()
        self.assertEqual(self.cli(*self.flash_app_args()), 2)
        self.assertIn("--source dump", self.out)
        self.assertEqual(self.bench.calls, [])

    def test_flash_app_refuses_image_from_older_final_read(self):
        self.ready()
        self.bench.flash[R.OLD_FS_OFFSET + 4096 * 700] ^= 0x01
        self.final_read()
        self.assertEqual(self.cli(*self.flash_app_args()), 2)
        self.assertIn("older final read", self.out)
        self.assertEqual(self.bench.calls, [])

    # ── flash-fs ──
    def test_flash_fs_refuses_before_flash_app(self):
        self.ready()
        self.assertEqual(self.cli(*self.flash_fs_args()), 2)
        self.assertIn("run flash-app first", self.out)
        self.assertEqual(self.commands(), [
            self.esp("default_reset", "no_reset", "read_flash", "0x8000", "0xc00",
                     self.d / "partition_table_before_flash_fs.bin")])

    def test_flash_fs_fails_on_bad_verify(self):
        self.ready()
        self.ok(*self.flash_app_args())

        def corrupt(addr, data):
            data = bytearray(data)
            data[4096 * 3 + 7] ^= 0x55
            return bytes(data)
        self.bench.write_hook = corrupt
        self.assertEqual(self.cli(*self.flash_fs_args()), 1)
        self.assertIn("exit code 2", self.out)

    # ── the whole way, then back ──
    def test_full_migration_then_rollback(self):
        self.ready()
        self.ok(*self.flash_app_args())
        img = self.d / "littlefs_32mb.bin"
        self.ok(*self.flash_fs_args())
        self.assertEqual(self.commands(), [
            self.esp("default_reset", "no_reset", "read_flash", "0x8000", "0xc00",
                     self.d / "partition_table_before_flash_fs.bin"),
            self.esp("default_reset", "no_reset", "write_flash", "-z", *PIO_ARGS, "0x910000", img),
            self.esp("default_reset", "hard_reset", "verify_flash", *PIO_ARGS, "0x910000", img),
        ])
        # what the panel will mount: the new partition, block_count from the superblock, grown to size
        region = bytes(self.bench.flash[R.NEW_FS_OFFSET:R.NEW_FS_OFFSET + R.NEW_FS_SIZE])
        fs = self.mount_image(region)
        fs.fs_grow(R.NEW_FS_SIZE // R.BLOCK_SIZE)
        for path, data in FILES.items():
            with fs.open(path, "rb") as f:
                self.assertEqual(f.read(), data, path)

        self.ok("restore-old", "--in", self.d, *self.hw(), "--yes-i-mean-it")
        full = self.d / "flash_full.bin"
        self.assertEqual(self.commands(), [
            self.esp("default_reset", "no_reset", "write_flash", "-z", *KEEP, "0x0", full),
            self.esp("default_reset", "hard_reset", "verify_flash", *KEEP, "0x0", full),
        ])
        self.assertEqual(bytes(self.bench.flash), bytes(self.old_flash))
        steps = [json.loads(line)["step"] for line in (self.d / "journal.jsonl").read_text().splitlines()]
        self.assertEqual(steps, ["flash-app", "flash-fs", "restore-old"])


class EsptoolLookupTest(unittest.TestCase):
    def test_picks_the_package_platform_6_12_0_resolves(self):
        with tempfile.TemporaryDirectory() as tmp:
            pk = pathlib.Path(tmp) / "packages"
            for name, version in (("tool-esptoolpy", "2.41100.260830"),
                                  ("tool-esptoolpy@2.40900.250804", "2.40900.250804")):
                (pk / name).mkdir(parents=True)
                (pk / name / "package.json").write_text(json.dumps({"version": version}))
                (pk / name / "esptool.py").write_text("")
            self.assertEqual(R.find_esptool(tmp), pk / "tool-esptoolpy@2.40900.250804" / "esptool.py")
            (pk / "tool-esptoolpy@2.40900.250804" / "package.json").unlink()
            with self.assertRaises(R.Refuse):
                R.find_esptool(tmp)


if __name__ == "__main__":
    unittest.main()

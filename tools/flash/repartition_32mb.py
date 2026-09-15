#!/usr/bin/env python3
"""Move the Waveshare panel (ESP32-S3-WROOM-2-N32R16V, 32 MB octal flash) from
arduino-esp32's default_16MB.csv to large_littlefs_32MB.csv, keeping what is on
its LittleFS (/anim, /icons, /market).

One step per run, in this order; each refuses unless the ones before left their files:

  backup      --port P --out DIR               reads: the 32 MB rollback dump, verified
  adopt       --dump F --out DIR [--reread OFFSET FILE]... [--final FILE]
                                               offline: a dump read by hand instead of backup
  build-app   --in DIR --env E                 offline: pio run, check the table and the sizes
  final-read  --port P --in DIR                reads: the old LittleFS, panel left in download mode
  extract     --in DIR                         offline: final read -> DIR/fs/
  build-fs    --in DIR                         offline: DIR/fs/ -> DIR/littlefs_32mb.bin
  flash-app   --in DIR --env E --port P --yes-i-mean-it     refuses if the panel booted since final-read
  flash-fs    --in DIR --port P --yes-i-mean-it
  restore-old --in DIR --port P --yes-i-mean-it             rollback: the whole dump back

final-read, extract, build-fs and flash-app follow each other without a reset of the panel.

Run every step the same way, from the repository root:

  uv run --with littlefs-python==0.19.0 python tools/flash/repartition_32mb.py <step> ...

tools/flash/README.md has the order, the reasoning and the sources.
Exit codes: 0 done, 1 a check failed or a command failed, 2 refused.
"""
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import time
import unicodedata
from pathlib import Path

TOOL = "repartition_32mb/2"

# ── the two tables (framework-arduinoespressif32 3.20017, tools/partitions/) ──
FLASH_SIZE = 0x2000000
PT_OFFSET, PT_LEN = 0x8000, 0xC00          # gen_esp32part.py MAX_PARTITION_LENGTH
OLD_CSV, NEW_CSV = "default_16MB.csv", "large_littlefs_32MB.csv"
OLD_FS_OFFSET, OLD_FS_SIZE = 0xC90000, 0x360000
NEW_FS_OFFSET, NEW_FS_SIZE = 0x910000, 0x16E0000
NEW_APP_SIZE = 0x480000
NVS_OFFSET, NVS_SIZE = 0x9000, 0x5000

# ── LittleFS as esp_littlefs 41873c2 (the framework's pin) configures it ──
# esp_littlefs.c: block_size = CONFIG_LITTLEFS_BLOCK_SIZE (#define 4096), read/prog/cache/
# lookahead/block_cycles from sdkconfig (128/128/512/128/512), block_count = 0 (from the
# superblock), name_max unset -> LFS_NAME_MAX, disk_version unset (no MULTIVERSION) ->
# LFS_DISK_VERSION of littlefs f53a0cc (v2.9) = 0x00020001. lfs_init in the framework's
# libesp_littlefs.a loads 255 / 0x7fffffff / 1022 for name/file/attr max.
BLOCK_SIZE = 4096
LFS_CFG = dict(block_size=BLOCK_SIZE, read_size=128, prog_size=128, cache_size=512,
               lookahead_size=128, block_cycles=512, name_max=255, disk_version=0x00020001)
LFS_FILE_MAX, LFS_ATTR_MAX = 0x7FFFFFFF, 1022
LFS_ERR_IO, LFS_ERR_NOATTR = -5, -61

# ── how PlatformIO's espressif32@6.12.0 writes for this env (builder/main.py) ──
# write_flash -z --flash_mode ${_get_board_flash_mode} (opi_opi + arduino -> "dout")
# --flash_freq ${_get_board_f_image} (board f_flash 80000000L -> "80m")
# --flash_size ${board upload.flash_size} (platformio.ini board_upload.flash_size = 32MB)
PIO_FLASH_ARGS = ["--flash_mode", "dout", "--flash_freq", "80m", "--flash_size", "32MB"]
# esptool rewrites header bytes 2-3 of an image at offset 0x0 unless all three are keep
# (cmds.py _update_image_flash_params): a rollback must put the dump back byte for byte.
VERBATIM_ARGS = ["--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]

PIO_HOME = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio")))
ESPTOOL_PKG_VERSION = "2.40900."     # espressif32@6.12.0 requires tool-esptoolpy ~2.40900.0 (esptool 4.9.0)
REPO_ROOT = Path(__file__).resolve().parents[2]
HOST_JUNK = {".DS_Store"}


class Refuse(Exception):
    """A prerequisite is missing or the panel is not in the state this step expects."""


class Mismatch(Exception):
    """A hash, a size, a read-back or a command did not come out as it must."""


# ── small helpers ───────────────────────────────────────────────────────────
def hx(n):
    return f"{n:#x}"


def now():
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path, obj):
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2) + "\n")
    os.replace(tmp, path)


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except ValueError as e:
        raise Refuse(f"{path} is not valid JSON: {e}")


def require_files(*paths):
    missing = [str(p) for p in paths if not Path(p).exists()]
    if missing:
        raise Refuse("missing prerequisite(s):\n    " + "\n    ".join(missing))


def resolve_in(d, name):
    p = Path(name)
    return p if p.is_absolute() else d / p


def private_dir(path):
    """The dump holds NVS (WiFi credentials): its directory is the owner's only."""
    path.mkdir(parents=True, exist_ok=True)
    os.chmod(path, 0o700)


def plan(step, lines):
    print(f"\n== {step}: what it will do")
    for line in lines:
        print(f"   - {line}")
    print(f"== {step}: doing it")


def did(msg):
    print(f"   done: {msg}")


def journal(d, step, **fields):
    with open(Path(d) / "journal.jsonl", "a") as f:
        f.write(json.dumps({"time": now(), "step": step, **fields}) + "\n")


def differing_blocks(a, b):
    return [i for i in range(min(len(a), len(b)) // BLOCK_SIZE)
            if a[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE] != b[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]]


def run_streaming(cmd, cwd=None):
    """Run a command, echo its output live, return (exit code, output)."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    chunks = []
    while True:
        b = proc.stdout.read1(4096)
        if not b:
            break
        chunks.append(b)
        sys.stdout.buffer.write(b)
        sys.stdout.flush()
    return proc.wait(), b"".join(chunks).decode("utf-8", "replace")


def find_esptool(pio_home=PIO_HOME):
    """The tool-esptoolpy PlatformIO resolves for espressif32@6.12.0 (pio pkg list -e ...)."""
    seen = []
    for d in sorted((Path(pio_home) / "packages").glob("tool-esptoolpy*")):
        try:
            version = json.loads((d / "package.json").read_text()).get("version", "")
        except (OSError, ValueError):
            continue
        seen.append(f"{d.name} {version}")
        if version.startswith(ESPTOOL_PKG_VERSION) and (d / "esptool.py").is_file():
            return d / "esptool.py"
    raise Refuse(f"no tool-esptoolpy {ESPTOOL_PKG_VERSION}x under {pio_home}/packages "
                 f"(found: {', '.join(seen) or 'none'}); pass --esptool")


def import_littlefs():
    try:
        import littlefs
    except ImportError:
        raise Refuse("littlefs-python is missing; run as: uv run --with littlefs-python==0.19.0 "
                     "python tools/flash/repartition_32mb.py ...")
    if tuple(littlefs.__LFS_DISK_VERSION__) != (2, 1):
        raise Refuse(f"littlefs-python {littlefs.__version__} writes disk version "
                     f"{littlefs.__LFS_DISK_VERSION__}, the panel's littlefs v2.9 reads 2.1")
    print(f"   littlefs-python {littlefs.__version__} (littlefs {littlefs.__LFS_VERSION__[0]}."
          f"{littlefs.__LFS_VERSION__[1]}, disk version 2.1)")
    return littlefs


def read_only_context(littlefs, buf):
    """A block device that refuses every write and counts the attempts."""
    class ReadOnlyContext(littlefs.UserContext):
        def __init__(self, buffer):
            super().__init__(buffer=buffer)
            self.writes = 0

        def prog(self, cfg, block, off, data):
            self.writes += 1
            return LFS_ERR_IO

        def erase(self, cfg, block):
            self.writes += 1
            return LFS_ERR_IO

    return ReadOnlyContext(buf)


def fs_stat_dict(fs):
    st = fs.fs_stat()
    return {k: getattr(st, k) for k in
            ("disk_version", "name_max", "file_max", "attr_max", "block_count", "block_size")}


def walk_fs(fs):
    dirs, files = [], []
    for root, dnames, fnames in fs.walk("/"):
        dirs += [posixpath.join(root, n) for n in dnames]
        files += [posixpath.join(root, n) for n in fnames]
    return sorted(dirs), sorted(files)


def read_attrs(littlefs, fs, path):
    """Every user attribute (esp_littlefs stores the mtime as type 't')."""
    out = {}
    for t in range(256):
        try:
            value = fs.getattr(path, t)
        except littlefs.LittleFSError as e:
            if e.code == LFS_ERR_NOATTR:
                continue
            raise
        out[f"0x{t:02x}"] = bytes(value).hex()
    return out


def write_attrs(fs, path, attrs):
    for t, value in attrs.items():
        fs.setattr(path, int(t, 16), bytes.fromhex(value))


def host_path(base, lfs_path):
    parts = lfs_path.strip("/").split("/")
    for p in parts:
        if p in ("", ".", "..") or "\\" in p or "\x00" in p:
            raise Refuse(f"name {lfs_path!r} cannot be stored on the host as it is")
    return base.joinpath(*parts)


# ── context: the tools, the tables ──────────────────────────────────────────
class Ctx:
    def __init__(self, args, runner):
        self.args = args
        self.runner = runner
        self.framework = Path(args.framework_dir).expanduser()
        self._gen = None

    def tool(self, attr, default):
        path = Path(getattr(self.args, attr, None) or default).expanduser()
        if not path.is_file():
            raise Refuse(f"{attr} not found: {path}")
        return str(path)

    def check_tools(self, esptool=True, pio=False):
        if esptool:
            self.python = self.tool("python", PIO_HOME / "penv" / "bin" / "python")
            self.esptool = self.tool("esptool", None if self.args.esptool else find_esptool())
        if pio:
            self.pio = self.tool("pio", PIO_HOME / "penv" / "bin" / "pio")

    def esp(self, before, after, *op):
        return [self.python, self.esptool, "--chip", "esp32s3", "--port", self.args.port,
                "--baud", str(self.args.baud), "--before", before, "--after", after, *op]

    def run(self, cmd, cwd=None, expect=None, fail=None):
        print(f"   $ {shlex.join(cmd)}" + (f"    (in {cwd})" if cwd else ""), flush=True)
        rc, out = self.runner(cmd, str(cwd) if cwd else None)
        if rc != 0 or (expect and expect not in out):
            why = f"exit code {rc}" if rc != 0 else f"{expect!r} not in its output"
            raise Mismatch(f"{fail + ' ' if fail else ''}({why}: {shlex.join(cmd)})")
        return out

    @property
    def gen(self):
        """Partition tables through the framework's own gen_esp32part.py."""
        if self._gen is None:
            path = self.framework / "tools" / "gen_esp32part.py"
            require_files(path)
            spec = importlib.util.spec_from_file_location("gen_esp32part", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            mod.quiet = True
            self._gen = mod
        return self._gen

    def csv_rows(self, name):
        path = self.framework / "tools" / "partitions" / name
        require_files(path)
        return table_rows(self.gen.PartitionTable.from_csv(path.read_text()))

    def blob_rows(self, blob):
        try:
            return table_rows(self.gen.PartitionTable.from_binary(blob))
        except Exception as e:     # InputError and friends: garbage is "not the table"
            print(f"   the table does not decode: {e}")
            return None


def table_rows(table):
    return [(p.name, p.type, p.subtype, p.offset, p.size, bool(p.encrypted)) for p in table]


def show_rows(rows):
    if not rows:
        return "   (no partitions)"
    return "\n".join(f"   {n:<9} type {t:#04x} subtype {s:#04x} {o:#010x} {z:#010x}" for n, t, s, o, z, _ in rows)


def identify_table(ctx, blob):
    rows = ctx.blob_rows(blob)
    if rows is None:
        return None, None
    for name in (OLD_CSV, NEW_CSV):
        if rows == ctx.csv_rows(name):
            return name, rows
    return "unknown", rows


def require_old_table(ctx, blob, where):
    which, rows = identify_table(ctx, blob)
    print(show_rows(rows))
    if which == NEW_CSV:
        raise Refuse(f"{where} already carries {NEW_CSV}; there is no old filesystem to carry over")
    if which != OLD_CSV:
        raise Refuse(f"the partition table in {where} is not {OLD_CSV}")
    did(f"partition table in {where} is {OLD_CSV}")


def backup_manifest(step, dump_file, data, read_back, detected):
    return {
        "tool": TOOL, "created": now(), "step": step,
        "chip": {"detected_flash_size": detected},
        "partition_table": {"offset": hx(PT_OFFSET), "size": PT_LEN,
                            "sha256": sha256_bytes(data[PT_OFFSET:PT_OFFSET + PT_LEN]), "is": OLD_CSV},
        "full": {"file": dump_file, "offset": "0x0", "size": FLASH_SIZE, "sha256": sha256_bytes(data)},
        "read_back": read_back,
        "old_fs": {"label": "spiffs", "offset": hx(OLD_FS_OFFSET), "size": OLD_FS_SIZE,
                   "sha256": sha256_bytes(data[OLD_FS_OFFSET:OLD_FS_OFFSET + OLD_FS_SIZE])},
        "nvs": {"offset": hx(NVS_OFFSET), "size": NVS_SIZE,
                "sha256": sha256_bytes(data[NVS_OFFSET:NVS_OFFSET + NVS_SIZE])},
    }


# ── prerequisites shared by the later steps ─────────────────────────────────
def load_backup(d, need_read_back=False):
    """The manifest and the rollback dump it names; need_read_back before anything that writes."""
    man_p = d / "manifest.json"
    require_files(man_p)
    man = read_json(man_p)
    if man.get("tool") != TOOL or man.get("partition_table", {}).get("is") != OLD_CSV:
        raise Refuse(f"{man_p} is not a backup manifest made by this version of the tool")
    full = resolve_in(d, man["full"]["file"])
    require_files(full)
    if not any(r.get("match") for r in man.get("read_back", [])):
        if need_read_back:
            raise Refuse("the rollback dump was never read back from the panel, and this step leads to "
                         "writing: read a region of it again and run adopt with --reread OFFSET FILE")
        print("   note: the rollback dump was not read back a second time")
    if full.stat().st_size != FLASH_SIZE:
        raise Mismatch(f"{full} is {full.stat().st_size} bytes, not {FLASH_SIZE}")
    got = sha256_file(full)
    if got != man["full"]["sha256"]:
        raise Mismatch(f"{full} changed since it was recorded: sha256 {got} != {man['full']['sha256']}")
    did(f"rollback dump {full.name} is intact (sha256 {got[:16]}...)")
    return man, full


def load_final(d):
    meta_p = d / "final_read.json"
    require_files(meta_p)
    meta = read_json(meta_p)
    f = resolve_in(d, meta["file"])
    require_files(f)
    if f.stat().st_size != OLD_FS_SIZE:
        raise Mismatch(f"{f} is {f.stat().st_size} bytes, not {OLD_FS_SIZE}")
    if sha256_file(f) != meta["sha256"]:
        raise Mismatch(f"{f} changed since it was recorded in final_read.json")
    did(f"final read {f.name} is intact (sha256 {meta['sha256'][:16]}..., {meta['how']})")
    return meta, f


def load_image(d, man, need_final):
    img, meta_p, fsman_p = d / "littlefs_32mb.bin", d / "littlefs_32mb.json", d / "fs_manifest.json"
    require_files(img, meta_p, fsman_p)
    meta, fsman = read_json(meta_p), read_json(fsman_p)
    src = fsman.get("source", {})
    if src.get("full_sha256") != man["full"]["sha256"]:
        raise Refuse("fs_manifest.json does not belong to this directory's rollback dump")
    if meta.get("fs_manifest_sha256") != sha256_file(fsman_p):
        raise Refuse("littlefs_32mb.bin was not built from this fs_manifest.json; run build-fs again")
    if need_final:
        if src.get("kind") != "final":
            raise Refuse("the image was built from the rollback dump (--source dump), not from a final read")
        fmeta, _ = load_final(d)
        if src.get("sha256") != fmeta["sha256"]:
            raise Refuse("the image was built from an older final read; run extract --replace and build-fs")
    if img.stat().st_size != NEW_FS_SIZE:
        raise Mismatch(f"{img} is {img.stat().st_size} bytes, not {NEW_FS_SIZE}")
    got = sha256_file(img)
    if got != meta["sha256"]:
        raise Mismatch(f"{img} changed since build-fs: sha256 {got} != {meta['sha256']}")
    did(f"image {img.name} is intact ({len(fsman['files'])} files, sha256 {got[:16]}...)")
    return img, meta


def require_write(args):
    if not args.yes_i_mean_it:
        raise Refuse("this step writes the panel's flash; pass --yes-i-mean-it")
    if not args.port:
        raise Refuse("pass --port explicitly")


# ── backup ──────────────────────────────────────────────────────────────────
def step_backup(ctx):
    out = Path(ctx.args.out).expanduser().resolve()
    if (out / "manifest.json").exists() or (out / "flash_full.bin").exists():
        raise Refuse(f"{out} already holds a backup; give a new --out directory")
    ctx.check_tools()
    ctx.csv_rows(OLD_CSV)
    plan("backup", [
        "reads only; nothing is written to the panel",
        "esptool flash_id: require 'Detected flash size: 32MB'",
        f"read the partition table ({hx(PT_OFFSET)}, {hx(PT_LEN)} bytes), require {OLD_CSV}",
        f"read the whole flash 0x0..{hx(FLASH_SIZE)} into flash_full.bin (the rollback image)",
        f"read the old LittleFS ({hx(OLD_FS_OFFSET)}, {hx(OLD_FS_SIZE)}) a second time, compare its SHA-256 "
        "with the dump's slice; verify_flash the whole dump (MD5 computed on the chip)",
        "write manifest.json into a mode-700 directory; calls end --after no_reset",
    ])
    private_dir(out)
    pt_file, full, reread = out / "partition_table_before.bin", out / "flash_full.bin", out / "spiffs_reread.bin"

    text = ctx.run(ctx.esp("default_reset", "no_reset", "flash_id"))
    m = re.search(r"Detected flash size:\s*(\S+)", text)
    if not m or m.group(1) != "32MB":
        raise Refuse(f"the chip reports flash size {m.group(1) if m else 'unknown'}, not 32MB")
    did("the chip reports 32MB of flash")

    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", hx(PT_OFFSET), hx(PT_LEN), str(pt_file)))
    pt = pt_file.read_bytes()
    if len(pt) != PT_LEN:
        raise Mismatch(f"{pt_file} is {len(pt)} bytes, not {PT_LEN}")
    require_old_table(ctx, pt, "the panel")

    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", "0x0", hx(FLASH_SIZE), str(full)))
    data = full.read_bytes()
    if len(data) != FLASH_SIZE:
        raise Mismatch(f"{full} is {len(data)} bytes, not {FLASH_SIZE}")
    if data[PT_OFFSET:PT_OFFSET + PT_LEN] != pt:
        raise Mismatch("the partition table in the dump differs from the one read before it")
    slice_ = data[OLD_FS_OFFSET:OLD_FS_OFFSET + OLD_FS_SIZE]
    did(f"dump read: {len(data)} bytes, sha256 {sha256_bytes(data)[:16]}...")

    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", hx(OLD_FS_OFFSET), hx(OLD_FS_SIZE), str(reread)))
    rr = reread.read_bytes()
    if rr != slice_:
        raise Mismatch(f"second read of the old LittleFS differs from the dump in blocks "
                       f"{differing_blocks(rr, slice_)[:20]}; run backup again into a new directory")
    did(f"second read of {hx(OLD_FS_OFFSET)}..{hx(OLD_FS_OFFSET + OLD_FS_SIZE)} matches the dump")
    ctx.run(ctx.esp("default_reset", "no_reset", "verify_flash", *VERBATIM_ARGS, "0x0", str(full)),
            expect="-- verify OK", fail="the panel's flash differs from the dump just read.")
    did("verify_flash: the whole flash matches the dump")

    read_back = [{"offset": hx(OLD_FS_OFFSET), "size": OLD_FS_SIZE, "file": reread.name,
                  "sha256": sha256_bytes(rr), "match": True},
                 {"offset": "0x0", "size": FLASH_SIZE, "method": "verify_flash (MD5 on the chip)", "match": True}]
    write_json(out / "manifest.json", backup_manifest("backup", full.name, data, read_back, "32MB"))
    did(f"wrote {out / 'manifest.json'}")


# ── adopt: a dump read by hand ──────────────────────────────────────────────
def step_adopt(ctx):
    a = ctx.args
    dump = Path(a.dump).expanduser().resolve()
    out = Path(a.out).expanduser().resolve()
    plan("adopt", [
        "offline; for a dump read by hand with: esptool ... read_flash 0x0 0x2000000 FILE",
        f"require exactly {FLASH_SIZE} bytes and {OLD_CSV} at {hx(PT_OFFSET)} inside it",
        "each --reread OFFSET FILE (a second hand read) must equal the dump at OFFSET; at least one is "
        "needed before any step that leads to writing. Use a region the firmware does not write (an app slot)",
        "--final FILE records a hand read of the old LittleFS as the final read (flash-app still checks "
        "it against the panel)",
        "write manifest.json into --out (mode 700) naming the dump; nothing is copied",
    ])
    require_files(dump)
    if (out / "manifest.json").exists():
        raise Refuse(f"{out} already holds a manifest; give a new --out directory")
    size = dump.stat().st_size
    if size != FLASH_SIZE:
        raise Refuse(f"{dump} is {size} bytes, not {FLASH_SIZE}: not a whole 32 MB dump (yet)")
    data = dump.read_bytes()
    require_old_table(ctx, data[PT_OFFSET:PT_OFFSET + PT_LEN], dump.name)
    read_back = []
    for off_s, name in a.reread or []:
        off, rp = int(off_s, 0), Path(name).expanduser().resolve()
        require_files(rp)
        rr = rp.read_bytes()
        if not rr or off + len(rr) > FLASH_SIZE or rr != data[off:off + len(rr)]:
            raise Mismatch(f"{rp.name} differs from the dump at {hx(off)}; the dump cannot be trusted "
                           "(or the firmware writes that region: re-read an app slot instead)")
        read_back.append({"offset": hx(off), "size": len(rr), "file": str(rp), "sha256": sha256_bytes(rr),
                          "match": True})
        did(f"{rp.name} equals the dump at {hx(off)}..{hx(off + len(rr))}")
    final = None
    if a.final:
        fp = Path(a.final).expanduser().resolve()
        require_files(fp)
        fb = fp.read_bytes()
        if len(fb) != OLD_FS_SIZE:
            raise Refuse(f"{fp.name} is {len(fb)} bytes, not {OLD_FS_SIZE}")
        blocks = differing_blocks(fb, data[OLD_FS_OFFSET:OLD_FS_OFFSET + OLD_FS_SIZE])
        final = {"tool": TOOL, "created": now(), "file": str(fp), "offset": hx(OLD_FS_OFFSET), "size": OLD_FS_SIZE,
                 "sha256": sha256_bytes(fb), "how": "adopted hand read", "blocks_differing_from_dump": blocks}
        did(f"{fp.name} as the final read; differs from the dump's old LittleFS in blocks {blocks}")
    private_dir(out)
    man = backup_manifest("adopt", str(dump), data, read_back, None)
    write_json(out / "manifest.json", man)
    if final:
        write_json(out / "final_read.json", final)
    did(f"wrote {out / 'manifest.json'} (dump sha256 {man['full']['sha256'][:16]}..., "
        f"read back: {'yes' if read_back else 'no'})")


# ── build-app ───────────────────────────────────────────────────────────────
def check_env_config(ctx, project, env):
    out = ctx.run([ctx.pio, "project", "config", "--json-output"], cwd=project)
    try:
        sections = json.loads(out[out.index("["):])
    except ValueError as e:
        raise Mismatch(f"cannot read `pio project config --json-output`: {e}")
    cfg = next((dict((k, v) for k, v in items) for name, items in sections if name == f"env:{env}"), None)
    if cfg is None:
        raise Refuse(f"env:{env} is not in {project}/platformio.ini")
    wants = {"board_build.partitions": NEW_CSV, "board_upload.flash_size": "32MB",
             "board_build.arduino.memory_type": "opi_opi"}
    for key, want in wants.items():
        got = str(cfg.get(key, ""))
        if (Path(got).name if key == "board_build.partitions" else got) != want:
            raise Refuse(f"env:{env} has {key} = {got or '(unset)'}, need {want}")
    did(f"env:{env} config: {wants}")


def check_artifacts(ctx, build):
    parts, fw, bl = build / "partitions.bin", build / "firmware.bin", build / "bootloader.bin"
    boot_app0 = ctx.framework / "tools" / "partitions" / "boot_app0.bin"
    require_files(parts, fw, bl, boot_app0)
    which, rows = identify_table(ctx, parts.read_bytes())
    if which != NEW_CSV:
        print(show_rows(rows))
        raise Refuse(f"{parts} is not {NEW_CSV}")
    app0 = next(r for r in rows if r[0] == "app0")
    size = fw.stat().st_size
    if size > NEW_APP_SIZE or size > app0[4]:
        raise Refuse(f"{fw.name} is {size} bytes, more than app0's {hx(app0[4])}")
    for image in (bl, fw):
        head = image.read_bytes()[:4]
        if len(head) < 4 or head[0] != 0xE9:
            raise Refuse(f"{image} is not an ESP image")
        if head[3] & 0xF0 != 0x50:      # esptool targets/esp32.py FLASH_SIZES: "32MB": 0x50
            raise Refuse(f"{image} header says flash size code {head[3] & 0xF0:#x}, not 32MB (0x50)")
    did(f"build: partitions.bin is {NEW_CSV}; firmware.bin {size} bytes <= {hx(app0[4])}; "
        "bootloader and app headers say 32MB")
    return {"partitions.bin": sha256_file(parts), "firmware.bin": sha256_file(fw),
            "bootloader.bin": sha256_file(bl), "boot_app0.bin": sha256_file(boot_app0)}


def step_build_app(ctx):
    a = ctx.args
    d = Path(a.in_dir).expanduser().resolve()
    project = Path(a.project_dir).expanduser().resolve()
    build = Path(a.build_dir).expanduser().resolve() if a.build_dir else project / ".pio" / "build" / a.env
    plan("build-app", [
        "offline; run it before final-read, so the panel never waits on a compile",
        f"`pio project config`: env:{a.env} must have board_build.partitions = {NEW_CSV}, "
        "board_upload.flash_size = 32MB, board_build.arduino.memory_type = opi_opi",
        f"`pio run -e {a.env}`; partitions.bin must decode to {NEW_CSV}, firmware.bin <= {hx(NEW_APP_SIZE)}, "
        "image headers must say 32MB",
        "record the artifacts' hashes in app_build.json; flash-app refuses if they change",
    ])
    require_files(d / "manifest.json")
    ctx.check_tools(esptool=False, pio=True)
    check_env_config(ctx, project, a.env)
    ctx.run([ctx.pio, "run", "-e", a.env], cwd=project)
    arts = check_artifacts(ctx, build)
    write_json(d / "app_build.json", {"tool": TOOL, "created": now(), "env": a.env,
                                      "project_dir": str(project), "build_dir": str(build), "artifacts": arts})
    did(f"wrote {d / 'app_build.json'}")


# ── final-read ──────────────────────────────────────────────────────────────
def step_final_read(ctx):
    a = ctx.args
    d = Path(a.in_dir).expanduser().resolve()
    out = d / "spiffs_final.bin"
    plan("final-read", [
        "reads only; the files to migrate come from this read, not from the rollback dump",
        f"read_flash {hx(OLD_FS_OFFSET)} {hx(OLD_FS_SIZE)} spiffs_final.bin --before default_reset "
        "--after no_reset (the panel stays in download mode; the firmware does not run again)",
        f"verify_flash {hx(OLD_FS_OFFSET)} spiffs_final.bin (MD5 computed on the chip), again --after no_reset",
        "write final_read.json; do NOT reset or replug the panel until flash-app",
    ])
    load_backup(d, need_read_back=True)
    ctx.check_tools()
    (d / "final_read.json").unlink(missing_ok=True)       # an old record must not outlive a new read
    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", hx(OLD_FS_OFFSET), hx(OLD_FS_SIZE), str(out)))
    data = out.read_bytes()
    if len(data) != OLD_FS_SIZE:
        raise Mismatch(f"{out} is {len(data)} bytes, not {OLD_FS_SIZE}")
    ctx.run(ctx.esp("default_reset", "no_reset", "verify_flash", *VERBATIM_ARGS, hx(OLD_FS_OFFSET), str(out)),
            expect="-- verify OK", fail="the panel's LittleFS differs from the read just taken.")
    man = read_json(d / "manifest.json")
    with open(resolve_in(d, man["full"]["file"]), "rb") as f:
        f.seek(OLD_FS_OFFSET)
        blocks = differing_blocks(data, f.read(OLD_FS_SIZE))
    write_json(d / "final_read.json", {
        "tool": TOOL, "created": now(), "file": out.name, "offset": hx(OLD_FS_OFFSET), "size": OLD_FS_SIZE,
        "sha256": sha256_bytes(data), "how": "final-read, verified on the chip",
        "blocks_differing_from_dump": blocks})
    did(f"read and verified; differs from the rollback dump in blocks {blocks}")
    print(f"\n   The panel waits in download mode. Now, without resetting it:\n"
          f"     extract --in {d} --replace\n     build-fs --in {d}\n     flash-app --in {d} ...")


# ── extract ─────────────────────────────────────────────────────────────────
def step_extract(ctx):
    a = ctx.args
    d = Path(a.in_dir).expanduser().resolve()
    fs_dir, fsman_p, partial = d / "fs", d / "fs_manifest.json", d / "fs.partial"
    plan("extract", [
        "offline; the panel is not touched",
        "take the old LittleFS from " + ("the final read (spiffs_final.bin)" if a.source == "final"
                                         else "the rollback dump's slice (inspection only: flash-app refuses it)"),
        "mount it read-only (every write refused and counted) with the panel's LittleFS settings; "
        "a mount error stops here, nothing is ever formatted",
        "copy every directory, file and user attribute to DIR/fs/, hash each, list names and sizes",
        "write fs_manifest.json",
    ])
    man, full = load_backup(d)
    if fs_dir.exists() or fsman_p.exists():
        if not a.replace:
            raise Refuse(f"{fs_dir} or {fsman_p} already exists; pass --replace to extract again")
        shutil.rmtree(fs_dir, ignore_errors=True)
        fsman_p.unlink(missing_ok=True)
    littlefs = import_littlefs()

    if a.source == "final":
        fmeta, final = load_final(d)
        region = bytearray(final.read_bytes())
        src = {"kind": "final", "file": fmeta["file"], "sha256": fmeta["sha256"], "how": fmeta["how"]}
    else:
        with open(full, "rb") as f:
            f.seek(OLD_FS_OFFSET)
            region = bytearray(f.read(OLD_FS_SIZE))
        src = {"kind": "dump", "file": man["full"]["file"], "sha256": man["old_fs"]["sha256"]}
    src.update(full_sha256=man["full"]["sha256"], offset=hx(OLD_FS_OFFSET), size=OLD_FS_SIZE)
    if sha256_bytes(region) != src["sha256"]:
        raise Mismatch("the old LittleFS bytes do not match their record")
    ro = read_only_context(littlefs, region)
    fs = littlefs.LittleFS(context=ro, mount=False, **{**LFS_CFG, "block_count": 0})
    try:
        fs.mount()
    except littlefs.LittleFSError as e:
        raise Mismatch(f"the old LittleFS does not mount: {e}. Nothing was formatted; the read is untouched")
    stat = fs_stat_dict(fs)
    did(f"mounted read-only: {stat}")
    if stat["block_size"] != BLOCK_SIZE or stat["block_count"] * BLOCK_SIZE > OLD_FS_SIZE:
        raise Mismatch(f"superblock geometry {stat} does not fit the {hx(OLD_FS_SIZE)} partition")
    if stat["block_count"] * BLOCK_SIZE != OLD_FS_SIZE:
        print(f"   note: superblock says {stat['block_count']} blocks, the partition has "
              f"{OLD_FS_SIZE // BLOCK_SIZE} (the panel grows it on mount); files are read all the same")

    dirs, files = walk_fs(fs)
    folded = {}
    for p in dirs + files:
        key = unicodedata.normalize("NFC", p).casefold()
        if key in folded:
            raise Refuse(f"{folded[key]!r} and {p!r} would be one name on a case-insensitive host disk")
        folded[key] = p

    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir()
    rec_dirs, rec_files, total = [], [], 0
    root_attrs = read_attrs(littlefs, fs, "/")
    for p in dirs:
        host_path(partial, p).mkdir(parents=True, exist_ok=True)
        rec_dirs.append({"path": p, "attrs": read_attrs(littlefs, fs, p)})
    for p in files:
        size = fs.stat(p).size
        with fs.open(p, "rb") as fh:
            data = fh.read()
        if len(data) != size:
            raise Mismatch(f"{p}: read {len(data)} bytes, the directory entry says {size}")
        hp = host_path(partial, p)
        hp.parent.mkdir(parents=True, exist_ok=True)
        hp.write_bytes(data)
        rec_files.append({"path": p, "size": size, "sha256": sha256_bytes(data),
                          "attrs": read_attrs(littlefs, fs, p)})
        total += size
    fs.unmount()
    if ro.writes:
        raise Mismatch(f"littlefs tried to write the old image {ro.writes} times while reading it")
    if sha256_bytes(region) != src["sha256"]:
        raise Mismatch("the old image changed while it was read")
    for r in rec_files:
        if sha256_file(host_path(partial, r["path"])) != r["sha256"]:
            raise Mismatch(f"{r['path']}: the copy on the host disk differs from the image")
    os.replace(partial, fs_dir)

    write_json(fsman_p, {
        "tool": TOOL, "created": now(), "step": "extract", "source": src,
        "littlefs_python": littlefs.__version__, "old_fs_stat": stat,
        "root_attrs": root_attrs, "dirs": rec_dirs, "files": rec_files, "total_bytes": total,
    })
    print(f"\n   {'bytes':>9}  path")
    for r in rec_dirs:
        print(f"   {'<dir>':>9}  {r['path']}/")
    for r in rec_files:
        print(f"   {r['size']:>9}  {r['path']}")
    did(f"{len(rec_files)} files, {len(rec_dirs)} directories, {total} bytes -> {fs_dir}; wrote {fsman_p.name}")


# ── build-fs ────────────────────────────────────────────────────────────────
def check_host_tree(fs_dir, fsman):
    want_files = {r["path"]: r for r in fsman["files"]}
    want_dirs = {r["path"] for r in fsman["dirs"]}
    extra = []
    for root, dnames, fnames in os.walk(fs_dir):
        rel = "/" + Path(root).relative_to(fs_dir).as_posix().lstrip(".").lstrip("/")
        for n in dnames:
            p = posixpath.join(rel, n)
            if p not in want_dirs:
                extra.append(p + "/")
        for n in fnames:
            p = posixpath.join(rel, n)
            if n in HOST_JUNK:
                print(f"   ignoring host file {p}")
            elif p not in want_files:
                extra.append(p)
    if extra:
        raise Refuse("DIR/fs holds entries fs_manifest.json does not list:\n    " + "\n    ".join(sorted(extra)))
    for p in want_dirs:
        if not host_path(fs_dir, p).is_dir():
            raise Mismatch(f"directory {p} is missing from {fs_dir}")
    for p, r in want_files.items():
        hp = host_path(fs_dir, p)
        if not hp.is_file() or sha256_file(hp) != r["sha256"]:
            raise Mismatch(f"{p} in {fs_dir} is missing or differs from what extract copied")


def verify_image(littlefs, image, fsman):
    """Mount the image as the panel will and compare everything with the manifest."""
    blocks = NEW_FS_SIZE // BLOCK_SIZE
    ro = read_only_context(littlefs, bytearray(image))
    fs = littlefs.LittleFS(context=ro, mount=False, **{**LFS_CFG, "block_count": 0})
    fs.mount()
    stat = fs_stat_dict(fs)
    want = {"block_size": BLOCK_SIZE, "block_count": blocks, "disk_version": LFS_CFG["disk_version"],
            "name_max": LFS_CFG["name_max"], "file_max": LFS_FILE_MAX, "attr_max": LFS_ATTR_MAX}
    if stat != want:
        raise Mismatch(f"image superblock {stat} != {want}")
    dirs, files = walk_fs(fs)
    if dirs != sorted(r["path"] for r in fsman["dirs"]) or files != sorted(r["path"] for r in fsman["files"]):
        raise Mismatch("the image's directory tree differs from fs_manifest.json")
    if read_attrs(littlefs, fs, "/") != fsman["root_attrs"]:
        raise Mismatch("root attributes differ")
    for r in fsman["dirs"]:
        if read_attrs(littlefs, fs, r["path"]) != r["attrs"]:
            raise Mismatch(f"{r['path']}: attributes differ")
    for r in fsman["files"]:
        with fs.open(r["path"], "rb") as fh:
            data = fh.read()
        if len(data) != r["size"] or sha256_bytes(data) != r["sha256"]:
            raise Mismatch(f"{r['path']}: bytes in the image differ")
        if read_attrs(littlefs, fs, r["path"]) != r["attrs"]:
            raise Mismatch(f"{r['path']}: attributes differ")
    used = fs.used_block_count
    fs.unmount()
    if ro.writes:
        raise Mismatch(f"mounting the image tried {ro.writes} writes")
    # LittleFS.begin() passes grow_on_mount = true: on a full-size image that must change nothing.
    grow_ctx = littlefs.UserContext(buffer=bytearray(image))
    g = littlefs.LittleFS(context=grow_ctx, mount=False, **{**LFS_CFG, "block_count": 0})
    g.mount()
    g.fs_grow(blocks)
    g.unmount()
    if grow_ctx.buffer != image:
        raise Mismatch("lfs_fs_grow to the partition size changed the image")
    return stat, used


def step_build_fs(ctx):
    d = Path(ctx.args.in_dir).expanduser().resolve()
    fs_dir, fsman_p = d / "fs", d / "fs_manifest.json"
    img, meta_p = d / "littlefs_32mb.bin", d / "littlefs_32mb.json"
    plan("build-fs", [
        "offline; the panel is not touched",
        "check DIR/fs/ against fs_manifest.json (hashes, nothing extra)",
        f"format a {hx(NEW_FS_SIZE)}-byte LittleFS ({NEW_FS_SIZE // BLOCK_SIZE} blocks) with the panel's "
        f"settings {LFS_CFG}, copy the directories, files and attributes in",
        "mount it again read-only with block_count 0 (as esp_littlefs does), compare superblock, tree, "
        "bytes and attributes; check that grow-on-mount is a no-op",
        "write littlefs_32mb.bin and littlefs_32mb.json",
    ])
    man, _ = load_backup(d)
    require_files(fs_dir, fsman_p)
    fsman = read_json(fsman_p)
    if fsman.get("source", {}).get("full_sha256") != man["full"]["sha256"]:
        raise Refuse("fs_manifest.json does not belong to this directory's rollback dump")
    if fsman["source"]["kind"] == "final" and fsman["source"]["sha256"] != load_final(d)[0]["sha256"]:
        raise Refuse("fs_manifest.json comes from an older final read; run extract --replace")
    littlefs = import_littlefs()
    check_host_tree(fs_dir, fsman)
    did(f"DIR/fs matches fs_manifest.json ({len(fsman['files'])} files)")

    wctx = littlefs.UserContext(buffer=bytearray(b"\xff" * NEW_FS_SIZE))
    fs = littlefs.LittleFS(context=wctx, mount=False, **{**LFS_CFG, "block_count": NEW_FS_SIZE // BLOCK_SIZE})
    fs.format()
    fs.mount()
    write_attrs(fs, "/", fsman["root_attrs"])
    for r in sorted(fsman["dirs"], key=lambda r: (r["path"].count("/"), r["path"])):
        fs.mkdir(r["path"])
        write_attrs(fs, r["path"], r["attrs"])
    for r in fsman["files"]:
        data = host_path(fs_dir, r["path"]).read_bytes()
        if sha256_bytes(data) != r["sha256"]:
            raise Mismatch(f"{r['path']} changed on the host disk during build-fs")
        with fs.open(r["path"], "wb") as fh:
            fh.write(data)
        write_attrs(fs, r["path"], r["attrs"])
    fs.unmount()
    image = bytes(wctx.buffer)
    if len(image) != NEW_FS_SIZE:
        raise Mismatch(f"image is {len(image)} bytes, not {NEW_FS_SIZE}")
    stat, used = verify_image(littlefs, image, fsman)
    did(f"image re-mounted and verified: {stat}, {used} blocks used")

    tmp = img.with_name(img.name + ".tmp")
    tmp.write_bytes(image)
    os.replace(tmp, img)
    sha = sha256_bytes(image)
    if sha256_file(img) != sha:
        raise Mismatch(f"{img} on disk differs from the image just verified")
    write_json(meta_p, {
        "tool": TOOL, "created": now(), "step": "build-fs", "file": img.name, "size": NEW_FS_SIZE,
        "sha256": sha, "offset": hx(NEW_FS_OFFSET), "lfs_config": LFS_CFG, "fs_stat": stat,
        "used_blocks": used, "files": len(fsman["files"]), "source": fsman["source"],
        "littlefs_python": littlefs.__version__, "fs_manifest_sha256": sha256_file(fsman_p),
    })
    did(f"wrote {img} ({NEW_FS_SIZE} bytes, sha256 {sha[:16]}...) and {meta_p.name}")


# ── flash-app ───────────────────────────────────────────────────────────────
def step_flash_app(ctx):
    a = ctx.args
    require_write(a)
    d = Path(a.in_dir).expanduser().resolve()
    plan("flash-app", [
        "check the rollback dump (read back), the final read, the image built from it, and app_build.json",
        f"`pio project config` again; the build artifacts must still hash as build-app recorded them",
        f"read the panel's table ({OLD_CSV} expected)",
        f"verify_flash {hx(OLD_FS_OFFSET)} spiffs_final.bin (MD5 on the chip): refuse unless the old "
        "LittleFS is still exactly the final read, i.e. the firmware has not run since",
        f"`pio run -e {a.env} -t upload --upload-port {a.port}`: bootloader 0x0, {NEW_CSV} at 0x8000, "
        "boot_app0.bin at 0xe000 (otadata -> app0), firmware at 0x10000; NVS at 0x9000 is not written",
        "then the new firmware boots once with an empty LittleFS (it formats it) until flash-fs",
    ])
    man, _ = load_backup(d, need_read_back=True)
    fmeta, final = load_final(d)
    load_image(d, man, need_final=True)
    ab_p = d / "app_build.json"
    require_files(ab_p)
    ab = read_json(ab_p)
    if ab.get("env") != a.env:
        raise Refuse(f"app_build.json is for env {ab.get('env')}, not {a.env}")
    project, build = Path(ab["project_dir"]), Path(ab["build_dir"])
    ctx.check_tools(pio=True)

    check_env_config(ctx, project, a.env)
    arts = check_artifacts(ctx, build)
    if arts != ab["artifacts"]:
        raise Refuse("the build changed since build-app; run build-app again, then final-read")
    did("build artifacts are the ones build-app checked")

    pt_file = d / "partition_table_before_flash_app.bin"
    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", hx(PT_OFFSET), hx(PT_LEN), str(pt_file)))
    which, rows = identify_table(ctx, pt_file.read_bytes())
    if which != OLD_CSV:
        print(show_rows(rows))
        if which == NEW_CSV:
            raise Refuse(f"the panel already carries {NEW_CSV}; go on with flash-fs")
        raise Refuse(f"the panel's table is not {OLD_CSV}")
    did(f"the panel still carries {OLD_CSV}")
    ctx.run(ctx.esp("default_reset", "no_reset", "verify_flash", *VERBATIM_ARGS, hx(OLD_FS_OFFSET), str(final)),
            expect="-- verify OK",
            fail="the panel's LittleFS is no longer the final read (the firmware ran since final-read). "
                 "Nothing was written. Run final-read, extract --replace and build-fs again.")
    did("the old LittleFS is still exactly the final read")

    ctx.run([ctx.pio, "run", "-e", a.env, "-t", "upload", "--upload-port", a.port], cwd=project)
    if check_artifacts(ctx, build) != arts:
        raise Mismatch("the build artifacts changed during upload; what was flashed is not what was checked")
    journal(d, "flash-app", env=a.env, artifacts=arts, final_read_sha256=fmeta["sha256"])
    did(f"uploaded; {NEW_CSV} is on the panel. Next: flash-fs --in {d} --port ... --yes-i-mean-it")


# ── flash-fs ────────────────────────────────────────────────────────────────
def step_flash_fs(ctx):
    a = ctx.args
    require_write(a)
    d = Path(a.in_dir).expanduser().resolve()
    plan("flash-fs", [
        "check the rollback dump and littlefs_32mb.bin (size, sha256, built from the current final read)",
        f"read the panel's table at {hx(PT_OFFSET)}; refuse unless it is {NEW_CSV} (otherwise "
        f"{hx(NEW_FS_OFFSET)} is still the old app1 / old filesystem)",
        f"write_flash -z {' '.join(PIO_FLASH_ARGS)} {hx(NEW_FS_OFFSET)} littlefs_32mb.bin "
        "(the arguments PlatformIO's uploadfs uses for this env)",
        f"verify_flash the same {hx(NEW_FS_SIZE)} bytes, then hard_reset",
    ])
    man, _ = load_backup(d, need_read_back=True)
    img, meta = load_image(d, man, need_final=True)
    ctx.check_tools()
    pt_file = d / "partition_table_before_flash_fs.bin"
    ctx.run(ctx.esp("default_reset", "no_reset", "read_flash", hx(PT_OFFSET), hx(PT_LEN), str(pt_file)))
    which, rows = identify_table(ctx, pt_file.read_bytes())
    if which != NEW_CSV:
        print(show_rows(rows))
        raise Refuse(f"the panel's table is {which or 'unreadable'}, not {NEW_CSV}; run flash-app first")
    did(f"the panel carries {NEW_CSV}")
    ctx.run(ctx.esp("default_reset", "no_reset", "write_flash", "-z", *PIO_FLASH_ARGS,
                    hx(NEW_FS_OFFSET), str(img)), expect="Hash of data verified.")
    did(f"wrote {img.name} at {hx(NEW_FS_OFFSET)}")
    ctx.run(ctx.esp("default_reset", "hard_reset", "verify_flash", *PIO_FLASH_ARGS,
                    hx(NEW_FS_OFFSET), str(img)), expect="-- verify OK")
    journal(d, "flash-fs", sha256=meta["sha256"], offset=hx(NEW_FS_OFFSET))
    did(f"verified {hx(NEW_FS_OFFSET)}..{hx(NEW_FS_OFFSET + NEW_FS_SIZE)}; the panel is reset and "
        "mounts the carried-over files")


# ── restore-old ─────────────────────────────────────────────────────────────
def step_restore_old(ctx):
    a = ctx.args
    require_write(a)
    d = Path(a.in_dir).expanduser().resolve()
    plan("restore-old", [
        "check the rollback dump against manifest.json (size, sha256, read back)",
        f"write_flash -z {' '.join(VERBATIM_ARGS)} 0x0 <dump>: all 32 MB back byte for byte "
        "(keep: esptool must not patch the bootloader header)",
        "verify_flash the whole 32 MB, then hard_reset; the panel is as it was at the dump, NVS included",
    ])
    _, full = load_backup(d, need_read_back=True)
    ctx.check_tools()
    ctx.run(ctx.esp("default_reset", "no_reset", "write_flash", "-z", *VERBATIM_ARGS, "0x0", str(full)),
            expect="Hash of data verified.")
    did("wrote the dump at 0x0")
    ctx.run(ctx.esp("default_reset", "hard_reset", "verify_flash", *VERBATIM_ARGS, "0x0", str(full)),
            expect="-- verify OK")
    journal(d, "restore-old", sha256=sha256_file(full))
    did("verified the whole flash against the dump")


STEPS = {"backup": step_backup, "adopt": step_adopt, "build-app": step_build_app, "final-read": step_final_read,
         "extract": step_extract, "build-fs": step_build_fs, "flash-app": step_flash_app,
         "flash-fs": step_flash_fs, "restore-old": step_restore_old}


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--framework-dir", default=str(PIO_HOME / "packages" / "framework-arduinoespressif32"),
                        help="gen_esp32part.py, the partition CSVs and boot_app0.bin come from here")
    indir = argparse.ArgumentParser(add_help=False)
    indir.add_argument("--in", "--out", dest="in_dir", required=True, help="the directory with manifest.json")
    hw = argparse.ArgumentParser(add_help=False)
    hw.add_argument("--port", required=True, help="the panel's serial port, always explicit")
    hw.add_argument("--baud", type=int, default=921600)
    hw.add_argument("--python", help="default: PlatformIO's penv python")
    hw.add_argument("--esptool", help=f"default: tool-esptoolpy {ESPTOOL_PKG_VERSION}x (esptool 4.9.0)")
    wr = argparse.ArgumentParser(add_help=False)
    wr.add_argument("--yes-i-mean-it", action="store_true", help="required for every step that writes")
    sub = p.add_subparsers(dest="step", required=True)

    s = sub.add_parser("backup", parents=[common, hw], help="read and verify the whole flash")
    s.add_argument("--out", required=True, help="a new directory for the dump")
    s = sub.add_parser("adopt", parents=[common], help="record a dump read by hand (offline)")
    s.add_argument("--dump", required=True, help="a 32 MB read_flash 0x0 0x2000000 file")
    s.add_argument("--out", required=True, help="a new directory for manifest.json and the next steps")
    s.add_argument("--reread", nargs=2, action="append", metavar=("OFFSET", "FILE"),
                   help="a second hand read at OFFSET to check the dump against (repeatable)")
    s.add_argument("--final", metavar="FILE", help="a hand read of 0xc90000 0x360000 to use as the final read")
    s = sub.add_parser("build-app", parents=[common, indir], help="pio run and check the build (offline)")
    s.add_argument("--env", required=True)
    s.add_argument("--project-dir", default=str(REPO_ROOT))
    s.add_argument("--build-dir", help="default: PROJECT/.pio/build/ENV")
    s.add_argument("--pio", help="default: PlatformIO's penv pio")
    sub.add_parser("final-read", parents=[common, indir, hw], help="read the old LittleFS, stay in download mode")
    s = sub.add_parser("extract", parents=[common, indir], help="copy the old LittleFS out (offline)")
    s.add_argument("--source", choices=("final", "dump"), default="final")
    s.add_argument("--replace", action="store_true", help="replace an earlier DIR/fs and fs_manifest.json")
    sub.add_parser("build-fs", parents=[common, indir], help="build and verify the 0x16E0000 image (offline)")
    s = sub.add_parser("flash-app", parents=[common, indir, hw, wr], help="upload firmware + new table")
    s.add_argument("--env", required=True)
    s.add_argument("--pio", help="default: PlatformIO's penv pio")
    sub.add_parser("flash-fs", parents=[common, indir, hw, wr], help="write and verify the new LittleFS image")
    sub.add_parser("restore-old", parents=[common, indir, hw, wr], help="rollback: write the whole dump back")
    return p


def main(argv=None, runner=None):
    args = build_parser().parse_args(argv)
    ctx = Ctx(args, runner or run_streaming)
    t0 = time.monotonic()
    try:
        STEPS[args.step](ctx)
    except Refuse as e:
        print(f"\nREFUSED ({args.step}): {e}")
        return 2
    except Mismatch as e:
        print(f"\nFAILED ({args.step}): {e}")
        return 1
    print(f"\n== {args.step}: OK ({time.monotonic() - t0:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

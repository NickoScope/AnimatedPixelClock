# Repartition the Waveshare panel to 32 MB

`repartition_32mb.py` moves the ESP32-S3-WROOM-2-N32R16V panel (env `matrix-waveshare-rgb`)
from arduino-esp32's `default_16MB.csv` to `large_littlefs_32MB.csv`, the table upstream
AnimatedPixelClock took in 86955e61 (v2.3.1). The LittleFS files (`/anim`, `/icons`, `/market`)
come along. OTA cannot do this: the partition table only changes over USB.

| | default_16MB.csv | large_littlefs_32MB.csv |
|---|---|---|
| nvs | 0x9000 / 0x5000 | same |
| otadata | 0xe000 / 0x2000 | same |
| app0, app1 | 0x10000, 0x650000 / 0x640000 each | 0x10000, 0x490000 / 0x480000 each |
| spiffs (LittleFS) | 0xc90000 / 0x360000 | 0x910000 / 0x16E0000 |
| coredump | 0xFF0000 / 0x10000 | 0x1FF0000 / 0x10000 |

NVS and otadata keep their places, so settings and WiFi survive. The filesystem moves and grows,
so its files are copied into a new image.

## Order

```
R="uv run --with littlefs-python==0.19.0 python tools/flash/repartition_32mb.py"
$R backup      --port P --out DIR                      # or: $R adopt --dump F --out DIR --reread 0x650000 APP1_READ
$R build-app   --in DIR --env matrix-waveshare-rgb     # offline, before the panel is held
$R final-read  --port P --in DIR                       # panel now waits in download mode
$R extract     --in DIR --replace                      # offline, well under a second
$R build-fs    --in DIR                                # offline, well under a second
$R flash-app   --in DIR --env matrix-waveshare-rgb --port P --yes-i-mean-it
$R flash-fs    --in DIR --port P --yes-i-mean-it
$R restore-old --in DIR --port P --yes-i-mean-it       # only to roll back
```

Why two reads:
- **The full dump is the rollback image only.** The firmware appends to the LittleFS while it
  runs; a re-read of 0xc90000 taken after one boot differed from the dump in blocks 720 and 721,
  a metadata pair.
- **The files to migrate come from `final-read`.** It ends `--after no_reset`, and nothing resets
  the panel until the upload in `flash-app`.
- **`flash-app` proves no boot happened.** It runs `verify_flash 0xc90000 spiffs_final.bin` (MD5
  computed on the chip) and refuses to upload if the old filesystem is no longer exactly the
  final read.

Each step prints what it will do, the exact commands, what it did, and how long it took.
Exit codes: 0 done, 1 a check or a command failed, 2 refused. The dump holds NVS (WiFi
credentials): DIR is made mode 700. Keep it out of any repository.

## What each step runs

All esptool calls go through PlatformIO's penv python and `tool-esptoolpy@2.40900.250804`
(esptool 4.9.0): `esptool.py --chip esp32s3 --port P --baud 921600 --before default_reset --after ...`.

| step | commands (after `--after`) |
|---|---|
| backup | `no_reset flash_id`; `no_reset read_flash 0x8000 0xc00 partition_table_before.bin`; `no_reset read_flash 0x0 0x2000000 flash_full.bin`; `no_reset read_flash 0xc90000 0x360000 spiffs_reread.bin`; `no_reset verify_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 flash_full.bin` |
| build-app | `pio project config --json-output`; `pio run -e ENV` |
| final-read | `no_reset read_flash 0xc90000 0x360000 spiffs_final.bin`; `no_reset verify_flash --flash_mode keep --flash_freq keep --flash_size keep 0xc90000 spiffs_final.bin` |
| flash-app | `pio project config --json-output`; `no_reset read_flash 0x8000 0xc00 ...`; `no_reset verify_flash keep×3 0xc90000 spiffs_final.bin`; `pio run -e ENV -t upload --upload-port P` |
| flash-fs | `no_reset read_flash 0x8000 0xc00 ...`; `no_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 32MB 0x910000 littlefs_32mb.bin`; `hard_reset verify_flash --flash_mode dout --flash_freq 80m --flash_size 32MB 0x910000 littlefs_32mb.bin` |
| restore-old | `no_reset write_flash -z keep×3 0x0 flash_full.bin`; `hard_reset verify_flash keep×3 0x0 flash_full.bin` |

For this env, `pio run -t upload` is:
`esptool.py --chip esp32s3 --port P --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 32MB 0x10000 firmware.bin 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin`

## Sources for the flash arguments (read in the installed packages)

- **Upload flags.** `platforms/espressif32` 6.12.0 `builder/main.py`.
  - `_get_board_flash_mode` returns `dout` for an arduino `opi_opi` board; `--flash_freq` is the board's `f_flash` (80m).
  - `--flash_size` is `board_upload.flash_size` (32MB, read back with `pio project config`); `--before` is `upload.before_reset`, absent from the board json, so `default_reset`.
  - `uploadfs` uses the same flags at `$FS_START`.
- **Extra images.** `framework-arduinoespressif32/tools/platformio-build.py` `FLASH_EXTRA_IMAGES`: bootloader at 0x0 (esp32s3), `partitions.bin` at 0x8000, `boot_app0.bin` at 0xe000; the app goes at `ESP32_APP_OFFSET` 0x10000.
- **Flags on data images.** esptool `cmds.py` `_update_image_flash_params` changes an image only at the bootloader offset (0x0 on the S3).
  - At 0x910000 and 0xc90000 the mode/freq bytes cannot change the data; `--flash_size` only sets the stub's flash size and the fit check.
  - At 0x0 they would patch the bootloader header and its SHA-256, so the rollback uses `keep` for all three.
- **NVS is not written.** esptool erases only the sectors each file covers (`cmds.py`: "Flash will be erased from ... to ..."; the stub erases as it writes).
  - `partitions.bin` is 3072 bytes at 0x8000, so it erases 0x8000-0x8fff; `boot_app0.bin` is 8192 bytes at 0xe000.
  - PlatformIO adds no `--erase-all`, and this env has no `upload_flags`. NVS at 0x9000-0xdfff stays.
- **otadata.** `boot_app0.bin` byte 0 is 0x01 (ota_seq 1) and its second sector is 0x00000000, so the bootloader picks app0.
- **Reads are checked.** Stub `read_flash` checks an MD5 digest sent by the chip (`loader.py` "Digest mismatch"). `verify_flash` compares an MD5 computed on the chip (`flash_md5sum`).
- **esptool version.** PlatformIO resolves `tool-esptoolpy @ 2.40900.250804` for this env (`pio pkg list -e matrix-waveshare-rgb`). The un-versioned `packages/tool-esptoolpy` is 4.11.0, which belongs to espressif32 6.13.0.

### `default_reset` on the native USB port enters the ROM bootloader without booting the firmware

- **Reset selection.** esptool 4.9.0 `loader.py` `_construct_reset_strategy_sequence` returns `USBJTAGSerialReset` when the port's USB PID is `USB_JTAG_SERIAL_PID = 0x1001`. That is the S3's USB-Serial/JTAG, a `/dev/cu.usbmodem*` port on macOS; the board json's hwid is 0x303A:0x1001.
- **Reset sequence.** `reset.py` `USBJTAGSerialReset.reset` sets RTS=0/DTR=0, then DTR=1 ("Set IO0"), then RTS=1/DTR=0 ("Reset"), then RTS=0/DTR=0. The ESP32-S3 Technical Reference Manual defines this:
  - Table 33.3-2: RTS=0/DTR=1 sets the download-mode flag; RTS=1/DTR=0 resets the chip.
  - A reset with the flag set "will reboot into download mode".
  - Table 33.4-3 "Reset SoC into Download Mode" lists the same order, step for step.
- **If download mode is not reached.** `_connect_attempt` fails with "Wrong boot mode detected ... The chip needs to be in download mode" instead of writing anything.
- **Leaving it there.** `--after no_reset` leaves the chip in the ROM loader ("Staying in bootloader.", `__init__.py`); the next `default_reset` repeats the download-mode reset.

## LittleFS parameters (checked, with sources)

| parameter | value | source |
|---|---|---|
| esp_littlefs | 41873c2 (v1.14.1) | `framework-arduinoespressif32/tools/sdk/versions.txt` |
| block_size | 4096 | `esp_littlefs.c@41873c2`: `#define CONFIG_LITTLEFS_BLOCK_SIZE 4096`, `cfg.block_size = CONFIG_LITTLEFS_BLOCK_SIZE` |
| read/prog size | 128 / 128 | `tools/sdk/esp32s3/opi_opi/include/sdkconfig.h` `CONFIG_LITTLEFS_READ_SIZE`, `_WRITE_SIZE`; `esp_littlefs_init_efs` |
| cache / lookahead / block_cycles | 512 / 128 / 512 | same sdkconfig; runtime only, not on disk |
| block_count | 5856 (0x16E0000 / 4096) | `cfg.block_count = 0` (from the superblock); `LittleFS.cpp` mounts with `grow_on_mount = true`. The image is full size, so the grow is a no-op (checked) |
| name_max | 255 | `esp_littlefs.c` never sets `cfg.name_max`, so littlefs uses `LFS_NAME_MAX`. `lfs_init` in the framework's `libesp_littlefs.a` loads 255 (file_max 0x7fffffff, attr_max 1022). `CONFIG_LITTLEFS_OBJ_NAME_LEN 64` only sizes VFS path buffers |
| disk version | 2.1 (0x00020001) | no `CONFIG_LITTLEFS_MULTIVERSION` in sdkconfig, so `lfs_fs_disk_version` returns `LFS_DISK_VERSION` of littlefs f53a0cc (v2.9) `lfs.h`: 0x00020001 |
| mtime | user attribute `'t'` | `esp_littlefs.c` `LITTLEFS_ATTR_MTIME`, its only `lfs_setattr`; the tool copies every user attribute |

littlefs-python 0.19.0 bundles littlefs 2.11 (disk version 2.1). The v2.10 and v2.11 release
notes list no on-disk format change. `disk_version=0x00020001` is passed explicitly, and
littlefs-python honours it (tested). A mount with a larger `name_max` than the reader's fails with
`LFS_ERR_INVAL` (tested), so the image must not exceed the panel's 255. The real panel's own
superblock reads name_max 255, disk version 2.1, block_count 864.

## Safety design

- **Port and consent.** The port is always explicit. Steps that write need `--yes-i-mean-it`, and a rollback dump that was read back: `backup` does that itself; `adopt` needs `--reread OFFSET FILE` of a region the firmware does not write.
- **Refusals before anything is written.**
  - A dump, final read or image differs from its recorded SHA-256.
  - The image was built from the dump instead of the current final read.
  - The build changed since `build-app`.
  - `partitions.bin` is not the 32 MB table.
  - The firmware is larger than app0 (0x480000).
  - The bootloader or app header does not say 32MB.
  - The panel's table is not the expected one.
  - The old filesystem is no longer the final read.
- **`extract` never formats.** It mounts through a block device that refuses and counts every write. It also refuses paths that would collide on a case-insensitive Mac disk.
- **`build-fs` mounts the image back.** It uses `block_count 0`, as the panel does, then compares superblock, tree, bytes and attributes, and checks that grow-on-mount changes nothing.
- **Between `flash-app` and `flash-fs`.** The new firmware boots once with no valid LittleFS at 0x910000. `LittleFS.begin(true)` formats it; `flash-fs` then overwrites the whole partition.

## Not verified on hardware yet

- **The reset chain.** The USB reset chain above is read from esptool and the TRM, not observed on this panel. If the firmware did run, `flash-app`'s `verify_flash` refuses.
- **`--baud 921600`.** Irrelevant on USB-Serial/JTAG (Table 33.3-1: line coding is ignored).
- **Write time.** How long the 23.9 MB `write_flash` takes is unmeasured.

## Tests

```
uv run --with littlefs-python==0.19.0 python -m unittest tools/flash/test_repartition_32mb.py -v
```

The tests synthesise an old-layout 32 MB dump: the `default_16MB.csv` table from `gen_esp32part.py`
at 0x8000, and at 0xc90000 a 0x360000 LittleFS with `/anim/a.pca` (300 KB), `/anim/b.pca` (1 MB),
`/icons/sun`, `/market/last.bin` (87 KB), an empty directory and mtime attributes. A fake
esptool/pio acts on it in memory and records the exact command lines; `subprocess` is patched to
fail if anything reaches it.

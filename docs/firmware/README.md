# Firmware and companion releases

Run `python release.py` from the repository root. It reads `FIRMWARE_VERSION`
from `src/config/config.h`, builds both targets, validates the flash layout,
and packages the firmware with the prebuilt Windows companion. It does not
commit, push, or publish a GitHub Release.

| File ID | PlatformIO environment | Hardware |
| --- | --- | --- |
| `waveshare` | `matrix-waveshare-rgb` | Waveshare ESP32-S3-RGB-Matrix (WROOM-2 N32R16V), 32 MB octal flash + 16 MB octal PSRAM, native USB |
| `supermini` | `matrix-s3` | ESP32-S3-Zero / Super Mini, 4 MB, native USB |
| `wroom` | `matrix-s3-wroom` | ESP32-S3-WROOM-1 N16R8, 16 MB, USB-UART |

The `waveshare` image is not interchangeable with `wroom`. That module carries
octal flash and needs a bootloader built for it; the 16 MB devkit image writes
cleanly to this board and then dies in `do_core_init` on every boot, right
after "Octal Flash Mode Enabled". Packaging checks the flash size declared in
each bootloader header against the board, so a mislabelled image is rejected
here rather than discovered on a wall.

The `supermini` filename is retained for compatibility; it also covers the
Waveshare ESP32-S3-Zero. Choose the image for your board and flash size.

## Packaging

1. Set the intended firmware version in `src/config/config.h`.
2. Build and test the Windows companion using
   `PC-Companion-App-v4/win-companion/build_exe.bat`.
3. Run `python release.py`. The script includes the EXE from
   `PC-Companion-App-v4/win-companion/dist/pc_stats_monitor_v4.exe`.
   Use `--companion PATH` to package another tested build.
4. Review the files and `SHA256SUMS.txt` in `release/v<version>/`.

`--skip-build` reuses existing firmware and still validates it and refreshes
PlatformIO flash metadata. Use it only when those binaries match the source.
An optional version argument must match `FIRMWARE_VERSION`; it cannot silently
relabel an older firmware build.

Full images contain the bootloader at `0x0`, partition table at `0x8000`, OTA
initialization (`boot_app0.bin`) at `0xe000`, and application at `0x10000`.
Offsets and files come from PlatformIO's generated metadata. Packaging rejects
overlapping parts, mismatched chip/flash sizes and images too large for OTA slots.

## Outputs

The web flasher reads `docs/firmware/latest/VERSION` and writes, for the
Waveshare board, four parts at their own offsets and nothing between them:

```text
AnimatedPixelClock-waveshare-v<version>-bootloader.bin   0x0
AnimatedPixelClock-waveshare-v<version>-partitions.bin   0x8000
AnimatedPixelClock-waveshare-v<version>-otadata.bin      0xE000
OTA_ONLY_firmware-v<version>-waveshare.bin               0x10000
AnimatedPixelClock-waveshare-v<version>-Full.bin         (the merged image, for writing by hand)
SHA256SUMS.txt
```

The settings partition (NVS, 0x9000-0xE000) lies between the partition table
and the OTA data. The merged Full.bin fills that gap with 0xFF, so writing it
wipes the settings, the Wi-Fi and the keys even without an erase; the parts do
not touch it (2026-09-30, tested on a board). The portal's Update now downloads
the OTA_ONLY image from here too.

These files are committed under `docs/` and published by GitHub Pages from
`main:/docs`. The filename IDs match `BOARDS` in `docs/flasher.js`.

The GitHub Release assets are prepared in `release/v<version>/`:

```text
firmware-v<version>-supermini.bin
firmware-v<version>-wroom.bin
OTA_ONLY_firmware-v<version>-supermini.bin
OTA_ONLY_firmware-v<version>-wroom.bin
pc_stats_monitor_v4.exe
SHA256SUMS.txt
```

- **New device / USB installation:** use the web flasher, or flash the full
  `firmware-*.bin` at `0x0`. Writing the full image by hand wipes the saved
  settings, the Wi-Fi and the keys even without an erase; the web flasher's
  "don't erase" keeps them.
- **Existing device / WiFi update:** upload the matching `OTA_ONLY_*.bin` through
  the clock's Firmware Update page. Do not upload a full image as an OTA update.
- **Windows:** download and run the EXE. No Python installation is needed.

The release-directory EXE copy is gitignored to avoid storing it twice; upload
it as a release asset. The original EXE remains tracked in `win-companion/dist`.

## Publishing

Commit the source, flasher updates, `docs/firmware/latest` and release BINs/checksums.
Push `main`, tag that exact commit as `v<version>`, and create a GitHub Release
with all six assets listed above. Use a draft until every upload is complete,
then publish it and mark stable releases as latest.

The flasher's Windows download points to that same tag's `pc_stats_monitor_v4.exe`.
Keep that asset name stable. Check the public page, both BIN downloads, the EXE
download and their SHA-256 hashes after GitHub Pages finishes deploying.

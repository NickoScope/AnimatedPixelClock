/*
 * AnimatedPixelClock - Clip gallery on the TF card
 *
 * Clips too long for the flash store live on the card, one PCA1 file each in
 * /clips, and play from it. A reader task on core 0 keeps the chosen file
 * open and reads frames ahead into a ring in PSRAM (clip_stream.h); the render
 * loop only takes the next frame when it is due. No card I/O happens on the
 * render path, and a slow read costs a held frame, never a stalled one.
 *
 * The slot is wired for SD 1-bit only: CLK GPIO1, CMD GPIO44, D0 GPIO17, per
 * Waveshare's BSP config.h as recorded in the bring-up notes
 * (docs/02-controller.md). Mounted on the panel 2026-09-14 with those pins
 * at 20 MHz, 1-bit: a 32 GB SDHC card, FAT. The HUB75 map, the encoder
 * (45/46/0) and USB-CDC leave all three free. The card must be FAT: this
 * ESP-IDF (4.4.7) is built without exFAT (ffconf.h FF_FS_EXFAT 0), so an
 * exFAT card does not mount.
 *
 * settings.ambientCustomFile names a card clip as "sd:<name>" (3 + 24 + NUL
 * fits its 28 bytes); anything else is a clip in the flash store.
 */

#ifndef CLIP_SD_H
#define CLIP_SD_H

#if defined(CLIPS_SD_ENABLED) && !defined(CONTROL_ENCODER_ENABLED)
#error "CLIPS_SD_ENABLED needs CONTROL_ENCODER_ENABLED: its routes and its page are the portal's Panel group"
#endif
#if defined(CLIPS_SD_ENABLED) && !defined(BOARD_WAVESHARE_RGB_MATRIX)
#error "CLIPS_SD_ENABLED is wired for the TF slot of the Waveshare ESP32-S3-RGB-Matrix (BOARD_WAVESHARE_RGB_MATRIX)"
#endif

#include <Arduino.h>
#include <FS.h>

#include "../ambient/anim_store.h"

#define CLIP_SD_DIR "/clips"
#define CLIP_SD_TMP "/clips/upload.tmp"
#define CLIP_SD_REF "sd:"

// The most frames a card clip may hold: 12000, 8:00 at 25 fps, 10:00 at 20.
// - PCA1 counts frames in a u16: the format stops at 65535 (43:41 at 25 fps,
//   268.6 MB), so a whole track needs no format extension.
// - FAT32 stops a file at 4 GiB - 1, far above either.
// - What binds is the phone and the time: the clip is rendered in the
//   browser's memory and sent over Wi-Fi. The longest track the owner has,
//   Oscilloscope Music's "Deconstruct", runs 7:40.5 (11512 frames). 12000
//   frames are 49.2 MB: about 40 s of card writes at the 1221 KB/s measured
//   on the panel 2026-09-14, plus the Wi-Fi, which is not measured.
#define CLIP_SD_MAX_FRAMES 12000
#define CLIP_SD_MAX_BYTES \
  (PCA_HEADER_BYTES + PCA_MAX_PALETTE * 2UL + CLIP_SD_MAX_FRAMES * (2UL + PCA_FRAME_BYTES))
// Kept free on the card: a clip's last cluster and the directory's growth.
#define CLIP_SD_FREE_MARGIN (1024UL * 1024UL)

enum ClipSdState : uint8_t { CLIP_SD_IDLE, CLIP_SD_PLAYING, CLIP_SD_FAILED };

struct ClipSdStats {
  ClipSdState state;
  char clip[25];
  uint32_t frames, reads, readUsAvg, readUsMax, underruns, loops, queued;
};

// Mount at boot, or again later (a card put in after boot). False with a
// reason when there is no card, it does not mount, or there is no PSRAM.
void clipSdInit();
bool clipSdMount();
bool clipSdMounted();
const char* clipSdReason();
const char* clipSdCardType();
uint64_t clipSdTotalBytes();
// Cached: FAT's free count can mean reading the whole table, so it is taken at
// mount and after a write or a delete (clipSdRefresh), never in a poll.
uint64_t clipSdFreeBytes();
void clipSdRefresh();

fs::FS& clipSdFs();
String clipSdPath(const char* name);          // "/clips/<name>.pca"
bool clipSdIsRef(const char* setting);        // "sd:..."

// Render side. Play starts the reader and returns at once; stop closes the
// file and waits for the reader to say so (at most 3 s) - call it before the
// file is deleted or replaced. Take only succeeds while PLAYING for the last
// request.
void clipSdPlay(const char* name);
void clipSdStop();
ClipSdState clipSdState();
bool clipSdTake(uint8_t* frame, uint16_t* delayMs);
void clipSdMissed();
const uint16_t* clipSdPalette();
ClipSdStats clipSdStats();

#endif  // CLIP_SD_H

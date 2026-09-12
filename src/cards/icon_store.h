#pragma once
// Icons for cards, kept in the filesystem rather than the firmware.
//
// AWTRIX uses 8x8, sized for a 32x8 display. We have 128x64, so 16x16 is the
// honest equivalent: big enough to be a picture rather than a glyph, small
// enough that one fits in a single MQTT message.
//
//   nickoscope_matrix/icon/<name>   512 bytes, raw RGB565 big-endian, retained
//                                   an empty payload deletes it
//
// 16 x 16 x 2 bytes = 512, so the whole icon arrives in one publish with no
// chunking and no base64 - MQTT payloads are binary-safe, and base64 would have
// cost a third more for nothing.

#include <stdint.h>
#include <stddef.h>

#if defined(CARDS_ENABLED)

#define ICON_W    16
#define ICON_H    16
#define ICON_PX   (ICON_W * ICON_H)
#define ICON_BYTES (ICON_PX * 2)

void iconStoreBegin();

// Writes atomically: a half-written icon is worse than a missing one, because
// it looks like a corrupted panel rather than a missing file.
bool iconSave(const char *name, const uint8_t *data, size_t len);
void iconRemove(const char *name);

// Loads into a shared one-icon cache and returns it, or NULL. Repeated calls
// for the same name do not touch the filesystem - this runs once per frame.
const uint16_t *iconGet(const char *name);

#endif

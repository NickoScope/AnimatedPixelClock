#pragma once
// Boot health: an image flashed over the air is confirmed only once it has
// run, so the bootloader can put the previous one back if it has not; and the
// crash report the SDK keeps in flash is read out at boot, kept in NVS and
// shown in /api/info. See boot_health.cpp.

#include <ArduinoJson.h>

// setup(), right after Serial: reads the running image's OTA state and the
// crash report in flash, if there is one.
void healthBegin();

// Every loop(). displayIdle: nothing is expected on the panel (no display, or
// switched off by schedule), so a drawn page is not asked for.
void healthTick(bool displayIdle);

// After each display flip: counts drawn frames towards confirming the image.
void healthNoteFrame();

// For /api/info: "ota" (slot, state, confirmation) and "lastCrash".
void healthInfoJson(JsonObject out);

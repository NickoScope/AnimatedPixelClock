/*
 * AnimatedPixelClock - Settings Module Header
 *
 * Declarations for settings persistence functions.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "../config/config.h"
#include <Preferences.h>

// Initialize settings (load from NVS or set defaults)
void loadSettings();

// Save current settings to NVS
void saveSettings();

// Save the clock style alone: one NVS key, for the knob and the portal's style
// switch. saveSettings() writes 136 keys.
void saveClockStyle();
bool saveBrightnessSetting();   // the "brightness" key only
int16_t savedBrightnessNvs();   // NVS now: -1 cannot open, -2 no key
int16_t lastBrightnessSave();   // -1 never, 0 failed, else value + 1

// Brightness helpers
uint8_t sanitizeBrightnessValue(uint8_t value);
bool isZeroBrightnessAllowed();
void sanitizeBrightnessSettings();

extern Preferences preferences;

#endif // SETTINGS_H

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

// Brightness helpers
uint8_t sanitizeBrightnessValue(uint8_t value);
bool isZeroBrightnessAllowed();
void sanitizeBrightnessSettings();

extern Preferences preferences;

#endif // SETTINGS_H

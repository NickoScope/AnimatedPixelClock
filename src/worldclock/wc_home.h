#pragma once
// Home when nobody has chosen one: the city where the panel is.
//
// In order, each step only when the one before has nothing to say:
//   1. the owner's choice, made in the portal and kept in NVS by src/panel.
//      It always wins, and nothing below runs while it stands;
//   2. the weather location (settings.weatherLat/Lon): the built-in or custom
//      city within HOME_RADIUS_KM of it, else a city made there, named HOME,
//      in the panel's own zone;
//   3. the panel's public IP, looked up over HTTPS at most once per boot, and
//      only when there is no choice and no location;
//   4. until that answers, or if it fails: the built-in city whose UTC offset
//      is the panel's own zone's right now.
// The reasons for each are in src/worldclock/README.md.

#include <stdint.h>

#if defined(WORLDCLOCK_ENABLED)

void        wcHomeBegin();     // setup(), from panelBegin() once the stored choice is applied
void        wcHomeTick();      // loop(), from panelTick(): comparisons only, unless something moved
void        wcHomeRethink();   // a choice was forgotten, or a custom city came or went
const char *wcHomeSource();    // "chosen", "location", "ip" or "zone"

#endif

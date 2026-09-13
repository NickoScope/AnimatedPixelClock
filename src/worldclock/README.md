# World clock

A dotted world map on the panel's own grid. 128×64 divides exactly into 64×32
dots on a 2 px pitch, so the look needs no rescaling. Land in daylight is lit,
night is dim, and civil twilight (sun between 0 and −6°) blends between them,
so the terminator draws itself and creeps across the map through the day.
HH:MM sits in the empty South Pacific; cities are orange 2×2 dots, and the
first one, home, breathes.

![](../../tools/luasim/preview/world_clock.png)

## Where it lives

- Long press on the knob: the page after the clock. The carousel gives it 20 s.
- Build flag `WORLDCLOCK_ENABLED`; needs `CONTROL_ENCODER_ENABLED` (`#error` otherwise).

## One source

The land mask and the city list are written by
[`tools/luasim/gen_world.py`](../../tools/luasim/gen_world.py) into both
`worldmap.h` here and the Lua prototype
[`world_clock.lua`](../../tools/luasim/scripts/world_clock.lua). To change the
cities, edit `CITIES` there and rerun it. The pre-commit hook runs
`gen_world.py --check`, which needs no network, and refuses a commit where the
two copies disagree.

Cities today: Cannes (home), Moscow, New York, London, Dubai, Almaty.

## How it was checked

- The C module was compiled on the host with a stand-in display and rendered
  at 23:07 CEST on 13 September; after the same RGB565 step, it matches the Lua
  frame pixel for pixel (0 of 8 192 differ).
- The Lua frame was checked against the reference picture: the Americas lit,
  Europe, Africa and most of Asia dark, eastern Australia in morning.
- Not yet on hardware.

## Cost

Measured against the same build without the flag: **+2 284 B flash,
+4 096 B RAM**.

- The mask is 32 × `uint64_t`, 256 bytes of flash.
- The RAM is the colour table: 32×64 `uint16_t`, computed once a minute, so a
  frame is 774 pixel writes and no trigonometry.
- Refresh drops to 10 fps on this page: only the home dot moves.

## Honest limits

- Declination by a cosine approximation, sun longitude straight from UTC. The
  equation of time moves the line by up to 4°; a dot is 5.6° wide.
- Before NTP sync the map is all night and the time reads `--:--`, rather than
  drawing 1970 with confidence.

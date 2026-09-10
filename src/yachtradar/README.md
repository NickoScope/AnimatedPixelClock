# Yacht Radar

Live AIS vessels in the Bay of Cannes. Left 64x64 is the plot, right 64x64 the
vessel table.

Ported from NickoScope32 V1.b:

| From | What was taken |
|---|---|
| `Main-S3 v33.58.0` `src/iot/yacht_radar.cpp` | the AISstream client - same ESP32-S3, so the transport is proven on this silicon |
| `H743 v46.78.0` `src/effects/fx34_yacht_radar.cpp` | the visual design and the coastline data |

## What changed in the port

fx34 draws on a CRT in DAC space and floats a badge over each hull as the sweep
crosses it. 128x64 has no room to float anything, so the panel splits: plot on
the left, and the badges become a standing table on the right, nearest first.

The centre (43.5030 N, 7.0050 E), the 300 DAC/km scale and the 1900 DAC outer
ring are fx34's, unchanged - the coastline was clipped to them, so moving either
would put the shoreline in the wrong place.

## The chart is baked, not drawn

fx34 draws a wireframe because a CRT can only draw lines. This panel is a
raster with 8 bits per channel, so the map is a **map**: sea coloured by
measured depth, land by measured elevation with a north-west hillshade, and the
shoreline drawn over the top with anti-aliasing.

None of that changes, so none of it runs on the panel. It is resolved at build
time into one 64x64 RGB565 array:

```
  fx34_yacht_radar.cpp ──yr_coastline_gen.py──> coastline.h    500 B, DAC precision
  coastline.h + terrain.json ──yr_basemap_gen.py──> basemap.h  8192 B, RGB565
```

`tools/yr_data/terrain.json` holds the measured terrain: land elevation from
EU-DEM 25 m (gaps filled from SRTM 30 m) and sea depth from GEBCO 2020, both via
opentopodata, fetched 2026-09-10. It is committed so the map can be rebuilt
without network access. Within the frame the land reaches 269 m - the Esterel
behind Theoule, which is why high ground is shaded toward red porphyry rather
than grey - and the sea reaches 1323 m.

Note the coastline is kept in **DAC units, not rounded to pixels**. Rounding at
generation time would throw away the sub-pixel position before anything could
use it; on a 64 px chart that sub-pixel position, carried into pixel
brightness, is the only resolution left to spend. Vessels are drawn the same
way: the basemap is in flash, so the pixel underneath is known exactly and a
vessel can blend a soft halo against it instead of snapping to the grid.

Regenerating the coastline needs the NickoScope32 baseline firmware checked
out; regenerating the basemap needs only what is in this repo.

```bash
python3 tools/yr_coastline_gen.py <path>/NickoScope32-v1B-H743-*/src/effects/fx34_yacht_radar.cpp
python3 tools/yr_basemap_gen.py --png /tmp/preview.png
```

The map is held deliberately dim. Vessels must be the brightest thing on the
page, and on an LED panel a bright 64x64 fill is real current.

## The AIS key is not in this repository

It lives in NVS, namespace `yr`, key `ais`. Without it the page says `NO AIS KEY`
rather than pretending to scan.

```cpp
Preferences p; p.begin("yr", false); p.putString("ais", "<key>"); p.end();
```

## Why a vessel appears before its static data

AIS `ShipStaticData` carries type and length but is event-driven and rare -
roughly every six minutes for a vessel under way. AISstream puts the name in
`MetaData` on every message. Waiting for static data therefore means an empty
screen over a bay full of boats; the NickoScope32 bridge measured exactly that
(four named vessels, zero static messages, seven minutes). So a vessel is
plotted as soon as position and name are known, and type and length refine the
entry when they arrive.

## Cost, measured

| | |
|---|---|
| Flash | +31.1 KB (1 617 625 -> 1 649 461 bytes), of which 8 KB is the baked chart |
| Static RAM | +1 120 bytes |

TLS session heap is **not** in those numbers and is not yet measured - it is
allocated at runtime when the socket opens. The stream is held open only while
the page is up for that reason.

Nothing here is hardware-verified: the panels have not arrived.

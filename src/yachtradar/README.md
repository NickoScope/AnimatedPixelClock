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

## Coastline

`coastline.h` is generated, 125 points in 9 segments, 250 bytes:

```bash
python3 tools/yr_coastline_gen.py <path>/NickoScope32-v1B-H743-*/src/effects/fx34_yacht_radar.cpp
```

It needs the NickoScope32 baseline firmware checked out, so it is not part of
the normal build - regenerate only if the radar frame changes.

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
| Flash | +22.9 KB (1 617 625 -> 1 641 057 bytes) |
| Static RAM | +1 120 bytes |

TLS session heap is **not** in those numbers and is not yet measured - it is
allocated at runtime when the socket opens. The stream is held open only while
the page is up for that reason.

Nothing here is hardware-verified: the panels have not arrived.

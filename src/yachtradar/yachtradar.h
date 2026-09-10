#pragma once
// Yacht Radar - live AIS vessels in the Bay of Cannes on a 128x64 panel.
//
// Ported from NickoScope32: the AIS client from Main-S3 iot/yacht_radar.cpp
// (same ESP32-S3, so the transport is proven on this silicon) and the visual
// design from H743 effects/fx34_yacht_radar.cpp. What changed is the canvas:
// fx34 draws on a CRT in DAC space with sweep-triggered badges floating over
// the plot; 128x64 has no room to float anything, so the panel is split -
// radar on the left 64x64, a vessel table on the right.
//
// The AIS key is never compiled in. It lives in NVS under namespace "yr",
// key "ais"; without it the page says so instead of pretending to scan.

#include <stdint.h>

#if defined(YACHTRADAR_ENABLED)

#define YR_MAX_VESSELS  16     // tracked; the radar plots all of them
#define YR_TABLE_ROWS    6     // (64 - YR_Y_ROW0) / YR_ROW_H
#define YR_NAME_LEN     20

enum YrMotion : uint8_t {      // what the hull is doing, drives colour
  YR_ANCHORED = 0,             // SOG < 0.5 kn
  YR_MANOEUVRE,                // 0.5 .. 3 kn
  YR_UNDERWAY,                 // > 3 kn
};

// Open the AIS stream. Cheap to call repeatedly; only the first opens a socket.
// Returns false when no key is stored, which the page renders as a message.
bool yachtRadarBegin();

// Pump the websocket. Must be called from the main loop while the page is up.
void yachtRadarLoop();

// Close the socket. Called when the page is left, so the stream is not held
// open for a display nobody is looking at - the same gating fx34 uses.
void yachtRadarStop();

void     yachtRadarRender();

// Encoder handles. Rotation scrolls the table when more vessels are in the bay
// than fit; the press swaps what the list is sorted by, because "what is
// nearest" and "what is biggest" are the two questions the bay actually raises.
void     yachtRadarScroll(int8_t delta);
void     yachtRadarToggleSort();
bool     yachtRadarSortsBySize();
bool     yachtRadarHasData();
uint8_t  yachtRadarCount();      // vessels currently on the plot
uint32_t yachtRadarAge();        // ms since the last position report

#endif  // YACHTRADAR_ENABLED

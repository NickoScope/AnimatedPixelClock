#pragma once
// The "no WiFi" icon (8x8 pixels): a WiFi symbol with a diagonal cross, on any
// Adafruit GFX target. drawNoWiFiIcon() (clock_globals.cpp) draws it on the
// panel for every clock; the weather screen's layout draws it through this
// template so the host check runs the same calls. tools/climate/render.py
// executes the calls below from this file.

#ifndef DISPLAY_WHITE
#define DISPLAY_WHITE 0xFFFF
#endif

template <class G>
void drawNoWiFiIconOn(G &g, int x, int y) {
  // WiFi arcs (signal strength bars)
  // Small arc (closest to antenna)
  g.drawPixel(x + 3, y + 5, DISPLAY_WHITE);
  g.drawPixel(x + 4, y + 5, DISPLAY_WHITE);

  // Medium arc
  g.drawPixel(x + 2, y + 4, DISPLAY_WHITE);
  g.drawPixel(x + 5, y + 4, DISPLAY_WHITE);
  g.drawPixel(x + 2, y + 3, DISPLAY_WHITE);
  g.drawPixel(x + 5, y + 3, DISPLAY_WHITE);

  // Large arc (outer signal)
  g.drawPixel(x + 1, y + 2, DISPLAY_WHITE);
  g.drawPixel(x + 6, y + 2, DISPLAY_WHITE);
  g.drawPixel(x + 0, y + 1, DISPLAY_WHITE);
  g.drawPixel(x + 7, y + 1, DISPLAY_WHITE);

  // Center dot (antenna/device)
  g.fillRect(x + 3, y + 6, 2, 2, DISPLAY_WHITE);

  // Diagonal cross (X through the icon to indicate "no connection")
  g.drawLine(x, y, x + 7, y + 7, DISPLAY_WHITE);
}

// Host run of the weather clock's drawing, src/clocks/weather_layout.h, on the
// real Adafruit GFX library (Adafruit_GFX.cpp and glcdfont.c as PlatformIO
// installed them). Reads scenes, one per line, from the file named on the
// command line (tools/climate/check_weather_screen.py writes it from
// render.py's scenes) and prints for each the 128x64 raster in RGB565, the
// number of pixels drawn off the panel, and the strings the layout drew.
//
// Scene fields, space separated:
//   name haveTime hour minute colon h12 pm wifi nowMs
//   digitColor iconColor accentColor tempColor (hex RGB565)
//   state(0 not set up, 1 fetching, 2 ready) icon tempC maxC minC humidity sunrise sunset fahrenheit
//   indoor(0 none, 1 live, 2 stale) inTempC inHumidity

#include <Adafruit_GFX.h>

#include <cstdio>
#include <cstring>

#include "weather_layout.h"

namespace {

class HostCanvas : public Adafruit_GFX {
 public:
  HostCanvas() : Adafruit_GFX(128, 64) { std::memset(px, 0, sizeof(px)); }
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= 128 || y >= 64) {
      off++;
      return;
    }
    px[y][x] = color;
  }
  uint16_t px[64][128];
  unsigned off = 0;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s scenes.txt\n", argv[0]);
    return 2;
  }
  FILE *in = std::fopen(argv[1], "r");
  if (!in) {
    std::perror(argv[1]);
    return 2;
  }
  char buf[512];
  int scenes = 0;
  while (std::fgets(buf, sizeof(buf), in)) {
    if (buf[0] == '#' || buf[0] == '\n') continue;
    char name[64], rise[6], set[6];
    int haveTime, hour, minute, colon, h12, pm, wifi, state, icon, rh, fahr, indoor;
    unsigned now, cDigit, cIcon, cAccent, cTemp;
    float t, hi, lo, inT, inRh;
    const int n = std::sscanf(buf, "%63s %d %d %d %d %d %d %d %u %x %x %x %x %d %d %f %f %f %d %5s %5s %d %d %f %f",
                              name, &haveTime, &hour, &minute, &colon, &h12, &pm, &wifi, &now, &cDigit, &cIcon,
                              &cAccent, &cTemp, &state, &icon, &t, &hi, &lo, &rh, rise, set, &fahr, &indoor, &inT,
                              &inRh);
    if (n != 25) {
      std::fprintf(stderr, "bad scene line (%d fields): %s", n, buf);
      return 2;
    }

    WeatherScreen s{};
    s.haveTime = haveTime;
    s.ntpSynced = true;
    s.hour = (uint8_t)hour;
    s.minute = (uint8_t)minute;
    s.colon = colon;
    s.h12 = h12;
    s.pm = pm;
    s.wifi = wifi;
    s.nowMs = now;
    s.digitColor = (uint16_t)cDigit;
    s.iconColor = (uint16_t)cIcon;
    s.accentColor = (uint16_t)cAccent;
    s.tempColor = (uint16_t)cTemp;
    s.state = (WeatherState)state;
    s.icon = (WeatherIconKind)icon;
    s.tempC = t;
    s.tempMaxC = hi;
    s.tempMinC = lo;
    s.humidity = rh;
    std::memcpy(s.sunrise, rise, sizeof(s.sunrise));
    std::memcpy(s.sunset, set, sizeof(s.sunset));
    s.fahrenheit = fahr;
    s.indoor = (climate::WeatherIndoor)indoor;
    s.inTempC = inT;
    s.inHumidity = inRh;

    static HostCanvas *g;
    g = new HostCanvas();   // a fresh GFX state per scene: size 1, white, cursor 0,0
    WeatherScreenNotes notes;
    drawWeatherScreen(*g, s, &notes);

    std::printf("FRAME %s %u\n", name, g->off);
    for (int y = 0; y < 64; y++) {
      for (int x = 0; x < 128; x++) std::printf("%04x", g->px[y][x]);
      std::printf("\n");
    }
    std::printf("NOTES %s\t%s\t%s\t%s\t%s\t%d\t%d\n", name, notes.outdoor, notes.details, notes.inTemp, notes.inHum,
                notes.split ? 1 : 0, notes.unitLetter ? 1 : 0);
    delete g;
    scenes++;
  }
  std::fclose(in);
  std::printf("SCENES %d\n", scenes);
  return 0;
}

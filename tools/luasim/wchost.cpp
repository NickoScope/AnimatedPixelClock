// ============================================================
// wchost.cpp - the world clock page, run on the host
// ============================================================
// Compiles src/worldclock/worldclock.cpp and posix_tz.cpp - the files the
// panel runs - against a stand-in display (host/wc_host.h), and drives
// worldClockDraw() with exactly the clock luasim hands world_clock.lua, so
// fx_parity.py can compare the page with its prototype. The command line is
// luasim's; the script argument is ignored, the page being compiled in:
//
//   wchost world_clock.lua frames out.raw [--start HH:MM] [--yday N] [--utc H]
//          [--year Y] [--sweep] [--home ID] [--custom "NAME|lat|lon|POSIX"]
//          [--settled] [--always]
//
//   --home ID     the page's city id (0.. built in, 100 the custom city)
//   --custom      fills custom slot 0, id 100
//   --settled     home changed long ago: the name holds still
//   --always      the name breathes on every frame, for the design preview
//   --fps N       real time for previews (wc_preview.py): frame f is f/N seconds
//                 after home changed, instead of luasim's phase clock
//
// Frames are RGB888, spread from the page's RGB565 as the panel's driver does.
//
//   wchost --tz   reads "POSIX<TAB>utc" lines, writes the offset east of UTC
//                 in seconds, or "reject", one line each
//   wchost --fit  reads UTF-8 place names, writes worldClockFitName's answer
//                 and its width in pixels, "NAME|px", one line each
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "posix_tz.h"
#include "wc_host.h"
#include "worldclock.h"

WcHostDisplay display;
uint32_t millis() { return 0; }

static int tzMode() {
  static char line[512];
  while (fgets(line, sizeof(line), stdin)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = '\0';
    PosixTz tz;
    if (!posixTzParse(line, &tz)) { puts("reject"); continue; }
    printf("%d\n", (int)posixTzOffset(tz, strtoll(tab + 1, nullptr, 10)));
  }
  return 0;
}

static int fitMode() {
  static char line[512];
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\n")] = '\0';
    char out[WC_NAME_MAX + 1];
    worldClockFitName(line, out, sizeof(out));
    printf("%s|%d\n", out, worldClockNameWidth(out));
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--tz")) return tzMode();
  if (argc >= 2 && !strcmp(argv[1], "--fit")) return fitMode();
  if (argc < 4) {
    fprintf(stderr, "usage: wchost script.lua frames out.raw [--start HH:MM] [--yday N] [--utc H] "
                    "[--year Y] [--sweep] [--home ID] [--custom NAME|lat|lon|POSIX] [--settled] [--always]\n"
                    "       wchost --tz < lines\n");
    return 2;
  }
  const int frames = atoi(argv[2]);
  int startMin = 12 * 60 + 34, yday = 255, utcH = 2, year = 2026, home = 0, fps = 0;
  bool sweep = false, settled = false, always = false;
  const char *custom = nullptr;
  for (int a = 4; a < argc; a++) {
    if (!strcmp(argv[a], "--start") && a + 1 < argc) {
      int hh = 0, mm = 0; sscanf(argv[++a], "%d:%d", &hh, &mm); startMin = hh * 60 + mm;
    } else if (!strcmp(argv[a], "--yday") && a + 1 < argc)   yday = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--utc") && a + 1 < argc)      utcH = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--year") && a + 1 < argc)     year = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--home") && a + 1 < argc)     home = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--fps") && a + 1 < argc)      fps = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--custom") && a + 1 < argc)   custom = argv[++a];
    else if (!strcmp(argv[a], "--sweep"))                    sweep = true;
    else if (!strcmp(argv[a], "--settled"))                  settled = true;
    else if (!strcmp(argv[a], "--always"))                   always = true;
  }

  if (custom) {
    WcCity c;
    memset(&c, 0, sizeof(c));
    char name[64] = "", posix[128] = "";
    if (sscanf(custom, "%63[^|]|%f|%f|%127[^|]", name, &c.lat, &c.lon, posix) != 4) {
      fprintf(stderr, "--custom wants NAME|lat|lon|POSIX\n");
      return 2;
    }
    strncpy(c.name, name, sizeof(c.name) - 1);
    strncpy(c.posix, posix, sizeof(c.posix) - 1);
    if (const char *why = worldClockCheck(c)) {
      fprintf(stderr, "--custom refused: %s\n", why);
      return 2;
    }
    worldClockSetCustom(0, &c);
  }
  worldClockSetHome((uint8_t)home, true);
  if (worldClockHome() != home) {
    fprintf(stderr, "no city with id %d\n", home);
    return 2;
  }

  FILE *out = fopen(argv[3], "wb");
  if (!out) { perror("open"); return 1; }
  static uint8_t rgb[64 * 128 * 3];
  const int startHour = startMin / 60 % 24, startMinute = startMin % 60;
  for (int f = 0; f < frames; f++) {
    // luasim's clock, field for field: the phase runs over the whole render,
    // the seconds tick every 30 frames, and a sweep walks one day.
    const double phase = frames > 1 ? (double)f / (double)frames : 0.0;
    int hour = startHour, min = startMinute, sec;
    if (sweep) {
      const int m = (startMin + f * 1440 / (frames > 0 ? frames : 1)) % 1440;
      hour = m / 60; min = m % 60; sec = 0;
    } else {
      sec = (56 + f / 30) % 60;
    }
    int64_t utc = (posixDaysFromCivil(year, 1, 1) + yday) * 86400 + hour * 3600 + min * 60 + sec - utcH * 3600;
    // px.t() arrives in the script as a lua_Number, which LUA_32BITS makes a float.
    float t60 = (float)phase * 60.0f, breath = (float)sec + t60;
    if (fps > 0) {
      t60 = breath = (float)f / (float)fps;
      utc = (posixDaysFromCivil(year, 1, 1) + yday) * 86400 + startMin * 60 - utcH * 3600 + f / fps;
    }
    const float since = settled ? -1.0f : (always ? 0.0f : t60);
    memset(display.px, 0, sizeof(display.px));        // the script's px.clear(0, 0, 0)
    worldClockDraw(utc, true, breath, since);
    uint8_t *p = rgb;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 128; x++, p += 3) WcHostDisplay::color565to888(display.px[y][x], p[0], p[1], p[2]);
    fwrite(rgb, 1, sizeof(rgb), out);
  }
  fclose(out);
  return 0;
}

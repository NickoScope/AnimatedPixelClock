# World clock

A dotted world map on the panel's own grid. 128×64 divides exactly into 64×32
dots on a 2 px pitch, so the look needs no rescaling. Land in daylight is lit,
night is dim, and civil twilight (sun between 0 and −6°) blends between them,
so the terminator draws itself and creeps across the map through the day.
HH:MM sits in the empty South Pacific and is **home's** time, in home's own
zone; home's name sits beside it. Cities are orange 2×2 dots, and home's dot
breathes.

![](../../tools/luasim/preview/world_home_cannes@6x.png)

## Where it lives

- Long press on the knob: the page after the clock. The carousel gives it 20 s.
- Build flag `WORLDCLOCK_ENABLED`; needs `CONTROL_ENCODER_ENABLED` (`#error` otherwise).
- The portal's *World clock* page: preview, home, and adding cities.

## Home's time and name

The time is home's local time, summer time included, whatever zone the panel
itself is set to. Each city carries a POSIX TZ string, and
[`posix_tz.cpp`](posix_tz.cpp) evaluates it for the current UTC second. The
page does not switch the process zone with `setenv("TZ")`: that zone is shared
by every task that calls `localtime_r`, the clock page and the weather task
among them.

The name is drawn in Picopixel, in orange like the dots, bottom-aligned with
the digits. It starts at x 43, past Tierra del Fuego's dots, and has 72 px
before New Zealand's: open Southern Ocean on this mask, so it needs no backing
box and cannot reach the time, which ends at x 30. Picopixel because it is
the small font the flight board, rail board and cards already use on this
panel, and the one the prototype can draw (`px.text`). *SAINT PETERSBURG* takes
61 of the 72 px:

![](../../tools/luasim/preview/world_home_long@6x.png)

**When home changes, the name breathes with home's dot for 10 s, eases out
over the last 2 s, and then holds still.** The chosen design is on the left,
the alternative (breathing for good) on the right:

| pulse, then settle (chosen) | breathe for good |
|---|---|
| ![](../../tools/luasim/preview/world_name_settles@4x.gif) | ![](../../tools/luasim/preview/world_name_breathes@4x.gif) |

Why this way:

- The name has to stay. The time belongs to whichever city is home, so
  without the label nobody can tell whose time it is.
- The movement does not have to stay. This page hangs on the wall all day and
  already has one thing that breathes. A label that pulses forever pulls the
  eye every time it rises, while the change it announced is long over.
- It pulses **in step with the dot**, using the same brightness value, so the
  name and the dot rise and fall together and read as one thing.
- 10 s is half the 20 s the carousel gives the page, so a change made while
  the page is up is seen pulsing and then seen settled within one visit.
- The pulse starts on the page's first frame after the change. A change made
  while another page is showing is announced when the world clock is next on
  screen. A change made in the portal also puts the page on screen, since the
  owner asked to see the new home there.

A city just added and made home, caught at the bottom of a breath:

![](../../tools/luasim/preview/world_custom_added@6x.png)

## Cities

Six are built in: Cannes, Moscow, New York, London, Dubai, Almaty. They cannot
be deleted. Up to **six more** can be added from the portal and deleted again.

Six because the map is 64×32 dots and Europe already holds three of the
built-in cities within a few dots of each other. Twelve orange dots is about
what the map carries before they stop reading as places, and the portal's list
stays one screen long. Memory is not the limit: a city is 134 bytes in NVS
and 136 in RAM.

Every city has an id that never moves: 0–5 for the built-in list, 100–105 for
the added slots, and 200 for a city made at the panel's location (below),
which is never stored. Deleting one added city leaves the others' ids alone, so
a stored home still means the same place.

A city is checked by `worldClockCheck()` on every way in: from the portal, from
NVS at boot, and from the IP lookup.

| | rule |
|---|---|
| name | 1–20 characters of `A–Z 0–9 space . - '`, no space at an end or two in a row, at most 72 px of Picopixel advance |
| lat | on the map: 58 S to 78 N (the mask's rows) |
| lon | −180 up to, not including, 180 |
| zone | an IANA name found in `tzdb.h`; the panel supplies the POSIX string itself. A string that `posixTzParse()` refuses is refused |

Same-name cities are refused (409): two rows nobody can tell apart.

### NVS

Namespace `panel`, written by `panelTick()` once changes stop arriving, like
every other panel setting. Only what differs from NVS is written.

| key | type | |
|---|---|---|
| `wcHome` | u8 | the chosen home's id. Absent = never chosen: home follows the location. Values 0–5 mean what they meant before custom cities |
| `wcC0`–`wcC5` | blob, 134 B | an added city: version byte 1, name[21], lat and lon as float, POSIX string[64], IANA name[40]. Absent = empty slot |

A record of the wrong length or version is skipped at boot, not repaired. A
stored home that is no city any more is removed at the next save, so a city
later added to that slot is not mistaken for the owner's choice.

## Home when nobody chose one

[`wc_home.cpp`](wc_home.cpp), in order, each step only when the one before has
nothing to say:

1. **The owner's choice** from the portal (radio button, or *Add and make
   home*). It always wins and is kept. Choosing the city made at the location
   stores it as an added city first. *Follow the panel's location* forgets the
   choice.
2. **The weather location**, when `settings.weatherLat/Lon` is set. The
   built-in or added city within **25 km** is home. A circle the size of
   Greater London (1 572 km², Wikipedia) has a radius of 22.4 km, so a panel
   anywhere in a city that size is named after it. Nice, 26.5 km from Cannes by
   the geocoder's own coordinates, stays a place of its own. With no city that
   close, a city is made there, named **HOME**, in the panel's own zone.
   Open-Meteo documents no reverse geocoding (its geocoding API page has only
   `/v1/search`), so there is no better name to give it.
3. **The panel's public IP**, when there is no choice and no location:
   `https://free.freeipapi.com/api/v1/json`, called once NTP has synced, at
   most once per boot. Its `cityName` is folded to the page's capitals, its
   first `timeZones` entry gives the zone, and the nearest-city rule of step 2
   applies. Provider notes, read on 2026-09-14:
   - freeipapi.com's free tier has TLS and no key, allows "10 requests per 10
     seconds, up to 60 per minute", and is for "commercial and non-commercial
     use";
   - ipapi.co's free plan is "not for production use";
   - ip-api.com's terms list SSL among what the pro plan adds;
   - ipinfo.io's free plan stops at the country.
4. **The zone**, until the lookup answers or if it fails: the built-in city
   whose UTC offset is the panel's own zone's right now. The first wins a tie,
   and the closest offset wins when none is equal. Rechecked every minute, so
   it follows NTP arriving and summer time.

The order puts what the owner said first, then what the owner configured, then
a guess from the network, then a guess from the clock.

## Adding a city from the portal

The browser asks [Open-Meteo's geocoder](https://open-meteo.com/en/docs/geocoding-api)
itself, not the panel:

```
GET https://geocoding-api.open-meteo.com/v1/search?name=Cannes&count=3&language=en&format=json
Origin: http://192.168.1.50

HTTP/1.1 200 OK
access-control-allow-origin: *
access-control-allow-methods: GET, OPTIONS

{"results":[{"name":"Cannes","latitude":43.55135,"longitude":7.01275,
 "timezone":"Europe/Paris","country":"France", ...}]}
```

(curl, 2026-09-14.) A GET with no custom headers is a CORS "simple request",
which needs no preflight (MDN), and the answer allows any origin. So no proxy
is needed. The panel is spared a TLS handshake, the largest allocation this
firmware makes (`weather.cpp`), for every pause in typing, and hears only the
city that is added. The same site's OPTIONS answers 404, so a request that did
need a preflight would fail.

Open-Meteo's terms: free for non-commercial use, "less than 10'000 API calls
per day, 5'000 per hour and 600 per minute", CC BY 4.0 with attribution. The
portal sends one request per pause in typing (350 ms) and credits Open-Meteo
and GeoNames under the search box.

Picking a result fills the name with a first guess by `worldClockFitName`'s
rules (capitals without accents, cut after a word or before a hyphen to fit
72 px). It is measured with the font advances the panel reports in
`GET /api/worldclock`, so the width meter reads what the panel will check. The
portal's copy of those rules gives the same answer as the C++ for 14 of 14
test names.

API: [`web_panel.cpp`](../web/web_panel.cpp), top of the file.

## Zones

[`tzdb.h`](tzdb.h) maps 489 IANA zone names to POSIX strings, 88 of them
distinct: 11 315 bytes. It is written by
[`tools/luasim/gen_tz.py`](../../tools/luasim/gen_tz.py) from the tz database
on the machine that runs it, tzdata **2026c** for this copy. The tz database is
in the public domain (its `LICENSE`). Each string is its zone's TZif footer,
which tzfile(5) defines as the POSIX-TZ-style string "for use in handling
instants after the last transition time stored in the file".

A footer only takes over after the file's last stored transition, so the
generator checks every footer against its zone from 2026-09-01 to 2028-09-01,
using Python's zoneinfo for both. The two must agree at both ends of that
window and one second either side of every change in either. By hand, it
refused a week-early March rule for Paris, an hour-late October rule, Paris
without summer time, and posix_tz_db's strings for America/Vancouver and
Europe/Chisinau. Two zones have no footer that matches in the window. Each gets
the fixed offset it keeps longest there, and the header lists both:
Africa/Casablanca and Africa/El_Aaiun, at +00, which misses 19 days at +01.

Why not [nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db)
as it stands: its table was last regenerated for tzdata 2025b, and 7 of its 461
strings differ from 2026c. Among them, 2026c moves America/Vancouver and
America/Edmonton to permanent UTC−7 and UTC−6 from November 2026. GitHub
reports its licence as MIT, but the `LICENSE` file still carries the
`[year] [fullname]` template. Its method is the one used here: its `gen-tz.py`
reads the same `/usr/share/zoneinfo` footers.

## One source

The land mask and the built-in cities are written by
[`tools/luasim/gen_world.py`](../../tools/luasim/gen_world.py) into both
`worldmap.h` here and the Lua prototype
[`world_clock.lua`](../../tools/luasim/scripts/world_clock.lua). A city's zone
is its IANA name there. The POSIX string written beside it comes from
`tzdb.h`, the same table the portal's cities use. To change the built-in
cities, edit `CITIES` and rerun it. The pre-commit hook runs `gen_tz.py --check`
and `gen_world.py --check`, which need no network, and refuses a commit where
the copies disagree.

## How it was checked

`python3 tools/luasim/fx_parity.py --quick` runs these world clock checks:

- **The page against its prototype.** `wchost` compiles `worldclock.cpp` and
  `posix_tz.cpp` for the host against a stand-in display. It renders the page
  under the same four clocks luasim gives `world_clock.lua`: 12:34 CEST, 23:59,
  a day's sweep, and 06:10 on 1 January at UTC−5. The homes are Cannes just
  changed, New York settled, a custom *SAINT PETERSBURG* just added, and a
  custom *SYDNEY* whose summer spans the new year. The frames are compared
  after the panel's RGB565 step: 16 of 16 identical. A negative control first
  requires Cannes and New York to look different.
- **The zone evaluator against zoneinfo.** Every string in `tzdb.h`, plus
  POSIX forms no zone uses today, is checked every 6 h from 2026 to 2029 and one
  second either side of each change: 543 992 instants, all equal. The
  zero-based day form (`59`) is checked against macOS libc instead, because
  Python 3.9's zoneinfo starts it a day early; POSIX (XBD 8.3) and libc agree
  with each other. 14 malformed strings must be refused, and are. A negative
  control requires a one-week change in a rule to show up.
- **Name folding.** `worldClockFitName` on 9 names with known answers: accents,
  a Cyrillic name that folds to nothing, a hyphenated one cut before the
  hyphen, a Welsh one cut at exactly 72 px.
- The Lua runtime's own parity (luasim against `fxhost`) still covers the
  script, as it does every script.

What parity does **not** cover: custom cities beyond the one slot the harness
fills, the city made at the location, NVS, the IP lookup, the web API and the
portal. They exist only at run time, and the prototype has no way to express
them.

Not yet on hardware: nothing here has run on the board.

## Cost

`pio run -e matrix-waveshare-rgb` against fb60bb0, the same branch without
these changes: **+38 468 B flash, +2 200 B RAM** (2 004 849 → 2 043 317 and
92 940 → 95 140).

- The zone table is 11 315 B of the flash, and the mask 256 B.
- The RAM is city copies: six custom slots in the page and six more in
  src/panel as "what NVS holds" (12 × 136 B), plus home, the city made at the
  location and the IP answer (3 × 136 B).
- The colour table is 32×64 `uint16_t`, computed once a minute, so a frame is
  774 pixel writes and no trigonometry.
- The IP lookup's task has 8 KB of stack while it runs, once per boot, and only
  when there is neither a choice nor a location.
- Refresh drops to 10 fps on this page: only the home dot and the name move.

## Honest limits

- Declination by a cosine approximation, sun longitude straight from UTC. The
  equation of time moves the line by up to 4°; a dot is 5.6° wide.
- Before NTP sync the map is all night and the time reads `--:--`, rather than
  drawing 1970 with confidence.
- A POSIX string holds one rule for ever. A zone that changes its rules after
  tzdata 2026c is wrong until `tzdb.h` is regenerated and the firmware is
  rebuilt. Morocco's Ramadan changes cannot be written as a POSIX rule at all.
- The IP lookup uses `setInsecure()`, as the weather fetch does, so someone on
  the network path could answer for freeipapi.com and move home. It sends the
  panel's public IP to freeipapi.com.
- IP location is the ISP's idea of where the line ends, often the nearest big
  city, and wrong behind a VPN. Setting the weather location, or choosing a
  home, replaces it.
- Open-Meteo's geocoder is free for non-commercial use only.

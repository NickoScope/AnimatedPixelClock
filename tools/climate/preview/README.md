# Weather screen with the indoor reading: previews, 2026-09-15

The owner's request (18:24): build the board's temperature/humidity sensor
into the weather screen, beautifully. Three designs were drawn here before any
weather-screen code.

**Chosen: B, split "outside | inside"** (owner, 2026-09-15 19:10). The firmware
draws it from `src/clocks/weather_layout.h`, whose `CL_B_*` constants, colours,
house glyph and dashes equal render.py's name for name.

`tools/climate/check_weather_screen.py` runs that header on the host with the
real Adafruit GFX library. It draws 11 frames, and all of them come out
pixel-identical, with the strings in `frames.json`:
- `today_live`, `today_absent`, `today_worst`;
- the three `b_split_*` frames;
- B with the sensor absent, which must equal `today_absent`;
- the four edge frames below.

A and C stay here as the record of the choice.

Every PNG is drawn by `tools/climate/render.py`. Each frame exists at 6x
(`name.png`) and at 1:1 (`name_128x64.png`). `contact_sheet.png` shows them all
with labels. `frames.json` lists the strings drawn in each frame, the ink box of
every part, and (under `_meta`) the layout budget.

## Where the pixels come from

**Today's screen, "today", is the weather clock the panel draws now.** The
renderer reads from the firmware:
- the layout `#define`s of `src/clocks/clock_weather.cpp` (`WTIME_Y` 2,
  `WICON_X` 10, `WICON_Y` 24, `WDETAIL_Y` 55);
- the `drawTemperature(52, WICON_Y + 3)` call;
- the default colours in `src/config/settings.cpp`.

It then mirrors every draw call:
- the animated icons, at 2000 ms;
- the size-3 temperature and its radius-2 `drawCircle` degree;
- the details row;
- `drawMeridiemIndicator()`;
- `drawNoWiFiIcon()`, whose calls are executed from its source.

The fonts are the panel's own: the built-in 5x7 (`tools/glcdfont.json`) and
Picopixel (`tools/picopixel.json`). Discs, circles and lines follow Adafruit
GFX. Colours are quantised through color565.

**The values.**
- The weather is a typical September afternoon: partly cloudy, 17 °C, high 19,
  low 11, 62 %RH.
- Indoors: 23.4 °C and 45 % (the brief's values).
- `*_worst` frames are **synthetic**, to test widths:
  - 12-hour clock with PM;
  - the no-WiFi icon;
  - Fahrenheit;
  - 104 °F outside with 100 %;
  - indoors 80.6 °F and 100 %.

## The three designs

### A: indoor line (`a_line_*`)

A fourth line joins the screen, under the details row:
- an amber house;
- the indoor temperature, white 5x7, with a thin dot and a 3x3 degree mark;
- the humidity, dim.

To make room:
- the time moves up 1 row;
- the icon and big temperature move up 6 rows (`CL_A_ICON_Y` 18);
- the details row moves up 8 rows (`CL_A_DETAIL_Y` 47);
- the indoor line sits at row 56.

**Trade-offs.**
- The most legible indoor reading of the three: full 5x7, centred.
- The outdoor block keeps its size and colours, so outdoor stays primary.
- The bottom now holds two text lines two rows apart, so the screen reads more
  like a list than a picture.
- It changes every vertical position on today's screen.

### B: split (`b_split_*`)

**Outdoor on the left:**
- the icon moves to the edge (`CL_B_ICON_X` 1; at 0 the fog's drifting lines
  would leave the panel);
- the size-3 temperature follows at x 28.

**Divider:** a dim rule at x 89 spans the big digits (rows 26-47).

**Indoor on the right:** a column from x 92:
- house and temperature (5x7) at row 28;
- humidity under the number at row 39.

The time and the details row do not move.

**Trade-offs.**
- It reads as "outside | inside" at a glance, and the indoor numbers stay 5x7.
- It is the largest change to today's composition: the weather block leaves the
  centre.
- A three-character outdoor temperature drops its unit letter to keep clear of
  the rule; the degree mark stays (`b_split_worst`). That happens at -10 °C and
  below, or 100 °F and above.

**The edge frames** (`b_split_edge_*`, sheet `b_edges_sheet.png`, synthetic):
- -9 °C and 99 °F keep the letter.
- -10 °C and 100 °F drop the letter and keep the degree mark.

The check reads this off the firmware's raster: the degree ring's pixels are
lit, and the letter's are not.

### C: badge (`c_badge_*`)

**Nothing on today's screen moves.** Two Picopixel lines sit at the right edge,
under the unit letter:
- house and temperature at rows 37-41;
- humidity at rows 43-47, starting under the number.

The house stands at a fixed column, x 104. A three-digit outdoor temperature
ends its ink at x 102, so one dark column always separates the two.

**Trade-offs.**
- The least intrusive design, and today's screen stays exactly as it is.
- The smallest text: Picopixel capitals are 3x5 against 5x7.
- The badge could be mistaken for a second outdoor figure, a "feels like";
  the house glyph is what tells them apart.
- Beside a three-digit outdoor temperature it is cramped (`c_badge_worst`).
- In the stale state the dashes are small enough to look like noise.

## States

| Frame | What it shows |
|---|---|
| `*_live` | a good reading younger than the stale limit |
| `*_stale` | the last good reading is older than three intervals, and at least 30 s old (`climate::staleAfterMs`, src/climate/climate_model.h), so the sensor stopped answering or its CRC keeps failing. House and dashes `--.-°` `--%`, all dim |
| `today_absent` | **the absent state of all three.** When no SHTC3 was found or the sensor is switched off, the variant draws nothing extra and today's screen comes back unchanged. For A that means the block returns to its place, rather than leaving an empty line |

## Rules shared by all three

**Numbers.**
- The indoor temperature has one decimal while it fits four characters (-9.9
  to 99.9) and whole degrees beyond.
- The humidity is in whole percent, since the sensor is ±2 %RH typical
  (datasheet Table 1).
- The unit follows the weather's Fahrenheit switch.

**Colours, before color565.**
| Element | RGB | color565 |
|---|---|---|
| House | amber 255/150/0 | 248/148/0 |
| Indoor temperature | white | 248/252/248 |
| Humidity, and a stale reading | dim 110/122/128 | 104/120/128 |
| B's rule | 52/60/64 | 48/60/64 |

These are the house colours of the rail board and the market pages. Today's
weather colours stay the user's sprite colours.

**Layout budget** (`budget()` in render.py). Each design is drawn 448 times
over every combination that moves its parts:
- °C and °F;
- outdoor temperatures -40, -18, -10, 8, 31, 40 and 50 °C;
- indoor temperatures -9.9, 37.7 and 23.4 °C, humidity 8-100 %, and stale;
- 12-hour and 24-hour clock;
- with and without the no-WiFi icon;
- both phases of the details row.

The icon stands for all seven kinds at every animation phase over 9.6 s.
No two parts may touch or come within one pixel, and nothing may leave the
panel. **Clean: 1792 cases.**

## Decided, and still open

- **Decided:** B. The portal's "On the weather screen" is Off or
  Outside | inside (`climateShow` 0 or 1), with the split as the default.
- **Open:** the indoor colours. White and dim were a first proposal and went in
  as drawn; judge them on the panel.
- **Open:** "Weather not set up" and "Fetching weather..." stay as they are,
  with no indoor column. No design was drawn for them.

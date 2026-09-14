# Flight board

Everything for this feature lives in this repository.

| | |
|---|---|
| `flightboard.cpp` | the layout constants, written by hand **only** here |
| `tools/fb_render.py` | host render, reads the constants from the source |
| `sim/flightboard-sim.html` | browser simulation, layout block is generated |

## Changing the layout

Edit `flightboard.cpp`, then:

```bash
python3 tools/fb_sim_build.py    # push the constants into the simulation
python3 tools/fb_check.py        # exit 1 if anything is stale
```

`.githooks/pre-commit` runs the check automatically. Enable once per clone:

```bash
git config core.hooksPath .githooks
```

## Why the status words are short

Every pixel the status column takes is a pixel the destination name does not
get. On 30 live Nice rows, `ENROUTE` (29 px) and `DELAYED` (28 px) alone cost
four destinations their name; shortening just those two to `IN AIR` and `DELAY`
took coverage from 23/30 to 27/30. Coverage is flat from 20 px to 28 px, so
`ON TIME`, `LANDED` and `DEPART` keep their spelling.

## Direct from AeroAPI

`FLIGHTBOARD_DIRECT_ENABLED` (on in `matrix-waveshare-rgb`; needs
`FLIGHTBOARD_ENABLED` and `CONTROL_ENCODER_ENABLED`, both refused otherwise by
`#error`). With a key stored in NVS the panel asks FlightAware AeroAPI itself
and Home Assistant plays no part: the MQTT transport unsubscribes and never
publishes a request. Without a key, or in a build without the flag, the six
built-in airports come over MQTT exactly as described under Transport below.

| | |
|---|---|
| `aero_transform.{h,cpp}` | AeroAPI answers to board rows and to a tracked flight; plain C++, host-tested |
| `fb_settings.h` | the budget and its bounds, the call counters, airport and ident checks |
| `aero_direct.{h,cpp}` | the scheduler, the call task, TLS, NVS, `/api/flightboard` diagnostics |
| `aero_roots.h` | the pinned roots for `aeroapi.flightaware.com` |
| `tools/flightboard/` | `check_aero.py` (host tests), `aero_ref.py`, `gen_aero_fixture.py`, `previews.py` |

### The key

NVS namespace **`aero`**, key **`key`** (string), written by `env:provision`.
Home Assistant keeps the same key as `aeroapi_key` in `secrets.yaml`; its five
REST sensors send it as `x-apikey: !secret aeroapi_key` (counted on nickohome,
2026-09-14, key names only). The owner provisions it:

```bash
python3 tools/provision_secrets.py aeroapi-from-ha     # ssh nickohome, sudo grep ^aeroapi_key:
# or: python3 tools/provision_secrets.py aeroapi-prompt   (typed, not echoed)
python3 tools/provision_secrets.py check               # set / length only
pio run -e provision -t upload
pio run -e matrix-waveshare-rgb -t upload
pio run -e provision -t clean                          # drop the value from the build cache
```

The script prints the key's length and nothing else, writes
`provision_secrets.ini` 0600, and the provision image prints only the length.
`-DPROV_AEROAPI_CLEAR` removes a stored key. The firmware reads the key into a
PSRAM buffer for each call and zeroes it once HTTPClient holds its copy; no
key is logged, printed, published or returned by `/api/flightboard`, which says
only whether one is stored. As with the rail board, the copy inside HTTPClient's
header string and whatever TLS holds of the request are not wiped.

### What it asks

AeroAPI OpenAPI spec **4.17.1**
(`https://www.flightaware.com/commercial/aeroapi/resources/aeroapi-openapi.yml`,
read 2026-09-14; line numbers in that file):

| fact | lines |
|---|---|
| server `https://{env}.flightaware.com/aeroapi`, env `aeroapi` | 122–128 |
| key in the `x-apikey` header, applied globally | 130–140 |
| `GET /airports/{id}/flights/arrivals`: ordered by `actual_on` desc; `start` defaults to 24 h back; `max_pages` (default 1), `cursor`; answer `links.next`, `num_pages`, `arrivals[]` | 7887– |
| `.../departures`: compared against `actual_off`, 24 h back | 8482– |
| `.../scheduled_departures`: compared against `scheduled_off`, 2 h back to 24 h ahead | 9079– |
| `.../scheduled_arrivals`: compared against `estimated_on`, 48 h back to 24 h ahead | 9677– |
| `GET /flights/{ident}`: `ident_type` designator / registration / fa_flight_id; about 14 days without `start`/`end`; ICAO idents recommended over IATA | 1464–1560 |
| `diverted`, `cancelled` ("no longer being tracked", not always an airline cancellation) | 1683–1694 |
| `departure_delay`, `arrival_delay` in seconds, negative when early; `status` free text | 1806–1836 |

A board is four calls, one per list, each `?type=Airline&max_pages=1` as Home
Assistant's sensors send, plus `start` = now − 2 h (and `end` = now + 2 h for
the scheduled lists). Home Assistant left those at the defaults, which reach
back 24 h or 48 h, so one page of 15 records often held few flights inside
±2 h; asking for the window costs the same.

A tracked flight is `GET /flights/{ident}?ident_type=designator&max_pages=1`
with `start` = now − 24 h and `end` = now + 47 h (the spec allows 2 days ahead;
an hour is left for the request to arrive).

### The board, three times

`aero_transform.cpp` ports the payload template of Home Assistant's automation
**flightboard_serve v3.7** (read through the HA MCP, 2026-09-14): past and future
list merged, first record of each `fa_flight_id` kept (past first), ±2 h on the
gate time (actual, else estimated, else scheduled), sorted, `now_idx` at the
first flight not yet due, fifteen rows from seven before it, `ident_iata` else
`ident_icao` else `ident`, the city cut at `(` and `/`. `tools/flightboard/aero_ref.py`
is a second port, written from the template rather than from the C++, and
`check_aero.py` requires the two to give identical boards. Three differences
from the template, on purpose, marked DIFF in both:

- **landed is `actual_in` or `actual_on`.** The template read `actual_in` only.
  In Home Assistant's own LHR arrivals at 20:32 on 2026-09-14, 10 of 15 records
  were "Landed / Taxiing" with `actual_on` set and `actual_in` null: the MQTT
  board showed them IN AIR.
- **the code falls back to ICAO.** The template's `code_iata | default(code_icao)`
  lets a null through, since Jinja's `default()` replaces only undefined values.
- **city names lose their accents** (U+00C0–U+017F to their base letter) instead
  of reaching a font that cannot draw them.

Times go into the panel's own zone, as `as_local` put them into Home
Assistant's; the airport's zone is stored with a custom airport but not used to
convert (the board did not do that before).

### Airports

The six built-in ones keep ids 0–5, so `panel`/`fbApt` written before keeps its
meaning. Up to **six** custom airports, ids 100–105, in `panel`/`fbA0`–`fbA5`
(63-byte versioned records, checked again on the way in, like the world
clock's `wcC0`–`wcC5`), walked by the knob after the built-in ones. A custom
airport holds ICAO (4 of A-Z 0-9, a letter first), IATA (3 capitals or empty),
a header name (A-Z 0-9 space `. - '`, at most 12 characters and 56 px of
Picopixel, which leaves 4 px before the clock after DEPARTURES) and an IANA
zone name. Two entries with one ICAO code are refused (409): they would fetch
and pay for the same board twice. Deleting the selected airport selects Nice.
Only the built-in six are asked of Home Assistant; without a key a custom
airport's board says `NEEDS A KEY`.

The portal's search runs in the browser against
[mwgg/Airports](https://github.com/mwgg/Airports) (MIT), pinned to commit
`2473bd8f` on jsDelivr: checked 2026-09-14 with a real request,
`access-control-allow-origin: *`, 1.17 MB brotli, cached immutable; 23 773
entries with an ICAO code and a zone, 7 908 with an IATA code. OurAirports
(public domain, also `access-control-allow-origin: *`) was the first choice and
was not taken: its `airports.csv` has no time-zone column and is 3.9 MB gzip.
AeroAPI's `/airports/{id}` would give the zone but costs $0.015 a call.

### Tracked flights

Up to **three** idents in `fbcfg`/`trk0`–`trk2`. Three because the pinned row
is one row and the flights take turns in it with each 10 s swap, and because a
tracked flight costs about 25 calls on the day (below) against a default day
cap of 30. The ident is folded to capitals without spaces and must be an
airline code (3 letters, or 2 characters with a letter) then 1–4 digits and at
most one letter.

Which flight the ident means, from the answer (a choice): one that has left
the gate and not landed; else one that landed within 3 h and after the tracker
was added (less 1 h); else the earliest still on the ground whose departure is
at most 6 h past; else the latest, shown but not current.

| state | pinned row says | next call |
|---|---|---|
| waiting | `...` | now |
| not found | `NO FLIGHT` | 6 h |
| scheduled | `ON TIME`, or `GATE 12` in the last hour when `gate_origin` is known | a third of the time left until an hour before departure, 10 min – 6 h |
| delayed > 15 min | `DELAY 25` | same |
| left the gate | `TAXI` | 10 min |
| airborne | `IN AIR`, or `LATE 25` when `arrival_delay` ≥ 15 min | 20 min; 10 min once due within 45 min |
| landed | `LANDED` | none; **removed 2 h after landing** |
| cancelled | `CANX` | none; **removed 6 h after the scheduled departure** |
| diverted | `DIVERT` | 30 min |

There is no BOARDING: no AeroAPI field says boarding has started, so the gate
stands in for it. The time shown is the departure until take-off, the arrival
after. The row is the first of seven on a dark blue band (0,26,70) with a
bright blue bar; the board keeps six rows below it. Previews:
`tools/flightboard/preview/*.png` (`python3 tools/flightboard/previews.py`).

Simulated from `trackNextS()` for a 2 h flight: added 24 h before departure,
28 calls ($0.14); 12 h before, 26; 3 h before, 22.

### The budget

AeroAPI charges per call. Prices, from
`https://www.flightaware.com/commercial/aeroapi/` (read 2026-09-14): the four
airport flight lists and `/flights/{ident}` are $0.005 per result set, a result
set is 15 records, the same price on every tier; `/airports/{id}` $0.015,
`/airports/{id}/flights` $0.020. The Personal tier includes up to $5 a month
free and allows 10 result sets a minute. Every call here sends `max_pages=1`,
so it is one result set at most.

| guard | default | bounds | why |
|---|---|---|---|
| lists for the selected airport only | | | |
| board lists only while the page is on screen or was within | 120 s | fixed | a knob turned past the page and back is not a new visit |
| floor per list, per airport and direction | 15 min (past lists 30) | 5–240 min | coming flights change more than landed ones; 5 min is about the fastest a status moves |
| calls per UTC day, board and trackers together | 30 | 0–1000 | 0 turns the fetch off; 1000 is $5 a day |
| of which the board may use, while flights are tracked | all but a fifth | fixed | a board can wait, a take-off cannot |
| calls per UTC month | 900 ($4.50) | 0–20000 ($100) | inside the Personal tier's $5 with $0.50 left for Home Assistant's own calls |
| spacing between calls | 7 s | fixed | above 60 s / 10 |
| after failures in a row | 60 s, 5 min, 15 min, 1 h | fixed | never a loop |
| key refused (401, 403) | 1 h doubling to 24 h | fixed | |
| 429 | `Retry-After` within 1 min – 1 h, else 15 min | fixed | |
| 400 or 404 | a list: 6 h; a tracker: 24 h | fixed | a bad code does not mend itself |

The counters (`fbuse`/`u`: UTC day, month, calls today, this month, total) are
written **before** each call, and a call whose count cannot be written is not
made: a counter that a reboot forgets is a cap that does not hold. Every call
made is counted, answered or not. The portal cannot reset them. Home
Assistant's own calls with the same key are not counted here.

**Expected cost at typical use**, with the defaults:

- page in the carousel through the day: both halves want 12 calls an hour
  (four coming lists every 15 min, four past lists every 30 min), so the day
  cap of 30 is reached in about 2½ hours of display. 30 × 30 days = the month
  cap, **$4.50 a month**; the board then stands still, amber clock, until
  midnight UTC.
- the page looked at for an hour a day: about 12 calls, **$1.80 a month**.
- plus about $0.14 per flight tracked, inside the same caps.
- at most, whatever is set: month cap × $0.005.

`/api/flightboard` → `direct.usage` reports calls today and this month, their
estimated cost, the month cap in dollars, the board's share and the time to
midnight UTC; the Flight board page shows the same.

### When there is no board

`NO KEY` (a build with the flag, no key), `NEEDS A KEY` (a custom airport, no
key), `FETCHING`, `WAITING`, `DAY CAP`, `MONTH CAP`, `OFF` (a cap of 0),
`NO WIFI`, `NO CLOCK`, `LOW MEM`, `AUTH 401`, `RATE 429`, `REFUSED 404`, `TLS`,
`NET`, `HTTP 500`, `BAD`, `BIG`, `NOMEM`, `NVS`. With a board, the header clock
turns amber once a half's lists are older than four floors and 5 min.

### TLS and memory

`aero_roots.h` pins **SSL.com Root Certification Authority RSA** (to 2041) and
**SSL.com TLS RSA Root CA 2022** (to 2046), taken from macOS's system root
store. On 2026-09-14 `aeroapi.flightaware.com` presented `flightaware.com` ←
SSL.com RSA SSL subCA ← SSL.com Root Certification Authority RSA over TLS 1.2
with ECDHE-RSA-AES256-GCM-SHA384, and `openssl verify` accepted the leaf with
the first root alone. An unauthenticated GET answered 401.

As the rail board's direct fetch: a task (`aerocall`, 12 KB internal stack,
core 0, priority 1) per call; TLS, the 192 KB body buffer and the 64 KB JSON
cap in PSRAM; not started below 28 KB internal free. Synthetic answers of 7–16
records are 12–30 KB and parse, filtered, in at most 15 KB on a 64-bit host;
a real record Home Assistant held is 2.4 KB indented. `/api/flightboard` →
`direct.last` reports the real bytes, parse peak, heap low-water mark and stack
spare, and `direct.sample` the start of the last answer's array.

### Verified

- `python3 tools/flightboard/check_aero.py`: time parsing, the four list queries
  and the tracker query exactly, city cleanup (and the accent fold over all of
  U+00C0–U+017F, C++ = Python), ident checks, every board rule on synthetic
  fixtures (duplicates across lists, ±2 h edges, no ident, no time, cancelled,
  runway-only landing, no IATA, fifteen of 26), parse with and without the
  filter identical, both boards identical to `aero_ref.py`; the tracker pick,
  cadence, expiry, shown time and delay on eleven answers; the budget's bounds
  and messages, UTC day and month roll-over, the board's share, and the custom
  airport checks.
- The C++ transform over Home Assistant's own AeroAPI data (LHR arrivals and
  scheduled departures held by `sensor.aeroapi_*_board` at 20:32 on 2026-09-14,
  run locally, not stored): 15 records each parsed, none skipped, C++ = Python.
- `tools/provision_secrets.py aeroapi-prompt` with a made-up key into a scratch
  file: length printed, value not, file 0600.
- The portal script parses in JavaScriptCore (`new Function`), and
  `tools/flightboard/check_portal_js.py` runs the Flight board page's code in
  JavaScriptCore against a stub DOM and a mocked `/api/flightboard`: escaping
  of a hostile row, pinned rows first, the airport list by id, usage and budget
  fields, diagnostics warnings, the tracker list and its limit, client-side
  ident and name checks, the folded search, and the three POST bodies. Not run
  in a real browser: layout on a phone is unchecked.
- `matrix-waveshare-rgb` and `provision` build with no warning from these files.
  `tools/flag_matrix.py`: **28/28** as intended, including `flight direct, no
  MQTT` and `flight direct + MQTT` building and `flight direct, no board` and
  `flight direct, no knob` refused.
- `tools/flightboard/previews.py`: ten host renders with a pinned flight in
  `tools/flightboard/preview/`. The widest case (an ICAO ident and DELAY 125)
  showed the route one pixel from the ident; the route now keeps 4 px clear.

### Cost

Against `5515371`: **+59 456 B flash, +2 472 B RAM** (2 057 889 → 2 117 345 and
95 212 → 97 684, static). Most of the flash is the portal's page and script and
the direct fetch; HTTPClient and certificate verification were already linked
by the rail board.

### Not verified

- **Nothing has run on hardware**, and **no call from this work has reached
  AeroAPI with a key.** The real answer's size and shape as the panel receives
  it, the stack, heap and PSRAM numbers, NVS behaviour and the pinned row's
  colours on the panel are untested; the previews are host renders.
- The owner's tier. The price is the same on every tier by FlightAware's page;
  the 7 s spacing assumes Personal's 10 result sets a minute.
- Whether a failed call, a 4xx or an empty answer is billed, and when
  FlightAware's month turns over: not on the pricing page. Every call is
  counted, and the month is UTC's.
- AeroAPI's 429 behaviour and `Retry-After`: the spec does not mention 429.
- What `/flights/{ident}` answers for an ident it does not know (an empty
  `flights` array, handled as NOT FOUND, or a 400/404, handled as REFUSED).
- Which field `start` is compared against on `/arrivals`: the spec does not say.
- An ident flying more than 15 times in the 71 h window: only the first page is
  read.
- That SSL.com will ever issue under its 2022 root.
- The key's length: 16–255 characters is accepted.
- Airport data quality in mwgg/Airports, and whether its zone names are all
  current IANA names.

## Transport

Without an AeroAPI key (or without `FLIGHTBOARD_DIRECT_ENABLED`) the board comes
from Home Assistant. It already serves this board over MQTT for another device in the
house, so the panel is a second subscriber to a deployed contract rather than a
new feature.

```
request   nickoscope_watch/flightboard/req            {"apt":"LFMN","dir":"arr"}
response  nickoscope_watch/flightboard/state/<apt>/<dir>   retained
```

Keyed on (airport, direction), not on the client: two devices watching
different airports would otherwise overwrite each other's payload *and* pay
twice for the same AeroAPI fetch, because Home Assistant's throttle only
short-circuits when the currently selected airport matches.

Three things `fb_mqtt.cpp` gets right that are easy to get wrong:

**The buffer.** PubSubClient defaults to 256 bytes and does not truncate an
oversized PUBLISH - it drops it, silently. A live Nice board measured 1060
bytes on the wire. The default would have produced a page that never updated
with nothing in the log to explain it. Set to 2048.

**Resubscribing.** Subscriptions do not survive a reconnect, so the subscribe
runs from the connect path, not only on a selection change.

**Not asking when there is no need.** A retained payload lands within
milliseconds of subscribing - measured, not assumed. A request is published
only if none does within 1.5 s, so turning the knob through six airports costs
one subscribe and at most one fetch. Each fetch is roughly a cent of AeroAPI.

The knob's steps are debounced 1.2 s before any of that happens.

### Credentials

Broker and key live in NVS and are never compiled in. Write them once:

```bash
pio run -e provision -t upload \
  --project-option="build_flags=-DPROV_MQTT_HOST=\\\"192.168.4.35\\\" -DPROV_MQTT_USER=\\\"...\\\" -DPROV_MQTT_PASS=\\\"...\\\""
```

then flash `matrix-waveshare-rgb` over it.

### When there is no data

The page names the reason - `NO BROKER`, `NO WIFI`, `CONNECTING`, `FETCHING`,
`NO DATA` - rather than showing a bare "NO DATA" that sends you looking at Home
Assistant when the panel never reached WiFi.

### Verified

The contract was exercised against the live broker on 2026-09-10: subscribe to
`.../state/LFMN/arr` returned a retained 1060-byte payload immediately, 15
flights, `upd=09:34`. The firmware path itself is **not** hardware-verified.

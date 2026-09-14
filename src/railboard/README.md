# Rail board

A UK station screen for any National Rail station, from Realtime Trains. One
list on the whole panel at a time: departures, then arrivals, 10 s each.
Guildford until the web portal chooses another station.

![Departures](../../tools/railboard/preview/departures.png)
![Arrivals](../../tools/railboard/preview/arrivals.png)

## Two paths to the same board

1. **Direct.** The panel asks Realtime Trains itself (`RAILBOARD_DIRECT_ENABLED`,
   token in the panel's NVS). No Home Assistant needed.
2. **Home Assistant.** Home Assistant asks and publishes retained boards over
   MQTT. The panel uses them when it has no token, or its own fetch has failed
   and its board has gone stale.

A fresh board from the panel's own fetch wins over Home Assistant's; Home
Assistant's boards that arrive meanwhile are counted and set aside. Both paths
spend **one daily quota**, that of the same RTT account.

> **Realtime Trains' terms**, `specification/main.yml` line 19: "no token is placed
> in a distributable user application unless specifically authorised by us ...
> End-user applications are expected to proxy their requests through a
> server-side application". Path 1 places the token in the panel. The owner has
> been told; the choice is his.

## Where it lives

- The knob browses to it as TRAINS; a click enters it (hint `TURN: LISTS`).
  Inside, each detent steps **departures → arrivals → diagnostics** and round
  again. A list chosen this way holds for `RB_HOLD_S` = 60 s after the last
  detent; then the 10 s alternation resumes from that list.
- The carousel gives the page 20 s (or the common slot): both lists once.
- Build flags: `RAILBOARD_ENABLED` needs `MQTT_BUS_ENABLED` and
  `CONTROL_ENCODER_ENABLED`; `RAILBOARD_DIRECT_ENABLED` needs `RAILBOARD_ENABLED`
  and `BOARD_HAS_PSRAM`. Each missing one stops the build with `#error`. Both are
  on in `matrix-waveshare-rgb`.

## Data flow

```
web portal ──{"crs":"WAT"}──▶ /api/railboard ──▶ src/panel (NVS "panel"/"rbStn")
                                                   └▶ railboardSetStation(): drop old boards,
                                                      resubscribe .../WAT/+, publish selection,
                                                      fetch WAT now

path 1   panel task "rttfetch" ──HTTPS, ISRG roots pinned──▶ data.rtt.io
             │ one answer, both lists (rtt_transform.cpp)
             ▼ handed to the loop task → s_board[] → railboardRender()

path 2   panel ──retained {"crs":"WAT"}──▶ nickoscope_matrix/railboard/select
                                               │ MQTT trigger (package)
                                               ▼
                                     input_text.railboard_crs
                                               │ AppDaemon app (A) or shell_command (B)
                     data.rtt.io ◀──HTTPS──────┘ exchange, fetch, same transform in Python
                                               ▼ MQTT, retained
                  nickoscope_matrix/railboard/WAT/departures | arrivals | status | config
                                               ▼ mqtt_bus → railboardIngest() → s_board[]
```

## Direct from Realtime Trains

### The token

NVS namespace **`rb`**, written by `env:provision`:

| key | type | |
|---|---|---|
| `token` | string | the token as issued, without `Bearer ` |
| `kind` | string, optional | `refresh`: exchange it first. `access`: never exchange. Absent: try it as an access token and exchange it on a 401 |

A refresh token is exchanged at `GET https://data.rtt.io/api/get_access_token`
with itself as the bearer (main.yml lines 1644–1669; the only server, lines
74–76). The answer's `token` is kept in PSRAM only, never in NVS, and used until
its `validUntil` less 5 min; a missing or unreadable `validUntil` counts as
10 min. A 401 with that access token drops it and exchanges once more.

**A refused exchange (401 or 403) means the stored token is bad.** Polling stops
for 15 min, doubling to 6 h; the board shows `RTT token refused` when it has
nothing else to show, diagnostics shows `DIRECT AUTH 401` in red, and the portal
shows the token as *refused by Realtime Trains*. A reboot, as after provisioning
a new token, starts again.

No token is logged, printed, published, or returned by `/api/railboard`. The
stored token is read from NVS into a PSRAM buffer for each fetch and zeroed
after it; the `Authorization` string is zeroed once HTTPClient has taken its
copy; the exchange's body and JSON are zeroed before they are freed. Two copies
are not wiped: the one inside HTTPClient's header string, freed when the fetch
ends, and whatever the TLS layer holds of the request in PSRAM.

### When it asks

GET `/gb-nr/location` with the query the package always used:
`code=<crs>&timeFrom=<now − 30 min>Z&timeWindow=90`.

| outcome | next fetch |
|---|---|
| OK | 30 s; 120 s once RTT reports ≤ 2 × floor left today, 15 min at ≤ floor or none left this hour. floor = max(500, limit-day / 10) |
| a new station | at once, unless a refused token is backing off |
| 429 | `Retry-After`, clamped to 60 s – 1 h; 15 min without one |
| AUTH | 15 min, doubling to 6 h |
| NET, TLS, HTTP, BAD, BIG, NOMEM, LOW MEM, TASK | 60 s, 120 s, then 300 s |
| no Wi-Fi, no clock | when there is (certificates need the date) |

Home Assistant keeps its own 20 s when its poll is on. The panel is the one that
yields, so the dashboard keeps working when the day's requests run low.

### Why a task, and not the loop

A fetch is a DNS lookup, a TLS handshake with certificate verification, a
download of up to a megabyte and a parse: seconds, not milliseconds. In `loop()`
that would freeze the clock, the knob and the web server for that long, and the
15 s watchdog would need the trick the AIS client uses (unsubscribing around
the call). So the loop task creates a FreeRTOS task, `rttfetch`, on core 0 at
priority 1 when a fetch is due, and the task deletes itself when it is done.
Nothing large lives between polls: the stack, the TLS session, the HTTP client,
the body and the JSON all exist only while it runs. The finished lists are
handed over under a spinlock, and the loop task copies them into the board -
where the MQTT ingest and the render already run, so the render needed no
change.

### TLS

The AIS client passes an empty fingerprint, which makes the library call
`setInsecure()`: no certificate is checked. **This client verifies**, because
what it sends is a bearer token that grants the owner's API access; anything
able to answer for `data.rtt.io` on the way would receive it.

`rtt_roots.h` pins **ISRG Root X1** (valid to 2035) and **ISRG Root X2** (to
2040), 2.7 KB, taken from macOS's system root store. On 2026-09-14 `data.rtt.io`
presented `*.rtt.io ← Let's Encrypt YE2 ← ISRG Root YE ← ISRG Root X2 ← ISRG Root
X1`, and `openssl verify` accepted the leaf with X1 alone and with X2 alone. The
server offers TLS 1.2 with ECDHE-ECDSA-AES256-GCM-SHA384 (checked with
`openssl s_client -tls1_2`); this SDK's mbedTLS has TLS 1.2, ECDHE-ECDSA, GCM
and both P-256 and P-384 (`sdkconfig.h`).

Considered and not taken: the ESP-IDF certificate bundle
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` is on in this SDK) would survive a change of
CA, but needs the bundle embedded in flash, far more than two roots. If RTT
moves to a CA outside ISRG, fetches fail as `TLS` and Home Assistant's boards
take over.

TLS allocations go to PSRAM through `tlsUsePsram()` (src/network/tls_psram.cpp).

### Memory

**Internal SRAM while a fetch runs, estimated:**

| | bytes | source |
|---|---:|---|
| task stack | 12 288 | `kStackBytes`; internal because this SDK has no `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` (src/lua/lua_effects.cpp) |
| task control block | ~350 | not measured |
| `sslclient_context` | 2 208 | measured: the size WiFiClientSecure's constructor passes to `operator new`, read from the built object; under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` = 4096, so internal |
| HTTPClient strings, socket, PCB | ~1 000 | not measured |
| mbedTLS contexts, record buffers, certificates | 0 | PSRAM via `tlsUsePsram()` |
| lwIP buffers | little | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` = 1 |
| **total** | **~16 KB** | for the seconds a fetch runs; 0 between polls |

**With the AIS websocket open.** Its TLS is in PSRAM too (since `ed201cf`); its
own `sslclient_context` and websocket buffers are part of the running baseline
while the yacht page is up, and the two sessions share nothing. Against the
~43 KB low-water mark from the first brief, a fetch would leave about 27 KB.
The task is not started with less than **28 KB** internal free or no free block
of **13.3 KB**; the page then shows `LOW MEM` and tries again in 60 s.

These are estimates. Every fetch measures the real numbers, in
`/api/railboard` → `direct`: `heapBefore`, `heapMin` (sampled after connecting,
after the body and after parsing) and `stackFree` (the stack's high-water mark).

**Sizes, designed for.** The api-specification repository has no example
responses - only field names, types and one-value examples in `main.yml` - so
`tools/railboard/gen_rtt_fixture.py` builds answers with every field the
line-up schema lists:

| services | compact | indented | filtered parse peak (64-bit host) |
|---:|---:|---:|---:|
| 60 | 142 KB | 227 KB | 71 KB |
| 120 | 284 KB | 454 KB | 140 KB |
| 240 | 568 KB | 908 KB | 278 KB |

The body buffer is **1.5 MB** and the JSON cap **384 KB**, both PSRAM and both
only while a fetch runs: about 390 indented services or 300 parsed. Past that
the fetch reports `BIG` or `NOMEM`. On the 32-bit target a parsed value takes
fewer bytes than on the host, so its peak should be lower. The real answer's
size is in `/api/railboard` as `bytes`.

### The transform, three times

The same rules exist in three places, and the tests hold them together:

- `rtt_transform.cpp`, C++, on the panel;
- `tools/railboard/rtt_client.py`, Python, for Home Assistant;
- the Jinja template of the earlier package, which both were ported from.

`check_direct.py` requires the C++ and Python results to be identical on
`samples/rtt_location_small.json`, a 19-service fixture with one service per
rule. The Jinja was never executed (no jinja2 here), so the ports' faithfulness
to it rests on reading. One known difference, by design: a time without a zone
is refused by both ports, where `as_timestamp` would read it in Home
Assistant's zone. The spec rules such times out (line 183).

## Home Assistant

The owner's token is a **refresh** token (confirmed). An automation can call
`/api/get_access_token`, but the access token would then sit in a response
variable, and automation traces store the variables that change
(`homeassistant/helpers/trace.py`, `changed_variables`) and are saved to
`.storage/trace.saved_traces`. No template or helper keeps it out. So the RTT
call moves to Python, in one of two forms. **Install one, never both.**

| | A. AppDaemon app (recommended) | B. shell_command |
|---|---|---|
| files | `appdaemon/railboard_rtt.py`, `rtt_client.py`, `apps_railboard.yaml` | `ha_package_railboard_shell.yaml`, `rtt_client.py` |
| refresh token | AppDaemon's `secrets.yaml` (0600), a second copy on the host | only in `/config/secrets.yaml` |
| access token | AppDaemon process memory | a 0600 file in the container's `/tmp`, gone on restart |
| runs | one long-lived app, as `flight_board` does on this AppDaemon | a Python process every 20 s |
| publishes | straight to the broker with paho (no `call_service` in the recorder) | `mqtt.publish` from an automation |
| trace | none | the script's printed JSON: boards and outcome, no token |
| depends on | the AppDaemon add-on (installed), paho (imported by `flight_board`) | `python3` in the homeassistant container (not checked) |

Both back off the same way: a refused token makes no request for 15 min,
doubling to 6 h; a 429 waits out `Retry-After`. The poll that got 401 every
20 s, and was switched off on 2026-09-14 at 19:14, is gone from
`ha_package_railboard.yaml`, which keeps only the station helper and the two
automations that make no RTT call.

## What the owner does

**Path 1, the panel** (from the repository, on his machine):

```bash
python3 tools/provision_secrets.py rtt-from-ha --kind refresh   # prints "found, N chars", never the value
python3 tools/provision_secrets.py check                         # set / length only
pio run -e provision -t upload                                   # writes rb/token and rb/kind; prints a length
pio run -e matrix-waveshare-rgb -t upload
pio run -e provision -t clean                                    # the token out of the build cache
```

`rtt-from-ha` runs `ssh nickohome 'sudo grep ^rtt_bearer: /config/secrets.yaml'`,
keeps what it prints inside the process, strips the quotes and `Bearer `, and
writes one line into the gitignored `provision_secrets.ini` (0600), leaving the
broker and AIS lines as they are. It refuses a value with a space, quote,
backslash, `;`, `#` or `$`. Without ssh: `rtt-prompt --kind refresh` asks
without echo. To remove a stored token: add `-DPROV_RTT_CLEAR` to the ini and
run the provision image. The portal then shows the token as set, its kind,
`validUntil`, the last HTTP status, the quota and the fetch age.

**Path 2, option A** (review first; nothing has been installed):

```bash
# 1. The token into AppDaemon's secrets, on the host; prints only its length:
ssh nickohome 'sudo sh -s' < tools/railboard/appdaemon/copy_rtt_token.sh
# 2. In /addon_configs/a0d7b954_appdaemon/secrets.yaml, by hand: railboard_mqtt_user and
#    railboard_mqtt_pass, the broker login the flight_board app uses.
# 3. The app and its client into AppDaemon's apps directory:
scp tools/railboard/rtt_client.py tools/railboard/appdaemon/railboard_rtt.py nickohome:/tmp/
ssh nickohome 'sudo cp /tmp/rtt_client.py /tmp/railboard_rtt.py /addon_configs/a0d7b954_appdaemon/apps/ && rm /tmp/rtt_client.py /tmp/railboard_rtt.py'
# 4. Append the entry in tools/railboard/appdaemon/apps_railboard.yaml to
#    /addon_configs/a0d7b954_appdaemon/apps/apps.yaml (after a backup, as the earlier ones there).
# 5. Replace /config/packages/railboard.yaml with tools/railboard/ha_package_railboard.yaml;
#    reload automations and input_text, or restart Home Assistant.
# 6. appdaemon_main.log should say "rail board: polling Realtime Trains every 20 s".
```

**Path 2, option B** instead of A: the install steps at the top of
`ha_package_railboard_shell.yaml`.

Never enable debug logging for `homeassistant.components.rest_command` or
`shell_command` while testing: the first logs request headers, the second the
command's output.

## Payloads (schema v1)

All topics are retained. The panel refuses a board whose `v` is not 1, whose
`dir` does not match its topic, or whose `crs` is not the selected station, and
keeps what it was showing.

### `…/select` (published by the panel)

```json
{"crs":"GLD"}
```

### `…/<crs>/departures`, `…/<crs>/arrivals`

```json
{"v":1,"crs":"GLD","stn":"Guildford","dir":"dep","ts":1789391107,"rt":"OK",
 "s":[{"t":1789391280,"x":1789391700,"p":"5","n":"London Waterloo","o":"SW","st":"late","d":7}]}
```

| key | meaning |
|---|---|
| `ts` | when the answer was fetched, UTC epoch seconds. Staleness is judged by this |
| `stn` | `query.location.description`, as RTT spells it; the code for a 204 |
| `rt` | `systemStatus.realtimeNetworkRail`: `OK`, `REALTIME_DATA_LIMITED`, `REALTIME_DATA_NONE` |
| `s` | at most 8 services, sorted by scheduled time |
| `t` | scheduled (`scheduleAdvertised`), UTC epoch seconds |
| `x` | `realtimeActual`, else `realtimeForecast`, else `realtimeEstimate`; 0 if none |
| `p` | platform, `actual` else `planned`, ≤3 chars |
| `n` | destination (departures) or origin (arrivals); several joined with ` & `, cut to 24 on a word |
| `o` | `scheduleMetadata.operator.code`, or `BUS` for any bus `modeType`. Not drawn |
| `st` | `ok` · `late` (expected minute after scheduled) · `canc` · `nr` (no realtime time yet) · `arr` (arrived) |
| `d` | minutes late, for `late` and `arr` |

A service counts as cancelled when its event has `isCancelled`, when
`displayAs` is `CANCELLED` or `DIVERTED`, or when it `TERMINATES` here (for
departures) or `STARTS` here (for arrivals). Passing trains (`displayAs` `PASS`
or null), non-passenger services, `OPERATIONAL_ONLY` calls, set-down-only calls
on departures and pick-up-only calls on arrivals are left out.

### `…/<crs>/status`

```json
{"v":1,"at":1789391700,"err":"NET","code":0,"retry":0,"rl":""}
```

`err` is empty on success, else `AUTH` (401, 403), `RATE` (429), `HTTP` (other
non-2xx), `BAD` (not a line-up), `NET` (no answer). `retry` is `Retry-After`; `rl`
is `X-RateLimit-Remaining-Day` as sent.

### `…/<crs>/config` (optional)

```json
{"v":1,"rows":8,"switch_s":10,"level":100,"stale_s":80,"diag":false}
```

| key | range | |
|---|---|---|
| `rows` | 1–8 | services listed; six to a page, a second page for more |
| `switch_s` | 3–600 | seconds each list is on screen; with two pages, each page gets half |
| `level` | 10–100 | percent of full colour on this page |
| `stale_s` | 30–3600 | Data updating after this long without a fresh board |
| `diag` | bool | show diagnostics instead of the board |

## Layout

Modelled on the owner's photograph of a UK station screen: black ground, white
title and column headings, amber rows in mixed case, a large amber clock at the
foot, pages when the rows do not fit. Unchanged by the direct fetch.

```
 y   departures                                   arrivals
 0   Departures (big)            Plat Expt        Arrivals (big)     Time Plat Expt
 9   Time Destination            Guildford        From                    Guildford
16   14:08 London Waterloo          5 On time     Redhill            14:04    2 14:06
23   14:12 Portsmouth               3 14:19       Reading            14:14      Cancelled
..   six rows to a page                           page 2 starts with Continued......
57   Page 1 of 2                   14:05:14 (big) Page 1 of 1                 14:05:14
```

When no board has arrived at all, the first row says `Waiting for GLD`, or
`RTT token refused` once RTT has refused the panel's token.

| scene | preview |
|---|---|
| departures, page 1 of 2 | [`departures.png`](../../tools/railboard/preview/departures.png), 1:1 [`departures_128x64.png`](../../tools/railboard/preview/departures_128x64.png) |
| arrivals | [`arrivals.png`](../../tools/railboard/preview/arrivals.png), 1:1 [`arrivals_128x64.png`](../../tools/railboard/preview/arrivals_128x64.png) |
| page 2, Continued...... | [`departures_page2.png`](../../tools/railboard/preview/departures_page2.png) |
| delays | [`delay.png`](../../tools/railboard/preview/delay.png) |
| cancellations and a late arrival | [`cancellation.png`](../../tools/railboard/preview/cancellation.png) |
| 10 min without data | [`stale.png`](../../tools/railboard/preview/stale.png) |
| a station name too long | [`long_name.png`](../../tools/railboard/preview/long_name.png) |
| diagnostics | [`diagnostics.png`](../../tools/railboard/preview/diagnostics.png) |
| before any board | [`waiting.png`](../../tools/railboard/preview/waiting.png) |

## Diagnostics

`UPDATED`: when the newest board was fetched, London time, and how long ago.
Then services and receipt age per list; `HA`: Home Assistant's last attempt;
`RTT`: the panel's own fetch (`DIRECT OK 200`, `DIRECT AUTH 401`, `HA ONLY` with
no token) followed by RTT's realtime status; MQTT state and refused payloads;
internal heap now and lowest, and the JSON parse peak; `SELECT`: the station and
whether its selection has gone out, then the day's remaining requests (the
panel's count, else Home Assistant's) or `NO NTP`.

## Web portal

Panel → Rail board. **Station**: a validated code field and presets checked on
National Rail's station pages (GLD, WAT, WOK, RDG, GTW, LRD). **Status** adds:

- `source`: direct from Realtime Trains, Home Assistant, or none fresh, with the
  count of Home Assistant boards set aside;
- `direct`: the last state and HTTP status, the last fetch's age, the next in;
- `token`: not set, or set with its kind (access token; refresh token, exchanged;
  refused) and the access token's expiry. The route never carries a token;
- `quota`: requests left today, of the limit, shared by panel and Home Assistant,
  and the panel's slowed interval.

`/api/railboard` → `direct` also carries `heapBefore`, `heapMin`, `stackFree`,
`bytes`, `services`, `jsonPeak`, `polls` and `fails`.

## London time

Computed from UTC by `uk_time.h`: The Summer Time Order 2002 (SI 2002/262),
article 2(2). `check_uk_time.py` compares it with zoneinfo `Europe/London`:
548 480 instants, 0 differ.

## Verified

RTT spec, `realtimetrains/api-specification` at `57ace42` (still HEAD on
2026-09-14), `specification/main.yml`:

| fact | lines |
|---|---|
| server `https://data.rtt.io`, the only one | 74–76 |
| bearer auth, applied globally | 98–101, 789–790 |
| access token or refresh token; a refresh token is exchanged | 12–21 |
| `GET /api/get_access_token` → `token`, `entitlements`, `validUntil` | 1644–1669 |
| `GET /gb-nr/location`, parameters, 200 / 204 / 400 | 1018–1169 |
| times, `isCancelled`, `displayAs`, platform, operator, origin/destination | 185–476 |
| 429 with `Retry-After`; `X-RateLimit-*-<dimension>` | 37–46 |
| tokens in distributed applications | 19 |

Run on this machine:

- `matrix-waveshare-rgb` and `provision` build, 0 warnings. `tools/flag_matrix.py`:
  **24/24** as intended, including `rail direct + board` building and
  `rail direct, no board` refused - run on the branch at each base it was
  rebased onto, last on the tip over `66bee4b` (2026-09-14).
- `tools/railboard/check_direct.py`: time parsing (zones, fractions, refusals),
  the query, the token answer, the transform on the fixture with and without the
  parse filter, the sizes above, and C++ = Python on the fixture.
- `tools/railboard/check_rtt_client.py`: the Python client against a local fake
  RTT - refresh token used as access token (401, exchange, retry), reuse,
  re-exchange near expiry, kind refresh, a refused token (no request through
  10 min of polls, then the back-off doubles), 429, 204, a bad body, no network,
  the shell_command path, and the AppDaemon app on stand-in modules. No made-up
  token appears in anything returned, printed, logged or published.
- `tools/provision_secrets.py` token handling, with a made-up value: each
  secrets.yaml form parsed, never printed, other lines kept, 0600, refusals.
- `tools/railboard/appdaemon/copy_rtt_token.sh` with fake files under `sh` and
  `bash --posix`.
- The pinned roots verify `data.rtt.io`'s chain (`openssl verify`), and the
  server speaks TLS 1.2 with a suite this mbedTLS has.
- AppDaemon on nickohome, read only: config in `/addon_configs/a0d7b954_appdaemon`,
  its own `secrets.yaml`, HASS plugin only, `flight_board` imports paho.
- The portal script passes JavaScriptCore's `checkSyntax`; the YAML files parse
  and their Jinja blocks close.

## Not verified

- **Nothing has run on hardware.** The task, TLS with pinned roots in a task,
  the 12 KB stack, the heap estimate and the size caps are untested on the
  panel; `/api/railboard` will report the real numbers.
- **No request from this work has reached Realtime Trains.** That RTT answers a
  refresh token with 401 on `/gb-nr/location` comes from Home Assistant's
  status as the coordinator reported it. The exchange's real answer,
  `validUntil`'s format, whether an exchange counts against the quota, the
  rate-limit headers' presence and values, and the real response size are the
  spec's word or unknown.
- **Nothing was installed in Home Assistant or AppDaemon.** Not seen: that this
  AppDaemon calls callbacks with either convention the app accepts; that it
  reloads a new app by itself; that its admin page on port 5050 does not show
  app arguments; `python3` in the homeassistant container (option B).
- The Jinja template the ports came from was never executed.
- `Retry-After` as an HTTP date is read as absent (15 min).

## Choices that are not standards

Design choices, taken from the owner's brief, the panel and the photograph:

- Direct fetch every 30 s (the brief); 120 s and 15 min when the quota runs low;
  floor max(500, limit-day / 10).
- A refused token: 15 min doubling to 6 h. Errors: 60, 120, 300 s.
  `Retry-After` clamped to 60 s – 1 h.
- Access token used until `validUntil` − 5 min; 10 min when `validUntil` is missing.
- Task stack 12 KB; start only with 28 KB internal free and a 13.3 KB block.
- Body buffer 1.5 MB, JSON cap 384 KB.
- 10 s per list, six rows to a page, `RB_HOLD_S` 60 s, `stale_s` 80 s, grace
  60 s (120 s once arrived), lookback 30 min and window 90 min, amber 255,150,0.
- Late from one minute: the expected minute differs from the scheduled one.

## Cost

`RAILBOARD_DIRECT_ENABLED` against its base `66bee4b`: **+24 664 B flash,
+248 B RAM** (static; 1 978 481 → 2 003 145 and 92 396 → 92 644). Measured
against each base it was rebased onto: +24 572 B on `1c82839`, +24 568 B on
`ab4314c`; RAM +248 B every time. Most of the flash is
HTTPClient and WiFiClientSecure's use of mbedTLS certificate verification, which
nothing else on the board linked before, plus the transform and the two roots.

## Merge touch points

- New: `rb_model.h` (RbService and RbBoard, moved out of `railboard.cpp`),
  `rtt_transform.{h,cpp}`, `rtt_direct.{h,cpp}`, `rtt_roots.h`.
- `railboard.cpp`: board sources and precedence, `applyDirect()`, the waiting line,
  diagnostics lines 5 and 8, `/api/railboard` `source`, per-list `src`, `direct`,
  `haShadowed`. `railboard.h`: the two `#error` guards. `main.cpp` and
  `web_panel.cpp` untouched.
- `platformio.ini`: `-DRAILBOARD_DIRECT_ENABLED`. `tools/flag_matrix.py`: a build
  row, a refusal row, the flag in "everything".
- `bringup/provision.cpp`, `provision_secrets.example.ini`,
  `tools/provision_secrets.py`: `rb/token`, `rb/kind`, `rtt-from-ha`, `rtt-prompt`.
- `web_panel_page.h`: four Status rows and the lede; `web_panel_js.h`: `renderRb()`.
- `tools/railboard/render.py`: sizes from `rb_model.h`, diagnostics as built.
- Home Assistant: `ha_package_railboard.yaml` (poll removed),
  `ha_package_railboard_shell.yaml`, `rtt_client.py`, `appdaemon/`.
- Tests: `check_direct.py`, `direct_host_test.cpp`, `gen_rtt_fixture.py`,
  `samples/rtt_location_small.json`, `check_rtt_client.py`.

## Status and next steps

Done on `wip/rail-direct`: the panel's direct fetch with the refresh-token
exchange, provisioning without showing the token, the portal rows, the Home
Assistant side in two leak-free forms, host tests, build and flag matrix.
Open, in order:

1. Coordinator review of the Home Assistant files; the owner picks A or B.
2. The owner provisions the token and flashes (commands above), then reads
   `/api/railboard` → `direct` for a day: `state`, `kind`, `validUntil`, `left`,
   `bytes`, `heapMin`, `stackFree`. Tune the stack, the caps and the floor from
   those numbers.
3. Save one real `/gb-nr/location` answer (without its headers) as a fixture and
   rerun `check_direct.py` against it.
4. Decide on RTT's line 19 for path 1.

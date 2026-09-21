# Working with this panel

You are looking at the firmware for a **128x64 HUB75 RGB LED matrix on an
ESP32-S3**, and at everything needed to drive it, watch it and add screens to
it. This file is what an agent arriving cold needs; it is kept honest, and where
it is unsure it says so.

**A running panel answers on your network**, from any machine on it. Everything
below is an HTTP call. There is no authentication - it is a device on a home LAN.

### Find it first. Never hard-code an address.

A panel's IP comes from the router and moves. What does not move is the MAC the
firmware advertises over mDNS (`src/network/network.cpp:232-236`):

    _http._tcp  port 80
    TXT: model=AnimatedPixelClock  version=<firmware>  mac=<the chip's>

```bash
python3 tools/agent/discover.py            # every panel on this network, one line each
python3 tools/agent/discover.py --json     # the same, for a tool to read
python3 tools/agent/discover.py --mac 90:E5:B1:D2:0E:C8   # just that one's address
```

```
  90:E5:B1:D2:0E:C8  NickoScope-64x128.local   v2.5.0   NickoScope-64x128
```

It works from macOS (`dns-sd`) and Linux (`avahi-browse`), and it confirms every
advertisement by asking the panel itself - a stale mDNS record for a panel that
has gone is worse than no answer, because you would go on to talk to nothing.

**With several panels on one network, identify them by MAC, not by address.**
`discover.py` exits **3** when it finds more than one and nobody said which:
that is a decision for the caller or a person, not a failure. Pass `--mac` to
name one.

```bash
PANEL=$(python3 tools/agent/discover.py --mac 90:E5:B1:D2:0E:C8) || exit
curl -s "http://$PANEL/api/info"
```

Everything below writes `$PANEL`, and never an address.

---

## 1. What this panel can do right now

Sixteen pages, walked by a knob, by a carousel, or by you over HTTP.

| # | page | what it is |
|---|---|---|
| 0 | CLOCK | the clock, in one of ~15 drawn styles (Mario, Pac-Man, Tetris, Asteroids, Weather…) |
| 1-6 | FOOTBALL / MINECRAFT / ROOM RADAR / SNAKE / SNOOKER / TETRIS CLOCK | clock faces written in Lua |
| 7 | WORLD CLOCK | several cities |
| 8 | FLIGHTS | live departures/arrivals, FlightAware AeroAPI, **metered** |
| 9 | TRAINS | live UK departures/arrivals, Realtime Trains |
| 10-13 | MARKETS / TICKER / PORTFOLIO / HOLDINGS | market data via Home Assistant |
| 14 | YACHTS | AIS vessel radar (a long-lived websocket; heavy) |
| 15 | MEDIA | now playing + a remote for a Home Assistant / Music Assistant player |

Also on board: an indoor temperature/humidity sensor, an IR receiver slot, a
rotary knob with a button, SD card clips, a 3D effects engine, an audio
visualiser (microphone), MQTT to Home Assistant, OTA updates, and a
configuration portal served from the device itself.

Which of these are compiled in depends on build flags - see `platformio.ini`,
environment `matrix-waveshare-rgb`. `GET /api/info` tells you what a running
panel actually has.

### The page numbers are not fixed. Read them.

`CtrlPage` in `src/main.cpp:91-122` is assembled from `#if` blocks, one per
build flag. Turn a module off and every page after it shifts down. The table
above is this build; **it is not a constant**.

```bash
curl -s http://$PANEL/api/panel | python3 -c \
  'import json,sys; [print(p["i"], p["key"], p["name"]) for p in json.load(sys.stdin)["pages"]]'
```

Match on `key` or `name`, never on a number you read in a document.

---

## 2. The kinds of screen, and what each one needs from you

Six kinds, and they differ in **where a screen's content comes from** - which is
what decides how you drive it, how it fails, and what it costs.

### a. Clock styles — page 0, chosen with `style`

About fifteen faces (Mario, Pac-Man, Tetris, Asteroids, Dino, Tron, Weather…),
all drawn in C++ every frame from the time and nothing else.

*Working with them:* `{"style": N}`, and `styles[]` in `GET /api/panel` gives the
ids - **which are not contiguous**, so iterate the list rather than counting.
They are a second axis, not pages: page 0 plus a style. No network, no budget,
they cannot fail from outside. The carousel walks them by itself when
`carousel.allStyles` is on.

### b. Lua effect pages — one page each

Scripted faces: football, Minecraft, room radar, snake, snooker, Tetris. Each is
a page of its own so the knob and carousel walk them singly.

*Working with them:* `/api/lua`. They run on their own task with a frame cap and
PSRAM frame buffers, and the panel logs each one's cost - `[luafx] open <name>:
ok in N ms, ~N instr, heap N B, fps cap N, stack free N`. Some are slow to open
(a few hundred milliseconds) and the portal is unresponsive while one does.

### c. Data boards — they fetch, and only while you are looking

World clock, flights, trains, the four market pages, media, yachts.

*Working with them, and this is the part that surprises people:* **a board does
not fetch while its page is off the screen.** That is deliberate - it is the
owner's brief - so "the data is stale" usually means "nothing has looked at it".
Put the page up, wait, then read.

Each has its own rhythm and its own ceiling:

| board | source | rhythm | ceiling |
|---|---|---|---|
| trains | Realtime Trains, direct | ~120 s | 9,000/day, 30/minute |
| flights | FlightAware AeroAPI, direct | on demand | **30 calls/day, ~$0.005 each** |
| markets | Home Assistant over MQTT | pushed | none |
| media | Home Assistant / Music Assistant | pushed | none |
| world clock | one IP lookup per boot | once | none |
| yachts | AIS websocket, **long-lived** | continuous while shown | holds 17.5 KB while the page is up |

They all fetch through one broker now (`src/net/net_broker.h`), one request at a
time, and a change you make by hand - a new station, a new city - goes in as
*interactive* and jumps the queue ahead of scheduled refreshes.

### d. Overlays — drawn on top of whatever is showing

Notifications and cards. They own the screen while they are up, and the first
click dismisses one instead of doing what the click would normally do
(`src/main.cpp`, `cardsNotifyActive()`).

*Working with them:* `/api/notify`, `/api/notify/dismiss`. Be aware that a card
can be on screen when you think a page is, and `GET /api/panel`'s `now.notify`
says so.

### e. fx3d — a scene engine that takes the screen

Three-dimensional scenes with their own looks. When one is running it **owns the
display**, and the first knob event stops it and gives the panel back.

*Working with them:* `/api/fx3d`. Documented separately in the knowledge base
(doc 27) because the scene and look vocabulary is large.

### f. Clips and animations — played from the SD card

*Working with them:* `/api/anim/list|play|upload|delete`, `/api/clips`,
`/api/clips/frame`, `/api/clips/upload`. Note the known fault: `/api/anim/play`
answers ok and `animationPlaying` stays false - it needs eyes on the screen to
confirm, and nobody has.

### Two things true of every kind

**Only one thing draws at a time**, and the precedence is: a notification card,
then fx3d, then the page. If you set a page and see something else, check those
two first rather than doubting the page call.

**The carousel moves pages on its own** unless somebody has touched the knob
recently. `carousel` in `GET /api/panel` gives `enabled`, `idleS`, `slotS`. If
you are testing and the screen keeps wandering off, that is why - and if you
reboot the panel repeatedly, the carousel will walk it onto the metered flight
page for you, which is one way to spend a day's API budget without meaning to.

---

## 3. Read this before you call anything

These are not hypotheticals. Each one cost this project real time in the week
before this file was written.

**A 200 does not mean it happened.** The panel answers `HTTP 200` to payloads it
does not understand, and does nothing. Two documented examples, both of which
were written down *wrong* in the knowledge base for months:

| what looks right | what is right |
|---|---|
| `{"showPage": 9}` | `{"show": {"page": 9}}` |
| `{"styleId": 14}` | `{"style": 14}` |

**So verify every change by reading the state back.** Not the reply - the state.
There is a tool that does this for you (§6).

**An "ok" that only means "it was already like that" proves nothing.** If you set
page 9 and it was already on page 9, your read-back confirms nothing about
whether your call worked. Check what it was *before*.

**HTTP 503 is normal and is not a failure.** When internal memory is tight or a
fetch is on the wire, the panel deliberately refuses expensive responses rather
than starving the radio. Retry after a few seconds, with a bound. A panel that
answers 503 is working correctly; a panel that answers nothing is not.

**Polling changes what you are measuring.** The panel stands aside for a web
client for 1,500 ms after every request (`src/network/net_turns.h`). Poll faster
than that and you suppress every background fetch - weather, clocks, trains,
flights - and then observe that the data is stale. If you are measuring, pace
yourself and say what your pacing was.

**Some calls cost money.** The flight board is metered: 30 AeroAPI calls a day,
~$0.005 each, and it only calls while its page is on screen. Rebooting the panel
thirty times while testing will exhaust the day's budget. The rail board's limit
is generous (9,000/day) but real.

---

## 4. Driving it

### Pages and styles

```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"show":{"page":9}}' http://$PANEL/api/panel
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"style":14}' http://$PANEL/api/panel
curl -s http://$PANEL/api/panel      # now.page, now.style, pages[], styles[], carousel
```

`GET /api/panel` returns `now` (what is on screen this second), the page list,
the style list and the carousel settings. **Read it back after every change.**

### The screen itself

```bash
curl -s "http://$PANEL/api/display/brightness?value=128"   # 0-255
curl -s http://$PANEL/api/display/off
curl -s http://$PANEL/api/display/on
```

### The rail board

```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"crs":"GLD"}' http://$PANEL/api/railboard              # change station
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"favourites":["GLD","WAT","CLJ","WOK","SUR"]}' http://$PANEL/api/railboard
curl -s http://$PANEL/api/railboard
```

`crs` is a three-letter station code, capitals only. `favourites` (up to eight)
is the list the **knob** turns through on the rail page - the configuration
portal has no station controls, by the owner's decision, and this replaced them.

### Everything else

Each page has its own route with the same shape - `GET` to read, `POST` JSON to
change: `/api/worldclock`, `/api/flightboard`, `/api/market`, `/api/yachtradar`,
`/api/media`, `/api/lua`, `/api/fx3d`, `/api/knob`, `/api/notify`,
`/api/anim/*`, `/api/clips/*`, `/api/ir/*`.

**Do not guess payload keys.** Read the handler. Every route is registered in
`src/web/web.cpp` or through the `route()` helper in `src/web/web_panel.cpp` -
and grepping only for `server.on` misses half of them, which has cost two wasted
test sweeps.

---

## 5. Seeing what is happening

This is the part worth learning first. A panel this small fails quietly, and
nearly every mistake in this project's history was made by reasoning instead of
reading.

### The one call that answers most questions

```bash
curl -s http://$PANEL/api/info
```

| field | what it tells you |
|---|---|
| `freeInternalHeap`, `largestHeapBlock` | internal SRAM. **This is the scarce thing** (§7) |
| `allocFails`, `allocFailBytes`, `allocFailTask` | failed allocations - `wifi` means the radio went short |
| `lastCrash` | the last crash from flash. **Check `thisBoot` and `sameFirmware`** before blaming your change |
| `resetReason` | 1 power-on, 3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog |
| `linkRecoveries` | the Wi-Fi watchdog firing. Should be 0 |
| `loopMaxMs`, `loopSlowPart` | the longest `loop()` pass in the last 10 s, and which part |
| `netBroker` | the network broker: what is on the wire, how many served, its stack high-water |
| `climate` | the onboard sensor, raw and corrected |
| `ota` | partition and state - **`valid` before you reboot after an update** |

### The log, over the network

Off by default and free while off. On, it costs 32 KB of **PSRAM** and never
internal RAM, so turning it on cannot change what it is there to observe.

```bash
curl -s "http://$PANEL/api/log?on=1"
curl -s "http://$PANEL/api/log?since=0"      # read from the start
curl -s "http://$PANEL/api/log?clear=1"
curl -s "http://$PANEL/api/log?on=0"
```

Read with the cursor headers: `X-Log-From` (larger than you asked means lines
were dropped while you were away), `X-Log-Bytes` (the body's length **in
bytes**, never measure it as a string), `X-Log-Seq`, `X-Log-Dropped`. A read is
capped at 1 KB, so drain a burst in a loop.

It captures everything written through `dbgLogf`/`dbgLogWrite` - the `[mem]`,
`[loop]`, `[nb]` lines - and the IDF's own `ESP_LOGx`. It does **not** capture
Arduino's `log_e`/`log_w`; in this build those go to the cable only.

### The cable

`/dev/cu.usbmodem*` at 115200, when one is plugged in. Worth grepping for:
`[mem]`, `[loop]`, `[nb]` (the network broker), `[luafx]`, `[audio]`, `[fx3d]`.

**The serial console drives the knob for real** - which is how a screen that is
only reachable by turning a knob gets tested without hands:

```
ir ok           the button
ir cw 1         one detent clockwise
ir ccw 1        one detent anticlockwise
ir ok 1200      a long hold (the firmware treats it as a click anyway)
ir status       what the IR module thinks
```

`/api/ir/sim` over HTTP is **inert** in this build (no receiver compiled). The
serial console is not - do not confuse them.

---

## 6. Tools that already exist

In the knowledge base repository, not here, under `tools/nsc/`:

| tool | what it does |
|---|---|
| `nsc.py` | one JSON object per command, meaningful exit codes, **every change verified by reading it back** |
| `functional.py` | the whole capability list as an executable sweep - 41 checks |
| `stackreport.py` | every function's stack frame, from GCC's `.su` reports |
| `measure/paired.py` | the memory protocol: seven boots, one reading each |
| `measure/visit.py` | one portal visit, the way a browser does it |
| `measure/stress.py` | deliberate abuse: 250 concurrent requests |

`nsc`'s exit codes are worth copying if you write your own: `0` done · `1`
retryable · `2` bad call · **`3` a person must decide** · `4` retrying is
pointless. The third one is the useful one for an autonomous agent.

---

## 7. Memory, and why it dominates everything here

The chip has 512 KB of internal SRAM and 16 MB of PSRAM. **PSRAM is not the
constraint and flash is not the constraint. Internal SRAM is.**

A 128x64 HUB75 panel holds its DMA framebuffer in internal RAM: 32 row-pairs x
128 pixels x 8 bits of colour depth x 2 bytes, double-buffered - **131,072 bytes,
from boot, for ever**. That is where the internal heap goes. Moving it to PSRAM
has been tried (2026-09-14): it frees the 130 KB and breaks both the picture and
TLS certificate verification.

What is left is roughly **19-24 KB free**, and the number that matters is not how
much is free but **how much is free in one contiguous piece**.

Two things follow, and both are counter-intuitive enough to be worth stating:

**`largestHeapBlock` does not decay over time.** Sampled repeatedly within one
boot it does not move by a single byte. It varies *between* boots, in 1,024-byte
steps, with roughly one boot in seven landing 6 KB below the usual band. Anyone
comparing two single readings is measuring the boot, not the change. Seven boots,
compare distributions - `tools/nsc/measure/paired.py`.

**Anything that needs a large contiguous block at an unpredictable moment is the
enemy.** Four modules used to create 8-13 KB fetch tasks on demand; the panel
died when a browser opened the portal at the wrong moment. They now share one
permanent task whose stack is in `.bss` (`src/net/net_broker.h`), so nothing asks
for contiguous memory at a moment nobody chose. If you add a feature, prefer
PSRAM (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`) and prefer
allocating at boot over allocating on demand.

The build enforces stack frames: `-fstack-usage` and `-Wstack-usage=2048` on our
sources. Do not raise a task's stack to fit a work buffer - move the buffer.

---

## 8. Adding a screen

Two routes, and which you want depends on whether you need C++.

*(This section is written from a survey of the source in progress; until it is
finished, read `src/lua/README.md` and `tools/luasim/` for the Lua path, and
`src/clocks/` for a small C++ page to copy.)*

---

## 9. Rules

- **Never flash from an automated tool.** Building and uploading is a human's
  call, at a moment they chose, with the panel in front of them. A tool that can
  reflash a wall-mounted device without a witness is how a bad build becomes an
  outage nobody saw start.
- **After an OTA, wait for `ota.state` to read `valid` before any reboot.**
  Otherwise the image rolls back and you are testing the old firmware while
  believing you are testing the new one.
- **Before flashing anything, do the arithmetic on paper.** Compare
  `largestHeapBlock` against what each module needs contiguous. A build that
  passes this check may still be wrong; a build that fails it is certainly wrong,
  and the check costs nothing.
- **Do not trust this file over the source.** It has been wrong before. Where it
  and the code disagree, the code is right and this file needs fixing - please
  fix it.

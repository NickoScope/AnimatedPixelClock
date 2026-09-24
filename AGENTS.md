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
python3 tools/agent/discover.py --mac AA:BB:CC:00:11:22   # just that one's address
```

```
  AA:BB:CC:00:11:22  NickoSha-64x128.local   v2.5.0   NickoSha-64x128
```

**Is it healthy?** One command, before and after any change you make:

```bash
python3 tools/agent/health.py --panel <name|MAC|IP>   # ~1 min; --read-only to only look
```

It pings underneath, exercises the controls and the portal, and prints a
verdict with findings from the panel's own counters; every raw log goes to
`health-logs/<time>/`. Read the findings, open a log only for what a finding
points at. Same thing as the `panel_selftest` MCP tool.

**Is there a new firmware?** `python3 tools/agent/update.py --panel <name>`
lists every published release newer than the panel, with its notes;
`--install` asks the person three questions and then updates over the air
(MCP: `panel_update_check`, `panel_update`). The person answers, not you.

It works from macOS (`dns-sd`) and Linux (`avahi-browse`), and it confirms every
advertisement by asking the panel itself - a stale mDNS record for a panel that
has gone is worse than no answer, because you would go on to talk to nothing.

**With several panels on one network, identify them by MAC, not by address.**
`discover.py` exits **3** when it finds more than one and nobody said which:
that is a decision for the caller or a person, not a failure. Pass `--mac` to
name one.

```bash
PANEL=$(python3 tools/agent/discover.py --mac AA:BB:CC:00:11:22) || exit
curl -s "http://$PANEL/api/info"
```

Everything below writes `$PANEL`, and never an address.

---

## Bringing a new panel to life

A panel arriving fresh needs three things, in this order, before anything else
is worth doing with it - and before all three, if the board has never been
flashed, step 0.

### 0. A board with nothing on it

Everything else in this file is an HTTP call, which means it starts at a panel
that is already running this firmware and already on the network. Getting it
there is the one part you do not do alone: **you build, a person uploads**, and
a person types their own Wi-Fi password.

**Offer the browser first.** <https://nickoscope.github.io/AnimatedPixelClock/>
is this repository's own flasher - ESP Web Tools over WebSerial, Improv for the
Wi-Fi in the same dialog, all three boards including this one. Chrome or Edge on
a desktop, cable, Install, thirty seconds. A person still picks the port and
types the password; you cannot click a native browser dialog and should not try.
Build from source only when the published image lacks something they need.

**The SDK owns this, not this file.** `panel_bringup` in
`tools/agent/mcp_server.py` is the full version and the one that is kept
current: every build environment with the module it belongs to, the memory-type
trap that kills a Waveshare board on every boot, the two steps that are the
person's, and what to do the moment the panel first answers.

Call the tool if this server is registered; read it in the source if it is not -
it is one function and it is plain data.

The short of it: pick the environment by the **module**, never by the board's
marketing name - `matrix-waveshare-rgb` for the WROOM-2-N32R16V here,
`matrix-s3-wroom` for the 16 MB devkit, `matrix-s3` for the 4 MB boards. Build
it, hand over the upload command with an explicit `--upload-port`, and let the
person bring it onto their network.

Once `discover.py` sees it, you are at step 1 and everything below works.

### 1. A name. Asked for, never invented.

Every panel out of a flash calls itself whatever the build's default was, and so
does the next one. Two of them on one network advertise the same mDNS name and
the only thing telling them apart is the MAC. So the first act is to give this
one a name of its own - and to **ask the person for it**, because it is what
they will type for the rest of the panel's life. An agent does not choose it.

```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"name":"NickoSha-64x128"}' "http://$PANEL/api/rename"
# {"success":true,"name":"NickoSha-64x128"}
```

The rules are enforced in the handler (`src/web/web.cpp:613-649`): **1 to 31
characters, letters, digits and hyphens only, and it must start with a letter.**
Anything else is a 400 that names the rule. The route saves to NVS and restarts
mDNS on the spot - **no reboot**, and the panel answers on the new
`<name>.local` within a couple of seconds.

**Do not use the portal's "Device name" field for this.** The portal posts to
`/save`, which is a whole-form replace of about 125 fields, and every boolean it
does not carry is written back as *false* (section 3). `/api/rename` is one
field and touches nothing else. Tried here on 2026-09-21: filling the portal
field and pressing Save & apply rebooted the panel and left the name unchanged;
`/api/rename` did it in one call. The portal's own `/save` path for `deviceName`
looks correct in the source, so this is recorded as what happened rather than
as a diagnosed bug - either way, use the single-purpose route.

Then read it back from the panel itself, and re-find it by MAC - the name
changed, the MAC did not:

```bash
python3 tools/agent/discover.py --mac AA:BB:CC:00:11:22
```

### 2. No Home Assistant? Switch off what has nothing behind it.

**Several pages have no source other than MQTT.** On a panel with no broker they
are not "empty for now" - nothing will ever arrive, and the carousel still
dwells fifteen seconds on each of them. So probe, and switch those off.

The probe is the same object on three routes - `/api/flightboard`,
`/api/railboard`, `/api/media` all return `mqtt{configured,connected,status}`;
whichever the build has will answer:

```bash
curl -s "http://$PANEL/api/railboard" | python3 -c 'import sys,json;print(json.load(sys.stdin)["mqtt"])'
# {'configured': True, 'connected': True, 'status': 'NO DATA'}
```

What needs what, from the source rather than from habit:

| Page | Where its data comes from | With no Home Assistant |
|---|---|---|
| CARDS | `nickoscope_matrix/card/+`, `icon/+` (`src/cards/cards.cpp:124-129`) | **switch off** - MQTT is the only source |
| MEDIA | `nickoscope_matrix/<dev>/media/+` (`src/media/media_ha.cpp:218-225`) | **switch off** |
| MARKETS · TICKER · PORTFOLIO · HOLDINGS | `nickoscope_matrix/<dev>/market/#` (`src/market/market_ha.cpp:236-243`) | **switch off** |
| FLIGHTS | MQTT via HA **or** its own AeroAPI key | keep it **if** `/api/flightboard` → `direct.key` is true |
| TRAINS | MQTT via HA **or** its own RTT token | keep it **if** `/api/railboard` → `direct.token` is true |
| YACHTS | straight to `wss://stream.aisstream.io` (`src/yachtradar/yachtradar.cpp:342`) | keep - never used HA |
| ROOM RADAR | MQTT presence, with a scripted story as fallback | keep - it draws the story, not real people |
| CLOCK · WORLD CLOCK · Lua effects | the panel itself | keep |
| Indoor temperature | a local sensor; MQTT only *publishes* it to HA | keep - the reading is local |
| Notifications | MQTT **or** `POST /api/notify` | keep - HTTP works with no broker |

Switching one off is one call per key, and `clock` cannot be switched off:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"enable":{"key":"media","on":false}}' "http://$PANEL/api/panel"
```

The answer carries the whole new state, so **compare `pages[]` rather than
trusting the 200** - see section 3 for why that matters here.

### 3. The panel is yours, and so is the fork.

**Work on your own panels in your own fork of this repository.** Your names,
your stations and airports, your keys, your screens - they belong in your fork
and in your NVS, not in a pull request. Nothing here reads a central server and
nothing here phones home; a panel is a device on your LAN and the repository is
just the firmware that runs on it.

**Issues and pull requests are welcome upstream.** A fix, a new screen, a trap
you hit that this file should have warned you about - open an issue or send a
PR. What stays in your fork is your configuration; what comes back upstream is
anything that would help the next person.

### 4. A new screen needs no flash

**Replacing one that is on screen takes effect at once.** Until 2026-09-22 it
did not: `luaEffectsSelect` returns early when the index has not changed, which
is right for a knob and wrong for an upload, so the chunk compiled from the old
file kept running and a new version looked identical until you left the page
and came back. `luaEffectsReload()` now bumps the sequence word the effect task
keys its reload off, and the upload path calls it whenever an uploaded script
is showing.


Since 2026-09-21 a Lua effect can be sent to a running panel and shown straight
away. Thirty-six fit (none are compiled in since 2.6.0), they survive a
firmware update, and the whole loop is one MCP call:

```bash
python3 tools/luasim/gen_effects.py --help   # (the compiled-in route, rarely wanted now)
```

```
effect_api  ->  effect_write  ->  effect_preview  ->  effect_check  ->  effect_upload
                                                                        ^ on the panel,
                                                                          no build
```

Over HTTP directly:

```bash
curl -X POST -F "script=@my_effect.lua" "http://$PANEL/api/lua/upload?name=my_effect"
curl -X POST -H 'Content-Type: application/json' -d '{"show":7}' "http://$PANEL/api/lua"
curl -X POST -H 'Content-Type: application/json' -d '{"delete":"my_effect"}' "http://$PANEL/api/lua"
```

`GET /api/lua` reports `uploaded{count,slots,builtIn,maxBytes,ceiling,maxDepth,fsFree,scripts}`
(`maxBytes` is how big an upload may be right now, `ceiling` the most it ever may)
and `stackFreeMin`.

**What the panel refuses, and why it is not fussiness.** More than the
filesystem has room for - since 2.6.3 a script may be as big as it needs, like
a file: the room is LittleFS's free space less 512 KB kept for everything else,
under a 512 KB ceiling set by parse time (`maxBytes` says how much now); nothing
called `draw`; blocks and brackets nested deeper than 16. That last one is the
interesting one: the effect task has a 12 KB stack and Lua's parser recurses
with the source's nesting at up to 384 bytes a level, so depth is the one thing
a script can spend that the instruction and time budgets do not see. Blocks
count as well as brackets - `local function` inside `local function` is the
expensive kind and has no bracket in it.

**`pcall` and `xpcall` are not in the sandbox.** A nested pcall costs 560 bytes
of C stack a level, and `local function f() pcall(f) end` is three lines that no
reading of the source can recognise as deep. An effect is a draw loop and its
failures are caught around `draw()` anyway.

Measured on the panel, 2026-09-21: the deepest script it will accept leaves
**6,684 bytes of the 12,288 free** - read it yourself from `stackFreeMin`. A
760,000-instruction effect that loads for 874 ms sets no new low, because stack
depth follows nesting and not work.

**The two routes that carry code refuse a foreign `Origin`.** Nothing on this
panel is authenticated and that stays the posture - but `multipart/form-data`
is CORS-safelisted, so without it any page in your browser could POST a script
to the panel's address with no preflight. `curl` and the agent tools send no
Origin and are unaffected.

---

### 5. A photograph on the panel

One call, and it is a photograph rather than ASCII art:

```
effect_photo  image=/path/to/photo.jpg  name=my_photo  crop=0.0,0.02,1.0,0.478
```

It crops, quantises, writes the Lua, runs the panel's own checks, uploads and
shows. `tools/luasim/photo_to_lua.py` is the same thing by hand.

The panel is private, the gallery is not. A photograph goes to the gallery
(`gallery_publish`) only with `photo_no_people=true`, meaning no person is in
it: a landscape, a tree, the sea. A photograph of a person stays on the panel
and in the gitignored `tools/luasim/scripts/private/`.

**Six things about it, each of which cost a try:**

- **The aspect ratio is the first decision, not the last.** The panel is 2:1 and
  almost no photograph is. `aspect=fit` (the default) keeps the whole picture
  undistorted and fills the sides with a blurred darkened copy of itself;
  `fill` crops to 2:1 and loses the edges; `crop` chooses which part.
  Until 2026-09-22 this tool simply resized to 128x64, which squashed a
  1007x1078 portrait **2.14x flat** - and the flattening is not obvious in a
  thumbnail, only on the wall.
- **256 colours, and do not spend more.** `truecolor=true` writes 24-bit colour
  at 35 KB and the panel **cannot show the difference** - tried on 2026-09-22,
  plainly different in a PNG and indistinguishable on the hardware. The panel
  has no colour depth of its own: FM6124 drivers are constant-current sources
  behind a latch, a LED is on or off, and all greyscale is the ESP32 library's
  binary-code modulation where **every extra bit halves the refresh rate**. Its
  own `doc/BuildOptions.md` says that from 64x64 up, 24-bit either flickers or
  loses the shadows. A CIE 1931 table then maps each channel's 256 inputs onto
  **174 distinct outputs**, 82 collapsing in the dark end. At 2 mm pitch under
  GOB epoxy the neighbours blend anyway. See §9's rule: spend bytes on time.
- **Do not run-length encode it.** It was tried, in palette mode. Dithering is
  what keeps a face from banding at this size, and it is exactly what destroys
  runs - 8,192 pixels came out as 7,232 of them, the length character became
  overhead, and the file went over the limit at 25 KB.
- **Painted once.** The canvas is not cleared between frames, so it is drawn on
  the first frame and never again. Measured on the panel: that frame is 235 ms
  of the 500 a draw is allowed and 229,000 instructions of 2,000,000; every
  frame after it is 4.6 ms, which is the clock and nothing else. `FPS = 2` and
  no higher - there is nothing to animate.
- **A little unsharp after the downscale** is worth more than any amount of
  colour. A face at 128x64 has lost every edge it had.

**And a photograph of a person is not a code sample.** It goes in
`tools/luasim/scripts/private/`, which git ignores and the tools look in. This
repository is public. Nothing of the kind goes in `gallery/` unless the owner
asks for it.

---

### The whole of 1 and 2 in one command

```bash
python3 tools/agent/bringup.py                        # what is here, and what it needs
python3 tools/agent/bringup.py --mac <MAC> --check --name <NAME>   # say what would change
python3 tools/agent/bringup.py --mac <MAC> --name <NAME>           # do it
python3 tools/agent/bringup.py --mac <MAC> --name <NAME> --keep-ha # leave the pages alone
```

Called without `--name` **it asks for one and stops with exit code 3** - a name
is a person's decision, and the script will not invent it. It then probes MQTT,
switches off only pages that have no source at all, and reads every change back
before reporting it. `--json` gives the same thing for a tool to read. Exit
codes: 0 done · 2 bad arguments · 3 a person must decide · 4 no panel answered.

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

Scripted screens, each a page of its own so the knob and the carousel walk them
singly. **Since 2.6.0 none are compiled in**: every effect is uploaded (up to 36,
`LUA_USER_MAX`), the former built-ins (football, Minecraft, room radar, snake,
snooker, Tetris, La Gioconda) included, from the gallery.

*Switching them:* since 2.5.7 each effect is in or out of the knob's walk and
the carousel on its own, by name (kept in NVS; `/api/lua` `inWalk`, POST
`{"walk":{"i":n,"on":b}}`, or `/api/panel` `{"enable":{"page":i,"on":b}}`;
MCP `effect_walk`). "Show" still shows one that is out. The page key `lua`
switches them all. Deleting an upload forgets its switch. The portal's
Effects & clips page does all of this and adds from the GitHub gallery
(`gallery/index.json`, written by `tools/gallery_index.py`). A switch may carry
the name it saw (`walk.name`, `enable.name`): if the list moved in between the
panel answers 409 instead of switching the neighbour. An upload whose name
would read as an effect already there (built-in, or another case of the same
stem) is refused. Since 2.5.9 an upload is also **run before it is kept**
(load plus 4 frames, off screen, the real budgets): one that errors or takes
more than 500 ms a frame here is refused with the measured frame times and
never reaches the list; an accepted upload's answer carries `trial` with its
measured cost. Agents publish to and remove from the gallery with MCP
`gallery_publish` / `gallery_unpublish` / `gallery_scoreboard`, into a staging
branch on their own machine; a maintainer brings them to GitHub with
`gallery.py sync` (tools/agent/README.md). A photograph goes to the public
gallery only with `photo_no_people=true`, when no person is in it; a photograph
of a person never does, and the maintainer looks at every preview.

*Working with them:* `/api/lua`. They run on their own task with a frame cap and
PSRAM frame buffers, and the panel logs each one's cost - `[luafx] open <name>:
ok in N ms, ~N instr, heap N B, fps cap N, stack free N`. Some are slow to open
(a few hundred milliseconds) and the portal is unresponsive while one does.

### c. Data boards — they fetch, and only while you are looking

World clock, flights, trains, the four market pages, media, yachts.

*The knob and the remote inside a page* (click / OK to enter, the arrows act
inside, click again to leave; the system corner shows an amber arrow meanwhile):
flights step the airport, trains walk LISTS then STATION, market pages their
window and list, media TUNE then VOLUME, yachts scroll, and since 2.5.9 the
**world clock steps the home city** through the portal's cities, built-in and
custom, keeping the choice as the portal's "home" does.

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

*The system corners* (since 2.5.8, `src/display/sys_corners.h`) sit on every
page, under notifications and the knob's toast, **and since 2.6.1 only for 5 s
after the remote was heard**; the rest of the time the corners belong to the
page. Each is a 5 to 7 px mark on a
black patch, so a screen of your own should leave the top-left 6x6 and the
top-right 8x7 pixels to them.
- **Top left:**
  - A green: the carousel is on and walking;
  - A dim: on, held after a hand turned a page;
  - M amber: off;
  - an amber arrow instead, while a click has entered a page.
- **Top right:**
  - the Wi-Fi icon, green at -67 dBm and better, amber to -80, red below (from
    MetaGeek's RSSI table);
  - a red cross when not connected;
  - a blinking red 6x6 dot while the remote is being received.

### e. fx3d — a scene engine, and **not in the firmware you are talking to**

Three-dimensional scenes with their own looks, which own the display while one
is running. It is listed here so nobody spends an afternoon on it: **`FX3D_ENABLED`
is set only in `env:matrix-waveshare-rgb-fx3dbench` (`platformio.ini:253-257`),
not in the shipping `env:matrix-waveshare-rgb`.** A route whose module is not
built is not registered, so `/api/fx3d` and `/fx3d` answer **404** on a normal
panel - checked against this one on 2026-09-21, HTTP 404.

*If you want it:* build the bench environment. The scene and look vocabulary is
in the knowledge base (doc 27). Doc 29 lists these two routes among the
firmware's registered routes without that caveat, and is wrong to.

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
| `dmaFree`, `dmaLargest`, `dmaMin` | the radio's pool (internal, DMA-capable): Wi-Fi takes its 1,626 B receive buffers here. `dmaMin` is the lowest since boot; 21.5 KB over 2 h 20 min on 2.5.6 |
| `stateInPsram` | bytes of page and effect state held in PSRAM (`PSRAM_ARRAY`, §7): about 36 KB on 2.5.6 |
| `allocFails`, `allocFailBytes`, `allocFailTask` | failed allocations - `wifi` means the radio went short |
| `lastCrash` | the last crash from flash. **Check `thisBoot` and `sameFirmware`** before blaming your change |
| `resetReason` | 1 power-on, 3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog |
| `linkRecoveries` | the Wi-Fi watchdog firing. Should be 0 |
| `brightnessNvs`, `brightnessLastSave` | the brightness NVS holds (0-255; -1 cannot open, -2 no key) and the last single-key save (-1 none since boot, 0 failed, else value + 1). The remote's brightness is saved 3 s after its last press |
| `loopMaxMs`, `loopSlowPart` | the longest `loop()` pass in the last 10 s, and which part |
| `netBroker` | the network broker: what is on the wire, how many served, its stack high-water |
| `climate` | the onboard sensor, raw and corrected. `idle: true` = not being read: nothing on screen shows it and Home Assistant is off |
| `presence` | the MTR-1 feed. `source: "idle"`, `subscribed: false` while no page reads it; `visits` counts the times a page turned it on |
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
ir ok 1200      held past the 1 s threshold: a long press
ir do home      any function by name (`ir help` lists them)
ir press 5      remote button 5, whatever it is set to
ir status       what the IR module thinks
```

The same over HTTP, since 2.5.4 (receiver on GPIO0, ten buttons with a
function each, src/ir/ir_map.h): `GET /api/ir` is the table and the functions
this build offers; `/api/ir/do?fn=<name>[&page=N][&hold=ms]` runs a function,
`/api/ir/press?btn=1..10`, `/api/ir/fn?btn=N&fn=<name>[&page=N]`,
`/api/ir/learn?btn=N`, `/api/ir/cancel`, `/api/ir/clear?btn=N|all`. Every one
answers with the table. `do` and `press` go down the same path a decoded frame
does, so they drive the knob's state machine and the actions for real.

---

## 6. Tools that already exist

**In this repository, under `tools/agent/` - these are the ones that work from
any machine against any panel:**

| tool | what it does |
|---|---|
| `discover.py` | every panel on the network, identified by MAC. `--json`, `--mac`. Exit 3 when several and none chosen |
| `bringup.py` | a new panel: asks for a name, sets it, then switches off the pages that have no source without Home Assistant. `--check` is read-only |
| `mcp_server.py` | eighteen MCP tools over stdio - driving, debugging and writing screens. `tools/agent/README.md` has the registration and the traps |
| `panel.py` | the transport everything shares: mDNS resolution, the 503 back-off absorbed, and read-back verification on every change |

**In the knowledge base repository**, under `tools/nsc/`:

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
constraint and flash is not the constraint. Internal SRAM is.** It is also the
radio's memory: the Wi-Fi driver takes its 1,626-byte receive buffers from the
internal DMA-capable heap (`dmaFree`, `dmaMin` in /api/info), and when one cannot
be had the panel stops answering until the link watchdog restarts Wi-Fi.

A 128x64 HUB75 panel holds its DMA framebuffer in internal RAM: 32 row-pairs x
128 pixels x 8 bits of colour depth x 2 bytes, double-buffered - **131,072 bytes,
from boot, for ever**. It stays there, and so do double buffering and colour
depth. Moving the frames to PSRAM has been tried (2026-09-14): it frees the 130 KB
and breaks both the picture and TLS certificate verification. A single DMA frame
has been tried (2026-09-23, branch feat/frame-in-psram): it frees 64 KB, but a
frame copied while the panel scans it tore 1-2 % of frames, measured on the
panel. Double buffering never tears.

**What sits in internal RAM, 2.5.6, Waveshare build.** The frames (128 KB), the
ESP-IDF, Wi-Fi and lwIP statics, our own statics (`.dram0.bss` + `.dram0.data`
= 81,840 B in total, see `tools/ram_budget.json`), every task stack, and the heap.
Up to 2.5.5 our code also kept about 36 KB of page and effect state there (the
world clock's colour map, the custom animation's frames, the clock games, the
star fields, the flight, rail and yacht boards). The pool had 13-15 KB free and
fell to 172 B in ordinary running. 2.5.6 moved that state to PSRAM. Over a
2 h 20 min run the pool had 30-50 KB free and 21.5 KB at the lowest, with no
failed allocation.

**Where the pool goes at run time.** Every task stack is internal: FreeRTOS
asserts it for a static stack, and the prebuilt config has no external stacks.
That is 8-12 KB for each fetch task. Sockets and Wi-Fi buffers are internal too.
mbedTLS's own buffers are in PSRAM already (`src/network/tls_psram.cpp`). One-off
fetches take turns (`src/network/net_lock.h`, `src/net/net_broker.h`), so adding
one does not raise the peak. A long-lived connection does: the yacht radar's AIS
stream holds 16-17.5 KB for as long as its page is on screen (17.5 KB measured
2026-09-14, the network table above and the knowledge base's docs/32-net-broker.md;
16.3 KB on 2.5.6, 2026-09-23).

**The rules** (`tools/ram_budget.py` enforces the first one at every commit that
touches src/, and in release.py):

1. **New page or effect state goes to PSRAM.** Declare it with `PSRAM_ARRAY()` or
   `PSRAM_OBJECT()` from `src/util/psram_state.h`, or `heap_caps_malloc(n,
   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` at boot. Only what DMA, an interrupt or
   a task stack touches has to be internal. Such an object goes on the budget's
   list with its reason (`tools/ram_budget.py --allow ... --reason ...`), and a
   reasoned rise of the total goes through `--update --reason`. The check fails
   on a new internal object of 256 B or more in the object files of src/ (lib/
   is covered only by the total), or a total more than 1,024 B over the budget. Both numbers are our own choice, not a standard: 256 B was the
   cut-off of the 2026-09-23 inventory, and the slack lets small variables pass
   while arrays are caught.
2. **Network work takes turns.** A new fetch goes through the lock or the broker.
   A new long-lived connection is measured before it is kept: it spends the
   margin directly.
3. **Measure against the baseline, in the same conditions.** Run
   `tools/agent/health.py` (normal pace, and `--stress`) and log `dmaMin` over a
   long run. Compare with 21.5 KB, the lowest since boot on 2.5.6 over
   2 h 20 min. `largestHeapBlock` varies between boots, in 1,024-byte steps: a
   single reading measures the boot, not the change, so compare distributions
   (`tools/nsc/measure/paired.py`, in the knowledge-base repository
   LED-MATRIX APOLLO, not this one).
4. **The sdkconfig that matters is the one for the board's memory type.** It is
   `tools/sdk/esp32s3/<type>/include/sdkconfig.h` in the framework, not the
   top-level file. CONFIG_SPIRAM_BOOT_INIT, for one, is on for opi_opi and
   qio_opi and off for qio_qspi.

Two more facts, both counter-intuitive enough to be worth stating:

**`largestHeapBlock` does not decay over time.** Sampled repeatedly within one
boot it does not move by a single byte. It varies *between* boots, in 1,024-byte
steps, with roughly one boot in seven landing 6 KB below the usual band.

**Anything that needs a large contiguous block at an unpredictable moment is the
enemy.** Four modules used to create 8-13 KB fetch tasks on demand; the panel
died when a browser opened the portal at the wrong moment. They now share one
permanent task whose stack is in `.bss` (`src/net/net_broker.h`), so nothing asks
for contiguous memory at a moment nobody chose. Prefer allocating at boot over
allocating on demand.

The build enforces stack frames: `-fstack-usage` and `-Wstack-usage=2048` on our
sources. Do not raise a task's stack to fit a work buffer - move the buffer.

---

## 8. Adding a screen

Two routes. **Take the Lua one unless you need C++** - it is a single file, it
previews on your laptop, and the whole loop up to the flash needs no hardware.

### a. A Lua effect

One effect is one persistent `lua_State` on its own task, drawing into a
128x64 RGB888 canvas in PSRAM that the render task blits to the panel.

**The contract** (`src/lua/lua_fx.cpp:120-143`):

| | required? |
|---|---|
| global `function draw()` | **yes** - absent and the load fails with "the script defines no draw()" |
| global `PERIOD`, a number > 0 | optional, default 60.0. **Read once, at load** - changing it inside `draw()` does nothing |
| global `FPS` | optional, default 20, clamped to 1..30 |

The chunk body runs once at load; build your tables and precomputed grids there.
`draw()` is then called once a frame with no arguments.

**What will run on the panel: the requirements, with the panel's own numbers.**
The simulator draws exactly what the panel draws, byte for byte (fx_parity).
It says nothing about how long a frame takes: the panel's core is shared with
Wi-Fi and is tens of times slower than a laptop. A script that is instant in
luasim can be refused by the panel. The hard limits (`kLuaFxPanelLimits`,
`src/lua/lua_fx.h`):

| limit | value | what happens past it |
|---|---|---|
| a frame, `draw()` | **500 ms** and 2,000,000 instructions | the frame is dropped; 3 in a row stop the effect |
| the load, the chunk body | 3,000 ms and 20,000,000 instructions | it does not open |
| Lua heap | 4 MB | error |
| source | the filesystem's room (`maxBytes`, at most 512 KB), nesting depth 16 | refused at upload |

**The upload is the test.** Since 2.5.9 the panel runs every upload once, off
screen, before it keeps it: the load and 4 frames. More than 1 of the 4 frames
over 500 ms, or any error, refuses the upload with the frame times, and nothing
is stored. An accepted upload's answer carries `trial` with the measured load
and frame times. **Read it.** 500 ms is where the panel gives up, not a target:
at 18-20 fps a frame has 50-55 ms before the effect starts to stutter.

**What things cost on this panel**, measured on 2026-09-23 through that trial
(firmware 2.5.9, Wi-Fi on; each is 4 frames of one loop, and every figure
includes the Lua loop around the call):

| operation | per call | a full 128x64 pass |
|---|---|---|
| an empty Lua `for` step | 0.35 us | |
| a step with a little arithmetic | 1.6 us | |
| `px.pixel` | 6.5 us | 53 ms |
| `px.get` | 5.6 us | 46 ms |
| `px.blend` | 13.5 us | **110 ms** |
| `px.rect` filled, whole screen | 1.7 ms | |
| `px.clear` | 0.16 ms | |
| `px.circle` filled, r 20 | 0.35 ms | |
| `px.line`, 128 px | 0.04 ms | |
| `px.text`, 5 letters | 0.02 ms | |
| **`px.glow`, r 10** | **5.6 ms** | 100 of them: over 500 ms |
| `px.save` / `px.restore` (2.6.6) | a 24 KB copy, a fraction of a ms | a still scene drawn once and restored each frame: the golf's frames went from ~75 k to ~30 k instructions |
| `px.grab` / `px.blit` (2.7.1) | a sprite stamped in one call, mirrored and dimmed in C | a pose drawn once, cut out and stamped: the aquarium's fish went from about forty calls each to one |
| `px.terrain` (2.6.3, native) | not measured on the panel yet | a whole 3D view in one call; each ground sample is charged to the frame's budget as one instruction |

What follows from that:
- Never do a per-pixel pass with `blend` or `glow` every frame.
- Paint what does not move once, at load or on the first frame. The canvas
  keeps it.
- Redraw only what changes.
- Count your glows.
- Aim for **under 50 ms** a frame.

The two screens refused on 2026-09-23 each spent more than 500 ms a frame on
these calls.

**The canvas is not cleared between frames.** It is zeroed once when the effect
opens and never again (`src/lua/lua_effects.cpp:164`). Call `px.clear()`
yourself for a clean frame - or leave it out and get trails for nothing.

**There is no clock in milliseconds.** No `sys`, no `os.time`, no `os.clock`;
`io`, `os`, `debug` and `package` are not compiled in, and `coroutine` is
compiled but deliberately not opened, because a new thread starts with a fresh
hook count and would escape the instruction budget
(`src/lua/nslua_sandbox.cpp:80-84`). Time comes from `px.t()` and `px.now()`.

`px.t()` is the animation phase in `[0,1)`, aligned to the epoch - so at
`PERIOD=60` it is the second hand, and a clock effect lands its change exactly
on the minute.

**The budgets are real and they are enforced** (`src/lua/lua_fx.h:39-45`):

| | |
|---|---|
| load instructions | 20,000,000 |
| draw instructions | 2,000,000 |
| load deadline | 3,000 ms |
| draw deadline | 500 ms |
| Lua heap | 4 MiB, from PSRAM |
| task stack | 12 KiB **internal** RAM (`lua_effects.cpp:63`) |

A `draw()` over the *time* budget is dropped rather than fatal, up to three in a
row; the fourth stops the effect. Anything else - syntax, runtime, instruction
budget, heap - stops it at once.

**The whole API in one call:** the MCP server's `effect_api` tool returns the
`px` table, the `presence` table, the environment and every budget as one
object. Otherwise `src/lua/lua_px.cpp:300-305` is the list, and
`tools/luasim/scripts/demo.lua` exercises every call and is the intended
template.

**The loop, which needs hardware only at the end:**

```bash
$EDITOR tools/luasim/scripts/my_effect.lua     # [A-Za-z0-9_] only in the name
cd tools/luasim && make
./luasim scripts/my_effect.lua 40 out.raw --start 12:34
python3 render.py out.raw out.gif 6            # look at it
python3 fx_parity.py                           # the REAL budgets, on your Mac
cd ../.. && python3 tools/luasim/gen_effects.py
#   then a person builds and flashes
```

`luasim` compiles the *same vendored Lua 5.4.8* against a host copy of `px`, so
what it draws is what the panel draws - but it does **not** enforce the budgets,
ignores `PERIOD` and `FPS`, and has no `presence` table. `fx_parity.py` is the
one that does: it builds the firmware's own `lua_fx.cpp`, `lua_px.cpp` and
sandbox for the host, applies the panel's real limits, compares frames byte for
byte against luasim at four clocks, and prints each script's instruction counts,
heap peak and C stack used. Run it before you believe a script is finished.

The file name is not cosmetic: `gen_effects.py:59-60` refuses anything outside
`[A-Za-z0-9_]`, and the on-panel name is the stem with underscores as spaces,
upper-cased - `football_clock.lua` becomes `FOOTBALL CLOCK`. The generator also
runs in the pre-commit hook with `--check`, so a stale header blocks a commit.

**The compiled-in route is empty since 2.6.0** (`gen_effects.py` finds every
script marked `@upload-only` and emits LUA_EFFECT_COUNT 0). Scripts arrive over
`/api/lua/upload` and are validated and tried before they are kept.

### b. A C++ page

Not a class, not a registry, not a vtable: a module directory plus six edits in
`src/main.cpp`. The world clock is the smallest complete example - 85 lines of
header, 465 of drawing - and copying it is the intended way in.

**The only mandatory function** is `void <module>Render();` - draw one frame,
now. The caller has already cleared the screen and will flip the buffer.

**Its six touch points in `main.cpp`**, all guarded by the module's build flag:

| | |
|---|---|
| `:99-101` | the page enum entry, `PAGE_WORLDCLOCK` |
| `:163` | the include |
| `:186-188` | `panelPageSeconds()` - the carousel slot, 20 s here |
| `:270-273` | `getOptimalRefreshRate()` - the page's own frame rate, 10 Hz here |
| `:684-686` | `ctrlPageName()` - the banner and toast name |
| `:734-736` | `panelPageKey()` - maps the page to `PANEL_KEY_WORLD` |
| `:1350-1353` | the render dispatch |

And outside it: a `PanelPageKey` in `src/panel/panel.h:40-54`, the `route()`
line and handler in `src/web/web_panel.cpp`, a word added to
`panelWebFeatures()`, and the flag in `platformio.ini`.

**`PanelPageKey` is append-only.** The switches are bits in the NVS `pages`
value, so a key inserted in the middle moves every bit after it and silently
rearranges what the owner had switched on. The enum says so at
`src/panel/panel.h:45-47`; believe it. Skipping the key entirely gives
`PANEL_KEY_NONE`, which means "always visited, cannot be switched off".

**Knob handling is optional and three-layered:** `ctrlPageHasControls()` makes a
click *enter* the page, `ctrlEnterHint()` is the toast text ("TURN: AIRPORT"),
and the knob block in `loop()` (`main.cpp:1032-1070`) dispatches the click and
the rotation. The yacht radar is the cleanest example of a page with controls
and a lifecycle - `yachtRadarBegin()` / `yachtRadarStop()` open and close a
network task as the page is entered and left.

**Drawing.** `extern MatrixDisplay display;` from `src/display/display.h`, which
is an `Adafruit_GFX` with panel-native RGB888 overloads beside the RGB565 ones:
`drawPixelRGB888`, `fillScreenRGB888`, `drawFastHLine/VLine`, `fillRect`. What
the codebase actually leans on, by count: `fillRect` 199, `drawPixel` 190,
`setCursor` 132, `print` 111, `setTextSize` 68, `color565` 66.

Three things to know before your first frame:

- **`display.getBuffer()` always returns `nullptr`** (`matrix_display.h:84`).
  There is no readable framebuffer; you cannot read a pixel back. That is
  exactly why the Lua path keeps its own canvas in PSRAM.
- **`setTextSize` and `setFont` are sticky global state.** The clock styles
  leave the size at 3 or 4. Any page that draws text must set every text
  attribute it depends on and restore `setFont(NULL)` on the way out -
  `lua_effects.cpp:275` and `:293` do exactly this, with the comment saying why.
- **The system font: Latin and Cyrillic, capitals and lowercase, in every
  screen** (since 2026-09-23, `src/fonts/sys_text.h`, `sys_print.h`). Two fonts:
  the classic 5x7 (no font set) and `PicopixelFB` (`src/fonts/picopixel_fb.h`,
  stock Adafruit Picopixel with the `U` given a flat bottom, because at 2 mm
  pitch the stock `U` reads as `V` across a room). **`display.print()` is
  UTF-8**: write Russian as it is, in any page, the authors' clock styles
  included. `display.getTextBounds()` and `display.textWidth()` count letters,
  not bytes - never `strlen() * 6`. Cut text with `utf8DropLast()` /
  `utf8TrimPartial()`, never a byte at a time. Unknown code points draw a solid
  block, not a space; a byte that starts no UTF-8 sequence draws as the CP437
  byte it always was. The glyphs: Cyrillic 5x7 from X11 misc-fixed 6x10,
  Picopixel lowercase from X11 4x6 (public domain, `tools/fonts/x11/`), letters
  that look Latin are the Latin pixels; generators `tools/fonts/mksysfont.py`
  and `mkcyr.py`; host test `tools/fonts/check_sysfont.py` runs the panel's
  print() over the real Adafruit GFX. Place names (world clock, airports) take
  capitals A-Z and А-Я (`src/fonts/name_chars.h`), two bytes a Cyrillic letter.

**Every module header `#error`s on a missing prerequisite** - the world clock
needs `CONTROL_ENCODER_ENABLED`, presence needs `MQTT_BUS_ENABLED` *and*
`LUA_EFFECTS_ENABLED`. Follow that pattern; a page that silently half-exists is
worse than one that refuses to compile.

### c. Before you write either one

Do the arithmetic. Internal RAM is the binding constraint on this board: the
HUB75 DMA framebuffer takes 131,072 bytes of it, which is why about 19-24 KB of
heap is all there is, and `largestHeapBlock` is usually under 14 KB. A page that
wants a contiguous buffer needs to be checked against that number before it is
written, not after it fails on the panel. Section 7 has the detail.

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
- **Ask for a panel's name; do not invent one.** It is what a person types for
  the rest of that panel's life. See "Bringing a new panel to life".
- **Your configuration lives in your fork, not in a pull request.** Names,
  stations, airports, keys, your own screens: your fork and your NVS. Issues and
  PRs for anything that helps the next person are welcome upstream.
- **Spend a script's bytes on TIME, not colour.** A bigger budget buys nothing
  as a richer palette - the panel resolves far less colour than a file can
  carry, and the arithmetic above says why. It buys duration and motion: more
  phases, more states, more things that move. A stored full-screen frame at 256
  colours is 16 KB, so even 120 KB is seven frames and useless as animation; but
  `aquarium.lua` is 24 KB of *code* and animates for ever at 15 fps. Shape and
  movement read at 2 mm pitch. Extra colours do not. `effect_api` carries this
  rule with its derivation.
- **Do not trust this file over the source.** It has been wrong before - the
  fx3d entry in section 2 was wrong until 2026-09-21. Where it and the code
  disagree, the code is right and this file needs fixing - please fix it.

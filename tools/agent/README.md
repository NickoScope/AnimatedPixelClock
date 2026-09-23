# An SDK for this panel, and an MCP server that wraps it

Seven files, and between them everything an AI agent needs to bring a panel up,
find it on your network, drive it, see what is actually happening inside it, and
write new screens for it.

| file | what it is |
|---|---|
| `discover.py` | finds every panel on the network by MAC. Standalone CLI, and the thing everything else resolves addresses through |
| `bringup.py` | a new panel: asks for a name, sets it, switches off the pages that have no source without Home Assistant. It will not make that call over a broker that is merely down |
| `panel.py` | the transport. One place that knows how this firmware really behaves |
| `gallery.py` | the `gallery/` screens, and any of them onto a running panel in about a second |
| `health.py` | a self-test: pings underneath, exercises the controls and the portal, reads the panel's counters and log ring (and the USB console if given), writes every raw log to `health-logs/<time>/` and prints a short verdict with findings. `--list`, `--panel`, `--all` choose among several panels. Also the `panel_selftest` MCP tool |
| `update.py` | firmware updates: what is new (every published release newer than the panel, with its notes), and installing one over the air with three confirmations and an honest outcome. Also the `panel_update_check` and `panel_update` MCP tools |
| `mcp_server.py` | twenty-seven MCP tools over stdio. This is what you register with Claude Code, Codex or anything else that speaks MCP |

**This SDK is the source of truth for working with a panel.** Where a fact about
driving one has to live in exactly one place, it lives here - as a tool that
returns it - and `AGENTS.md` points at it rather than keeping its own copy that
drifts. `panel_bringup`, `effect_api` and `effect_photo` are written that way on
purpose: they are documents you can call.

Firmware changes one way only: `update.py` / `panel_update`, which installs a
**published** release of this fork over the air after **three confirmations
from the person** in front of the panel, checks the image (SHA-256 against the
release, ESP32-S3 header) before sending it, and reports what the panel says
afterwards - UPDATED, ROLLED BACK (the panel's own rollback: an image that does
not prove itself in its first minute is replaced by the previous one), NOT
APPLIED, PENDING or NOT BACK. Nothing here builds and flashes an image of its
own: building and uploading a build is a person's call, at a moment they chose.

---

## Registering it

### The one thing that will waste your afternoon on macOS

**Register it with a Python that has Local Network access, not with `uv run`.**

macOS grants Local Network access per binary. A Python that an MCP client
launches - `uv`'s downloaded interpreter is the usual one - can be denied it
while exactly the same code run from your terminal works fine. The denial does
not look like a denial: connecting to a `192.168.x.x` address returns
**`[Errno 65] No route to host` instantly**, which reads precisely like a panel
that is switched off.

Measured on the development Mac on 2026-09-21: under `uv run python`,
`example.com` answered in 0.54 s and the panel's own IP gave Errno 65 in 0.00 s.
The same two calls from the system Python both succeeded. mDNS discovery still
works under `uv` - it shells out to `dns-sd` - so the panel is *found* and then
appears dead, which is the confusing part.

`panel.py` detects this and says so instead of reporting "no panel found", but
the fix is to register it properly:

```bash
cd tools/agent
python3.12 -m venv .venv                    # any Python >= 3.10 that reaches your LAN
./.venv/bin/pip install 'mcp>=1.2.0,<2' 'pydantic>=2.6'
./.venv/bin/python -c "import sys;sys.path.insert(0,'.');import panel;print(panel.resolve())"
```

That last line must print your panel. If it prints the Local Network message
instead, that interpreter is the problem, not the panel - try another one, or
grant it access in System Settings → Privacy & Security → Local Network.

Then:

```bash
claude mcp add ledmatrix --scope user -- \
  /abs/path/tools/agent/.venv/bin/python /abs/path/tools/agent/mcp_server.py
```

Absolute paths on both, because the client does not launch from an interactive
shell. For Codex or any other MCP client, the same two strings are the command
and its single argument.

On Linux none of this applies; `uv run tools/agent/mcp_server.py` works, and the
script carries its own PEP 723 dependency header.

### Choosing a panel

Every tool takes an optional `panel`: a MAC, a device name or an address. With
one panel on the network you can leave it out. With several, leaving it out
returns the list and asks which - it never guesses. To stop being asked:

```bash
claude mcp add ledmatrix --scope user --env LEDMATRIX_PANEL=AA:BB:CC:00:11:22 -- ...
```

---

## The tools

**Bringing one up**
- `panel_bringup` - a bare board to a panel on the network: which environment
  belongs to which module, the memory-type trap that kills a Waveshare board on
  every boot, and the two steps that are the person's, not yours. **There is no
  flashing tool here and there will not be one.**

**Finding**
- `panel_list` - every panel on this network, by MAC

**Seeing what is happening**
- `panel_status` - the live page, the style, the carousel, every page and whether it is on
- `panel_health` - memory, network, faults, and a plain reading of what the numbers mean here
- `panel_log` - the firmware's own log. **The only place a Lua error's text appears**
- `panel_capabilities` - which modules this build has, which routes answer, and whether Home Assistant is behind it

**Driving**
- `panel_carousel` - start or stop the page rotation, and set how long it dwells.
  **Stop it before showing somebody one screen**, or it walks off mid-sentence
- `panel_show_page` · `panel_set_style` · `panel_enable_page`
- `panel_effects` · `panel_show_effect`
- `panel_notify` · `panel_display` · `panel_rename`

**Writing a screen**
- `effect_api` - the whole Lua drawing API and every budget, in one object. Read it before writing a script
- `effect_write` - put a script in `tools/luasim/scripts/`
- `effect_preview` - run it in the reference simulator and get a GIF. No hardware needed
- `effect_check` - run every script through the firmware's real runtime and budgets, on your Mac

**Putting one on a panel, with no flash at all**
- `effect_upload` - a script onto a running panel over the air, into one of
  twelve slots, up to 50 KB each. `from_gallery=true` takes it straight out of
  `gallery/`
- `gallery_list` - what is in `gallery/`, with sizes and previews
- `effect_delete` - free a slot
- `effect_walk` - one effect in or out of the knob's walk and the carousel
  (2.5.7+; kept on the panel by name, and the name goes along with the index,
  so a list renumbered in between is refused rather than the neighbour
  switched). `effect` is an index or a name. `panel_effects` shows `inWalk`

**Publishing to the gallery** (what every panel's portal lists under "Add from
the gallery", once it is on GitHub)
- `gallery_publish` - a finished script from `tools/luasim/scripts/` into
  `gallery/`: the panel's own checks, 300 frames in the simulator (an error or
  an all-black screen is refused), a preview, a README section from `about`,
  the index, one commit touching only `gallery/`
- `gallery_unpublish` - take one of **your own** entries out again
- `gallery_scoreboard` - replace `gallery/SCREEN_OF_THE_DAY.md`, the daily
  screen and the owner's thumbs: Markdown, no HTML, pictures only gallery
  previews

  Entries are marked with who published them (`-- @by <name>`, from
  `LEDMATRIX_PUBLISHER` where the server starts); a publisher can replace or
  remove only its own, never a person's. A photograph goes in only with
  `photo_no_people` (no person in it: a landscape, a tree, the sea; the owner's
  rule, 2026-09-23) and is tagged `-- @photo no-people`; a photograph of a
  person never goes in, and the maintainer looks at every preview. The CLI is `tools/agent/gallery.py
  publish|unpublish|scoreboard` (`--dry-run`; `--any` is a person's override).

  **Where it goes.** The remote is `$LEDMATRIX_GALLERY_REMOTE` or `git config
  gallery.remote`, the branch `$LEDMATRIX_GALLERY_BRANCH` or `git config
  gallery.branch`. The agent's machine has **no key for GitHub**, by the
  owner's decision (2026-09-23): there it is its own clone (`.`) and the
  branch `gallery-staging`. The maintainer carries it to GitHub from a machine
  that can push:

  ```
  python3 tools/agent/gallery.py sync pi@nickol.local:ledmatrix-mcp --by openclaw
  ```

  `sync` mirrors the agent's entries **by state**, not by replaying its
  commits: each one goes through every check again, its preview is made again
  from the script, a person's entry changed in staging is not carried, and the
  staging branch then starts again from what GitHub has. Everything works in a
  throwaway worktree; the local checkout is never touched. Needs a C compiler,
  `make` and Pillow in the venv (`pip install pillow`).

**A photograph**
- `effect_photo` - a real photograph on the panel, one call: crop, enhance,
  quantise to 256 colours with Floyd-Steinberg, two base64 characters a pixel,
  upload, show. Not ASCII art - the picture itself, painted once because the
  canvas is never cleared between frames. The rule and every knob are in the
  tool's own description; `tools/luasim/photo_to_lua.py` is the same thing by
  hand. **A photograph of a person does not go in the repository** - keep those
  in `tools/luasim/scripts/private/`, which is gitignored

Every mutating tool reads the state back and compares. It reports `changed` and
`already` as different outcomes, because an "ok" that only means "nothing needed
doing" is not a verification.

---

## Testing the branch you cannot reach

`tools/agent/tests/` holds a panel's own captured answers and a stub that
serves them, so the branches a live panel cannot show you still run. It is how
the no-Home-Assistant path in `bringup.py` was first executed at all - the panel
here has a working broker, so that path had never run on anything.

```bash
python3 tools/agent/tests/test_bringup_no_ha.py
```

Five cases: no broker, a broker configured but down, direct API keys instead of
a broker, a working broker, `--keep-ha`. The pre-commit hook runs them whenever
`tools/agent/` changes. The fixtures are real answers with every MAC, address,
topic, station and now-playing scrubbed - this repository is public.

Two defects it caught the first time it ran, both worth knowing about when you
write the next tool against `/api/panel`:

- **Several pages share one key.** Four of them are `market`. The firmware
  stores one enable bit per KEY, not per page, so `{"enable":{"key":"market"}}`
  takes all four. A dict keyed by page key keeps the last one read, and then
  your report names one page while four go dark.
- **`connected: false` is not "no Home Assistant".** A broker restarting looks
  exactly like a broker that was never there, for about ten seconds.

---

## Text on the panel: the system font (2.5.6)

Everything the panel draws as text is UTF-8: Latin and Cyrillic, capitals and
lowercase, in both of its fonts (the classic 5x7 and the small Picopixel).
Banners (`panel_notify`), cards, media titles, the boards, the author's clock
screens all draw Russian as it is. A Lua effect picks the font with a last
argument: `px.text(x, y, s, r, g, b, "5x7")` or `"pico"`; without it px.text
draws as it always did (small, lowercase as capitals). Widths count letters,
not bytes: a Cyrillic letter is two bytes, which is what the byte limits count
(a banner 200, a world-clock city 20, an airport 12). The design and the tests:
AGENTS.md section 8 and `tools/fonts/check_sysfont.py`.

## What `/api/info` says since 2.5.6

- `dmaFree`, `dmaMin`: the radio's pool. `dmaMin` under 1,626 B means Wi-Fi went
  short of a receive buffer at some point since boot.
- `stateInPsram`: page and effect state kept out of internal RAM (about 36 KB).
- `presence.source: "idle"`, `presence.subscribed: false`, `climate.idle: true`:
  the room radar feed and the onboard sensor run only while a screen needs them.
  Idle is normal, not a fault. `health.py` checks that they follow the screen.
- `health.py` also switches one effect out of the carousel and back (2.5.7),
  and with `--effects` does the portal's gallery round trip: an effect from the
  GitHub gallery that is not on the panel is uploaded, shown and deleted.

## Грабли

One entry per real debugging day. This section is the most valuable part of this
file.

### 1. A 200 from `/api/panel` proves nothing

The route has **no key whitelist**. An unrecognised key is never read, the
handler falls through to building the state, and you get
`200 {"success":true, …}` with the full state attached. `{"showPage":7}` and
`{"styleId":14}` have each cost this project an afternoon; both answer success
and do nothing. The real shapes are `{"show":{"page":N}}` and `{"style":N}`.

`/api/media`, `/api/market` and `/api/flightboard` *do* whitelist keys and
answer 400. `/api/panel`, `/api/railboard`, `/api/worldclock`, `/api/knob` and
`/api/clips` do not. **Always compare the state afterwards** - which is why
`panel.verify()` exists and why nothing in `mcp_server.py` trusts a status code.

### 2. 503 is the panel working

Every route in the panel group is wrapped in `webBusyRefuse()`. It refuses while
a background fetch holds the network, or while the largest free block of
internal RAM is under the back-off threshold. The refusal is `503`,
`Retry-After: 1`, **`Content-Type: text/plain` with an empty body** - so a
client that calls `.json()` on it throws. Retry and carry on; `panel.py` absorbs
it and never surfaces it.

`/api/info` and `/api/status` are deliberately never refused.

### 3. `largestHeapBlock`, not `freeHeap`

The HUB75 DMA framebuffer takes 131,072 bytes of internal RAM - 32 row-pairs ×
128 px × 8-bit depth × 2 bytes, double-buffered - which is why about 19-24 KB of
heap is all there is. What kills a request is the demand for a **contiguous**
block at an unchosen moment, not the total.

The number is also **constant within a boot** and varies between boots in
1,024-byte steps, with roughly one boot in seven landing 6 KB low. A single
reading compared against a single earlier reading is worth nothing. The measured
protocol is seven boots, one reading each (`tools/nsc/measure/paired.py` in the
knowledge base).

`allocFails` climbing with `allocFailTask: "wifi"` under web load is expected
and is not a fault. `linkRecoveries` climbing is.

### 4. macOS Local Network permission, above

Errno 65 in 0.00 s to a LAN address while the internet works. It is a
permission, not a network.

### 5. Starting a Lua effect is not synchronous, and its error is not in HTTP

`POST /api/lua {"show":i}` only sets a request. The main loop picks it up, the
Lua task loads the script, and a load is allowed up to 3 seconds. In between,
the reported index is already the new one while the screen is still black. Poll
`now.hz` - above the 2 Hz floor means it is really drawing.

When it fails, the panel draws `LUA ERROR` with a wrapped message and the full
traceback goes to serial and to the ring log. **No HTTP route returns that
text.** Use `panel_log`. That is an SDK gap in the firmware worth closing.

### 6. `/save` is destructive by omission

The portal posts about 125 form fields, and every boolean is read as
`server.hasArg("x")` - **present means true, absent means false**. A partial
`/save` therefore clears every boolean it does not carry: `showClock`,
`notifyEnabled`, `weatherEnabled`, `micAgc`, and the rest.

Use `POST /api/import` for a patch - it tests every key for null and is a true
patch - and `POST /api/rename` for the name. Renaming through the portal was
tried here on 2026-09-21 and the name did not take; `/api/rename` did it in one
call, saved to NVS and restarted mDNS with no reboot.

### 7. Clock style ids are not a range

Fifteen styles with ids up to 16, and **4 and 13 do not exist**. Read `styles[]`
from `/api/panel`.

Two routes set the style and they disagree: `POST /api/panel {"style":N}`
validates, tells the page machinery and persists; `GET /api/clock/style?id=N`
accepts ids that do not exist, writes the setting without saving it, and does
not tell the page machinery. Use the first.

### 8. Nothing the "runtime control" routes do is persisted

`/api/display/on|off|brightness`, `/api/mode/*`, `/api/clock/style`,
`/api/anim/play`, `/api/clips {"play":…}` change RAM only. A reboot restores
what is saved. That is deliberate, and it makes them safe.

`brightness` takes a **percent, 0-100**, not 0-255, and silently clamps rather
than refusing.

### 9. A route whose module is not built answers 404, not 501

Features are compile-time flags, so two panels from the same repository can have
different capabilities. `/api/fx3d` and `/fx3d` in particular are **404 on a
shipping panel** - `FX3D_ENABLED` is set only in
`env:matrix-waveshare-rgb-fx3dbench`. Ask `panel_capabilities` before assuming.

### 10. Several pages have no source but MQTT

Cards, media and the four market pages arrive over MQTT and nowhere else. On a
panel with no broker they are not "empty for now" - nothing will ever arrive,
and the carousel still dwells fifteen seconds on each. Flights and trains work
either through Home Assistant or through a direct API key of their own; yachts
goes straight to `aisstream.io` and never used Home Assistant at all.

`bringup.py` probes this and switches off only what has no source.

### 11. Measuring through the mDNS name costs seconds

Fine for a tool call. Not fine for a benchmark - resolve once and use the IP.

### 12. `subprocess` returns bytes on `TimeoutExpired` even with `text=True`

The decoding never happens because the call did not return normally. Both mDNS
browsers are always cut off by a timeout, because browsing does not end on its
own - so this bit on the very first run of `discover.py`.

---

## Writing an effect without hardware

The loop, which needs a panel only at the very end:

```
effect_api      → the whole drawing API and every budget
effect_write    → tools/luasim/scripts/<name>.lua
effect_preview  → luasim + render.py → a GIF you can look at
effect_check    → fx_parity.py: the firmware's real runtime and real budgets
effect_install  → regenerates src/lua/lua_effects_scripts.h
                  ↓
              a person builds and flashes
```

`luasim` compiles the *same vendored Lua 5.4.8* against a host copy of the `px`
API, so what it draws is what the panel draws. `fx_parity.py` goes further: it
compiles the firmware's own `lua_fx.cpp`, `lua_px.cpp` and sandbox for the host,
applies the panel's real instruction and time budgets, and compares frames byte
for byte against luasim at four different clocks.

The two things that most often surprise a first script: **the canvas is not
cleared between frames** (call `px.clear()` yourself, or enjoy the trails), and
**there is no clock in milliseconds** - no `sys`, no `os.time`, no `os.clock`.
Time comes only from `px.t()` and `px.now()`.

---

## Exit codes, for the CLI tools

`0` done · `1` retryable · `2` bad arguments · **`3` a person must decide** ·
`4` retrying is pointless. The third is the useful one for an autonomous agent:
several panels and none chosen, or a new panel that needs a name.

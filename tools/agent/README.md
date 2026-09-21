# An SDK for this panel, and an MCP server that wraps it

Four files, and between them everything an AI agent needs to find a panel on
your network, drive it, see what is actually happening inside it, and write new
screens for it.

| file | what it is |
|---|---|
| `discover.py` | finds every panel on the network by MAC. Standalone CLI, and the thing everything else resolves addresses through |
| `bringup.py` | a new panel: asks for a name, sets it, switches off the pages that have no source without Home Assistant |
| `panel.py` | the transport. One place that knows how this firmware really behaves |
| `mcp_server.py` | eighteen MCP tools over stdio. This is what you register with Claude Code, Codex or anything else that speaks MCP |

Nothing here has a flashing tool, and nothing here will get one. Building and
uploading is a person's call, at a moment they chose, with the panel in front of
them.

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
claude mcp add ledmatrix --scope user --env LEDMATRIX_PANEL=90:E5:B1:D2:0E:C8 -- ...
```

---

## The tools

**Finding**
- `panel_list` - every panel on this network, by MAC

**Seeing what is happening**
- `panel_status` - the live page, the style, the carousel, every page and whether it is on
- `panel_health` - memory, network, faults, and a plain reading of what the numbers mean here
- `panel_log` - the firmware's own log. **The only place a Lua error's text appears**
- `panel_capabilities` - which modules this build has, which routes answer, and whether Home Assistant is behind it

**Driving**
- `panel_show_page` · `panel_set_style` · `panel_enable_page`
- `panel_effects` · `panel_show_effect`
- `panel_notify` · `panel_display` · `panel_rename`

**Writing a screen**
- `effect_api` - the whole Lua drawing API and every budget, in one object. Read it before writing a script
- `effect_write` - put a script in `tools/luasim/scripts/`
- `effect_preview` - run it in the reference simulator and get a GIF. No hardware needed
- `effect_check` - run every script through the firmware's real runtime and budgets, on your Mac
- `effect_install` - regenerate the effect table. Then a **person** builds and flashes

Every mutating tool reads the state back and compares. It reports `changed` and
`already` as different outcomes, because an "ok" that only means "nothing needed
doing" is not a verification.

---

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

#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mcp>=1.2.0,<2", "pydantic>=2.6"]
# ///
"""An MCP server for a 128x64 HUB75 LED panel on an ESP32-S3.

It exists so that a model arriving with no knowledge of this firmware can drive
a panel, see what is actually happening on it, and write new screens for it -
from any machine, against any panel on the network, and with a choice when there
is more than one.

Three things shape every tool here, and they are all consequences of how the
firmware really behaves rather than preferences:

**Nothing is addressed by IP.** Every tool takes an optional `panel`, which may
be a MAC, a device name or an address. Given nothing it uses the only panel on
the network, and when there are several it says so and lists them rather than
guessing. Set `LEDMATRIX_PANEL` to a MAC to stop being asked.

**Every change is read back.** `/api/panel` has no key whitelist: a misspelled
key is ignored and the route still answers `200 {"success":true}`. So a tool
that says it changed something has compared the state afterwards, and it
distinguishes "changed" from "was already so".

**There is no flashing tool, and there will not be.** Building and uploading is
a person's call, with the panel in front of them. The effect tools go as far as
writing the script, previewing it, measuring it against the firmware's real
budgets and generating the header - and then stop and say what a person must do.

Run it directly (`./mcp_server.py`) or register it:

    claude mcp add ledmatrix --scope user -- \\
      /Users/you/.local/bin/uv run --quiet /abs/path/tools/agent/mcp_server.py
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Annotated, Literal

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, ConfigDict, Field

sys.path.insert(0, str(Path(__file__).resolve().parent))
import panel as P  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
LUASIM = REPO / "tools" / "luasim"
SCRIPTS = LUASIM / "scripts"

mcp = FastMCP("ledmatrix")

DEFAULT_PANEL = os.environ.get("LEDMATRIX_PANEL") or None

PanelArg = Annotated[str | None, Field(
    default=None,
    description="Which panel: a MAC (90:E5:B1:D2:0E:C8), a device name "
                "(NickoSha-64x128), or an address. Omit when there is only one "
                "on the network; with several, omitting it returns the list to "
                "choose from rather than guessing.")]


def _ok(**kw):
    return json.dumps({"ok": True, **kw}, ensure_ascii=False, indent=2)


def _say(e: Exception) -> str:
    """A fault becomes a sentence. A traceback never leaves this server."""
    if isinstance(e, P.PanelError):
        return str(e)
    return f"Unexpected failure {type(e).__name__}: {e}"


def _pick(panel):
    return P.resolve(panel or DEFAULT_PANEL)


# =========================================================================
#  Finding a panel
# =========================================================================

@mcp.tool(
    name="panel_list",
    annotations={"title": "List the panels on this network", "readOnlyHint": True,
                 "openWorldHint": True})
async def panel_list(refresh: bool = False) -> str:
    """Every LED panel on this network, identified by MAC.

    Discovery is by mDNS: the firmware advertises `_http._tcp` on port 80 with a
    TXT record carrying `model`, `version` and the chip's `mac`. Each
    advertisement is then confirmed by asking the panel itself, because a stale
    record for a panel that has gone is worse than no answer.

    Call this first when a machine has never talked to these panels, and
    whenever a tool reports that a choice is needed.

    Args:
        refresh: browse again instead of using the ~30 s cache.

    Returns:
        {"ok": true, "panels": [{"mac", "address", "name", "version",
        "reachable", "uptime"}], "count": N, "default": "<LEDMATRIX_PANEL or null>"}

    Examples:
        "what panels are on the network" -> here.
        "which panel am I talking to" -> here.
        "what can the panel do" -> panel_capabilities.
    """
    try:
        panels = P.known(refresh=refresh)
        return _ok(panels=panels, count=len(panels), default=DEFAULT_PANEL)
    except Exception as e:  # noqa: BLE001
        return _say(e)


# =========================================================================
#  Seeing what is happening
# =========================================================================

@mcp.tool(
    name="panel_status",
    annotations={"title": "What the panel is showing", "readOnlyHint": True,
                 "openWorldHint": True})
async def panel_status(panel: PanelArg = None) -> str:
    """What is on screen now, and what else could be.

    Reads `/api/panel`, which answers with the live page, the clock style, the
    carousel, every page and whether it is switched on, and the style table.

    **Page numbers are not fixed.** They depend on which modules the firmware was
    built with and on how many user cards exist, so read them here rather than
    remembering them. The same is true of clock style ids: they are not
    contiguous - 4 and 13 do not exist in this build.

    Returns:
        {"ok": true, "now": {"page", "name", "key", "style", "styleName", "hz",
        "mode", "off", "time", "lua"}, "carousel": {...},
        "pages": [{"i", "key", "name", "on"}], "styles": {"<id>": "<name>"},
        "panel": {"mac", "address", "name"}}
    """
    try:
        p = _pick(panel)
        d = P.get(p["address"], "/api/panel")
        return _ok(now=d.get("now"), carousel=d.get("carousel"),
                   pages=d.get("pages"), styles=P.styles_of(d),
                   panel={"mac": p.get("mac"), "address": p["address"],
                          "name": p.get("name")})
    except Exception as e:  # noqa: BLE001
        return _say(e)


@mcp.tool(
    name="panel_health",
    annotations={"title": "Memory, network and faults", "readOnlyHint": True,
                 "openWorldHint": True})
async def panel_health(panel: PanelArg = None) -> str:
    """The one call that answers most "why is it behaving oddly" questions.

    Reads `/api/info` and reports it with the interpretation this hardware needs,
    because the raw numbers mislead:

    **`largestHeapBlock` matters more than `freeInternalHeap`.** The HUB75 DMA
    framebuffer takes 131,072 bytes of internal RAM, leaving roughly 19-24 KB of
    heap, and what kills a request is the demand for a *contiguous* block, not
    the total. The number is also constant within a boot and varies between
    boots in 1,024-byte steps, so a single reading compared against a single
    earlier reading proves nothing.

    **`allocFails` climbing with `allocFailTask: "wifi"` is expected under load**
    and is not by itself a fault; it is the back-off doing its job.
    `linkRecoveries` climbing is a real symptom - that is the watchdog having to
    rescue the Wi-Fi link.

    Returns:
        {"ok": true, "memory": {...}, "network": {...}, "broker": {...},
        "faults": {...}, "reading": ["a sentence per thing worth saying"],
        "raw_keys": [...]}
    """
    try:
        p = _pick(panel)
        d = P.get(p["address"], "/api/info")
        mem = {k: d.get(k) for k in (
            "freeHeap", "freeInternalHeap", "largestHeapBlock", "minFreeHeap",
            "psramBytes", "psramFreeBytes")}
        net = {k: d.get(k) for k in (
            "rssi", "wifiStatus", "ip", "hostname", "httpServed", "secsSinceHttp",
            "secsSinceTraffic", "linkRecoveries", "lastLinkRecovery", "ntpSynced")}
        faults = {k: d.get(k) for k in (
            "allocFails", "allocFailBytes", "allocFailTask", "webRefused",
            "netReserveDrops", "wifiFailAgeS", "resetReason", "loopMaxMs",
            "loopSlowPart", "loopSlowPartMs")}

        reading = []
        lb = mem.get("largestHeapBlock") or 0
        if lb and lb < 12000:
            reading.append(
                f"largestHeapBlock is {lb} B. Below about 11,866 the panel starts "
                "refusing web requests with 503 when the broker is down; that is "
                "the back-off, not a fault. Expect portal assets to need a retry.")
        if (faults.get("allocFails") or 0) > 0:
            reading.append(
                f"allocFails is {faults['allocFails']} on task "
                f"{faults.get('allocFailTask')!r}. Under web load this is normal "
                "here and the panel stays up; it matters only alongside a rising "
                "linkRecoveries.")
        if (net.get("linkRecoveries") or 0) > 0:
            reading.append(
                f"linkRecoveries is {net['linkRecoveries']} - the Wi-Fi link has "
                "had to be rescued. This one is worth investigating.")
        if d.get("weatherValid") is False:
            reading.append("The weather fetch has no valid result yet.")
        if not reading:
            reading.append("Nothing here needs attention.")

        return _ok(firmware={"version": d.get("version"), "build": d.get("build"),
                             "chip": d.get("chip"), "uptime": d.get("uptime")},
                   memory=mem, network=net, broker=d.get("netBroker"),
                   faults=faults, reading=reading, raw_keys=sorted(d.keys()))
    except Exception as e:  # noqa: BLE001
        return _say(e)


class LogIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    panel: str | None = Field(default=None, description="MAC, name or address.")
    enable: bool | None = Field(
        default=None, description="Turn the ring buffer on or off. Leave unset to read.")
    clear: bool = Field(default=False, description="Empty the buffer before reading.")
    since: int | None = Field(
        default=None, ge=0, description="Only lines newer than this sequence number.")


@mcp.tool(
    name="panel_log",
    annotations={"title": "Read the panel's log", "readOnlyHint": False,
                 "destructiveHint": False, "openWorldHint": True})
async def panel_log(args: LogIn) -> str:
    """The firmware's own log, over the network.

    Worth knowing: **this is the only place a Lua effect's error text appears.**
    When a script fails, the panel draws "LUA ERROR" with a wrapped message on
    the screen and writes the full traceback here. No HTTP route returns that
    text as data, so after `panel_show_effect` reports a failure, read this.

    The buffer is off by default; turning it on costs a little RAM on a device
    that has very little. Turn it on, reproduce, read, turn it off.

    Returns:
        {"ok": true, "on": bool, "seq": N, "dropped": N, "bytes": N,
        "lines": ["..."], "text": "..."}
    """
    try:
        p = P.resolve(args.panel or DEFAULT_PANEL)
        q = []
        if args.enable is not None:
            q.append(f"on={1 if args.enable else 0}")
        if args.clear:
            q.append("clear")
        if args.since is not None:
            q.append(f"since={args.since}")
        path = "/api/log" + ("?" + "&".join(q) if q else "")
        text, headers = P.get_text(p["address"], path)
        return _ok(on=headers.get("X-Log-On"), seq=headers.get("X-Log-Seq"),
                   frm=headers.get("X-Log-From"), dropped=headers.get("X-Log-Dropped"),
                   bytes=headers.get("X-Log-Bytes"),
                   lines=text.splitlines(), text=text)
    except Exception as e:  # noqa: BLE001
        return _say(e)


@mcp.tool(
    name="panel_capabilities",
    annotations={"title": "What this firmware was built with", "readOnlyHint": True,
                 "openWorldHint": True})
async def panel_capabilities(panel: PanelArg = None) -> str:
    """Which modules this particular build has, and whether Home Assistant is behind it.

    Two panels running the same repository can have different capabilities,
    because features are compile-time flags. A route whose module is not built is
    **not registered**, so it answers 404 rather than a diagnostic. Ask here
    before assuming a page or a route exists.

    It also reports MQTT. Several pages have no source other than MQTT - cards,
    media and the four market pages - so on a panel with no broker they are not
    "empty for now": nothing will ever arrive. Flights and trains work either
    through Home Assistant or through a direct API key of their own.

    Returns:
        {"ok": true, "features": ["panel", "lua", ...], "routes": {"<path>": 200|404},
        "mqtt": {"configured", "connected", "status"}, "mqtt_only_pages": [...],
        "needs_home_assistant": bool, "note": "..."}
    """
    try:
        p = _pick(panel)
        a = p["address"]
        doc = P.get(a, "/api/panel")
        feats = sorted({pg.get("key") for pg in doc.get("pages", []) if pg.get("key")})
        mqtt = P.mqtt_of(a) or {}

        routes = {}
        for path in ("/api/lua", "/api/worldclock", "/api/railboard", "/api/flightboard",
                     "/api/yachtradar", "/api/media", "/api/market", "/api/clips",
                     "/api/fx3d"):
            try:
                P.get(a, path, timeout=8.0, tries=2)
                routes[path] = 200
            except P.PanelError as e:
                routes[path] = 404 if "404" in str(e) else str(e)[:60]

        mqtt_only = [k for k in ("cards", "media", "market")
                     if k in feats]
        return _ok(features=feats, routes=routes, mqtt=mqtt,
                   mqtt_only_pages=mqtt_only,
                   needs_home_assistant=bool(mqtt_only) and not mqtt.get("connected"),
                   note=("Pages listed in mqtt_only_pages have no source other than "
                         "MQTT. With no broker connected they will never fill, and "
                         "the carousel still dwells on them. tools/agent/bringup.py "
                         "switches them off."))
    except Exception as e:  # noqa: BLE001
        return _say(e)


# =========================================================================
#  Driving it
# =========================================================================

class ShowPageIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    page: int = Field(ge=0, le=63, description="Page index, from panel_status's pages[].")
    panel: str | None = None


@mcp.tool(
    name="panel_show_page",
    annotations={"title": "Put a page on screen", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_show_page(args: ShowPageIn) -> str:
    """Show a page, and prove it is showing.

    The payload the firmware actually reads is `{"show":{"page":N}}`. A flatter
    `{"showPage":N}` is silently ignored and still answers 200 with success -
    that mistake has cost this project real afternoons, which is why this tool
    compares the state afterwards instead of trusting the status code.

    Page numbers move with the build and with how many user cards exist. Read
    them from `panel_status` rather than remembering them.

    Returns:
        {"ok": true, "outcome": "changed"|"already", "value": N, "name": "...",
        "from": N} - or ok:false with outcome "refused" when the panel answered
        200 and nothing moved.
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        state = P.get(a, "/api/panel")
        pages = {pg["i"]: pg for pg in state.get("pages", [])}
        if args.page not in pages:
            return (f"There is no page {args.page} on this panel. It has "
                    f"{len(pages)}: " + ", ".join(f"{i}={pg['name']}"
                                                  for i, pg in sorted(pages.items())))
        r = P.verify(a, {"show": {"page": args.page}}, "/api/panel",
                     lambda d: (d.get("now") or {}).get("page"), "page", args.page)
        r["name"] = pages[args.page]["name"]
        if not pages[args.page].get("on"):
            r["note"] = (f"{r['name']} is switched off in the page list, so the "
                         "carousel will not visit it; showing it directly still works.")
        return json.dumps(r, ensure_ascii=False, indent=2)
    except Exception as e:  # noqa: BLE001
        return _say(e)


class StyleIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    style: int = Field(ge=0, le=16, description="A clock style id from panel_status's styles.")
    panel: str | None = None


@mcp.tool(
    name="panel_set_style",
    annotations={"title": "Set the clock style", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_set_style(args: StyleIn) -> str:
    """Choose the clock face, and prove it took.

    **The ids are not a contiguous range.** There are fifteen styles with ids up
    to 16, and 4 and 13 do not exist. Read them from `panel_status`.

    Two routes in this firmware set the style and they disagree: this tool uses
    `POST /api/panel {"style":N}`, which validates against the real style list,
    tells the page machinery, and persists. The other one, `GET
    /api/clock/style?id=N`, accepts ids that do not exist and writes the setting
    without saving it, so it is lost on the next reboot. This tool does not use
    it.

    Returns:
        {"ok": true, "outcome": "changed"|"already", "value": N, "name": "..."}
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        state = P.get(a, "/api/panel")
        styles = P.styles_of(state)
        if args.style not in styles:
            return ("That style id does not exist in this build. The ids are not "
                    "contiguous; this panel has: "
                    + ", ".join(f"{i}={n}" for i, n in sorted(styles.items())))
        r = P.verify(a, {"style": args.style}, "/api/panel",
                     lambda d: (d.get("now") or {}).get("style"), "style", args.style)
        r["name"] = styles[args.style]
        if (state.get("now") or {}).get("page") != 0:
            r["note"] = ("The style is set, but the clock page is not on screen - "
                         "use panel_show_page with the CLOCK page to see it.")
        return json.dumps(r, ensure_ascii=False, indent=2)
    except Exception as e:  # noqa: BLE001
        return _say(e)


class EnableIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    key: Literal["world", "flights", "trains", "yachts", "cards", "lua",
                 "media", "market"] = Field(
        description="Which page group. 'clock' is deliberately absent: the panel "
                    "refuses to switch it off.")
    on: bool
    panel: str | None = None


@mcp.tool(
    name="panel_enable_page",
    annotations={"title": "Switch a page on or off", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_enable_page(args: EnableIn) -> str:
    """Include a page in the walk, or take it out of it.

    A page switched off is still reachable directly; what changes is that the
    carousel and the knob stop visiting it. This is the right tool for a panel
    with no Home Assistant: cards, media and the market pages have no source
    other than MQTT, and the carousel otherwise dwells fifteen seconds on each
    of them showing nothing.

    The setting is persisted in NVS, written a couple of seconds after changes
    stop.

    Returns:
        {"ok": true, "outcome": "changed"|"already", "key": "...", "on": bool}
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        state = P.get(a, "/api/panel")
        if args.key not in P.pages_by_key(state):
            return (f"This build has no {args.key!r} page. It has: "
                    + ", ".join(k for k in P.pages_by_key(state) if k))
        r = P.verify(a, {"enable": {"key": args.key, "on": args.on}}, "/api/panel",
                     lambda d: (P.pages_by_key(d).get(args.key) or {}).get("on"),
                     f"{args.key}.on", args.on)
        return json.dumps(r, ensure_ascii=False, indent=2)
    except Exception as e:  # noqa: BLE001
        return _say(e)


class EffectIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    index: int = Field(ge=0, le=31, description="Effect index from panel_effects.")
    panel: str | None = None


@mcp.tool(
    name="panel_effects",
    annotations={"title": "List the Lua effects", "readOnlyHint": True,
                 "openWorldHint": True})
async def panel_effects(panel: PanelArg = None) -> str:
    """Which Lua effects this firmware carries, and which is running.

    Effects are compiled into the image, not uploaded, so this list is fixed
    until someone flashes. `current` is -1 when no effect page is showing.

    Returns: {"ok": true, "effects": ["FOOTBALL CLOCK", ...], "current": N}
    """
    try:
        p = _pick(panel)
        d = P.get(p["address"], "/api/lua")
        return _ok(effects=d.get("effects"), current=d.get("current"))
    except Exception as e:  # noqa: BLE001
        return _say(e)


@mcp.tool(
    name="panel_show_effect",
    annotations={"title": "Run a Lua effect", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_show_effect(args: EffectIn) -> str:
    """Start an effect, and wait long enough to know whether it actually started.

    **Starting one is not synchronous.** The POST only sets a request; the main
    loop picks it up, and the Lua task then loads the script - which is allowed
    up to 3 seconds. In between, the reported index is already the new one while
    the screen is still black. So this tool polls for up to 5 seconds and reports
    the frame rate it settles at, which is the honest signal that the effect is
    drawing.

    If it fails, the panel shows "LUA ERROR" on screen and **no HTTP route
    returns the error text** - read `panel_log` for it.

    Returns:
        {"ok": true, "outcome": "...", "index": N, "name": "...", "hz": N,
        "drawing": bool}
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        listing = P.get(a, "/api/lua")
        names = listing.get("effects") or []
        if args.index >= len(names):
            return (f"There is no effect {args.index}. This build has {len(names)}: "
                    + ", ".join(f"{i}={n}" for i, n in enumerate(names)))
        r = P.verify(a, {"show": args.index}, "/api/lua",
                     lambda d: d.get("current"), "current", args.index,
                     settle=1.0, tries=5)
        r["name"] = names[args.index]
        hz = None
        for _ in range(5):
            time.sleep(1.0)
            now = (P.get(a, "/api/panel").get("now") or {})
            hz = now.get("hz")
            if hz and hz > 2:
                break
        r["hz"] = hz
        r["drawing"] = bool(hz and hz > 2)
        if not r["drawing"]:
            r["note"] = ("The effect was selected but the panel is not reporting a "
                         "frame rate above the 2 Hz floor. It may have failed to "
                         "load - read panel_log, which is the only place the Lua "
                         "error text appears.")
        return json.dumps(r, ensure_ascii=False, indent=2)
    except Exception as e:  # noqa: BLE001
        return _say(e)


class NotifyIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    text: str = Field(min_length=1, max_length=160)
    color: str | None = Field(default=None, pattern=r"^#[0-9A-Fa-f]{6}$")
    duration_ms: int | None = Field(default=None, ge=1000, le=60000)
    position: Literal["top", "bottom"] | None = None
    panel: str | None = None


@mcp.tool(
    name="panel_notify",
    annotations={"title": "Show a banner", "readOnlyHint": False,
                 "destructiveHint": False, "openWorldHint": True})
async def panel_notify(args: NotifyIn) -> str:
    """Draw a banner over whatever is on screen.

    This is the one way to put arbitrary text on the panel without building
    anything, and unlike the cards it works with no MQTT broker at all.

    Two traps the firmware sets here: the route answers **403 until
    notifications are switched on** in the settings, and a malformed colour is
    *silently* ignored and falls back to white - so the pattern on `color` is
    enforced here rather than left to the device.

    Returns: {"ok": true, "shown": "<text>"}
    """
    try:
        p = _pick(args.panel)
        body = {"text": args.text}
        if args.color:
            body["color"] = args.color
        if args.duration_ms:
            body["duration"] = args.duration_ms
        if args.position:
            body["position"] = args.position
        try:
            P.post(p["address"], "/api/notify", body)
        except P.PanelError as e:
            if "403" in str(e):
                return ("The panel refused with 403: notifications are switched off "
                        "in its settings. Turn them on with a patch to /api/import "
                        '({"notifyEnabled": true}) and try again. This tool does not '
                        "change a persisted setting on its own.")
            raise
        return _ok(shown=args.text)
    except Exception as e:  # noqa: BLE001
        return _say(e)


class DisplayIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    action: Literal["on", "off", "brightness"]
    percent: int | None = Field(default=None, ge=0, le=100,
                                description="Required for action=brightness. A PERCENT, not 0-255.")
    panel: str | None = None


@mcp.tool(
    name="panel_display",
    annotations={"title": "Screen on, off, or brightness", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_display(args: DisplayIn) -> str:
    """Turn the screen on or off, or set its brightness.

    **None of this is persisted** - these routes change RAM only, and a reboot
    restores whatever is saved. That is deliberate in the firmware, and it makes
    them safe to use: nothing you do here survives to surprise someone later.

    `percent` really is a percent. The documented "0..255" for this route is
    wrong, and the panel silently clamps rather than refusing, so the bound is
    enforced here.

    Returns: {"ok": true, "displayOn": bool, "brightness": N}
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        if args.action == "brightness":
            if args.percent is None:
                return "action=brightness needs `percent` (0-100)."
            P.get(a, f"/api/display/brightness?value={args.percent}")
        else:
            P.get(a, f"/api/display/{args.action}")
        time.sleep(0.4)
        st = P.get(a, "/api/status")
        return _ok(displayOn=st.get("displayOn"), brightness=st.get("brightness"),
                   forcedOff=st.get("forcedOff"), scheduledOff=st.get("scheduledOff"),
                   note="Not persisted - a reboot restores the saved settings.")
    except Exception as e:  # noqa: BLE001
        return _say(e)


class RenameIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(pattern=r"^[A-Za-z][A-Za-z0-9-]{0,30}$",
                      description="Letters, digits and hyphens; starts with a letter; <=31.")
    panel: str | None = None


@mcp.tool(
    name="panel_rename",
    annotations={"title": "Give a panel its name", "readOnlyHint": False,
                 "destructiveHint": False, "idempotentHint": True,
                 "openWorldHint": True})
async def panel_rename(args: RenameIn) -> str:
    """Name a panel. **Ask the person for the name; do not choose it.**

    This is the first thing a new panel needs. Fresh out of a flash they all call
    themselves the same thing, so two on one network collide in mDNS and only the
    MAC tells them apart. The name is what a person will type for the rest of
    that panel's life, which makes it their decision, not a model's.

    It goes through `/api/rename`, which saves to NVS and restarts mDNS on the
    spot - no reboot. Not through the configuration portal: that posts a
    whole-form replace of about 125 fields and writes every boolean it does not
    carry back as false.

    The MAC does not change, so re-find the panel by MAC afterwards.

    Returns: {"ok": true, "from": "...", "name": "...", "mac": "...", "address": "..."}
    """
    try:
        p = _pick(args.panel)
        a = p["address"]
        before = P.get(a, "/api/info")
        if before.get("deviceName") == args.name:
            return _ok(outcome="already", name=args.name, mac=before.get("mac"),
                       address=a)
        P.post(a, "/api/rename", {"name": args.name})
        time.sleep(2.0)
        P.known(refresh=True)
        after = P.get(a, "/api/info")
        if after.get("deviceName") != args.name:
            return (f"The panel answered success but still reports "
                    f"{after.get('deviceName')!r}. Nothing was verified.")
        return _ok(outcome="changed", **{"from": before.get("deviceName")},
                   name=args.name, mac=after.get("mac"),
                   address=f"{args.name}.local",
                   note="Re-find it by MAC; the name changed, the MAC did not.")
    except Exception as e:  # noqa: BLE001
        return _say(e)


# =========================================================================
#  Writing a screen
# =========================================================================

@mcp.tool(
    name="effect_api",
    annotations={"title": "The Lua drawing API and its limits", "readOnlyHint": True})
async def effect_api() -> str:
    """Everything a script may call, and every budget it must stay inside.

    Read this before writing an effect. The sandbox is small and deliberately so,
    and the two things that most often surprise a model writing its first script
    are that **the canvas is not cleared between frames** and that **there is no
    clock in milliseconds** - time comes only from `px.t()` and `px.now()`.

    Returns: a JSON object describing the contract, the `px` table, the
    `presence` table, the environment, and the budgets.
    """
    return json.dumps({
        "contract": {
            "draw()": "REQUIRED. A global function, called once per frame, no args, no return.",
            "PERIOD": "optional global number > 0, default 60.0. Read ONCE at load; "
                      "changing it inside draw() does nothing.",
            "FPS": "optional global number, default 20, clamped to 1..30.",
            "chunk body": "runs once at load - build tables and precomputed grids there.",
        },
        "canvas": {
            "size": "128 x 64, RGB888",
            "persistence": "NOT cleared between frames. Zeroed once when the effect "
                           "opens, never again. Call px.clear() yourself for a clean "
                           "frame; leave it out and you get trails for free.",
            "readback": "px.get(x,y) reads the canvas; the C++ display has no readable "
                        "framebuffer, the Lua path does.",
        },
        "px": {
            "px.size()": "-> 128, 64",
            "px.t()": "-> the animation phase in [0,1), aligned to the epoch. At "
                      "PERIOD=60 this is the second hand, so a clock lands its change "
                      "exactly on the minute.",
            "px.now()": "-> {hour,min,sec,yday,utc,year}. hour/min/sec are LOCAL; yday "
                        "is 0-based; utc is the offset in HOURS (5.5 for India).",
            "px.clear(r,g,b)": "3 optional ints, default 0. Does NOT clamp - it narrows "
                               "to a byte, so clear(300,0,0) gives R=44.",
            "px.pixel(x,y,r,g,b)": "clamps to 0..255",
            "px.line(x0,y0,x1,y1,r,g,b)": "",
            "px.rect(x,y,w,h,r,g,b,fill)": "fill is truthy",
            "px.circle(cx,cy,rad,r,g,b,fill)": "",
            "px.text(x,y,s,r,g,b)": "",
            "px.width(s)": "-> pixel advance of that string",
            "px.get(x,y)": "-> r,g,b (0,0,0 off-canvas)",
            "px.blend(x,y,r,g,b,a)": "a is alpha, a number",
            "px.glow(cx,cy,rad,r,g,b[,amp])": "amp defaults to 1.0",
        },
        "text": {
            "font": "PicopixelFB, and it is the only one. Stock Adafruit Picopixel "
                    "with the U given a flat bottom, because at 2 mm pitch the stock "
                    "U reads as V across a room.",
            "range": "0x20..0x7E. **px.text folds lowercase onto uppercase** "
                     "(lua_px.cpp:202), so 'a' and 'A' draw the same ink - a ramp or "
                     "a palette built on letter case has half as many shapes as it "
                     "looks like it has.",
            "glyph box": "at most 3 px wide and 5 tall for the usual characters; "
                         "W, M, #, N, & and a few others are 4-5 wide.",
            "yAdvance": 7,
            "where the ink lands": "px.text(x, y, ...) puts the baseline at y+6 and "
                                   "the ink of a normal 5-tall glyph at rows y+2 "
                                   "through y+6. So to fill a cell whose top row is "
                                   "R, call px.text(x, R-2, ...).",
            "advance": "per glyph, NOT monospace: 2 px for . : I ! | ', 3 for ` , ; "
                       "< > ( ) [ ], 4 for most, 5-6 for W M # N &. px.width(s) sums "
                       "them. For a fixed grid, place each character yourself rather "
                       "than letting the advance do it.",
            "tiling": "glyph ink is exactly 5 rows, so rows placed 5 px apart tile "
                      "with no gap and no overlap; 6 px apart leaves one row of air.",
        },
        "character graphics": {
            "the cell idiom": "px.rect(x, y, w, h, r, g, b, true) then px.text over "
                              "it gives a cell with a background colour AND a "
                              "foreground colour - the pair that asciicker's AnsiCell "
                              "carries. Two colours and a glyph hold far more than "
                              "one colour and a brightness ramp.",
            "which way round": "for a picture, glyph ink is the lighter colour on a "
                               "darker fill. For something that must read as a SHAPE "
                               "- a digit, a bar - do the opposite: fill with the "
                               "bright colour and lay a sparse dark glyph over it. No "
                               "ASCII glyph is solid, so a shape built out of glyph "
                               "ink comes out as a scatter of dots.",
            "never leave a cell blank": "a flat area with no glyph turns the whole "
                                        "thing back into a colour mosaic. Give it a "
                                        "character whose ink coverage matches its "
                                        "brightness, and part the two colours around "
                                        "the cell's own mean so the level does not "
                                        "shift.",
            "worked example": "tools/luasim/scripts/la_gioconda.lua",
        },
        "presence": {
            "available": "only on a build with PRESENCE_ENABLED; detect it with "
                         'local RAD = rawget(_G, "presence")',
            "presence.scale()": "-> metres the radar fan covers (2, 4 or 6)",
            "presence.state()": "-> 0 scripted story, 1 live, 2 feed stopped",
            "presence.count([k])": "-> targets held k steps of 0.1 s ago",
            "presence.target(i)": "-> x_mm, y_mm, speed_cms, or nil. i is 1..3",
            "presence.trail(i,k)": "-> x_mm, y_mm, or nil",
        },
        "environment": {
            "libraries": "base, table, string, math. That is all.",
            "removed": "dofile, loadfile, load, collectgarbage are nil",
            "absent": "io, os, debug, package are not compiled in. coroutine is "
                      "compiled but deliberately not opened - a new thread would "
                      "escape the instruction budget.",
            "replaced": {
                "print(...)": "tab-separated, truncated to 159 chars, to serial",
                "log(v)": "one value to serial",
                "require(name)": "only what is already in package.loaded; there is no "
                                 "filesystem",
            },
            "NOT available": "there is no `sys` table for effects, so no millis() and "
                             "no heap_free(). No os.time, no os.clock. Time comes only "
                             "from px.t() and px.now().",
        },
        "budgets": {
            "loadInstructions": 20_000_000,
            "drawInstructions": 2_000_000,
            "loadMs": 3000,
            "drawMs": 500,
            "heapBytes": 4 * 1024 * 1024,
            "overrun tolerance": "a draw() over the TIME budget is dropped, up to 3 in "
                                 "a row; the 4th stops the effect. Any other error - "
                                 "syntax, runtime, instruction budget, heap - stops it "
                                 "immediately.",
            "on failure": 'the panel draws "LUA ERROR" and the text goes to the log '
                          "only. No HTTP route returns it.",
        },
        "installing": "Scripts are compiled into the firmware image. Write with "
                      "effect_write, look at it with effect_preview, measure it with "
                      "effect_check, generate the header with effect_install - then a "
                      "PERSON builds and flashes. There is no upload route and no "
                      "flashing tool here.",
    }, ensure_ascii=False, indent=2)


class WriteIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(pattern=r"^[A-Za-z0-9_]{1,40}$",
                      description="File stem. Letters, digits and underscore only - the "
                                  "generator enforces this. Becomes the on-panel name "
                                  "with underscores as spaces, upper-cased.")
    source: str = Field(min_length=1, max_length=200_000, description="The Lua source.")
    overwrite: bool = Field(default=False)


@mcp.tool(
    name="effect_write",
    annotations={"title": "Write a Lua effect to the repository", "readOnlyHint": False,
                 "destructiveHint": False})
async def effect_write(args: WriteIn) -> str:
    """Put a script in `tools/luasim/scripts/`, where everything else expects it.

    The file name matters beyond the filesystem: the generator turns
    `football_clock.lua` into the on-panel name "FOOTBALL CLOCK", and it refuses
    anything outside `[A-Za-z0-9_]`.

    This only writes the file. Nothing reaches a panel until a person builds and
    flashes.

    Returns: {"ok": true, "path": "...", "panel_name": "...", "bytes": N}
    """
    try:
        SCRIPTS.mkdir(parents=True, exist_ok=True)
        dest = SCRIPTS / f"{args.name}.lua"
        if dest.exists() and not args.overwrite:
            return (f"{dest} already exists. Pass overwrite=true to replace it, or "
                    "choose another name.")
        if "function draw()" not in args.source and "draw =" not in args.source:
            return ("This script defines no global draw(), so the panel would refuse "
                    "to load it. Add `function draw() ... end` and try again.")
        dest.write_text(args.source, encoding="utf-8")
        return _ok(path=str(dest), bytes=len(args.source.encode()),
                   panel_name=args.name.replace("_", " ").upper(),
                   next="effect_preview to look at it, then effect_check for the budgets")
    except Exception as e:  # noqa: BLE001
        return _say(e)


class PreviewIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(pattern=r"^[A-Za-z0-9_]{1,40}$")
    frames: int = Field(default=40, ge=1, le=600)
    start: str | None = Field(default=None, pattern=r"^\d{1,2}:\d{2}$",
                              description="Clock the simulator starts at, HH:MM.")
    animated: bool = Field(default=True, description="GIF when true, a single PNG when false.")


def _run(cmd, cwd, timeout=300):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


@mcp.tool(
    name="effect_preview",
    annotations={"title": "Render an effect without hardware", "readOnlyHint": False,
                 "destructiveHint": False})
async def effect_preview(args: PreviewIn) -> str:
    """Run the script in the reference simulator and produce a picture.

    `luasim` builds the *same vendored Lua* against a host copy of the `px`
    API, so what it draws is what the panel draws. You can write and fully
    preview an effect with no panel in the room.

    What it does **not** simulate: the PSRAM allocator, the instruction and time
    budgets, or the task's C stack. It also ignores `PERIOD` and `FPS`, and has
    no `presence` table. For the budgets, use `effect_check`.

    Returns:
        {"ok": true, "image": "/abs/path.gif", "frames": N} - open the image to
        see the effect.
    """
    try:
        src = SCRIPTS / f"{args.name}.lua"
        if not src.exists():
            return f"No script at {src}. Write it with effect_write first."
        code, out = _run(["make"], LUASIM)
        if code != 0:
            return f"luasim did not build:\n{out[-2000:]}"
        raw = LUASIM / f"{args.name}.raw"
        cmd = ["./luasim", f"scripts/{args.name}.lua", str(args.frames), raw.name]
        if args.start:
            cmd += ["--start", args.start]
        code, out = _run(cmd, LUASIM)
        if code != 0:
            return f"The script failed in the simulator:\n{out[-2000:]}"
        img = LUASIM / f"{args.name}.{'gif' if args.animated else 'png'}"
        code, out = _run(["python3", "render.py", raw.name, img.name, "6"], LUASIM)
        if code != 0:
            return f"render.py failed:\n{out[-2000:]}"
        return _ok(image=str(img), frames=args.frames,
                   note=("luasim does not enforce the panel's budgets and has no "
                         "presence table - run effect_check for those."))
    except subprocess.TimeoutExpired:
        return "The simulator did not finish in time."
    except Exception as e:  # noqa: BLE001
        return _say(e)


@mcp.tool(
    name="effect_check",
    annotations={"title": "Measure an effect against the panel's budgets",
                 "readOnlyHint": True})
async def effect_check(quick: bool = False) -> str:
    """Run every script through the firmware's own runtime, on this machine.

    `fx_parity.py` compiles the real `lua_fx.cpp`, `lua_px.cpp` and the sandbox
    for the host, **applies the panel's real budgets**, and compares each
    script's frames byte for byte against the reference simulator at four
    different clocks. It prints per-script instruction counts for load and draw,
    host draw time, Lua heap peak and C stack used.

    This is the check that catches a script which looks fine in a preview and
    then blows the 2,000,000-instruction draw budget on the panel.

    It needs one PlatformIO build to have happened first, for a header.

    Args:
        quick: parity only, skip the measurements.

    Returns: the tool's own report as text.
    """
    try:
        cmd = ["python3", "fx_parity.py"] + (["--quick"] if quick else [])
        code, out = _run(cmd, LUASIM, timeout=900)
        return json.dumps({"ok": code == 0, "exit": code, "report": out[-8000:]},
                          ensure_ascii=False, indent=2)
    except subprocess.TimeoutExpired:
        return "fx_parity.py did not finish in time."
    except Exception as e:  # noqa: BLE001
        return _say(e)


@mcp.tool(
    name="effect_install",
    annotations={"title": "Generate the effect table", "readOnlyHint": False,
                 "destructiveHint": False})
async def effect_install() -> str:
    """Regenerate `src/lua/lua_effects_scripts.h` from the scripts on disk.

    Every `.lua` in `tools/luasim/scripts/` (minus a small skip list) is compiled
    into the firmware image as a C string literal. This regenerates that header.

    **It does not reach the panel.** Effects are not uploadable; the image has to
    be built and flashed, and that is a person's call with the panel in front of
    them. This server has no flashing tool and will not get one.

    The same generator runs in the pre-commit hook with `--check`, so a stale
    header blocks a commit.

    Returns: {"ok": true, "effects": [...], "count": N, "next": "..."}
    """
    try:
        code, out = _run(["python3", "tools/luasim/gen_effects.py"], REPO)
        if code != 0:
            return f"gen_effects.py failed:\n{out[-2000:]}"
        names = sorted(p.stem for p in SCRIPTS.glob("*.lua")
                       if p.stem not in ("demo", "world_clock"))
        return _ok(effects=[n.replace("_", " ").upper() for n in names],
                   count=len(names), output=out.strip()[-1000:],
                   next=("A person must now build and flash. Nothing here does that: "
                         "uploading to a wall-mounted panel is a human's call, at a "
                         "moment they chose, with the panel in front of them."))
    except Exception as e:  # noqa: BLE001
        return _say(e)


if __name__ == "__main__":
    print(f"ledmatrix MCP server, repo {REPO}", file=sys.stderr)
    if DEFAULT_PANEL:
        print(f"default panel: {DEFAULT_PANEL}", file=sys.stderr)
    mcp.run()

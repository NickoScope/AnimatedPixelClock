"""Talking to a panel: finding it, asking it, and proving a change landed.

Everything an agent does to a panel goes through here, because four things are
true of this firmware that a plain `requests.post` gets wrong every time.

**The address moves and the name changes.** A panel is identified by the MAC in
its mDNS TXT record, and nothing else is stable. `resolve()` takes whatever the
caller has - a MAC, a name, an address, or nothing at all - and either returns
one panel or says plainly that a person has to choose.

**503 is the panel working, not failing.** Every `/api/...` route in the panel
group is wrapped in `webBusyRefuse()`: it refuses while a background fetch holds
the network, or while the largest free block of internal RAM is below the
back-off threshold. The refusal is `503`, `Retry-After: 1`, **`Content-Type:
text/plain` with an empty body** - so a client that calls `.json()` on it throws
on what was really "ask again in a second". Retried here, bounded, and never
surfaced.

**A 200 does not mean anything happened.** `/api/panel` has no key whitelist: an
unknown key is ignored, the handler falls through to building the state, and the
answer is `200 {"success":true, ...}`. `{"showPage":7}` has cost this project
real afternoons. So every mutating call here reads the state back and compares,
and reports "changed" and "was already so" as different outcomes - because an
"ok" that only means "nothing needed doing" is not a verification.

**A traceback is not an answer.** Anything that goes wrong comes back as a
`PanelError` carrying a sentence a person or an agent can act on.
"""

import json
import os
import pathlib
import socket
import re
import time
import uuid
import urllib.error
import urllib.request

import discover as D
from discover import discover

# The panel group demands this on POST or answers 415; it is deliberate
# hardening, because a text/plain body would skip the CORS preflight.
JSON_HEADERS = {"Content-Type": "application/json"}

BODY_MAX = 1024          # src/web/web_panel.cpp:129 - over this is a 413
BODY_MAX_MARKET = 8192   # src/web/web_panel.cpp:815

NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9-]{0,30}$")
MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$")


class PanelError(Exception):
    """Something a person or an agent can act on. Never a stack trace."""


class ChoiceNeeded(PanelError):
    """Several panels, and nobody said which. That is a decision, not a fault."""

    def __init__(self, panels):
        self.panels = panels
        rows = "\n".join(
            f"  {p.get('mac') or '??'}  {p['address']}  {p['name']}" for p in panels)
        super().__init__(
            "There is more than one panel on this network and none was chosen. "
            "Pass `panel` as one of these MACs (or a name, or an address):\n" + rows)


# --- when nothing answers, say which of the three things it is ----------

def _lan_blocked(addresses):
    """Can this interpreter reach the panels it can see at all?

    On macOS, Local Network access is granted per binary, and a Python that an
    MCP client launched - `uv`'s downloaded interpreter is the usual one - can
    be denied it while the same code run from a terminal works. The denial does
    not look like a denial: connecting to a 192.168.x.x address returns
    **`[Errno 65] No route to host` instantly**, which reads exactly like a
    panel that is switched off.

    The tell is that the internet still works. So: if a public address answers
    and a private one refuses a route, this is the permission and not the
    network. Measured on this Mac, 2026-09-21: `uv run python` reached
    example.com in 0.54 s and got Errno 65 from the panel's IP in 0.00 s, while
    the system python3 reached both."""
    import socket
    # Ask the real panels rather than a guessed gateway: a guess that happens
    # not to exist times out, which is a different symptom and would hide this
    # one. Errno 65/101/113 is the refusal we are looking for, and it comes
    # back instantly.
    refused = False
    for addr in addresses:
        host = addr.split(":")[0]
        try:
            ip = socket.gethostbyname(host)
        except OSError:
            continue
        try:
            k = socket.socket()
            k.settimeout(3.0)
            k.connect((ip, 80))
            k.close()
            return False          # something got through: not a permission
        except OSError as e:
            if e.errno in (65, 101, 113):
                refused = True
        except Exception:
            pass
    if not refused:
        return False
    # Nothing on the LAN has a route. Does the public internet?
    try:
        k = socket.socket()
        k.settimeout(4.0)
        k.connect(("1.1.1.1", 443))
        k.close()
        return True
    except Exception:
        return False


def _nothing_answered(seen):
    advertised = [p for p in seen if p.get("mac")]
    if advertised and _lan_blocked([p["address"] for p in advertised]):
        return (
            "mDNS found "
            + ", ".join(f"{p['name']} ({p['mac']})" for p in advertised)
            + " but this process cannot open a connection to a private address, "
            "while the public internet works. On macOS that is Local Network "
            "privacy, which is granted per binary: the Python your MCP client "
            "launched has not got it. Register the server with an interpreter "
            "that has - a venv made from the system python3 is the reliable one - "
            "rather than with `uv run`. See tools/agent/README.md.")
    if advertised:
        return (
            "mDNS found "
            + ", ".join(f"{p['name']} ({p['mac']})" for p in advertised)
            + " but none of them answered /api/info. They may be rebooting; try "
            "again in a few seconds.")
    if not D.can_browse():
        # "None answered" would be a lie. Nothing looked.
        return ("Nothing on this machine can browse mDNS, so nothing was "
                "searched - avahi-daemon resolves names but cannot browse; that "
                "needs `sudo apt install avahi-utils`. Until then, name the "
                "panel: LEDMATRIX_PANEL=<name|mac|address>, and a name is "
                "resolved as <name>.local through the OS resolver.")
    return ("No panel answered on this network. Check it is powered and on the "
            "same subnet; `python3 tools/agent/discover.py` shows what mDNS sees.")


# --- finding one ---------------------------------------------------------

_cache = {"at": 0.0, "panels": []}
_CACHE_S = 30.0     # within one process
_DISK_S = 600.0     # and across them

# Browsing mDNS costs the better part of a minute on a quiet network, and a
# command-line tool pays it on every invocation - which turned "put this screen
# on the panel" into a minute of waiting for nothing. The answer barely changes:
# a panel's MAC never does and its address rarely does. So the last answer is
# kept on disk, used if it still works, and thrown away the moment it does not.
_DISK = pathlib.Path(os.environ.get("XDG_CACHE_HOME") or
                     (pathlib.Path.home() / ".cache")) / "nickopanel" / "panels.json"


def _read_disk():
    try:
        d = json.loads(_DISK.read_text())
        if time.time() - d.get("at", 0) > _DISK_S:
            return None
        return d.get("panels") or None
    except Exception:  # noqa: BLE001
        return None


def _write_disk(panels):
    try:
        _DISK.parent.mkdir(parents=True, exist_ok=True)
        _DISK.write_text(json.dumps({"at": time.time(), "panels": panels}))
    except Exception:  # noqa: BLE001
        pass


def forget():
    """Drop the remembered answer. Called when one of them stops answering."""
    _cache["at"] = 0.0
    try:
        _DISK.unlink()
    except Exception:  # noqa: BLE001
        pass


def known(refresh=False):
    """Every panel on this network, remembered where that is safe to do.

    A cached entry is only handed back after the panel behind it has answered -
    a remembered address that has moved is worse than no answer, because the
    caller would go on to talk to nothing."""
    now = time.time()
    if not refresh and now - _cache["at"] <= _CACHE_S and _cache["panels"]:
        return _cache["panels"]

    if not refresh:
        remembered = _read_disk()
        if remembered:
            alive = []
            for p in remembered:
                got = _identify_quick(p.get("address"))
                if got:
                    q = dict(p)
                    q["reachable"] = True
                    q["version"] = got.get("version") or p.get("version")
                    q["uptime"] = got.get("uptime")
                    alive.append(q)
            if alive:
                _cache["panels"], _cache["at"] = alive, now
                return alive
            forget()

    panels = discover()
    # Resolve the mDNS name to an address once and keep both. Every HTTP call
    # through a .local name pays another multicast round trip - measured at
    # about five seconds a request on this network - and a tool that makes four
    # of them spends twenty seconds resolving a name that has not moved. The
    # name stays for display; the address is what is dialled.
    for p in panels:
        if p.get("reachable") and p.get("address", "").endswith(".local"):
            try:
                p["hostname"] = p["address"]
                p["address"] = socket.gethostbyname(p["hostname"])
            except OSError:
                pass
    _cache["panels"], _cache["at"] = panels, now
    if any(p.get("reachable") for p in panels):
        _write_disk(panels)
    return panels


def _identify_quick(addr, timeout=3.0):
    """Is the remembered address still one of ours? Short, because this is the
    fast path and a slow answer here costs what the cache was meant to save."""
    if not addr:
        return None
    try:
        with urllib.request.urlopen(f"http://{addr}/api/info", timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        if "freeInternalHeap" not in d:
            return None
        return {"version": d.get("version"), "uptime": d.get("uptime")}
    except urllib.error.HTTPError:
        return {"version": None, "uptime": None}     # answering is enough
    except Exception:  # noqa: BLE001
        return None


def resolve(panel=None, refresh=False):
    """`panel` may be a MAC, a device name, an address, or None.

    None means "the only one", and raises ChoiceNeeded when there is more than
    one. An address is taken at its word and not looked up - that is the escape
    hatch for a panel whose mDNS is not working."""
    if panel:
        p = panel.strip()
        # An address given outright: believe it, and let the request fail if it
        # is wrong. Someone passing an IP has a reason.
        if not MAC_RE.match(p) and ("." in p or ":" in p) and not p.count(":") == 5:
            return {"address": p, "mac": None, "name": None, "given": True}

    panels = [x for x in known(refresh) if x.get("reachable")]

    # Nothing browsed, but a name was given: resolve it through the OS. Browsing
    # mDNS and resolving a .local name are different abilities, and a Linux box
    # can have the second without the first - glibc with nss-mdns resolves names
    # through a running avahi-daemon, while browsing needs the avahi-browse
    # binary from avahi-utils, which Raspberry Pi OS does not install. Found on
    # nickol.local on 2026-09-22, where discovery returned nothing while the
    # panel answered perfectly well by name.
    if not panels and panel and not D.can_browse():
        one = D.by_name(panel.strip())
        if one:
            return one

    if not panels:
        raise PanelError(_nothing_answered(known(refresh)))

    if panel:
        want = panel.strip().lower().replace("-", ":")
        by_mac = [x for x in panels if (x.get("mac") or "").lower() == want]
        if by_mac:
            return by_mac[0]
        want_name = panel.strip().lower().removesuffix(".local")
        by_name = [x for x in panels
                   if (x.get("name") or "").lower().removesuffix(".local") == want_name]
        if by_name:
            return by_name[0]
        rows = ", ".join(f"{x.get('mac')} ({x['name']})" for x in panels)
        raise PanelError(f"No panel matches {panel!r}. On this network: {rows}")

    if len(panels) > 1:
        raise ChoiceNeeded(panels)
    return panels[0]


# --- asking it -----------------------------------------------------------

def _request(req, timeout, tries, what):
    """One request, with the 503 back-off absorbed.

    The bound matters in both directions: without it a genuinely stuck panel
    looks like a slow one forever; too small and the panel's designed back-off
    reads as a failure."""
    last = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                body = r.read().decode("utf-8", "replace")
                if not body:
                    return {}
                try:
                    return json.loads(body)
                except json.JSONDecodeError:
                    return {"_text": body}
        except urllib.error.HTTPError as e:
            if e.code == 503:
                # The panel is standing aside for something. Retry-After is 1 s.
                last = ("busy", e.code)
                time.sleep(float(e.headers.get("Retry-After") or 1) + 0.4 * attempt)
                continue
            detail = ""
            try:
                raw = e.read().decode("utf-8", "replace")
                detail = (json.loads(raw) or {}).get("error") or raw[:200]
            except Exception:
                pass
            raise PanelError(
                f"{what}: the panel answered HTTP {e.code}"
                + (f" - {detail}" if detail else "")) from None
        except urllib.error.URLError as e:
            last = ("unreachable", str(getattr(e, "reason", e)))
            time.sleep(0.8)
        except Exception as e:  # noqa: BLE001 - a transport fault, reported plainly
            last = ("error", f"{type(e).__name__}: {e}")
            time.sleep(0.8)

    if last and last[0] == "busy":
        raise PanelError(
            f"{what}: the panel refused with 503 for {tries} attempts. That is its "
            "own back-off - it does this while a fetch holds the network or while "
            "internal RAM is too fragmented to serve a request. Try again shortly, "
            "or look at `largestHeapBlock` in the status.")
    raise PanelError(f"{what}: could not reach the panel ({last[1] if last else 'no answer'})")


def get(address, path, timeout=12.0, tries=6):
    return _request(urllib.request.Request(f"http://{address}{path}"),
                    timeout, tries, f"GET {path}")


def post(address, path, payload, timeout=12.0, tries=6):
    body = json.dumps(payload).encode()
    cap = BODY_MAX_MARKET if path.startswith("/api/market") else BODY_MAX
    if len(body) > cap:
        raise PanelError(
            f"POST {path}: the body is {len(body)} bytes and this route accepts "
            f"{cap}. The panel would answer 413.")
    return _request(urllib.request.Request(f"http://{address}{path}", data=body,
                                           headers=JSON_HEADERS),
                    timeout, tries, f"POST {path}")


def post_file(address, path, field, filename, data, timeout=60.0, tries=4):
    """One multipart file, the way the firmware's upload routes want it.

    These routes are registered with a raw server.on rather than through
    route(), because webBusyRefuse() would reject a transfer already in flight -
    so there is no 503 to absorb here, only the ordinary faults."""
    boundary = "----nickopanel" + uuid.uuid4().hex
    head = (f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n").encode()
    body = head + data + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(
        f"http://{address}{path}", data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}",
                 "Content-Length": str(len(body))})
    return _request(req, timeout, tries, f"POST {path}")


def get_text(address, path, timeout=20.0, tries=4):
    """For /api/log, which is text/plain and carries its state in headers."""
    last = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(
                    urllib.request.Request(f"http://{address}{path}"), timeout=timeout) as r:
                return r.read().decode("utf-8", "replace"), dict(r.headers)
        except urllib.error.HTTPError as e:
            if e.code == 503:
                last = e.code
                time.sleep(1.0 + 0.4 * attempt)
                continue
            raise PanelError(f"GET {path}: HTTP {e.code}") from None
        except Exception as e:  # noqa: BLE001
            last = f"{type(e).__name__}: {e}"
            time.sleep(0.8)
    raise PanelError(f"GET {path}: no answer ({last})")


# --- proving it landed ---------------------------------------------------

def verify(address, payload, path, read, field, want, settle=0.6, tries=4):
    """POST, then read the state back and compare. Returns what really happened.

    `read` pulls the compared value out of a state document; the POST's own
    answer is tried first because `/api/panel` returns the new state inline, and
    only then is a fresh GET made. The three outcomes are kept apart on purpose:
    `changed` is a verification, `already` is not a failure but is not a
    verification either, and `refused` is the silent-no-op case this whole
    function exists for."""
    before = read(get(address, path))
    if before == want:
        return {"ok": True, "outcome": "already", "field": field, "value": want,
                "note": f"{field} was already {want!r}; nothing was sent"}

    answer = post(address, path, payload)
    got = read(answer) if answer else None
    for _ in range(tries):
        if got == want:
            return {"ok": True, "outcome": "changed", "field": field,
                    "from": before, "value": want}
        time.sleep(settle)
        got = read(get(address, path))

    return {"ok": False, "outcome": "refused", "field": field,
            "from": before, "wanted": want, "got": got,
            "note": ("The panel answered 200 but the state did not change. On "
                     "/api/panel an unknown key is silently ignored and still "
                     "returns success - check the key spelling against the "
                     "handler, not against this tool's memory.")}


# --- small readers the tools share --------------------------------------

def pages_by_key(panel_doc):
    return {p.get("key"): p for p in (panel_doc or {}).get("pages", [])}


def styles_of(panel_doc):
    """The ids are NOT contiguous - 4 and 13 do not exist. Always read them."""
    return {s["id"]: s["name"] for s in (panel_doc or {}).get("styles", [])}


def mqtt_of(address):
    """The same mqtt{configured,connected,status} rides on three routes."""
    for route in ("flightboard", "railboard", "media"):
        try:
            d = get(address, f"/api/{route}", tries=2)
        except PanelError:
            continue
        if isinstance(d.get("mqtt"), dict):
            return d["mqtt"]
    return None

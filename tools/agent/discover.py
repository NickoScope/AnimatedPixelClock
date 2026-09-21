#!/usr/bin/env python3
"""Find the panels on this network, from any machine, and say which is which.

There is no hard-coded address anywhere in the agent tooling on purpose. A panel
gets its IP from the router and it moves; what does not move is the MAC the
firmware puts in its mDNS TXT record (src/network/network.cpp:232-236):

    _http._tcp   port 80
    TXT: model=AnimatedPixelClock  version=<firmware>  mac=<the chip's>

So: browse, keep the ones whose model says they are ours, and identify them by
MAC. Several panels on one network are therefore told apart by something stable
rather than by whichever address they happened to be given.

    python3 discover.py                 # human, one line each
    python3 discover.py --json          # one JSON object, for a tool to read
    python3 discover.py --mac 90:E5:...  # just that one's address, or exit 4

Exit codes follow the rest of the agent tooling: 0 found, 3 several found and
none named (a person or a caller must choose), 4 none found.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

SERVICE = "_http._tcp"
MODEL = "AnimatedPixelClock"


def _run(cmd, timeout):
    """Run it, and get text back whether it finished or was cut off.

    The subtlety that cost a first run: on TimeoutExpired, subprocess hands back
    `stdout` as BYTES even when text=True was asked for - the decoding never
    happens because the call did not return normally. And these two tools are
    always cut off, because browsing mDNS does not end on its own."""
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout or ""
    except subprocess.TimeoutExpired as e:
        out = e.stdout or b""
        return out.decode("utf-8", "replace") if isinstance(out, bytes) else out
    except FileNotFoundError:
        return ""


def _browse_macos(timeout):
    """dns-sd ships with macOS. It streams, so it is run with a timeout and read."""
    names = []
    out = _run(["dns-sd", "-B", SERVICE, "local"], timeout)
    for line in out.splitlines():
        m = re.search(r"_http\._tcp\.\s+(.+?)\s*$", line)
        if m and "Add" in line:
            names.append(m.group(1).strip())
    return list(dict.fromkeys(names))


def _resolve_macos(name, timeout):
    out = _run(["dns-sd", "-L", name, SERVICE, "local"], timeout)
    host, txt = None, {}
    for line in out.splitlines():
        m = re.search(r"can be reached at\s+([^\s:]+)", line)
        if m:
            host = m.group(1).rstrip(".")
        for k, v in re.findall(r"(\w+)=([^\s]+)", line):
            if k in ("model", "version", "mac"):
                txt[k] = v
    return host, txt


def _browse_avahi(timeout):
    """Linux. avahi-browse resolves in one pass, which is simpler than dns-sd."""
    found = []
    out = _run(["avahi-browse", "-rptk", SERVICE], timeout)
    for line in out.splitlines():
        if not line.startswith("="):
            continue
        f = line.split(";")
        if len(f) < 10:
            continue
        txt = {}
        for kv in re.findall(r'"([^"]+)"', f[9]):
            if "=" in kv:
                k, v = kv.split("=", 1)
                txt[k] = v
        found.append({"name": f[3], "host": f[6], "ip": f[7], "txt": txt})
    return found


def _identify(addr, timeout=8.0):
    """Ask the panel itself. This is what makes a subnet-scan fallback honest:
    only something that answers /api/info with a version is one of ours.

    **An HTTP error is still an answer.** `/api/info` is one of the two routes
    the firmware never refuses, but a panel under load can still fail a request,
    and every other route answers 503 while it stands aside for a background
    fetch. Something that sends back a status code is unambiguously there, so it
    is reported as reachable and busy - not as absent. Getting this wrong makes
    a working panel disappear from the list exactly when it is being used, which
    is the worst possible moment."""
    try:
        with urllib.request.urlopen(f"http://{addr}/api/info", timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        if "freeInternalHeap" not in d:
            return None
        return {"version": d.get("version"), "build": d.get("build"),
                "name": d.get("deviceName"), "uptime": d.get("uptime")}
    except urllib.error.HTTPError as e:
        return {"version": None, "build": None, "name": None, "uptime": None,
                "busy": True, "http": e.code}
    except Exception:
        return None


def discover(timeout=6.0):
    panels = []
    if shutil.which("avahi-browse"):
        for e in _browse_avahi(timeout):
            if e["txt"].get("model") == MODEL:
                panels.append({"name": e["name"], "address": e["ip"] or e["host"],
                               "mac": e["txt"].get("mac"), "version": e["txt"].get("version")})
    elif shutil.which("dns-sd"):
        for name in _browse_macos(timeout):
            host, txt = _resolve_macos(name, timeout)
            if txt.get("model") == MODEL and host:
                panels.append({"name": name, "address": host,
                               "mac": txt.get("mac"), "version": txt.get("version")})
    # Whatever mDNS said, confirm each one by asking it. A stale advertisement
    # for a panel that has since gone is worse than no answer, because a caller
    # would go on to talk to nothing.
    live = []
    for p in panels:
        got = _identify(p["address"])
        if got:
            p["reachable"] = True
            p["version"] = got["version"] or p.get("version")
            p["uptime"] = got.get("uptime")
            if got.get("busy"):
                p["busy"] = True
                p["http"] = got.get("http")
        else:
            p["reachable"] = False
        live.append(p)
    return live


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--mac", help="print only this panel's address")
    ap.add_argument("--timeout", type=float, default=6.0)
    args = ap.parse_args()

    panels = discover(args.timeout)
    reachable = [p for p in panels if p.get("reachable")]

    if args.mac:
        want = args.mac.lower().replace("-", ":")
        for p in panels:
            if (p.get("mac") or "").lower() == want:
                print(p["address"])
                return 0 if p.get("reachable") else 4
        print(f"no panel with MAC {args.mac}", file=sys.stderr)
        return 4

    if args.json:
        print(json.dumps({"schema_version": 1, "ok": bool(reachable),
                          "code": "ok" if reachable else "none_found",
                          "result": {"panels": panels, "count": len(panels)},
                          "error": None, "warnings": []}, ensure_ascii=False))
    else:
        if not panels:
            print("no panels found on this network", file=sys.stderr)
        for p in panels:
            mark = ("" if p.get("reachable") else "  (advertised but not answering)")
            if p.get("busy"):
                mark = f"  (busy, HTTP {p.get('http')} - it is there)"
            print(f"  {p.get('mac') or '??':17}  {p['address']:28}  "
                  f"v{p.get('version') or '?':8}  {p['name']}{mark}")

    if not reachable:
        return 4
    # Several, and nobody said which: that is a decision, not a failure.
    return 0 if len(reachable) == 1 else 3


if __name__ == "__main__":
    sys.exit(main())

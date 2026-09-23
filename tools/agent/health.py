#!/usr/bin/env python3
"""A panel self-test: exercise it, watch it, keep the logs, say what was odd.

The same test a person or an agent used to run by hand, call by call, spending
a conversation's worth of tokens each time: is it on the network, do the
controls work, does the portal load, did anything fail underneath. Here it is
one run, with every raw reading written to a folder and a short summary out,
so the tokens go on reading the anomalies rather than on producing them.

    python3 tools/agent/health.py --list                # the panels on this network
    python3 tools/agent/health.py                       # the only panel on the network
    python3 tools/agent/health.py --panel NickoScopeMatrix-64x128-01   # or a MAC, or an IP
    python3 tools/agent/health.py --all                 # every panel, one after another
    python3 tools/agent/health.py --stress              # pages every 0.5 s, calls back to back
    python3 tools/agent/health.py --serial /dev/cu.usbmodem2101   # also log the USB console
    python3 tools/agent/health.py --read-only           # look, do not touch
    python3 tools/agent/health.py --json                # the whole report on stdout

With more than one panel on the network and none named, it lists them and
stops rather than guessing: a self-test changes brightness and pages, and the
wrong panel is someone else's evening.

Pace, chosen to be real use rather than kind to the panel (owner, 2026-09-23):
a page every 2 s, as fast as a hand walks the knob; control calls 0.5 s apart,
as a person clicks; the portal loaded with no pause at all, because the
portal's own request queue sends the next request as soon as the last one is
answered. --stress puts a page up every 0.5 s with no pauses anywhere.
On 2026-09-23 a page every 1.5 s, eight seconds after a boot, took the radio's
pool from 11,252 B to 1,396 B: the flight and rail pages start TLS fetches, and
with the API calls on top the Wi-Fi driver failed its receive buffers and the
panel was off the network for three minutes until the link watchdog restarted
Wi-Fi. A test that cannot reproduce that would not be worth running.

Opening the USB console (--serial) resets the board on macOS even with DTR and
RTS held low, so the test then starts on a fresh boot. The report says how
long after boot it started, because the first minute is the tightest one.

Whatever happens during the run - an exception, the network gone, Ctrl+C -
it puts back what it changed: brightness, the screen's forced-off state, the
page on screen, the log ring's on/off. --read-only changes nothing at all, the
log ring included: it reads the ring as it finds it. Brightness goes back as a
percentage, the only way the API takes it, so a byte the portal slider set
(0..255) may come back one or two steps off in the byte while reading the same
percent.

What it does, in order, with an ICMP ping running underneath the whole time:

 1. reads /api/info, /api/status, /api/panel (the baseline);
 2. switches the firmware's log ring on and clears it;
 3. unless --read-only: brightness to another value and back, the screen off and
    on, a notification banner, and one page of each kind put on screen - every
    change read back from the panel, because a 200 proves nothing here
    (see panel.py) - then the original page and brightness restored;
 4. loads the portal as a browser does, one request at a time (the page, its
    CSS and JS, then the JSON routes the portal polls), timing each and
    counting 503s, which are the panel saying "not now", not a fault;
 5. reads /api/info again, reads the log ring and switches it back off;
 6. compares before and after and writes the findings.

What it deliberately does not do: Save & apply (the form is built by the
portal's JavaScript; the browser is the honest test of it), flash, reboot,
or put the yacht radar on screen unless asked (--include yachts): on firmware
up to 2.5.4 that page either fails to start its 12 KB task or, when it does
start, its TLS session starves the radio (knowledge base HANDOFF, 2026-09-23).

Every finding is a fact from the panel's own counters, not a guessed
threshold. The one number used as a line is 1,626 B: the size of the Wi-Fi
driver's receive buffer, measured failing (caps 0x80c) on 2026-09-22.

Exit status: 0 PASS, 1 WARN, 2 FAIL, 3 could not reach the panel.
"""
import argparse, gzip, json, platform, re, subprocess, sys, threading, time
import urllib.error, urllib.request
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import panel as P  # noqa: E402

RADIO_BUFFER = 1626          # bytes, caps 0x80c: the allocation that fails first
RISKY_KEYS = {"yachts"}      # pages that are known to hurt the panel when shown
PORTAL_JSON = ["/api/portal", "/api/info", "/api/status", "/api/panel", "/metrics",
               "/api/anim/list", "/api/lua", "/api/worldclock", "/api/knob"]
RESET_REASONS = {0: "unknown", 1: "power-on", 2: "external pin", 3: "software",
                 4: "panic", 5: "interrupt watchdog", 6: "task watchdog",
                 7: "other watchdog", 8: "deep sleep", 9: "brownout", 10: "SDIO"}


def _version(v):
    try:
        return tuple(int(x) for x in re.findall(r"\d+", v)[:3])
    except ValueError:
        return ()


def now():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


class Log:
    """Every line the run produces, kept for the folder."""
    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()

    def __call__(self, kind, text):
        with self.lock:
            self.lines.append(f"{now()} {kind:6s} {text}")


# --- the network underneath -------------------------------------------------

class Pinger(threading.Thread):
    def __init__(self, host):
        super().__init__(daemon=True)
        self.host, self.stop, self.rows = host, threading.Event(), []
        wait = ["-W", "1000"] if platform.system() == "Darwin" else ["-W", "1"]
        self.cmd = ["ping", "-c", "1", *wait, host]

    def run(self):
        while not self.stop.is_set():
            ok = subprocess.run(self.cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL).returncode == 0
            self.rows.append((now(), ok))
            self.stop.wait(1.0)

    def summary(self):
        lost = [t for t, ok in self.rows if not ok]
        longest, run = 0, 0
        for _, ok in self.rows:
            run = 0 if ok else run + 1
            longest = max(longest, run)
        return {"sent": len(self.rows), "lost": len(lost), "longest_gap_s": longest,
                "first_loss": lost[0] if lost else None}


class Serial(threading.Thread):
    """The USB console, read-only. DTR and RTS are held low before the port
    opens: on the ESP32-S3's USB-Serial-JTAG, RTS high with DTR low is a reset,
    and pyserial asserts both on open by default."""
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port, self.stop, self.lines, self.error = port, threading.Event(), [], None
        self.opened = None      # monotonic time the port opened
        self.stamps = []        # monotonic time of each line

    def run(self):
        try:
            import serial  # pyserial
        except ImportError:
            self.error = "pyserial is not installed (pip install pyserial)"
            return
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = self.port, 115200, 0.3
            s.dtr = False
            s.rts = False
            s.open()
            self.opened = time.monotonic()
        except Exception as e:  # noqa: BLE001
            self.error = f"could not open {self.port}: {e}"
            return
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = s.read(4096)
            except Exception as e:  # noqa: BLE001 - the port goes when the board resets
                self.error = f"lost {self.port} after {len(self.lines)} lines: {e}"
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append(f"{now()} {line.decode('utf-8', 'replace').rstrip()}")
                self.stamps.append(time.monotonic())
        try:
            s.close()
        except Exception:  # noqa: BLE001
            pass


SERIAL_MARKS = [
    ("radio buffer failed", re.compile(r"allocation failed: 1626 B, caps 0x80c")),
    ("allocation failed", re.compile(r"allocation failed")),
    ("reboot", re.compile(r"rst:0x|ESP-ROM:")),
    ("crash", re.compile(r"Guru Meditation|panic|abort\(\)|Backtrace:|Brownout")),
    ("link watchdog", re.compile(r"Link (probe|blind|recovery)|transmit path stuck")),
    ("wifi reconnect", re.compile(r"WiFi reconnect|Attempting WiFi")),
]


# --- asking -----------------------------------------------------------------

def raw(address, path, log, method="GET", body=None, timeout=10.0):
    """One request, no retries: the code, the time, the bytes. For the portal
    phase, where a 503 is information rather than something to hide."""
    url = f"http://{address}{path}"
    req = urllib.request.Request(url, data=body, method=method,
                                 headers={"Content-Type": "application/json"} if body else {})
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
            if r.headers.get("Content-Encoding") == "gzip":
                data = gzip.decompress(data)
            code, hdrs = r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        code, data, hdrs = e.code, e.read(), dict(e.headers)
    except Exception as e:  # noqa: BLE001
        code, data, hdrs = None, str(e).encode(), {}
    ms = round((time.monotonic() - t0) * 1000)
    log("http", f"{method} {path} -> {code} {len(data)} B {ms} ms")
    return code, data, hdrs, ms


def status(address):
    return P.get(address, "/api/status")


# --- the run ----------------------------------------------------------------

def run(panel=None, serial_port=None, read_only=False, include=(), notify=True,
        out_dir=None, stress=False):
    log = Log()
    report = {"started": datetime.now().isoformat(timespec="seconds"), "findings": [],
              "steps": {}}
    def find(level, text):
        report["findings"].append({"level": level, "text": text})
        log(level, text)

    try:
        p = P.resolve(panel)
    except P.ChoiceNeeded:
        raise
    except Exception as e:  # noqa: BLE001
        report["verdict"] = "UNREACHABLE"
        report["findings"].append({"level": "FAIL", "text": str(e)})
        return report, log
    addr = p["address"]
    report["panel"] = {"address": addr, "mac": p.get("mac"), "name": p.get("name")}

    ser = Serial(serial_port) if serial_port else None
    if ser:
        ser.start()
        time.sleep(2.0)
        # If opening the port reset the board, wait for it to come back.
        for _ in range(45):
            try:
                P.get(addr, "/api/info", timeout=3.0, tries=1)
                break
            except Exception:  # noqa: BLE001
                time.sleep(1.0)
    ping = Pinger(addr)
    ping.start()

    orig = {}          # what the run may change, for restore()
    changed = set()    # what it did change
    ring_on = None
    try:
        before = P.get(addr, "/api/info")
        t_before = time.monotonic()      # after the answer: expected uptime is then a floor
        st0 = status(addr)
        pan0 = P.get(addr, "/api/panel")
        st0 = dict(st0, version=before.get("version"))   # /api/status has no version
        orig = {"brightness": st0.get("brightness"), "forcedOff": st0.get("forcedOff"),
                "page": (pan0.get("now") or {}).get("page")}
        report["before"] = before
        report["firmware"] = before.get("version")
        report["pace"] = "stress" if stress else "normal"
        up = before.get("uptime") or 0
        report["uptime_at_start"] = up
        if up < 120:
            log("info", f"the test starts {up} s after a boot: the tightest minutes for memory")
        log("info", f"firmware {before.get('version')}, uptime {before.get('uptime')} s, "
                    f"reset reason {RESET_REASONS.get(before.get('resetReason'), before.get('resetReason'))}")

        try:
            _, hdrs = P.get_text(addr, "/api/log?since=999999999")
            ring_on = hdrs.get("X-Log-On") == "1"
            if not read_only:
                P.get_text(addr, "/api/log?on=1&clear")
                changed.add("ring")
        except Exception as e:  # noqa: BLE001
            log("warn", f"log ring not switched on: {e}")

        if not read_only:
            report["steps"]["controls"] = controls(
                addr, st0, pan0, include, notify, log, find, changed,
                0.5 if stress else 2.0, 0 if stress else 0.5)
        report["steps"]["portal"] = portal(addr, log, find, 0)

        time.sleep(2)
        t_after = time.monotonic()
        after = P.get(addr, "/api/info")
        report["after"] = after
        try:
            text, hdrs = P.get_text(addr, "/api/log")
            report["panel_log"] = text
            report["panel_log_dropped"] = hdrs.get("X-Log-Dropped")
        except Exception as e:  # noqa: BLE001
            log("warn", f"log ring not read: {e}")
        compare(before, after, t_after - t_before, find)
    except BaseException as e:  # noqa: BLE001 - Ctrl+C and SIGTERM too
        find("FAIL", f"the run stopped: {e!r}")
        stopped = None if isinstance(e, Exception) else e
    else:
        stopped = None
    restore(addr, orig, changed, ring_on, log, find)
    finish(report, log, ping, ser, out_dir, find)
    if stopped is not None:
        raise stopped
    return report, log


def finish(report, log, ping, ser, out_dir, find):
    """Stop the watchers, add their findings, set the verdict, write the folder.
    Runs on an interrupted run too, so its logs are kept."""
    if report.get("finished"):
        return report
    ping.stop.set()
    ping.join(3)
    if ser:
        ser.stop.set()
        ser.join(3)
    report["ping"] = ping.summary()
    if not report["ping"]["sent"]:
        find("WARN", "the ping never ran, so the network was not watched (is ping installed?)")
    if report["ping"]["lost"]:
        find("FAIL" if report["ping"]["longest_gap_s"] >= 5 else "WARN",
             f"ping lost {report['ping']['lost']} of {report['ping']['sent']}, longest gap "
             f"{report['ping']['longest_gap_s']} s, first at {report['ping']['first_loss']}")
    if ser:
        report["serial"] = serial_findings(ser, find)

    levels = {f["level"] for f in report["findings"]}
    report["verdict"] = "FAIL" if "FAIL" in levels else "WARN" if "WARN" in levels else "PASS"
    report["finished"] = datetime.now().isoformat(timespec="seconds")
    report["ping_rows"] = ping.rows
    report["serial_lines"] = ser.lines if ser else None
    if out_dir:
        write(out_dir, report, log)
    return report


def restore(addr, orig, changed, ring_on, log, find):
    """Put back what the run changed. Each step on its own, so one failure does
    not leave the others undone; safe to call twice."""
    if not changed:
        return
    def step(what, fn):
        try:
            fn()
            changed.discard(what)
        except Exception as e:  # noqa: BLE001
            find("FAIL", f"could not restore {what}: {e} - check the panel by hand")
    if "brightness" in changed and orig.get("brightness") is not None:
        def b():
            want = orig["brightness"]
            P.get(addr, f"/api/display/brightness?value={want}")
            got = status(addr).get("brightness")
            if got == want - 1:       # firmware up to 2.5.3 truncates the round trip
                P.get(addr, f"/api/display/brightness?value={want + 1}")
        step("brightness", b)
    if "display" in changed:
        step("display", lambda: P.get(addr, "/api/display/off" if orig.get("forcedOff")
                                      else "/api/display/on"))
    if "page" in changed:
        if orig.get("page") is None:
            find("WARN", "the page on screen before the test was not known; the panel is "
                         "left on the last page the test showed")
            changed.discard("page")
        else:
            step("page", lambda: P.post(addr, "/api/panel", {"show": {"page": orig["page"]}}))
    if "ring" in changed:
        step("ring", lambda: P.get_text(addr, "/api/log?on=" + ("1" if ring_on else "0")))


def controls(addr, st0, pan0, include, notify, log, find, changed, dwell, gap):
    done = {}
    def pause():
        if gap:
            time.sleep(gap)
    b0 = st0.get("brightness")
    b1 = 40 if (b0 or 0) > 60 else 80
    exact = _version(st0.get("version") or "") >= (2, 5, 4)
    try:
        if b0 is None:
            raise RuntimeError("/api/status has no brightness; step skipped")
        changed.add("brightness")
        P.get(addr, f"/api/display/brightness?value={b1}")
        pause()
        got = status(addr).get("brightness")
        pause()
        P.get(addr, f"/api/display/brightness?value={b0}")
        pause()
        back = status(addr).get("brightness")
        if back is not None and b0 is not None and back == b0 - 1:
            # Firmware up to 2.5.3 truncates percent -> 0..255 -> percent, so
            # writing 98 reads 97; one more writes the byte that reads 98.
            P.get(addr, f"/api/display/brightness?value={b0 + 1}")
            pause()
            back = status(addr).get("brightness")
            done["brightness_drift"] = "1% lost per set-and-read: firmware up to 2.5.3"
        done["brightness"] = {"set": b1, "read": got, "restored": back}
        changed.discard("brightness")
        # Up to 2.5.3 percent -> 0..255 -> percent truncated, so 98 came back as
        # 97; 2.5.4 rounds and every 0..100 must survive exactly.
        tol = 0 if exact else 1
        if got is None or back is None or abs(got - b1) > tol or abs(back - b0) > tol:
            find("FAIL", f"brightness did not follow: set {b1} read {got}, restored {b0} read {back}")
    except RuntimeError as e:
        log("info", str(e))
    except Exception as e:  # noqa: BLE001
        find("FAIL", f"brightness: {e}")

    try:
        if st0.get("scheduledOff"):
            raise RuntimeError("the screen is in its scheduled off hours; off/on step skipped")
        if st0.get("forcedOff"):
            raise RuntimeError("the screen was switched off by its owner; off/on step skipped")
        changed.add("display")
        P.get(addr, "/api/display/off")
        pause()
        off = status(addr).get("displayOn")
        pause()
        P.get(addr, "/api/display/on")
        pause()
        on = status(addr).get("displayOn")
        changed.discard("display")
        done["display"] = {"off_read": off, "on_read": on}
        if off is not False or on is not True:
            find("FAIL", f"display off/on did not follow: off read {off}, on read {on}")
    except RuntimeError as e:
        log("info", str(e))
    except Exception as e:  # noqa: BLE001
        find("FAIL", f"display off/on: {e}")

    if notify:
        try:
            P.post(addr, "/api/notify", {"text": "SELFTEST", "icon": "check", "duration": 3000})
            done["notify"] = "sent"
        except Exception as e:  # noqa: BLE001
            find("FAIL", f"notify: {e}")

    now0 = (pan0.get("now") or {}).get("page")
    shown, skipped, seen = [], [], set()
    for pg in pan0.get("pages") or []:
        key = pg.get("key")
        if not pg.get("on") or key in seen:
            continue
        if key == "lua" and not pg.get("name"):
            continue                       # an empty upload slot
        seen.add(key)
        if key in RISKY_KEYS and key not in include:
            skipped.append(key)
            continue
        try:
            changed.add("page")
            d = P.post(addr, "/api/panel", {"show": {"page": pg["i"]}})
            landed = (d.get("now") or {}).get("page")
            time.sleep(dwell)
            shown.append({"page": pg["i"], "key": key, "name": pg.get("name"), "landed": landed})
            if landed != pg["i"]:
                find("FAIL", f"page {pg['i']} ({pg.get('name')}) did not come up: now {landed}")
        except Exception as e:  # noqa: BLE001
            find("FAIL", f"page {pg['i']} ({pg.get('name')}): {e}")
    done["pages"] = shown           # restore() puts the original page back
    if skipped:
        log("info", f"not shown, known to hurt the panel: {', '.join(skipped)} "
                    "(--include to show anyway)")
        done["skipped"] = skipped
    return done


def portal(addr, log, find, gap):
    rows = []
    code, data, _, ms = raw(addr, "/", log)
    rows.append(("/", code, ms))
    if code != 200:
        find("FAIL", f"the portal page answered {code}")
        return [{"path": p, "code": c, "ms": ms} for p, c, ms in rows]
    assets = re.findall(r'(?:src|href)="(/[^"]+\.(?:css|js)[^"]*)"', data.decode("utf-8", "replace"))
    for path in assets + PORTAL_JSON:
        code, _, _, ms = raw(addr, path, log)
        rows.append((path, code, ms))
        if code == 503:
            time.sleep(1.0)                   # Retry-After: 1, as the portal's queue does
            code, _, _, ms = raw(addr, path, log)
            rows.append((path + " (retry)", code, ms))
            if code == 503:
                find("WARN", f"portal {path} still answered 503 after its Retry-After")
        if code not in (200, 503, 404):
            find("FAIL", f"portal {path} answered {code}")
        if gap:
            time.sleep(gap)
    refused = sum(1 for _, c, _ in rows if c == 503)
    if refused:
        log("info", f"portal: {refused} answers were 503 (the panel's back-off)")
    slow = [(p, ms) for p, c, ms in rows if c == 200 and ms > 3000]
    for p, ms in slow:
        find("WARN", f"portal {p} took {ms} ms")
    return [{"path": p, "code": c, "ms": ms} for p, c, ms in rows]


def compare(b, a, elapsed, find):
    expected = (b.get("uptime") or 0) + elapsed
    reset = RESET_REASONS.get(a.get("resetReason"), a.get("resetReason"))
    # uptime is whole seconds from millis()/1000: 5 s covers rounding and the
    # request times, and a reboot costs more than that in boot time alone.
    if (a.get("uptime") or 0) < expected - 5:
        find("FAIL", f"the panel rebooted during the test (uptime {b.get('uptime')} s, "
                     f"{elapsed:.0f} s later {a.get('uptime')} s; reset reason {reset})")
        return
    for k in ("allocFails", "linkRecoveries", "webRefused"):
        if (a.get(k) or 0) < (b.get(k) or 0):
            find("FAIL", f"{k} went down ({b.get(k)} -> {a.get(k)}): the counters were "
                         f"reset, which means a reboot (reset reason {reset})")
            return
    df = (a.get("allocFails") or 0) - (b.get("allocFails") or 0)
    if df > 0:
        task, size = a.get("allocFailTask"), a.get("allocFailBytes")
        radio = task == "wifi" and size == RADIO_BUFFER
        find("WARN", f"{df} failed allocation(s) during the test, the last "
                     f"{size} B in task {task!r}"
                     + (" - the radio's receive buffer, which is how the 2026-09-22 drop began"
                        if radio else ""))
    dr = (a.get("linkRecoveries") or 0) - (b.get("linkRecoveries") or 0)
    if dr > 0:
        find("FAIL", f"the link watchdog had to restart Wi-Fi {dr} time(s)")
    if a.get("dmaMin") is not None and a["dmaMin"] < RADIO_BUFFER:
        find("WARN", f"since boot the radio's memory pool (DMA-capable) has fallen to "
                     f"{a['dmaMin']} B at its lowest, below the {RADIO_BUFFER} B buffer "
                     "the Wi-Fi driver asks for (dmaMin, since boot, not only this test)")
    if (a.get("linkBlindS") or 0) > 0:
        find("WARN", f"the link has been blind for {a['linkBlindS']} s")


def serial_findings(ser, find):
    if ser.error:
        find("WARN", f"serial: {ser.error}")
        return {"error": ser.error}
    counts = {}
    for line, t in zip(ser.lines, ser.stamps):
        for name, rx in SERIAL_MARKS:
            if rx.search(line):
                # Opening the port resets the board on macOS: that reboot is ours.
                if name == "reboot" and ser.opened and t - ser.opened < 5:
                    name = "reset by opening the port"
                counts[name] = counts.get(name, 0) + 1
                break
    for name in ("reboot", "crash"):
        if counts.get(name):
            find("FAIL", f"serial: {counts[name]} line(s) of {name}")
    if counts.get("radio buffer failed"):
        find("WARN", f"serial: the radio's buffer failed {counts['radio buffer failed']} time(s)")
    return {"lines": len(ser.lines), "marks": counts}


def write(out_dir, report, log):
    d = Path(out_dir)
    d.mkdir(parents=True, exist_ok=True)
    slim = {k: v for k, v in report.items()
            if k not in ("ping_rows", "serial_lines", "panel_log", "before", "after")}
    (d / "report.json").write_text(json.dumps(slim, indent=2, ensure_ascii=False))
    (d / "info_before.json").write_text(json.dumps(report.get("before"), indent=2))
    (d / "info_after.json").write_text(json.dumps(report.get("after"), indent=2))
    (d / "run.log").write_text("\n".join(log.lines) + "\n")
    (d / "ping.log").write_text("\n".join(f"{t} {'ok' if ok else '--'}"
                                          for t, ok in report.get("ping_rows") or []) + "\n")
    if report.get("panel_log") is not None:
        (d / "panel_log.txt").write_text(report["panel_log"])
    if report.get("serial_lines") is not None:
        (d / "serial.log").write_text("\n".join(report["serial_lines"]) + "\n")


def summary(report, out_dir=None):
    lines = [f"panel self-test: {report.get('verdict')}  "
             f"{(report.get('panel') or {}).get('name') or ''} "
             f"{(report.get('panel') or {}).get('address') or ''}  firmware {report.get('firmware')}"]
    pg = report.get("ping") or {}
    if pg:
        lines.append(f"  ping {pg.get('sent', 0) - pg.get('lost', 0)}/{pg.get('sent', 0)}")
    c = (report.get("steps") or {}).get("controls")
    if c:
        lines.append(f"  controls: brightness {c.get('brightness')}, display {c.get('display')}, "
                     f"pages shown {len(c.get('pages') or [])}"
                     + (f", skipped {c['skipped']}" if c.get("skipped") else ""))
    pr = (report.get("steps") or {}).get("portal")
    if pr:
        codes = {}
        for r in pr:
            codes[r["code"]] = codes.get(r["code"], 0) + 1
        lines.append(f"  portal: {len(pr)} requests, codes {codes}, "
                     f"slowest {max(r['ms'] for r in pr)} ms")
    a = report.get("after") or {}
    if a:
        lines.append(f"  after: uptime {a.get('uptime')} s, allocFails {a.get('allocFails')} "
                     f"({a.get('allocFailTask')}), linkRecoveries {a.get('linkRecoveries')}, "
                     f"dma {a.get('dmaFree')}/{a.get('dmaLargest')} min {a.get('dmaMin')}")
    if report.get("serial"):
        lines.append(f"  serial: {report['serial']}")
    for f in report.get("findings") or []:
        lines.append(f"  [{f['level']}] {f['text']}")
    if out_dir:
        lines.append(f"  logs: {out_dir}")
    return "\n".join(lines)


def _on_term(signum, frame):
    # SIGTERM is how a process is usually stopped, and Python's default for it
    # exits without running finally blocks: the screen would stay off.
    raise KeyboardInterrupt(f"signal {signum}")


def main():
    import signal
    signal.signal(signal.SIGTERM, _on_term)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--panel", help="MAC, name or address; default: the only panel")
    ap.add_argument("--list", action="store_true", help="list the panels on this network and stop")
    ap.add_argument("--all", action="store_true", help="test every panel, one after another")
    ap.add_argument("--stress", action="store_true", help="a page every 0.5 s instead of 2 s, no pauses")
    ap.add_argument("--serial", help="USB console port to log during the test")
    ap.add_argument("--read-only", action="store_true", help="look only, change nothing")
    ap.add_argument("--include", action="append", default=[],
                    help="also show a page kind skipped as risky (e.g. yachts)")
    ap.add_argument("--no-notify", action="store_true", help="skip the banner")
    ap.add_argument("--out", help="folder for the logs (default: health-logs/<time>)")
    ap.add_argument("--json", action="store_true", help="print the full report")
    a = ap.parse_args()
    panels = [x for x in P.known(refresh=True) if x.get("reachable")] if (a.list or a.all) else None
    if a.list:
        for x in panels or []:
            print(f"{x.get('name') or '?':34s} {x.get('mac') or '?':17s} {x['address']}")
        if not panels:
            print("no panel answered on this network")
        sys.exit(0 if panels else 3)
    targets = [x.get("mac") or x["address"] for x in panels] if a.all else [a.panel]
    if a.all and not targets:
        print("no panel answered on this network")
        sys.exit(3)
    if a.all and a.serial and len(targets) > 1:
        sys.exit("--serial with --all: the USB console belongs to one panel; name it with --panel")
    worst = 0
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    for t in targets:
        base = Path(a.out) if a.out else Path.cwd() / "health-logs" / stamp
        out = str(base / (t or "panel").replace(":", "") if len(targets) > 1 else base)
        try:
            report, _ = run(t, a.serial, a.read_only, tuple(a.include), not a.no_notify, out, a.stress)
        except KeyboardInterrupt:
            print(f"interrupted; what it changed was put back, the logs are in {out}")
            sys.exit(130)
        except P.ChoiceNeeded as e:
            print("More than one panel is on this network; name one with --panel "
                  "(or test them all with --all):")
            for x in e.panels:
                print(f"  {x.get('name') or '?':34s} {x.get('mac') or '?':17s} {x['address']}")
            sys.exit(3)
        if a.json:
            print(json.dumps({k: v for k, v in report.items() if k not in ("ping_rows", "serial_lines")},
                             indent=2, ensure_ascii=False))
        else:
            print(summary(report, out))
        worst = max(worst, {"PASS": 0, "WARN": 1, "FAIL": 2}.get(report.get("verdict"), 3))
    sys.exit(worst)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Headless check of the twin's web flasher, before anyone opens it in a browser.

Needs a twin started with the page:  tools/twin/twin.py run --web PORT  (any flash state; --blank is
fine). Starts Google Chrome headless with a throw-away profile and drives it over the DevTools
protocol, standard library only:

  1. the page: /flasher/index.html?twin=auto - navigator.serial is the shim, ESP Web Tools 10.4.0
     renders its install button (not the "unsupported" or "not allowed" slot), flasher.js shows the
     firmware version, every part its manifest names (bootloader, partition table, otadata, app) is
     served, and nothing throws;
  2. the port: /flasher/selftest.html?twin=auto&run=1 - the shim against the Web Serial draft, then
     esptool-js 0.6.0 (the one inside ESP Web Tools 10.4.0) through it: reset into download mode,
     stub, flash ID, the hard reset ESP Web Tools does after flashing. Nothing is written to flash;
  3. the language (twin-lang.js): ?lang=ru puts the twin's note and the title in Russian and keeps it
     in localStorage, the port chooser follows window.twinSetLang while it is open, and the self-test
     page takes the kept language and its EN button switches it back. Runs between 1 and 2 and leaves
     English kept.

  python3 tools/twin/flasher/check_flasher.py --port 8790 [--chrome PATH] [--only page|lang|port]

Exit status 0 all passed, 1 something failed, 2 not run (no Chrome, no twin). Loads ESP Web Tools and
esptool-js from unpkg.com, as the page does, so it needs the internet.
"""
import argparse, base64, json, os, shutil, socket, struct, subprocess, sys, tempfile, time, urllib.parse, urllib.request

CHROMES = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
           "/Applications/Chromium.app/Contents/MacOS/Chromium", "google-chrome", "chromium", "chromium-browser"]


class WS:
    """A WebSocket client (RFC 6455) for the DevTools endpoint: text frames, masked, no extensions."""

    def __init__(self, url):
        u = urllib.parse.urlparse(url)
        self.s = socket.create_connection((u.hostname, u.port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET {u.path} HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\n"
                        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            c = self.s.recv(1)
            if not c:
                raise ConnectionError("DevTools closed the connection")
            head += c
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError(head.split(b"\r\n")[0].decode(errors="replace"))
        self.buf = b""

    def _frame(self, op, payload):
        n, m = len(payload), os.urandom(4)
        h = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
                                  if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n))
        self.s.sendall(h + m + bytes(b ^ m[i & 3] for i, b in enumerate(payload)))

    def send(self, text):
        self._frame(1, text.encode())

    def _need(self, n, deadline):
        while len(self.buf) < n:
            self.s.settimeout(max(0.01, deadline - time.time()))
            try:
                d = self.s.recv(1 << 16)
            except socket.timeout:
                return False
            if not d:
                raise ConnectionError("DevTools closed the connection")
            self.buf += d
        return True

    def recv(self, timeout):
        """One whole message (text), or None on timeout."""
        deadline, parts = time.time() + timeout, []
        while True:
            if not self._need(2, deadline):
                return None
            fin, op, n, off = self.buf[0] & 0x80, self.buf[0] & 0x0F, self.buf[1] & 0x7F, 2
            if n == 126:
                self._need(4, deadline + 5)
                n, off = struct.unpack(">H", self.buf[2:4])[0], 4
            elif n == 127:
                self._need(10, deadline + 5)
                n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
            self._need(off + n, deadline + 30)
            data, self.buf = self.buf[off:off + n], self.buf[off + n:]
            if op == 9:
                self._frame(10, data)
                continue
            if op == 8:
                raise ConnectionError("DevTools closed the connection")
            parts.append(data)
            if fin:
                return b"".join(parts).decode("utf-8", "replace")


class Chrome:
    def __init__(self, exe):
        self.profile = tempfile.mkdtemp(prefix="twin-flasher-check-")
        self.p = subprocess.Popen([exe, "--headless=new", f"--user-data-dir={self.profile}", "--remote-debugging-port=0",
                                   "--no-first-run", "--no-default-browser-check", "--disable-extensions",
                                   "--disable-sync", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port_file = os.path.join(self.profile, "DevToolsActivePort")
        for _ in range(200):
            if os.path.exists(port_file) and open(port_file).read().count("\n") >= 1:
                break
            time.sleep(0.05)
        else:
            self.close()
            raise RuntimeError("Chrome did not open its DevTools port")
        port = int(open(port_file).read().split()[0])
        pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=10))
        self.ws = WS(next(p for p in pages if p["type"] == "page")["webSocketDebuggerUrl"])
        self.next_id, self.events = 0, []
        for m in ("Runtime.enable", "Log.enable", "Page.enable"):
            self.call(m)

    def call(self, method, params=None, timeout=30):
        self.next_id += 1
        me = self.next_id
        self.ws.send(json.dumps({"id": me, "method": method, "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.ws.recv(deadline - time.time())
            if msg is None:
                break
            m = json.loads(msg)
            if m.get("id") == me:
                if "error" in m:
                    raise RuntimeError(f"{method}: {m['error']}")
                return m.get("result", {})
            if "method" in m:
                self.events.append(m)
        raise TimeoutError(method)

    def eval(self, expr, timeout=30, gesture=False):
        """EXPR's value; GESTURE runs it as if from a click (user activation, which requestPort needs)."""
        r = self.call("Runtime.evaluate", {"expression": expr, "returnByValue": True, "awaitPromise": True,
                                           "userGesture": gesture}, timeout)
        if "exceptionDetails" in r:
            raise RuntimeError(r["exceptionDetails"].get("text", "exception"))
        return r["result"].get("value")

    def wait(self, expr, timeout):
        """Poll EXPR until it is truthy; returns its value or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                v = self.eval(expr)
            except RuntimeError:
                v = None
            if v:
                return v
            time.sleep(0.25)
        return None

    def problems(self, since=0):
        """Exceptions and console/log errors seen since event number SINCE."""
        out = []
        for e in self.events[since:]:
            p = e.get("params", {})
            if e["method"] == "Runtime.exceptionThrown":
                d = p["exceptionDetails"]
                out.append("exception: " + (d.get("exception", {}).get("description") or d.get("text", "")).split("\n")[0])
            elif e["method"] == "Runtime.consoleAPICalled" and p.get("type") == "error":
                out.append("console.error: " + " ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", []))[:200])
            elif e["method"] == "Log.entryAdded" and p["entry"].get("level") == "error" \
                    and not p["entry"].get("url", "").endswith("/favicon.ico"):   # the browser's own request
                out.append(f"log: {p['entry'].get('text', '')[:160]} {p['entry'].get('url', '')}")
        return out

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(10)
        except Exception:
            self.p.kill()
        shutil.rmtree(self.profile, ignore_errors=True)


results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  - {detail}" if detail else ""), flush=True)


def page(c, base):
    since = len(c.events)
    c.call("Page.navigate", {"url": f"{base}/flasher/index.html?twin=auto"})
    shim = c.wait("window.__twinSerial && navigator.serial === window.__twinSerial.serial", 20)
    check("page: navigator.serial is the twin's shim", bool(shim))
    version = c.wait("(t => /^v?[0-9]/.test(t || '') || t === 'unavailable' ? t : null)(document.getElementById('spec-version')?.textContent?.trim())", 30)
    check("page: flasher.js loaded firmware/latest/VERSION", bool(version) and version != "unavailable", str(version))
    slot = c.wait("""(() => { const b = document.querySelector('esp-web-install-button');
        const r = b && b.shadowRoot; if (!r || !customElements.get('esp-web-install-button')) return null;
        return [...r.querySelectorAll('slot')].map(s => s.name).join(',') || null; })()""", 30)
    check("page: ESP Web Tools renders the install button (slot 'activate')", slot == "activate", f"slots: {slot}")
    ewt = c.eval("[...document.scripts].map(s => s.src).filter(s => s.includes('esp-web-tools')).join(' ')")
    check("page: ESP Web Tools pinned to 10.4.0", "esp-web-tools@10.4.0/" in (ewt or ""), ewt)
    # Every file the manifest names is served (twin.py build_web puts the release's parts there): a missing
    # part would only show as a failed install, after the person has chosen the port and the erase.
    parts = c.eval("""(async () => { const b = document.querySelector('esp-web-install-button');
        const m = b && b.getAttribute('manifest'); if (!m) return null;
        const man = await (await fetch(m)).json(); const out = [];
        for (const p of man.builds[0].parts) {
          const r = await fetch(p.path, { cache: 'no-store' }); const n = r.ok ? (await r.arrayBuffer()).byteLength : 0;
          out.push({ file: p.path.split('/').pop(), offset: '0x' + p.offset.toString(16), status: r.status, bytes: n });
        }
        return out; })()""", timeout=60) if slot == "activate" else None
    check("page: every part of the manifest is served", bool(parts) and all(p["status"] == 200 and p["bytes"] for p in parts),
          "; ".join(f"{p['offset']} {p['file']} {p['status']} {p['bytes']} B" for p in parts or []))
    time.sleep(1.0)
    bad = c.problems(since)
    check("page: no exceptions or console errors", not bad, "; ".join(bad))


def lang(c, base):
    since = len(c.events)
    c.call("Page.navigate", {"url": f"{base}/flasher/index.html?lang=ru"})
    ready = c.wait("!!(location.search === '?lang=ru' && window.twinLang && document.getElementById('twin-banner') "
                   "&& document.readyState !== 'loading')", 20)
    ru = c.eval("""(() => { const b = document.getElementById('twin-banner'); if (!b) return null;
        return { title: document.title, note: b.lang, html: document.documentElement.lang,
                 link: b.querySelector('a').getAttribute('href'), text: b.querySelector('a').textContent,
                 pressed: b.querySelector('button[aria-pressed="true"]').dataset.lang,
                 kept: localStorage.getItem('twin-lang') }; })()""") if ready else None
    check("lang: ?lang=ru puts the twin's note and title in Russian and keeps it; the page stays English",
          bool(ru) and ru["title"].startswith("Двойник · ") and ru["note"] == "ru" and ru["html"] == "en"
          and ru["link"] == "../panel.html?lang=ru" and ru["text"] == "Панель двойника" and ru["pressed"] == "ru"
          and ru["kept"] == "ru", json.dumps(ru, ensure_ascii=False))
    ch = c.eval("""(async () => {
        const p = navigator.serial.requestPort().then(() => 'resolved', (e) => e.name);
        const d = document.getElementById('twin-serial-chooser');
        if (!d) return { cancel: await p };
        const words = () => [d.lang, d.querySelector('h2').textContent, d.querySelector('[data-choice="twin"]').textContent];
        const ru = words();
        window.twinSetLang('en');
        const en = words();
        d.querySelector('[data-choice="cancel"]').click();
        const b = document.getElementById('twin-banner');
        return { ru, en, cancel: await p, gone: !document.getElementById('twin-serial-chooser'), title: document.title,
                 link: b.querySelector('a').getAttribute('href'), kept: localStorage.getItem('twin-lang') };
    })()""", gesture=True) if ready else None
    check("lang: the port chooser speaks the page's language and follows twinSetLang while open",
          bool(ch) and ch.get("ru") == ["ru", "Какой порт открыть?", "Двойник — USB-Serial/JTAG эмулятора (303A:1001)"]
          and ch.get("en") == ["en", "Which port to open?", "Twin — the emulator’s USB-Serial/JTAG (303A:1001)"]
          and ch.get("cancel") == "NotFoundError" and ch.get("gone"), json.dumps(ch, ensure_ascii=False))
    check("lang: twinSetLang('en') switches the note and the panel link back and keeps it",
          bool(ch) and str(ch.get("title", "")).startswith("Twin · ") and ch.get("link") == "../panel.html?lang=en"
          and ch.get("kept") == "en", json.dumps({k: ch.get(k) for k in ("title", "link", "kept")} if ch else None,
                                                 ensure_ascii=False))
    if ready:
        c.eval("window.twinSetLang('ru')")
    c.call("Page.navigate", {"url": f"{base}/flasher/selftest.html"})
    ready = c.wait("!!(location.pathname.endsWith('/selftest.html') && window.twinLang && document.getElementById('run') "
                   "&& document.readyState === 'complete')", 20)
    words = """(() => ({ html: document.documentElement.lang, title: document.title,
        heading: document.querySelector('h1').textContent, run: document.getElementById('run').textContent,
        pressed: document.querySelector('.lang button[aria-pressed="true"]').dataset.lang,
        kept: localStorage.getItem('twin-lang') }))()"""
    st_ru = c.eval(words) if ready else None
    check("lang: the self-test page takes the kept language (no ?lang=)",
          bool(st_ru) and st_ru == {"html": "ru", "title": "Двойник · самотест порта",
                                    "heading": "Самотест: порт двойника и esptool-js 0.6.0", "run": "Запустить",
                                    "pressed": "ru", "kept": "ru"}, json.dumps(st_ru, ensure_ascii=False))
    st_en = c.eval("document.querySelector('.lang button[data-lang=\"en\"]').click(), " + words) if ready else None
    check("lang: the self-test page's EN button switches it at once and keeps it",
          bool(st_en) and st_en == {"html": "en", "title": "Twin · port self-test",
                                    "heading": "Self-test: the twin’s port and esptool-js 0.6.0", "run": "Run",
                                    "pressed": "en", "kept": "en"}, json.dumps(st_en, ensure_ascii=False))
    bad = c.problems(since)
    check("lang: no exceptions or console errors", not bad, "; ".join(bad))


def port(c, base, timeout):
    since = len(c.events)
    c.call("Page.navigate", {"url": f"{base}/flasher/selftest.html?twin=auto&run=1"})
    t0 = time.time()
    done = c.wait("window.__selftest && window.__selftest.done", timeout)
    st = c.eval("JSON.stringify(window.__selftest || null)")
    st = json.loads(st) if st else None
    if not done or not st:
        check("port: self-test finished", False, f"not done after {timeout} s")
        return
    for k in st["checks"]:
        check("port: " + k["name"], k["ok"], k.get("detail", ""))
    print(f"      self-test {time.time() - t0:.1f} s; times (ms) {st.get('ms')}; counters {st.get('counters')}")
    print(f"      ROM after the reset: {st.get('banner')}")
    for line in st.get("terminal", []):
        print("      esptool-js | " + line)
    bad = c.problems(since)
    check("port: no exceptions or console errors", not bad, "; ".join(bad))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8790, help="the twin's --web port")
    ap.add_argument("--chrome", default=os.environ.get("CHROME"), help="Chrome or Chromium executable")
    ap.add_argument("--only", choices=("page", "lang", "port"))
    ap.add_argument("--timeout", type=float, default=240, help="seconds for the port self-test")
    a = ap.parse_args()
    exe = next((p for p in ([a.chrome] if a.chrome else CHROMES) if p and (os.path.exists(p) or shutil.which(p))), None)
    if not exe:
        print("NOT RUN: no Chrome or Chromium found (--chrome PATH or CHROME=...)")
        sys.exit(2)
    base = f"http://127.0.0.1:{a.port}"
    try:
        urllib.request.urlopen(f"{base}/flasher/index.html", timeout=5)
    except Exception as e:
        print(f"NOT RUN: {base}/flasher/index.html does not answer ({e}); start tools/twin/twin.py run --web {a.port}")
        sys.exit(2)
    c = Chrome(shutil.which(exe) or exe)
    try:
        print(f"Chrome: {c.call('Browser.getVersion').get('product')}")
        if a.only in (None, "page"):
            page(c, base)
        if a.only in (None, "lang"):
            lang(c, base)
        if a.only in (None, "port"):
            port(c, base, a.timeout)
    finally:
        c.close()
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()

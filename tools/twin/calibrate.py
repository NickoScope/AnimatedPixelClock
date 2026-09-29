#!/usr/bin/env python3
"""calibrate.py - how fast the twin runs Lua, against the panel's own measurements.

The panel's numbers are its firmware's 30-second reports ("[luafx] NAME: N frames in 30 s
(F fps), draw avg A max M ms, ..."), taken on 2026-09-29 13:37-13:47 with kd_all.py: every
KINETIC DIGITS LED scene, one press of the effect's button each (the script as committed in
4136eb8). The twin runs the same script through the same firmware routes - upload, show, click -
and reads the same report from its console, once per timing configuration, all in parallel.

  calibrate.py [--configs cpi1,cpi2,...] [--out DIR]

A configuration is a name and the engine flags it adds:
  cpi1        the default: one cycle an instruction
  cpiN        --cpi N: N cycles an instruction, uniform (the engine's JIT timing); N may be
              fractional (cpi2.45, the default of twin.py run), 1..256 as the engine takes it
  approxM     --approximate-timing --approximate-cache --approximate-memory M
Results: DIR/<config>.json and a table on stdout (twin draw avg / panel draw avg per scene).
"""
import argparse, json, os, re, shutil, subprocess, sys, threading, time, urllib.request

HOME = os.path.expanduser(os.environ.get("TWIN_HOME", "~/twin"))
EXE = os.path.join(HOME, "esp32sim", "target", "release", "esp32sim")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from twin import wifi_spec   # noqa: E402  one check of what the engine's --wifi can carry
SCRIPT = os.path.join(HOME, "research", "kinetic_4136eb8.lua")   # git show 4136eb8:tools/luasim/scripts/kinetic_digits_led.lua

# The panel, kd_all.txt 2026-09-29 13:47 (scene, draw avg ms, fps, instr max). The first press
# moves from the opening scene to the second one and holds it, so the order starts there.
PANEL = [
    ("8x12 plasma", 39.1, 15.2, 48000), ("6x11 rings", 43.7, 15.2, 48000), ("11x21 clock", 7.7, 15.2, 8000),
    ("5x9 life", 36.6, 15.2, 61000), ("5x9 cube", 47.2, 15.0, 82000), ("dots plasma", 58.7, 15.0, 83000),
    ("8x16 text", 12.3, 15.2, 21000), ("dots rings", 62.8, 14.0, 109000), ("4x7 plasma", 65.9, 14.3, 77000),
    ("8x12 text", 13.2, 15.2, 24000),
]
REPORT = re.compile(r"\[luafx\] (.+?): (\d+) frames in (\d+) s \(([\d.]+) fps\), draw avg ([\d.]+) max ([\d.]+) ms, ~(\d+) instr max")


def flags(name):
    if name.startswith("cpi"):
        # The engine parses --cpi as f64 in 1.0..=256 (esp32sim cli/src/lib.rs, --cpi), so the text
        # goes through as written; only plain decimals, so the name and the flag read the same.
        v = name[3:]
        if not re.fullmatch(r"\d+(\.\d+)?", v) or not 1 <= float(v) <= 256:
            raise SystemExit(f"unknown configuration {name}: cpiN takes N from 1 to 256, e.g. cpi2.45")
        return [] if float(v) == 1 else ["--cpi", v]
    if name.startswith("approx"):
        return ["--approximate-timing", "--approximate-cache", "--approximate-memory", name[6:]]
    raise SystemExit(f"unknown configuration {name}")


class Twin:
    def __init__(self, name, port, out):
        self.name, self.port, self.out = name, port, out
        self.flash = os.path.join(out, f"{name}.flash.bin")
        self.log = os.path.join(out, f"{name}.console.log")
        shutil.copyfile(os.path.join(HOME, "state", "flash.bin"), self.flash)
        ssid, pw = (open(os.path.join(HOME, "state", "wifi.txt")).read().splitlines() + ["", ""])[:2]
        args = [EXE, "--board", "hub75-panel", "--boot", "rom", "--rom", os.path.join(HOME, "rom", "esp32s3_rev0_rom.elf"),
                "--flash-image", os.path.join(HOME, "fw", "v2.7.3", "merged.bin"), "--flash-mb", "32", "--flash-id", "c28039",
                "--psram-mb", "16", "--efuse-regs", os.path.join(HOME, "efuse-opi.txt"), "--console", "usb", "--no-dump",
                "--flash-persist", self.flash, "--wifi", wifi_spec(ssid, pw), "--hostfwd", f"tcp:{port}-80",
                "--max-seconds", "3000"] + flags(name)
        self.proc = subprocess.Popen(args, stdout=open(self.log, "wb"), stderr=subprocess.STDOUT)

    def url(self, p):
        return f"http://127.0.0.1:{self.port}{p}"

    def get(self, p):
        return urllib.request.urlopen(self.url(p), timeout=120).read().decode()

    def post(self, p, d):
        r = urllib.request.Request(self.url(p), data=json.dumps(d).encode(), headers={"Content-Type": "application/json"})
        return urllib.request.urlopen(r, timeout=120).read().decode()

    def upload(self, path, name):
        boundary = "twincalibrate"
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"script\"; filename=\"{os.path.basename(path)}\"\r\n"
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + open(path, "rb").read() + f"\r\n--{boundary}--\r\n".encode()
        r = urllib.request.Request(self.url(f"/api/lua/upload?name={name}"), data=body,
                                   headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        return urllib.request.urlopen(r, timeout=300).read().decode()

    def reports(self):
        text = open(self.log, "rb").read().decode("utf-8", "replace")
        return [m.groups() for m in REPORT.finditer(text)]

    def wait_up(self, limit=600):
        t0 = time.time()
        while time.time() - t0 < limit:
            try:
                return json.loads(self.get("/api/info"))
            except Exception:
                time.sleep(2)
        raise RuntimeError(f"{self.name}: no /api/info in {limit} s")

    def wait_reports(self, effect, after, n=2, limit=900):
        t0 = time.time()
        while time.time() - t0 < limit:
            key = effect.replace(" ", "_")   # the log names an effect by its id: spaces become _
            got = [r for r in self.reports()[after:] if r[0].replace(" ", "_") == key]
            if len(got) >= n:
                return got[n - 1]
            time.sleep(3)
        return None

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def run(name, port, out, results):
    # Everything that can fail, the Twin itself included, is inside the try: an error lands in
    # res["error"] and <name>.json instead of killing the thread with the column left blank.
    res, t = {"config": name, "flags": None, "scenes": [], "oceanarium": None}, None
    try:
        res["flags"] = flags(name)
        t = Twin(name, port, out)
        t.wait_up()
        # A fresh chip's defaults run the carousel (15 s a page), which would take the effect off
        # the screen before its 30-second report; the owner's panel has it off.
        t.post("/api/panel", {"carousel": {"enabled": False}})
        t.post("/api/panel", {"show": {"page": 0}}); time.sleep(2)
        effects = json.loads(t.get("/api/lua"))["effects"]
        if not any("KINETIC" in e for e in effects):
            print(f"[{name}] upload:", t.upload(SCRIPT, "KINETIC_DIGITS_LED")[:160], flush=True)
            effects = json.loads(t.get("/api/lua"))["effects"]
        kd = next(i for i, e in enumerate(effects) if "KINETIC" in e)
        oc = next((i for i, e in enumerate(effects) if "OCEANARIUM" in e), None)
        if oc is not None:
            t.post("/api/lua", {"show": oc})
            r = t.wait_reports("OCEANARIUM", len(t.reports()))
            res["oceanarium"] = r
            print(f"[{name}] OCEANARIUM {r}", flush=True)
            t.post("/api/panel", {"show": {"page": 0}}); time.sleep(2)
        t.post("/api/lua", {"show": kd}); time.sleep(3)
        for scene, *_ in PANEL:
            t.post("/api/lua", {"click": True}); time.sleep(1)
            r = t.wait_reports(effects[kd], len(t.reports()))
            res["scenes"].append([scene, r])
            print(f"[{name}] {scene}: {r}", flush=True)
            json.dump(res, open(os.path.join(out, f"{name}.json"), "w"), indent=1)
    except (Exception, SystemExit) as e:
        res["error"] = repr(e)
        print(f"[{name}] error {e!r}", flush=True)
    finally:
        json.dump(res, open(os.path.join(out, f"{name}.json"), "w"), indent=1)
        if t is not None:
            t.stop()
        results[name] = res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--configs", default="cpi1,cpi2,approx40,approx100")
    ap.add_argument("--out", default=os.path.join(HOME, "calibration"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = a.configs.split(",")
    for n in names:
        flags(n)   # a misspelt configuration stops here, before any emulator starts
    results, threads = {}, []
    for i, n in enumerate(names):
        th = threading.Thread(target=run, args=(n, 18080 + i, a.out, results))
        th.start(); threads.append(th); time.sleep(2)
    for th in threads:
        th.join()
    print("\nscene            panel ms " + " ".join(f"{n:>14}" for n in names))
    for k, (scene, pms, *_rest) in enumerate(PANEL):
        row = []
        for n in names:
            sc = results.get(n, {}).get("scenes", [])
            r = sc[k][1] if k < len(sc) else None
            row.append(f"{float(r[4]):6.1f} ({float(r[4]) / pms:4.2f})" if r else f"{'-':>14}")
        print(f"{scene:15} {pms:9.1f} " + " ".join(f"{c:>14}" for c in row))


if __name__ == "__main__":
    main()

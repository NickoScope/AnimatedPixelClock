#!/usr/bin/env python3
"""tools/agent/update.py's decisions, with no panel and no GitHub.

What must hold, because an update is the one thing in the SDK that can leave a
panel off the network:
  - the list of updates is every published release newer than what runs;
  - three questions, in order; any wrong answer ends the plan, nothing sent;
  - only the third right answer installs; a plan cannot be answered twice
    past its end, nor after ten minutes;
  - an image that fails its SHA-256 or its ESP32-S3 header is never sent;
  - the outcome read back is UPDATED, ROLLED BACK or NOT APPLIED as the panel
    says, never guessed.

    python3 tools/agent/tests/test_update_flow.py
"""
import hashlib, pathlib, sys, time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import panel as P    # noqa: E402
import update as U   # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what)
    FAILS += 0 if cond else 1


def image(chip=9):
    d = bytearray(b"\x00" * 64)
    d[0] = 0xE9
    d[12:14] = chip.to_bytes(2, "little")
    return bytes(d)


GOOD = image()
REL = [{"version": v, "tag": "v" + v, "date": "2026-09-2" + v[-1], "notes": f"notes of {v}",
        "url": "https://example/" + v, "asset": "img-" + v, "asset_name": f"OTA_ONLY_firmware-v{v}-waveshare.bin",
        "size": len(GOOD), "sums": "sums-" + v} for v in ("2.5.6", "2.5.5", "2.5.4", "2.5.3")]
INFO = {"version": "2.5.4", "deviceName": "Panel-01", "chip": "ESP32-S3", "mac": "AA:BB:CC:00:11:22",
        "ota": {"partition": "app0", "state": "undefined"}}
SENT = []


def fake_get_url(url, timeout=30):
    if url.startswith("img-"):
        return FAKE_IMAGE[0]
    if url.startswith("sums-"):
        v = url[5:]
        return f"{hashlib.sha256(GOOD).hexdigest()}  OTA_ONLY_firmware-v{v}-waveshare.bin\n".encode()
    raise AssertionError(url)


FAKE_IMAGE = [GOOD]
AFTER = [None]
U.releases = lambda: list(REL)
U._get = fake_get_url
P.resolve = lambda panel=None: {"address": "10.0.0.9", "mac": INFO["mac"], "name": INFO["deviceName"]}
P.get = lambda address, path, timeout=12.0, tries=6: dict(AFTER[0] if AFTER[0] and path == "/api/info" and SENT else INFO)
U.upload = lambda address, data, name, progress=None: (SENT.append(name) or (200, "OK"))
U.time.sleep = lambda s: None
quiet = lambda *a, **k: None

# the list
c = U.check()
check([u["version"] for u in c["updates"]] == ["2.5.5", "2.5.6"], "updates: every release newer than 2.5.4, oldest first")
check(c["latest"] == "2.5.6" and not c["up_to_date"], "latest and not up to date")

# three questions; a wrong one ends it
pl = U.plan()
check(pl["to"] == "2.5.6" and pl["ask"].startswith("1/3"), "plan: newest, first question")
try:
    U.answer(pl["token"], "sure", quiet); check(False, "a wrong first answer is refused")
except U.UpdateError:
    check(not SENT, "a wrong first answer: refused, nothing sent")
try:
    U.answer(pl["token"], "yes", quiet); check(False, "a refused plan is gone")
except U.UpdateError:
    check(True, "a refused plan cannot be continued")

pl = U.plan(version="2.5.5")
check(pl["to"] == "2.5.5", "a chosen version")
r = U.answer(pl["token"], "YES", quiet)
check(r["ask"].startswith("2/3") and not SENT, "answer 1 (any case): second question, nothing sent")
r = U.answer(pl["token"], "update", quiet)
check(r["ask"].startswith("3/3") and "Panel-01" in r["ask"] and not SENT, "answer 2: third question names the panel")
try:
    U.answer(pl["token"], "panel-01", quiet); check(False, "the name is exact")
except U.UpdateError:
    check(not SENT, "answer 3: the name must be exact, nothing sent")

# the whole way, and the outcomes
def run(after):
    SENT.clear()
    AFTER[0] = after
    pl = U.plan()
    U.answer(pl["token"], "yes", quiet)
    U.answer(pl["token"], "update", quiet)
    return U.answer(pl["token"], "Panel-01", quiet)

out = run({**INFO, "version": "2.5.6", "ota": {"partition": "app1", "state": "valid"}})
check(SENT == ["OTA_ONLY_firmware-v2.5.6-waveshare.bin"] and out["result"] == "UPDATED", "three right answers: sent once, UPDATED")
out = run({**INFO, "version": "2.5.4", "ota": {"partition": "app0", "state": "valid", "rolledBackFrom": "app1"}})
check(out["result"] == "ROLLED BACK" and "app1" in out["detail"], "old version and a refused partition: ROLLED BACK")
out = run({**INFO, "version": "2.5.4"})
check(out["result"] == "ROLLED BACK", "taken (200), restarted, old version: ROLLED BACK even with no refused partition shown")
SENT.clear()
U.upload = lambda address, data, name, progress=None: (SENT.append(name) or (500, "Update failed: No Space"))
AFTER[0] = None
pl = U.plan(); U.answer(pl["token"], "yes", quiet); U.answer(pl["token"], "update", quiet)
out = U.answer(pl["token"], "Panel-01", quiet)
check(out["result"] == "NOT APPLIED" and "No Space" in out["detail"], "the panel refuses the image: NOT APPLIED, its reason shown")

# images that must never be sent
for bad, why in ((image(chip=2), "an ESP32-S2 image"), (b"\x00" + GOOD[1:], "not an ESP image")):
    SENT.clear()
    FAKE_IMAGE[0] = bad
    try:
        U.fetch_image(REL[0]); check(False, why + " is refused")
    except U.UpdateError:
        check(not SENT, why + ": refused before sending")
FAKE_IMAGE[0] = GOOD

# a plan expires
pl = U.plan()
U._PLANS[pl["token"]]["at"] -= U.TOKEN_LIFE_S + 1
try:
    U.answer(pl["token"], "yes", quiet); check(False, "an old plan is refused")
except U.UpdateError:
    check(True, "a plan older than ten minutes is refused")

print(f"\n{'all passed' if not FAILS else str(FAILS) + ' failed'}")
sys.exit(1 if FAILS else 0)

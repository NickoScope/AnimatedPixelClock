#!/usr/bin/env python3
"""Firmware updates over the air: what is new, and installing it - with three
confirmations from the person the panel belongs to, and an honest outcome.

    python3 tools/agent/update.py --panel <name|MAC|IP>            # check: what is new
    python3 tools/agent/update.py --panel <name|MAC|IP> --install  # the three questions, then the update

Where updates come from: the GitHub Releases of NickoScope/AnimatedPixelClock,
the same releases the web flasher is published with. Each carries
OTA_ONLY_firmware-v<ver>-waveshare.bin and SHA256SUMS.txt; the release notes
are the list of what changed. Only the Waveshare ESP32-S3-RGB-Matrix image is
published, so only an ESP32-S3 panel is offered one.

Before anything is sent the image is checked: its SHA-256 against the
release's SHA256SUMS.txt, and its header - the ESP image magic 0xE9 and chip id
9, ESP32-S3 (ESP-IDF esp_app_format.h, ESP_CHIP_ID_ESP32S3). A mismatch stops
the update; nothing reaches the panel.

The three confirmations (owner, 2026-09-23). Updating is a person's decision,
taken with the panel in front of them, so the tool asks three times and each
answer has to come from that person:
  1. the plan - which panel, from which version to which, what changed;  "yes"
  2. what happens - the panel restarts and is off the network for about two
     minutes; how a failed update is undone;                             "update"
  3. the panel's own name, typed - so the wrong panel cannot be updated by
     a slip of the finger.
The MCP tool (panel_update) takes the same three steps, one call each, and
each answer must be the person's words relayed, never the agent's own. Said
plainly: the tool asks three times and relies on the agent relaying the
person's answers - it cannot tell who typed them. A real witness would be a
confirmation on the panel itself (the knob, the remote): not built yet.

The outcome, read off the panel after it comes back (src/health/boot_health.cpp):
the new firmware confirms itself only after a minute on the network with
frames drawn. If it crashes or hangs before that, the watchdog resets it and
the bootloader rolls back to the previous image. So the result is one of:
  UPDATED       the new version runs and has confirmed itself;
  ROLLED BACK   the panel took the image, restarted, and came back on the
                previous version: the new one did not prove itself;
  NOT APPLIED   the panel refused the image before restarting (its answer is
                shown) and kept the version it had;
  PENDING       the new version runs but has not confirmed itself yet when the
                tool stopped waiting - check again in a minute;
  UPDATED, NOT CONFIRMED   the new version runs with no OTA state to confirm
                (not expected on this build: rollback is on);
  INTERRUPTED   the upload broke off; power-cycle and check;
  NOT BACK      the panel did not return to the network. Power-cycle it; if it
                still does not come back, reflash over USB from the web flasher.
"""
import argparse, hashlib, http.client, json, re, secrets, sys, threading, time, urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import panel as P  # noqa: E402

REPO = "NickoScope/AnimatedPixelClock"
RELEASES = f"https://api.github.com/repos/{REPO}/releases?per_page=100"
ASSET_PREFIX = f"https://github.com/{REPO}/releases/download/"
BOARD_ASSET = "OTA_ONLY_firmware-{tag}-waveshare.bin"
ESP_IMAGE_MAGIC = 0xE9
ESP_CHIP_ID_ESP32S3 = 9          # esp_app_format.h
CONFIRM_1, CONFIRM_2 = "yes", "update"
TOKEN_LIFE_S = 600               # a plan older than ten minutes is asked for again
BACK_WAIT_S = 240                # how long to wait for the panel to answer again
CONFIRM_WAIT_S = 180             # and then for the new image to confirm itself
WATCH_LIMIT_S = BACK_WAIT_S + CONFIRM_WAIT_S + 60   # the whole watch, whatever happens
MAX_IMAGE = 8 * 1024 * 1024      # no app image of this board comes near it
MAX_SUMS = 64 * 1024


class UpdateError(Exception):
    pass


def _ver(tag):
    """(2, 5, 4) from "v2.5.4" or "2.5.4"; () when it is not a version."""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", tag or "")
    return tuple(int(x) for x in m.groups()) if m else ()


def _get(url, timeout=30, limit=4 * 1024 * 1024):
    req = urllib.request.Request(url, headers={"User-Agent": "ledmatrix-sdk",
                                               "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        if not r.geturl().startswith("https://"):
            raise UpdateError(f"refused a non-HTTPS redirect to {r.geturl()}")
        data = r.read(limit + 1)
    if len(data) > limit:
        raise UpdateError(f"{url} is larger than {limit} bytes: refused")
    return data


def releases():
    """Published releases of this fork that carry the board's OTA image,
    newest first."""
    out = []
    for r in json.loads(_get(RELEASES)):
        if r.get("draft") or r.get("prerelease") or not _ver(r.get("tag_name")):
            continue
        tag = r["tag_name"]
        assets = {a["name"]: a for a in r.get("assets") or []}
        ota = assets.get(BOARD_ASSET.format(tag=tag))
        if not ota or not ota.get("browser_download_url", "").startswith(ASSET_PREFIX):
            continue                       # only this fork's own release downloads
        out.append({"version": tag.lstrip("v"), "tag": tag, "date": (r.get("published_at") or "")[:10],
                    "notes": r.get("body") or "", "url": r.get("html_url"),
                    "asset": ota["browser_download_url"], "asset_name": ota["name"], "size": ota["size"],
                    "sums": (assets.get("SHA256SUMS.txt") or {}).get("browser_download_url")})
    out.sort(key=lambda x: _ver(x["tag"]), reverse=True)
    return out


def check(panel=None):
    """What the panel runs, and every published release newer than that."""
    p = P.resolve(panel)
    info = P.get(p["address"], "/api/info")
    have = info.get("version") or "?"
    rel = releases()
    newer = [r for r in rel if _ver(r["tag"]) > _ver(have)]
    return {"panel": {"name": info.get("deviceName") or p.get("name"), "address": p["address"],
                      "mac": p.get("mac") or info.get("mac"), "chip": info.get("chip")},
            "current": have, "ota": info.get("ota"),
            "latest": rel[0]["version"] if rel else None,
            "up_to_date": not newer,
            "updates": [{k: r[k] for k in ("version", "date", "notes", "url", "size")} for r in reversed(newer)]}


def fetch_image(rel):
    """The image, checked: SHA-256 against the release's list, header against
    an ESP32-S3 application image."""
    data = _get(rel["asset"], timeout=120, limit=MAX_IMAGE)
    if len(data) < 24:
        raise UpdateError(f"the download is {len(data)} bytes: not an image")
    if len(data) != rel["size"]:
        raise UpdateError(f"download is {len(data)} bytes, the release says {rel['size']}")
    if not rel.get("sums"):
        raise UpdateError("the release has no SHA256SUMS.txt, so the image cannot be checked")
    sums = {}
    for line in _get(rel["sums"], limit=MAX_SUMS).decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) == 2:
            sums[parts[1].lstrip("*")] = parts[0].lower()
    want = sums.get(rel["asset_name"])
    got = hashlib.sha256(data).hexdigest()
    if not want:
        raise UpdateError(f"{rel['asset_name']} is not in SHA256SUMS.txt")
    if got != want:
        raise UpdateError(f"SHA-256 mismatch: got {got}, the release says {want}")
    if data[0] != ESP_IMAGE_MAGIC:
        raise UpdateError(f"not an ESP application image (first byte 0x{data[0]:02X})")
    chip = int.from_bytes(data[12:14], "little")
    if chip != ESP_CHIP_ID_ESP32S3:
        raise UpdateError(f"the image is for chip id {chip}, not the ESP32-S3 (9)")
    return data, got


def upload(address, data, name, progress=None):
    """POST the image to /update, as the portal's Firmware card does, with no
    retry: a half-sent image is not resumed, it is sent again by a person."""
    boundary = "----ledmatrixota" + secrets.token_hex(8)
    head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"firmware\"; "
            f"filename=\"{name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    host, _, port = address.partition(":")
    conn = http.client.HTTPConnection(host, int(port or 80), timeout=300)
    conn.putrequest("POST", "/update")
    conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
    conn.putheader("Content-Length", str(len(head) + len(data) + len(tail)))
    conn.endheaders()
    conn.send(head)
    step = 16384
    for i in range(0, len(data), step):
        conn.send(data[i:i + step])
        if progress:
            progress(min(i + step, len(data)), len(data))
    conn.send(tail)
    r = conn.getresponse()
    body = r.read().decode("utf-8", "replace")
    conn.close()
    return r.status, body


def identify(address):
    """/api/info at an address, or None."""
    try:
        return P.get(address, "/api/info", timeout=4.0, tries=2)
    except Exception:  # noqa: BLE001
        return None


def same_panel(info, who):
    """Is this /api/info the panel the person named? By MAC when the plan has
    one (the name and the address both move: panel.py), and by name too."""
    if not info:
        return False
    if who.get("mac") and (info.get("mac") or "").lower() != who["mac"].lower():
        return False
    return (info.get("deviceName") or "") == who["name"]


def find_again(address, who):
    """The panel's current address: the old one if it is still the same panel,
    else looked up again by MAC (DHCP may hand out a new one after a restart)."""
    info = identify(address)
    if same_panel(info, who):
        return address, info
    if who.get("mac"):
        try:
            q = P.resolve(who["mac"], refresh=True)
            info = identify(q["address"])
            if same_panel(info, who):
                return q["address"], info
        except Exception:  # noqa: BLE001
            pass
    return address, None


def watch(address, who, target, before, sent_at, say=print):
    """After the upload: wait for the panel - the same panel, by MAC and name -
    and read what it runs. One loop with one deadline; a panel that drops out
    again while it is still unconfirmed is waited for, within that deadline."""
    t0 = time.monotonic()
    time.sleep(5)                     # /update answers 200, waits a second, restarts
    info, back_at = None, None
    while time.monotonic() - t0 < WATCH_LIMIT_S:
        address, now = find_again(address, who)
        if now is None:
            if back_at is None and time.monotonic() - t0 > BACK_WAIT_S:
                break
            time.sleep(3)
            continue
        # Came back only if it has restarted since the image was taken: its
        # uptime is shorter than the time since the 200 (plus request slack).
        if back_at is None:
            if (now.get("uptime") or 1e9) > time.monotonic() - sent_at + 5:
                time.sleep(3)
                continue                  # still the old run, not yet restarted
            back_at = time.monotonic()
            say(f"back on the network after {back_at - t0:.0f} s at {address}, running {now.get('version')}")
        info = now
        ver = info.get("version")
        if not _ver(ver):
            return {"result": "UNKNOWN", "detail": f"the panel reports no version ({ver!r})", "info": info}
        if _ver(ver) != _ver(target):
            # The panel took the image (/update answers 200 only after
            # Update.end() set the new boot partition) and restarted: coming
            # back on the old version is a rollback, whether the new image
            # crashed or never proved itself.
            ota = info.get("ota") or {}
            where = (f" (last partition on record as rolled back: {ota['rolledBackFrom']})"
                     if ota.get("rolledBackFrom") else "")
            return {"result": "ROLLED BACK", "detail": f"{target} did not prove itself, so the "
                    f"bootloader put {ver} back{where}.", "info": info}
        state = (info.get("ota") or {}).get("state")
        if state == "valid":
            return {"result": "UPDATED", "detail": f"{before} -> {ver}, confirmed by the panel "
                    f"(OTA state valid, partition {info['ota'].get('partition')})", "info": info}
        if state not in ("pending", "new"):
            # An OTA image is never "undefined" on this build (rollback is on):
            # say what was seen rather than call it confirmed.
            return {"result": "UPDATED, NOT CONFIRMED", "detail": f"{ver} runs, but its OTA state is "
                    f"{state!r}, so the panel cannot say it proved itself or roll it back", "info": info}
        if time.monotonic() - back_at > CONFIRM_WAIT_S:
            return {"result": "PENDING", "detail": f"{ver} runs but had not confirmed itself after "
                    f"{CONFIRM_WAIT_S} s; run the check again in a minute", "info": info}
        say(f"running {ver}, waiting for it to confirm itself (state {state})")
        time.sleep(10)
    if info is None:
        return {"result": "NOT BACK", "detail": f"no answer from the panel for {BACK_WAIT_S} s. "
                "Power-cycle it; if it still does not come back, reflash over USB from the web flasher."}
    return {"result": "PENDING", "detail": "the watch ran out of time; run the check again", "info": info}


# --- the three confirmations, for the MCP tool ------------------------------
# plan() makes the plan and asks the first question; answer() takes the
# person's reply to the current question. Three right answers, in order,
# install; any wrong one ends the plan with nothing sent.
_PLANS = {}
_LOCK = threading.Lock()
_INFLIGHT = set()     # MACs (or addresses) with an update being sent or watched


def _questions(p):
    return [
        (f"1/3 Update {p['name']} ({p['panel']['address']}) from {p['from']} to {p['rel']['version']}? "
         f"The person answers: {CONFIRM_1}", CONFIRM_1),
        (f"2/3 The panel restarts and is off the network for about two minutes. If the new "
         f"version fails to prove itself it goes back to {p['from']} by itself. The person "
         f"answers: {CONFIRM_2}", CONFIRM_2),
        (f"3/3 The person types the panel's name to start: {p['name']}", p["name"]),
    ]


def plan(panel=None, version=None):
    """The plan and the first question, or "up to date"."""
    c = check(panel)
    if not c["updates"]:
        return {"up_to_date": True, **c}
    rel = [r for r in releases() if _ver(r["tag"]) > _ver(c["current"])]
    target = next((r for r in rel if not version or _ver(r["tag"]) == _ver(version)), None)
    if not target:
        raise UpdateError(f"no published release {version} newer than {c['current']}")
    if (c["panel"].get("chip") or "") != "ESP32-S3":
        raise UpdateError(f"the published image is for the ESP32-S3; this panel reports {c['panel'].get('chip')}")
    key = c["panel"].get("mac") or c["panel"]["address"]
    token = secrets.token_hex(6)
    p = {"asked": 0, "at": time.time(), "panel": c["panel"], "from": c["current"],
         "rel": target, "name": c["panel"]["name"], "key": key}
    with _LOCK:
        if key in _INFLIGHT:
            raise UpdateError("an update of this panel is already running")
        for t in [t for t, x in _PLANS.items() if time.time() - x["at"] > TOKEN_LIFE_S]:
            _PLANS.pop(t, None)
        _PLANS[token] = p
    return {"token": token, "panel": c["panel"], "from": c["current"], "to": target["version"],
            "changes": c["updates"], "ask": _questions(p)[0][0]}


def answer(token, reply, say=print):
    """The person's reply to the question the plan is on. After the third right
    one, the update runs and its outcome is returned."""
    with _LOCK:                       # one answer at a time: no double install
        p = _PLANS.get(token)
        if not p:
            raise UpdateError("no such plan: start again")
        if time.time() - p["at"] > TOKEN_LIFE_S:
            _PLANS.pop(token, None)
            raise UpdateError("the plan is older than ten minutes: start again")
        qs = _questions(p)
        want = qs[p["asked"]][1]
        got = (reply or "").strip()
        ok = got == want if p["asked"] == 2 else got.lower() == want
        if not ok:
            _PLANS.pop(token, None)
            raise UpdateError(f"answer {p['asked'] + 1} was not '{want}': nothing was sent")
        p["asked"] += 1
        if p["asked"] < 3:
            return {"token": token, "ask": qs[p["asked"]][0]}
        _PLANS.pop(token, None)
        if p["key"] in _INFLIGHT:
            raise UpdateError("an update of this panel is already running")
        _INFLIGHT.add(p["key"])
    try:
        return install(p["panel"], p["rel"], p["from"], say)
    finally:
        with _LOCK:
            _INFLIGHT.discard(p["key"])


def install(who, rel, before, say=print):
    """who: {"address", "mac", "name"} as the person confirmed it."""
    say(f"downloading {rel['asset_name']} ({rel['size']} bytes)")
    data, sha = fetch_image(rel)
    say(f"image checked: SHA-256 {sha[:16]}..., ESP32-S3 application")
    # The panel the image goes to is the one the person named, checked now,
    # not the address remembered when the plan was made: names and addresses
    # both move on this network.
    address, info = find_again(who["address"], who)
    if info is None:
        raise UpdateError(f"{who['name']} does not answer as itself (by MAC and name) now: nothing was sent")
    if _ver(info.get("version")) != _ver(before):
        raise UpdateError(f"{who['name']} now runs {info.get('version')}, not {before}: nothing was sent")
    last = [0]
    def prog(done, total):
        pct = done * 100 // total
        if pct >= last[0] + 10 or done == total:
            last[0] = pct
            say(f"sent {pct}%")
    try:
        status, body = upload(address, data, rel["asset_name"], prog)
    except OSError as e:
        # Mid-upload, or the answer lost after Update.end(): nobody can say
        # which. The firmware keeps an interrupted upload "running" until it
        # restarts, so a second try would be refused anyway.
        return {"result": "INTERRUPTED", "detail": f"the upload stopped ({e}). The panel may or may not "
                "have taken the image. Power-cycle the panel, then run the check: it shows which "
                "version runs."}
    sent_at = time.monotonic()
    if status != 200:
        return {"result": "NOT APPLIED", "detail": f"the panel answered {status}: {body.strip()[:300]}"}
    say("the panel took the image and is restarting")
    return watch(address, who, rel["version"], before, sent_at, say)


def main():
    ap = argparse.ArgumentParser(description="Check for firmware updates, and install one over the air.")
    ap.add_argument("--panel", help="MAC, name or address; default: the only panel")
    ap.add_argument("--install", action="store_true", help="ask the three questions, then update")
    ap.add_argument("--version", help="a specific published version instead of the newest")
    a = ap.parse_args()
    try:
        c = check(a.panel)
    except P.ChoiceNeeded as e:
        print("More than one panel is on this network; name one with --panel:")
        for x in e.panels:
            print(f"  {x.get('name') or '?':34s} {x.get('mac') or '?':17s} {x['address']}")
        sys.exit(3)
    except (P.PanelError, UpdateError, OSError) as e:
        sys.exit(f"Cannot check: {e}")
    print(f"{c['panel']['name']} ({c['panel']['address']}) runs {c['current']}; "
          f"newest published: {c['latest'] or 'none'}")
    if c["up_to_date"]:
        print("Up to date.")
        return
    for u in c["updates"]:
        print(f"\n== {u['version']} ({u['date']}) {u['url']}\n{u['notes'].strip()}")
    if not a.install:
        print("\nTo install: add --install")
        return
    rels = [r for r in releases() if _ver(r["tag"]) > _ver(c["current"])]
    rel = next((r for r in rels if not a.version or _ver(r["tag"]) == _ver(a.version)), None)
    if not rel:
        sys.exit(f"no published release {a.version} newer than {c['current']}")
    if (c["panel"].get("chip") or "") != "ESP32-S3":
        sys.exit(f"the published image is for the ESP32-S3; this panel reports {c['panel'].get('chip')}")
    name = c["panel"]["name"]
    print()
    if input(f"1/3  Update {name} from {c['current']} to {rel['version']}? Type '{CONFIRM_1}': ").strip().lower() != CONFIRM_1:
        sys.exit("Not confirmed. Nothing was sent.")
    if input(f"2/3  The panel restarts and is off the network for about two minutes. If the new version "
             f"fails to prove itself it goes back to {c['current']} by itself. Type '{CONFIRM_2}': "
             ).strip().lower() != CONFIRM_2:
        sys.exit("Not confirmed. Nothing was sent.")
    if input(f"3/3  Type the panel's name to start ({name}): ").strip() != name:
        sys.exit("That is not the panel's name. Nothing was sent.")
    try:
        out = install(c["panel"], rel, c["current"])
    except (UpdateError, OSError) as e:
        sys.exit(f"Stopped before sending anything: {e}")
    print(f"\n{out['result']}: {out['detail']}")
    sys.exit(0 if out["result"] == "UPDATED" else 1)


if __name__ == "__main__":
    main()

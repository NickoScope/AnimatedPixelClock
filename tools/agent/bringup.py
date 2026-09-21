#!/usr/bin/env python3
"""Bring a new panel to life: give it a name, and switch off what it cannot do.

Two things are true of every panel arriving fresh, and this script exists so
neither is left to memory.

**It has no name of its own.** A freshly flashed panel calls itself whatever the
build's default was, and so does the next one. With two on a network the mDNS
names collide and the only thing telling them apart is the MAC. So a panel is
named before anything else is done with it, and the name is *asked for* - never
invented here, because it is what a person will type for the rest of the panel's
life.

**It may have no Home Assistant behind it.** Four of the sixteen pages have no
other source than MQTT: cards, media, and the market pages. Two more - flights
and trains - work either through Home Assistant or through their own API key. A
panel with no broker shows those pages empty and the carousel dwells on them for
fifteen seconds each. So: probe, and switch off what has no source. Nothing is
turned off that has a direct key of its own.

**And one case it refuses to decide.** A broker that is *configured but not
connected* is not a panel without Home Assistant - it is a broker that is down,
restarting, or slower to connect than the panel was to boot. Switching pages off
over that turns a two-minute outage into a setting somebody has to find and
undo. That case is reported and left alone; --force-no-ha if the broker really
is gone for good.

    python3 bringup.py                          # what is here, and what it needs
    python3 bringup.py --mac <MAC> --name X     # name it, then settle the pages
    python3 bringup.py --mac <MAC> --check      # read-only: say what would change
    python3 bringup.py --mac <MAC> --name X --keep-ha   # leave the pages alone
    python3 bringup.py --mac <MAC> --name X --force-no-ha  # broker configured but gone

Exit codes match the rest of the agent tooling: 0 done, 2 bad arguments, 3 a
person must decide (no name given, or several panels and none chosen), 4 the
panel could not be reached.
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discover import discover  # noqa: E402

NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9-]{0,30}$")

# Pages whose only source is MQTT. Turning these off on a panel with no broker
# is not a preference - there is nothing behind them. Keys are the ones
# /api/panel's `enable` accepts (src/web/web_panel.cpp:199-200).
MQTT_ONLY = {
    "cards":  "cards arrive on nickoscope_matrix/card/+ and nowhere else",
    "media":  "the player is followed over MQTT; there is no direct path",
    "market": "quotes arrive on nickoscope_matrix/<dev>/market/# and nowhere else",
}

# Pages that work either way. Each names the /api/<route> to ask, and the path
# through its JSON to the flag that says a direct key is present.
DUAL = {
    "flights": ("flightboard", ("direct", "key")),
    "trains":  ("railboard",   ("direct", "token")),
}


def _get(host, path, timeout=12.0, tries=5):
    """The panel answers 503 while it is busy; that is its back-off working."""
    last = 0
    for _ in range(tries):
        try:
            with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
                return r.status, json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            last = e.code
            if e.code != 503:
                return e.code, None
        except Exception:
            last = 0
        time.sleep(1.5)
    return last, None


def _post(host, path, payload, timeout=12.0, tries=5):
    last = 0
    for _ in range(tries):
        req = urllib.request.Request(
            f"http://{host}{path}", data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                body = r.read().decode("utf-8", "replace")
                try:
                    return r.status, json.loads(body)
                except Exception:
                    return r.status, None
        except urllib.error.HTTPError as e:
            last = e.code
            if e.code != 503:
                return e.code, None
        except Exception:
            last = 0
        time.sleep(1.5)
    return last, None


def probe(host):
    """Everything the decisions below rest on, read once."""
    _, info = _get(host, "/api/info")
    if not info:
        return None
    st, panel = _get(host, "/api/panel")
    out = {"info": info, "panel": panel, "mqtt": None, "direct": {}}
    # Three routes carry the same mqtt{configured,connected,status}; whichever
    # is built answers. Asking one is enough, but a panel built without the
    # flight board still has to be readable, so try in order.
    for route in ("flightboard", "railboard", "media"):
        st, d = _get(host, f"/api/{route}", tries=2)
        if d and isinstance(d.get("mqtt"), dict):
            out["mqtt"] = d["mqtt"]
            break
    for key, (route, path) in DUAL.items():
        st, d = _get(host, f"/api/{route}", tries=2)
        if not d:
            continue
        node = d
        for step in path:
            node = (node or {}).get(step) if isinstance(node, dict) else None
        out["direct"][key] = bool(node)
    return out


def page_state(panel):
    """key -> {on, name, names, i} for the pages /api/panel reports.

    **Several pages can share one key**, and on a full build four of them do:
    MARKETS, TICKER, PORTFOLIO and HOLDINGS are all `market`. The firmware
    stores one enable BIT per key, not per page (src/panel/panel.cpp:414), so
    switching off `market` switches off all four - which is right, and is why
    this keeps every name rather than the last one to be read. Reporting one
    name for four pages tells a person three lies about their own panel.
    """
    out = {}
    for p in (panel or {}).get("pages", []):
        st = out.setdefault(p.get("key"),
                            {"on": False, "name": p.get("name"), "names": [], "i": p.get("i")})
        st["names"].append(p.get("name"))
        if p.get("on"):
            st["on"] = True
    for st in out.values():
        st["name"] = ", ".join(n for n in st["names"] if n)
    return out


def plan_ha(pr):
    """What to switch off, and the reason for each. Empty when HA is there.

    Returns (todo, why, blocked). `blocked` is the case this deliberately will
    NOT act on: a broker that is configured but not connected right now. That
    is not "no Home Assistant" - it is a broker that is down, restarting, or
    simply slower to connect than a panel is to boot, and switching four pages
    off over it would turn a two-minute outage into a setting a person has to
    find and undo. Say it, and let them decide.
    """
    mqtt = pr.get("mqtt") or {}
    if mqtt.get("connected"):
        return [], "MQTT connected: nothing to switch off", False
    blocked = bool(mqtt.get("configured"))
    why = ("no broker is configured" if not blocked
           else f"a broker IS configured but is not connected right now ({mqtt.get('status')})")
    pages = page_state(pr.get("panel"))
    todo = []
    for key, reason in MQTT_ONLY.items():
        st = pages.get(key)
        if st is None:
            continue          # not in this build
        if st["on"]:
            todo.append((key, st["name"], reason))
    for key, _ in DUAL.items():
        st = pages.get(key)
        if st is None or not st["on"]:
            continue
        if pr["direct"].get(key):
            continue          # it has its own key: leave it
        todo.append((key, st["name"], "neither MQTT nor a direct key of its own"))
    return todo, why, blocked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mac", help="which panel; required when there are several")
    ap.add_argument("--host", help="skip discovery and talk to this address")
    ap.add_argument("--name", help="the name to give it (letters, digits, hyphen; starts with a letter; <=31)")
    ap.add_argument("--check", action="store_true", help="read-only: say what would change")
    ap.add_argument("--keep-ha", action="store_true", help="do not switch off the MQTT pages")
    ap.add_argument("--force-no-ha", action="store_true",
                    help="switch the pages off even though a broker is configured but "
                         "not connected. Without this, that case is left to a person")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    warnings, actions = [], []

    def out(code, ok, error=None, **result):
        if args.json:
            print(json.dumps({"schema_version": 1, "ok": ok, "code": code,
                              "result": result, "error": error,
                              "warnings": warnings}, ensure_ascii=False))
        return result

    # --- which panel -----------------------------------------------------
    if args.host:
        host, mac = args.host, None
    else:
        panels = [p for p in discover() if p.get("reachable")]
        if not panels:
            if not args.json:
                print("панелей в сети не видно", file=sys.stderr)
            out("none_found", False, "no panel answered")
            return 4
        if args.mac:
            want = args.mac.lower().replace("-", ":")
            panels = [p for p in panels if (p.get("mac") or "").lower() == want]
            if not panels:
                out("none_found", False, f"no panel with MAC {args.mac}")
                if not args.json:
                    print(f"панели с MAC {args.mac} нет", file=sys.stderr)
                return 4
        if len(panels) > 1:
            if not args.json:
                print("панелей несколько, назовите одну через --mac:", file=sys.stderr)
                for p in panels:
                    print(f"  {p['mac']}  {p['address']}  {p['name']}", file=sys.stderr)
            out("choose", False, "several panels and none chosen",
                panels=[{"mac": p["mac"], "address": p["address"], "name": p["name"]} for p in panels])
            return 3
        host, mac = panels[0]["address"], panels[0]["mac"]

    pr = probe(host)
    if not pr:
        out("unreachable", False, f"{host} did not answer /api/info")
        if not args.json:
            print(f"{host} не отвечает", file=sys.stderr)
        return 4

    info = pr["info"]
    current = info.get("deviceName")
    mac = mac or info.get("mac")
    mqtt = pr.get("mqtt") or {}

    if not args.json:
        print(f"панель  {mac}  {host}")
        print(f"  имя сейчас    {current}")
        print(f"  прошивка      {info.get('version')}  build {info.get('build')}")
        print(f"  MQTT          configured={mqtt.get('configured')} "
              f"connected={mqtt.get('connected')} ({mqtt.get('status')})")

    # --- the name --------------------------------------------------------
    # Asked for, never invented. A panel keeps this name for the rest of its
    # life and a person types it; that is not a decision for a script.
    renamed = False
    if not args.name:
        if not args.json:
            print("\n  ИМЯ. Новой панели нужно имя - своё, не общее с соседней.")
            print("  Повторите вызов с --name <имя>:")
            print("    буквы, цифры и дефис, начинается с буквы, до 31 знака")
            print(f"    сейчас: {current}")
        todo, why, blocked = plan_ha(pr)
        out("name_needed", False, "a name is required and was not given",
            mac=mac, address=host, current_name=current, mqtt=mqtt,
            would_disable=[t[0] for t in todo], ha_reason=why,
            ha_decision_is_a_persons=blocked)
        return 3

    if not NAME_RE.match(args.name):
        out("bad_args", False, "name must be letters, digits and hyphens, start with a letter, <=31")
        if not args.json:
            print("имя: буквы, цифры, дефис; начинается с буквы; не длиннее 31", file=sys.stderr)
        return 2

    if args.name == current:
        actions.append(f"имя уже {current}, не трогали")
    elif args.check:
        actions.append(f"переименовал бы {current} -> {args.name}")
    else:
        st, body = _post(host, "/api/rename", {"name": args.name})
        if st != 200 or not (body or {}).get("success"):
            out("rename_failed", False, f"/api/rename answered {st}", mac=mac, address=host)
            if not args.json:
                print(f"переименование не прошло: HTTP {st}", file=sys.stderr)
            return 1
        # /api/rename saves and restarts mDNS without a reboot, but the name
        # takes a moment to propagate. Read it back from the panel itself.
        time.sleep(2.0)
        _, again = _get(host, "/api/info")
        got = (again or {}).get("deviceName")
        if got != args.name:
            warnings.append(f"панель ответила успехом, но отдаёт имя {got!r}")
        else:
            renamed = True
        actions.append(f"переименована {current} -> {args.name}")

    # --- Home Assistant --------------------------------------------------
    todo, why, blocked = plan_ha(pr)
    if args.keep_ha:
        if todo:
            actions.append(f"страницы оставлены как есть по --keep-ha ({why})")
    elif blocked and todo and not args.force_no_ha:
        # A broker that is down is not a panel without Home Assistant. Refuse,
        # and name both ways out.
        actions.append(f"страницы НЕ тронуты: {why}")
        actions.append("  это похоже на упавший или ещё не поднявшийся брокер, а не на "
                       "панель без Home Assistant")
        actions.append("  почините брокер и запустите снова - или, если Home Assistant "
                       "тут действительно больше нет, повторите с --force-no-ha")
        warnings.append("решение о страницах оставлено человеку: брокер настроен, но не на связи")
    elif not todo:
        actions.append(why)
    else:
        for key, name, reason in todo:
            if args.check:
                actions.append(f"выключил бы {name} ({key}): {reason}")
                continue
            st, body = _post(host, "/api/panel", {"enable": {"key": key, "on": False}})
            ok = st == 200
            # /api/panel returns the new state in the same answer: compare it
            # rather than trusting the code. A 200 here can mean nothing at all
            # happened - see AGENTS.md section 3.
            if ok and body:
                after = page_state(body).get(key)
                ok = after is not None and after["on"] is False
            if ok:
                actions.append(f"выключена {name} ({key}): {reason}")
            else:
                warnings.append(f"{name} ({key}) выключить не удалось: HTTP {st}")

    if not args.json:
        print()
        for a in actions:
            print(f"  · {a}")
        for w in warnings:
            print(f"  ! {w}")
        if renamed:
            print(f"\n  теперь ищется так:  python3 discover.py --mac {mac}")

    out("ok", not warnings, None, mac=mac, address=host,
        name=args.name, renamed=renamed, actions=actions)
    return 0 if not warnings else 1


if __name__ == "__main__":
    sys.exit(main())

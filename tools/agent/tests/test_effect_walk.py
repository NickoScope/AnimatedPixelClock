#!/usr/bin/env python3
"""effect_walk, the MCP tool, against a stand-in panel.

Checks what it sends and what it says, without a panel: an index or a name
(case and underscores do not matter) becomes {walk:{i,name,on}}; an unknown
name is refused before anything is sent; a firmware older than 2.5.7 is told
apart; a 409 (the list moved) comes back as the panel's own sentence.

Needs the agent's venv (it imports the MCP server):

    tools/agent/.venv/bin/python tools/agent/tests/test_effect_walk.py
"""
import asyncio, json, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import mcp_server as M   # noqa: E402

LUA = {"success": True, "current": -1, "effects": ["FOOTBALL CLOCK", "AQUARIUM", "AUTUMN LEAVES"],
       "inWalk": [True, True, True]}
sent = []


def fake_get(a, path, **k):
    return dict(LUA)


def fake_post(a, path, body, **k):
    sent.append(body)
    w = body["walk"]
    if w["name"] != LUA["effects"][w["i"]]:
        raise M.P.PanelError("walk.name is not effect i any more: reload the list")
    d = dict(LUA)
    d["inWalk"] = list(LUA["inWalk"])
    d["inWalk"][w["i"]] = w["on"]
    return d


M._pick = lambda panel: {"address": "stub"}
M.P.get, M.P.post = fake_get, fake_post
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += 0 if cond else 1


def run(**kw):
    return asyncio.run(M.effect_walk(M.WalkIn(**kw)))


r = json.loads(run(effect=1, on=False))
check(sent[-1] == {"walk": {"i": 1, "name": "AQUARIUM", "on": False}}, "an index posts {walk:{i,name,on}}")
check(r == {"ok": True, "effect": "AQUARIUM", "inWalk": False}, "and answers with the effect and its state")
r = json.loads(run(effect="autumn_leaves", on=True))
check(sent[-1]["walk"]["i"] == 2 and r["effect"] == "AUTUMN LEAVES", "a name finds it (case, underscores)")
n = len(sent)
r = run(effect="nope", on=True)
check(len(sent) == n and "No effect 'nope'" in r and "AQUARIUM" in r, "an unknown name is refused before anything is sent")
r = run(effect=7, on=True)
check(len(sent) == n and "No effect 7" in r, "an index out of range too")
LUA.pop("inWalk")
r = run(effect=0, on=True)
check("2.5.7" in r and len(sent) == n, "firmware without the switch: said, nothing sent")
LUA["inWalk"] = [True, True, True]
orig = M.P.get
M.P.get = lambda a, p, **k: {**LUA, "effects": ["FOOTBALL CLOCK", "AUTUMN LEAVES", "AQUARIUM"]}
r = run(effect=1, on=False)
check("reload the list" in r, "the list moved in between: the panel's 409 comes back as a sentence")
M.P.get = orig
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)

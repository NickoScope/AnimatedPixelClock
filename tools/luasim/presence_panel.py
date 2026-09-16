#!/usr/bin/env python3
"""Render what the PANEL will show from a recorded presence session.

presence_feed.py answers a different question. It holds a sample until the next
one arrives, which is what a Home Assistant state does, and it was the right
tool for choosing the scale. The panel does not behave that way: src/presence
ages targets out, smooths the 1 Hz jitter, and says so when the feed stops.
This renders those rules, so the behaviour can be judged before any firmware is
believed.

    python3 tools/luasim/presence_panel.py --frames <session.json> \\
            --from 740 --to 780 --name walk --out gifs/presence-panel

Unlike presence_feed.py this does NOT rewrite the scene's scripted reads.
room_radar.lua asks a binding called `presence` for its people now, so the stub
below IS that binding, filled in from the recording, and the scene that renders
is the one that ships - character for character, apart from the window length.
What you see here is what src/presence has to reproduce.

The recording is a log of where people were in a room. It is read, never
copied: the generated Lua, the frames and the GIFs go to an ignored directory
(gifs/ is one) and nothing from it reaches the repository.

The rules, which src/presence/presence_model.h implements again in C++ and
tools/presence/presence_host_test.cpp checks against these same numbers:

  the newest message decides.  A slot the latest message reports as null is
            empty at once. Absence is something the publisher states, not
            something to be inferred from silence, and the contract sends a
            message every 5 s while the room is empty - so waiting out a 5 s
            age limit would race that heartbeat exactly.
  FRESH_MS  when messages STOP, the targets in the last one age out after this.
            Without it the scene holds the final sample for ever: a frozen dot,
            "1 IN ROOM", indistinguishable from someone sitting still.
  LOST_MS   no message at all for this long and the screen says NO FEED, rather
            than drawing an empty room it has no evidence for.
  LAG_MS    the dot is drawn this far behind the newest sample, so its position
            always falls BETWEEN two samples that really arrived. At 1 Hz a dot
            drawn at the newest sample teleports once a second; extrapolating
            past it would invent motion, and a target that stopped reporting
            would keep gliding across the room. It is clamped at both ends, so
            when the feed slows the dot stops where it was last really seen.
"""
import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import presence_feed as feed          # load_window, render, write_gif, sub1

FRESH_S = 5.0
LOST_S = 30.0
# The same two rules the firmware applies (src/presence/presence_model.h): a slot
# is drawn only after two messages in a row, and a sample that would need more
# than MAX_SPEED to reach restarts it instead of drawing a line across the jump.
CONFIRM = 2
MAX_SPEED_MMS = 2000.0
MIN_GAP_S = 0.3
LAG_S = 1.0

DEMO, LIVE, LOST = 0, 1, 2


# ------------------------------------------------------------ the panel's rules
def simulate(rows, fps, story):
    """One entry per rendered frame: ([slot|None x3], count, state).

    rows are the messages as they arrived, (t, [slot|None x3]), x and y in mm
    and the speed already abs(mm/s)/10 - that conversion is presence_feed's and
    stays there, because it is the conversion the firmware makes too.
    """
    # Per slot, the samples that arrived, each marked when it is the first after
    # an absence. Smoothing must not run between the place a target was last
    # seen and the place it came back: nobody walked that line, and drawing it
    # would be the same invention as letting a stopped target keep gliding.
    per_slot = [[], [], []]
    was = [False, False, False]
    for t, slots in rows:
        for i, s in enumerate(slots):
            if s is not None:
                per_slot[i].append((t, s[0], s[1], s[2], not was[i]))
            was[i] = s is not None

    frames = int(round(story * fps))
    out = []
    for f in range(frames):
        now = f / fps
        heard = [r for r in rows if r[0] <= now]
        if not heard:
            out.append(([None, None, None], 0, DEMO))   # nothing yet: the story runs
            continue
        last_t, last_slots = heard[-1]
        stale = now - last_t > FRESH_S                  # the feed stopped, not the room emptied

        shown = []
        for i in range(3):
            if stale or last_slots[i] is None:
                shown.append(None)
                continue
            seen = [s for s in per_slot[i] if s[0] <= now]
            t1, x1, y1, v1, fresh = seen[-1]
            # Count the messages in a row that filled this slot, and break the
            # run where the target teleported: both as the firmware does.
            run, prev = 1, None
            for s_t, s_x, s_y, _sv, s_fresh in seen:
                if prev is None or s_fresh:
                    run, prev = 1, (s_t, s_x, s_y)
                    continue
                gap = max(s_t - prev[0], MIN_GAP_S)
                reach = MAX_SPEED_MMS * gap
                jumped = (s_x - prev[1]) ** 2 + (s_y - prev[2]) ** 2 > reach * reach
                run = 1 if jumped else min(run + 1, CONFIRM)
                prev = (s_t, s_x, s_y)
            if run < CONFIRM:
                shown.append(None)        # one message is not a target yet
                continue
            t0, x0, y0 = (t1, x1, y1) if (fresh or len(seen) < 2) else seen[-2][:3]
            at = now - LAG_S
            if at <= t0 or t1 <= t0:
                x, y = x0, y0
            elif at >= t1:
                x, y = x1, y1                           # clamped: never past the data
            else:
                k = (at - t0) / (t1 - t0)
                x, y = x0 + (x1 - x0) * k, y0 + (y1 - y0) * k
            shown.append((int(round(x)), int(round(y)), v1))

        state = LOST if now - last_t > LOST_S else LIVE
        out.append((shown, sum(s is not None for s in shown), state))
    return out


def lua_stub(sim, range_m):
    """The `presence` binding src/presence provides, filled from the recording."""
    rows = ["{" + ",".join("false" if s is None else f"{{{s[0]},{s[1]},{s[2]}}}"
                           for s in shown) + "}," for shown, _, _ in sim]
    return "\n".join([
        "-- Generated by tools/luasim/presence_panel.py from a recorded session.",
        "-- Where people were in a room: it does not go into the repository.",
        "--",
        "-- This is the `presence` table src/presence binds on the panel, standing in",
        "-- for it under luasim. The scene below is the shipped one, untouched.",
        "local PF = {", *rows, "}",
        "local PC = {" + ",".join(str(c) for _, c, _ in sim) + "}",
        "local PS = {" + ",".join(str(s) for _, _, s in sim) + "}",
        f"local PN = {len(sim)}",
        "",
        "local floor = math.floor",
        "local function idx()",
        "  local i = floor(px.t() * PN) + 1",
        "  return i < 1 and 1 or (i > PN and PN or i)",
        "end",
        "",
        "presence = {",
        f"  scale = function() return {range_m:g} end,",
        "  state = function() return PS[idx()] end,",
        "  count = function(k)",
        "    local i = idx() - k",
        "    return i >= 1 and PC[i] or 0",
        "  end,",
        "  target = function(i)",
        "    local r = PF[idx()][i]",
        "    if not r then return nil end",
        "    return r[1], r[2], r[3]",
        "  end,",
        "  trail = function(i, k)",
        "    local j = idx() - k",
        "    if j < 1 then return nil end",
        "    local r = PF[j][i]",
        "    if not r then return nil end",
        "    return r[1], r[2]",
        "  end,",
        "}",
        "",
    ])


def patch_story(story):
    """Only the window length. The scale arrives through the stub's scale()."""
    s = feed.SCENE.read_text()
    return feed.sub1(s, r"local STORY  = 24\.0.*?\n", f"local STORY  = {story:g}"
                     "            -- seconds of the recorded window; px.t() spans it\n",
                     "STORY line")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", required=True, help="the recorded session JSON (kept outside the repo)")
    ap.add_argument("--from", dest="lo", type=int, required=True)
    ap.add_argument("--to", dest="hi", type=int, required=True)
    ap.add_argument("--range", type=float, default=4.0, help="metres the fan covers (the owner chose 4)")
    ap.add_argument("--mirror", action="store_true")
    ap.add_argument("--stop-at", type=float, default=None,
                    help="seconds in, the feed stops: nothing more arrives")
    ap.add_argument("--fps", type=float, default=10.0)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--name", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-gif", action="store_true")
    a = ap.parse_args()

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    name = a.name or f"panel_{a.range:g}m" + ("_mirror" if a.mirror else "")

    rows, start_utc = feed.load_window(a.frames, a.lo, a.hi, a.mirror, "cms", a.stop_at)
    story = a.hi - a.lo
    sim = simulate(rows, a.fps, story)

    lua = out / f"{name}.lua"
    lua.write_text(lua_stub(sim, a.range) + patch_story(story))

    import time
    clock = time.localtime(start_utc + a.lo)
    raw = feed.render(lua, len(sim), out / f"{name}.raw", time.strftime("%H:%M", clock))

    empty = sum(1 for _, c, s in sim if c == 0 and s == LIVE)
    lost = sum(1 for _, _, s in sim if s == LOST)
    print(f"{name}: {len(rows)} messages over {story}s, {len(sim)} frames at {a.fps:g} fps, "
          f"range {a.range:g} m{', mirrored' if a.mirror else ''}"
          f"{f', feed stops at {a.stop_at:g}s' if a.stop_at is not None else ''}")
    print(f"  empty-room frames {empty}, NO FEED frames {lost}, "
          f"peak targets {max(c for _, c, _ in sim)}")
    if not a.no_gif:
        feed.write_gif(raw, out / f"{name}.gif", a.fps, a.scale)
        print(f"  {out / (name + '.gif')}")


if __name__ == "__main__":
    main()

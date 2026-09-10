# Flight board — where the numbers live

The board's layout is described **once**, in `flightboard.cpp`. Everything that
draws or documents the board reads it from there. This exists because the same
constants used to be typed by hand in three places, in two different git
repositories, and staying in step depended entirely on somebody remembering.

## The dependency

```
  src/flightboard/flightboard.cpp          <-- the only hand-written copy
            |
            |  tools/fb_layout.py  (parses the .cpp, emits JSON + a digest)
            |
      +-----+------------------------------+
      |                                    |
  tools/fb_render.py                  NickoScope/waveshare-rgb-matrix-p2-64x64
  (host render, this repo)            sim/flightboard-sim.html   (other repo)
  imports fb_layout directly          sim/flightboard-layout.json
  -> cannot go stale                  -> generated, carries a digest stamp
```

`fb_layout.py` extracts the geometry constants, the status words for both
directions, and the airport code/name table. It hashes them into a short
`digest`; that digest is what tells you whether two repos still agree.

## The two commands

```bash
python3 tools/fb_check.py        # are all consumers in step? exit 1 if not
python3 tools/fb_sim_build.py    # push the current layout into the other repo
```

Both find the knowledge-base repo at `../LED-MATRIX APOLLO` by default;
override with an argument or `FB_KB_REPO`.

`tools/fb_check.py` also fails if somebody reintroduces literal layout
constants into `fb_render.py`, which is how the drift started last time.

## Automatic check

`.githooks/pre-commit` runs the check whenever the flight board or its tools
are staged. It is not installed by cloning — turn it on once per clone:

```bash
git config core.hooksPath .githooks
```

## Changing the layout

1. Edit the constants in `flightboard.cpp`. Nothing else.
2. `python3 tools/fb_sim_build.py`
3. Commit **both** repos. The digest changes in both or the check fails.

## Why the status words are the length they are

They are not a style choice — every pixel the status column takes is a pixel
the destination name does not get. Measured against 30 live Nice rows: with
`ENROUTE` (29 px) and `DELAYED` (28 px) only 23 of 30 rows could show the IATA
code and the full city name; shortening just those two to `IN AIR` and `DELAY`
brought it to 27. Coverage is flat between 20 px and 28 px of status width and
falls off a cliff after 28, so `ON TIME`, `LANDED` and `DEPART` keep their full
spelling: cutting them further would cost legibility and buy nothing.

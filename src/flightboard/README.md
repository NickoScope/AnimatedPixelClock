# Flight board

Everything for this feature lives in this repository.

| | |
|---|---|
| `flightboard.cpp` | the layout constants, written by hand **only** here |
| `tools/fb_render.py` | host render, reads the constants from the source |
| `sim/flightboard-sim.html` | browser simulation, layout block is generated |

## Changing the layout

Edit `flightboard.cpp`, then:

```bash
python3 tools/fb_sim_build.py    # push the constants into the simulation
python3 tools/fb_check.py        # exit 1 if anything is stale
```

`.githooks/pre-commit` runs the check automatically. Enable once per clone:

```bash
git config core.hooksPath .githooks
```

## Why the status words are short

Every pixel the status column takes is a pixel the destination name does not
get. On 30 live Nice rows, `ENROUTE` (29 px) and `DELAYED` (28 px) alone cost
four destinations their name; shortening just those two to `IN AIR` and `DELAY`
took coverage from 23/30 to 27/30. Coverage is flat from 20 px to 28 px, so
`ON TIME`, `LANDED` and `DEPART` keep their spelling.

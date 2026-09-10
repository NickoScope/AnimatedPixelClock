# luasim — the panel's Lua runtime, on your desk

Runs the **same vendored Lua 5.4.8** the firmware runs, against the same `px.*`
raster API, on the same 128×64 canvas, with the same corrected Picopixel font —
so an effect can be written and judged before the panels exist and without a
flash cycle.

```bash
make                                   # builds luasim and regenerates the font
./luasim scripts/demo.lua 40 out.raw
python3 render.py out.raw out.gif 6    # or out.png for a single frame
```

## What is in scripts/

| | | |
|---|---|---|
| ![](preview/snake_clock.png) | **snake_clock.lua** | HH:MM where each digit is a snake. On the minute the four crawl off the bottom and four more crawl in from the top and lay themselves out as the next time. A pulse runs head to tail the whole time — [full minute](preview/snake_clock.gif) |
| ![](preview/tetris_clock.png) | **tetris_clock.lua** | The same clock in tetrominoes. The rows clear from the bottom like completed lines, everything above drops, and the next minute falls in. Two glints sweep the stack between changes — [full minute](preview/tetris_clock.gif) |
| ![](preview/minecraft.png) | **minecraft.lua** | A blocky world with a full day/night cycle in one minute: terrain, lake, trees, drifting clouds, torches that pool light after dark, a creeper pacing the ridge — [full minute](preview/minecraft.gif) |
| | **demo.lua** | Exercises every `px.*` call. Start here when writing a new effect |

Previews are 225 frames at 3× — a quarter of the real frame rate, so they are
choppier than the panel will be.

## The contract a script follows

Define a global `draw()`. The host calls it once per frame.

```lua
function draw()
  local t = px.t()        -- animation phase, [0,1). NEVER seconds
  local n = px.now()      -- {hour, min, sec} - wall time comes from here
  px.clear(0, 0, 0)
  px.text(2, 2, "HELLO", 255, 180, 0)
end
```

`px.t()` being a phase and not a clock is one of the three rules the flagship
project calls *cannot be otherwise* — mixing the two is the classic bug.

## The API

| Call | |
|---|---|
| `px.size()` | → `128, 64` |
| `px.t()` | animation phase `[0,1)` |
| `px.now()` | `{hour, min, sec}` |
| `px.clear(r,g,b)` | |
| `px.pixel(x,y,r,g,b)` | |
| `px.line(x0,y0,x1,y1,r,g,b)` | |
| `px.rect(x,y,w,h,r,g,b,fill)` | |
| `px.circle(x,y,rad,r,g,b,fill)` | |
| `px.text(x,y,s,r,g,b)` | Picopixel, upper-cased |
| `px.width(s)` | pixel width of `s`, for right-alignment |
| `px.get(x,y)` | → `r,g,b` already on the canvas |
| `px.blend(x,y,r,g,b,a)` | alpha-blend one pixel |
| `px.glow(x,y,rad,r,g,b,amp)` | a radial light, falling off as `(1-d/rad)²` |

The raster calls **overwrite**. That is right for shapes and wrong for light: a
dim colour paints a dark blot, not a faint glow. `blend` and `glow` are the way
to add light to something already drawn, and `glow` is one C call rather than a
Lua loop over a few hundred pixels — the same reason `beam.kit` exists on the
H743.

No `io`, no `os`, no `package` — those libraries are not compiled into the
vendored tree at all, so a script cannot reach the filesystem or the host.

## What this does NOT simulate

Deliberately. These are hardware properties, and answering them is the point of
the bring-up bench rather than of this tool:

- the PSRAM allocator, and whether a Lua heap competes with the HUB75 DMA
- the instruction-budget hook and the wall-clock deadline
- the dedicated task's C stack, which is what stops a hostile nested script
  from overflowing the parser's stack at compile time

A script that looks right here can still be too slow on the panel. Frame budget
is measured on hardware, not here.

## Why the font is generated

`gen_font.py` builds `font_picopixel.inc` from `src/fonts/picopixel_fb.h`, the
firmware's own font — including the corrected `U`. A second copy would drift,
and text is exactly where drift is invisible until it is embarrassing.

# fx3d

3D on the panel, mono and anaglyph (red-blue glasses). Two layers on one model:

- **Scenes**: 3D content that owns the screen while it shows - a calibration for the
  glasses, a cube, depth layers, stars, a helix, rings, a clock in layers, a lit torus,
  a voxel clock, a landscape, a tunnel, metaballs, the Earth lit as it is now, and the
  visualizer's spectrum as hills.
- **Looks**: any page shown in 3D without the page knowing - `pop`, `layers`, `float`,
  `dome` for the glasses; `wiggle`, `card`, `relief`, `drum` without them.

Why each rule is what it is, the owner's words and the decisions: `docs/27-fx3d.md` in the
knowledge base.

**Flag:** `-DFX3D_ENABLED`. Off by default; without it nothing changes (the preprocessed
`main.cpp` was compared with the branch's own, token for token). **Bench:**
`env:matrix-waveshare-rgb-fx3dbench` adds `-DFX3D_BENCH`.

## Files

| | |
|---|---|
| `fx3d_model.h` | vectors, mono and off-axis stereo projection, the baseline for a disparity budget, the rasteriser (sub-pixel points, Wu lines, integer-edge triangles with a depth test), the anaglyph compose, the CIE encode |
| `fx3d_scene.h` | the frame buffers, the drawing context, the scene interface, `renderFrame()` |
| `fx3d_scenes_*.h` | the scenes: lines, solids, raster (per-pixel rays), music |
| `fx3d_present.h` | the looks: a page's frame in, 3D out |
| `fx3d_catalog.h` | every scene, built in memory the caller hands in |
| `fx3d_display.h` | `Fx3dDisplay`: the global `display` under the flag, which can capture a page's frame |
| `fx3d.cpp` | the panel: PSRAM buffers, the blit, the hooks, `/api/fx3d`, the bench |

All headers but `fx3d_display.h` are plain C++11 with no Arduino: `tools/fx3d/check_fx3d.py`
tests them on the Mac, and `tools/fx3d/render.py` and `looks.py` render the previews with them.

## How it reaches the screen

- A **scene** is shown by `/api/fx3d?scene=<id>`; the render tick then draws it instead of
  the page (`fx3dOwnsScreen()`), at 30 Hz. The first touch of the knob gives the panel back.
- A **look** is chosen by `/api/fx3d?look=<name>` and stays on across pages. While it is on,
  `Fx3dDisplay` catches the page's drawing calls in a PSRAM copy of the frame, and at
  `display()` the look puts its 3D picture on the panel. The looks that move raise the
  render tick to at least 30 Hz. While a scene shows, the look waits.
- `/api/fx3d` also takes `mode=mono|redblue|redcyan|redgreen` (one mode for everything
  shown), `depth` (pixels of the largest disparity, 0-16, starting at 2), `swap=0|1`,
  `gl`/`gr` (the eyes' gains, %), `page=0..5` for `calib`, and `bench=1|0`. Every argument
  is parsed strictly and checked before anything changes.
- **`/fx3d`** is the owner's remote for the glasses: the calibration's six steps with what
  to look for at each, every scene, every look, the mode, depth, swap, the eyes' gains, and
  the bench's button. It only calls `/api/fx3d`, and polls only while its tab is visible.
- While a look is on, the render tick runs at **most 30 Hz**, even on a page that wants 60:
  the look renders and blits the whole frame at every flip. The price, until the bench says
  whether 60 fits: the clock animations that take one step per tick (Mario, Pong, Pac-Man)
  run at about half speed under a look, and the classic equaliser's smoothing, set per
  frame, reacts about half as fast.

## What it costs

Measured with `platformio run`, against the same env without the flag:

| | Without | With `FX3D_ENABLED` |
|---|---|---|
| Static RAM | 103,376 B | 103,592 B (**+216 B**) |
| Flash | 2,270,697 B | 2,328,181 B (**+57,484 B**), the page 6.4 KB of it |

At run time: **no internal heap**. PSRAM: 73,984 B of frame buffers from boot (colour, two
eye planes, depth, the encoder); the scene on screen (from 0.1 KB to 320 KB for the
landscape); while a look is on, 24,576 B for the captured frame plus the look (225,352 B
on the host, most of it the drum's per-eye tables). Every one is freed when it stops.

**Not measured yet: the time.** The frame time of every scene and look, and of the blit of
8192 pixels, is what the bench is for (`/api/fx3d?bench=1`, the page's button, or the bench
env 20 s after boot): one line per run, `[fx3d] scene=... frame_us=... blit_us=... fps=...
heap_internal_min=... stack_min_free=...`, then `[fx3d] bench done`. The bench keeps the owner's
mode and look and gives them back.

## What it refuses to do

- No `double`, no allocation in a frame, no big arrays on `loopTask`'s 8 KB stack.
- No serial console: the IR console reads every byte of `Serial`.
- Known, not fixed: `3D SOUND HILLS` gets no sound yet (the visualizer's bands are not
  wired in); the globe's home city does not pulse (`setHome()` has no caller); a page that
  flips twice in one tick (`displayErrorStatus()`) makes a look render twice.
- No writes past the capture into the DMA buffer while a look is on, except its own frame.
- No rotation: the capture assumes `setRotation()` is never called (it is not, in `src/`).
- A `fillRect` with a side below one is caught as nothing; the library itself paints a band
  across the panel there (a counting fault in `fillRectDMA`), which the capture does not copy.

## Sources

- The CIE 1931 table: ESP32-HUB75-MatrixPanel-DMA 3.0.14, `src/cie_luts.h`,
  `lumConvTab_8bit`; `check_fx3d.py` compares our copy byte for byte with the library's file.
- The colour conversion the capture uses: the same library, `color565to888()`.
- Single-precision FPU, `double` in software: ESP32-S3 datasheet; ESP-IDF *Speed
  Optimization*; ESP-IDF *FreeRTOS (IDF), Floating Point Usage*.
- Off-axis stereo, the disparity formula and the checks: the colleagues' brief,
  `docs/drafts/27-anaglyph-handoff-2026-09-18.md` in the knowledge base, §4 and §10.
- The globe's land and sun: the world clock's own (`src/worldclock/worldmap.h`, Natural
  Earth 1:110m; `worldclock.cpp`'s declination and subsolar longitude).
- Algorithms, written here from their descriptions: Xiaolin Wu's line (1991); the voxel
  landscape as in NovaLogic's Comanche and Sebastian Macke's *VoxelSpace*; Inigo Quilez's
  articles *smooth minimum*, *normals for an SDF* and *palettes*.

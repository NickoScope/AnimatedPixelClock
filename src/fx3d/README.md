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
| `fx3d_profile.h` | the glasses profile in NVS: the keys, reading that never refuses, writing only what changed, the deferred write |
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
  `gl`/`gr` (the eyes' gains, %), `page=0..5` for `calib`, `bench=1|0`, and
  `profile=reset`. Every argument is parsed strictly and checked before anything changes.
  Mode, depth, swap and the gains are the **glasses profile**, which NVS keeps (below).
- **`/fx3d`** is the owner's remote for the glasses: the calibration's six steps with what
  to look for at each, every scene, every look, the mode, depth, swap, the eyes' gains,
  whether the profile is saved, a button back to the defaults, and the bench's button. It
  only calls `/api/fx3d`, **one request at a time**, and polls only while its tab is
  visible. Overlapping requests are what drained the panel's internal heap in the
  integration session's measurement (`docs/drafts/27-heap-block-experiment-2026-09-18.md`
  in the knowledge base). A control's request waits under the control's name, so a second
  touch replaces the first one still waiting; a toggle's query is built when it goes, from
  the panel's last answer; a poll is dropped while anything is out or waiting; a request
  that hangs is dropped after 12 s (our choice: longer than a stalled reply can wait on the
  server, where `WiFiClient::write` tries a 1 s `select` up to 10 times; a reply still
  trickling out is dropped all the same, and the next poll puts the page right).
  `tools/fx3d/page_queue_test.js` runs the queue in JavaScriptCore; five broken queues are its
  negative controls.
- While a look is on, the render tick runs at **most 30 Hz**, even on a page that wants 60:
  the look renders and blits the whole frame at every flip. The price, until the bench says
  whether 60 fits: the clock animations that take one step per tick (Mario, Pong, Pac-Man)
  run at about half speed under a look, and the classic equaliser's smoothing, set per
  frame, reacts about half as fast.

## The glasses profile in NVS

What the owner sets with the calibration survives a reboot. Namespace **`fx3d`**, one typed
key per value, so any NVS dump shows them (`fx3d_profile.h` holds the same table):

| key | type | meaning | range |
|---|---|---|---|
| `mode` | u8 | 0 mono, 1 red-blue, 2 red-cyan, 3 red-green | 0..3 |
| `swap` | u8 | 1: the red channel shows the right eye's picture | 0..1 |
| `depth` | u16 | the largest disparity, in 1/100 of a pixel | 0..1600 |
| `gl` | u8 | the left eye's gain, per cent | 0..100 |
| `gr` | u8 | the right eye's gain, per cent | 0..100 |
| `ver` | u8 | this layout, written last | 1 |

- **Nothing stored** means the defaults: red-blue, the eyes as they are, 2 px, both eyes at
  100 %. The reset (`profile=reset`, the page's button) erases the namespace, so a later
  firmware's defaults apply too.
- **Reading never refuses.** A key that is missing, of another type, or out of its range
  gives that value's default; a `ver` other than 1, or none, gives every default. A
  missing namespace is the first boot, not an error, though `Preferences::begin()` reports
  it through `log_e` (arduino-esp32 2.0.17, `Preferences.cpp`), as it does for `irmap`.
- **Writing** waits 2.5 s after the last change (the clock styles' `SETTLE_MS`, chosen there
  for flash wear), and not while the bench runs, which borrows the profile and gives it back
  when it ends. It puts only the keys whose value changed, `ver` last. Depth is rounded to
  the 1/100 px NVS keeps as soon as it is set, so what shows is what a reboot brings back.
- **A key of ours with another type**, or a write that failed half way, makes the next write
  erase the namespace and put every key. In ESP-IDF 4.4.7, which arduino-esp32 2.0.17 is
  built on, a put of another type does not replace the old entry: `Page::findItem` stops at
  it with a type mismatch, `Storage::findItem` moves on, so `Storage::writeItem` adds the new
  entry and erases nothing, and since the index hash leaves the type out
  (`Item::calculateCrc32WithoutValue`), reads on that page keep stopping at the old one
  (`nvs_page.cpp`, `nvs_storage.cpp`, `nvs_types.cpp` at v4.4.7). IDF v5.2 and later call it
  the legacy behaviour, keep it behind `CONFIG_NVS_LEGACY_DUP_KEYS_COMPATIBILITY`, and test
  both (`host_test/nvs_host_test/main/test_nvs.cpp`).
- **A refusal** is tried again after each settle, three times (our choice, not a measured
  figure; `src/railboard/railboard.cpp` retries a refused open without a limit), then waits
  for the next change. A refused reset leaves the defaults on and retries as a write of them.
- **What it says:** `/api/fx3d` answers `"profile":"kept"` (a reboot brings back what is on),
  `"pending"` (a change or a retry waits for its 2.5 s) or `"failed"` (the retries are spent).
  The log: `[fx3d] glasses from NVS, 5 of 5 values: redblue swap=0 depth=2.00 gl=100 gr=100`
  at boot, `[fx3d] glasses saved, 1 keys in N ms` per write, and on a refusal what a reboot
  would bring back.
- **Not kept:** the look and the scene. A reboot brings the panel back flat, on its page.
- **The portal's factory reset** (`src/web/web.cpp`) clears the `pcmonitor` namespace only:
  the glasses profile survives it, as the IR map and the other modules' namespaces do.

The rules and the glue the firmware runs (`ProfileKeeper`: the bench, reset before the other
arguments, retries, what the API says) are tested on the Mac against a stand-in for NVS that
holds typed keys, refuses writes and erases on demand, and behaves either way for a key
rewritten with another type (`tools/fx3d/fx3d_host_test.cpp`, `profile()`), with a negative
control: without the erase, the legacy behaviour leaves the new depth unread.

## What it costs

Measured with `platformio run`, against the same env without the flag:

| | Without | With `FX3D_ENABLED` |
|---|---|---|
| Static RAM | 103,376 B | 104,000 B (**+624 B**, of it the blit's 384 B row) |
| Flash | 2,270,665 B | 2,335,925 B (**+65,260 B**), the page 8.2 KB of it |

At run time: **no internal heap per frame**. Saving or resetting the glasses profile opens NVS
for a moment: ESP-IDF allocates the handle then (`nvs_api.cpp`), and a write that adds an entry
to a page can grow NVS's index by a 128-byte block (`nvs_item_hash_list.hpp`/`.cpp`, v4.4.7);
blocks that small come from internal RAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` = 4096 in
arduino-esp32 2.0.17's S3 sdkconfig). PSRAM: 73,984 B of frame buffers from boot (colour, two
eye planes, depth, the encoder); the scene on screen (from 16 B to 415,072 B for the landscape and 295,752 B for the globe,
whose per-eye tables hold what its rays find,
87,360 B of which are its noise lattices, dead once the map is built and kept only because 87 KB
of 16 MB is not worth a second allocation); while a look is on, 24,576 B for the captured frame plus the look (8,432 B
on the host; 225,624 B at `6200dc7`, before the drum's tables went from per pixel to per column).
Every one is freed when it stops.

**Measured on the panel, 2026-09-18 (the integration session, over the network):**
- At `264d6f1`: a blit of 8192 single pixels cost 14.2-14.9 ms in every scene; eight scenes held
  30 Hz in mono and red-blue (calib, cube, layers, stars, dial, helix, rings, vclock); torus,
  terrain, voxel, tunnel, globe and blobs did not (blobs 111.8 / 217.8 ms a frame); voxel took
  1.08 s to open; under a look pages ran at 10-20 fps. Answered by: the blit writes runs of one
  colour with the library's hlineDMA; blobs march one ray per 2 x 2 block; the landscape hashes
  each noise lattice once; the looks sort a row's pixels into their depth bands once.
- At `80eb788` (`docs/drafts/27-fx3d-panel-measurements-80eb788.md` in the knowledge base): the
  blit 6.3-9.7 ms; six of eight looks at 30 Hz, card 46.0 ms a frame and drum 31.5 ms (about
  38 and 25 ms of it their own work beyond the blit); blobs
  41.7 / 82.2 ms (mono / red-blue); globe, tunnel and voxel 18-22 fps in mono and 10-13 in
  red-blue; voxel opens in 0.45 s.
- The landscape's map, which took 1.08 s to build at `264d6f1` and 0.45 s at `80eb788`, made
  twelve divisions for every one of its 65,536 cells: the noise divided by each octave's cell
  width. A cell's width is a power of two, so multiplying by its reciprocal gives the same map
  to the last bit - the host test checks it cell by cell - without the calls.
- **Measured on the panel at `6016158`** (`docs/drafts/27-fx3d-panel-measurements-6016158.md`):
  tunnel 34,960 -> 6,366 us a frame in mono and 69,372 -> 12,044 in red-blue (5.5x and 5.8x);
  globe 45,516 -> 11,680 and 90,091 -> 23,064 (3.9x both); the landscape 32,827 -> 13,306 and
  62,344 -> 26,591 (2.5x and 2.3x), and its map opens in 258 ms instead of 452; blobs only
  1.25x, 33,455 us in mono and 65,772 in red-blue. So in mono every scene but blobs holds
  30 Hz, and with the glasses every scene but blobs and the landscape. The globe's table
  reading from PSRAM did not become the wall: its gain is the same in both modes.
- Answered since, the four heavy scenes: tunnel works its eight band colours out once a frame
  instead of three `cosf` a pixel; blobs march with `sqrtFast` and no division, and the upscale
  reads a table instead of calling `floorf` twice a pixel; the landscape's march reads a table
  of its steps (where, `f / z`, the fog) and floors in integers, and paints a slice and the sky
  as bytes; the globe keeps a table per eye of what each ray finds (the normal in the Earth's
  frame before the spin, the latitude, the longitude before the spin, the limb, the
  atmosphere), so a frame is multiplications where it used to call `asinf` twice, `atan2f`,
  `sinf`, three square roots and six divisions a pixel - at the price of reading 98,304 B an
  eye from PSRAM every frame, which is where its gain on the panel will run into the cache. Each is held to what it drew before in
  `tools/fx3d/fx3d_host_test.cpp`, which keeps the old code verbatim: within one code for
  tunnel, the landscape and the globe, and a mean of 0.0016 for blobs, whose march can end a
  step early. On the Mac: tunnel 2.3-2.8x, the landscape 1.1-1.2x, the globe 2.9-4.0x, blobs
  the same (the Mac's square root is one instruction). The panel's figures are the integration
  session's to measure.
- Answered since: card and drum sample the picture in integers and write bytes straight into
  the target; the drum keeps its table per column instead of per pixel (the rays of a column
  meet a vertical drum at one angle); the card carries its reciprocal along the row instead of
  dividing - on the S3 `/` on floats is a call into ROM (below), where the old card divided
  three times a pixel and the old drum twice. On the Mac, which divides in hardware, the drum
  is 3.6 times cheaper and the card about the same. Their panel figures are not measured yet.

**What floats cost on the S3.** The FPU adds, multiplies (with fused multiply-add), compares
and converts inline. Everything else is a call, read from this firmware's disassembly
(`xtensa-esp32s3-elf-objdump` of `firmware.elf`, `.pio/build/<env>/`) and from Espressif's ROM
image (`tool-esp-rom-elfs/esp32s3_rev0_rom.elf`), arduino-esp32 2.0.17:

| In C | What runs | Size |
|---|---|---|
| `a / b` | `__divsf3` in ROM, through a trampoline at 0x40002274 (`esp32s3.rom.libgcc.ld`) | about 28 FPU instructions around the `div0.s` estimate and `divn.s` |
| `sqrtf` | newlib's wrapper (errno) calling `__ieee754_sqrtf` | about 30 FPU instructions around `sqrt0.s`, two calls deep |
| `floorf`, `lrintf` | newlib | 53 and 49 instructions, bit work in integers |
| `sinf`, `cosf` | newlib: argument reduction, then polynomial kernels | about 40 instructions of wrapper, and the kernels |
| `atan2f`, `asinf` | newlib wrappers around `__ieee754_atan2f`, `__ieee754_asinf` | software |

A division by a constant that is not a power of two is not turned into a multiplication (no
`-ffast-math`): `x / 255.0f` is a call too. The instruction counts are counts, not cycles.
Espressif has measured the cycles on an S3 with its FPU - 10,000 calls in a loop, the average
including the call's own cost: a float addition 25, a float division **69**, `cosf` **121**, a
`double` division 75, a `double` cosine 1619 ("Floating-Point Units on Espressif SoCs",
developer.espressif.com/blog/2025/10/cores_with_fpu/, read 2026-09-20; the article does not
give its compiler flags). So three `cosf` a pixel, as the tunnel had, is about 290 cycles a
pixel over an addition, and 8192 pixels of that is some 10 ms an eye at 240 MHz. What each
change is worth on this panel is still the bench's to say. The hot loops of the scenes and looks keep
these out: `sqrtFast()`/`normalizeFast()` in `fx3d_model.h`, reciprocals worked out once,
tables where the geometry does not change.
The blobs changed more than their resolution: the march also stops at 28 steps instead of 40
and calls a hit at 0.006 instead of 0.004, which moves their surface more than the halved
resolution does.

**The blit by runs and PSRAM DMA buffers do not mix.** `hlineDMA` does not write the cache
back (`Cache_WriteBack_Addr`) the way the per-pixel path does under `SPIRAM_DMA_BUFFER`. That
flag is not set here (the buffers stay internal, docs/03 in the knowledge base), so it is
harmless today; if it is ever set, the blit by runs goes wrong where single pixels would not.

**Opening a scene** draws one frame at once and throws it away, so a scene that builds tables
for the view it is shown in - the tunnel, the drum's look, the globe - pays for them inside
`open_us` rather than in the first tick the owner sees. In red-blue that is both eyes' tables.

**Still to measure: the time.** The frame time of every scene and look, and of the blit of
8192 pixels, is what the bench is for (`/api/fx3d?bench=1`, the page's button, or the bench
env 20 s after boot): one line per run, `[fx3d] scene=... frame_us=... blit_us=... fps=...
heap_internal_min=... stack_min_free=... depth=... swap=... gl=... gr=...`, then `[fx3d] bench done`.
The bench sets only the mode for each run; depth, swap and the gains are the owner's profile,
printed on every line. It keeps the owner's mode and look and gives them back.

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

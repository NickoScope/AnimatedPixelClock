# Credits

This project stands on other people's work: hardware, a firmware to fork, ideas, algorithms, data and
fonts. Thank you all. Each entry below was checked against its source (the file and line here, or the
author's own page); where the code only took an idea, that is said.

## Hardware: Waveshare

Thank you to [Waveshare](https://www.waveshare.com/) for the beautiful panels and the board.
- Two [RGB-Matrix-P2-64x64-B](https://www.waveshare.com/rgb-matrix-p2-64x64.htm) panels (P2, GOB-coated,
  SKU 33838) make the 128x64 canvas.
- The [ESP32-S3-RGB-Matrix](https://www.waveshare.com/esp32-s3-rgb-matrix.htm) driver board (SKU 34422).
- Its [board support package and examples](https://github.com/waveshareteam/ESP32-S3-RGB-Matrix)
  (Apache-2.0), where the TF-card, I2S and I2C pins come from (`src/clips/clip_sd.h`, `src/audio/audio_mic.cpp`,
  `src/board/board_i2c.h`), and the 30 dB microphone gain of its `08_Matrix_Audio` example (`src/config/config.h`).

## Where the firmware comes from

- [Keralots/AnimatedPixelClock](https://github.com/Keralots/AnimatedPixelClock) by Keralots (Rafał Stolarek),
  MIT: the clock styles, the ambient effects, the web portal, the PC companion and the audio visualizer.
- **Cycle All Styles** and letting an animation finish before the style changes: BaltasarParreira
  ([PR #52](https://github.com/Keralots/SmallOLED-PCMonitor/pull/52),
  [issue #60](https://github.com/Keralots/SmallOLED-PCMonitor/issues/60) in the sibling project).
- **Code EQ** (visualizer style 2) reuses the Matrix Rain clock style's grid, characters, fade and colours by Keralots.

## Ideas and algorithms behind the effects

| Effect or helper | Whose idea or method | What was taken |
|---|---|---|
| KINETIC DIGITS LED | Ksawery Kirklewski, [Flipdigits Player](https://ksawerykomputery.com/tools/flipdigits-player) | the idea of sampling a picture into seven-segment digits (the code is our own) |
| KALEIDOSCOPE, plasma | Lode Vandevenne, [Plasma](https://lodev.org/cgtutor/plasma.html) | sums of sines along x, y, the diagonal and in rings, animated by the palette |
| `px.step(L, "fire")` | Lode Vandevenne, [Fire Effect](https://lodev.org/cgtutor/fire.html) | the algorithm, in integers |
| `px.step(L, "wave")` | Hugo Elias, [2D Water](https://web.archive.org/web/20160607052007/http://freespace.virgin.net/hugo.elias/graphics/x_water.htm) | the two-buffer ripple |
| REACTION, `px.reaction` | Karl Sims, [Reaction-Diffusion Tutorial](https://www.karlsims.com/rd.html); Robert Munafo, [Xmorphia](https://www.mrob.com/pub/comp/xmorphia/) | the Gray-Scott model and its parameters |
| `px.step(L, "life")`, the Life scene | John Horton Conway, the Game of Life (1970) | the rule |
| NEBULA, FLOW, WARP's land (`px.noise`, `px.field`) | Ken Perlin, [Improved Noise](https://mrl.cs.nyu.edu/~perlin/noise/) (2002) | the noise (our own Q16 implementation) |
| cosine palettes (`px.palette{"cos",…}`), KALEIDOSCOPE's moods, VORTEX | Inigo Quilez, [palettes](https://iquilezles.org/articles/palettes/), [smooth minimum](https://iquilezles.org/articles/smin/), [normals for an SDF](https://iquilezles.org/articles/normalsSDF/) | formulas and parameter sets |
| VORTEX, `px.feedback` | Ryan Geiss, [MilkDrop](https://www.geisswerks.com/milkdrop/) | the zoom-rotate-decay feedback technique |
| `px.aline`, `px.mesh` (SOLIDS), fx3d lines | Xiaolin Wu, [An efficient antialiasing technique](https://doi.org/10.1145/127719.122734) (1991) | the antialiased line |
| `px.terrain` (GOLF in 3D), fx3d voxel scene | NovaLogic's Comanche (1992); Sebastian Macke, [VoxelSpace](https://github.com/s-macke/VoxelSpace) (MIT) | the Voxel Space technique |
| fx3d terrain hash | Chris Wellons, [hash-prospector](https://github.com/skeeto/hash-prospector) `lowbias32` (Unlicense) | constants and mixing |
| `px.blur` (LASER CLOCK's glow) | [FastLED](https://github.com/FastLED/FastLED) `blur2d` (MIT, © 2013 FastLED; credited in `src/lua/px_raster.h`, notice below) | the algorithm, reproduced from its source |
| random numbers (visualizer, gallery scripts) | George Marsaglia, [Xorshift RNGs](https://doi.org/10.18637/jss.v008.i14) (2003) | xorshift32 |
| FAMILY PORTRAIT (`photo_to_lua.py`) | Robert W. Floyd and Louis Steinberg (1976) | error-diffusion dithering |
| LA GIOCONDA | Hans Petter Jansson, [chafa](https://github.com/hpjansson/chafa) (LGPL-3.0) | the tool whose output the script carries (no chafa code in the firmware) |
| LA GIOCONDA, early versions | [asciicker](https://github.com/msokalski/asciicker) (MIT) for the idea; [libtcod](https://github.com/libtcod/libtcod) `generate_quadrant_graphic` (BSD-3-Clause; notice in `tools/luasim/quad.py`), credited there to Jeff Lait | idea; code transcribed into a tool |
| fx3d brightness table | mrcodetastic, ESP32-HUB75-MatrixPanel-DMA 3.0.14 (MIT) | the CIE 1931 table `lumConvTab_8bit` |
| world clock | Apple, the clocks of [StandBy](https://support.apple.com/guide/iphone/use-standby-iph878d77632/ios) on iPhone (on its side while charging), the owner's reference | the look (drawn here from scratch) |
| cards and notifications from Home Assistant | Blueforcer, [AWTRIX 3](https://github.com/Blueforcer/awtrix3) | the idea of the architecture |
| oscilloscope-music clips | Jerobeam Fenderson, [Oscilloscope Music](https://oscilloscopemusic.com/watch/oscilloscope_music) (2016) | his audio as the clips' source (not in this repository) |

## Data

- [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL 1.0): the Old Course Cannes-Mandelieu
  in GOLF OLD COURSE and GOLF PESTOVO. © OpenStreetMap contributors.
- Pestovo Golf Club, [course guide](https://pestovo.golf/club): par, length and index of the holes in GOLF PESTOVO.
- [Natural Earth](https://www.naturalearthdata.com/) 1:110m land (public domain): the world clock's map and fx3d's Earth.
- The [tz database](https://www.iana.org/time-zones) (public domain), Howard Hinnant's
  [date algorithms](http://howardhinnant.github.io/date_algorithms.html) (public domain) and
  [mwgg/Airports](https://github.com/mwgg/Airports) (MIT).
- The WPBSA rules of snooker (September 2024) and the IFAB Laws of the Game 2026/27: SNOOKER CLOCK and FOOTBALL CLOCK.
- Espressif's [esp_codec_dev](https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev)
  (Apache-2.0): the ES7210 microphone start-up sequence.

## Fonts

- Picopixel by Sebastian Weber, TomThumb by Brian J. Swetland and Vassilii Khachaturov, and the classic 5x7
  `glcdfont`, all part of [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) (BSD).
- The Cyrillic glyphs from X11 misc-fixed 6x10 (public domain).

## The virtual twin

The twin runs on [esp32sim](https://github.com/joakimeriksson/esp32sim) by Joakim Eriksson and Alice
(@aliceisjustplaying). Its other building blocks and their authors are in
[tools/twin/README.md](tools/twin/README.md#authors-and-thanks).

## Libraries

See [Libraries](README.md#libraries) in the README.

## Themes

Several screens are homages to games and stories; see [Trademarks and attribution](README.md#trademarks-and-attribution).

## License notices

### FastLED (MIT) - for `px.blur` in `src/lua/px_raster.h`

The MIT License (MIT)

Copyright (c) 2013 FastLED

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

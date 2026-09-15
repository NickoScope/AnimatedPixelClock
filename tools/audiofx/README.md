# Audio visualizer simulator

Previews of the visualizer driven by the board's microphones, rendered on the
host before anything changes on the panel. The DSP here is the reference that
`src/audio/audio_dsp.cpp` is tested against; the effects are the six that ship
(ported line by line from `src/viz/`) and the eight the owner picked on 2026-09-15,
built as styles 7-14 in `src/viz/wow/` and held to `effects_wow.py` pixel for pixel. The design and the source of every number: `docs/22` in the KB.

```bash
python3 tools/audiofx/gen_wavs.py        # wav/: six synthetic WAVs, each with a .json of its true beats
python3 tools/audiofx/render.py          # out/: a GIF and an MP4 per effect, two contact sheets
make -C tools/audiofx/host check         # the firmware's C++ DSP against dsp.py on the same WAVs
make -C tools/audiofx/host wow           # styles 7-14 in C++ against effects_wow.py, every pixel of 300 frames
python3 tools/audiofx/host/compare_wow.py --pc-gifs   # and out/pc_*.gif: the eight fed from PC packets
```

`wav/` and `out/` are generated and not committed. Needs numpy and Pillow;
ffmpeg for the MP4s (`--no-mp4` skips them).

## Files

| File | What it is |
|---|---|
| `gen_wavs.py` | 48 kHz 16-bit stereo test signals made from numpy, with ground truth |
| `dsp.py` | the DSP chain: bands, gate, AGC, levels, peaks, beats, waveform, the `FFT1` packet |
| `gfx.py` | a 128x64 RGB565 canvas drawing like Adafruit GFX and the HUB75 library, and the LED-look upscale |
| `effects_current.py` | the six shipping styles, from `src/viz/visualizer.cpp`, `starfield.cpp`, `oscilloscope.cpp` |
| `effects_wow.py` | styles 7-14, written to be repeated exactly in C++: xorshift32, explicit interpolation, byte buffers, Python's round and % spelled out |
| `render.py` | WAV → DSP → effects → GIF (6x, 20 fps), MP4 (60 fps with sound), contact sheets |
| `host/` | `test_dsp.cpp` runs `src/audio/audio_dsp.cpp` over a WAV and `compare.py` holds it to `dsp.py`; `test_wow.cpp` renders `src/viz/wow` from the C++ DSP's frames and `compare_wow.py` holds it to `effects_wow.py`, built with `real` as double (must be identical) and as float (as the panel; reported) |

## The test signals

| WAV | What it holds | What it checks |
|---|---|---|
| `showreel` | 0–1.3 s kick and bass at 120 BPM; 1.3–2.5 s full groove with snares, hats and a chord; 2.5–3.3 s sweep 80 Hz–12 kHz; 3.3–4.1 s silence with 50 Hz hum at −74 dBFS; 4.1–5 s the groove again | every preview is rendered from it |
| `kick120` | twelve kicks over a −50 dBFS pink bed, hats on the off-beats | beat recall and latency |
| `pink` | pink noise at −30 dBFS | no beats from steady noise |
| `tone_1k_100` | 1 kHz then 100 Hz, silence around | band placement, gate, onsets |
| `sweep` | log sweep 50 Hz–16 kHz | every band in turn |
| `silence_clip` | room noise, a clipped 200 Hz burst | gate, clipping flag |

## What each preview shows

Every render is from `showreel.wav`. The current effects receive a packet every
40 ms, as `vizIngest()` would from the microphones; the proposals read every
20 ms DSP frame. Both are drawn at 60 Hz like the panel, with the small clock on.
Stills in `contact_*.png`: 0.62 s kick, 1.90 s groove, 2.95 s sweep, 3.90 s
silence, 4.62 s groove returns.

### As they ship

| Effect | Bass | Beat | Silence |
|---|---|---|---|
| `classic_eq` | the left bars rise through green and yellow | a jump, then peak dots falling | bars drop, the dots settle last |
| `neon_mirror` | the left half of the mirror swells | the same, at half height | an empty horizon |
| `phosphor_waterfall` | short bright bands at the left | a new bright row | a black gap scrolls down |
| `purple_led_stage` | the purple waves open wider | brighter lamps | dim swaying lamps |
| `starfield_overdrive` | the stars fly faster | a hyperspace boost, from its own packet flux | slow drift |
| `oscilloscope` | slow wide swings | a bigger swing | a flat line; the sweep fills the screen |

### Styles 7-14, the owner's picks

| Effect | Bass | Beat | Silence |
|---|---|---|---|
| `w1_prism_eq` | tall violet bars at the left, each bright at its top | the palette slides a step, a violet floor line flashes | caps fall, bars dark |
| `w2_neon_mirror_plus` | the centre of the mirror swells | colours warm toward gold, the horizon flashes | peak dots drift down |
| `w3_spectrogram` | a bright floor across the screen | a bright column and an amber tick | black |
| `w4_radial_bloom` | the core breathes, top and bottom lobes grow | a violet shockwave ring | a small dim ring |
| `w5_beat_particles` | a glow along the floor, stars drift faster | a fountain of particles in the loudest group's colour | slow stars |
| `w6_scope_afterglow` | slow swings leaving green afterglow | a thicker trace, brighter graticule | a flat line |
| `w7_twin_vu` | the LO needle swings | the pivot caps glow | needles rest left |
| `w8_synthwave_grid` | the sun grows, edge mountains rise | the grid flashes and rushes forward | the grid idles, the ridge flattens |

## The beat detector

The four beat numbers in `dsp.py` (flux floor, K, delta, bass rise) were picked
from a grid run on these six files: every kick in `kick120` and `showreel`, no
beat from `pink`, one onset where a sound starts. Six synthetic files are not
real music through a microphone, so the numbers are reference values until the
panel test in docs/22 §12.

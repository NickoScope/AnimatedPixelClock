# Twin timing calibration, 2026-09-29

The question: at what speed does the twin run the firmware's Lua the way the panel does?

**How the panel was measured.**
- The panel's figures are its firmware's own 30-second reports ("draw avg").
- KINETIC DIGITS LED was the script in commit 4136eb8, one press per scene (kd_all.py, 13:37-13:47).
- The twin ran the same script through the same firmware routes (upload, show, click). `calibrate.py` read the same report from the twin's console.
- The twin's carousel was turned off: a fresh chip has it on at 15 s a page.

**The model.** A uniform number of cycles per instruction (`--cpi`, the engine's JIT timing). The draw time scaled almost exactly with it: going from cpi 1 to cpi 2 multiplied every scene's time by 2.02-2.10.

## Fit: ten scenes (draw avg, ms)

| Scene | Panel | Twin, cpi 1 | Panel / twin |
|---|---|---|---|
| 8x12 plasma | 39.1 | 14.8 | 2.64 |
| 6x11 rings | 43.7 | 15.7 | 2.78 |
| 11x21 clock | 7.7 | 2.9 | 2.66 |
| 5x9 life | 36.6 | 14.9 | 2.46 |
| 5x9 cube | 47.2 | 17.4 | 2.71 |
| dots plasma | 58.7 | 25.6 | 2.29 |
| 8x16 text | 12.3 | 5.8 | 2.12 |
| dots rings | 62.8 | 24.4 | 2.57 |
| 4x7 plasma | 65.9 | 26.4 | 2.50 |
| 8x12 text | 13.2 | 7.0 | 1.89 |

- **Geometric mean of the ratios: 2.45.** That becomes `--cpi 2.45`, the default of `twin.py run`.
- **Spread:** the ratios run from 1.89 to 2.78. Divided by 2.45, a scene's twin time is off from the panel's by −23% to +13%.
- **The two text scenes are the fastest on the panel relative to the twin.** A likely reason: their time goes into the firmware's C text routines more than into the Lua VM, whose heap lives in PSRAM. This is not verified.

## Held out: not used for the fit, cpi 2.45

| Effect | Panel | Twin | Twin vs panel |
|---|---|---|---|
| OCEANARIUM | 37-47 (v2.7.3 notes; about 40) | 44.3 | inside the range |
| KINETIC 372ce25, dots cube | 24.9 (kd_ball.py, 13:59) | 28.8 | +16% |
| KINETIC 372ce25, ball + lasers | 52.4 (kd_ball.py, 13:59) | 46.6 | −11% |

Every effect and scene held 15.1-15.2 fps in the twin. On the panel, dots rings ran at 14.0 fps and 4x7 plasma at 14.3. The twin does not reproduce those two drops.

## What this does not cover

- **Only one kind of work.** The factor is fitted on Lua effect frames. With the same factor, everything else runs 2.45x slower than at full engine speed: boot, Wi-Fi, the web server. Nothing on the panel has been measured to say whether that is right.
- **The engine's approximate timing** (`--approximate-timing --approximate-cache --approximate-memory 40|100`) was tried and did not do better. 8x12 plasma came to 20.9-21.0 ms against the panel's 39.1, and OCEANARIUM to 26.5-30.4. It also runs slower than real time.
- **Two points to settle with the panel:**
  - which script version was on the panel at 13:59: 372ce25 was committed at 14:00:07;
  - the 13:37 run's version (4136eb8 is the commit before it).
- **Per the owner's rule, this factor is a reference.** It rests on 10 + 3 measurements from one panel on one day.

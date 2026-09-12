#!/usr/bin/env python3
"""Build every compile-flag combination that matters, and say which break.

Written because the obvious way to do this does not work: `platformio run
--project-option=...` is not an option in this PlatformIO, the command exits
with "Error: No such option", and a grep for "error:" over its output finds
nothing and reports success. Three combinations were reported green that day
that had never been built at all.

This writes a scratch env into a copy of platformio.ini and checks the RETURN
CODE, which cannot be fooled.

  python3 tools/flag_matrix.py
"""
import pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIO  = pathlib.Path.home() / ".platformio/penv/bin/platformio"
COMMON = ("-DBOARD_HAS_PSRAM -DBOARD_WAVESHARE_RGB_MATRIX "
          "-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1")

# (name, flags, must_build). The false ones are dependencies we declare with
# #error: a build that succeeds there would mean the guard is not doing its job.
COMBOS = [
    ("nothing enabled",        "", True),
    ("bus only",               "-DMQTT_BUS_ENABLED", True),
    ("bus + cards",            "-DMQTT_BUS_ENABLED -DCARDS_ENABLED", True),
    ("bus + cards + encoder",  "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED -DCARDS_ENABLED", True),
    ("+ carousel",             "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED -DCARDS_ENABLED "
                               "-DCAROUSEL_ENABLED", True),
    ("flight board + bus",     "-DFLIGHTBOARD_ENABLED -DMQTT_BUS_ENABLED -DFB_MQTT_ENABLED", True),
    ("yacht radar + encoder",  "-DYACHTRADAR_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("lua only",               "-DNSLUA_ENABLED", True),
    ("everything",             "-DFLIGHTBOARD_ENABLED -DYACHTRADAR_ENABLED "
                               "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED "
                               "-DFB_MQTT_ENABLED -DCARDS_ENABLED -DCAROUSEL_ENABLED "
                               "-DNSLUA_ENABLED", True),
    ("cards without the bus",  "-DCARDS_ENABLED", False),
    ("flight MQTT without bus","-DFLIGHTBOARD_ENABLED -DFB_MQTT_ENABLED", False),
    ("carousel without knob",  "-DCAROUSEL_ENABLED", False),
]

def main():
    base = (ROOT / "platformio.ini").read_text()
    tmp  = pathlib.Path("/tmp/pio_flag_matrix.ini")
    bad  = 0
    for name, flags, must in COMBOS:
        tmp.write_text(base + "\n[env:flagtest]\nextends = env:matrix-waveshare-rgb\n"
                              f"build_flags =\n\t{COMMON}\n\t{flags}\n")
        r = subprocess.run([str(PIO), "run", "-e", "flagtest", "--project-conf", str(tmp)],
                           capture_output=True, text=True)
        built = r.returncode == 0
        good  = built == must
        note  = "" if must else ("guard held" if not built else "GUARD MISSING")
        print(f"  {name:26s} {'builds' if built else 'refused':8s} "
              f"{'ok' if good else 'UNEXPECTED':10s} {note}")
        bad += not good
    tmp.unlink(missing_ok=True)
    print(f"\n{len(COMBOS) - bad}/{len(COMBOS)} behaved as intended")
    sys.exit(1 if bad else 0)

main()

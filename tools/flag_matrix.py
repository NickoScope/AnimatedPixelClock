#!/usr/bin/env python3
"""Build every compile-flag combination that matters, and say which break.

Written because the obvious way to do this does not work: `platformio run
--project-option=...` is not an option in this PlatformIO, the command exits
with "Error: No such option", and a grep for "error:" over its output finds
nothing and reports success. Three combinations were reported green that day
that had never been built at all.

This writes a scratch env into a copy of platformio.ini and checks the RETURN
CODE, which cannot be fooled. It then builds the two images the bring-up
flashes before the firmware, which nothing else ever builds.

  python3 tools/flag_matrix.py
"""
import pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIO  = pathlib.Path.home() / ".platformio/penv/bin/platformio"
# MARKET_NO_LOCAL_DEFAULTS: every row builds the market pages' neutral defaults,
# whatever src/market/market_local_defaults.h holds on this machine.
COMMON = ("-DBOARD_HAS_PSRAM -DBOARD_WAVESHARE_RGB_MATRIX "
          "-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DMARKET_NO_LOCAL_DEFAULTS")

# (name, flags, must_build). The false ones are dependencies we declare with
# #error: a build that succeeds there would mean the guard is not doing its job.
COMBOS = [
    ("nothing enabled",        "", True),
    ("fx3d only",                  "-DFX3D_ENABLED", True),
    ("bus only",               "-DMQTT_BUS_ENABLED", True),
    ("bus + cards",            "-DMQTT_BUS_ENABLED -DCARDS_ENABLED", True),
    ("bus + cards + encoder",  "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED -DCARDS_ENABLED", True),
    ("+ carousel",             "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED -DCARDS_ENABLED "
                               "-DCAROUSEL_ENABLED", True),
    ("+ carousel, all styles", "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED -DCARDS_ENABLED "
                               "-DCAROUSEL_ENABLED -DCAROUSEL_ALL_STYLES", True),
    ("flight board + bus",     "-DFLIGHTBOARD_ENABLED -DMQTT_BUS_ENABLED -DFB_MQTT_ENABLED", True),
    ("flight direct, no MQTT", "-DFLIGHTBOARD_ENABLED -DFLIGHTBOARD_DIRECT_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("flight direct + MQTT",   "-DFLIGHTBOARD_ENABLED -DFLIGHTBOARD_DIRECT_ENABLED -DCONTROL_ENCODER_ENABLED "
                               "-DMQTT_BUS_ENABLED -DFB_MQTT_ENABLED", True),
    ("yacht radar + encoder",  "-DYACHTRADAR_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("lua only",               "-DNSLUA_ENABLED", True),
    ("lua effects + knob",     "-DNSLUA_ENABLED -DCONTROL_ENCODER_ENABLED -DLUA_EFFECTS_ENABLED", True),
    ("world clock + encoder",  "-DWORLDCLOCK_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("rail board + bus + knob", "-DRAILBOARD_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("rail direct + board",    "-DRAILBOARD_DIRECT_ENABLED -DRAILBOARD_ENABLED -DMQTT_BUS_ENABLED "
                               "-DCONTROL_ENCODER_ENABLED", True),
    ("everything",             "-DFLIGHTBOARD_ENABLED -DFLIGHTBOARD_DIRECT_ENABLED -DYACHTRADAR_ENABLED "
                               "-DCONTROL_ENCODER_ENABLED -DMQTT_BUS_ENABLED "
                               "-DFB_MQTT_ENABLED -DCARDS_ENABLED -DCAROUSEL_ENABLED "
                               "-DNSLUA_ENABLED -DWORLDCLOCK_ENABLED -DRAILBOARD_ENABLED -DRAILBOARD_DIRECT_ENABLED "
                               "-DLUA_EFFECTS_ENABLED -DCLIPS_SD_ENABLED -DMEDIAPLAYER_ENABLED -DMARKET_ENABLED "
                               "-DCLIMATE_ENABLED -DPRESENCE_ENABLED -DIR_ENABLED "
                               "-DAUDIO_MIC_ENABLED -DVIZ_WOW_ENABLED", True),
    ("mic on, debug asserts",  "-DAUDIO_MIC_ENABLED -DAUDIO_DEBUG", True),
    ("mic only, no PC stream", "-DAUDIO_MIC_ENABLED -DAUDIO_MIC_ONLY", True),
    ("viz wow styles",         "-DVIZ_WOW_ENABLED", True),
    ("viz wow + mic",          "-DVIZ_WOW_ENABLED -DAUDIO_MIC_ENABLED", True),
    ("card clips + knob",      "-DCONTROL_ENCODER_ENABLED -DCLIPS_SD_ENABLED", True),
    ("media + bus + knob",     "-DMEDIAPLAYER_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("media + carousel + cards", "-DMEDIAPLAYER_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED "
                               "-DCAROUSEL_ENABLED -DCARDS_ENABLED", True),
    ("market + bus + knob",     "-DMARKET_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("market, local defaults",  "-DMARKET_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED "
                               r'-DMARKET_LOCAL_DEFAULTS_FILE=\"../../tools/market/panel/local_defaults_example.h\"', True),
    ("ir console only",        "-DIR_ENABLED", True),
    ("ir + receiver",          "-DIR_ENABLED -DIR_RX_ENABLED", True),
    ("ir receiver with a knob","-DIR_ENABLED -DIR_RX_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("ir receiver on the knob's IO45", "-DIR_ENABLED -DIR_RX_ENABLED -DCONTROL_ENCODER_ENABLED -DIR_PIN=45", False),
    ("ir receiver on its own", "-DIR_RX_ENABLED", False),
    ("climate + bus",          "-DCLIMATE_ENABLED -DMQTT_BUS_ENABLED", True),
    ("climate without MQTT",   "-DCLIMATE_ENABLED", True),
    ("presence + bus + scene", "-DPRESENCE_ENABLED -DMQTT_BUS_ENABLED -DNSLUA_ENABLED "
                               "-DLUA_EFFECTS_ENABLED -DCONTROL_ENCODER_ENABLED", True),
    ("presence without bus",   "-DPRESENCE_ENABLED -DNSLUA_ENABLED -DLUA_EFFECTS_ENABLED "
                               "-DCONTROL_ENCODER_ENABLED", False),
    ("presence without scene", "-DPRESENCE_ENABLED -DMQTT_BUS_ENABLED", False),
    ("cards without the bus",  "-DCARDS_ENABLED", False),
    ("world clock, no encoder", "-DWORLDCLOCK_ENABLED", False),
    ("rail board without bus", "-DRAILBOARD_ENABLED -DCONTROL_ENCODER_ENABLED", False),
    ("rail direct, no board",  "-DRAILBOARD_DIRECT_ENABLED -DMQTT_BUS_ENABLED -DCONTROL_ENCODER_ENABLED", False),
    ("flight MQTT without bus","-DFLIGHTBOARD_ENABLED -DFB_MQTT_ENABLED", False),
    ("flight direct, no board", "-DFLIGHTBOARD_DIRECT_ENABLED -DCONTROL_ENCODER_ENABLED", False),
    ("flight direct, no knob", "-DFLIGHTBOARD_ENABLED -DFLIGHTBOARD_DIRECT_ENABLED", False),
    ("carousel without knob",  "-DCAROUSEL_ENABLED", False),
    ("all styles, no carousel", "-DCONTROL_ENCODER_ENABLED -DCAROUSEL_ALL_STYLES", False),
    ("lua effects, no nslua",  "-DCONTROL_ENCODER_ENABLED -DLUA_EFFECTS_ENABLED", False),
    ("card clips, no knob",    "-DCLIPS_SD_ENABLED", False),
    ("media without the bus",  "-DMEDIAPLAYER_ENABLED -DCONTROL_ENCODER_ENABLED", False),
    ("media without the knob", "-DMEDIAPLAYER_ENABLED -DMQTT_BUS_ENABLED", False),
    ("media radio (phase 2)",  "-DMEDIAPLAYER_ENABLED -DMEDIAPLAYER_RADIO_ENABLED -DMQTT_BUS_ENABLED "
                               "-DCONTROL_ENCODER_ENABLED", False),
    ("market without the bus", "-DMARKET_ENABLED -DCONTROL_ENCODER_ENABLED", False),
    ("market without the knob", "-DMARKET_ENABLED -DMQTT_BUS_ENABLED", False),
    ("mic only, no mic",       "-DAUDIO_MIC_ONLY", False),
]

# Built from bringup/ with their own source filters. When provision.cpp was
# added next to hello_matrix.cpp the bring-up image stopped linking - two
# setup()s - and nobody knew until the hardware was on the desk.
IMAGES = ["provision", "matrix-waveshare-rgb-bringup"]

def main():
    base = (ROOT / "platformio.ini").read_text()
    # Inside this tree's own .pio, not /tmp. On 2026-09-14 a worktree and the
    # main tree ran the matrix at the same time: both wrote /tmp/pio_flag_matrix.ini,
    # so between writing its env and building it each run could pick up the
    # other's flags - a green row that was never built, the very failure this
    # script exists to prevent. Each checkout already has its own .pio/build.
    tmp  = ROOT / ".pio" / "flag_matrix.ini"
    tmp.parent.mkdir(exist_ok=True)
    bad  = 0
    for name, flags, must in COMBOS:
        tmp.write_text(base + "\n[env:flagtest]\nextends = env:matrix-waveshare-rgb\n"
                              f"build_flags =\n\t{COMMON}\n\t{flags}\n")
        r = subprocess.run([str(PIO), "run", "-e", "flagtest", "--project-conf", str(tmp)],
                           capture_output=True, text=True, cwd=ROOT)
        built = r.returncode == 0
        good  = built == must
        note  = "" if must else ("guard held" if not built else "GUARD MISSING")
        print(f"  {name:26s} {'builds' if built else 'refused':8s} "
              f"{'ok' if good else 'UNEXPECTED':10s} {note}")
        bad += not good
    tmp.unlink(missing_ok=True)
    for env in IMAGES:
        r = subprocess.run([str(PIO), "run", "-e", env], capture_output=True, text=True, cwd=ROOT)
        built = r.returncode == 0
        print(f"  {'image ' + env:26s} {'builds' if built else 'FAILS':8s} "
              f"{'ok' if built else 'UNEXPECTED':10s}")
        bad += not built
    total = len(COMBOS) + len(IMAGES)
    print(f"\n{total - bad}/{total} behaved as intended")
    sys.exit(1 if bad else 0)

main()

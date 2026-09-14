# Encoder host test

Runs the real `src/control/control.cpp` on the Mac against simulated knob signals,
one sample per simulated millisecond, through the firmware's own fallback path
(the shimmed `esp_timer_create` fails, so `controlLoop()` samples).

```bash
cd tools/control
c++ -std=c++17 -Wall -Wno-unused-function -DCONTROL_ENCODER_ENABLED -DBOARD_WAVESHARE_RGB_MATRIX \
    -I . -I ../../src/control ../../src/control/control.cpp encoder_host_test.cpp -o /tmp/enc_test && /tmp/enc_test
```

Covers a full-detent knob and a half-detent one (learned from a 00 rest), slow,
fast and bouncing turns both ways, a half click and back, and the switch: clicks,
a long hold, bouncing clicks. It proves the decoding, not the hands-on feel -
that still needs a knob turned on the real panel.

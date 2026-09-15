#!/usr/bin/env python3
"""Synthesise the WAVs the visualizer simulator and the host DSP test share.

48 kHz, 16-bit, stereo: the format src/audio/ captures from the ES7210. Both
channels carry the same programme plus their own noise, as two microphones a
few centimetres apart would. Nothing here is a recording; every file is made
from numpy so that a beat's true time is known to the sample.

  python3 tools/audiofx/gen_wavs.py        # writes tools/audiofx/wav/*.wav and *.json

The .json beside a file lists the ground truth: kick and snare times.
"""
import json
import math
import pathlib
import wave

import numpy as np

FS = 48000
HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "wav"
NOISE_DB = -78.0   # per channel, white: the quiet room plus the ADC


def db(x):
    return 10.0 ** (x / 20.0)


def tt(n):
    return np.arange(n) / FS


def blank(sec):
    return np.zeros(int(round(sec * FS)))


def place(buf, t0, sig):
    i = int(round(t0 * FS))
    n = min(len(sig), len(buf) - i)
    if n > 0:
        buf[i:i + n] += sig[:n]


def kick(amp=db(-6)):
    t = tt(int(0.35 * FS))
    f = 45.0 + 70.0 * np.exp(-t / 0.03)            # pitch drop, the thump
    ph = 2 * np.pi * np.cumsum(f) / FS
    click = 0.25 * np.exp(-t / 0.004) * np.sin(2 * np.pi * 1500 * t)
    return amp * (np.sin(ph) * np.exp(-t / 0.11) + click)


def snare(rng, amp=db(-12)):
    t = tt(int(0.25 * FS))
    n = np.diff(rng.standard_normal(len(t)), prepend=0.0)
    n /= np.abs(n).max()
    return amp * (0.6 * n * np.exp(-t / 0.06) + 0.5 * np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.05))


def hat(rng, amp=db(-26)):
    t = tt(int(0.06 * FS))
    n = np.diff(np.diff(rng.standard_normal(len(t)), prepend=0.0), prepend=0.0)
    n /= np.abs(n).max()
    return amp * n * np.exp(-t / 0.012)


def tone(freq, sec, amp):
    t = tt(int(round(sec * FS)))
    env = np.minimum(1.0, t / 0.005) * np.minimum(1.0, (sec - t) / 0.005)
    return amp * np.sin(2 * np.pi * freq * t) * env


def bass_note(freq, sec, amp=db(-15)):
    t = tt(int(sec * FS))
    s = sum(np.sin(2 * np.pi * freq * h * t) / h for h in (1, 2, 3, 4))
    return amp * s * np.minimum(1.0, t / 0.01) * np.exp(-t / 0.25) / 1.5


def chord(freqs, sec, amp=db(-22)):
    t = tt(int(sec * FS))
    s = sum(np.sin(2 * np.pi * f * t) for f in freqs) / len(freqs)
    return amp * s * np.minimum(1.0, t / 0.08) * np.clip((sec - t) / 0.1, 0.0, 1.0)


def sweep(f0, f1, sec, amp):
    t = tt(int(sec * FS))
    k = math.log(f1 / f0)
    ph = 2 * np.pi * f0 * sec / k * (np.exp(t * k / sec) - 1.0)
    return amp * np.sin(ph) * np.minimum(1.0, t / 0.02) * np.clip((sec - t) / 0.02, 0.0, 1.0)


def pink(n, rng, rms_db):
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(spec), dtype=float)
    f[0] = 1.0
    p = np.fft.irfft(spec / np.sqrt(f), n)
    return p * db(rms_db) / np.sqrt(np.mean(p ** 2))


def write(name, mono, truth=None, seed=1):
    rng = np.random.default_rng(seed)
    st = np.stack([mono + rng.normal(0, db(NOISE_DB), mono.size),
                   mono + rng.normal(0, db(NOISE_DB), mono.size)], axis=1)
    pcm = np.clip(np.round(st * 32767.0), -32768, 32767).astype("<i2")
    OUT.mkdir(exist_ok=True)
    with wave.open(str(OUT / (name + ".wav")), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(pcm.tobytes())
    (OUT / (name + ".json")).write_text(json.dumps(truth or {}, indent=1) + "\n")
    print(f"  {name + '.wav':20s} {len(mono) / FS:4.1f} s")


def main():
    rng = np.random.default_rng(7)

    # A steady 1 kHz, then 100 Hz, with silence around: band placement and the gate.
    buf = blank(4.0)
    place(buf, 0.5, tone(1000, 1.5, db(-12)))
    place(buf, 2.5, tone(100, 1.0, db(-12)))
    write("tone_1k_100", buf, {"tones": [[0.5, 2.0, 1000], [2.5, 3.5, 100]]})

    # Log sweep: every band lights in turn, left to right.
    buf = blank(4.5)
    place(buf, 0.25, sweep(50, 16000, 4.0, db(-12)))
    write("sweep", buf, {"sweep": [0.25, 4.25, 50, 16000]})

    # Kick on every beat at 120 BPM, hats on the off-beats, a quiet pink bed.
    buf = pink(int(6.0 * FS), rng, -50)
    kicks = [0.25 + 0.5 * i for i in range(12)]
    for t0 in kicks:
        place(buf, t0, kick())
        place(buf, t0 + 0.25, hat(rng))
    write("kick120", buf, {"kicks": kicks})

    # Pink noise at a moderate level: AGC settles, no beats should fire.
    write("pink", pink(int(3.0 * FS), rng, -30), {"kicks": []})

    # Two seconds of room noise, a clipped 200 Hz burst, silence again.
    buf = blank(4.0)
    place(buf, 2.0, np.clip(tone(200, 1.0, 1.5), -1.0, 1.0))
    write("silence_clip", buf, {"clip": [2.0, 3.0]})

    # The showreel every GIF is rendered from: bass, a full groove, a sweep,
    # silence with 50 Hz hum below the gate, and the groove coming back.
    buf = blank(5.0)
    place(buf, 0.0, tone(50, 5.0, db(-74)))
    kicks, snares = [], []
    for t0 in (0.10, 0.60, 1.10):
        place(buf, t0, kick())
        place(buf, t0, bass_note(55, 0.45))
        kicks.append(t0)
    notes = (55, 65.4, 73.4, 49)
    for i, t0 in enumerate((1.60, 2.10)):
        place(buf, t0, kick())
        place(buf, t0, bass_note(notes[i], 0.45))
        place(buf, t0 + 0.25, snare(rng))
        for h in range(4):
            place(buf, t0 + h * 0.125, hat(rng))
        kicks.append(t0)
        snares.append(round(t0 + 0.25, 3))
    place(buf, 1.35, chord((220, 277.2, 329.6), 1.15))
    place(buf, 2.50, sweep(80, 12000, 0.8, db(-14)))
    for i, t0 in enumerate((4.10, 4.60)):
        place(buf, t0, kick())
        place(buf, t0, bass_note(notes[i + 2], 0.40))
        place(buf, t0 + 0.25, hat(rng))
        kicks.append(t0)
    place(buf, 4.10, chord((196, 246.9, 293.7), 0.85))
    write("showreel", buf, {"kicks": kicks, "snares": snares,
                            "sections": [[0.0, 1.3, "kick and bass"], [1.3, 2.5, "full groove"],
                                         [2.5, 3.3, "sweep 80 Hz to 12 kHz"],
                                         [3.3, 4.1, "silence, 50 Hz hum at -74 dBFS"],
                                         [4.1, 5.0, "groove returns"]]})


if __name__ == "__main__":
    main()

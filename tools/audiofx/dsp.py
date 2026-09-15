#!/usr/bin/env python3
"""The onboard-microphone DSP chain in Python: the reference that
src/audio/audio_dsp.cpp is tested against (tools/audiofx/host).

One analysis frame per HOP new samples, over a Hann window of the newest N:

  mono = (L + R) / 2
  -> 32 log bands 50..16000 Hz, mapped to FFT bins exactly as the PC companion
     maps them (PC-Companion-App-v4/companion-common/audio_spectrum.py)
  -> a noise gate on the frame's RMS level, AGC over a 38 dB span like the companion
  -> 0..255 band bytes: the "FFT1" packet the visualizer already reads
  -> attack/release levels, peak hold with gravity, bass/mid/treble
  -> spectral flux over the bass bands against an adaptive threshold: beats
  -> a trigger-aligned 128-point waveform, computed as the companion computes it

Every default below is a starting value to tune on the panel unless its comment
names a source. docs/22 in the KB says which is which.

  python3 tools/audiofx/dsp.py tools/audiofx/wav/kick120.wav out.csv
"""
import csv
import math
import pathlib
import sys
import wave

import numpy as np

FS = 48000                  # 256*fs = 12.288 MHz MCLK, a pair es7210.c lists for 48 kHz
N = 2048                    # window and FFT: 23.4 Hz bins, the companion's FFT_N
HOP = 960                   # 20 ms: 50 analysis frames per second
BANDS = 32                  # audio_spectrum.py BANDS, visualizer.h VIZ_BANDS
F_LO, F_HI = 50.0, 16000.0  # audio_spectrum.py F_LO, F_HI
PUBLISH_EVERY = 2           # a legacy packet every 40 ms: the companion's cadence

AGC_RANGE_DB = 38.0         # audio_spectrum.py AGC_RANGE_DB
AGC_DECAY_DB_PER_S = 1.5    # audio_spectrum.py AGC_DECAY_DB_PER_S
AGC_FLOOR_DB = -70.0        # dBFS. Starting value: the companion's -55 is on an unnormalised scale
FIXED_REF_DB = -20.0        # AGC off: a band at this level draws full height. Starting value
GATE_DB = -60.0             # frame RMS in dBFS below which the bands go dark. Starting value
GATE_HOLD_FRAMES = 12       # 240 ms, so a decaying note does not chatter the gate
ATTACK_S, RELEASE_S = 0.012, 0.200
PEAK_HOLD_S = 0.35
PEAK_GRAVITY = 2.5          # full heights per second squared
FLUX_BANDS = 10             # bands 0..9, 50..~300 Hz: where a kick drum lives
FLUX_FLOOR_DB = -90.0       # flux reads band dB floored here, never at the AGC floor
FLUX_HISTORY = 50           # one second of flux behind the adaptive threshold
FLUX_K = 2.5                # threshold = mean + K * std of that second ...
FLUX_DELTA_DB = 3.0         # ... and never below this
BEAT_RISE_DB = 3.0          # and the bass must stand this far above its own last-second mean
# The four beat numbers above were chosen on the six synthetic WAVs (tools/audiofx/README.md):
# reference values until they are checked on real music through the microphones.
BEAT_WAIT_FRAMES = 8        # 160 ms, the starfield's own cooldown (src/viz/starfield.cpp)
CLIP_LEVEL = 32700          # |sample| at or above this counts as clipping
CLIP_HOLD_FRAMES = 25       # the flag stays up 500 ms
SILENT_FRAMES = 100         # gated this long (2 s) = silence
WAVE_POINTS, WAVE_DECIM, WAVE_SPAN = 128, 8, 1920               # audio_spectrum.py
WAVE_AGC_DECAY, WAVE_AGC_FLOOR, WAVE_GAIN = 0.55, 0.02, 118.0   # audio_spectrum.py
BPM_MIN_S, BPM_MAX_S = 0.30, 1.00                               # 60..200 BPM


def band_bins():
    """(lo, hi) FFT bin span of each band, as audio_spectrum.py computes it."""
    bin_hz = FS / float(N)
    edges = [F_LO * (F_HI / F_LO) ** (i / BANDS) for i in range(BANDS + 1)]
    spans = []
    for i in range(BANDS):
        lo = int(edges[i] / bin_hz)
        hi = max(lo + 1, int(edges[i + 1] / bin_hz))
        spans.append((lo, hi))
    return spans


def load_wav(path):
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != FS or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: needs {FS} Hz 16-bit")
        ch = w.getnchannels()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)
    if ch == 1:
        pcm = np.repeat(pcm, 2, axis=1)
    return pcm[:, :2]


def median(values):
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


class Dsp:
    def __init__(self, agc=True, gate_db=GATE_DB):
        self.agc = agc
        self.gate_db = gate_db
        n = np.arange(N)
        self.win = (0.5 - 0.5 * np.cos(2 * np.pi * n / (N - 1))).astype(np.float32)
        self.norm = 2.0 / float(np.sum(self.win, dtype=np.float64))   # full-scale sine = 0 dBFS
        self.bins = band_bins()
        self.ring = np.zeros(N, np.float32)
        self.filled = 0
        self.k = 0
        self.ref = AGC_FLOOR_DB
        self.gate_hold = 0
        self.gated_run = 0
        self.prev_x = None
        self.flux_hist = []
        self.bass_hist = []
        self.prev_flux = 0.0
        self.since_beat = BEAT_WAIT_FRAMES
        self.level = np.zeros(BANDS)
        self.peak = np.zeros(BANDS)
        self.peak_hold = np.zeros(BANDS)
        self.peak_vel = np.zeros(BANDS)
        self.clip_hold = 0
        self.beat_times = []
        self.beats = 0
        self.bpm = 0.0
        self.wave_ref = WAVE_AGC_FLOOR
        self.wave = np.full(WAVE_POINTS, 128, np.uint8)
        self.packets = 0

    def process_hop(self, hop):
        """hop: HOP x 2 int16 samples. Returns a frame once N samples are in, else None."""
        hop = np.asarray(hop, dtype=np.int16)
        mono = (hop[:, 0].astype(np.float32) + hop[:, 1].astype(np.float32)) * np.float32(0.5 / 32768.0)
        self.ring[:-HOP] = self.ring[HOP:]
        self.ring[-HOP:] = mono
        self.filled += HOP
        peak_abs = int(np.abs(hop.astype(np.int32)).max())
        self.clip_hold = CLIP_HOLD_FRAMES if peak_abs >= CLIP_LEVEL else max(0, self.clip_hold - 1)
        if self.filled < N:
            return None
        dt = HOP / float(FS)

        spec = np.abs(np.fft.rfft((self.ring * self.win).astype(np.float64)))[:N // 2] * self.norm
        amps = np.array([math.sqrt(float(np.mean(spec[lo:hi] ** 2))) for lo, hi in self.bins])
        band_db = 20.0 * np.log10(amps + 1e-7)

        rms = math.sqrt(float(np.mean(mono.astype(np.float64) ** 2)))
        level_db = max(-120.0, 20.0 * math.log10(max(rms, 1e-7)))
        self.gate_hold = GATE_HOLD_FRAMES if level_db >= self.gate_db else max(0, self.gate_hold - 1)
        gated = self.gate_hold == 0
        self.gated_run = self.gated_run + 1 if gated else 0

        if self.agc:
            decayed = self.ref - AGC_DECAY_DB_PER_S * dt
            self.ref = max(decayed, AGC_FLOOR_DB) if gated else max(decayed, float(band_db.max()), AGC_FLOOR_DB)
            ref = self.ref
        else:
            ref = FIXED_REF_DB
        lo_db = ref - AGC_RANGE_DB
        if gated:
            bands8 = np.zeros(BANDS, np.uint8)
        else:
            bands8 = np.clip((band_db - lo_db) / AGC_RANGE_DB * 255.0, 0, 255).astype(np.uint8)

        x = np.maximum(band_db[:FLUX_BANDS], FLUX_FLOOR_DB)
        bass_db = float(x.mean())
        if self.prev_x is None or gated:
            flux = 0.0
        else:
            flux = float(np.sum(np.maximum(x - self.prev_x, 0.0))) / FLUX_BANDS
        self.prev_x = x
        n = len(self.flux_hist)
        if n:
            mean = sum(self.flux_hist) / n
            std = math.sqrt(sum((h - mean) ** 2 for h in self.flux_hist) / n)
            rise_ok = bass_db - sum(self.bass_hist) / n >= BEAT_RISE_DB
        else:
            mean = std = 0.0
            rise_ok = True
        thr = max(FLUX_DELTA_DB, mean + FLUX_K * std)
        self.since_beat += 1
        beat = ((not gated) and flux >= thr and flux > self.prev_flux
                and self.since_beat >= BEAT_WAIT_FRAMES and rise_ok)
        self.flux_hist.append(flux)
        self.bass_hist.append(bass_db)
        if n + 1 > FLUX_HISTORY:
            self.flux_hist.pop(0)
            self.bass_hist.pop(0)
        self.prev_flux = flux
        t = (self.filled / float(FS))
        strength = 0.0
        if beat:
            self.since_beat = 0
            self.beats += 1
            strength = min(1.0, (flux - thr) / max(thr, 1e-3))
            self.beat_times.append(t)
            self.beat_times = self.beat_times[-9:]
            iv = [b - a for a, b in zip(self.beat_times, self.beat_times[1:]) if BPM_MIN_S <= b - a <= BPM_MAX_S]
            if len(iv) >= 3:
                self.bpm = 60.0 / median(iv)
        if self.beat_times and t - self.beat_times[-1] > 3.0:
            self.bpm = 0.0

        target = bands8 / 255.0
        ca = 1.0 - math.exp(-dt / ATTACK_S)
        cr = 1.0 - math.exp(-dt / RELEASE_S)
        for i in range(BANDS):
            c = ca if target[i] > self.level[i] else cr
            self.level[i] += (target[i] - self.level[i]) * c
            if self.level[i] >= self.peak[i]:
                self.peak[i] = self.level[i]
                self.peak_hold[i] = PEAK_HOLD_S
                self.peak_vel[i] = 0.0
            elif self.peak_hold[i] > 0.0:
                self.peak_hold[i] -= dt
            else:
                self.peak_vel[i] += PEAK_GRAVITY * dt
                self.peak[i] = max(self.level[i], self.peak[i] - self.peak_vel[i] * dt)

        published = self.k % PUBLISH_EVERY == 0
        if published:
            low = self.ring[N - WAVE_SPAN:].astype(np.float64).reshape(-1, WAVE_DECIM).mean(axis=1)
            limit = len(low) - WAVE_POINTS
            head = low[:limit + 1]
            rising = np.nonzero((head[:-1] <= 0.0) & (head[1:] > 0.0))[0]
            start = int(rising[0]) + 1 if rising.size else 0
            seg = low[start:start + WAVE_POINTS]
            dtw = HOP * PUBLISH_EVERY / float(FS)
            self.wave_ref = max(self.wave_ref * (WAVE_AGC_DECAY ** dtw), float(np.abs(seg).max()), WAVE_AGC_FLOOR)
            self.wave = np.clip(seg / self.wave_ref * WAVE_GAIN + 128.0, 0, 255).astype(np.uint8)
            self.packets += 1

        frame = {
            "k": self.k, "t": t, "published": published, "packets": self.packets,
            "bands8": bands8, "wave": self.wave.copy(),
            "level": self.level.copy(), "peak": self.peak.copy(),
            "bass": float(self.level[0:8].mean()), "mid": float(self.level[8:24].mean()),
            "treble": float(self.level[24:32].mean()),
            "level_db": level_db, "ref_db": ref, "gated": gated,
            "silent": self.gated_run >= SILENT_FRAMES, "clipping": self.clip_hold > 0,
            "flux": flux, "thr": thr, "beat": beat, "strength": strength,
            "beats": self.beats, "bpm": self.bpm,
        }
        self.k += 1
        return frame


def run(path, **kw):
    pcm = load_wav(path)
    dsp = Dsp(**kw)
    frames = []
    for i in range(0, len(pcm) - HOP + 1, HOP):
        f = dsp.process_hop(pcm[i:i + HOP])
        if f is not None:
            frames.append(f)
    return frames


CSV_HEADER = (["k", "t_ms", "gated", "beat", "clip", "silent", "published", "level_db", "ref_db",
               "flux", "thr", "bpm"] + [f"b{i}" for i in range(BANDS)] + [f"w{i}" for i in range(WAVE_POINTS)])


def write_csv(frames, out):
    with open(out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(CSV_HEADER)
        for f in frames:
            w.writerow([f["k"], int(round(f["t"] * 1000)), int(f["gated"]), int(f["beat"]), int(f["clipping"]),
                        int(f["silent"]), int(f["published"]), f"{f['level_db']:.2f}", f"{f['ref_db']:.2f}",
                        f"{f['flux']:.3f}", f"{f['thr']:.3f}", f"{f['bpm']:.1f}"]
                       + [int(v) for v in f["bands8"]] + [int(v) for v in f["wave"]])


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    frames = run(pathlib.Path(argv[1]))
    write_csv(frames, argv[2])
    beats = [round(f["t"], 3) for f in frames if f["beat"]]
    print(f"{argv[1]}: {len(frames)} frames, {len(beats)} beats at {beats}")


if __name__ == "__main__":
    main(sys.argv)

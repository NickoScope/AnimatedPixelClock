"""PcFrameDeriver in Python: the frame styles 7-14 (and the new Matrix Rain)
draw from, rebuilt from a PC companion packet. Mirrors
src/viz/wow/viz_frame.cpp line by line, in float32 like the C++, so a preview
fed "from the PC" shows what the panel would.
"""
import math

import numpy as np

F = np.float32
DB_PER_STEP = F(38.0) / F(255.0)
ATTACK_S, RELEASE_S = F(0.012), F(0.200)
PEAK_HOLD_S, PEAK_GRAVITY = F(0.35), F(2.5)
FLUX_K, FLUX_DELTA_DB, BEAT_RISE_DB = F(2.5), F(3.0), F(3.0)
BEAT_WAIT_S = F(0.16)
FLUX_BANDS, HISTORY = 10, 25


class PcFrameDeriver:
    def __init__(self):
        self.prev = [F(0)] * FLUX_BANDS
        self.have_prev = False
        self.flux_hist = [F(0)] * HISTORY
        self.bass_hist = [F(0)] * HISTORY
        self.hist_len = 0
        self.hist_pos = 0
        self.prev_flux = F(0)
        self.since_beat = F(1)
        self.level = [F(0)] * 32
        self.peak = [F(0)] * 32
        self.peak_hold = [F(0)] * 32
        self.peak_vel = [F(0)] * 32

    def feed(self, bands, wave, dt):
        dt = F(min(max(dt, 0.01), 0.1))
        bands = [int(b) for b in bands]
        gated = not any(bands)
        x = [F(bands[i]) * DB_PER_STEP for i in range(FLUX_BANDS)]
        bass_db = F(0)
        for v in x:
            bass_db += v
        bass_db /= F(FLUX_BANDS)
        flux = F(0)
        if self.have_prev and not gated:
            for i in range(FLUX_BANDS):
                flux += max(F(0), x[i] - self.prev[i])
            flux /= F(FLUX_BANDS)
        self.prev = x
        self.have_prev = True
        mean, sd, rise_ok = F(0), F(0), True
        if self.hist_len:
            bass_avg = F(0)
            for i in range(self.hist_len):
                mean += self.flux_hist[i]
                bass_avg += self.bass_hist[i]
            mean /= F(self.hist_len)
            bass_avg /= F(self.hist_len)
            for i in range(self.hist_len):
                sd += (self.flux_hist[i] - mean) * (self.flux_hist[i] - mean)
            sd = F(math.sqrt(sd / F(self.hist_len)))
            rise_ok = bass_db - bass_avg >= BEAT_RISE_DB
        thr = max(FLUX_DELTA_DB, mean + FLUX_K * sd)
        self.since_beat += dt
        beat = (not gated) and flux >= thr and flux > self.prev_flux and self.since_beat >= BEAT_WAIT_S and rise_ok
        self.flux_hist[self.hist_pos] = flux
        self.bass_hist[self.hist_pos] = bass_db
        self.hist_pos = (self.hist_pos + 1) % HISTORY
        self.hist_len = min(HISTORY, self.hist_len + 1)
        self.prev_flux = flux
        strength = F(0)
        if beat:
            self.since_beat = F(0)
            strength = min(F(1), (flux - thr) / max(thr, F(1e-3)))
        ca = F(1) - F(math.exp(-dt / ATTACK_S))
        cr = F(1) - F(math.exp(-dt / RELEASE_S))
        bass = mid = treble = F(0)
        for b in range(32):
            target = F(bands[b]) / F(255)
            self.level[b] += (target - self.level[b]) * (ca if target > self.level[b] else cr)
            if self.level[b] >= self.peak[b]:
                self.peak[b] = self.level[b]
                self.peak_hold[b] = PEAK_HOLD_S
                self.peak_vel[b] = F(0)
            elif self.peak_hold[b] > F(0):
                self.peak_hold[b] -= dt
            else:
                self.peak_vel[b] += PEAK_GRAVITY * dt
                self.peak[b] = max(self.level[b], self.peak[b] - self.peak_vel[b] * dt)
            if b < 8:
                bass += self.level[b]
            elif b < 24:
                mid += self.level[b]
            else:
                treble += self.level[b]
        return {"bands8": list(bands), "wave": [int(w) for w in wave],
                "level": [float(v) for v in self.level], "peak": [float(v) for v in self.peak],
                "bass": float(bass / F(8)), "mid": float(mid / F(16)), "treble": float(treble / F(8)),
                "strength": float(strength), "beat": bool(beat), "clipping": False, "steps": 2}


def mic_frames(dsp_frames):
    """dsp.run() frames as VizFrame-shaped dicts, each with its time."""
    out = []
    for f in dsp_frames:
        out.append({"t": f["t"], "bands8": [int(v) for v in f["bands8"]], "wave": [int(v) for v in f["wave"]],
                    "level": [float(v) for v in f["level"]], "peak": [float(v) for v in f["peak"]],
                    "bass": f["bass"], "mid": f["mid"], "treble": f["treble"], "strength": f["strength"],
                    "beat": f["beat"], "clipping": f["clipping"], "steps": 1})
    return out


def pc_frames(dsp_frames):
    """What a PC companion would deliver: the DSP's 40 ms packets through PcFrameDeriver."""
    d, out = PcFrameDeriver(), []
    for f in dsp_frames:
        if f["published"]:
            v = d.feed(f["bands8"], f["wave"], 0.04)
            v["t"] = f["t"]
            out.append(v)
    return out

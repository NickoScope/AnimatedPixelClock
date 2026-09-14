#!/usr/bin/env python3
"""Host checks for the clip path: the portal's clip maker against the Python.

  python3 tools/oscmusic/test_clip_maker.py [WAV@START+DUR ...]

The page's renderer, WAV reader and PCA1 writer live in src/web/web_panel_js.h,
between "make a clip" and "The card". This pulls that block out of the header
and runs it in JavaScriptCore's shell, which macOS ships (node is not needed):

  1. frames: the JS renderer against scope_render.py on synthetic signals at
     192 and 96 kHz, and on spans of real tracks given as arguments, e.g.
     "Blocks.wav@2:15+14" (any rate, 16/24-bit, any channel count);
  2. the same synthetic sound as 24-bit, 32-bit float and an extensible WAV
     with a chunk ahead of the data: the JS must draw identical frames;
  3. the WAV reader reads the header and the span only: every slice it asks
     for is logged and must fall inside what the host copied out;
  4. PCA bytes: the JS writer against gif2pca's build_pca on the same frames;
  5. every JS clip through the firmware's own validator, compiled here from
     src/ambient/anim_store.cpp, and the size limits of the stores;
  6. the card's read-ahead stream (src/clips/clip_stream.cpp) on threads: frame
     order, delays, the wrap to frame 0, underruns held and counted.

Audio and clips go to a temporary directory that is removed afterwards; the
tracks given as arguments are only read. Nothing is written into the repo.
"""
import json, os, pathlib, shutil, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST = ROOT / "tools/oscmusic/host"
sys.path.insert(0, str(ROOT / "tools/oscmusic"))
sys.path.insert(0, str(ROOT / "PC-Companion-App-v4/companion-common"))
import scope_render as SR      # noqa: E402
import gif_converter as GC     # noqa: E402

JSC = "/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
FAILED = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        FAILED.append(what)


# ---------------------------------------------------------------- sound
def synth(rate, secs):
    """A turning square with the beam slowing at its corners, then a
    Lissajous figure driven past full scale so the edge clamp is exercised."""
    t = np.arange(int(rate * secs)) / rate
    half = len(t) // 2
    tt = t[:half]
    p = ((tt * 60.0) % 1.0) * 4.0
    e = np.floor(p).astype(int)
    s = p - e
    s = s * s * (3 - 2 * s)
    cx = np.array([-1, 1, 1, -1, -1.0])
    cy = np.array([-1, -1, 1, 1, -1.0])
    x = cx[e] + (cx[e + 1] - cx[e]) * s
    y = cy[e] + (cy[e + 1] - cy[e]) * s
    a = 1.3 * tt
    k = 0.8 / np.sqrt(2)
    X1, Y1 = k * (x * np.cos(a) - y * np.sin(a)), k * (x * np.sin(a) + y * np.cos(a))
    tl = t[half:]
    X2 = 1.02 * np.sin(2 * np.pi * 300 * tl)
    Y2 = 1.02 * np.sin(2 * np.pi * 200 * tl + 0.9 * tl)
    return np.stack([np.concatenate([X1, X2]), np.concatenate([Y1, Y2])], 1)


def to_i16(v):
    return np.clip(np.round(v * 32768), -32768, 32767).astype("<i2")


def wav_bytes(i16, rate, kind):
    """kind: pcm16, pcm24, f32, or ext24 (WAVE_FORMAT_EXTENSIBLE with a LIST
    chunk of odd length, so one pad byte, ahead of the data)."""
    if i16.ndim == 1:
        i16 = i16[:, None]
    ch = i16.shape[1]
    if kind == "pcm16":
        tag, bits, data = 1, 16, i16.astype("<i2").tobytes()
    elif kind in ("pcm24", "ext24"):
        v = i16.astype(np.int32).reshape(-1) * 256
        b = np.empty((v.size, 3), np.uint8)
        b[:, 0], b[:, 1], b[:, 2] = v & 255, (v >> 8) & 255, (v >> 16) & 255
        tag, bits, data = 1, 24, b.tobytes()
    else:
        tag, bits, data = 3, 32, (i16.astype(np.float32) / 32768).astype("<f4").tobytes()
    ba = ch * bits // 8
    extra = b""
    if kind == "ext24":
        guid_tail = bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, ch, rate, rate * ba, ba, bits, 22, bits, 3) + struct.pack("<H", 1) + guid_tail
        extra = b"LIST" + struct.pack("<I", 5) + b"INFOx\x00"
    else:
        fmt = struct.pack("<HHIIHH", tag, ch, rate, rate * ba, ba, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + extra + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def wav_info(path):
    """The chunk walk, independently of the JS: format, rate, data offset."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(12)
        assert head[:4] == b"RIFF" and head[8:12] == b"WAVE", path
        pos, info = 12, {}
        while True:
            f.seek(pos)
            h = f.read(8)
            if len(h) < 8:
                raise ValueError("no data chunk in " + str(path))
            cid, ln = h[:4], struct.unpack("<I", h[4:])[0]
            if cid == b"fmt ":
                b = f.read(min(ln, 40))
                tag, ch, rate, _, ba, bits = struct.unpack("<HHIIHH", b[:16])
                if tag == 0xFFFE:
                    tag = struct.unpack("<H", b[24:26])[0]
                info.update(tag=tag, ch=ch, rate=rate, ba=ba, bits=bits)
            if cid == b"data":
                off = pos + 8
                info.update(off=off, n=min(ln, size - off) // info["ba"], size=size)
                return info
            pos += 8 + ln + (ln & 1)


def decode_lr(raw, info):
    """First two channels as float32, the way the page scales each width."""
    bps = info["ba"] // info["ch"]
    n = len(raw) // info["ba"]
    a = np.frombuffer(raw[: n * info["ba"]], np.uint8).reshape(n, info["ch"], bps)[:, :2, :]
    if info["tag"] == 3:
        return a.copy().view("<f4")[..., 0].astype(np.float32)
    if bps == 2:
        return a.copy().view("<i2")[..., 0].astype(np.float32) / 32768
    if bps == 3:
        v = a[..., 0].astype(np.int32) | a[..., 1].astype(np.int32) << 8 | a[..., 2].astype(np.int32) << 16
        return ((v << 8) >> 8).astype(np.float32) / 8388608
    raise ValueError("width %d not covered here" % bps)


def py_frames(lr, rate, n):
    frames, _ = SR.render(rate, lr, n, 48000.0 / rate)
    return np.stack(frames)


def palette565():
    return [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in SR.palette16()]


def py_pca(frames, ms=40):
    ims = [Image.frombytes("P", (128, 64), f.tobytes()) for f in frames]
    return GC.build_pca(ims, palette565(), [ms] * len(frames))


# ---------------------------------------------------------------- the JS side
JS_DRIVER = r"""
var log = [];
function proxyFile(size, segs) {   // a File whose slices come only from the spans the host copied
  var parts = segs.map(function (s) { return { a: s[0], u: readFile(s[1], 'binary') }; });
  return { size: size, slice: function (a, b) {
    b = Math.min(b === undefined ? size : b, size); a = Math.min(a, b);
    log.push([a, b]);
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i];
      if (a >= p.a && b <= p.a + p.u.length) { var c = p.u.slice(a - p.a, b - p.a); return { arrayBuffer: function () { return Promise.resolve(c.buffer); } }; }
    }
    throw new Error('read outside the copied spans: ' + a + '-' + b);
  } };
}
function wavTask(T) {
  log = [];
  return wavOpen(proxyFile(T.size, T.segs)).then(function (src) {
    if (!src) throw new Error('wavOpen gave null');
    var o = { rate: src.rate, fps: T.fps, gain: T.gain, decay: T.decay, zoom: T.zoom }, sc = scope(o), spf = sc.spf;
    return src.read(T.at, T.n * spf).then(function (s) {
      var out = new Uint8Array(T.n * 8192), idx = new Uint8Array(8192), head = pcaHead(T.n, 1000 / T.fps, scopePal(T.col)[1]),
        pca = new Uint8Array(head.length + T.n * 4096), i;
      pca.set(head);
      for (i = 0; i < T.n; i++) { sc.frame(s, i * spf, idx); out.set(idx, i * 8192); pcaPut(pca, head.length + i * 4096, idx); }
      writeFile(T.out + '.frames', out);
      writeFile(T.out + '.pca', pca);
      return { id: T.id, rate: src.rate, ch: src.ch, n: src.n, spf: spf, wav: src.wav, reads: log, flat: flat(s) };
    });
  });
}
function encTask(T) {
  var u = readFile(T.frames, 'binary'), n = u.length / 8192, head = pcaHead(n, T.ms, scopePal(T.col)[1]),
    all = new Uint8Array(head.length + n * 4096);
  all.set(head);
  for (var i = 0; i < n; i++) pcaPut(all, head.length + i * 4096, u.subarray(i * 8192, (i + 1) * 8192));
  writeFile(T.out, all);
  return Promise.resolve({ id: T.id, headBytes: pcaHead(12000, 40, scopePal(0)[1]).length });
}
var results = [], chain = Promise.resolve();
TASKS.forEach(function (T) {
  chain = chain.then(function () { return T.kind === 'enc' ? encTask(T) : wavTask(T); })
    .then(function (r) { results.push(r); }, function (e) { results.push({ id: T.id, error: String(e) }); });
});
chain.then(function () { print(JSON.stringify({ results: results, pal: [0, 1, 2, 3].map(function (c) { return scopePal(c)[1]; }) })); });
"""


def js_sources():
    t = (ROOT / "src/web/web_panel_js.h").read_text()
    whole = t[t.index('R"JS(') + 5:t.rindex(')JS"')]
    a = whole.index("// ---------------------------------------------------------------- make a clip")
    b = whole.index("\n// The card.")
    return whole, whole[a:b]


def run_js(tmp, block, tasks):
    script = tmp / "driver.js"
    script.write_text(block + "\nvar TASKS = " + json.dumps(tasks) + ";\n" + JS_DRIVER)
    r = subprocess.run([JSC, str(script)], capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        print(r.stdout, r.stderr)
        raise SystemExit("jsc failed")
    out = json.loads(r.stdout.strip().splitlines()[-1])
    return {x["id"]: x for x in out["results"]}, out["pal"]


def compare(label, js_path, ref):
    js = np.frombuffer(pathlib.Path(js_path).read_bytes(), np.uint8).reshape(-1, 64, 128)
    n = min(len(js), len(ref))
    d = np.abs(js[:n].astype(int) - ref[:n].astype(int))
    bad = int((d > 0).sum())
    lit = int((ref[:n] > 0).sum())
    print(f"        {label}: {n} frames, {lit} lit pixels, {bad} differ, max difference {int(d.max())}")
    return n, bad, int(d.max()), lit, d.size


# ---------------------------------------------------------------- host C++
def build_host(tmp, name, sources, flags=()):
    exe = tmp / name
    cmd = ["c++", "-std=c++17", "-O2", "-pthread", "-DCLIP_HOST_TEST", "-I", str(HOST / "stubs"),
           "-I", str(ROOT / "src/ambient"), "-I", str(ROOT / "src/clips"), *flags, *map(str, sources), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    check(r.returncode == 0, f"{name} builds on the host" + ("" if r.returncode == 0 else ":\n" + r.stderr[-1500:]))
    return exe if r.returncode == 0 else None


def validate(exe, maxf, paths):
    r = subprocess.run([str(exe), str(maxf), *map(str, paths)], capture_output=True, text=True)
    return {line.split(" ", 2)[2]: (line[0] == "1", int(line.split(" ")[1])) for line in r.stdout.splitlines()}


def sparse_pca(path, frames, palette=16):
    """Header plus the exact length the validator expects, as a sparse file."""
    with open(path, "wb") as f:
        f.write(b"PCA1" + struct.pack("<HHBBH", frames & 0xFFFF, 40, palette, 1, 0))
        f.truncate(12 + palette * 2 + frames * (2 + 4096))


def clip_sd_cap():
    t = (ROOT / "src/clips/clip_sd.h").read_text() if (ROOT / "src/clips/clip_sd.h").exists() else ""
    for line in t.splitlines():
        if line.startswith("#define CLIP_SD_MAX_FRAMES"):
            return int(line.split()[2])
    return None


# ---------------------------------------------------------------- main
def parse_span(spec):
    path, _, when = spec.rpartition("@") if "@" in spec else (spec, "", "")
    start, _, dur = when.partition("+")
    sec = lambda s: sum(float(p) * 60 ** i for i, p in enumerate(reversed(s.split(":")))) if s else None
    return pathlib.Path(path), sec(start) or 0.0, sec(dur) or 4.0


def main():
    specs = sys.argv[1:]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="clipmaker-"))
    try:
        whole, block = js_sources()
        r = subprocess.run([JSC, "-e", "new Function(read(%s)); print('ok')" % json.dumps(str(tmp / "panel.js"))],
                           capture_output=True, text=True, input=None) if (tmp / "panel.js").write_text(whole) else None
        check(r.stdout.strip() == "ok", "panel.js parses in JavaScriptCore (%d bytes)" % len(whole.encode()))

        tasks, refs, wavs = [], {}, {}
        # Synthetic: 2.4 s, 60 frames, at both rates; four encodings at 192 kHz.
        for rate in (192000, 96000):
            i16 = to_i16(synth(rate, 2.4))
            for kind in (("pcm16", "pcm24", "f32", "ext24") if rate == 192000 else ("pcm16",)):
                p = tmp / f"syn{rate}_{kind}.wav"
                p.write_bytes(wav_bytes(i16, rate, kind))
                tid = f"syn{rate}_{kind}"
                tasks.append(dict(kind="wav", id=tid, size=p.stat().st_size, segs=[[0, str(p)]], at=0, n=60, fps=25,
                                  gain=1, decay=0.55, zoom=1, col=0, out=str(tmp / tid)))
            refs[f"syn{rate}_pcm16"] = py_frames(i16.astype(np.float32) / 32768, rate, 60)
        # The other settings: they have no Python twin, so they must only make valid clips.
        base = tasks[0]
        tasks.append(dict(base, id="syn_zoom_amber_20fps", zoom=1.6, col=1, fps=20, gain=2, decay=0.7, out=str(tmp / "syn_z")))
        # Mono, and stereo whose channels part by less than half a pixel (0.022): both only a diagonal line.
        tone = 0.8 * np.sin(2 * np.pi * 110 * np.arange(96000) / 48000)
        for tid, sound in (("mono", to_i16(tone)), ("twin", to_i16(np.stack([tone, tone + 0.01 * np.sin(np.arange(96000))], 1)))):
            p = tmp / f"{tid}.wav"
            p.write_bytes(wav_bytes(sound, 48000, "pcm16"))
            tasks.append(dict(base, id=tid, size=p.stat().st_size, segs=[[0, str(p)]], n=40, out=str(tmp / tid)))

        # Real spans: the host copies out the header and the span, nothing else.
        for i, spec in enumerate(specs):
            path, start, dur = parse_span(spec)
            info = wav_info(path)
            spf = info["rate"] // 25
            at = int(start * info["rate"])
            n = min(int(dur * 25), (info["n"] - at) // spf)
            a = info["off"] + at * info["ba"]
            hp, sp = tmp / f"real{i}.head", tmp / f"real{i}.span"
            with open(path, "rb") as f:
                hp.write_bytes(f.read(info["off"] + 64))
                f.seek(a)
                raw = f.read(n * spf * info["ba"])
            sp.write_bytes(raw)
            tid = f"real{i}"
            tasks.append(dict(kind="wav", id=tid, size=info["size"], segs=[[0, str(hp)], [a, str(sp)]], at=at, n=n, fps=25,
                              gain=1, decay=0.55, zoom=1, col=0, out=str(tmp / tid)))
            lr = decode_lr(raw, info)
            if info["bits"] == 16 and info["tag"] == 1 and i == 0:
                _, span, _ = SR.read_span(str(path), start, n * spf / info["rate"])
                check(np.array_equal(span[: len(lr)], lr), f"{path.name}: this test's reader matches scope_render.read_span")
            refs[tid] = py_frames(lr, info["rate"], n)
            wavs[tid] = (path, info, start, n, len(raw))
            del raw, lr

        # Python frames through the JS writer, for the byte comparison.
        np.stack(refs["syn192000_pcm16"]).tofile(tmp / "pyframes.bin")
        tasks.append(dict(kind="enc", id="enc_syn", frames=str(tmp / "pyframes.bin"), ms=40, col=0, out=str(tmp / "enc_syn.pca")))
        if specs:
            refs["real0"].tofile(tmp / "pyframes_real0.bin")
            tasks.append(dict(kind="enc", id="enc_real0", frames=str(tmp / "pyframes_real0.bin"), ms=40, col=0, out=str(tmp / "enc_real0.pca")))

        res, pal = run_js(tmp, block, tasks)
        errors = [f"{k}: {v['error']}" for k, v in res.items() if "error" in v]
        check(not errors, "every JS task ran" + ("" if not errors else ": " + "; ".join(errors)))
        if errors:
            return

        print("\nPalette")
        check(pal[0] == palette565(), "green RGB565 levels equal scope_render.palette16()")
        check(all(len(set(p)) == 16 for p in pal), "amber, blue and white have 16 distinct levels")

        print("\nRenderer: JS against scope_render.py")
        worst = []
        for tid in [k for k in refs if k.startswith("syn")] + [k for k in refs if k.startswith("real")]:
            label = tid if tid.startswith("syn") else f"{wavs[tid][0].name} at {wavs[tid][2]:.0f} s, {wavs[tid][1]['rate'] // 1000} kHz " \
                f"{wavs[tid][1]['bits']}-bit {wavs[tid][1]['ch']} ch"
            worst.append((label,) + compare(label, tmp / f"{tid}.frames", refs[tid]))
        for label, n, bad, mx, lit, total in worst:
            # Identical, not close: the page keeps the script's float32 where it
            # matters (Float32Array state, Math.fround constants), and 16- and
            # 24-bit samples scale exactly in both.
            check(bad == 0, f"{label}: identical to scope_render.py ({lit} lit pixels)")

        print("\nWAV reader")
        ref16 = (tmp / "syn192000_pcm16.frames").read_bytes()
        for kind in ("pcm24", "f32", "ext24"):
            check((tmp / f"syn192000_{kind}.frames").read_bytes() == ref16, f"{kind} draws the same frames as pcm16")
        for tid, (path, info, start, n, span_len) in wavs.items():
            r = res[tid]
            got = sum(b - a for a, b in r["reads"])
            check(r["rate"] == info["rate"] and r["ch"] == info["ch"] and r["n"] == info["n"],
                  f"{path.name}: {r['rate']} Hz, {r['ch']} ch, {r['n']} sample frames as the header says")
            check(got <= span_len + 64 * 8, f"{path.name}: read {got / 1e6:.2f} MB of {info['size'] / 1e6:.1f} MB "
                  f"({len(r['reads'])} slices), the header and the span only")

        print("\nMono and near-mono")
        mono = np.frombuffer((tmp / "mono.frames").read_bytes(), np.uint8).reshape(-1, 64, 128)
        ys, xs = np.nonzero(mono.max(0) >= 12)
        check(res["mono"]["ch"] == 1 and res["mono"]["flat"] and len(xs) > 20 and np.all(np.abs((xs - 32) - (63 - ys)) <= 1),
              "a mono WAV draws the same sound on X and Y, a diagonal line, and is flagged")
        check(res["twin"]["flat"], "stereo channels 0.01 apart are flagged as well")
        check(not any(res[k]["flat"] for k in res if k.startswith("syn")), "the synthetic XY drawings are not flagged")
        for k in (k for k in res if k.startswith("real")):
            print(f"        {wavs[k][0].name}: {'flagged' if res[k]['flat'] else 'not flagged'}")

        print("\nPCA1")
        check((tmp / "enc_syn.pca").read_bytes() == py_pca(refs["syn192000_pcm16"]),
              "JS writer = gif_converter.build_pca, byte for byte, on the same frames (synthetic)")
        if specs:
            check((tmp / "enc_real0.pca").read_bytes() == py_pca(refs["real0"]),
                  f"JS writer = gif_converter.build_pca on {len(refs['real0'])} frames of {wavs['real0'][0].name}")
        check(res["enc_syn"]["headBytes"] == 44 + 2 * 12000, "pcaHead(12000) is 44 + 2 x 12000 bytes")
        if specs:
            path, start, dur = parse_span(specs[0])
            if wavs["real0"][1]["bits"] == 16:
                r = subprocess.run([sys.executable, SR.__file__, "pca", str(path), str(tmp / "sr.pca"), "--start", str(start), "--dur", str(dur)],
                                   capture_output=True, text=True)
                check(r.returncode == 0 and (tmp / "sr.pca").read_bytes() == (tmp / "real0.pca").read_bytes(),
                      f"scope_render.py pca writes the page's clip of {path.name}, byte for byte")

        print("\nFirmware validator (src/ambient/anim_store.cpp on the host)")
        exe = build_host(tmp, "validate", [ROOT / "src/ambient/anim_store.cpp", HOST / "validate_test.cpp"])
        if exe:
            clips = sorted(tmp.glob("*.pca"))
            flash = validate(exe, 0, clips)
            for c in clips:
                ok, frames = flash[str(c)]
                size = c.stat().st_size
                expect = frames <= 360 and size <= 1536 * 1024
                check(ok == expect if frames <= 360 else not ok, f"{c.name}: {size} bytes, {frames} frames, flash store "
                      f"{'accepts' if ok else 'refuses'}")
            lim = {}
            for fr in (360, 361, 12000, 12001, 65535):
                sparse_pca(tmp / f"lim{fr}.bin", fr)
            got = validate(exe, 0, [tmp / "lim360.bin", tmp / "lim361.bin"])
            check(got[str(tmp / "lim360.bin")][0] and not got[str(tmp / "lim361.bin")][0], "flash store: 360 frames accepted, 361 refused")
            got = validate(exe, 65535, [tmp / "lim65535.bin"])
            check(got[str(tmp / "lim65535.bin")] == (True, 65535),
                  "65535 frames, the u16 count's limit, validate: %d bytes, under FAT32's 4 GiB" % (tmp / "lim65535.bin").stat().st_size)
            cap = clip_sd_cap()
            if cap:
                got = validate(exe, cap, [tmp / "lim12000.bin", tmp / "lim12001.bin"])
                check(cap == 12000 and got[str(tmp / "lim12000.bin")][0] and not got[str(tmp / "lim12001.bin")][0],
                      f"card: CLIP_SD_MAX_FRAMES {cap} accepted, one more refused; {12 + 32 + cap * 4098} bytes at most")

        if (ROOT / "src/clips/clip_stream.cpp").exists():
            print("\nCard stream (src/clips/clip_stream.cpp on the host, reader and render on threads)")
            exe = build_host(tmp, "stream", [ROOT / "src/ambient/anim_store.cpp", ROOT / "src/clips/clip_stream.cpp", HOST / "stream_test.cpp"])
            if exe:
                r = subprocess.run([str(exe), str(tmp / "stream.pca")], capture_output=True, text=True)
                for line in r.stdout.splitlines():
                    check(line.startswith("ok"), line[3:] if line[:3] in ("ok ", "no ") else line)
                check(r.returncode == 0 and r.stdout, "stream test ran" + ("" if r.returncode == 0 else ": " + r.stderr[-500:]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%s" % ("all checks passed" if not FAILED else "%d FAILED" % len(FAILED)))
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()

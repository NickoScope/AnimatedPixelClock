#!/usr/bin/env python3
"""The gallery: a screen onto a panel in about a second.

`gallery/` holds one .lua a screen and a preview beside it, and this puts any of
them on a panel over the air - no build, no flash, no reboot. It is the same
upload route the MCP server's effect_upload uses; this is the hand version.

    python3 tools/agent/gallery.py list
    python3 tools/agent/gallery.py show starship
    python3 tools/agent/gallery.py show cannes --mac AA:BB:CC:00:11:22
    python3 tools/agent/gallery.py add my_effect      # from tools/luasim/scripts
    python3 tools/agent/gallery.py remove starship    # from the panel, not here

Exit codes as elsewhere: 0 done, 2 bad arguments, 3 a person must decide,
4 nothing answered.
"""

import argparse
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
GALLERY = ROOT / "gallery"
SCRIPTS = ROOT / "tools" / "luasim" / "scripts"

sys.path.insert(0, str(HERE))
import panel as P  # noqa: E402


def names():
    return sorted(p.stem for p in GALLERY.glob("*.lua"))


def cmd_list(args):
    for n in names():
        f = GALLERY / f"{n}.lua"
        prev = GALLERY / "preview" / f"{n}.png"
        print(f"  {n:16} {f.stat().st_size:>7} B   {'preview' if prev.exists() else '—'}")
    if not names():
        print("  (the gallery is empty)")
    return 0


def cmd_show(args):
    src = GALLERY / f"{args.name}.lua"
    if not src.exists():
        print(f"no {args.name} in the gallery. Have: {', '.join(names())}", file=sys.stderr)
        return 2
    # The panel's own rules, before the transfer is spent.
    sys.path.insert(0, str(ROOT / "tools" / "luasim"))
    try:
        import validate as V  # noqa: PLC0415
        v = V.check(src)
        if v and not v["ok"]:
            print(f"the panel would refuse it: {v['error']}", file=sys.stderr)
            return 2
    except Exception:  # noqa: BLE001
        pass

    try:
        p = P.resolve(args.mac)
    except P.ChoiceNeeded as e:
        print(e, file=sys.stderr)
        return 3
    except P.PanelError as e:
        print(e, file=sys.stderr)
        return 4

    a = p["address"]
    r = P.post_file(a, f"/api/lua/upload?name={args.name}", "script",
                    f"{args.name}.lua", src.read_bytes())
    if not r or not r.get("success"):
        print(f"refused: {(r or {}).get('error')}", file=sys.stderr)
        return 1
    idx = r.get("index")
    if isinstance(idx, int) and idx >= 0:
        P.post(a, "/api/lua", {"show": idx})
    up = (P.get(a, "/api/lua").get("uploaded") or {})
    print(f"  {args.name} on {p.get('name') or a}, slot {idx}, "
          f"{up.get('count')}/{up.get('slots')} used")
    return 0


def cmd_add(args):
    src = SCRIPTS / f"{args.name}.lua"
    if not src.exists():
        print(f"no {src}", file=sys.stderr)
        return 2
    GALLERY.mkdir(exist_ok=True)
    (GALLERY / "preview").mkdir(exist_ok=True)
    dest = GALLERY / f"{args.name}.lua"
    text = src.read_text()
    if "@upload-only" not in text[:400]:
        text = "-- @upload-only\n" + text
    dest.write_text(text)

    # A preview, from the same simulator the effect was written against.
    sim = ROOT / "tools" / "luasim"
    raw = pathlib.Path("/tmp") / f"gal_{args.name}.raw"
    subprocess.run(["make", "-s", "-C", str(sim), "luasim"], check=False)
    subprocess.run([str(sim / "luasim"), str(dest), str(args.frames), str(raw)],
                   cwd=sim, check=False, capture_output=True)
    if raw.exists():
        try:
            from PIL import Image  # noqa: PLC0415
            W, H, sz = 128, 64, 128 * 64 * 3
            data = raw.read_bytes()
            have = len(data) // sz
            n = min(args.stills, have)
            picks = [int(i * (have - 1) / max(1, n - 1)) for i in range(n)]
            ims = [Image.frombytes("RGB", (W, H), data[f * sz:(f + 1) * sz])
                   .resize((W * 4, H * 4), Image.NEAREST) for f in picks]
            w, h = ims[0].size
            cols = 1 if n == 1 else (3 if n % 3 == 0 else 2)
            rows = (n + cols - 1) // cols
            sheet = Image.new("RGB", (w * cols + 6 * (cols - 1), h * rows + 6 * (rows - 1)),
                              (16, 16, 20))
            for i, im in enumerate(ims):
                sheet.paste(im, ((i % cols) * (w + 6), (i // cols) * (h + 6)))
            sheet.save(GALLERY / "preview" / f"{args.name}.png")
        except ImportError:
            print("  (no Pillow, so no preview was made)")
    print(f"  {args.name} added. Write its entry into gallery/README.md by hand - "
          "the index is prose, and prose is the point of it.")
    return 0


def cmd_remove(args):
    try:
        p = P.resolve(args.mac)
    except P.PanelError as e:
        print(e, file=sys.stderr)
        return 4
    try:
        P.post(p["address"], "/api/lua", {"delete": args.name})
    except P.PanelError as e:
        print(e, file=sys.stderr)
        return 1
    print(f"  {args.name} removed from {p.get('name') or p['address']}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    s = sub.add_parser("show"); s.add_argument("name"); s.add_argument("--mac")
    a = sub.add_parser("add"); a.add_argument("name")
    a.add_argument("--frames", type=int, default=300)
    a.add_argument("--stills", type=int, default=6)
    r = sub.add_parser("remove"); r.add_argument("name"); r.add_argument("--mac")
    args = ap.parse_args()
    return {"list": cmd_list, "show": cmd_show, "add": cmd_add,
            "remove": cmd_remove}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())

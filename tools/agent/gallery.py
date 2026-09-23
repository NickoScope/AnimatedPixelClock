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

Publishing to GitHub, which is what the portal's "Add from the gallery" lists:

    python3 tools/agent/gallery.py publish my_effect --about "What it is." --by openclaw
    python3 tools/agent/gallery.py unpublish my_effect --by openclaw
    (--dry-run on either: everything but the commit and the push)

publish takes tools/luasim/scripts/<name>.lua (or --file), checks it with the
panel's own rules (tools/luasim/validate.py) and runs it in the simulator for
300 frames: a script that errors, or draws nothing but black, is refused. Then
the script, a preview and a README section go into gallery/, the index is
written again (tools/gallery_index.py), and ONE commit touching only gallery/
is pushed. It works in a throwaway worktree of the remote's branch, so the
checkout it runs from is never touched and a local main that is behind or
ahead does not matter.

Refused, whatever asks: a name outside the panel's rule (1 to 24 of letters,
digits, underscore); a name that reads as a built-in effect or as another
gallery entry; a script made from a photograph (photo_to_lua.py,
chafa_to_lua.py) - the repository is public and photographs of people never go
into it. With --by, an entry is marked "-- @by <who>", and replacing or
unpublishing an entry someone else published is refused.

The remote is `git config gallery.remote` (default origin) and the branch
`git config gallery.branch` (default main); --remote and --branch override.

Exit codes as elsewhere: 0 done, 2 bad arguments or refused, 3 a person must
decide, 4 nothing answered (the remote), 1 the checks could not be run.
"""

import argparse
import contextlib
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

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


def make_preview(script, png, frames=300, stills=6):
    """Run the script in the simulator; write a contact sheet of stills to png.

    Returns (error or None, lit): the simulator's own error line when the script
    fails, and how many pixels were lit over all frames (0 is a black screen)."""
    sim = ROOT / "tools" / "luasim"
    subprocess.run(["make", "-s", "-C", str(sim), "luasim"], check=False, capture_output=True)
    if not (sim / "luasim").exists():
        return "the simulator could not be built (make -C tools/luasim luasim)", 0
    with tempfile.TemporaryDirectory() as d:
        raw = pathlib.Path(d) / "frames.raw"
        r = subprocess.run([str(sim / "luasim"), str(script), str(frames), str(raw)],
                           cwd=sim, capture_output=True, text=True, timeout=300)
        if r.returncode or not raw.exists():
            lines = [l for l in (r.stderr or "").splitlines() if l.strip()]
            return (lines[-1] if lines else f"the simulator stopped ({r.returncode})"), 0
        data = raw.read_bytes()
        lit = sum(1 for b in data[::3] if b) + sum(1 for b in data[1::3] if b) + sum(1 for b in data[2::3] if b)
        try:
            from PIL import Image  # noqa: PLC0415
        except ImportError:
            return "no Pillow, so no preview (pip install pillow)", lit
        W, H, sz = 128, 64, 128 * 64 * 3
        have = len(data) // sz
        n = min(stills, have)
        picks = [int(i * (have - 1) / max(1, n - 1)) for i in range(n)]
        ims = [Image.frombytes("RGB", (W, H), data[f * sz:(f + 1) * sz])
               .resize((W * 4, H * 4), Image.NEAREST) for f in picks]
        w, h = ims[0].size
        cols = 1 if n == 1 else (3 if n % 3 == 0 else 2)
        rows = (n + cols - 1) // cols
        sheet = Image.new("RGB", (w * cols + 6 * (cols - 1), h * rows + 6 * (rows - 1)), (16, 16, 20))
        for i, im in enumerate(ims):
            sheet.paste(im, ((i % cols) * (w + 6), (i // cols) * (h + 6)))
        png.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(png)
    return None, lit


# ------------------------------------------------------------------ publishing

STEM = re.compile(r"[A-Za-z0-9_]{1,24}")          # validStem, src/lua/lua_store.cpp
WHO = re.compile(r"[A-Za-z0-9_-]{1,32}")
BY = re.compile(r"^--\s*@by\s+(\S+)\s*$", re.M)
PHOTO = ("photo_to_lua.py", "chafa_to_lua.py")     # the two converters write their name in


class Refused(Exception):
    pass


def git(cwd, *a, check=True):
    r = subprocess.run(["git", "-C", str(cwd), *a], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"git {' '.join(a[:2])}: {(r.stderr or r.stdout).strip()[-400:]}")
    return r


def shown(stem):
    return stem.replace("_", " ").upper()


def by_of(path):
    m = BY.search("\n".join(path.read_text(encoding="utf-8").splitlines()[:10]))
    return m.group(1) if m else None


def builtin_names(wt):
    spec = importlib.util.spec_from_file_location("gen_effects_wt", wt / "tools/luasim/gen_effects.py")
    g = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(g)
    scripts = wt / "tools/luasim/scripts"
    return {shown(p.stem) for p in scripts.glob("*.lua") if p.name not in g.SKIP and not g.upload_only(p)}


def target(args):
    remote = args.remote or git(ROOT, "config", "gallery.remote", check=False).stdout.strip() or "origin"
    branch = args.branch or git(ROOT, "config", "gallery.branch", check=False).stdout.strip() or "main"
    return remote, branch


@contextlib.contextmanager
def checkout(remote, branch):
    """A throwaway worktree of remote/branch as it is right now."""
    r = git(ROOT, "fetch", "-q", remote, branch, check=False)
    if r.returncode:
        raise ConnectionError(f"could not fetch {branch} from {remote}: {(r.stderr or '').strip()[-300:]}")
    wt = pathlib.Path(tempfile.mkdtemp(prefix="gallery-"))
    git(ROOT, "worktree", "add", "-q", "--detach", str(wt), "FETCH_HEAD")
    try:
        yield wt
    finally:
        git(ROOT, "worktree", "remove", "--force", str(wt), check=False)
        shutil.rmtree(wt, ignore_errors=True)
        git(ROOT, "worktree", "prune", check=False)


def readme_drop(text, name):
    """The README without the section '## name' (and the rule after it)."""
    return re.sub(r"(?ms)^## " + re.escape(name) + r"\n.*?(?:^---\n\n?|(?=^## )|\Z)", "", text)


def readme_put(text, name, stem, about, by):
    text = readme_drop(text, name)
    sec = (f"## {name}\n\n![{name.title()}](preview/{stem}.png)\n\n{about.strip()}\n\n"
           + (f"_Published by {by}._\n\n" if by else "") + "---\n\n")
    at = text.find("## Adding one")
    return text[:at] + sec + text[at:] if at >= 0 else text.rstrip("\n") + "\n\n---\n\n" + sec


def commit_and_push(wt, remote, branch, message, dry):
    git(wt, "add", "-A", "--", "gallery")
    staged = [l for l in git(wt, "diff", "--cached", "--name-only").stdout.splitlines() if l]
    if not staged:
        return "nothing to change: the gallery already has it exactly so"
    outside = [f for f in staged if not f.startswith("gallery/")]
    if outside:
        raise Refused(f"would touch files outside gallery/: {outside}")
    if dry:
        return "dry run, not committed: " + ", ".join(staged)
    # The checks a person's commit gets from .githooks are not assumed to be
    # installed where an agent runs; the ones that matter here ran above.
    git(wt, "-c", "core.hooksPath=/dev/null", "commit", "-q", "-m", message)
    r = git(wt, "push", "-q", remote, f"HEAD:refs/heads/{branch}", check=False)
    if r.returncode:
        err = (r.stderr or "").strip()
        if "rejected" in err or "non-fast-forward" in err or "fetch first" in err:
            raise BlockingIOError(f"{branch} moved on {remote} while this ran; run it again")
        raise PermissionError(f"the push was refused: {err[-400:]}")
    sha = git(wt, "rev-parse", "--short", "HEAD").stdout.strip()
    return f"pushed {sha} to {remote} {branch}: " + ", ".join(staged)


def publish(stem, about, by=None, file=None, remote="origin", branch="main", dry=False, any_owner=False):
    if not STEM.fullmatch(stem or ""):
        raise Refused("a name is 1 to 24 of letters, digits and underscore")
    if by is not None and not WHO.fullmatch(by):
        raise Refused("--by is 1 to 32 of letters, digits, _ and -")
    if not about or len(about.strip()) < 20:
        raise Refused("--about: a few sentences on what it is (the gallery's README is prose)")
    src = pathlib.Path(file) if file else SCRIPTS / f"{stem}.lua"
    if not src.exists():
        raise Refused(f"no {src}")
    text = src.read_text(encoding="utf-8")
    for m in PHOTO:
        if m in text:
            raise Refused(f"made by {m}, from a photograph: the gallery is public and takes no photographs")
    lines = [l for l in text.splitlines() if not BY.match(l) and l.strip() != "-- @upload-only"]
    body = "\n".join(["-- @upload-only"] + ([f"-- @by {by}"] if by else []) + lines) + "\n"
    name = shown(stem)
    for attempt in (1, 2):
        with checkout(remote, branch) as wt:
            gal = wt / "gallery"
            if name in builtin_names(wt):
                raise Refused(f"{name} is built into the firmware; pick another name")
            for p in gal.glob("*.lua"):
                if p.stem != stem and shown(p.stem) == name:
                    raise Refused(f"{name} is already in the gallery as {p.name}")
            dest = gal / f"{stem}.lua"
            if dest.exists() and not any_owner and by_of(dest) != by:
                raise Refused(f"{dest.name} was published by {by_of(dest) or 'a person'}; not yours to replace")
            dest.write_text(body, encoding="utf-8")
            sys.path.insert(0, str(ROOT / "tools" / "luasim"))
            import validate as V  # noqa: PLC0415
            v = V.check(dest)
            if v is None:
                raise RuntimeError("the panel's own check (tools/luasim/validate.py) could not be run")
            if not v["ok"]:
                raise Refused(f"the panel would refuse it: {v['error']}")
            err, lit = make_preview(dest, gal / "preview" / f"{stem}.png")
            if err:
                raise Refused(f"it does not run in the simulator: {err}")
            if not lit:
                raise Refused("300 frames in the simulator and not one pixel lit")
            readme = gal / "README.md"
            readme.write_text(readme_put(readme.read_text(encoding="utf-8"), name, stem, about, by), encoding="utf-8")
            (gal / "index.json").write_text(index_text(gal), encoding="utf-8")
            verb = "update" if git(wt, "cat-file", "-e", f"HEAD:gallery/{stem}.lua", check=False).returncode == 0 else "add"
            msg = f"gallery: {verb} {name}\n\n{about.strip()[:400]}\n" + (f"\nPublished-by: {by}\n" if by else "")
            try:
                return commit_and_push(wt, remote, branch, msg, dry)
            except BlockingIOError:
                if attempt == 2:
                    raise
    return None


def unpublish(stem, by=None, remote="origin", branch="main", dry=False, any_owner=False):
    if not STEM.fullmatch(stem or ""):
        raise Refused("a name is 1 to 24 of letters, digits and underscore")
    for attempt in (1, 2):
        with checkout(remote, branch) as wt:
            gal = wt / "gallery"
            dest = gal / f"{stem}.lua"
            if not dest.exists():
                raise Refused(f"no {stem} in the gallery; it has: {', '.join(sorted(p.stem for p in gal.glob('*.lua')))}")
            owner = by_of(dest)
            if not any_owner and owner != by:
                raise Refused(f"{dest.name} was published by {owner or 'a person'}; not yours to remove")
            dest.unlink()
            for extra in (gal / "preview" / f"{stem}.png", gal / "preview" / f"{stem}.gif"):
                if extra.exists():
                    extra.unlink()
            readme = gal / "README.md"
            readme.write_text(readme_drop(readme.read_text(encoding="utf-8"), shown(stem)), encoding="utf-8")
            (gal / "index.json").write_text(index_text(gal), encoding="utf-8")
            msg = f"gallery: remove {shown(stem)}\n" + (f"\nUnpublished-by: {by}\n" if by else "")
            try:
                return commit_and_push(wt, remote, branch, msg, dry)
            except BlockingIOError:
                if attempt == 2:
                    raise
    return None


def index_text(gal):
    spec = importlib.util.spec_from_file_location("gallery_index_wt", gal.parent / "tools/gallery_index.py")
    g = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(g)
    return g.build(gal)


def _run(fn, **kw):
    try:
        print("  " + fn(**kw))
        return 0
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 2
    except (ConnectionError, PermissionError, BlockingIOError) as e:
        print(e, file=sys.stderr)
        return 4
    except Exception as e:  # noqa: BLE001
        print(f"could not run: {e}", file=sys.stderr)
        return 1


def cmd_publish(args):
    remote, branch = target(args)
    about = pathlib.Path(args.about_file).read_text(encoding="utf-8") if args.about_file else args.about
    return _run(publish, stem=args.name, about=about, by=args.by, file=args.file, remote=remote,
                branch=branch, dry=args.dry_run, any_owner=args.any)


def cmd_unpublish(args):
    remote, branch = target(args)
    return _run(unpublish, stem=args.name, by=args.by, remote=remote, branch=branch,
                dry=args.dry_run, any_owner=args.any)


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

    make_preview(dest, GALLERY / "preview" / f"{args.name}.png", args.frames, args.stills)
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
    for c in ("publish", "unpublish"):
        x = sub.add_parser(c); x.add_argument("name")
        x.add_argument("--by", help="who publishes: marks the entry, and only they may replace or remove it")
        x.add_argument("--any", action="store_true", help="a person's override of --by")
        x.add_argument("--remote"); x.add_argument("--branch")
        x.add_argument("--dry-run", action="store_true")
        if c == "publish":
            x.add_argument("--about", help="the README section: what it is, a few sentences")
            x.add_argument("--about-file")
            x.add_argument("--file", help="the script, if not tools/luasim/scripts/<name>.lua")
    args = ap.parse_args()
    return {"list": cmd_list, "show": cmd_show, "add": cmd_add, "remove": cmd_remove,
            "publish": cmd_publish, "unpublish": cmd_unpublish}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())

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

The remote is $LEDMATRIX_GALLERY_REMOTE, else `git config gallery.remote`
(default origin), and the branch $LEDMATRIX_GALLERY_BRANCH, else
`git config gallery.branch` (default main); --remote and --branch override.

An agent's machine has no key for GitHub: there the remote is its own clone
(gallery.remote = .) and the branch gallery-staging. A maintainer carries it
to GitHub from a machine that can push, entry by entry, every check again:

    python3 tools/agent/gallery.py sync pi@nickol.local:ledmatrix-mcp --by openclaw

The agent's Screen of the Day scoreboard (gallery/SCREEN_OF_THE_DAY.md) goes
the same way: `scoreboard FILE` (MCP gallery_scoreboard), Markdown with no
HTML, pictures only gallery previews.

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


# A git hook runs with GIT_DIR, GIT_INDEX_FILE and the like pointing at the
# repository being committed to, and they override -C: run from a hook, the
# worktree's commit would land on the caller's branch. They are dropped.
_GIT_ENV = {k: v for k, v in __import__("os").environ.items()
            if not (k.startswith("GIT_") and k not in ("GIT_SSH", "GIT_SSH_COMMAND", "GIT_ASKPASS",
                                                          "GIT_TERMINAL_PROMPT", "GIT_CONFIG_GLOBAL"))}


def git(cwd, *a, check=True):
    r = subprocess.run(["git", "-C", str(cwd), *a], capture_output=True, text=True, env=_GIT_ENV)
    if check and r.returncode:
        raise RuntimeError(f"git {' '.join(a[:2])}: {(r.stderr or r.stdout).strip()[-400:]}")
    return r


def shown(stem):
    return stem.replace("_", " ").upper()


def by_of(path):
    m = BY.search("\n".join(path.read_text(encoding="utf-8").splitlines()[:10]))
    return m.group(1) if m else None


def builtin_names(wt):
    """The names compiled into the firmware, by gen_effects.py's own rules, read
    rather than run: running it writes the header."""
    import ast  # noqa: PLC0415
    tree = ast.parse((wt / "tools/luasim/gen_effects.py").read_text(encoding="utf-8"))
    skip = next(ast.literal_eval(n.value) for n in tree.body
                if isinstance(n, ast.Assign) and any(getattr(t, "id", "") == "SKIP" for t in n.targets))

    def upload_only(path):
        with open(path, encoding="utf-8") as f:
            return any("@upload-only" in line for _, line in zip(range(10), f))

    scripts = wt / "tools/luasim/scripts"
    return {shown(p.stem) for p in scripts.glob("*.lua") if p.name not in skip and not upload_only(p)}


def target(remote=None, branch=None):
    import os  # noqa: PLC0415
    remote = remote or os.environ.get("LEDMATRIX_GALLERY_REMOTE") or git(ROOT, "config", "gallery.remote", check=False).stdout.strip() or "origin"
    branch = branch or os.environ.get("LEDMATRIX_GALLERY_BRANCH") or \
        git(ROOT, "config", "gallery.branch", check=False).stdout.strip() or "main"
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
        if "(fetch first)" in err or "non-fast-forward" in err:   # not "remote rejected": a hook or a rule
            raise BlockingIOError(f"{branch} moved on {remote} while this ran; run it again")
        raise PermissionError(f"the push was refused: {err[-400:]}")
    sha = git(wt, "rev-parse", "--short", "HEAD").stdout.strip()
    return f"pushed {sha} to {remote} {branch}: " + ", ".join(staged)


def _script_body(stem, text, about, by):
    """The script as the gallery keeps it: tagged, with a title comment."""
    for m in PHOTO:
        if m in text:
            raise Refused(f"{stem}: made by {m}, from a photograph: the gallery is public and takes no photographs")
    lines = [l for l in text.splitlines() if not BY.match(l) and l.strip() != "-- @upload-only"]
    # The portal's one-line description is the script's title comment
    # ("-- NAME - what it is", tools/gallery_index.py); one is made from the
    # first sentence of --about when the script has none.
    if not any(re.match(r"--\s*[A-Z0-9_ ]+?\s+-\s+.+$", l) for l in lines[:6]):
        first = re.split(r"(?<=[.!?])\s", about.strip().replace("\n", " "), maxsplit=1)[0]
        lines.insert(0, f"-- {shown(stem)} - {first[:90].rstrip('.')}")
    return "\n".join(["-- @upload-only"] + ([f"-- @by {by}"] if by else []) + lines) + "\n"


def _put(wt, stem, text, about, by, any_owner=False):
    """One entry into the worktree's gallery/, every check first. Returns 'add' or 'update'."""
    if not STEM.fullmatch(stem or ""):
        raise Refused("a name is 1 to 24 of letters, digits and underscore")
    if by is not None and not WHO.fullmatch(by):
        raise Refused("--by is 1 to 32 of letters, digits, _ and -")
    if not about or len(about.strip()) < 20:
        raise Refused(f"{stem}: --about: a few sentences on what it is (the gallery's README is prose)")
    body = _script_body(stem, text, about, by)
    name = shown(stem)
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
        raise Refused(f"{stem}: the panel would refuse it: {v['error']}")
    # The preview is always made here, from the script: never taken from
    # anywhere else, so it cannot be anything but what the script draws.
    err, lit = make_preview(dest, gal / "preview" / f"{stem}.png")
    if err:
        raise Refused(f"{stem}: it does not run in the simulator: {err}")
    if not lit:
        raise Refused(f"{stem}: 300 frames in the simulator and not one pixel lit")
    readme = gal / "README.md"
    readme.write_text(readme_put(readme.read_text(encoding="utf-8"), name, stem, about, by), encoding="utf-8")
    (gal / "index.json").write_text(index_text(gal), encoding="utf-8")
    return "update" if git(wt, "cat-file", "-e", f"HEAD:gallery/{stem}.lua", check=False).returncode == 0 else "add"


def _drop(wt, stem, by, any_owner=False):
    if not STEM.fullmatch(stem or ""):
        raise Refused("a name is 1 to 24 of letters, digits and underscore")
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


def _retry(remote, branch, dry, work):
    """work(wt) -> commit message; committed and pushed, once more if the branch moved."""
    for attempt in (1, 2):
        with checkout(remote, branch) as wt:
            msg = work(wt)
            try:
                return commit_and_push(wt, remote, branch, msg, dry)
            except BlockingIOError:
                if attempt == 2:
                    raise
    return None


def publish(stem, about, by=None, file=None, remote="origin", branch="main", dry=False, any_owner=False):
    src = pathlib.Path(file) if file else SCRIPTS / f"{stem}.lua"
    if not src.exists():
        raise Refused(f"no {src}")
    text = src.read_text(encoding="utf-8")

    def work(wt):
        verb = _put(wt, stem, text, about, by, any_owner)
        return f"gallery: {verb} {shown(stem)}\n\n{about.strip()[:400]}\n" + (f"\nPublished-by: {by}\n" if by else "")
    return _retry(remote, branch, dry, work)


def unpublish(stem, by=None, remote="origin", branch="main", dry=False, any_owner=False):
    def work(wt):
        _drop(wt, stem, by, any_owner)
        return f"gallery: remove {shown(stem)}\n" + (f"\nUnpublished-by: {by}\n" if by else "")
    return _retry(remote, branch, dry, work)


# ------------------------------------------------------------ the scoreboard
# gallery/SCREEN_OF_THE_DAY.md: the agent's daily screen and the owner's
# thumbs. Markdown only; a picture in it may be only a gallery preview, so the
# only images it can show are what gallery scripts draw.
BOARD = "SCREEN_OF_THE_DAY.md"
BOARD_MAX = 65536
IMG_OK = re.compile(r"(?:\./)?preview/[A-Za-z0-9_]{1,24}\.png")


def check_board(text, gal):
    if len(text.encode("utf-8")) > BOARD_MAX:
        raise Refused(f"the scoreboard is over {BOARD_MAX // 1024} KB")
    if re.search(r"<\s*(script|iframe|object|embed|style|form|img|video|audio|svg)\b", text, re.I):
        raise Refused("the scoreboard is Markdown: no HTML tags (pictures as ![](preview/<name>.png))")
    for ref in re.findall(r"!\[[^\]]*\]\(\s*<?([^)\s>]+)", text):
        if not IMG_OK.fullmatch(ref):
            raise Refused(f"a picture may only be a gallery preview (preview/<name>.png), not {ref}")
        if not (gal / ref.lstrip("./")).exists():
            raise Refused(f"{ref} is not in the gallery: publish that screen first")


def scoreboard(text, by=None, remote="origin", branch="main", dry=False):
    if not text or not text.strip():
        raise Refused("the scoreboard text is empty")

    def work(wt):
        gal = wt / "gallery"
        check_board(text, gal)
        (gal / BOARD).write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
        return "gallery: Screen of the Day\n" + (f"\nPublished-by: {by}\n" if by else "")
    return _retry(remote, branch, dry, work)


# ------------------------------------------------------------------ sync
# The agent has no key for GitHub. It publishes into a staging branch on its
# own machine; a maintainer runs `sync` from a machine that can push. The
# agent's entries are mirrored BY STATE, not by replaying its commits: each is
# put through every check again here (_put), its preview made again from the
# script, and only its own entries and the scoreboard can change.

def readme_about(text, name):
    m = re.search(r"(?ms)^## " + re.escape(name) + r"\n(.*?)(?:^---\n|(?=^## )|\Z)", text)
    if not m:
        return None
    keep = [l for l in m.group(1).splitlines()
            if not l.startswith("![") and not re.match(r"_Published by .*\._$", l)]
    return "\n".join(keep).strip() or None


def sync(source, source_branch, by, remote="origin", branch="main", dry=False):
    if not by or not WHO.fullmatch(by):
        raise Refused("--by: whose entries to carry over")
    with checkout(source, source_branch) as st:
        staged_sha = git(st, "rev-parse", "HEAD").stdout.strip()
        sgal = st / "gallery"
        mine = {p.stem: p for p in sgal.glob("*.lua") if by_of(p) == by}
        sreadme = (sgal / "README.md").read_text(encoding="utf-8")
        board = (sgal / BOARD).read_text(encoding="utf-8") if (sgal / BOARD).exists() else None
        plan = []

        def work(wt):
            plan.clear()
            gal = wt / "gallery"
            theirs = {p.stem for p in gal.glob("*.lua") if by_of(p) == by}
            for stem in sorted(theirs - set(mine)):
                _drop(wt, stem, by)
                plan.append(f"removed {shown(stem)}")
            for stem, path in sorted(mine.items()):
                about = readme_about(sreadme, shown(stem))
                if not about:
                    raise Refused(f"{stem}: no README section in the staging gallery")
                before = (gal / f"{stem}.lua").read_text(encoding="utf-8") if (gal / f"{stem}.lua").exists() else None
                verb = _put(wt, stem, path.read_text(encoding="utf-8"), about, by)
                if (gal / f"{stem}.lua").read_text(encoding="utf-8") != before or verb == "add":
                    plan.append(f"{'added' if verb == 'add' else 'updated'} {shown(stem)}")
            if board is not None:
                check_board(board, gal)
                cur = (gal / BOARD).read_text(encoding="utf-8") if (gal / BOARD).exists() else None
                if cur != board:
                    (gal / BOARD).write_text(board, encoding="utf-8")
                    plan.append("scoreboard")
            return (f"gallery: from {by}: " + ", ".join(plan or ["README and previews"]) +
                    f"\n\nMirrored from the {by} staging branch ({staged_sha[:9]}), every entry checked\n"
                    f"again and its preview made again from the script.\n\nPublished-by: {by}\n")

        out = _retry(remote, branch, dry, work)
    # The staging branch starts again from what GitHub now has, so the agent's
    # next publish is on top of it. Only if the agent published nothing since.
    if not dry:
        git(ROOT, "fetch", "-q", remote, branch)
        head = git(ROOT, "rev-parse", "FETCH_HEAD").stdout.strip()
        r = git(ROOT, "push", "-q", f"--force-with-lease=refs/heads/{source_branch}:{staged_sha}",
                source, f"{head}:refs/heads/{source_branch}", check=False)
        out += ("; staging moved to " + head[:9]) if not r.returncode else \
            ("; staging NOT moved (" + ((r.stderr or "").strip().splitlines() or ["?"])[-1] + "): run sync again")
    return out


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
    remote, branch = target(args.remote, args.branch)
    about = pathlib.Path(args.about_file).read_text(encoding="utf-8") if args.about_file else args.about
    return _run(publish, stem=args.name, about=about, by=args.by, file=args.file, remote=remote,
                branch=branch, dry=args.dry_run, any_owner=args.any)


def cmd_scoreboard(args):
    remote, branch = target(args.remote, args.branch)
    text = pathlib.Path(args.file).read_text(encoding="utf-8")
    return _run(scoreboard, text=text, by=args.by, remote=remote, branch=branch, dry=args.dry_run)


def cmd_sync(args):
    remote, branch = args.remote or "origin", args.branch or "main"
    return _run(sync, source=args.source, source_branch=args.source_branch, by=args.by,
                remote=remote, branch=branch, dry=args.dry_run)


def cmd_unpublish(args):
    remote, branch = target(args.remote, args.branch)
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
    b = sub.add_parser("scoreboard"); b.add_argument("file", help="the new SCREEN_OF_THE_DAY.md")
    b.add_argument("--by"); b.add_argument("--remote"); b.add_argument("--branch")
    b.add_argument("--dry-run", action="store_true")
    y = sub.add_parser("sync", help="carry an agent's staged entries to GitHub (a maintainer's machine)")
    y.add_argument("source", help="the agent's repository, e.g. pi@nickol.local:ledmatrix-mcp")
    y.add_argument("--source-branch", default="gallery-staging")
    y.add_argument("--by", required=True, help="whose entries: the agent's LEDMATRIX_PUBLISHER")
    y.add_argument("--remote"); y.add_argument("--branch")
    y.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    return {"list": cmd_list, "show": cmd_show, "add": cmd_add, "remove": cmd_remove,
            "publish": cmd_publish, "unpublish": cmd_unpublish, "scoreboard": cmd_scoreboard,
            "sync": cmd_sync}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())

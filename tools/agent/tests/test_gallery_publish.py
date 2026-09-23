#!/usr/bin/env python3
"""Gallery publishing, the whole path, on the disk only.

Two repositories stand in for the real ones: a bare "github" made from this
checkout's HEAD, and "pi", a clone of it with a gallery-staging branch - the
agent's machine, which has no key for GitHub. The agent publishes through the
MCP tools into its staging branch; the maintainer's `sync` carries its entries
to "github" with every check made again. Also: a person's entry changed in
staging is not carried; a remote that refuses pushes is told apart.

    tools/agent/.venv/bin/python tools/agent/tests/test_gallery_publish.py
"""
import asyncio, json, os, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import mcp_server as M   # noqa: E402
import gallery as G      # noqa: E402

# The test's own git calls run clean. The tool's run as if from a git hook:
# GIT_DIR and GIT_INDEX_FILE pointing at this checkout, which is exactly what
# once put a test commit on the branch being committed to. It must drop them.
CLEAN = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
os.environ["GIT_DIR"] = str(ROOT / ".git")
os.environ["GIT_INDEX_FILE"] = str(ROOT / ".git" / "index")

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += 0 if cond else 1


def git(*a, cwd=ROOT):
    return subprocess.run(["git", "-C", str(cwd), *a], capture_output=True, text=True, env=CLEAN).stdout.strip()


def call(tool, model, **kw):
    return asyncio.run(tool(model(**kw)))


STEM = "zz_gallery_test_fx"
SCRIPT = ROOT / "tools/luasim/scripts" / f"{STEM}.lua"
ABOUT = "A test pattern that sweeps one bar across the panel. Made by the test, removed by it."
here_head, here_status = git("rev-parse", "HEAD"), git("status", "--porcelain")

with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    gh, pi = d / "github.git", d / "pi"
    subprocess.run(["git", "init", "-q", "--bare", str(gh)], check=True, env=CLEAN)
    subprocess.run(["git", "-C", str(ROOT), "push", "-q", str(gh), "HEAD:refs/heads/main"], check=True, env=CLEAN)
    subprocess.run(["git", "clone", "-q", str(gh), str(pi)], check=True, env=CLEAN)
    git("branch", "gallery-staging", "origin/main", cwd=pi)
    start = git("rev-parse", "main", cwd=gh)
    os.environ.update(LEDMATRIX_GALLERY_REMOTE="file://" + str(pi), LEDMATRIX_GALLERY_BRANCH="gallery-staging",
                      LEDMATRIX_PUBLISHER="openclaw")
    SCRIPT.write_text("function draw()\n  local x = math.floor(px.t() * 20) % 128\n"
                      "  px.rect(x, 0, 4, 64, 255, 160, 40, true)\nend\n")

    def sync():
        return G.sync("file://" + str(pi), "gallery-staging", "openclaw", remote="file://" + str(gh), branch="main")
    try:
        # --- the agent, on its own machine
        r = json.loads(call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT))
        check(r.get("ok") and "gallery-staging" in r["result"], "agent: publish goes to its staging branch")
        check(git("rev-parse", "main", cwd=gh) == start, "and GitHub is untouched")
        files = git("show", "--name-only", "--format=", "gallery-staging", cwd=pi).splitlines()
        check(sorted(files) == sorted(["gallery/README.md", f"gallery/{STEM}.lua", "gallery/index.json",
                                       f"gallery/preview/{STEM}.png"]), "one commit, only gallery/ files")
        idx = json.loads(git("show", "gallery-staging:gallery/index.json", cwd=pi))
        e = [x for x in idx["effects"] if x["stem"] == STEM]
        check(e and e[0].get("by") == "openclaw" and e[0]["line"].startswith("A test pattern"),
              "the index marks the publisher and has a line from --about")
        r = json.loads(call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT))
        check("nothing to change" in r.get("result", ""), "publish again, unchanged: nothing committed")
        for name, why in (("aquarium", "a person's entry cannot be replaced"),):
            r = call(M.gallery_publish, M.GalleryPublishIn, name=name, about=ABOUT)
            check(r.startswith("Refused") and "not yours" in r, why)
        r = call(M.gallery_unpublish, M.GalleryRemoveIn, name="aquarium")
        check(r.startswith("Refused") and "not yours" in r, "nor removed")
        board = f"# Screen of the Day\n\n| Date | Screen | Verdict |\n|---|---|---|\n| 2026-09-24 | ![x](preview/{STEM}.png) | 👍 |\n"
        r = call(M.gallery_scoreboard, M.ScoreboardIn, markdown=board.replace(f"preview/{STEM}.png", "https://example.com/a.jpg"))
        check(r.startswith("Refused") and "only be a gallery preview" in r, "scoreboard: a picture from outside is refused")
        r = call(M.gallery_scoreboard, M.ScoreboardIn, markdown=board + '<img src="preview/aquarium.png">\n')
        check(r.startswith("Refused") and "no HTML" in r, "scoreboard: HTML is refused")
        r = json.loads(call(M.gallery_scoreboard, M.ScoreboardIn, markdown=board))
        check(r.get("ok") and "SCREEN_OF_THE_DAY.md" in r["result"], "scoreboard: a gallery preview and a thumb, committed")
        # someone changes a person's entry in staging behind the tools
        aq = pi / "gallery/aquarium.lua"
        git("checkout", "-q", "gallery-staging", cwd=pi)
        aq.write_text(aq.read_text() + "-- tampered\n")
        git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qam", "tamper", cwd=pi)
        git("checkout", "-q", "main", cwd=pi)
        staged = git("rev-parse", "gallery-staging", cwd=pi)

        # --- the maintainer
        out = sync()
        check("pushed" in out and f"added {G.shown(STEM)}" in git("log", "-1", "--format=%s", "main", cwd=gh)
              and "scoreboard" in git("log", "-1", "--format=%s", "main", cwd=gh), "sync: the entry and the scoreboard reach GitHub")
        gfiles = git("show", "--name-only", "--format=", "main", cwd=gh).splitlines()
        check(all(f.startswith("gallery/") for f in gfiles) and "gallery/aquarium.lua" not in gfiles,
              "only gallery/, and the person's entry changed in staging is not carried")
        check(git("rev-parse", "gallery-staging", cwd=pi) == git("rev-parse", "main", cwd=gh) and staged != "",
              "staging starts again from what GitHub has")
        out = sync()
        check("nothing to change" in out, "sync again: nothing to change")
        # the agent takes it back
        r = json.loads(call(M.gallery_unpublish, M.GalleryRemoveIn, name=STEM))
        check(r.get("ok"), "agent: unpublish into staging")
        board2 = "# Screen of the Day\n\nNothing yet.\n"
        call(M.gallery_scoreboard, M.ScoreboardIn, markdown=board2)
        out = sync()
        check("removed " + G.shown(STEM) in git("log", "-1", "--format=%s", "main", cwd=gh), "sync: removed on GitHub too")
        check(git("diff", "--stat", start, "main", "--", ":!gallery/SCREEN_OF_THE_DAY.md", cwd=gh) == "",
              "GitHub's gallery is back as it was, but for the scoreboard")
        # a remote that refuses every push, as GitHub does with no key
        ro = d / "ro.git"
        subprocess.run(["git", "clone", "-q", "--bare", str(gh), str(ro)], check=True, env=CLEAN)
        hook = ro / "hooks/pre-receive"
        hook.write_text("#!/bin/sh\necho 'ERROR: Permission to the repository denied.' >&2\nexit 1\n")
        hook.chmod(0o755)
        os.environ.update(LEDMATRIX_GALLERY_REMOTE="file://" + str(ro), LEDMATRIX_GALLERY_BRANCH="main")
        r = call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT)
        check("cannot write" in r and "denied" in r, "a remote that refuses: said, with where it should point")
    finally:
        SCRIPT.unlink(missing_ok=True)
check(not git("worktree", "list", "--porcelain").count("gallery-"), "no worktree left behind")
check(git("rev-parse", "HEAD") == here_head and git("status", "--porcelain") == here_status,
      "this checkout untouched: same HEAD, same status (git's hook variables were ignored)")
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)

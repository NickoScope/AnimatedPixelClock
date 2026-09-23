#!/usr/bin/env python3
"""gallery_publish / gallery_unpublish, the MCP tools, against a local remote.

A bare repository made from this checkout's HEAD stands in for GitHub, so the
whole path runs - worktree, the panel's checks, the simulator, the preview,
the README and the index, the commit, the push - and nothing leaves the disk.
A second bare one refuses every push (as a read-only deploy key would), to
check what an agent is told then.

    tools/agent/.venv/bin/python tools/agent/tests/test_gallery_publish.py
"""
import asyncio, json, os, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import mcp_server as M   # noqa: E402

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

with tempfile.TemporaryDirectory() as d:
    bare = pathlib.Path(d) / "remote.git"
    subprocess.run(["git", "init", "-q", "--bare", str(bare)], check=True, env=CLEAN)
    subprocess.run(["git", "-C", str(ROOT), "push", "-q", str(bare), "HEAD:refs/heads/main"], check=True, env=CLEAN)
    here_head, here_status = git("rev-parse", "HEAD"), git("status", "--porcelain")
    start = git("rev-parse", "main", cwd=bare)
    os.environ["LEDMATRIX_GALLERY_REMOTE"] = "file://" + str(bare)
    os.environ["LEDMATRIX_PUBLISHER"] = "openclaw"
    SCRIPT.write_text("function draw()\n  local x = math.floor(px.t() * 20) % 128\n"
                      "  px.rect(x, 0, 4, 64, 255, 160, 40, true)\nend\n")
    try:
        r = json.loads(call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT))
        check(r.get("ok") and "pushed" in r["result"], "publish: pushed")
        files = git("show", "--name-only", "--format=", "main", cwd=bare).splitlines()
        check(sorted(files) == sorted(["gallery/README.md", f"gallery/{STEM}.lua", "gallery/index.json",
                                       f"gallery/preview/{STEM}.png"]), "one commit, only gallery/ files")
        idx = json.loads(git("show", "main:gallery/index.json", cwd=bare))
        e = [x for x in idx["effects"] if x["stem"] == STEM]
        check(e and e[0].get("by") == "openclaw" and e[0]["line"].startswith("A test pattern"),
              "the index marks the publisher and has a line from --about")
        head = git("show", f"main:gallery/{STEM}.lua", cwd=bare).splitlines()[:3]
        check(head[0] == "-- @upload-only" and head[1] == "-- @by openclaw", "the script is tagged upload-only and by")
        r = json.loads(call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT))
        check("nothing to change" in r.get("result", ""), "publish again, unchanged: nothing pushed")
        r = call(M.gallery_publish, M.GalleryPublishIn, name="aquarium", about=ABOUT)
        check(r.startswith("Refused") and "not yours" in r, "a person's entry cannot be replaced")
        r = call(M.gallery_unpublish, M.GalleryRemoveIn, name="aquarium")
        check(r.startswith("Refused") and "not yours" in r, "nor removed")
        os.environ["LEDMATRIX_PUBLISHER"] = "someone_else"
        r = call(M.gallery_unpublish, M.GalleryRemoveIn, name=STEM)
        check(r.startswith("Refused"), "another publisher cannot remove it")
        os.environ["LEDMATRIX_PUBLISHER"] = "openclaw"
        r = json.loads(call(M.gallery_unpublish, M.GalleryRemoveIn, name=STEM))
        check(r.get("ok") and "pushed" in r["result"], "unpublish: pushed")
        check(git("diff", "--stat", start, "main", cwd=bare) == "", "the gallery is back exactly as it was")
        # a remote that refuses every push, as a read-only key does
        ro = pathlib.Path(d) / "ro.git"
        subprocess.run(["git", "clone", "-q", "--bare", str(bare), str(ro)], check=True, env=CLEAN)
        hook = ro / "hooks/pre-receive"
        hook.write_text("#!/bin/sh\necho 'ERROR: The key you are authenticating with has been marked as read only.' >&2\nexit 1\n")
        hook.chmod(0o755)
        os.environ["LEDMATRIX_GALLERY_REMOTE"] = "file://" + str(ro)
        r = call(M.gallery_publish, M.GalleryPublishIn, name=STEM, about=ABOUT)
        check("no write access" in r and "read only" in r, "no write access: said, with the owner's step")
    finally:
        SCRIPT.unlink(missing_ok=True)
check(not git("worktree", "list", "--porcelain").count("gallery-"), "no worktree left behind")
check(git("rev-parse", "HEAD") == here_head and git("status", "--porcelain") == here_status,
      "this checkout untouched: same HEAD, same status (git's hook variables were ignored)")
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)

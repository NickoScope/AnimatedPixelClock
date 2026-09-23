#!/usr/bin/env python3
"""gallery/index.json: what the portal's "Add from the gallery" lists.

The portal fetches this file from GitHub (raw.githubusercontent.com, which
answers any origin), shows each entry with its preview, and on "Add" fetches
the script itself and uploads it to the panel. Written from the scripts, so it
cannot disagree with them: the name is the file's stem in capitals (what the
panel calls an upload), the line is the script's own title comment
("-- NAME - what it is"), and the size is the file's.

    python3 tools/gallery_index.py           # rewrite gallery/index.json
    python3 tools/gallery_index.py --check   # exit 1 if it is stale
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAL = ROOT / "gallery"
OUT = GAL / "index.json"
MAX_BYTES = 51200   # LUA_USER_SRC_MAX, src/lua/lua_store.h


def build(gal=None):
    gal = pathlib.Path(gal) if gal else GAL
    items = []
    for f in sorted(gal.glob("*.lua")):
        text = f.read_text(encoding="utf-8")
        line = ""
        for l in text.splitlines()[:6]:
            m = re.match(r"--\s*([A-Z0-9_ ]+?)\s+-\s+(.+)$", l)
            if m:
                line = m.group(2).strip()
                break
        stem = f.stem
        # The panel's own rule (validStem, src/lua/lua_store.cpp). The portal
        # checks it again: a name outside it could reach into the page.
        if not re.fullmatch(r"[A-Za-z0-9_]{1,24}", stem):
            sys.exit(f"gallery_index: {f.name}: a name is 1 to 24 of letters, digits and underscore")
        prev = gal / "preview" / (stem + ".png")
        item = {
            "stem": stem,
            "name": stem.upper(),
            "file": f.name,
            "line": line,
            "bytes": len(text.encode("utf-8")),
            "preview": ("preview/" + prev.name) if prev.exists() else None,
        }
        # "-- @by openclaw": published by an agent (tools/agent/gallery.py
        # publish --by), which may replace or remove only its own.
        by = re.search(r"^--\s*@by\s+([A-Za-z0-9_-]{1,32})\s*$", "\n".join(text.splitlines()[:10]), re.M)
        if by:
            item["by"] = by.group(1)
        items.append(item)
        if items[-1]["bytes"] > MAX_BYTES:
            sys.exit(f"gallery_index: {f.name} is over the panel's {MAX_BYTES} B")
    return json.dumps({"version": 1, "maxBytes": MAX_BYTES, "effects": items}, indent=2, ensure_ascii=False) + "\n"


def builtin_drift(gal=None):
    """Gallery copies that no longer match their source in tools/luasim/scripts.

    A script kept in both places (the former built-ins since 2026-09-23, and the
    older gallery screens) is the same effect: the gallery copy is the source
    plus its tag and title lines. An edit to one and not the other would ship
    two different effects under one name."""
    gal = pathlib.Path(gal) if gal else GAL
    out = []
    for f in sorted(gal.glob("*.lua")):
        src = ROOT / "tools/luasim/scripts" / f.name
        if not src.exists():
            continue
        s = src.read_text(encoding="utf-8")
        body = [l for l in f.read_text(encoding="utf-8").splitlines(keepends=True)
                if not (l.startswith("-- @upload-only") or l.startswith("-- @by ") or
                        re.match(r"--\s*[A-Z0-9_ ]+?\s+-\s+.+$", l))]
        head = [l for l in s.splitlines(keepends=True)
                if not (l.startswith("-- @upload-only") or re.match(r"--\s*[A-Z0-9_ ]+?\s+-\s+.+$", l))]
        if "".join(body) != "".join(head):
            out.append(f.name)
    return out


if __name__ == "__main__":
    drift = builtin_drift()
    if drift:
        sys.exit("gallery_index: these gallery copies differ from their built-in source in "
                 "tools/luasim/scripts: " + ", ".join(drift) + ". Change both, or neither.")
    text = build()
    if "--check" in sys.argv:
        ok = OUT.exists() and OUT.read_text(encoding="utf-8") == text
        print("gallery/index.json " + ("in step" if ok else "STALE: run python3 tools/gallery_index.py"))
        sys.exit(0 if ok else 1)
    OUT.write_text(text, encoding="utf-8")
    print("wrote", OUT.relative_to(ROOT), "-", text.count('"stem"'), "effects")

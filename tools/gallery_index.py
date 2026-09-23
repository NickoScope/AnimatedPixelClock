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


def build():
    items = []
    for f in sorted(GAL.glob("*.lua")):
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
        prev = GAL / "preview" / (stem + ".png")
        items.append({
            "stem": stem,
            "name": stem.upper(),
            "file": f.name,
            "line": line,
            "bytes": len(text.encode("utf-8")),
            "preview": ("preview/" + prev.name) if prev.exists() else None,
        })
        if items[-1]["bytes"] > MAX_BYTES:
            sys.exit(f"gallery_index: {f.name} is over the panel's {MAX_BYTES} B")
    return json.dumps({"version": 1, "maxBytes": MAX_BYTES, "effects": items}, indent=2, ensure_ascii=False) + "\n"


if __name__ == "__main__":
    text = build()
    if "--check" in sys.argv:
        ok = OUT.exists() and OUT.read_text(encoding="utf-8") == text
        print("gallery/index.json " + ("in step" if ok else "STALE: run python3 tools/gallery_index.py"))
        sys.exit(0 if ok else 1)
    OUT.write_text(text, encoding="utf-8")
    print("wrote", OUT.relative_to(ROOT), "-", text.count('"stem"'), "effects")

#!/usr/bin/env python3
"""Gzip the config portal into src/web/web_assets.h: the page, its style and
script, the icon, and the Panel group's style and script.

The panel serves the portal from loop() at 70-100 KB/s, and the picture stands
still while it does. On 2026-09-14 / (128,660 B) held loop() for 1765 ms,
/panel.js (99,638 B) for 987 ms and /portal.js (44,115 B) for 537 ms.

The sources stay where they are, as the raw strings in src/web/web_pages.h,
web_panel_page.h and web_panel_js.h. Host checks read the scripts from there
(tools/media/check_media.py, tools/flightboard/check_portal_js.py,
tools/oscmusic/test_clip_maker.py), and so does every branch that changes the
portal. The firmware compiles only what this writes; nothing is served twice.

  python3 tools/web_assets_gen.py           # regenerate
  python3 tools/web_assets_gen.py --check   # exit 1 if web_assets.h is stale

The page is static and the same for every build. Three markers are resolved
here, never on the panel:
  %PANEL_NAV%, %PANEL_PAGES%  the Panel group's markup from web_panel_page.h; the
                              page drops what /api/portal says the build lacks
  ?v=%ASSETVER%               after /portal.css, /portal.js, /panel.css or
                              /panel.js: a hash of that file, so a browser keeps it
                              for a year only while its bytes stay the same
Any other %TOKEN% left in the page is refused, and so is a control in the
settings form that handlePortalValues() in web.cpp does not fill: the page
would show its HTML default, and Save would write that back.

The gzip carries no file name and mtime 0. A rerun keeps every blob that still
decompresses to its source, so another zlib on another machine neither rewrites
the header nor makes --check call it stale.
"""
import gzip, hashlib, pathlib, re, sys
from collections import namedtuple
from html.parser import HTMLParser

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB  = ROOT / "src/web"
OUT  = WEB / "web_assets.h"

PANEL_GUARD = "CONTROL_ENCODER_ENABLED"   # the builds that serve panel.css and panel.js

# Controls in the settings form that are not settings, with why.
NOT_SETTINGS = {
    "resetScope": "restores the oscilloscope defaults on this one save; unchecked on every load",
}

Asset = namedtuple("Asset", "name url note guard")


def raw_string(src, var):
    """The text of `static const char VAR[] PROGMEM = R"d(...)d";`, as the compiler sees it."""
    text = (WEB / src).read_text(encoding="utf-8")
    m = re.search(r'\b%s\[\]\s*PROGMEM\s*=\s*R"(\w*)\((.*?)\)\1"' % var, text, re.S)
    if not m:
        raise SystemExit(f"web_assets_gen: no raw string {var} in src/web/{src}")
    if "\r" in m[2]:
        raise SystemExit(f"web_assets_gen: {var} in src/web/{src} has CR line endings")
    return m[2]


def version(data):
    return hashlib.sha256(data).hexdigest()[:12]


def stamp(text, served, where):
    """Each /name?v=%ASSETVER% becomes /name?v=<hash of that file as served>."""
    def one(m):
        if m[1] not in served:
            raise SystemExit(f"web_assets_gen: {where} versions {m[1]}, which is not served from here")
        return f"{m[1]}?v={version(served[m[1]])}"
    return re.sub(r"(/[\w.]+)\?v=%ASSETVER%", one, text)


class FormControls(HTMLParser):
    """The name of every control inside <form id="cfgForm">."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.inside, self.names = False, []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "form":
            self.inside = a.get("id") == "cfgForm"
        elif self.inside and tag in ("input", "select", "textarea", "button") and a.get("name"):
            self.names.append(a["name"])

    def handle_endtag(self, tag):
        if tag == "form":
            self.inside = False


def check_form(page):
    """Every named control in the settings form is filled by /api/portal, and nothing else is."""
    parser = FormControls()
    parser.feed(page)
    names = parser.names
    filled = set(re.findall(r'\bform\["(\w+)"\]\s*=', (WEB / "web.cpp").read_text(encoding="utf-8")))
    problems = []
    if not names:
        problems.append('no named controls in <form id="cfgForm">')
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        problems.append("more than one control named " + ", ".join(dup))
    missing = sorted(set(names) - filled - set(NOT_SETTINGS))
    if missing:
        problems.append("in the form, not filled by handlePortalValues() in web.cpp: " + ", ".join(missing))
    unknown = sorted(filled - set(names))
    if unknown:
        problems.append("filled by handlePortalValues(), no such control in the form: " + ", ".join(unknown))
    if problems:
        raise SystemExit("web_assets_gen: the settings form and /api/portal disagree\n  " + "\n  ".join(problems))


def sources():
    """(Asset, the bytes it serves), in the order web_assets.h lists them."""
    portal_css = raw_string("web_pages.h", "PORTAL_CSS").encode()
    favicon    = raw_string("web_pages.h", "FAVICON_SVG").encode()
    panel_css  = raw_string("web_panel_page.h", "PANEL_CSS").encode()
    panel_js   = raw_string("web_panel_js.h", "PANEL_JS").encode()
    served = {"/panel.css": panel_css, "/panel.js": panel_js}

    # portal.js loads the Panel group's files, so their hashes are in it.
    portal_js = stamp(raw_string("web_pages.h", "PORTAL_JS"), served, "PORTAL_JS")
    if "%ASSETVER%" in portal_js:
        raise SystemExit("web_assets_gen: PORTAL_JS has a %ASSETVER% that is not after a served file's URL")
    portal_js = portal_js.encode()
    served.update({"/portal.css": portal_css, "/portal.js": portal_js})

    page = raw_string("web_pages.h", "PAGE_HTML")
    for marker, var in (("%PANEL_NAV%", "PANEL_NAV_HTML"), ("%PANEL_PAGES%", "PANEL_PAGES_HTML")):
        if page.count(marker) != 1:
            raise SystemExit(f"web_assets_gen: PAGE_HTML must hold {marker} exactly once")
        page = page.replace(marker, raw_string("web_panel_page.h", var))
    page = stamp(page, served, "PAGE_HTML")
    left = sorted(set(re.findall(r"%[A-Z][A-Z0-9_]*%", page)))
    if left:
        raise SystemExit("web_assets_gen: nothing is substituted on the panel any more, so these would reach "
                         "the browser as they are: " + ", ".join(left) +
                         "\n  a value belongs in handlePortalValues() in web.cpp, filled in by PORTAL_JS")
    check_form(page)

    return [
        (Asset("WEB_INDEX_GZ", "/", "PAGE_HTML in web_pages.h with PANEL_NAV_HTML and PANEL_PAGES_HTML\n"
               "// from web_panel_page.h spliced in, the same for every build", None), page.encode()),
        (Asset("WEB_PORTAL_CSS_GZ", "/portal.css", "PORTAL_CSS in web_pages.h", None), portal_css),
        (Asset("WEB_PORTAL_JS_GZ", "/portal.js", "PORTAL_JS in web_pages.h", None), portal_js),
        (Asset("WEB_FAVICON_GZ", "/favicon.svg", "FAVICON_SVG in web_pages.h", None), favicon),
        (Asset("WEB_PANEL_CSS_GZ", "/panel.css", "PANEL_CSS in web_panel_page.h", PANEL_GUARD), panel_css),
        (Asset("WEB_PANEL_JS_GZ", "/panel.js", "PANEL_JS in web_panel_js.h", PANEL_GUARD), panel_js),
    ]


def gunzip(blob):
    try:
        return gzip.decompress(blob)
    except Exception:
        return None


def existing_blobs():
    if not OUT.exists():
        return {}
    return {name: bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", body))
            for name, body in re.findall(r"static const uint8_t (\w+)\[\] PROGMEM = \{(.*?)\};",
                                         OUT.read_text(), re.S)}


def compress(name, plain, old):
    prev = old.get(name)
    # Kept only if it is still this content and still carries no name and no mtime.
    if prev and prev[3] == 0 and prev[4:8] == b"\0\0\0\0" and gunzip(prev) == plain:
        return prev
    return gzip.compress(plain, compresslevel=9, mtime=0)


def c_bytes(data, per_line=20):
    return "\n".join("  " + "".join(f"0x{b:02x}," for b in data[i:i + per_line])
                     for i in range(0, len(data), per_line))


def render(blobs):
    out = ["""#pragma once
// GENERATED by tools/web_assets_gen.py from the raw strings in src/web/web_pages.h,
// web_panel_page.h and web_panel_js.h - do not edit.
//
// The config portal as gzip, served with Content-Encoding: gzip and no
// uncompressed copy. The page carries no values: it fetches them from
// /api/portal. Edit the raw strings and rerun the generator; the pre-commit hook
// refuses a stale copy.

#include <Arduino.h>
"""]
    guard = None
    for a, plain, gz in blobs:
        if a.guard != guard:
            if guard:
                out.append(f"#endif  // {guard}\n")
            if a.guard:
                out.append(f"#if defined({a.guard})\n")
            guard = a.guard
        etag = f'#define {a.name[:-3]}_ETAG "\\"{version(plain)}\\""\n' if a.url == "/" else ""
        out.append(f"// {a.url} - {a.note}: {len(plain):,} B, gzip {len(gz):,} B\n{etag}"
                   f"static const uint8_t {a.name}[] PROGMEM = {{\n{c_bytes(gz)}\n}};\n")
    if guard:
        out.append(f"#endif  // {guard}\n")
    return "\n".join(out)


def main():
    old = existing_blobs()
    blobs = [(a, plain, compress(a.name, plain, old)) for a, plain in sources()]
    text = render(blobs)
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print("web_assets_gen: src/web/web_assets.h does not match the raw strings it is made from")
            sys.exit(1)
        return
    OUT.write_text(text)
    for a, plain, gz in blobs:
        print(f"  {a.url:14s} {len(plain):7,} B -> gzip {len(gz):6,} B ({100 * len(gz) / len(plain):.0f}%)")
    print(f"{OUT.relative_to(ROOT)}: {len(text):,} bytes")


main()

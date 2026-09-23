#!/usr/bin/env python3
"""The portal's Effects card and gallery, run without a browser.

Takes panel.js from src/web/web_panel_js.h, runs it in JavaScriptCore (built
into macOS) over a small stand-in DOM, feeds it an /api/lua answer and the
gallery index, and checks what it draws and what it sends:
  - every effect: a walk switch (ticked as inWalk says), Show, and Delete only
    on an uploaded one; a switch change posts {walk:{i,on}};
  - the gallery: one row per index entry, "On the panel" for an effect already
    there (spaces for underscores), "Add" otherwise;
  - Add fetches the script from GitHub and uploads it to
    /api/lua/upload?name=<NAME> as the multipart field "script".

    python3 tools/web/check_effects_ui.py
"""
import json, pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
JSC = "/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"

src = (ROOT / "src/web/web_panel_js.h").read_text()
js = re.search(r'R"JS\((.*)\)JS"', src, re.S)
if not js:
    js = re.search(r'R"(\w*)\((.*)\)\1"', src, re.S)
    body = js.group(2)
else:
    body = js.group(1)
# Expose the functions under test from inside the IIFE.
end = body.rstrip().rfind("})();")
assert end > 0, "panel.js is not one IIFE"
body = body[:end] + "globalThis.__t = {renderLua: renderLua, renderGallery: renderGallery, galAdd: galAdd, " \
                    "setGal: function (g) { galIndex = g; }};\n" + body[end:]

LUA = {"success": True, "current": -1, "effects": ["FOOTBALL CLOCK", "LA GIOCONDA", "AQUARIUM", "AUTUMN"],
       "inWalk": [True, False, True, True],
       "uploaded": {"count": 2, "slots": 12, "scripts": [{"i": 2, "name": "AQUARIUM", "bytes": 31417},
                                                        {"i": 3, "name": "AUTUMN", "bytes": 42177}]}}
GAL = json.loads((ROOT / "gallery/index.json").read_text())

harness = r"""
var log = [];
function El(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.handlers = {}; this.classList = { toggle: function () {}, add: function () {}, remove: function () {} };
  this._html = ''; this.textContent = ''; this.dataset = {}; this.style = {}; this.checked = false; this.disabled = false; }
El.prototype.appendChild = function (c) { this.children.push(c); return c; };
El.prototype.insertAdjacentHTML = function (w, h) { this._html += h; };
El.prototype.addEventListener = function (e, f) { this.handlers[e] = f; };
El.prototype.setAttribute = function (k, v) { this.attrs[k] = v; };
El.prototype.getAttribute = function (k) { return this.attrs[k] == null ? null : this.attrs[k]; };
Object.defineProperty(El.prototype, 'innerHTML', { get: function () { return this._html; }, set: function (v) { this._html = v; this.children = []; this._q = null; } });
El.prototype._parse = function () {
  if (this._q) return this._q;
  var q = { input: [], button: [] }, m, re = /<(input|button)([^>]*)>([^<]*)/g;
  while ((m = re.exec(this._html))) { var e = new El(m[1]); e.checked = /checked/.test(m[2]); e.disabled = /disabled/.test(m[2]); e.textContent = m[3]; q[m[1]].push(e); }
  return (this._q = q);
};
El.prototype.querySelector = function (s) { var q = this._parse(); return (q[s] || [])[0] || null; };
El.prototype.querySelectorAll = function (s) { var q = this._parse(); return q[s] || []; };
El.prototype.closest = function () { return null; };
var els = {};
var document = { getElementById: function (id) { return els[id] || (els[id] = new El('div')); },
  querySelector: function () { return null; }, querySelectorAll: function () { return []; },
  createElement: function (t) { return new El(t); }, addEventListener: function () {}, documentElement: new El('html'), hidden: false };
els.panelRoot = new El('div'); els.panelRoot.attrs['data-f'] = 'lua';
var window = globalThis; var location = { hash: '' };
function MutationObserver() { this.observe = function () {}; }
function Blob(parts, o) { this.text = parts.join(''); this.type = o && o.type; }
function FormData() { this.parts = []; this.append = function (k, v, n) { this.parts.push([k, v, n]); }; }
var fetched = [];
function fetch(url, o) {
  fetched.push({ url: url, o: o });
  var resp = function (obj, text) { return Promise.resolve({ ok: true, status: 200, json: function () { return Promise.resolve(obj); }, text: function () { return Promise.resolve(text); } }); };
  if (/index\.json/.test(url)) return resp(GAL);
  if (/\.lua$/.test(url)) return resp(null, '-- @upload-only\n-- CANNES - test\nfunction draw() end\n');
  if (/\/api\/lua\/upload/.test(url)) return resp({ success: true, name: 'CANNES', index: 4 });
  if (/\/api\/lua/.test(url)) return resp(LUA);
  return resp({ success: true, pages: [], now: {}, carousel: {}, styles: [] });
}
function alert() {} function confirm() { return true; }
function setTimeout(f) { return 0; } function clearTimeout() {} function setInterval() { return 0; }
"""

test = r"""
var t = globalThis.__t, out = {};
t.renderLua(LUA);
var rows = els.luaList.children;
out.effects = rows.map(function (r) {
  var b = r.querySelectorAll('button').map(function (x) { return x.textContent; });
  return { html: /<strong>([^<]*)/.exec(r._html)[1], walk: r.querySelector('input').checked, buttons: b };
});
// a switch change posts walk
var box = rows[1].querySelector('input'); box.checked = true; box.handlers.change();
out.walkPost = fetched.filter(function (f) { return f.o && f.o.body && /walk/.test(f.o.body); }).map(function (f) { return f.o.body; });
t.setGal(GAL); t.renderGallery();
out.gallery = els.galList.children.map(function (r) { return { name: /<strong>([^<]*)/.exec(r._html)[1], button: r.querySelector('button').textContent, disabled: r.querySelector('button').disabled }; });
var g = GAL.effects.filter(function (e) { return e.stem === 'cannes'; })[0];
var btn = new El('button');
t.galAdd(g, btn);
drainMicrotasks();
var up = fetched.filter(function (f) { return /\/api\/lua\/upload/.test(f.url); })[0];
out.upload = up ? { url: up.url, method: up.o.method, field: up.o.body.parts[0][0], file: up.o.body.parts[0][2], text: up.o.body.parts[0][1].text.slice(0, 20) } : null;
out.fetchedGitHub = fetched.filter(function (f) { return /raw\.githubusercontent/.test(f.url); }).map(function (f) { return f.url.replace(/^.*\/gallery\//, ''); });
print(JSON.stringify(out));
"""

with tempfile.TemporaryDirectory() as d:
    p = pathlib.Path(d) / "t.js"
    p.write_text("var LUA = " + json.dumps(LUA) + ";\nvar GAL = " + json.dumps(GAL) + ";\n" + harness + body +
                 "\n" + test)
    r = subprocess.run([JSC, str(p)], capture_output=True, text=True, timeout=60)
    if r.returncode or not r.stdout.strip():
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit("check_effects_ui: the portal script did not run")
    out = json.loads(r.stdout.strip().splitlines()[-1])

fails = 0
def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += 0 if cond else 1

e = {x["html"]: x for x in out["effects"]}
check(len(out["effects"]) == 4, "one row per effect")
check(e["LA GIOCONDA"]["walk"] is False and e["AQUARIUM"]["walk"] is True, "the switch shows inWalk")
check(e["FOOTBALL CLOCK"]["buttons"] == ["Show"], "a built-in effect: Show only")
check(e["AUTUMN"]["buttons"] == ["Show", "Delete"], "an uploaded effect: Show and Delete")
check(out["walkPost"] == [json.dumps({"walk": {"i": 1, "on": True}}, separators=(",", ":"))], "a switch posts {walk:{i,on}}")
gal = {x["name"]: x for x in out["gallery"]}
check(len(out["gallery"]) == len(GAL["effects"]), "one gallery row per index entry")
check(gal["AQUARIUM"]["button"] == "On the panel" and gal["AQUARIUM"]["disabled"], "an effect already there: On the panel")
check(gal["LA GIOCONDA"]["button"] == "On the panel", "a built-in one (underscores as spaces): On the panel")
check(gal["CANNES"]["button"] == "Add" and not gal["CANNES"]["disabled"], "one not there: Add")
check(out["fetchedGitHub"][-1] == "cannes.lua", "Add fetches the script from GitHub")
u = out["upload"] or {}
check(u.get("url") == "/api/lua/upload?name=CANNES" and u.get("method") == "POST", "and uploads it to /api/lua/upload?name=CANNES")
check(u.get("field") == "script" and u.get("file") == "cannes.lua" and u.get("text", "").startswith("-- @upload-only"),
      "as the multipart field 'script', the script's own text")
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)

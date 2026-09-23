#!/usr/bin/env python3
"""The portal's Effects card and gallery, run without a browser.

Takes panel.js from src/web/web_panel_js.h, runs it in JavaScriptCore (built
into macOS) over a small stand-in DOM, feeds it an /api/lua answer and the
gallery index, and checks what it draws and what it sends:
  - every effect: a walk switch (ticked as inWalk says), Show, and Delete only
    on an uploaded one; a switch change posts {walk:{i,name,on}}, and a refused
    one puts the tick back; Delete asks first and posts {delete:<its name>};
    a name is escaped, never markup;
  - the Pages card: an effect's own switch posts {enable:{page,name,on}};
  - the gallery: one row per index entry, "On the panel" for an effect already
    there (spaces for underscores), "Add" otherwise;
  - Add fetches the script from GitHub and uploads it to
    /api/lua/upload?name=<NAME> as the multipart field "script"; a refused
    upload gives the button back;
  - an index entry naming anything but <stem>.lua and preview/<stem>.png is
    not drawn; GitHub unreachable is said once and not asked again for a
    minute; "all slots used" goes away when a slot is free again.

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
                    "setGal: function (g) { galIndex = g; }, galLoad: galLoad, renderPages: renderPages, " \
                    "resetGal: function () { galIndex = null; galFailedAt = 0; }};\n" + body[end:]

LUA = {"success": True, "current": -1, "effects": ["FOOTBALL CLOCK", "LA GIOCONDA", "AQUARIUM", "AUTUMN LEAVES"],
       "inWalk": [True, False, True, True],
       "uploaded": {"count": 2, "slots": 12, "scripts": [{"i": 2, "name": "AQUARIUM", "bytes": 31417},
                                                        {"i": 3, "name": "autumn_leaves", "bytes": 42177}]}}
GAL = json.loads((ROOT / "gallery/index.json").read_text())

harness = r"""
var log = [];
function El(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.handlers = {}; this.classList = { toggle: function () {}, add: function () {}, remove: function () {} };
  this._html = ''; this.textContent = ''; this.dataset = {}; this.style = {}; this.checked = false; this.disabled = false; }
El.prototype.appendChild = function (c) { this.children.push(c); return c; };
El.prototype.insertAdjacentHTML = function (w, h) {   // keeps the elements already handed out
  var q = this._q; this._html += h;
  if (q) { var add = new El('div'); add._html = h; var n = add._parse(); q.input = q.input.concat(n.input); q.button = q.button.concat(n.button); }
};
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
var fetched = [], mode = {};
function fetch(url, o) {
  fetched.push({ url: url, o: o });
  var resp = function (obj, text, status) { status = status || 200; return Promise.resolve({ ok: status < 300, status: status, json: function () { return Promise.resolve(obj); }, text: function () { return Promise.resolve(text); } }); };
  if (/index\.json/.test(url)) return mode.offline ? Promise.reject(new Error('Load failed')) : resp(mode.gal || GAL);
  if (mode.walkFail && o && o.body && /walk/.test(o.body)) return resp({ success: false, error: 'refused' }, null, 409);
  if (mode.uploadFail && /\/api\/lua\/upload/.test(url)) return resp({ success: false, error: 'an effect called CANNES is already on the panel' }, null, 400);
  if (/\.lua$/.test(url)) return resp(null, '-- @upload-only\n-- CANNES - test\nfunction draw() end\n');
  if (/\/api\/lua\/upload/.test(url)) return resp({ success: true, name: 'CANNES', index: 4 });
  if (/\/api\/lua/.test(url)) return resp(LUA);
  return resp({ success: true, pages: [], now: {}, carousel: {}, styles: [] });
}
var confirmed = [];
function alert() {} function confirm(q) { confirmed.push(q); return true; }
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

// a refused switch puts the tick back
mode.walkFail = true;
var box2 = t.renderLua && els.luaList.children[0].querySelector('input');
box2.checked = false; box2.handlers.change(); drainMicrotasks();
out.walkRolledBack = box2.checked === true;
mode.walkFail = false;
// Delete asks, then posts the uploaded name
fetched = [];
els.luaList.children[3].querySelectorAll('button')[1].handlers.click(); drainMicrotasks();
out.deleteAsked = confirmed.length === 1 && /AUTUMN LEAVES/.test(confirmed[0]);
out.deletePost = fetched.filter(function (f) { return f.o && f.o.body && /delete/.test(f.o.body); }).map(function (f) { return f.o.body; });
// a name is text, not markup
var evil = JSON.parse(JSON.stringify(LUA)); evil.effects[0] = '<img src=x onerror=1>';
t.renderLua(evil);
out.nameEscaped = els.luaList.children[0]._html.indexOf('<img') < 0 && els.luaList.children[0]._html.indexOf('&lt;img') >= 0;
t.renderLua(LUA);
// a refused upload gives the button back
mode.uploadFail = true;
var btn2 = new El('button'); t.galAdd(g, btn2); drainMicrotasks();
out.uploadFailButton = btn2.textContent === 'Add' && !btn2.disabled;
out.uploadFailMsg = els.galMsg.textContent;
mode.uploadFail = false;
// a bad index entry is not drawn, and the preview src is escaped
var bad = JSON.parse(JSON.stringify(GAL));
bad.effects.push({ stem: 'x', name: 'X', file: '../../other/x.lua', line: '', bytes: 10, preview: null });
bad.effects.push({ stem: 'y', name: 'Y', file: 'y.lua', line: '', bytes: 10, preview: 'preview/y.png" onerror="alert(1)' });
bad.effects.push({ stem: 'z"a', name: 'Z"A', file: 'z"a.lua', line: '', bytes: 10, preview: null });
t.setGal(bad); t.renderGallery();
out.badDrawn = els.galList.children.length;
// GitHub unreachable: said once, not asked again within the minute
t.resetGal(); mode.offline = true; fetched = [];
t.galLoad(); drainMicrotasks(); t.galLoad(); drainMicrotasks(); t.galLoad(); drainMicrotasks();
out.offlineFetches = fetched.filter(function (f) { return /index\.json/.test(f.url); }).length;
out.offlineMsg = els.galList._html;
mode.offline = false;
// all slots used, then one free
var full = JSON.parse(JSON.stringify(LUA)); full.uploaded.count = 12;
t.setGal(GAL); t.renderLua(full);
out.fullMsg = els.galMsg.textContent;
t.renderLua(LUA);
out.freeMsg = els.galMsg.textContent;
// the Pages card: an effect's own switch
fetched = [];
var PANEL = { now: { page: 0 }, cardsOn: false, pages: [
  { i: 0, key: 'clock', name: 'CLOCK', on: true },
  { i: 1, key: 'lua', name: 'AQUARIUM', on: true, effectOn: true },
  { i: 2, key: 'lua', name: 'AUTUMN', on: true, effectOn: false },
  { i: 3, key: 'lua', name: '', on: true } ] };
try {
  t.renderPages(PANEL);
  var pg = els.pnPages.children, sw = null;
  pg.forEach(function (r) { if (/AUTUMN/.test(r._html) && r.querySelector('input')) sw = r.querySelector('input'); });
  out.pageSwitchOff = sw ? sw.checked === false : 'none';
  if (sw) { sw.checked = true; sw.handlers.change(); drainMicrotasks(); }
  out.pagePost = fetched.filter(function (f) { return f.o && f.o.body && /enable/.test(f.o.body); }).map(function (f) { return f.o.body; });
} catch (err) { out.pagesError = String(err); }
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
check(e["AUTUMN LEAVES"]["buttons"] == ["Show", "Delete"], "an uploaded effect: Show and Delete")
check(out["walkPost"] == [json.dumps({"walk": {"i": 1, "name": "LA GIOCONDA", "on": True}}, separators=(",", ":"))],
      "a switch posts {walk:{i,name,on}}")
check(out["walkRolledBack"], "a refused switch puts the tick back")
check(out["deleteAsked"], "Delete asks first, naming the effect")
check(out["deletePost"] == ['{"delete":"autumn_leaves"}'], "and posts {delete:<the uploaded stem>}, not the name shown")
check(out["nameEscaped"], "an effect's name is escaped, never markup")
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
check(out["uploadFailButton"] and "already" in out["uploadFailMsg"], "a refused upload gives the button back and says why")
check(out["badDrawn"] == len(GAL["effects"]), "index entries naming a path or markup are not drawn")
check(out["offlineFetches"] == 1 and "could not be read" in out["offlineMsg"], "GitHub unreachable: said once, asked again only after a minute")
check("slots are used" in out["fullMsg"] and out["freeMsg"] == "", "'all slots used' shows, and goes when a slot is free")
check(not out.get("pagesError") and out.get("pageSwitchOff") is True, "the Pages card: an effect switched off shows unticked" + (" (" + out.get("pagesError", "") + ")" if out.get("pagesError") else ""))
check(out.get("pagePost") == ['{"enable":{"page":2,"name":"AUTUMN","on":true}}'], "and its switch posts {enable:{page,name,on}}")
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)

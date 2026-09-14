#!/usr/bin/env python3
"""Run the Flight board page's script in JavaScriptCore, against a stub DOM and a mocked API.

The script is taken from src/web/web_panel_js.h as the panel serves it. A hook
added to this run's copy only exposes the page's functions; `document`,
`fetch`, timers and `confirm` are stand-ins. What it checks: a hostile row is
escaped, tracked flights come first, the airport list is by id, usage and
budget fields, diagnostics warnings, the tracker limit, client-side ident and
name checks, the accent-folded airport search, and the POST bodies sent.

What it cannot check: layout, CSS, a real browser's DOM. macOS only (jsc).

  python3 tools/flightboard/check_portal_js.py
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
JSC = "/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
NOW = 1789410732

ADV = [4] * 95
for i, ch in enumerate(chr(32 + k) for k in range(95)):
    g = json.load(open(ROOT / "tools/picopixel.json"))["glyphs"].get(ch)
    if g:
        ADV[i] = g["adv"]

BUILTIN = [("LFMD", "CEQ", "CANNES", "Europe/Paris"), ("LFMN", "NCE", "NICE", "Europe/Paris"),
           ("LFPG", "CDG", "PARIS CDG", "Europe/Paris"), ("EGLL", "LHR", "LONDON", "Europe/London"),
           ("EDDF", "FRA", "FRANKFURT", "Europe/Berlin"), ("EHAM", "AMS", "AMSTERDAM", "Europe/Amsterdam")]
MOCK = {
    "success": True, "page": 3, "showing": True, "airport": 100, "dir": "alt", "side": "arr", "altS": 10,
    "airports": [{"id": i, "code": c, "iata": a, "name": n, "tz": z, "kind": "builtin"} for i, (c, a, n, z) in enumerate(BUILTIN)]
    + [{"id": 100, "code": "KJFK", "iata": "JFK", "name": "NEW YORK", "tz": "America/New_York", "kind": "custom"}],
    "limits": {"custom": 6, "name": 12, "namePx": 56, "track": 3, "advance": ADV},
    "board": {"have": True, "source": "aeroapi", "times": "airport", "tz": "America/New_York", "clock": "14:32", "cue": "-6",
              "utcOffset": -14400, "apt": "KJFK", "name": "NEW YORK", "dir": "arr", "upd": "20:15", "age": 900, "now": 2,
              "sides": {"arr": {"have": True, "n": 3, "upd": "20:15", "age": 900}, "dep": {"have": False}},
              "rows": [{"tm": "19:58", "fn": "IB3456", "ct": "BCN", "cy": "BARCELONA", "st": "land", "w": "LANDED"},
                       {"tm": "20:22", "fn": "AF7302", "ct": "CDG", "cy": "PARIS", "st": "dep", "w": "IN AIR"},
                       {"tm": "21:45", "fn": "<b>X</b>", "ct": "<i>", "cy": "\"><img src=x onerror=alert(1)>", "st": "sched", "w": "DUE"}]},
    "direct": {"built": True, "key": True, "state": "WAITING", "fetching": False, "calls": 14, "fails": 1,
               "budget": {"floor_min": 15, "day_cap": 30, "month_cap": 900},
               "bounds": {"floor_min": [5, 240], "day_cap": [0, 1000], "month_cap": [0, 20000]},
               "usage": {"today": 12, "month": 341, "total": 341, "boardDayCap": 24, "usdPerCall": 0.005, "usdToday": 0.06,
                         "usdMonth": 1.705, "usdMonthCap": 4.5, "dayEndsInS": 12600, "spacingS": 7},
               "lists": [{"name": "arrivals", "have": True, "age": 900, "kept": 15, "seen": 15, "noTime": 0, "noIdent": 0, "overflow": 0, "more": True},
                         {"name": "departures", "have": False, "refusedWaitS": 20000}],
               "last": {"state": "OK", "http": 200, "what": "scheduled_arrivals", "for": "KJFK", "ago": 300, "bytes": 31744,
                        "jsonPeak": 14000, "heapBefore": 61000, "heapMin": 44000, "stackFree": 4100},
               "sample": ""},
    "tracked": [
        {"ident": "AFR7301", "added": NOW - 7200, "state": "enroute", "http": 200, "age": 240, "nextIn": 360, "fn": "AF7301",
         "from": "NCE", "to": "ORY", "gate": "A7", "current": True, "flights": 3, "shown": NOW + 3120, "delayMin": 7,
         "dep": {"sched": NOW - 2400, "est": NOW - 2100, "act": NOW - 1980, "tz": "Europe/Paris", "zone": "sent",
                 "schedHm": "19:52", "estHm": "19:57", "actHm": "19:59"},
         "arr": {"sched": NOW + 2700, "est": NOW + 3120, "tz": "Europe/Paris", "zone": "sent", "schedHm": "21:17", "estHm": "21:24"},
         "tmEnd": "arr", "w": "IN AIR", "tm": "21:24"},
        {"ident": "BA336", "added": NOW - 600, "state": "landed", "http": 200, "age": 100, "nextIn": -1, "fn": "BA336", "from": "LHR",
         "to": "NCE", "gate": "", "current": True, "flights": 2, "shown": NOW - 720, "delayMin": 3,
         "dep": {"sched": NOW - 9000, "act": NOW - 8900, "tz": "UTC", "zone": "utc", "schedHm": "16:02", "actHm": "16:04"},
         "arr": {"sched": NOW - 900, "act": NOW - 720, "tz": "Europe/Paris", "zone": "list", "schedHm": "20:17", "actHm": "20:20"},
         "expiresIn": 6480, "tmEnd": "arr", "w": "LANDED", "tm": "20:20"},
        {"ident": "EZY9999", "added": NOW - 60, "state": "notfound", "http": 200, "age": 30, "nextIn": 21570, "w": "NO FLIGHT", "tm": "--:--"}],
}
# What the search list would hold: made-up entries in mwgg/Airports' shape.
DB = {"KJFK": {"icao": "KJFK", "iata": "JFK", "name": "John F Kennedy International Airport", "city": "New York", "country": "US", "tz": "America/New_York"},
      "RJTT": {"icao": "RJTT", "iata": "HND", "name": "Tokyo Haneda International Airport", "city": "Tokyo", "country": "JP", "tz": "Asia/Tokyo"},
      "LSZH": {"icao": "LSZH", "iata": "ZRH", "name": "Zürich Airport", "city": "Zürich", "country": "CH", "tz": "Europe/Zurich"}}

SHIM = r'''
var ELS = {}, POSTS = [];
function El(id) {
  var e = { id: id, hidden: false, value: '', textContent: '', _html: '', children: [], disabled: false, attrs: {}, dataset: {},
    options: [], selectionStart: 0,
    classList: { s: {}, toggle: function (c, on) { this.s[c] = on === undefined ? !this.s[c] : !!on; },
                 contains: function (c) { return !!this.s[c]; }, add: function (c) { this.s[c] = true; } },
    addEventListener: function () {},
    getAttribute: function (k) { return k === 'data-f' ? 'panel flights' : (this.attrs[k] === undefined ? null : this.attrs[k]); },
    setAttribute: function (k, v) { this.attrs[k] = String(v); },
    appendChild: function (c) { this.children.push(c); if (this.id === 'fbApt') this.options.push(c); return c; },
    querySelector: function (sel) { return El('q:' + sel); }, querySelectorAll: function () { return []; },
    closest: function () { return null; }, focus: function () {}, setSelectionRange: function () {}, parentNode: null };
  Object.defineProperty(e, 'innerHTML', { get: function () { return this._html; },
    set: function (v) { this._html = String(v); if (this.id === 'fbApt' && v === '') this.options = []; this.children = []; } });
  return e;
}
var document = { hidden: false, activeElement: null, visibilityState: 'visible', documentElement: El('html'), body: El('body'),
  getElementById: function (id) { return ELS[id] || (ELS[id] = El(id)); },
  querySelectorAll: function () { return []; }, querySelector: function () { return null; },
  createElement: function (t) { return El('new:' + t); }, addEventListener: function () {} };
var window = globalThis;
function MutationObserver() { this.observe = function () {}; }
function setTimeout() { return 0; } function clearTimeout() {} function setInterval() { return 0; }
function confirm() { return true; }
var location = { hash: '' };
function fetch(url, o) {
  var body = o && o.body ? JSON.parse(o.body) : null;
  if (body) POSTS.push(body);
  var d = url.indexOf('cdn.jsdelivr.net') >= 0 ? DB : url.indexOf('/api/flightboard') === 0 ? MOCK : { success: true, now: {}, pages: [] };
  return Promise.resolve({ ok: true, status: 200, json: function () { return Promise.resolve(JSON.parse(JSON.stringify(d))); } });
}
'''

TESTS = r'''
var fb = globalThis.__fb, fails = 0, $ = function (id) { return document.getElementById(id); };
function check(name, cond) { print((cond ? 'ok   ' : 'FAIL ') + name); if (!cond) fails++; }
fb.renderFb(JSON.parse(JSON.stringify(MOCK)));
var rows = $('fbRows').innerHTML;
check('a hostile row is escaped', rows.indexOf('<img') < 0 && rows.indexOf('<b>') < 0 && rows.indexOf('&lt;b&gt;X') >= 0);
check('tracked flights come first, on the blue band', (rows.match(/background:#001a46/g) || []).length === 12 && rows.indexOf('AF7301') < rows.indexOf('IB3456'));
check('the direct build shows its three cards', !$('fbTrackCard').hidden && !$('fbAddCard').hidden && !$('fbBudgetCard').hidden);
check('the airport list is by id, custom one marked', $('fbApt').options.length === 7 && $('fbApt').options[6].textContent.indexOf('yours') > 0 && String($('fbApt').value) === '100');
check('usage and cost', $('fbUse').innerHTML.indexOf('12/30') >= 0 && $('fbUse').innerHTML.indexOf('$1.71') >= 0 && $('fbUse').innerHTML.indexOf('$4.50') >= 0);
check('budget inputs from the panel', String($('fbDay').value) === '30' && String($('fbMonth').value) === '900' && String($('fbFloor').value) === '15');
check('the budget hint names the price', $('fbBudgetMsg').textContent.indexOf('$0.005 a call') >= 0);
check('diagnostics warn of pages not fetched and a refused list', $('fbDirect').innerHTML.indexOf('more pages not fetched') >= 0 && $('fbDirect').innerHTML.indexOf('refused, again in') >= 0);
check('trackers listed, expiry spelled out', $('fbTracks').children.length === 3 && $('fbTracks').children[1]._html.indexOf('removes itself in 1 h 48 min') >= 0);
check('count tags', $('fbTrkCount').textContent === '3 of 3' && $('fbCount').textContent === '1 of 6');
check('board times labelled as the airport\'s', rows.indexOf('local time') >= 0 && $('fbFeed').innerHTML.indexOf('local at JFK · America/New_York · UTC-4') >= 0 && $('fbFeed').innerHTML.indexOf('-6 h beside its clock') >= 0);
check('tracked times in each end\'s zone, from the panel', $('fbTracks').children[0]._html.indexOf('left the gate 19:59 NCE time') >= 0 && $('fbTracks').children[0]._html.indexOf('arrives 21:24 ORY time (scheduled 21:17)') >= 0);
check('an end without a zone says UTC', $('fbTracks').children[1]._html.indexOf('left the gate 16:04 UTC, no zone known') >= 0 && $('fbTracks').children[1]._html.indexOf('landed 20:20 NCE time') >= 0);
var ha = JSON.parse(JSON.stringify(MOCK)); ha.board.source = 'mqtt'; ha.board.times = 'home-assistant'; ha.board.cue = 'HA'; delete ha.board.utcOffset;
fb.renderFb(ha);
check('the MQTT fallback is shown as such, with HA time', $('fbRows').innerHTML.indexOf('HA time') >= 0 && $('fbFeed').innerHTML.indexOf('fallback: Home Assistant over MQTT') >= 0 && $('fbFeed').innerHTML.indexOf('cannot be converted') >= 0);
fb.renderFb(JSON.parse(JSON.stringify(MOCK)));
$('fbIdent').value = 'BAW336'; fb.fbCheckIdent();
check('a fourth tracker is refused', $('fbTrackAdd').disabled && $('fbTrkMsg').textContent.indexOf('Three flights') === 0);
var one = JSON.parse(JSON.stringify(MOCK)); one.tracked = one.tracked.slice(0, 1); fb.renderFb(one);
$('fbIdent').value = '12345'; fb.fbCheckIdent();
check('an ident without an airline code is refused', $('fbTrackAdd').disabled);
$('fbIdent').value = 'BAW336'; fb.fbCheckIdent();
check('a good ident is accepted', !$('fbTrackAdd').disabled);
fb.fbTrack();
fb.fbFind('zur'); drainMicrotasks();
check('the search folds accents', $('fbResults').children.length === 1 && $('fbResults').children[0]._html.indexOf('LSZH') >= 0);
fb.fbFind('JFK'); drainMicrotasks();
check('an airport already listed cannot be picked', $('fbResults').children[0]._html.indexOf(' disabled') >= 0);
fb.fbChoose({ icao: 'LSZH', iata: 'ZRH', name: 'Zürich Airport', city: 'Zürich', tz: 'Europe/Zurich' });
check('the name is suggested from the city, folded', $('fbName').value === 'ZURICH' && !$('fbAdd').disabled);
$('fbName').value = 'WWWWWWWWWWWW'; fb.fbCheckName();
check('a name too wide for the header is refused', $('fbAdd').disabled && $('fbNameMsg').textContent.indexOf('Too wide') === 0);
$('fbName').value = 'ZURICH'; fb.fbCheckName();
fb.fbAddAirport(true); drainMicrotasks();
$('fbDay').value = '40'; $('fbMonth').value = '1000'; $('fbFloor').value = '20'; fb.fbSaveBudget();
$('fbDay').value = '4.5'; fb.fbSaveBudget();
check('a budget that is not whole numbers is refused before posting', $('fbBudgetMsg').textContent.indexOf('Whole numbers') === 0);
drainMicrotasks();
check('the POST bodies', JSON.stringify(POSTS) === JSON.stringify([{ track: 'BAW336' },
  { add: { icao: 'LSZH', iata: 'ZRH', name: 'ZURICH', tz: 'Europe/Zurich', select: true } },
  { budget: { day_cap: 40, month_cap: 1000, floor_min: 20 } }]));
print('RESULT ' + fails);
'''


def main():
    src = (ROOT / "src/web/web_panel_js.h").read_text()
    js = re.search(r'R"JS\((.*)\)JS"', src, re.S).group(1)
    end = js.rindex("})();")
    hook = ("\nglobalThis.__fb = { renderFb: renderFb, fbFind: fbFind, fbChoose: fbChoose, fbCheckName: fbCheckName, "
            "fbAddAirport: fbAddAirport, fbTrack: fbTrack, fbCheckIdent: fbCheckIdent, fbSaveBudget: fbSaveBudget };\n")
    program = (f"var MOCK = {json.dumps(MOCK)};\nvar DB = {json.dumps(DB, ensure_ascii=False)};\n" + SHIM
               + js[:end] + hook + js[end:] + TESTS)
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(program)
    r = subprocess.run([JSC, f.name], capture_output=True, text=True)
    pathlib.Path(f.name).unlink()
    print(r.stdout, end="")
    if r.stderr or r.returncode:
        print(r.stderr)
    ok = r.returncode == 0 and "RESULT 0" in r.stdout
    print("\nportal JS checks:", "passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


main()

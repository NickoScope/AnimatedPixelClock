// The Panel group's script, served from /panel.js. Included by web_panel_page.h.
//
// Loaded before portal.js on purpose: it removes the pages this build does not
// carry (data-need) before portal.js collects the navigation. Everything else
// waits for data. Only the Panel page in view is polled, and nothing while the
// tab is hidden; the knob tester polls fastest, at 250 ms, one request at a time.
#pragma once
#include <Arduino.h>

static const char PANEL_JS[] PROGMEM = R"JS(
(function () {
'use strict';
var root = document.getElementById('panelRoot');
if (!root) return;
var F = {};
(root.getAttribute('data-f') || '').split(' ').forEach(function (k) { if (k) F[k] = 1; });
Array.prototype.slice.call(document.querySelectorAll('[data-need]')).forEach(function (el) {
  if (!F[el.getAttribute('data-need')] && el.parentNode) el.parentNode.removeChild(el);
});

function $(id) { return document.getElementById(id); }
function each(list, fn) { Array.prototype.forEach.call(list, fn); }
function esc(s) { return String(s == null ? '' : s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }
function setText(id, t) { var e = $(id); if (e) e.textContent = t; }
function focused(el) { return el && document.activeElement === el; }
function cap(s) { return String(s || '').toLowerCase().replace(/\b[a-z]+/g, function (w) { return w.length <= 3 && /cdg|lhr/.test(w) ? w.toUpperCase() : w.charAt(0).toUpperCase() + w.slice(1); }); }
function ago(s) {
  if (s == null) return '--';
  if (s < 120) return Math.round(s) + ' s ago';
  if (s < 7200) return Math.round(s / 60) + ' min ago';
  return Math.round(s / 3600) + ' h ago';
}
function api(path, body) {
  var o = body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : {};
  return fetch(path, o).then(function (r) {
    return r.json().then(function (d) {
      if (!r.ok || !d.success) throw new Error(d.error || ('HTTP ' + r.status));
      return d;
    });
  });
}
function note(id, text, bad) { var e = $(id); if (!e) return; e.textContent = text; e.classList.toggle('pn-err', !!bad); }
function flash(btn, text, bad) {
  if (!btn) return;
  if (!btn.dataset.t) btn.dataset.t = btn.innerHTML;
  btn.textContent = text; btn.disabled = true;
  setTimeout(function () { btn.innerHTML = btn.dataset.t; btn.disabled = false; }, bad ? 2600 : 1200);
}
function seg(id, fn) {
  var g = $(id); if (!g) return;
  g.addEventListener('click', function (e) { var b = e.target.closest('button[data-v]'); if (b) fn(b.getAttribute('data-v'), b); });
}
function segSet(id, v) {
  var g = $(id); if (!g) return;
  each(g.querySelectorAll('button'), function (b) { var on = b.getAttribute('data-v') === String(v); b.classList.toggle('on', on); b.setAttribute('aria-pressed', on ? 'true' : 'false'); });
}

var NAMES = { clock: 'Clock', world: 'World clock', flights: 'Flight board', trains: 'Rail board', yachts: 'Yacht radar', cards: 'Card', other: 'Page' };
var HINTS = { clock: 'Always on - where the panel falls back to', world: 'Daylight map and the time', flights: 'Arrivals and departures', trains: 'Departures, then arrivals, for one station', yachts: 'AIS vessels in the bay', other: 'Always visited' };
var pageIdx = {}, lastPanel = null;

// ---------------------------------------------------------------- polling
var POLL = { pnow: [pollNow, 2000], pflights: [pollFb, 5000], ptrains: [pollRb, 5000], pworld: [pollWc, 30000], pyachts: [pollYr, 3000], plua: [pollEffects, 3000], pknob: [pollKnob, 250] };
var active = null, timer = null;
function activePage() { var s = document.querySelector('section.page.active'); return s && POLL[s.dataset.page] ? s.dataset.page : null; }
function tick() {
  clearTimeout(timer);
  if (!active || document.hidden) return;
  var key = active;
  POLL[key][0]().catch(function () {}).then(function () { if (active === key) timer = setTimeout(tick, POLL[key][1]); });
}
function schedule() {
  var a = activePage();
  document.documentElement.classList.toggle('pn-live', !!a);
  if (a === active) return;
  active = a; rbEditing = false; tick();
}
var mo = new MutationObserver(schedule);
each(document.querySelectorAll('section.page'), function (s) { mo.observe(s, { attributes: true, attributeFilter: ['class'] }); });
document.addEventListener('visibilitychange', tick);

document.addEventListener('click', function (e) {
  var b = e.target.closest('[data-show]');
  if (!b) return;
  var key = b.getAttribute('data-show');
  var go = function () { if (pageIdx[key] == null) throw new Error('Not in this build'); return api('/api/panel', { show: { page: pageIdx[key] } }); };
  (pageIdx[key] != null ? go() : api('/api/panel').then(function (d) { learn(d); return go(); }))
    .then(function (d) { learn(d); flash(b, 'On screen'); tick(); })
    .catch(function (err) { flash(b, err.message, true); });
});

// ---------------------------------------------------------------- now showing
function label(n) {
  if (!n) return '--';
  if (n.off) return 'panel off';
  if (n.key === 'clock') return n.mode === 'clock' ? 'clock · ' + String(n.styleName).toLowerCase() : n.mode;
  if (n.key === 'cards') return 'card · ' + (n.card || '');
  return (NAMES[n.key] || n.name || '').toLowerCase();
}
function learn(d) {
  if (!d || !d.pages) return;
  lastPanel = d;
  d.pages.forEach(function (p) { if (p.key !== 'cards') pageIdx[p.key] = p.i; });
  var dl = document.querySelector('#statusReadout .sr-rows');
  if (dl && !$('srScreen')) {
    var row = document.createElement('div');
    row.className = 'sr-row';
    row.innerHTML = '<dt>screen</dt><dd id="srScreen">--</dd>';
    dl.insertBefore(row, dl.firstChild);
  }
  setText('srScreen', label(d.now));
}
function pollNow() { return api('/api/panel').then(function (d) { learn(d); renderNow(d); }); }

var pagesSig = '', stylesSig = '';
function renderNow(d) {
  var n = d.now, c = d.carousel, count = d.pages.length;
  setText('pnMode', n.off ? 'panel off' : (n.notify ? 'notification' : 'live'));
  setText('pnTitle', label(n));
  setText('pnClock', n.time);
  var state;
  if (!F.carousel) state = 'no carousel in this build';
  else if (!c.enabled) state = 'carousel off';
  else if (c.running) state = 'carousel running';
  else state = 'held · resumes in ' + c.holdS + ' s';
  setText('pnState', state);
  var led = $('pnLed'); if (led) { led.classList.toggle('online', !!c.running); led.classList.toggle('offline', !c.running); }
  setText('pnPage', (NAMES[n.key] || n.name) + (n.card ? ' · ' + n.card : '') + ' · ' + (n.page + 1) + ' of ' + count);
  setText('pnStyle', String(n.styleName).toLowerCase() + (n.mode !== 'clock' ? ' · ' + n.mode + ' over it' : ''));
  setText('pnNext', c.nextS != null ? 'in ' + c.nextS + ' s' : (c.enabled ? 'stays' : 'when you turn the knob'));
  setText('pnKnob', n.entered ? 'inside the page' : 'browsing pages');
  setText('pnHz', n.hz + ' Hz');
  drawNow(d);

  var sig = JSON.stringify([n.page, d.cardsOn, d.pages]);
  if (sig !== pagesSig) { pagesSig = sig; renderPages(d); }
  sig = JSON.stringify([n.style, n.key, d.styles.length]);
  if (sig !== stylesSig) { stylesSig = sig; renderStyles(d); }
  renderCarousel(c);
}
function renderPages(d) {
  var host = $('pnPages'), cards = $('pnCards');
  if (host) host.innerHTML = '';
  if (cards) cards.innerHTML = '';
  d.pages.forEach(function (p) {
    var isCard = p.key === 'cards', here = p.i === d.now.page, locked = p.key === 'clock' || p.key === 'other';
    var row = document.createElement('div');
    row.className = 'pn-row' + (here ? ' here' : '');
    if (isCard) {
      row.innerHTML = '<div class="pn-name"><strong>' + esc(p.title || p.card) + '</strong><span class="ct-hint">' + esc(p.card) + '</span></div>';
    } else {
      row.innerHTML = '<label class="check-row standalone"><input type="checkbox"' + (p.on ? ' checked' : '') + (locked ? ' disabled' : '') +
        '><span class="check-box" aria-hidden="true"></span><span class="check-text"><strong>' + esc(NAMES[p.key] || p.name) +
        '</strong><span class="ct-hint">' + esc(HINTS[p.key] || '') + '</span></span></label>';
      var box = row.querySelector('input');
      box.addEventListener('change', function () {
        api('/api/panel', { enable: { key: p.key, on: box.checked } }).then(function (r) { learn(r); renderNow(r); })
          .catch(function (err) { box.checked = !box.checked; alert(err.message); });
      });
    }
    row.insertAdjacentHTML('beforeend', '<span class="pn-here">on screen</span><button type="button" class="btn btn-sm">Show</button>');
    var btn = row.querySelector('button');
    btn.addEventListener('click', function () {
      var show = { page: p.i }; if (isCard) show.card = p.card;
      api('/api/panel', { show: show }).then(function (r) { learn(r); renderNow(r); }).catch(function (err) { flash(btn, err.message, true); });
    });
    (isCard ? cards : host).appendChild(row);
  });
  var co = $('pnCardsOn'); if (co) co.checked = !!d.cardsOn;
}
function renderStyles(d) {
  var host = $('pnStyles'); if (!host) return;
  host.innerHTML = '';
  d.styles.forEach(function (s) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = 'chip' + (d.now.key === 'clock' && s.id === d.now.style ? ' sel' : (s.id === d.now.style ? ' placed' : ''));
    b.textContent = s.name;
    b.addEventListener('click', function () {
      api('/api/panel', { style: s.id }).then(function (r) { learn(r); renderNow(r); }).catch(function (err) { alert(err.message); });
    });
    host.appendChild(b);
  });
}
function secs(v) { return +v === 0 ? 'own' : v + ' s'; }
function renderCarousel(c) {
  var on = $('pcOn'), idle = $('pcIdle'), slot = $('pcSlot'), all = $('pcAll');
  if (!on) return;
  if (!focused(on)) on.checked = !!c.enabled;
  if (!focused(all)) all.checked = !!c.allStyles;
  if (!focused(idle)) { if (c.idleS > +idle.max) idle.max = c.idleS; idle.value = c.idleS; setText('pcIdleV', c.idleS + ' s'); }
  if (!focused(slot)) { if (c.slotS > +slot.max) slot.max = c.slotS; slot.value = c.slotS; setText('pcSlotV', secs(c.slotS)); }
}
function postCarousel(part) {
  api('/api/panel', { carousel: part }).then(function (r) { learn(r); renderNow(r); note('pcMsg', 'Applied. Written to flash a few seconds after the last change.'); })
    .catch(function (err) { note('pcMsg', err.message, true); });
}
if ($('pcOn')) {
  $('pcOn').addEventListener('change', function () { postCarousel({ enabled: this.checked }); });
  $('pcAll').addEventListener('change', function () { postCarousel({ allStyles: this.checked }); });
  $('pcIdle').addEventListener('input', function () { setText('pcIdleV', this.value + ' s'); });
  $('pcIdle').addEventListener('change', function () { postCarousel({ idleS: +this.value }); this.blur(); });
  $('pcSlot').addEventListener('input', function () { setText('pcSlotV', secs(this.value)); });
  $('pcSlot').addEventListener('change', function () { postCarousel({ slotS: +this.value }); this.blur(); });
}
if ($('pnCardsOn')) $('pnCardsOn').addEventListener('change', function () {
  var box = this;
  api('/api/panel', { enable: { key: 'cards', on: box.checked } }).then(function (r) { learn(r); renderNow(r); })
    .catch(function (err) { box.checked = !box.checked; alert(err.message); });
});

// ---------------------------------------------------------------- the sketch
// A layout sketch on a 128x64 grid, four canvas pixels to a panel pixel so the
// text stays crisp. Shapes stand for text the browser does not have.
var SK = 4;
function css(n, dflt) { return getComputedStyle(document.documentElement).getPropertyValue(n).trim() || dflt; }
function ctxOf(cv) {
  if (cv.width !== 128 * SK) { cv.width = 128 * SK; cv.height = 64 * SK; }
  var c = cv.getContext('2d'); c.clearRect(0, 0, cv.width, cv.height); return c;
}
function R(c, x, y, w, h, col) { c.fillStyle = col; c.fillRect(x * SK, y * SK, w * SK, h * SK); }
function T(c, x, y, s, px, col, align) {
  c.fillStyle = col; c.textAlign = align || 'left'; c.textBaseline = 'top';
  c.font = '600 ' + (px * SK) + 'px ui-monospace,Menlo,Consolas,monospace';
  c.fillText(s, x * SK, y * SK);
}
var AMBER = '#ffb400';
function drawNow(d) {
  var cv = $('pnCanvas'); if (!cv) return;
  var c = ctxOf(cv), fg = css('--crt-fg', '#84f3ad'), dim = css('--crt-dim', '#4d8a67'), n = d.now, i, y;
  if (n.key === 'world') {
    if (wcMap) drawWorld(c, wcMap);
    else { T(c, 64, 26, 'WORLD CLOCK', 8, dim, 'center'); if (!wcLoading) loadWorld().catch(function () {}); }
  } else if (n.key === 'flights') {
    T(c, 2, 1, 'FLIGHTS', 5, '#fff'); T(c, 126, 1, n.time, 5, dim, 'right'); R(c, 0, 8, 128, 1, dim);
    for (i = 0; i < 7; i++) { y = 11 + i * 7; R(c, 2, y, 17, 5, fg); R(c, 22, y, 22, 5, fg); R(c, 50, y, 14 + (i * 37) % 30, 5, fg); R(c, 104, y, 22, 5, dim); }
    R(c, 0, 18, 1, 6, AMBER);
  } else if (n.key === 'trains') {
    // railboard.cpp's departures screen: white title and headings, amber rows, the clock at the foot.
    var RBA = '#ff9600';
    T(c, 2, 0, 'Departures', 7, '#fff'); T(c, 88, 2, 'Plat', 5, '#fff', 'right'); T(c, 93, 2, 'Expt', 5, '#fff');
    T(c, 2, 9, 'Time', 5, '#fff'); T(c, 22, 9, 'Destination', 5, '#fff');
    for (i = 0; i < 6; i++) { y = 16 + i * 7; R(c, 2, y, 16, 5, RBA); R(c, 22, y, 26 + (i * 23) % 34, 5, RBA); R(c, 85, y, 3, 5, RBA); R(c, 93, y, i === 2 ? 17 : 24, 5, i === 3 ? '#ff2418' : RBA); }
    T(c, 2, 58, 'Page 1 of 2', 5, RBA); T(c, 126, 57, n.time, 7, RBA, 'right');
  } else if (n.key === 'yachts') {
    c.strokeStyle = dim; c.lineWidth = SK * 0.5;
    [31, 20, 10].forEach(function (r) { c.beginPath(); c.arc(31.5 * SK, 32 * SK, r * SK, 0, 2 * Math.PI); c.stroke(); });
    [[20, 25], [40, 38], [28, 45], [45, 20], [36, 30]].forEach(function (p, k) { R(c, p[0], p[1], 2, 2, k % 2 ? AMBER : fg); });
    T(c, 66, 1, 'YACHTS', 5, '#fff'); R(c, 66, 15, 60, 1, dim);
    for (i = 0; i < 6; i++) R(c, 66, 17 + i * 7, 20 + (i * 13) % 36, 5, i ? fg : '#fff');
  } else if (n.key === 'cards') {
    var p = d.pages[n.page] || {};
    T(c, 64, 6, String(p.title || 'CARD').toUpperCase(), 6, dim, 'center');
    T(c, 64, 26, String(n.card || '').toUpperCase(), 10, fg, 'center');
  } else if (n.key === 'clock') {
    if (n.mode !== 'clock') T(c, 64, 22, String(n.mode).toUpperCase(), 14, dim, 'center');
    else T(c, 64, 12, n.time, 26, fg, 'center');
    R(c, 0, 56, 128, 8, '#000'); R(c, 0, 56, 128, 1, dim); T(c, 64, 57, n.styleName, 6, AMBER, 'center');
  } else {
    T(c, 64, 26, String(n.name || '').toUpperCase(), 10, fg, 'center');
  }
  if (n.entered) R(c, 125, 0, 3, 3, '#ffa000');
  if (n.notify) { R(c, 0, 20, 128, 24, 'rgba(0,0,0,.85)'); T(c, 64, 28, 'NOTIFICATION', 8, AMBER, 'center'); }
  if (n.off) { R(c, 0, 0, 128, 64, 'rgba(0,0,0,.75)'); T(c, 64, 27, 'PANEL OFF', 10, dim, 'center'); }
}

// ---------------------------------------------------------------- world clock
// The panel's own sun: declination by the cosine approximation, the subsolar
// longitude from UTC hours and minutes, civil twilight blended over 6 degrees -
// worldclock.cpp recompute(), line for line.
var RAD = Math.PI / 180, wcMap = null, wcLoading = false, wcSig = '', wcPulseEnd = 0, wcPick = null, wcSeq = 0, wcTimer = null;
function sunOf(utc) {
  if (!utc) return null;
  var t = new Date(utc * 1000);
  var yday = Math.floor((Date.UTC(t.getUTCFullYear(), t.getUTCMonth(), t.getUTCDate()) - Date.UTC(t.getUTCFullYear(), 0, 1)) / 864e5);
  var decl = -23.44 * RAD * Math.cos(2 * Math.PI * (yday + 10) / 365);
  return { sd: Math.sin(decl), cd: Math.cos(decl), sub: -15 * ((t.getUTCHours() * 60 + t.getUTCMinutes()) / 60 - 12) * RAD };
}
function elev(s, latDeg, lonDeg) {
  var la = latDeg * RAD;
  return Math.asin(Math.sin(la) * s.sd + Math.cos(la) * s.cd * Math.cos(lonDeg * RAD - s.sub)) / RAD;
}
function utcNow(m) { return m.utc ? m.utc + (Date.now() - m.at) / 1000 : 0; }
// Home's HH:MM from the offset the panel reported, carried forward from that
// answer; a summer time change between two polls shows one poll late.
function wcHomeTime(m) {
  if (!m || !m.utc || m.homeOffset == null) return '--:--';
  var t = new Date((Math.floor(utcNow(m)) + m.homeOffset) * 1000);
  return ('0' + t.getUTCHours()).slice(-2) + ':' + ('0' + t.getUTCMinutes()).slice(-2);
}
function wcHome(m) {
  for (var i = 0; i < m.cities.length; i++) if (m.cities[i].id === m.home) return m.cities[i];
  return null;
}
function wcCell(m, lat, lon) { return [Math.floor((lon + 180) / 360 * m.cols), Math.floor((m.top - lat) / (m.top - m.bottom) * m.rows)]; }
function orange(f) { return 'rgb(' + Math.round(255 * f) + ',' + Math.round(140 * f) + ',' + Math.round(40 * f) + ')'; }
function drawWorld(c, m) {
  var s = sunOf(utcNow(m)), dLat = (m.top - m.bottom) / m.rows;
  for (var r = 0; r < m.rows; r++) {
    var h = m.mask[r], hi = parseInt(h.slice(0, 8), 16), lo = parseInt(h.slice(8), 16), lat = m.top - (r + 0.5) * dLat;
    for (var col = 0; col < m.cols; col++) {
      if (!(((col < 32 ? lo >>> col : hi >>> (col - 32)) & 1))) continue;
      var k = 0;
      if (s) k = Math.max(0, Math.min(1, (elev(s, lat, -180 + (col + 0.5) * 360 / m.cols) + 6) / 6));
      R(c, col * 2, r * 2, 1, 1, 'rgb(' + Math.round(52 + 173 * k) + ',' + Math.round(56 + 172 * k) + ',' + Math.round(64 + 168 * k) + ')');
    }
  }
  // Home breathes, and its name with it while a change is new: worldclock.cpp's
  // pulse, 10 s with the last 2 s easing out.
  var now = Date.now(), a = 0.65 + 0.35 * Math.abs(Math.sin(Math.PI * now / 1000)), home = wcHome(m);
  m.cities.forEach(function (ct) {
    var p = wcCell(m, ct.lat, ct.lon);
    if (p[0] < 0 || p[0] >= m.cols || p[1] < 0 || p[1] >= m.rows) return;
    R(c, p[0] * 2, p[1] * 2, 2, 2, orange(ct.id === m.home ? a : 1));
  });
  if (wcPick && Math.floor(now / 400) % 2) {   // where the city picked from the search will go
    var q = wcCell(m, wcPick.lat, wcPick.lon);
    R(c, q[0] * 2 - 1, q[1] * 2 - 1, 4, 4, '#fff');
  }
  var left = wcPulseEnd - now;
  if (home) T(c, 43, 55, home.name, 6, orange(left > 0 ? 1 - (1 - a) * Math.min(1, left / 2000) : 1));
  T(c, 1, 50, wcHomeTime(m), 11, '#fff');
}
// Every answer from /api/worldclock goes through here. A home that changed
// since the last one pulses in the previews, as it does on the panel.
function wcTake(d) {
  var before = wcMap && wcHome(wcMap), after = wcHome(d);
  d.at = Date.now(); wcMap = d; pageIdx.world = d.page;
  if (d.pulseMs) wcPulseEnd = Math.max(wcPulseEnd, d.at + d.pulseMs);
  if (before && after && (before.id !== after.id || before.name !== after.name)) wcPulseEnd = d.at + 10000;
  return d;
}
function loadWorld() {
  wcLoading = true;
  return api('/api/worldclock').then(function (d) { wcLoading = false; return wcTake(d); },
    function (e) { wcLoading = false; throw e; });
}
function pollWc() {
  return Promise.all([loadWorld(), api('/api/panel')]).then(function (r) { learn(r[1]); renderWc(); });
}
function drawWc() { var cv = $('wcCanvas'); if (cv && wcMap) drawWorld(ctxOf(cv), wcMap); }
var WC_FROM = {
  chosen: 'chosen here, and kept across reboots.',
  location: 'the city at the weather location in Settings, as none has been chosen. A city made there is called Home.',
  ip: "found from the panel's internet address, as none has been chosen and no weather location is set.",
  zone: "matched to the panel's time zone for now, until its location is known."
};
function wcPost(body, done) {
  return api('/api/worldclock', body).then(function (d) { wcTake(d); wcSig = ''; renderWc(); note('wcMsg', done); })
    .catch(function (err) { note('wcMsg', err.message, true); wcSig = ''; renderWc(); });
}
function renderWc() {
  var m = wcMap; if (!m) return;
  drawWc();
  var home = wcHome(m), custom = m.cities.filter(function (ct) { return ct.kind === 'custom'; }).length;
  setText('wcMeta', m.utc ? wcHomeTime(m) + ' in ' + (home ? cap(home.name) : 'home') : 'no time yet - all night');
  setText('wcFrom', home ? cap(home.name) + ': ' + (WC_FROM[m.homeSource] || '') : '--');
  setText('wcCount', custom + ' of ' + m.limits.custom);
  var follow = $('wcFollow'); if (follow) follow.hidden = !m.homeChosen;
  wcCheckName();
  var s = sunOf(utcNow(m));
  var sig = JSON.stringify([m.home, m.homeChosen, m.cities, m.cities.map(function (ct) { return s ? Math.round(elev(s, ct.lat, ct.lon)) : 0; })]);
  if (sig === wcSig) return;
  wcSig = sig;
  var host = $('wcCities'); if (!host) return;
  host.innerHTML = '';
  m.cities.forEach(function (ct) {
    var el = s ? elev(s, ct.lat, ct.lon) : null;
    var sun = el == null ? '' : (el > 0 ? 'day' : (el > -6 ? 'twilight' : 'night'));
    var kind = ct.kind === 'custom' ? 'added' : (ct.kind === 'auto' ? 'made at the panel; kept once chosen' : 'built in');
    var row = document.createElement('div');
    row.className = 'pn-row';
    row.innerHTML = '<label class="check-row standalone"><input type="radio" name="wcHome"' + (ct.id === m.home ? ' checked' : '') +
      '><span class="check-box" aria-hidden="true"></span><span class="check-text"><strong>' + esc(cap(ct.name)) + '</strong><span class="ct-hint">' +
      Math.abs(ct.lat).toFixed(2) + (ct.lat >= 0 ? ' N, ' : ' S, ') + Math.abs(ct.lon).toFixed(2) + (ct.lon >= 0 ? ' E' : ' W') +
      (ct.tz ? ' · ' + esc(ct.tz) : '') + ' · ' + kind + (ct.id === m.home ? ' · home' : '') + '</span></span></label>' +
      (sun ? '<span class="pn-sun' + (sun === 'day' ? ' day' : '') + '">' + sun + '</span>' : '') +
      (ct.kind === 'custom' ? '<button type="button" class="btn btn-sm btn-danger">Delete</button>' : '');
    row.querySelector('input').addEventListener('change', function () {
      wcPost({ home: ct.id }, cap(ct.name) + ' is home: on the panel now, and kept across reboots.');
    });
    var del = row.querySelector('button');
    if (del) del.addEventListener('click', function () {
      if (confirm('Delete ' + cap(ct.name) + ' from the map?')) wcPost({ remove: ct.id }, cap(ct.name) + ' deleted.');
    });
    host.appendChild(row);
  });
}

// ---- adding a city. This browser asks Open-Meteo's geocoder itself: its
// answers carry access-control-allow-origin: * and a plain GET needs no
// preflight, so no proxy is needed, and the panel is spared a TLS handshake -
// its largest allocation - for every pause in typing. The panel sees only the
// city that is added, and checks all of it again.
var WC_FIND_HINT = 'Type two letters or more, in any language. This browser asks Open-Meteo; the panel hears only the city you add.';
// Letters that do not come apart under NFD, as worldClockFitName spells them.
var WC_TWO = { 'Æ': 'AE', 'æ': 'AE', 'Þ': 'TH', 'þ': 'TH', 'ß': 'SS', 'Ĳ': 'IJ', 'ĳ': 'IJ',
  'Œ': 'OE', 'œ': 'OE', 'Ø': 'O', 'ø': 'O', 'Ð': 'D', 'ð': 'D', 'Đ': 'D', 'đ': 'D',
  'Ł': 'L', 'ł': 'L', 'Ŀ': 'L', 'ŀ': 'L', 'Ħ': 'H', 'ħ': 'H', 'ı': 'I', 'Ŧ': 'T',
  'ŧ': 'T', 'ĸ': 'K', 'ſ': 'S', 'Ŋ': 'N', 'ŋ': 'N' };
function wcAdv(ch) { var a = wcMap.limits.advance, i = ch.charCodeAt(0) - 32; return a[i >= 0 && i < a.length ? i : 0]; }
function wcWidth(s) { var w = 0; for (var i = 0; i < s.length; i++) w += wcAdv(s.charAt(i)); return w; }
// The name field's first guess, by worldClockFitName's rules: capitals without
// accents, cut after a word or before a hyphen when it is too wide.
function wcFit(s) {
  var lim = wcMap.limits, w = 0, cut = 0, whole = 0;
  var t = String(s || '').replace(/[ÆæÞþßĲĳŒœØøÐðĐđŁłĿŀĦħıŦŧĸſŊŋ]/g,
    function (ch) { return WC_TWO[ch]; }).normalize('NFD').replace(/[̀-ͯ]/g, '').toUpperCase()
    .replace(/[^A-Z0-9.' -]+/g, ' ').replace(/ +/g, ' ').trim();
  for (var i = 0; i < t.length && i < lim.name; i++) {
    w += wcAdv(t.charAt(i));
    if (w > lim.namePx) break;
    cut = i + 1;
    if (i + 1 === t.length || t.charAt(i + 1) === ' ' || t.charAt(i + 1) === '-') whole = i + 1;
  }
  if (cut < t.length && whole) cut = whole;
  return t.slice(0, cut).replace(/[ -]+$/, '');
}
// The panel's own checks, run first here so the buttons can say no in advance.
function wcCheckName() {
  var n = $('wcName'); if (!n || !wcMap || !wcPick) return;
  var lim = wcMap.limits, v = n.value, w = wcWidth(v), why = '';
  var full = wcMap.cities.filter(function (ct) { return ct.kind === 'custom'; }).length >= lim.custom;
  if (!v) why = 'Give it a name.';
  else if (/[^A-Z0-9.' -]/.test(v)) why = "Capitals A-Z, digits, space and . - ' only: the panel's font has nothing else.";
  else if (/^ | $|  /.test(v)) why = 'No space at either end, and no two in a row.';
  else if (w > lim.namePx) why = 'Too wide for the panel: ' + w + ' of ' + lim.namePx + ' px.';
  else if (wcMap.cities.some(function (ct) { return ct.kind !== 'auto' && ct.name === v; })) why = 'A city with that name is already on the map.';
  else if (full) why = 'All ' + lim.custom + ' of your cities are in use: delete one first.';
  note('wcNameMsg', why || (wcPick.label + ' · ' + wcPick.tz + ' · ' + w + ' of ' + lim.namePx + ' px'), !!why);
  each([$('wcAdd'), $('wcAddHome')], function (b) { if (b) b.disabled = !!why; });
}
function wcFind(q) {
  var seq = ++wcSeq, list = $('wcResults');
  if (!list) return;
  if (q.length < 2) { list.innerHTML = ''; note('wcFindMsg', WC_FIND_HINT); return; }
  if (!wcMap) { note('wcFindMsg', 'Waiting for the panel first...'); return; }
  note('wcFindMsg', 'Searching...');
  fetch('https://geocoding-api.open-meteo.com/v1/search?count=8&language=en&format=json&name=' + encodeURIComponent(q))
    .then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(function (d) {
      if (seq !== wcSeq) return;                // a newer search has already gone out
      var found = (d.results || []).filter(function (g) { return g.timezone && typeof g.latitude === 'number' && typeof g.longitude === 'number'; });
      list.innerHTML = '';
      note('wcFindMsg', found.length ? 'Pick one to add it.' : 'Nothing found. Try the English spelling, or a bigger place nearby.');
      found.forEach(function (g) {
        var off = g.latitude < wcMap.bottom || g.latitude > wcMap.top;
        var row = document.createElement('div');
        row.className = 'pn-row';
        row.innerHTML = '<div class="pn-name"><strong>' + esc(g.name) + '</strong><span class="ct-hint">' +
          esc([g.admin1, g.country].filter(Boolean).join(', ')) + ' · ' + esc(g.timezone) + (off ? ' · off the map' : '') +
          '</span></div><button type="button" class="btn btn-sm"' + (off ? ' disabled' : '') + '>Pick</button>';
        row.querySelector('button').addEventListener('click', function () { wcChoose(g); });
        list.appendChild(row);
      });
    })
    .catch(function (err) {
      if (seq === wcSeq) note('wcFindMsg', 'Search failed (' + err.message + '). It runs in this browser, so this browser needs the internet.', true);
    });
}
function wcChoose(g) {
  wcPick = { lat: g.latitude, lon: g.longitude, tz: g.timezone, label: g.name + (g.country ? ', ' + g.country : '') };
  var form = $('wcAddForm'), n = $('wcName');
  if (form) form.hidden = false;
  if (n) { n.value = wcFit(g.name); n.focus(); }
  wcCheckName();
}
function wcAdd(home) {
  var n = $('wcName'); if (!wcPick || !n) return;
  var name = n.value;
  api('/api/worldclock', { add: { name: name, lat: wcPick.lat, lon: wcPick.lon, tz: wcPick.tz, home: home } })
    .then(function (d) {
      wcTake(d); wcPick = null; wcSig = '';
      var form = $('wcAddForm'), q = $('wcFind'), list = $('wcResults');
      if (form) form.hidden = true;
      if (q) q.value = '';
      if (list) list.innerHTML = '';
      note('wcFindMsg', cap(name) + (home ? ' added and made home: it is on the panel now.' : ' added.'));
      renderWc();
    })
    .catch(function (err) { note('wcNameMsg', err.message, true); });
}
if ($('wcFind')) {
  $('wcFind').addEventListener('input', function () {
    var q = this.value.trim();
    clearTimeout(wcTimer);
    wcTimer = setTimeout(function () { wcFind(q); }, 350);   // one request per pause in typing, not one per key
  });
  $('wcName').addEventListener('input', function () {
    var at = this.selectionStart, up = this.value.toUpperCase();
    if (up !== this.value) { this.value = up; this.setSelectionRange(at, at); }
    wcCheckName();
  });
  $('wcAdd').addEventListener('click', function () { wcAdd(false); });
  $('wcAddHome').addEventListener('click', function () { wcAdd(true); });
  $('wcFollow').addEventListener('click', function () { wcPost({ auto: true }, "Home follows the panel's location again."); });
}
setInterval(function () {   // the home dot breathes, in both previews
  if (document.hidden || !wcMap) return;
  if (active === 'pworld') drawWc();
  else if (active === 'pnow' && lastPanel && lastPanel.now.key === 'world') drawNow(lastPanel);
}, 100);

// ---------------------------------------------------------------- flight board
// The airport search runs in this browser against mwgg/Airports (MIT), pinned
// to one commit on jsDelivr: checked 2026-09-14, access-control-allow-origin: *,
// 1.17 MB brotli, cached as immutable. The panel hears only the airport added,
// and checks all of it again.
var FB_DB_URL = 'https://cdn.jsdelivr.net/gh/mwgg/Airports@2473bd8f135c10c3c0edc8af58f9aad742541575/airports.json';
var FB_FIND_HINT = 'A code (JFK, RJTT) or two letters or more of a name or city. The first search loads the list into this browser.';
var FB_TRK_HINT = 'An airline code and a flight number: AFR7301 or AF7301, BAW336 or BA336. FlightAware recommends the ICAO form, three letters; the IATA form can match another airline.';
var FB_ST = { wait: 'waiting for the first answer', notfound: 'no flight by that ident from a day ago to two days ahead',
  sched: 'scheduled', delayed: 'delayed', taxi: 'left the gate', enroute: 'in the air', landed: 'landed',
  cancelled: 'cancelled', diverted: 'diverted' };
var fbLast = null, fbDb = null, fbDbWait = null, fbPick = null, fbSeq = 0, fbTimer = null;
var fbSigCustom = '', fbSigTracks = '', fbBudgetDirty = false;

function fbDirect(d) { return !!(d && d.direct && d.direct.built); }
function fbDur(s) {
  if (s == null || s < 0) return '--';
  if (s < 90) return Math.round(s) + ' s';
  if (s < 5400) return Math.round(s / 60) + ' min';
  var h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
  return h + ' h' + (m ? ' ' + m + ' min' : '');
}
function fbTime(t) { if (!t) return '--:--'; var d = new Date(t * 1000); return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2); }
function fbFold(s) { return String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toUpperCase(); }
function fbPost(body, done, msgId) {
  return api('/api/flightboard', body).then(function (d) { fbSigCustom = fbSigTracks = ''; renderFb(d); note(msgId, done); return d; })
    .catch(function (err) { note(msgId, err.message, true); });
}
function pollFb() { return api('/api/flightboard').then(renderFb); }

function renderFb(d) {
  fbLast = d; pageIdx.flights = d.page;
  var sel = $('fbApt'), sig = JSON.stringify(d.airports);
  if (sel && sel.getAttribute('data-sig') !== sig) {
    sel.innerHTML = '';
    d.airports.forEach(function (a) {
      var o = document.createElement('option'); o.value = a.id;
      o.textContent = cap(a.name) + ' · ' + a.code + (a.kind === 'custom' ? ' · yours' : '');
      sel.appendChild(o);
    });
    sel.setAttribute('data-sig', sig);
  }
  if (sel && !focused(sel)) sel.value = d.airport;
  segSet('fbDir', d.dir);
  setText('fbTag', d.showing ? 'on screen' : 'not on screen');

  var b = d.board || {}, rows = $('fbRows'), trk = d.tracked || [], direct = fbDirect(d);
  var html = '<span class="h">time</span><span class="h">flight</span><span class="h">to / from</span><span class="h r">status</span>';
  // The pinned rows, on the panel's own dark blue band.
  var PIN = ' style="background:#001a46;color:#ebf0f5;text-shadow:none"';
  trk.forEach(function (t) {
    html += '<span' + PIN + '>' + esc(t.tm) + '</span><span' + PIN + '>' + esc(t.fn || t.ident) + '</span><span' + PIN + '>' +
      esc(t.from ? t.from + '-' + t.to : '') + '</span><span class="r"' + PIN + '>' + esc(t.w) + '</span>';
  });
  if (b.have) {
    setText('fbHead', cap(b.name) + ' ' + (b.dir === 'dep' ? 'departures' : 'arrivals') + (d.dir === 'alt' ? ' · swaps every ' + d.altS + ' s' : ''));
    setText('fbAge', (b.source === 'aeroapi' ? 'data ' : 'received ') + ago(b.age));
    (b.rows || []).forEach(function (r, i) {
      var cls = 'st-' + (r.st || 'sched');
      html += '<span class="' + cls + (i === b.now ? ' now' : '') + '">' + esc(r.tm) + '</span><span class="' + cls + '">' + esc(r.fn) +
        '</span><span class="' + cls + '">' + esc(r.ct) + ' ' + esc(cap(r.cy !== r.ct ? r.cy : '')) + '</span><span class="r ' + cls + '">' + esc(r.w) + '</span>';
    });
    if (!(b.rows || []).length) html += '<span class="empty">No flights within two hours of now.</span>';
  } else {
    setText('fbHead', 'no board yet'); setText('fbAge', '--');
    var why = direct && d.direct.key ? d.direct.state : b.source === 'none' ? 'This airport needs an AeroAPI key.' : (d.mqtt ? d.mqtt.status : 'No data');
    html += '<span class="empty">' + esc(why) + '</span>';
  }
  if (rows) rows.innerHTML = html;

  var feed = $('fbFeed'), kv = [], sides = b.sides || {};
  kv.push(['source', b.source === 'aeroapi' ? 'FlightAware AeroAPI, fetched by the panel' : b.source === 'mqtt' ? 'Home Assistant over MQTT' : 'none: only the six built-in airports come from Home Assistant', b.source === 'none' ? 'pn-warn' : '']);
  ['arr', 'dep'].forEach(function (k) {
    var s = sides[k] || {}, wanted = d.dir === 'alt' || d.dir === k;
    kv.push([k === 'arr' ? 'arrivals' : 'departures', s.have ? s.n + ' flights, data from ' + s.upd : (wanted ? 'waiting' : 'not shown'), s.have || !wanted ? '' : 'pn-warn']);
  });
  if (d.mqtt && b.source === 'mqtt') {
    kv.push(['broker', d.mqtt.configured ? 'configured' : 'not configured', d.mqtt.configured ? '' : 'pn-warn']);
    kv.push(['mqtt', d.mqtt.connected ? 'connected' : 'not connected', d.mqtt.connected ? 'pn-ok' : 'pn-warn']);
    kv.push(['state', String(d.mqtt.status).toLowerCase()]);
  }
  if (feed) feed.innerHTML = kv.map(function (x) { return '<dt>' + esc(x[0]) + '</dt><dd class="' + (x[2] || '') + '">' + esc(x[1]) + '</dd>'; }).join('');

  renderFbTracks(d);
  renderFbCustom(d);
  renderFbBudget(d);
}

// ---- tracked flights
function fbIdentWhy(v) {
  if (!v) return '';
  if (!/^[A-Z0-9]+$/.test(v)) return 'Letters and digits only.';
  if (!/^(?:[A-Z]{3}|[A-Z0-9]{2})[0-9]{1,4}[A-Z]?$/.test(v) || /^[0-9]{2}/.test(v) && !/^[A-Z]{3}/.test(v)) return 'An airline code, then 1 to 4 digits: AFR7301, BA336.';
  return '';
}
function fbCheckIdent() {
  var n = $('fbIdent'), btn = $('fbTrackAdd'); if (!n || !fbLast) return;
  var v = n.value.replace(/\s+/g, ''), why = fbIdentWhy(v), trk = fbLast.tracked || [];
  if (!why && trk.some(function (t) { return t.ident === v; })) why = 'That flight is already tracked.';
  if (!why && trk.length >= fbLast.limits.track) why = 'Three flights are tracked already: remove one first.';
  if (!why && !fbLast.direct.key) why = 'No AeroAPI key is stored on the panel yet.';
  note('fbTrkMsg', why || FB_TRK_HINT, !!why);
  if (btn) btn.disabled = !v || !!why;
}
function renderFbTracks(d) {
  var card = $('fbTrackCard'); if (!card) return;
  card.hidden = !fbDirect(d);
  if (card.hidden) return;
  var trk = d.tracked || [];
  setText('fbTrkCount', trk.length + ' of ' + d.limits.track);
  fbCheckIdent();
  var sig = JSON.stringify(trk);
  if (sig === fbSigTracks) return;
  fbSigTracks = sig;
  var host = $('fbTracks'); if (!host) return;
  host.innerHTML = trk.length ? '' : '<p class="field-hint">No flight is tracked.</p>';
  trk.forEach(function (t) {
    var what = [], when = [];
    if (t.from) what.push(t.from + ' to ' + t.to);
    var dep = t.dep || {}, arr = t.arr || {};
    if (dep.act || dep.est || dep.sched) what.push((dep.act ? 'left the gate ' : 'departs ') + fbTime(dep.act || dep.est || dep.sched) +
      (!dep.act && dep.est && dep.sched && dep.est !== dep.sched ? ' (scheduled ' + fbTime(dep.sched) + ')' : ''));
    if (arr.act || arr.est || arr.sched) what.push((arr.act ? 'landed ' : 'arrives ') + fbTime(arr.act || arr.est || arr.sched));
    if (t.gate) what.push('gate ' + t.gate);
    if (t.age != null) when.push('asked ' + fbDur(t.age) + ' ago');
    when.push(t.nextIn < 0 ? 'no more calls' : t.nextIn === 0 ? 'due' : 'next in ' + fbDur(t.nextIn));
    if (t.expiresIn != null) when.push('removes itself in ' + fbDur(t.expiresIn));
    if (t.http && t.http !== 200) when.push('last answer HTTP ' + t.http);
    if (t.current === false) when.push('nothing current under this ident');
    var row = document.createElement('div');
    row.className = 'pn-row';
    row.innerHTML = '<div class="pn-name"><strong>' + esc(t.ident) + '</strong> ' + esc(t.w) + ' · ' + esc(FB_ST[t.state] || t.state) +
      '<span class="ct-hint">' + esc(what.join(' · ') || '--') + '</span><span class="ct-hint">' + esc(when.join(' · ')) +
      '</span></div><button type="button" class="btn btn-sm">Remove</button>';
    row.querySelector('button').addEventListener('click', function () {
      fbPost({ untrack: t.ident }, t.ident + ' is no longer tracked.', 'fbTrkMsg');
    });
    host.appendChild(row);
  });
}
function fbTrack() {
  var n = $('fbIdent'); if (!n) return;
  var v = n.value.replace(/\s+/g, '');
  fbPost({ track: v }, v + ' is tracked: the panel asks for it within a few seconds.', 'fbTrkMsg')
    .then(function (d) { if (d) { n.value = ''; fbCheckIdent(); } });
}

// ---- custom airports
function renderFbCustom(d) {
  var card = $('fbAddCard'); if (!card) return;
  card.hidden = !fbDirect(d);
  if (card.hidden) return;
  var custom = d.airports.filter(function (a) { return a.kind === 'custom'; });
  setText('fbCount', custom.length + ' of ' + d.limits.custom);
  fbCheckName();
  var sig = JSON.stringify([custom, d.airport]);
  if (sig === fbSigCustom) return;
  fbSigCustom = sig;
  var host = $('fbCustom'); if (!host) return;
  host.innerHTML = custom.length ? '' : '<p class="field-hint">None yet: find one below.</p>';
  custom.forEach(function (a) {
    var row = document.createElement('div');
    row.className = 'pn-row' + (a.id === d.airport ? ' here' : '');
    row.innerHTML = '<div class="pn-name"><strong>' + esc(cap(a.name)) + '</strong><span class="ct-hint">' +
      esc([a.code, a.iata, a.tz].filter(Boolean).join(' · ')) + '</span></div><span class="pn-here">on the board</span>' +
      '<button type="button" class="btn btn-sm" data-a="pick">Choose</button><button type="button" class="btn btn-sm" data-a="del">Delete</button>';
    row.querySelector('[data-a="pick"]').addEventListener('click', function () {
      fbPost({ airport: a.id }, cap(a.name) + ' is on the board.', 'fbMsg');
    });
    row.querySelector('[data-a="del"]').addEventListener('click', function () {
      if (confirm('Delete ' + cap(a.name) + ' from your airports?')) fbPost({ remove: a.id }, cap(a.name) + ' deleted.', 'fbFindMsg');
    });
    host.appendChild(row);
  });
}
function fbLoadDb() {
  if (fbDb) return Promise.resolve(fbDb);
  if (!fbDbWait) {
    fbDbWait = fetch(FB_DB_URL).then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
      .then(function (o) {
        var list = [];
        Object.keys(o).forEach(function (k) {
          var a = o[k] || {};
          if (!/^[A-Z][A-Z0-9]{3}$/.test(a.icao || '') || !/^[A-Za-z]+\/[A-Za-z0-9_+\-\/]+$/.test(a.tz || '') || a.tz.length > 39) return;
          list.push({ icao: a.icao, iata: /^[A-Z]{3}$/.test(a.iata || '') ? a.iata : '', name: a.name || '', city: a.city || '',
            country: a.country || '', tz: a.tz, key: fbFold((a.name || '') + ' ' + (a.city || '')) });
        });
        fbDb = list;
        return list;
      })
      .catch(function (e) { fbDbWait = null; throw e; });
  }
  return fbDbWait;
}
function fbFind(q) {
  var seq = ++fbSeq, list = $('fbResults');
  if (!list) return;
  if (q.length < 2) { list.innerHTML = ''; note('fbFindMsg', FB_FIND_HINT); return; }
  note('fbFindMsg', fbDb ? 'Searching...' : 'Loading the airport list, about 1.2 MB, once...');
  fbLoadDb().then(function (db) {
    if (seq !== fbSeq || !fbLast) return;
    var Q = fbFold(q.trim()), exact = [], withIata = [], rest = [];
    db.forEach(function (a) {
      if (a.iata === Q || a.icao === Q) exact.push(a);
      else if (a.key.indexOf(Q) >= 0) (a.iata ? withIata : rest).push(a);
    });
    var found = exact.concat(withIata, rest).slice(0, 8);
    list.innerHTML = '';
    note('fbFindMsg', found.length ? 'Pick one to add it.' : 'Nothing found. Try the code, or the English spelling.');
    found.forEach(function (a) {
      var listed = fbLast.airports.some(function (x) { return x.code === a.icao; });
      var row = document.createElement('div');
      row.className = 'pn-row';
      row.innerHTML = '<div class="pn-name"><strong>' + esc(a.name) + '</strong><span class="ct-hint">' +
        esc([a.icao, a.iata, [a.city, a.country].filter(Boolean).join(', '), a.tz].filter(Boolean).join(' · ')) +
        (listed ? ' · already listed' : '') + '</span></div><button type="button" class="btn btn-sm"' + (listed ? ' disabled' : '') + '>Pick</button>';
      row.querySelector('button').addEventListener('click', function () { fbChoose(a); });
      list.appendChild(row);
    });
  }).catch(function (err) {
    if (seq === fbSeq) note('fbFindMsg', 'Could not load the airport list (' + err.message + '). It is fetched by this browser, which needs the internet.', true);
  });
}
function fbAdv(ch) { var a = fbLast.limits.advance, i = ch.charCodeAt(0) - 32; return a[i >= 0 && i < a.length ? i : 0]; }
function fbWidth(s) { var w = 0; for (var i = 0; i < s.length; i++) w += fbAdv(s.charAt(i)); return w; }
// A first guess at the header name, by the panel's rules: capitals without
// accents, cut after a word when too wide.
function fbFit(s) {
  var lim = fbLast.limits, w = 0, cut = 0, whole = 0;
  var t = fbFold(s).replace(/[^A-Z0-9.' -]+/g, ' ').replace(/ +/g, ' ').trim();
  for (var i = 0; i < t.length && i < lim.name; i++) {
    w += fbAdv(t.charAt(i));
    if (w > lim.namePx) break;
    cut = i + 1;
    if (i + 1 === t.length || t.charAt(i + 1) === ' ' || t.charAt(i + 1) === '-') whole = i + 1;
  }
  if (cut < t.length && whole) cut = whole;
  return t.slice(0, cut).replace(/[ -]+$/, '');
}
function fbCheckName() {
  var n = $('fbName'); if (!n || !fbLast || !fbPick) return;
  var lim = fbLast.limits, v = n.value, w = fbWidth(v), why = '';
  var full = fbLast.airports.filter(function (a) { return a.kind === 'custom'; }).length >= lim.custom;
  if (!v) why = 'Give it a name.';
  else if (/[^A-Z0-9.' -]/.test(v)) why = "Capitals A-Z, digits, space and . - ' only: the panel's font has nothing else.";
  else if (/^ | $|  /.test(v)) why = 'No space at either end, and no two in a row.';
  else if (w > lim.namePx) why = 'Too wide for the header: ' + w + ' of ' + lim.namePx + ' px.';
  else if (fbLast.airports.some(function (a) { return a.code === fbPick.icao; })) why = 'That airport is already in the list.';
  else if (full) why = 'All ' + lim.custom + ' of your airports are in use: delete one first.';
  note('fbNameMsg', why || ([fbPick.icao, fbPick.iata, fbPick.tz].filter(Boolean).join(' · ') + ' · ' + w + ' of ' + lim.namePx + ' px'), !!why);
  each([$('fbAdd'), $('fbAddSel')], function (b) { if (b) b.disabled = !!why; });
}
function fbChoose(a) {
  fbPick = a;
  var form = $('fbAddForm'), n = $('fbName');
  if (form) form.hidden = false;
  if (n) { n.value = fbFit(a.city || a.name) || fbFit(a.name); n.focus(); }
  fbCheckName();
}
function fbAddAirport(select) {
  var n = $('fbName'); if (!fbPick || !n) return;
  var name = n.value;
  fbPost({ add: { icao: fbPick.icao, iata: fbPick.iata, name: name, tz: fbPick.tz, select: select } },
    cap(name) + (select ? ' added and on the board.' : ' added.'), 'fbFindMsg')
    .then(function (d) {
      if (!d) return;
      fbPick = null;
      var form = $('fbAddForm'), q = $('fbFind'), list = $('fbResults');
      if (form) form.hidden = true;
      if (q) q.value = '';
      if (list) list.innerHTML = '';
    });
}

// ---- budget
function renderFbBudget(d) {
  var card = $('fbBudgetCard'); if (!card) return;
  card.hidden = !fbDirect(d);
  if (card.hidden) return;
  var x = d.direct, u = x.usage || {}, bu = x.budget || {}, bo = x.bounds || {};
  setText('fbKeyTag', x.key ? 'key stored' : 'no key');
  var money = function (v) { return '$' + (v || 0).toFixed(2); };
  var use = $('fbUse');
  if (use) use.innerHTML = [[u.today + '/' + bu.day_cap, 'calls today'], [u.month + '/' + bu.month_cap, 'this month'],
    [money(u.usdMonth), 'spent, est.'], [money(u.usdMonthCap), 'month at most']]
    .map(function (c) { return '<div><b>' + esc(c[0]) + '</b><span>' + esc(c[1]) + '</span></div>'; }).join('');
  if (!fbBudgetDirty) {
    [['fbDay', 'day_cap'], ['fbMonth', 'month_cap'], ['fbFloor', 'floor_min']].forEach(function (f) {
      var el = $(f[0]); if (!el || focused(el)) return;
      el.value = bu[f[1]];
      if (bo[f[1]]) { el.min = bo[f[1]][0]; el.max = bo[f[1]][1]; }
    });
  }
  var bmsg = $('fbBudgetMsg');
  if (bmsg && !bmsg.classList.contains('pn-err') && !fbBudgetDirty)
    note('fbBudgetMsg', 'At $' + u.usdPerCall + ' a call - FlightAware lists $0.005 a result set for these endpoints on every tier, and each call here asks for one - a day cap of ' +
      bu.day_cap + ' is at most ' + money(bu.day_cap * 0.005 * 31) + ' in a 31-day month, and the month cap stops it at ' + money(u.usdMonthCap) +
      '. Every call made counts, answered or not. Home Assistant\'s own calls on the same account are not counted here. 0 turns the direct fetch off.');
  var kv = [];
  kv.push(['key', x.key ? 'stored on the panel (never shown)' : 'not stored: python3 tools/provision_secrets.py aeroapi-from-ha', x.key ? '' : 'pn-warn']);
  kv.push(['state', String(x.state).toLowerCase() + (x.waitS ? ', next call in ' + fbDur(x.waitS) : ''), /cap|auth|rate|tls|net|http|bad|nomem|refused/i.test(x.state) ? 'pn-warn' : '']);
  if (x.last) kv.push(['last call', x.last.what + ' ' + x.last.for + ' · ' + (x.last.http > 0 ? 'HTTP ' + x.last.http : String(x.last.state).toLowerCase()) +
    ' · ' + Math.round((x.last.bytes || 0) / 1024) + ' KB · ' + fbDur(x.last.ago) + ' ago']);
  (x.lists || []).forEach(function (l) {
    if (!l.have && !l.refusedWaitS) return;
    kv.push([l.name.replace('_', ' '), l.have ? l.kept + ' kept of ' + l.seen + ', ' + fbDur(l.age) + ' old' + (l.more ? ' · more pages not fetched' : '') : 'refused, again in ' + fbDur(l.refusedWaitS), l.more || l.refusedWaitS ? 'pn-warn' : '']);
  });
  if ((d.tracked || []).length) kv.push(['board share', 'the board may use ' + u.boardDayCap + ' of today\'s ' + bu.day_cap + ' calls while flights are tracked']);
  kv.push(['since boot', x.calls + ' calls, ' + x.fails + ' failed; one at a time, ' + u.spacingS + ' s apart']);
  if (u.dayEndsInS != null) kv.push(['day resets', 'in ' + fbDur(u.dayEndsInS) + ' (midnight UTC)']);
  if (x.last && x.last.heapMin) kv.push(['memory', 'internal free ' + Math.round(x.last.heapMin / 1024) + ' KB at the lowest, stack spare ' + x.last.stackFree + ' B']);
  var host = $('fbDirect');
  if (host) host.innerHTML = kv.map(function (r) { return '<dt>' + esc(r[0]) + '</dt><dd class="' + (r[2] || '') + '">' + esc(r[1]) + '</dd>'; }).join('');
}
function fbSaveBudget() {
  var num = function (id) { var el = $(id); return el ? Number(el.value) : NaN; };
  var body = { day_cap: num('fbDay'), month_cap: num('fbMonth'), floor_min: num('fbFloor') };
  var bad = Object.keys(body).filter(function (k) { return !Number.isInteger(body[k]); });
  if (bad.length) { note('fbBudgetMsg', 'Whole numbers only: ' + bad.join(', ') + '.', true); return; }
  fbPost({ budget: body }, 'Budget saved: in force now, kept across reboots.', 'fbBudgetMsg')
    .then(function (d) { if (d) fbBudgetDirty = false; });
}

if ($('fbApt')) $('fbApt').addEventListener('change', function () { fbPost({ airport: +this.value }, 'Applied. Kept across reboots.', 'fbMsg'); });
seg('fbDir', function (v) { fbPost({ dir: v }, 'Applied. Kept across reboots.', 'fbMsg'); });
if ($('fbFind')) {
  note('fbFindMsg', FB_FIND_HINT);
  $('fbFind').addEventListener('input', function () {
    var q = this.value.trim();
    clearTimeout(fbTimer);
    fbTimer = setTimeout(function () { fbFind(q); }, 300);
  });
  $('fbName').addEventListener('input', function () {
    var at = this.selectionStart, up = this.value.toUpperCase();
    if (up !== this.value) { this.value = up; this.setSelectionRange(at, at); }
    fbCheckName();
  });
  $('fbAdd').addEventListener('click', function () { fbAddAirport(false); });
  $('fbAddSel').addEventListener('click', function () { fbAddAirport(true); });
}
if ($('fbIdent')) {
  $('fbIdent').addEventListener('input', function () {
    var at = this.selectionStart, up = this.value.toUpperCase();
    if (up !== this.value) { this.value = up; this.setSelectionRange(at, at); }
    fbCheckIdent();
  });
  $('fbIdent').addEventListener('keydown', function (e) { if (e.key === 'Enter' && !$('fbTrackAdd').disabled) fbTrack(); });
  $('fbTrackAdd').addEventListener('click', fbTrack);
}
each([$('fbDay'), $('fbMonth'), $('fbFloor')], function (el) { if (el) el.addEventListener('input', function () { fbBudgetDirty = true; }); });
if ($('fbBudgetSave')) $('fbBudgetSave').addEventListener('click', fbSaveBudget);

// ---------------------------------------------------------------- rail board
var rbEditing = false, rbCrsDirty = false, rbPresetSig = '';
// Presets: each code exactly as National Rail's own station page heads it, read
// on 2026-09-14 (nationalrail.co.uk/stations/<name>/). Any other code can be typed.
var RB_PRESETS = [['GLD', 'Guildford'], ['WAT', 'London Waterloo'], ['WOK', 'Woking'], ['RDG', 'Reading'], ['GTW', 'Gatwick Airport'], ['LRD', 'London Road (Guildford)']];
function renderPresets(crs) {
  var host = $('rbPresets'); if (!host || rbPresetSig === crs) return;
  rbPresetSig = crs; host.innerHTML = '';
  RB_PRESETS.forEach(function (p) {
    var b = document.createElement('button');
    b.type = 'button'; b.className = 'chip' + (p[0] === crs ? ' sel' : '');
    b.textContent = p[1] + ' · ' + p[0];
    b.addEventListener('click', function () { var ci = $('rbCrs'); if (ci) ci.value = p[0]; setStation(p[0]); });
    host.appendChild(b);
  });
}
// The panel refuses anything but three capitals with 400; checked here first so
// the message can say what is wrong with what was typed.
function setStation(code, btn) {
  if (!/^[A-Z]{3}$/.test(code)) {
    note('rbStnMsg', 'A station code is exactly three letters, A to Z' + (code ? ' - "' + code + '" is not one.' : '.'), true);
    var ci = $('rbCrs'); if (ci) ci.focus();
    return;
  }
  api('/api/railboard', { crs: code }).then(function (d) {
    rbCrsDirty = false; rbPresetSig = ''; renderRb(d);
    note('rbStnMsg', code + ' is the station now, kept across reboots. Home Assistant fetches it within 20 s.');
    if (btn) flash(btn, 'Set');
  }).catch(function (err) { note('rbStnMsg', err.message, true); });
}
if ($('rbCrs')) {
  $('rbCrs').addEventListener('input', function () {
    var up = this.value.toUpperCase();
    if (up !== this.value) this.value = up;
    rbCrsDirty = true;
    var bad = /[^A-Z]/.test(up);
    note('rbStnMsg', bad ? 'Letters only, A to Z.' : '', bad);
  });
  $('rbCrs').addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); setStation(this.value, $('rbSet')); } });
  $('rbSet').addEventListener('click', function () { setStation($('rbCrs').value, this); });
}
function londonTime(ts) {
  try { return new Date(ts * 1000).toLocaleTimeString('en-GB', { timeZone: 'Europe/London' }); } catch (e) { return new Date(ts * 1000).toISOString().slice(11, 19) + ' UTC'; }
}
function pollRb() { return api('/api/railboard').then(renderRb); }
function listLine(l, now) {
  if (!l.have) return 'nothing yet';
  return l.count + ' services · received ' + ago(l.rx) + (l.stale ? ' · stale' : '');
}
function renderRb(d) {
  pageIdx.trains = d.page;
  // RTT's own spelling, mixed case, as the panel prints it; the code until a board has named it.
  var named = d.named ? d.station : '';
  setText('rbStation', named ? named + ' (' + d.crs + ')' : d.crs);
  setText('rbNow', named ? named + ' · ' + d.crs : d.crs + ' · no board has named it yet');
  setText('rbStnTag', d.select.sent ? 'sent to home assistant' : 'not sent yet');
  var ci = $('rbCrs'); if (ci && !focused(ci) && !rbCrsDirty) ci.value = d.crs;
  renderPresets(d.crs);
  var LIST = { dep: 'departures', arr: 'arrivals', diag: 'diagnostics' };
  setText('rbList', (LIST[d.list] || '--') + (d.knob !== 'auto' ? ' · chosen with the knob, ' + d.holdS + ' s left' : (d.list === 'diag' ? ' · pinned' : ' · alternating every ' + d.cfg.switch_s + ' s')));
  setText('rbTag', d.showing ? 'on screen' : 'not on screen');
  var stale = d.dep.stale && d.arr.stale;
  var led = $('rbLed'); if (led) { led.classList.toggle('online', !stale); led.classList.toggle('offline', stale); }
  setText('rbState', d.crs + ' · ' + (stale ? 'data updating' : 'fresh'));
  var last = Math.max(d.dep.ts || 0, d.arr.ts || 0);
  setText('rbUpd', last ? londonTime(last) + (d.synced ? ' · ' + ago(d.now - last) : '') : 'never');
  setText('rbDep', listLine(d.dep)); setText('rbArr', listLine(d.arr));
  var ha = d.ha;
  setText('rbHa', !ha.have ? 'no status yet' : (ha.err ? ha.err + (ha.code ? ' ' + ha.code : '') + (ha.retry ? ' · retry ' + ha.retry + ' s' : '') : 'ok ' + ha.code) + (ha.left ? ' · ' + ha.left + ' left today' : ''));
  // The panel's own fetch. Token presence, kind and expiry only - the route never carries the token.
  var dx = d.direct || {}, SRC = { direct: 'direct from Realtime Trains', ha: 'Home Assistant', none: 'none fresh' };
  setText('rbSrc', (SRC[d.source] || '--') + (d.haShadowed ? ' · ' + d.haShadowed + ' HA boards set aside' : ''));
  var dEl = $('rbDirect'), KIND = { unknown: 'kind not known yet', access: 'access token', 'refresh-exchanged': 'refresh token, exchanged', refused: 'refused by Realtime Trains' };
  if (!dx.built) {
    setText('rbDirect', 'not in this build'); setText('rbTok', '--');
    setText('rbQuota', ha.left ? ha.left + ' left today (Home Assistant)' : '--');
  } else {
    setText('rbTok', !dx.token ? 'not set - Home Assistant only' : 'set · ' + (KIND[dx.kind] || dx.kind) + (dx.validUntil ? ' · access valid until ' + londonTime(dx.validUntil) : ''));
    setText('rbDirect', String(dx.state).toLowerCase() + (dx.http ? ' ' + dx.http : '') + (dx.fetching ? ' · fetching' : '') +
      (dx.fetchedAgo != null ? ' · last fetch ' + ago(dx.fetchedAgo) : '') + (dx.nextIn != null && dx.token ? ' · next in ' + dx.nextIn + ' s' : ''));
    var left = dx.left != null && dx.left >= 0 ? dx.left : (ha.left ? +ha.left : null);
    setText('rbQuota', left == null ? 'not reported yet' : left + ' requests left today' + (dx.limit > 0 ? ' of ' + dx.limit : '') + ' · shared by the panel and Home Assistant' + (dx.interval > 30 ? ' · panel slowed to every ' + dx.interval + ' s' : ''));
  }
  if (dEl) dEl.classList.toggle('warn', !!dx.built && dx.token && dx.state !== 'OK' && dx.state !== 'WAITING');
  var rt = (d.dep.rt || d.arr.rt || '').replace(/^REALTIME_DATA_/, '');
  setText('rbRt', rt ? rt.toLowerCase() : '--');
  setText('rbMq', (d.mqtt.connected ? 'connected' : String(d.mqtt.status).toLowerCase()) + (d.mqtt.configured ? '' : ' · no broker stored') + ' · refused ' + d.refused);
  var dg = $('rbDiag'); if (dg && !focused(dg)) dg.checked = !!d.diag;
  setText('rbFrom', { ha: 'from Home Assistant', web: 'set on this page', build: 'build defaults' }[d.cfg.from] || '--');
  rbLast = d;
  renderRbSettings(d);
}

// ---- board settings: every control posts its own key at once (src/railboard/rb_settings.h)
var rbLast = null, rbSegsBuilt = {};
var RB_EXPT = { ok: 'On time', canc: 'Cancelled', arr: 'Arrived', nr: '', late: '' };
function rbHex(d, name) {
  var cols = (d.cfg.bounds && d.cfg.bounds.colors) || [], i;
  for (i = 0; i < cols.length; i++) if (cols[i].name === name && /^#[0-9a-f]{6}$/i.test(cols[i].hex)) return cols[i].hex;
  return '#ff9600';
}
function rbColourSeg(id, d, value, key) {
  var g = $(id); if (!g) return;
  var cols = (d.cfg.bounds && d.cfg.bounds.colors) || [];
  if (!rbSegsBuilt[id] && cols.length) {
    rbSegsBuilt[id] = true; g.innerHTML = '';
    cols.forEach(function (c) {
      var b = document.createElement('button'), sw = document.createElement('span');
      b.type = 'button'; b.setAttribute('data-v', c.name);
      sw.className = 'pn-sw'; sw.style.background = /^#[0-9a-f]{6}$/i.test(c.hex) ? c.hex : '#888';
      b.appendChild(sw); b.appendChild(document.createTextNode(c.name));
      g.appendChild(b);
    });
    seg(id, function (v) { var o = {}; o[key] = v; postRbCfg(o); });
  }
  segSet(id, value);
}
function rbFillSelect(sel, lo, hi, label) {
  if (!sel || sel.options.length === hi - lo + 1) return;
  sel.innerHTML = '';
  for (var v = lo; v <= hi; v++) { var o = document.createElement('option'); o.value = v; o.textContent = label(v); sel.appendChild(o); }
}
function renderRbSettings(d) {
  var c = d.cfg, b = c.bounds || {};
  rbColourSeg('rbRowCol', d, c.row_color, 'row_color');
  rbColourSeg('rbHeadCol', d, c.head_color, 'head_color');
  rbColourSeg('rbDueCol', d, c.due_color, 'due_color');
  segSet('rbClock', c.clock_seconds ? '1' : '0');
  if (b.due_min) rbFillSelect($('rbDue'), b.due_min[0], b.due_min[1], function (m) { return m ? m + ' min' : 'Off'; });
  if (b.rows) rbFillSelect($('rbRows'), b.rows[0], b.rows[1], function (r) { return r + (r === 6 ? ' - one page' : (r > 6 ? ' - two pages' : '')); });
  var set = function (id, v, range) {
    var el = $(id); if (!el || focused(el)) return;
    if (range) { el.min = range[0]; el.max = range[1]; }
    el.value = v;
  };
  set('rbDue', c.due_min); set('rbRows', c.rows);
  set('rbSwitch', c.switch_s, b.switch_s); set('rbStale', c.stale_s, b.stale_s); set('rbLevel', c.level, b.level);
  if (!focused($('rbLevel'))) setText('rbLevelV', c.level + ' pc');
  var rs = $('rbReset'); if (rs) rs.disabled = c.from !== 'web';
  drawRbPreview(d);
}
function postRbCfg(part) {
  api('/api/railboard', { config: part }).then(function (d) {
    renderRb(d); note('rbMsg', 'Applied on the panel. Kept across reboots - written to flash a few seconds after the last change.');
  }).catch(function (err) { note('rbMsg', err.message, true); if (rbLast) renderRbSettings(rbLast); });
}
function rbShade(hex, level) {
  var n = parseInt(hex.slice(1), 16), k = Math.max(10, Math.min(100, level)) / 100;
  return 'rgb(' + Math.round((n >> 16 & 255) * k) + ',' + Math.round((n >> 8 & 255) * k) + ',' + Math.round((n & 255) * k) + ')';
}
function drawRbPreview(d) {
  var cv = $('rbPreview'); if (!cv) return;
  var c = ctxOf(cv), cfg = d.cfg, lv = cfg.level, arr = d.list === 'arr';
  var row = rbShade(rbHex(d, cfg.row_color), lv), head = rbShade(rbHex(d, cfg.head_color), lv);
  var due = rbShade(rbHex(d, cfg.due_color), lv), red = rbShade('#ff2418', lv), dim = rbShade('#787e84', lv);
  R(c, 0, 0, 128, 64, '#000');
  T(c, 2, 0, arr ? 'Arrivals' : 'Departures', 7, head);
  if (arr) T(c, 72, 2, 'Time', 5, head, 'right');
  T(c, 88, 2, 'Plat', 5, head, 'right'); T(c, 93, 2, 'Expt', 5, head);
  T(c, 2, 9, arr ? 'From' : 'Time', 5, head); if (!arr) T(c, 22, 9, 'Destination', 5, head);
  T(c, 126, 9, d.named ? d.station : d.crs, 5, dim, 'right');
  var list = ((arr ? d.arr : d.dep) || {}).rows || [], hm = function (t) { return t ? londonTime(t).slice(0, 5) : '--:--'; };
  if (!list.length) T(c, 2, 16, 'Waiting for ' + d.crs, 5, row);
  list.slice(0, 6).forEach(function (s, i) {
    var y = 16 + i * 7, col = s.st === 'canc' ? row : (s.due ? due : row), name = String(s.n || '');
    var late = s.st === 'late' || (s.st === 'arr' && s.x && s.x - s.t >= 60);
    T(c, 93, y, late && s.x ? hm(s.x) : (RB_EXPT[s.st] || ''), 5, s.st === 'canc' ? red : col);
    if (s.st !== 'canc') T(c, 88, y, String(s.p || ''), 5, col, 'right');
    if (arr) { T(c, 72, y, hm(s.t), 5, col, 'right'); T(c, 2, y, name.slice(0, 11), 5, col); }
    else { T(c, 2, y, hm(s.t), 5, col); T(c, 22, y, name.slice(0, 15), 5, col); }
  });
  T(c, 2, 58, 'Page 1 of ' + (list.length > 6 ? 2 : 1), 5, row);
  var now = d.synced ? londonTime(d.now) : '--:--:--';
  T(c, 126, 57, cfg.clock_seconds ? now : now.slice(0, 5), 7, row, 'right');
  setText('rbPvTitle', arr ? 'arrivals' : 'departures');
  var dueCount = list.filter(function (s) { return s.due; }).length;
  setText('rbPvMeta', cfg.due_min ? dueCount + ' due within ' + cfg.due_min + ' min' : 'due soon off');
}
if ($('rbDue')) {
  $('rbDue').addEventListener('change', function () { postRbCfg({ due_min: +this.value }); this.blur(); });
  $('rbRows').addEventListener('change', function () { postRbCfg({ rows: +this.value }); this.blur(); });
  $('rbSwitch').addEventListener('change', function () { postRbCfg({ switch_s: +this.value }); });
  $('rbStale').addEventListener('change', function () { postRbCfg({ stale_s: +this.value }); });
  $('rbLevel').addEventListener('input', function () { setText('rbLevelV', this.value + ' pc'); });
  $('rbLevel').addEventListener('change', function () { postRbCfg({ level: +this.value }); this.blur(); });
  seg('rbClock', function (v) { postRbCfg({ clock_seconds: v === '1' }); });
  $('rbReset').addEventListener('click', function () {
    var btn = this;
    api('/api/railboard', { reset: true }).then(function (d) { renderRb(d); note('rbMsg', 'Settings handed back to ' + (d.cfg.from === 'ha' ? 'Home Assistant.' : 'the build defaults.')); flash(btn, 'Done'); })
      .catch(function (err) { note('rbMsg', err.message, true); });
  });
}
if ($('rbDiag')) $('rbDiag').addEventListener('change', function () {
  var box = this;
  api('/api/railboard', { diag: box.checked }).then(renderRb).catch(function (err) { box.checked = !box.checked; note('rbMsg', err.message, true); });
});

// ---------------------------------------------------------------- yacht radar
function pollYr() { return api('/api/yachtradar').then(renderYr); }
function renderYr(d) {
  pageIdx.yachts = d.page;
  setText('yrTag', d.showing ? 'on screen' : 'not on screen');
  var led = $('yrLed'); if (led) { led.classList.toggle('online', !!d.connected); led.classList.toggle('offline', !d.connected); }
  setText('yrState', !d.keyPresent ? 'no ais key' : (d.open ? (d.connected ? 'streaming' : 'connecting') : 'stream closed'));
  setText('yrKey', d.keyPresent ? 'present' : 'not stored');
  setText('yrStream', d.open ? (d.connected ? 'connected' : 'connecting') : 'closed · page not on screen');
  setText('yrCount', d.count + ' on the plot');
  setText('yrAge', d.age != null ? ago(d.age) : '--');
  setText('yrLogged', d.logged + ' since boot');
  segSet('yrSort', d.bySize ? '1' : '0');
  var html = '<span class="h">vessel</span><span class="h r">length</span><span class="h r">range</span><span class="h r">speed</span>';
  (d.vessels || []).forEach(function (v) {
    html += '<span class="m' + v.m + '">' + esc(v.name ? v.name : 'MMSI ' + v.mmsi) + '</span><span class="r">' + (v.len ? v.len + ' m' : '--') +
      '</span><span class="r">' + v.km.toFixed(1) + ' km</span><span class="r">' + v.sog.toFixed(1) + ' kn</span>';
  });
  if (!(d.vessels || []).length) html += '<span class="empty">' + (!d.keyPresent ? 'No AIS key is stored - provision one with env:provision.' : (!d.open ? 'Show the page on the panel to open the stream.' : 'No vessels reported yet.')) + '</span>';
  var rows = $('yrRows'); if (rows) rows.innerHTML = html;
}
seg('yrSort', function (v) { api('/api/yachtradar', { bySize: v === '1' }).then(renderYr).catch(function () {}); });

// ---------------------------------------------------------------- lua effects
var luaSig = '';
function pollLua() { return api('/api/lua').then(renderLua); }
function pollEffects() { return Promise.all([pollLua(), pollClips()]); }

// Clips: the uploaded .pca animations in the panel's flash (/api/anim/*, the
// clock firmware's own routes - they answer without a "success" field, so plain
// fetch, not api()) and, with a card in the TF slot, the gallery on the card
// (/api/clips, which does).
var clipSig = '', thumbs = {}, thumbQ = Promise.resolve();
function kb(b) { return Math.round(b / 1024) + ' KB'; }
function gb(k) { return (k / 1048576).toFixed(1) + ' GB'; }
function mmss(t) { return Math.floor(t / 60) + ':' + ('0' + (t % 60).toFixed(1)).slice(-4); }
function pollClips() {
  return Promise.all([fetch('/api/anim/list').then(function (r) { return r.json(); }),
    F.sdclips ? api('/api/clips').catch(function () { return null; }) : null]).then(function (r) { renderClips(r[0], r[1]); });
}
function renderClips(d, g) {
  var host = $('clipList');
  if (!host || !d) return;
  var sd = !!(g && g.card.mounted), anims = d.anims || [], clips = sd ? g.clips : [], cur = d.playing ? d.current : '';
  // Where a new clip goes: the card when there is one. cap is the most frames a
  // clip there may hold, fit what the free space takes now.
  cm.list = sd ? { sd: 1, cap: g.maxFrames, fit: Math.floor((g.maxBytes - 44) / 4098), max: g.maxBytes, clips: clips }
    : { cap: 360, fit: d.usable ? d.maxFrames : 0, max: d.maxUploadBytes, clips: anims };
  cmSize();
  setText('clipTag', d.playing ? 'playing' : (anims.length + clips.length) + ' clips');
  if (g) {
    setText('clipCard', sd ? 'Card ' + g.card.type + ': ' + gb(g.card.freeKB) + ' free of ' + gb(g.card.totalKB) + '. New clips go to the card.' : g.reason);
    $('clipMount').style.display = sd ? 'none' : '';
  }
  var sig = JSON.stringify([anims, clips, cur]);
  if (sig === clipSig) return;
  clipSig = sig;
  host.innerHTML = anims.length + clips.length ? '' : '<p class="field-hint">No clips are stored on the panel.</p>';
  clips.forEach(function (a) { clipRow(host, a, 1, cur === 'sd:' + a.name); });
  anims.forEach(function (a) { clipRow(host, a, 0, cur === a.name); });
}
function clipRow(host, a, sd, here) {
  var row = document.createElement('div');
  row.className = 'pn-row' + (here ? ' here' : '');
  row.innerHTML = (sd ? '<canvas class="pn-thumb" width="128" height="64"></canvas>' : '') + '<div class="pn-name"><strong>' + esc(a.name) +
    '</strong><span class="ct-hint">' + (sd ? mmss(a.frames * a.ms / 1000) + ' · card · ' : 'flash · ') + a.frames + ' frames · ' + kb(a.bytes) +
    '</span></div><span class="pn-here">on screen</span><button type="button" class="btn btn-sm">Play</button><button type="button" class="btn btn-sm btn-danger">Delete</button>';
  var b = row.querySelectorAll('button');
  b[0].addEventListener('click', function () { playClip(a.name, sd, b[0]); });
  b[1].addEventListener('click', function () {
    if (!confirm('Delete the clip "' + a.name + '" from the ' + (sd ? 'card' : 'panel') + '?')) return;
    (sd ? api('/api/clips', { 'delete': a.name }) : fetch('/api/anim/delete?name=' + encodeURIComponent(a.name)))
      .then(function () { clipSig = ''; return pollClips(); }).catch(function (err) { flash(b[1], err.message, true); });
  });
  if (sd) thumb(row.querySelector('canvas'), a);
  host.appendChild(row);
}
function playClip(name, sd, btn) {
  // A clip shows on the clock page, and choosing a page releases any forced
  // mode - so the page comes first and the clip second.
  api('/api/panel', { show: { page: 0 } })
    .then(function () { return sd ? api('/api/clips', { play: name }) : fetch('/api/anim/play?name=' + encodeURIComponent(name)).then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); }); })
    .then(function () { clipSig = ''; return pollClips(); })
    .catch(function (err) { flash(btn, err.message, true); });
}
function thumb(cv, a) {   // frame 0 from the card, fetched once, one request at a time
  var k = a.name + '/' + a.bytes, c = cv.getContext('2d');
  if (thumbs[k]) { c.putImageData(thumbs[k], 0, 0); return; }
  thumbQ = thumbQ.then(function () {
    return fetch('/api/clips/frame?i=0&name=' + encodeURIComponent(a.name)).then(function (r) { if (!r.ok) throw new Error(); return r.arrayBuffer(); });
  }).then(function (ab) {
    var b = new Uint8Array(ab), d = new DataView(ab), P = b[8], im = c.createImageData(128, 64), p = im.data, i, v;
    for (i = 0; i < 8192; i++) {
      v = d.getUint16(12 + 2 * Math.min(P - 1, b[12 + 2 * P + (i >> 1)] >> (i & 1 ? 0 : 4) & 15), true);
      p[4 * i] = (v >> 11) * 255 / 31; p[4 * i + 1] = (v >> 5 & 63) * 255 / 63; p[4 * i + 2] = (v & 31) * 255 / 31; p[4 * i + 3] = 255;
    }
    c.putImageData(thumbs[k] = im, 0, 0);
  }).catch(function () {});
}
if ($('clipStop')) $('clipStop').addEventListener('click', function () {
  var btn = this;
  fetch('/api/mode/auto').then(function () { clipSig = ''; return pollClips(); }).catch(function (err) { flash(btn, err.message, true); });
});
if ($('clipMount')) $('clipMount').addEventListener('click', function () {
  var btn = this;
  api('/api/clips', { mount: true }).then(function () { clipSig = ''; return pollClips(); }).catch(function (err) { flash(btn, err.message, true); });
});

// ---------------------------------------------------------------- make a clip
// Sound drawn the way a scope draws it, made into a clip in this browser: the
// renderer of tools/oscmusic/scope_render.py and the PCA1 writer of gif2pca.
// The sound stays on the phone; only the clip is sent. This block, down to
// "The card", is run against the Python by tools/oscmusic/test_clip_maker.py.
function scopePal(col) {   // [rgb, rgb565] of 16 levels: green (the script's), amber, blue, white
  var rgb = [], c16 = [];
  for (var i = 0; i < 16; i++) {
    var v = i / 15, m = Math.floor(255 * Math.pow(v, 0.8)),
      a = Math.floor(v > 0.75 ? 255 * (v - 0.75) / 0.25 * 0.85 : 20 * v),
      b = Math.floor(v > 0.8 ? 255 * (v - 0.8) / 0.2 * 0.7 : 40 * v),
      c = [[a, m, b], [m, 0.7 * m + 0.3 * b, 0.3 * a], [0.5 * a, 0.55 * m + 0.35 * a, m], [m, m, m]][col].map(function (x) { return Math.min(255, x | 0); });
    rgb.push(c);
    c16.push((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
  }
  return [rgb, c16];
}
// The face: every sample pair lands on a pixel, so where the beam dwells it
// glows. o: rate, fps, gain (times 1/10), decay (per 40 ms), zoom. At 25 fps and
// zoom 1 this is the script's arithmetic, float32 where the script is float32.
function scope(o) {
  var z = o.zoom, fw = Math.min(128, 2 * Math.round(32 * z)), ox = fw / 2 - 32, x0 = 64 - fw / 2, N = fw * 64,
    glow = new Float32Array(N), cnt = new Float32Array(N), v = new Float32Array(N), h = new Float32Array(N),
    dec = Math.fround(Math.pow(o.decay, 25 / o.fps)), k = Math.fround(48000 / o.rate * z), g = Math.fround(o.gain / 10),
    spf = Math.floor(o.rate / o.fps);
  return { spf: spf, frame: function (s, at, idx) {   // s: X,Y pairs; the frame starts at pair `at`; idx gets 128x64 levels
    var i, x, y, p;
    for (i = at; i < at + spf; i++) {
      x = (s[2 * i] * z + 1) * 32 + ox; y = (1 - s[2 * i + 1] * z) * 32;
      if (x >= 0 && y >= 0 && x <= fw && y <= 64) cnt[Math.min(y | 0, 63) * fw + Math.min(x | 0, fw - 1)]++;
    }
    for (i = 0; i < N; i++) { glow[i] = glow[i] * dec + cnt[i] / 4 * k; cnt[i] = 0; v[i] = 1 - Math.exp(-glow[i] * g); }
    for (p = 0; p < N; p++) { x = p % fw; h[p] = (x ? v[p - 1] : 0) / 4 + v[p] / 2 + (x < fw - 1 ? v[p + 1] : 0) / 4; }   // bloom [1,2,1]/4, rows...
    idx.fill(0);
    for (p = 0; p < N; p++) {   // ...then columns, zero past the edges
      y = p / fw | 0;
      idx[y * 128 + x0 + p % fw] = Math.round(Math.min(1, v[p] + 0.3 * ((y ? h[p - fw] : 0) / 4 + h[p] / 2 + (y < 63 ? h[p + fw] : 0) / 4)) * 15);
    }
  } };
}
// True when left and right never part by half a panel pixel: L = X, R = Y then
// draws only the diagonal X = Y. At zoom 1 a difference d between the channels
// puts a point 32 d / sqrt 2 = 22.6 d pixels off that line.
function flat(s) {
  for (var i = 0, m = 0; i < s.length; i += 2) m = Math.max(m, Math.abs(s[i] - s[i + 1]));
  return m < 0.5 / 22.6;
}
// PCA1: "PCA1", frames, ms, colours, flags (1 loop), 2 reserved; palette RGB565;
// a delay per frame; then 4 bpp frames, high nibble the left pixel.
function pcaHead(n, ms, pal) {
  var b = new Uint8Array(44 + 2 * n), d = new DataView(b.buffer), i;
  b.set([80, 67, 65, 49]); d.setUint16(4, n, true); d.setUint16(6, ms, true); b[8] = 16; b[9] = 1;
  for (i = 0; i < 16; i++) d.setUint16(12 + 2 * i, pal[i], true);
  for (i = 0; i < n; i++) d.setUint16(44 + 2 * i, ms, true);
  return b;
}
function pcaPut(b, at, idx) { for (var i = 0; i < 8192; i += 2) b[at++] = idx[i] << 4 | idx[i + 1]; }
function blobBuf(b) { return b.arrayBuffer ? b.arrayBuffer() : new Response(b).arrayBuffer(); }
var PCM = { 16: function (d, o) { return d.getInt16(o, true) / 32768; },
  24: function (d, o) { return (d.getInt8(o + 2) * 65536 + d.getUint16(o, true)) / 8388608; },
  32: function (d, o) { return d.getInt32(o, true) / 2147483648; },
  f32: function (d, o) { return d.getFloat32(o, true); }, f64: function (d, o) { return d.getFloat64(o, true); } };
// A WAV is read where it lies: its header, then only the span a frame needs.
// Resolves null for anything but PCM or float; the browser decodes those.
function wavOpen(f) {
  var h = {}, pos = 12;
  function tag(ab, o) { return String.fromCharCode.apply(null, new Uint8Array(ab, o, 4)); }
  function chunk() {
    return blobBuf(f.slice(pos, pos + 48)).then(function (ab) {
      if (ab.byteLength < 8) return null;
      var d = new DataView(ab), id = tag(ab, 0), len = d.getUint32(4, true), t;
      if (id === 'fmt ' && ab.byteLength >= 24) {
        t = d.getUint16(8, true);
        if (t === 0xFFFE && ab.byteLength >= 34) t = d.getUint16(32, true);   // WAVE_FORMAT_EXTENSIBLE: the sub-format's code
        h = { ch: d.getUint16(10, true), rate: d.getUint32(12, true), ba: d.getUint16(20, true), get: PCM[(t === 3 ? 'f' : t === 1 ? '' : '?') + d.getUint16(22, true)] };
      }
      if (id === 'data') {
        if (!h.get || !h.ch || !h.rate || !h.ba) return null;
        var off = pos + 8, ba = h.ba, s = ba / h.ch, ch = h.ch, g = h.get;
        return { wav: 1, ch: ch, rate: h.rate, n: Math.floor(Math.min(len || f.size, f.size - off) / ba), read: function (at, cnt) {
          return blobBuf(f.slice(off + at * ba, off + (at + cnt) * ba)).then(function (ab) {
            var d = new DataView(ab), m = Math.floor(ab.byteLength / ba), out = new Float32Array(2 * cnt), i, o;
            for (i = 0, o = 0; i < m; i++, o += ba) { out[2 * i] = g(d, o); out[2 * i + 1] = ch > 1 ? g(d, o + s) : out[2 * i]; }
            return out;
          });
        } };
      }
      pos += 8 + len + (len & 1);
      return chunk();
    });
  }
  return blobBuf(f.slice(0, 12)).then(function (ab) { return ab.byteLength === 12 && tag(ab, 0) === 'RIFF' && tag(ab, 8) === 'WAVE' ? chunk() : null; });
}
// Anything else - MP3, FLAC, the sound track of a video, a recording - goes
// through the browser's decoder: the whole file in memory, out at 48 kHz.
function decOpen(f) {
  return blobBuf(f).then(function (ab) {
    var C = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    return new Promise(function (ok, bad) { new C(2, 1, 48000).decodeAudioData(ab, ok, bad); });
  }).then(function (b) {
    var L = b.getChannelData(0), R = b.numberOfChannels > 1 ? b.getChannelData(1) : L;
    return { ch: b.numberOfChannels, rate: b.sampleRate, n: b.length, read: function (at, cnt) {
      var out = new Float32Array(2 * cnt);
      for (var i = 0; i < cnt && at + i < b.length; i++) { out[2 * i] = L[at + i]; out[2 * i + 1] = R[at + i]; }
      return Promise.resolve(out);
    } };
  });
}

// The card.
var cm = { src: null, list: null, busy: 0, stop: 0, job: null, anim: 0, auto: '', rec: null, xhr: null }, cmT = 0;
function cmNum(id) { return +$(id).value || 0; }
function cmPlan() {   // the settings as a job, fitted to the sound and to the room where the clip goes
  var s = cm.src, L = cm.list, o = { rate: s.rate, fps: cmNum('cmFps'), gain: Math.pow(2, cmNum('cmGain')),
    decay: cmNum('cmDecay'), zoom: cmNum('cmZoom'), col: cmNum('cmCol') }, spf = Math.floor(s.rate / o.fps);
  o.at = Math.min(Math.floor(Math.max(0, cmNum('cmStart')) * s.rate), s.n);
  o.ask = Math.round(cmNum('cmDur') * o.fps);
  o.want = Math.max(0, Math.min(o.ask, L ? L.cap : 360, Math.floor((s.n - o.at) / spf)));
  o.n = L ? Math.max(0, Math.min(o.want, L.fit)) : o.want;
  return o;
}
function cmSize() {
  var el = $('cmSize'), room = $('cmRoom'), L = cm.list;
  if (!cm.src || !el) return;
  var o = cmPlan(), where = L && L.sd ? 'the card' : "the panel's flash", short = L && o.n < o.want, sig,
    t = !o.want ? 'Nothing to draw: the start is at the end.' : o.want + ' frames, ' + mmss(o.want / o.fps) + ', ' + kb(44 + o.want * 4098) + ' on ' + where + '.';
  if (L && o.want && o.want < o.ask && o.want === L.cap) t += ' Cut to ' + mmss(L.cap / o.fps) + ', the most a clip on ' + where + ' may hold.';
  if (short) t += (o.n ? ' Room for ' + mmss(o.n / o.fps) + ' only: delete a clip below to free ' : ' No room: delete a clip below to free ') + kb(44 + o.want * 4098 - Math.max(0, L.max)) + '.';
  el.textContent = t;
  el.classList.toggle('pn-warn', !!short);
  if (!cm.busy) $('cmGo').disabled = !o.n;
  sig = short ? L.sd + JSON.stringify(L.clips) : '';
  if (room.dataset.sig === sig) return;
  room.dataset.sig = sig;
  room.innerHTML = '';
  if (short) L.clips.forEach(function (a) { clipRow(room, a, L.sd, false); });
}
function cmDraw(idx, rgb) {
  var c = $('cmCanvas').getContext('2d'), im = c.createImageData(128, 64), p = im.data, i, q;
  for (i = 0; i < 8192; i++) { q = rgb[idx[i]]; p[4 * i] = q[0]; p[4 * i + 1] = q[1]; p[4 * i + 2] = q[2]; p[4 * i + 3] = 255; }
  c.putImageData(im, 0, 0);
}
function cmRender(o, n, each) {   // n frames from o.at, after a run-in of up to 8 so the afterglow is lit
  var sc = scope(o), spf = sc.spf, pre = Math.min(8, Math.floor(o.at / spf)), pos = o.at - pre * spf, f = 0,
    idx = new Uint8Array(8192), job = cm.job = {};
  function block() {   // a second of sound per read, then the event loop gets a turn
    var k = Math.min(25, pre + n - f);
    return cm.src.read(pos + f * spf, k * spf).then(function (s) {
      if (cm.job !== job) throw new Error('Cancelled.');
      for (var i = 0; i < k; i++, f++) { sc.frame(s, i * spf, idx); if (f >= pre) each(idx, f - pre); }
      return f < pre + n && new Promise(function (r) { setTimeout(r, 0); }).then(block);
    });
  }
  return block();
}
function cmPreview() {
  clearTimeout(cmT);
  cmT = setTimeout(function () {
    if (!cm.src || cm.busy) return;
    var o = cmPlan(), rgb = scopePal(o.col)[0];
    clearInterval(cm.anim);
    setText('cmGainV', '×' + o.gain.toFixed(2)); setText('cmDecayV', o.decay.toFixed(2)); setText('cmZoomV', '×' + o.zoom.toFixed(2));
    setText('cmPvT', 'at ' + mmss(o.at / o.rate));
    cmSize();
    if (o.want) cmRender(o, 1, function (idx) { cmDraw(idx, rgb); }).catch(function () {});
  }, 150);
}
function cmOpen(f) {
  cm.src = null; clearInterval(cm.anim); $('cmBody').hidden = true; setText('cmFlat', '');
  note('cmInfo', 'Opening ' + f.name + '...');
  wavOpen(f).catch(function () { return null; }).then(function (w) {
    if (w) return w;
    if (f.size > 5e7 && !confirm(f.name + ' is ' + Math.round(f.size / 1048576) + ' MB and not a WAV this page reads piece by piece, so the browser must hold all of it and its decoded sound in memory. A phone may give up. Go on?'))
      throw new Error('Not opened. Saved as a WAV, the same sound is read a piece at a time.');
    note('cmInfo', 'Decoding ' + f.name + '...');
    return decOpen(f);
  }).then(function (s) {
    var len = s.n / s.rate, nm = $('cmName'), base = f.name.replace(/\.[^.]*$/, '').replace(/[^A-Za-z0-9_-]/g, '').slice(0, 24) || 'clip';
    if (!s.n) throw new Error('No sound in ' + f.name + '.');
    cm.src = s;
    $('cmAt').max = $('cmStart').max = len.toFixed(1);
    if (!nm.value || nm.value === cm.auto) nm.value = cm.auto = base;
    note('cmInfo', f.name + ': ' + mmss(len) + ', ' + s.rate / 1000 + ' kHz, ' + (s.ch > 2 ? s.ch + ' channels, the first two drawn' : s.ch > 1 ? 'stereo' : 'mono') + (s.wav ? ', read in place.' : ', decoded.'));
    // Allowed, but said plainly: a second from the middle shows whether the channels ever part.
    if (s.ch < 2) setText('cmFlat', 'Mono: the same sound on X and Y draws only a diagonal line.');
    else s.read(Math.floor(s.n / 2), Math.min(s.rate, s.n - Math.floor(s.n / 2))).then(function (b) {
      setText('cmFlat', flat(b) ? 'Left and right are all but the same here, so this draws only a diagonal line.' : '');
    });
    setText('cmPvM', s.rate / 1000 + ' kHz');
    $('cmBody').hidden = false;
    cmPreview();
  }).catch(function (err) { note('cmInfo', (err && err.message) || 'The browser cannot decode the sound of this file.', true); });
}
function cmRec(disp, btn) {   // secure pages only: a recording, then the same path as a file
  if (cm.rec) { cm.rec.stop(); return; }
  var md = navigator.mediaDevices;
  (disp ? md.getDisplayMedia({ video: true, audio: true }) : md.getUserMedia({ audio: { channelCount: 2, echoCancellation: false, noiseSuppression: false, autoGainControl: false } })).then(function (st) {
    var bits = [], rec = cm.rec = new MediaRecorder(st);
    rec.ondataavailable = function (e) { bits.push(e.data); };
    rec.onstop = function () {
      st.getTracks().forEach(function (t) { t.stop(); });
      cm.rec = null; btn.textContent = btn.dataset.t;
      var b = new Blob(bits, { type: rec.mimeType }); b.name = 'recording';
      cmOpen(b);
    };
    btn.dataset.t = btn.textContent; btn.textContent = 'Stop recording';
    rec.start();
  }).catch(function (err) { note('cmInfo', 'No recording: ' + err.message, true); });
}
function cmBusy(on) {
  cm.busy = on; cm.stop = 0;
  $('cmProg').classList.toggle('show', !!on);
  $('cmGo').textContent = on ? 'Cancel' : 'Make the clip';
  $('cmGo').disabled = false;
  $('cmPlay').disabled = $('cmFile').disabled = !!on;
  if (!on) cmSize();
}
function cmProg(fr, t) { $('cmBar').style.width = (100 * fr).toFixed(1) + '%'; setText('cmPct', t); }
if ($('cmFile')) {
  $('cmFile').addEventListener('change', function () { if (this.files[0]) cmOpen(this.files[0]); });
  // getUserMedia and getDisplayMedia exist in secure contexts only; the portal is plain http.
  if (window.isSecureContext && navigator.mediaDevices && window.MediaRecorder) {
    $('cmCap').style.display = '';
    seg('cmCap', function (v, b) { cmRec(v === '1', b); });
  } else setText('cmCapH', 'Recording the microphone or another tab needs a secure (https) page, and the panel serves plain http: record on the phone, then pick the file.');
  ['cmDur', 'cmFps', 'cmCol', 'cmGain', 'cmDecay', 'cmZoom'].forEach(function (id) { $(id).addEventListener('input', cmPreview); $(id).addEventListener('change', cmPreview); });
  $('cmAt').addEventListener('input', function () { $('cmStart').value = this.value; cmPreview(); });
  $('cmStart').addEventListener('input', function () { $('cmAt').value = this.value; cmPreview(); });
  $('cmAll').addEventListener('click', function () { if (!cm.src) return; $('cmAt').value = $('cmStart').value = 0; $('cmDur').value = (cm.src.n / cm.src.rate).toFixed(1); cmPreview(); });
  $('cmPlay').addEventListener('click', function () {
    if (!cm.src || cm.busy) return;
    var o = cmPlan(), rgb = scopePal(o.col)[0], fr = [], spf = Math.floor(o.rate / o.fps);
    clearInterval(cm.anim);
    cmRender(o, Math.min(o.want, 4 * o.fps), function (idx) { fr.push(idx.slice()); }).then(function () {
      var i = 0;
      cm.anim = setInterval(function () {
        if (i === fr.length) { clearInterval(cm.anim); return; }
        setText('cmPvT', 'at ' + mmss((o.at + i * spf) / o.rate));
        cmDraw(fr[i++], rgb);
      }, 1000 / o.fps);
    }).catch(function () {});
  });
  $('cmGo').addEventListener('click', function () {
    if (cm.busy) { cm.stop = 1; cm.job = null; if (cm.xhr) cm.xhr.abort(); return; }
    var name = $('cmName').value.trim(), o, parts, blk;
    if (!cm.src) return;
    if (!/^[A-Za-z0-9_-]{1,24}$/.test(name)) { note('cmMsg', 'Name the clip with 1 to 24 of A-Z a-z 0-9 _ - and no spaces.', true); return; }
    clearInterval(cm.anim); note('cmMsg', ''); $('cmShow').style.display = 'none';
    cmBusy(1); cmProg(0, 'Asking the panel for room');
    pollClips().then(function () {   // fresh room first: another phone may have filled it
      var L = cm.list;
      o = cmPlan();
      if (cm.stop) throw new Error('Cancelled.');
      if (!o.n) throw new Error(L.fit < 1 ? 'No room for the clip: delete one first.' : 'Nothing to draw here.');
      if (L.clips.some(function (a) { return a.name === name; }) && !confirm('Replace the clip "' + name + '" on ' + (L.sd ? 'the card' : 'the panel') + '?')) throw new Error('Nothing sent; the clip already there is kept.');
      parts = [pcaHead(o.n, 1000 / o.fps, scopePal(o.col)[1])];
      return cmRender(o, o.n, function (idx, f) {
        if (cm.stop) throw new Error('Cancelled.');
        if (!(f % 25)) parts.push(blk = new Uint8Array(Math.min(25, o.n - f) * 4096));
        pcaPut(blk, f % 25 * 4096, idx);
        if (f % 5 === 4 || f === o.n - 1) cmProg((f + 1) / o.n, 'Drawing frame ' + (f + 1) + ' of ' + o.n);
      });
    }).then(function () {
      var sd = cm.list.sd, body = new FormData(), t0 = Date.now();
      body.append('anim', new Blob(parts, { type: 'application/octet-stream' }), name + '.pca');
      parts = blk = null;
      return new Promise(function (ok, bad) {
        var x = cm.xhr = new XMLHttpRequest();
        x.open('POST', (sd ? '/api/clips/upload?bytes=' + (44 + o.n * 4098) + '&name=' : '/api/anim/upload?name=') + encodeURIComponent(name));
        x.upload.onprogress = function (e) { if (e.lengthComputable) cmProg(e.loaded / e.total, 'Sending ' + kb(e.loaded) + ' of ' + kb(e.total) + ', ' + kb(e.loaded / Math.max(1, (Date.now() - t0) / 1000)) + '/s'); };
        x.onload = function () { var d = {}; try { d = JSON.parse(x.responseText); } catch (e) {} if (d.success) ok(sd); else bad(new Error('The panel refused the clip: ' + (d.error || 'HTTP ' + x.status))); };
        x.onerror = function () { bad(new Error('The upload broke off. Is the panel still on the network?')); };
        x.onabort = function () { bad(new Error('Cancelled.')); };
        x.send(body);
      });
    }).then(function (sd) {
      var b = $('cmShow');
      note('cmMsg', 'Stored on ' + (sd ? 'the card' : 'the panel') + ' as ' + name + ', ' + mmss(o.n / o.fps) + '.');
      b.textContent = 'Play ' + name; b.style.display = '';
      b.onclick = function () { playClip(name, sd, b); };
      clipSig = '';
      pollClips().catch(function () {});
    }).catch(function (err) { note('cmMsg', err.message, true); }).then(function () { cm.xhr = null; cmBusy(0); });
  });
}
function renderLua(d) {
  setText('luaTag', d.current >= 0 ? 'playing' : (d.effects.length + ' effects'));
  var sig = JSON.stringify(d), host = $('luaList');
  if (sig === luaSig || !host) return;
  luaSig = sig;
  host.innerHTML = d.effects.length ? '' : '<p class="field-hint">No effects are loaded.</p>';
  d.effects.forEach(function (name, i) {
    var row = document.createElement('div');
    row.className = 'pn-row' + (i === d.current ? ' here' : '');
    row.innerHTML = '<div class="pn-name"><strong>' + esc(name) + '</strong></div><span class="pn-here">on screen</span><button type="button" class="btn btn-sm">Show</button>';
    var btn = row.querySelector('button');
    btn.addEventListener('click', function () { api('/api/lua', { show: i }).then(renderLua).catch(function (err) { flash(btn, err.message, true); }); });
    host.appendChild(row);
  });
}

// ---------------------------------------------------------------- knob
var knBase = null, knPrev = {};
function pollKnob() { return api('/api/knob').then(renderKnob); }
function renderKnob(d) {
  var s = d.stats, df = d.defaults;
  var rev = $('knRev'), lock = $('knLock'), deb = $('knDeb'), det = $('knDet');
  if (!rev) return;
  if (!focused(rev)) rev.checked = !!d.reverse;
  if (!focused(lock)) { if (d.lockoutMs > +lock.max) lock.max = d.lockoutMs; lock.value = d.lockoutMs; setText('knLockV', d.lockoutMs + ' ms'); }
  if (!focused(deb)) { if (d.debounceMs > +deb.max) deb.max = d.debounceMs; deb.value = d.debounceMs; setText('knDebV', d.debounceMs + ' ms'); }
  if (!focused(det)) det.value = d.detent;
  setText('knLockH', 'After a step the decoder ignores the knob this long. Longer drops the steps of a fast turn. Default ' + df.lockoutMs + ' ms.');
  setText('knDebH', 'The switch must stay put this long to count as pressed or released. Default ' + df.debounceMs + ' ms.');
  setText('knDetH', 'In force now: ' + (s.detent === 1 ? 'at 11 and at 00' : (s.detent === 0 ? 'at 11 only' : 'at 11, watching for a rest at 00')) +
    (d.detent === -1 && s.detent === 1 ? ' - learned from the knob' : '') + '.');
  setText('knTag', s.timer ? 'sampled at 1 kHz' : 'sampled per loop');
  if (!knBase) knBase = { cw: 0, ccw: 0, click: 0, long: 0 };
  ['cw', 'ccw', 'click', 'long'].forEach(function (k) {
    if (s[k] < knBase[k]) knBase[k] = 0;   // the panel rebooted: its counts restarted
    var el = $('kn' + k.charAt(0).toUpperCase() + k.slice(1)), v = s[k] - knBase[k];
    if (!el) return;
    if (knPrev[k] != null && s[k] !== knPrev[k]) { el.classList.remove('bump'); void el.offsetWidth; el.classList.add('bump'); }
    knPrev[k] = s[k];
    el.textContent = v;
  });
  var LAST = { cw: 'clockwise', ccw: 'anticlockwise', click: 'click', long: 'long press' };
  setText('knLast', s.last ? 'last: ' + LAST[s.last] + (s.agoMs != null ? ' · ' + (s.agoMs < 10000 ? (s.agoMs / 1000).toFixed(1) + ' s ago' : ago(s.agoMs / 1000)) : '') : 'turn or press the knob');
  setText('knHeld', s.held ? 'held' : 'released');
}
function postKnob(body) {
  api('/api/knob', body).then(function (d) { renderKnob(d); note('knMsg', 'Applied at once. Written to flash a few seconds after the last change.'); })
    .catch(function (err) { note('knMsg', err.message, true); });
}
if ($('knRev')) {
  $('knRev').addEventListener('change', function () { postKnob({ reverse: this.checked }); });
  $('knLock').addEventListener('input', function () { setText('knLockV', this.value + ' ms'); });
  $('knLock').addEventListener('change', function () { postKnob({ lockoutMs: +this.value }); this.blur(); });
  $('knDeb').addEventListener('input', function () { setText('knDebV', this.value + ' ms'); });
  $('knDeb').addEventListener('change', function () { postKnob({ debounceMs: +this.value }); this.blur(); });
  $('knDet').addEventListener('change', function () { postKnob({ detent: +this.value }); this.blur(); });
  $('knDefaults').addEventListener('click', function () { postKnob({ defaults: true }); });
  $('knZero').addEventListener('click', function () {
    api('/api/knob').then(function (d) { knBase = { cw: d.stats.cw, ccw: d.stats.ccw, click: d.stats.click, long: d.stats.long }; renderKnob(d); });
  });
}

// ---------------------------------------------------------------- start
api('/api/panel').then(learn).catch(function () {});
setInterval(function () {   // the sidebar's "screen" line, while no Panel page polls it
  if (document.hidden || active === 'pnow') return;
  api('/api/panel').then(learn).catch(function () {});
}, 5000);
document.addEventListener('DOMContentLoaded', schedule);
schedule();
})();
)JS";

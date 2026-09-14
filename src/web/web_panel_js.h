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
var HINTS = { clock: 'Always on - where the panel falls back to', world: 'Daylight map and the time', flights: 'Arrivals and departures', trains: 'Station departures and arrivals', yachts: 'AIS vessels in the bay', other: 'Always visited' };
var pageIdx = {}, lastPanel = null;

// ---------------------------------------------------------------- polling
var POLL = { pnow: [pollNow, 2000], pflights: [pollFb, 5000], ptrains: [pollRb, 5000], pworld: [pollWc, 30000], pyachts: [pollYr, 3000], plua: [pollLua, 3000], pknob: [pollKnob, 250] };
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
    if (wcMap) drawWorld(c, wcMap, n.time);
    else { T(c, 64, 26, 'WORLD CLOCK', 8, dim, 'center'); if (!wcLoading) loadWorld().catch(function () {}); }
  } else if (n.key === 'flights') {
    T(c, 2, 1, 'FLIGHTS', 5, '#fff'); T(c, 126, 1, n.time, 5, dim, 'right'); R(c, 0, 8, 128, 1, dim);
    for (i = 0; i < 7; i++) { y = 11 + i * 7; R(c, 2, y, 17, 5, fg); R(c, 22, y, 22, 5, fg); R(c, 50, y, 14 + (i * 37) % 30, 5, fg); R(c, 104, y, 22, 5, dim); }
    R(c, 0, 18, 1, 6, AMBER);
  } else if (n.key === 'trains') {
    T(c, 0, 1, 'STATION', 5, '#fff'); T(c, 128, 1, n.time, 5, '#fff', 'right');
    [0, 64].forEach(function (x0, k) {
      T(c, x0, 8, k ? 'ARRIVALS' : 'DEPARTURES', 5, AMBER);
      for (var j = 0; j < 3; j++) { var yy = 15 + j * 14; R(c, x0, yy, 17, 5, fg); R(c, x0 + 20, yy, 26, 5, dim); R(c, x0 + 55, yy, 5, 5, '#fff'); R(c, x0, yy + 7, 26 + (j * 11) % 20, 5, fg); }
    });
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
var RAD = Math.PI / 180, wcMap = null, wcLoading = false, wcTime = '--:--', wcSig = '';
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
function drawWorld(c, m, hm) {
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
  var a = 0.65 + 0.35 * Math.abs(Math.sin(Math.PI * Date.now() / 1000));
  m.cities.forEach(function (ct, i) {
    var cc = Math.floor((ct.lon + 180) / 360 * m.cols), rr = Math.floor((m.top - ct.lat) / (m.top - m.bottom) * m.rows);
    if (cc < 0 || cc >= m.cols || rr < 0 || rr >= m.rows) return;
    var f = i === m.home ? a : 1;
    R(c, cc * 2, rr * 2, 2, 2, 'rgb(' + Math.round(255 * f) + ',' + Math.round(140 * f) + ',' + Math.round(40 * f) + ')');
  });
  T(c, 1, 50, hm || '--:--', 11, '#fff');
}
function loadWorld() {
  wcLoading = true;
  return api('/api/worldclock').then(function (d) {
    d.at = Date.now(); wcMap = d; wcLoading = false; pageIdx.world = d.page; return d;
  }, function (e) { wcLoading = false; throw e; });
}
function pollWc() {
  return Promise.all([loadWorld(), api('/api/panel')]).then(function (r) { learn(r[1]); wcTime = r[1].now.time; renderWc(); });
}
function drawWc() { var cv = $('wcCanvas'); if (cv && wcMap) drawWorld(ctxOf(cv), wcMap, wcTime); }
function renderWc() {
  var m = wcMap; if (!m) return;
  drawWc();
  setText('wcMeta', m.utc ? wcTime + ' panel time' : 'no time yet - all night');
  var s = sunOf(utcNow(m));
  var sig = JSON.stringify([m.home, m.cities.map(function (ct) { return s ? Math.round(elev(s, ct.lat, ct.lon)) : 0; })]);
  if (sig === wcSig) return;
  wcSig = sig;
  var host = $('wcCities'); if (!host) return;
  host.innerHTML = '';
  m.cities.forEach(function (ct, i) {
    var el = s ? elev(s, ct.lat, ct.lon) : null;
    var sun = el == null ? '' : (el > 0 ? 'day' : (el > -6 ? 'twilight' : 'night'));
    var row = document.createElement('div');
    row.className = 'pn-row';
    row.innerHTML = '<label class="check-row standalone"><input type="radio" name="wcHome" value="' + i + '"' + (i === m.home ? ' checked' : '') +
      '><span class="check-box" aria-hidden="true"></span><span class="check-text"><strong>' + esc(cap(ct.name)) + '</strong><span class="ct-hint">' +
      Math.abs(ct.lat).toFixed(2) + (ct.lat >= 0 ? ' N, ' : ' S, ') + Math.abs(ct.lon).toFixed(2) + (ct.lon >= 0 ? ' E' : ' W') +
      (i === m.home ? ' · home' : '') + '</span></span></label>' + (sun ? '<span class="pn-sun' + (sun === 'day' ? ' day' : '') + '">' + sun + '</span>' : '');
    row.querySelector('input').addEventListener('change', function () {
      api('/api/worldclock', { home: i }).then(function (d) { d.at = Date.now(); wcMap = d; wcSig = ''; renderWc(); note('wcMsg', cap(ct.name) + ' is home. Kept across reboots.'); })
        .catch(function (err) { note('wcMsg', err.message, true); wcSig = ''; renderWc(); });
    });
    host.appendChild(row);
  });
}
setInterval(function () {   // the home dot breathes, in both previews
  if (document.hidden || !wcMap) return;
  if (active === 'pworld') drawWc();
  else if (active === 'pnow' && lastPanel && lastPanel.now.key === 'world') drawNow(lastPanel);
}, 100);

// ---------------------------------------------------------------- flight board
function pollFb() { return api('/api/flightboard').then(renderFb); }
function renderFb(d) {
  pageIdx.flights = d.page;
  var sel = $('fbApt');
  if (sel && !sel.options.length) d.airports.forEach(function (a, i) {
    var o = document.createElement('option'); o.value = i; o.textContent = cap(a.name) + ' · ' + a.code; sel.appendChild(o);
  });
  if (sel && !focused(sel)) sel.value = d.airport;
  segSet('fbDir', d.dir);
  setText('fbTag', d.showing ? 'on screen' : 'not on screen');
  var b = d.board || {}, rows = $('fbRows'), html = '';
  if (b.have) {
    setText('fbHead', cap(b.name) + ' ' + (b.dir === 'dep' ? 'departures' : 'arrivals'));
    setText('fbAge', 'received ' + ago(b.age));
    html = '<span class="h">time</span><span class="h">flight</span><span class="h">to / from</span><span class="h r">status</span>';
    (b.rows || []).forEach(function (r, i) {
      var cls = 'st-' + (r.st || 'sched');
      html += '<span class="' + cls + (i === b.now ? ' now' : '') + '">' + esc(r.tm) + '</span><span class="' + cls + '">' + esc(r.fn) +
        '</span><span class="' + cls + '">' + esc(r.ct) + ' ' + esc(cap(r.cy !== r.ct ? r.cy : '')) + '</span><span class="r ' + cls + '">' + esc(r.w) + '</span>';
    });
    if (!(b.rows || []).length) html += '<span class="empty">The board is empty.</span>';
  } else {
    setText('fbHead', 'no board yet'); setText('fbAge', '--');
    html = '<span class="empty">' + esc(d.mqtt ? d.mqtt.status : 'No data') + '</span>';
  }
  if (rows) rows.innerHTML = html;
  var feed = $('fbFeed'), kv = [];
  if (b.have && (b.apt !== d.airports[d.airport].code || b.dir !== d.dir)) kv.push(['showing', b.apt + ' ' + b.dir + ' - the selection has not arrived yet', 'pn-warn']);
  if (b.have) kv.push(['fetched by HA', b.upd]);
  if (d.mqtt) {
    kv.push(['broker', d.mqtt.configured ? 'configured' : 'not configured', d.mqtt.configured ? '' : 'pn-warn']);
    kv.push(['mqtt', d.mqtt.connected ? 'connected' : 'not connected', d.mqtt.connected ? 'pn-ok' : 'pn-warn']);
    kv.push(['state', String(d.mqtt.status).toLowerCase()]);
  }
  if (feed) feed.innerHTML = kv.map(function (x) { return '<dt>' + x[0] + '</dt><dd class="' + (x[2] || '') + '">' + esc(x[1]) + '</dd>'; }).join('');
}
function postFb(body) {
  api('/api/flightboard', body).then(function (d) { renderFb(d); note('fbMsg', 'Applied. Kept across reboots; the board follows once the choice has settled.'); })
    .catch(function (err) { note('fbMsg', err.message, true); });
}
if ($('fbApt')) $('fbApt').addEventListener('change', function () { postFb({ airport: +this.value }); });
seg('fbDir', function (v) { postFb({ dir: v }); });

// ---------------------------------------------------------------- rail board
var rbEditing = false;
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
  setText('rbStation', cap(d.station));
  setText('rbTag', d.showing ? 'on screen' : 'not on screen');
  var stale = d.dep.stale && d.arr.stale;
  var led = $('rbLed'); if (led) { led.classList.toggle('online', !stale); led.classList.toggle('offline', stale); }
  setText('rbState', d.crs + ' · ' + (stale ? 'data updating' : 'fresh'));
  var last = Math.max(d.dep.ts || 0, d.arr.ts || 0);
  setText('rbUpd', last ? londonTime(last) + (d.synced ? ' · ' + ago(d.now - last) : '') : 'never');
  setText('rbDep', listLine(d.dep)); setText('rbArr', listLine(d.arr));
  var ha = d.ha;
  setText('rbHa', !ha.have ? 'no status yet' : (ha.err ? ha.err + (ha.code ? ' ' + ha.code : '') + (ha.retry ? ' · retry ' + ha.retry + ' s' : '') : 'ok ' + ha.code) + (ha.left ? ' · ' + ha.left + ' left today' : ''));
  var rt = (d.dep.rt || d.arr.rt || '').replace(/^REALTIME_DATA_/, '');
  setText('rbRt', rt ? rt.toLowerCase() : '--');
  setText('rbMq', (d.mqtt.connected ? 'connected' : String(d.mqtt.status).toLowerCase()) + (d.mqtt.configured ? '' : ' · no broker stored') + ' · refused ' + d.refused);
  var dg = $('rbDiag'); if (dg && !focused(dg)) dg.checked = !!d.diag;
  setText('rbFrom', { ha: 'from Home Assistant', web: 'from this page', build: 'build defaults' }[d.cfg.from] || '--');
  if (rbEditing) return;
  var c = d.cfg;
  $('rbPanels').value = c.panels; $('rbFont').value = c.font; $('rbRows').value = c.rows;
  $('rbSwitch').value = c.switch_s; $('rbStale').value = c.stale_s; $('rbLevel').value = c.level;
  setText('rbLevelV', c.level + ' pc');
}
each(document.querySelectorAll('#rbPanels,#rbFont,#rbRows,#rbSwitch,#rbStale,#rbLevel'), function (el) {
  el.addEventListener('input', function () { rbEditing = true; if (el.id === 'rbLevel') setText('rbLevelV', el.value + ' pc'); });
  el.addEventListener('change', function () { rbEditing = true; });
});
if ($('rbApply')) $('rbApply').addEventListener('click', function () {
  var btn = this, cfg = { panels: +$('rbPanels').value, font: $('rbFont').value, rows: +$('rbRows').value,
    switch_s: +$('rbSwitch').value, stale_s: +$('rbStale').value, level: +$('rbLevel').value };
  api('/api/railboard', { config: cfg }).then(function (d) { rbEditing = false; renderRb(d); note('rbMsg', 'Applied until Home Assistant sends its config again or the panel reboots.'); flash(btn, 'Applied'); })
    .catch(function (err) { note('rbMsg', err.message, true); });
});
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

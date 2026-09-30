// twin-lang.js - the language of the twin's pages on the flasher's copy: English or Russian.
//
// The contract every page of the twin follows, and the Mac app TWIN-NickoScopeMatrix-64x128 with them:
//   - the language is 'en' or 'ru', 'en' by default;
//   - ?lang=en|ru in the page URL wins and is kept in localStorage['twin-lang']; without it the page
//     takes localStorage['twin-lang'], and without that 'en';
//   - a small EN · RU switch on each page changes the language at once, without a reload, and keeps
//     it in localStorage; window.twinSetLang(lang) does the same for the app;
//   - links between the twin's pages carry ?lang= of the language in force.
// localStorage can be missing or throw (a private window, blocked site data): the page then runs on
// the URL parameter and the default alone.
//
// Load it as a classic script in <head>, before the page's own scripts. For the page:
//   twinLang.get()           the language in force
//   twinLang.set(lang)       switch and keep it; returns the language in force (other values are ignored)
//   twinLang.watch(fn)       fn(lang) now and after every switch; returns a function that stops it
//   twinLang.bindSwitch(el)  the buttons [data-lang] inside EL switch the language and carry aria-pressed
// window.twinSetLang is twinLang.set. Each page applies its own texts, <html lang> included, in a
// watch function; the flasher's page is the public English one, so there only the twin's note does.
(function () {
  'use strict';
  if (window.twinLang) return;

  const KEY = 'twin-lang';
  const known = (v) => v === 'en' || v === 'ru';

  function keep(v) {
    try { localStorage.setItem(KEY, v); } catch (_) { /* no storage: the choice lasts as long as the page */ }
  }

  function initial() {
    let v = null;
    try { v = new URLSearchParams(location.search).get('lang'); } catch (_) { /* no query */ }
    if (known(v)) { keep(v); return v; }
    try { v = localStorage.getItem(KEY); } catch (_) { v = null; }
    return known(v) ? v : 'en';
  }

  let lang = initial();
  const watchers = new Set();

  function set(v) {
    if (!known(v)) return lang;
    keep(v);
    if (v !== lang) {
      lang = v;
      for (const fn of Array.from(watchers)) {
        try { fn(lang); } catch (e) { console.error('[twin] language switch:', e); }
      }
    }
    return lang;
  }

  function watch(fn) {
    watchers.add(fn);
    fn(lang);
    return () => { watchers.delete(fn); };
  }

  function bindSwitch(el) {
    const buttons = Array.from(el.querySelectorAll('button[data-lang]'));
    for (const b of buttons) b.addEventListener('click', () => set(b.dataset.lang));
    return watch((v) => { for (const b of buttons) b.setAttribute('aria-pressed', String(b.dataset.lang === v)); });
  }

  window.twinLang = Object.freeze({ get: () => lang, set, watch, bindSwitch });
  window.twinSetLang = set;
})();

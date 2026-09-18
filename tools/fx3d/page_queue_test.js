// The /fx3d page's request queue (src/fx3d/fx3d_page.h), run for real in
// JavaScriptCore with the browser stubbed. tools/fx3d/check_fx3d.py puts the
// part above the CHECKS marker before the page's own script and the part
// below it after, and runs the three as one script. Every request is answered
// by hand, so the order of things is the test's, not a network's.

let checks = 0, failed = 0;
function check(c, what) {
  checks++;
  if (!c) {
    failed++;
    print("FAIL " + what);
  }
}

const els = {};
function el(id) {
  return els[id] || (els[id] = {
    id: id, textContent: "", className: "", value: "", kids: [],
    set innerHTML(v) { this.kids = []; },   // what the page does to empty a group
    appendChild(c) { this.kids.push(c); }, onclick: null, onchange: null
  });
}
globalThis.document = {
  visibilityState: "visible",
  getElementById: el,
  createElement(tag) { return { tag: tag, textContent: "", className: "", onclick: null }; }
};
let timers = [], timerId = 0;
globalThis.setTimeout = (fn, ms) => { timers.push({ id: ++timerId, fn: fn, ms: ms }); return timerId; };
globalThis.clearTimeout = id => { timers = timers.filter(t => t.id !== id); };
let poll = null;
globalThis.setInterval = fn => { poll = fn; };
globalThis.confirm = () => true;
globalThis.AbortController = class {
  constructor() { this.signal = { on: [] }; }
  abort() { this.signal.on.forEach(f => f()); }
};
const out = [];   // requests sent and not yet answered, oldest first
globalThis.fetch = (url, opts) => new Promise((resolve, reject) => {
  out.push({
    url: url,
    answer: (ok, j) => resolve({ ok: ok, json: () => Promise.resolve(j) })
  });
  if (opts && opts.signal) opts.signal.on.push(() => reject(new Error("aborted")));
});
const base = { scene: "", page: -1, bench: false, look: "flat", mode: "redblue", depthPx: 2, swap: false,
               gainL: 100, gainR: 100, profile: "kept", frameUs: 0, blitUs: 0, fps: 0, openUs: 0,
               looks: ["flat", "pop"], scenes: ["calib", "cube"] };
function answer(o) {
  const j = JSON.parse(JSON.stringify(base));
  for (const k in o) j[k] = o[k];
  out.shift().answer(true, j);
  drainMicrotasks();
}
function refuse(error) {
  out.shift().answer(false, { error: error });
  drainMicrotasks();
}
function tick() {
  poll();
  drainMicrotasks();
}

// ---CHECKS---

drainMicrotasks();
check(out.length === 1 && out[0].url === "/api/fx3d", "the page asks once when it opens");
tick();
check(out.length === 1, "a poll while a request is out is dropped");
answer({});
check(out.length === 0 && el("kept").textContent.indexOf("сохранены") >= 0, "idle after the answer; kept shows");

// A toggle clicked twice before the panel answers: on, then off.
el("swap").onclick();
drainMicrotasks();
check(out.length === 1 && out[0].url === "/api/fx3d?swap=1", "the first click goes at once");
el("swap").onclick();
drainMicrotasks();
check(out.length === 1, "the second click waits");
answer({ swap: true, profile: "pending" });
check(out.length === 1 && out[0].url === "/api/fx3d?swap=0", "the second click, built from the answer, switches back");
check(el("kept").textContent.indexOf("через пару секунд") >= 0, "pending shows");
answer({ profile: "pending" });

// A slider let go three times while a request is out: one request, the last
// value, even though an answer in between redraws the slider itself.
tick();
for (const v of ["3", "3.5", "4"]) {
  el("depth").value = v;
  el("depth").onchange({ target: el("depth") });
}
tick();
check(out.length === 1, "still one request out, and the poll dropped");
answer({ depthPx: 2 });
check(el("depth").value === 2, "the answer redrew the slider");
check(out.length === 1 && out[0].url === "/api/fx3d?depth=4", "one depth request, with the value let go last");
answer({ depthPx: 4 });
check(out.length === 0, "nothing more");

// A hidden tab does not poll.
document.visibilityState = "hidden";
tick();
check(out.length === 0, "no poll while the tab is hidden");
document.visibilityState = "visible";

// Two controls: in the order they were touched.
tick();
el("gl").value = "80";
el("gl").onchange({ target: el("gl") });
el("bench").onclick();
answer({});
check(out.length === 1 && out[0].url === "/api/fx3d?gl=80", "the first control first");
answer({ gainL: 80 });
check(out.length === 1 && out[0].url === "/api/fx3d?bench=1", "then the second");
el("bench").onclick();   // stop it again before the panel has answered
answer({ bench: true });
check(out.length === 1 && out[0].url === "/api/fx3d?bench=0", "the bench button, built from the answer, stops it");
answer({});

// A refusal shows its reason as an error; the queue goes on.
tick();   // a poll out, for the click to wait behind
const mono = el("modes").kids.filter(b => b.textContent === "Без очков");
check(mono.length === 1, "the mode buttons are drawn once, not piled up");
mono[0].onclick();
refuse("no PSRAM for the look");
check(el("st").className === "st err" && el("st").textContent === "no PSRAM for the look", "a refusal shows its reason");
check(out.length === 1 && out[0].url === "/api/fx3d?mode=mono", "the next request goes after a refusal");
answer({ mode: "mono" });

// The reset asks first; a no sends nothing.
globalThis.confirm = () => false;
el("reset").onclick();
drainMicrotasks();
check(out.length === 0, "a reset the owner declines sends nothing");
globalThis.confirm = () => true;

// A request that hangs is dropped after 12 s, as an error, and the one waiting goes.
el("reset").onclick();
drainMicrotasks();
check(out.length === 1 && out[0].url === "/api/fx3d?profile=reset", "the reset goes");
el("gr").value = "70";
el("gr").onchange({ target: el("gr") });
const armed = timers.filter(t => t.ms === 12000);
check(armed.length === 1, "one timeout armed, for the request out");
armed[0].fn();
drainMicrotasks();
check(el("st").className === "st err" && el("st").textContent === "панель не отвечает", "a dropped request shows as an error");
check(out.length === 2 && out[1].url === "/api/fx3d?gr=70", "the waiting request goes after the drop");
out.shift();   // the dropped one: never answered
answer({ gainR: 70, profile: "failed" });
check(el("kept").className === "hint err", "failed shows as an error");
check(el("st").className === "st", "an answer clears the error");
check(out.length === 0 && timers.length === 0, "nothing out, no timeout left armed");

print("page queue: " + checks + " checks, " + failed + " failed");

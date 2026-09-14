// The portal's Panel group: markup, style and script. Only web.cpp includes
// this, and only when CONTROL_ENCODER_ENABLED is defined.
//
//   PANEL_NAV_HTML    streamed in place of %PANEL_NAV% in PAGE_HTML's sidebar
//   PANEL_PAGES_HTML  streamed in place of %PANEL_PAGES%, after the settings form;
//                     outside it on purpose: nothing here is saved by "Save & apply",
//                     every control applies itself through /api/*
//   PANEL_CSS         served from /panel.css (cached like portal.css)
//   PANEL_JS          served from /panel.js (web_panel_js.h)
//
// Both HTML blobs go through the same %TOKEN% resolver as PAGE_HTML, so a
// percent sign followed by capitals must not appear in their text.
//
// Components are the portal's own - .card, .field, .check-row, .subcard, .note,
// .chip, .crt, .status-readout - with a handful of additions prefixed pn-.
#pragma once
#include <Arduino.h>

static const char PANEL_NAV_HTML[] PROGMEM = R"PNL(<div class="nav-group" data-need="panel">
        <div class="nav-label">Panel</div>
        <button type="button" class="nav-item" data-nav="pnow">Now showing<span class="nv-tag pn-navtag" id="pnNavTag">live</span></button>
        <button type="button" class="nav-item" data-nav="pflights" data-need="flights">Flight board</button>
        <button type="button" class="nav-item" data-nav="ptrains" data-need="trains">Rail board</button>
        <button type="button" class="nav-item" data-nav="pworld" data-need="world">World clock</button>
        <button type="button" class="nav-item" data-nav="pyachts" data-need="yachts">Yacht radar</button>
        <button type="button" class="nav-item" data-nav="plua" data-need="lua">Lua effects<span class="nv-tag">Lua</span></button>
        <button type="button" class="nav-item" data-nav="pknob">Knob</button>
      </div>)PNL";

static const char PANEL_PAGES_HTML[] PROGMEM = R"PNL(<link rel="stylesheet" href="/panel.css?v=%ASSETVER%">
      <div id="panelRoot" data-f="%PANEL_FEATURES%" hidden></div>

      <!-- NOW SHOWING -->
      <section class="page" data-page="pnow">
        <div class="page-header">
          <h1 class="page-h1">Now showing</h1>
          <p class="page-lede">What the panel is drawing right now, with every page and clock style one click away. Everything in the Panel group applies as you change it and is kept across reboots - there is nothing to save.</p>
        </div>

        <div class="card">
          <h2 class="card-title">On screen <span class="tag" id="pnMode">connecting</span></h2>
          <div class="pn-stage">
            <div class="crt oled-preview">
              <div class="oled-pv-head"><span class="ttl" id="pnTitle">--</span><span class="meta" id="pnClock">--:--</span></div>
              <div class="oled-stage"><canvas id="pnCanvas" width="128" height="64"></canvas></div>
            </div>
            <div class="status-readout pn-readout">
              <div class="sr-head"><span class="sr-led online" id="pnLed"></span><span class="sr-title" id="pnState">--</span></div>
              <dl class="sr-rows">
                <div class="sr-row"><dt>page</dt><dd id="pnPage">--</dd></div>
                <div class="sr-row"><dt>style</dt><dd id="pnStyle">--</dd></div>
                <div class="sr-row"><dt>next</dt><dd id="pnNext">--</dd></div>
                <div class="sr-row"><dt>knob</dt><dd id="pnKnob">--</dd></div>
                <div class="sr-row"><dt>refresh</dt><dd id="pnHz">--</dd></div>
              </dl>
            </div>
          </div>
          <p class="field-hint">The picture is a sketch of the page's layout drawn in your browser, not a copy of the panel's pixels.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Pages <span class="tag">knob order</span></h2>
          <div class="pn-list" id="pnPages"><p class="field-hint">Loading...</p></div>
          <div data-need="cards">
            <div class="subcard">
              <label class="check-row standalone">
                <input type="checkbox" id="pnCardsOn">
                <span class="check-box" aria-hidden="true"></span>
                <span class="check-text"><strong>Cards</strong><span class="ct-hint">Pages Home Assistant publishes on <code>nickoscope_matrix/card/&lt;name&gt;</code>. Off keeps all of them out of the knob and the carousel.</span></span>
              </label>
              <div class="pn-list pn-cards" id="pnCards"></div>
            </div>
          </div>
          <div class="note">
            <span class="note-k">knob</span>
            <div>A page switched off is skipped by the knob and by the carousel; <strong>Show</strong> still puts it on screen. The clock is where the panel falls back to, so it stays on.</div>
          </div>
        </div>

        <div class="card">
          <h2 class="card-title">Clock styles</h2>
          <div class="chip-tray pn-chips" id="pnStyles"><span class="chip-empty">Loading...</span></div>
          <p class="field-hint">Click a style to put the clock page on screen in it. It becomes the clock style, as a turn of the knob does.</p>
        </div>

        <div class="card" data-need="carousel">
          <h2 class="card-title">Carousel</h2>
          <label class="check-row standalone">
            <input type="checkbox" id="pcOn">
            <span class="check-box" aria-hidden="true"></span>
            <span class="check-text"><strong>Advance on its own</strong><span class="ct-hint">When nobody has chosen anything for a while, the panel walks through the pages by itself. Touching the knob, or showing a page from here, holds it again.</span></span>
          </label>
          <div class="grid-2" style="margin-top:16px">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="pcIdle">Quiet time before it starts</label>
              <div class="range-row">
                <input type="range" id="pcIdle" min="5" max="600" step="5" value="60">
                <span class="range-val" id="pcIdleV">60 s</span>
              </div>
              <p class="field-hint">Seconds since the last choice. Default 60.</p>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="pcSlot">Time per page</label>
              <div class="range-row">
                <input type="range" id="pcSlot" min="0" max="300" step="5" value="15">
                <span class="range-val" id="pcSlotV">15 s</span>
              </div>
              <p class="field-hint">0 lets each page keep its own: clock 25 s, world clock and rail board 20 s, the rest 15 s. A card may ask for its own time.</p>
            </div>
          </div>
          <label class="check-row standalone" style="margin-top:16px">
            <input type="checkbox" id="pcAll">
            <span class="check-box" aria-hidden="true"></span>
            <span class="check-text"><strong>Walk every clock style</strong><span class="ct-hint">On the clock page each style gets a turn of its own before the next page. Custom rotation is left out: it is a mode, not a look.</span></span>
          </label>
          <p class="field-hint" id="pcMsg"></p>
        </div>
      </section>

      <!-- FLIGHT BOARD -->
      <section class="page" data-page="pflights" data-need="flights">
        <div class="page-header">
          <h1 class="page-h1">Flight board</h1>
          <p class="page-lede">Arrivals or departures for one of six airports, published by Home Assistant over MQTT. The choice here is the knob's choice too, and it is still there after a reboot.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Airport</h2>
          <div class="grid-2">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="fbApt">Airport</label>
              <div class="select-wrap"><select id="fbApt"></select></div>
            </div>
            <div class="field" style="margin-bottom:0">
              <span class="field-label">Direction</span>
              <div class="mode-toggle pn-seg" role="group" aria-label="Direction" id="fbDir">
                <button type="button" data-v="arr">Arrivals</button>
                <button type="button" data-v="dep">Departures</button>
              </div>
            </div>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" data-show="flights"><span class="gl"></span> Show on panel</button>
          </div>
          <p class="field-hint" id="fbMsg">Applies at once. The panel resubscribes when the choice has settled, and asks Home Assistant for the board only if none is retained.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Board <span class="tag" id="fbTag">--</span></h2>
          <div class="crt pn-board">
            <div class="oled-pv-head"><span class="ttl" id="fbHead">--</span><span class="meta" id="fbAge">--</span></div>
            <div class="pn-table pn-fb" id="fbRows"></div>
          </div>
          <dl class="pn-kv" id="fbFeed"></dl>
        </div>
      </section>

      <!-- RAIL BOARD -->
      <section class="page" data-page="ptrains" data-need="trains">
        <div class="page-header">
          <h1 class="page-h1">Rail board</h1>
          <p class="page-lede">Departures and arrivals for <span id="rbStation">the station</span> from Realtime Trains. Home Assistant polls it and holds the token; the panel only listens.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Status <span class="tag" id="rbTag">--</span></h2>
          <div class="status-readout pn-readout">
            <div class="sr-head"><span class="sr-led online" id="rbLed"></span><span class="sr-title" id="rbState">--</span></div>
            <dl class="sr-rows">
              <div class="sr-row"><dt>updated</dt><dd id="rbUpd">--</dd></div>
              <div class="sr-row"><dt>departs</dt><dd id="rbDep">--</dd></div>
              <div class="sr-row"><dt>arrivals</dt><dd id="rbArr">--</dd></div>
              <div class="sr-row"><dt>home asst</dt><dd id="rbHa">--</dd></div>
              <div class="sr-row"><dt>realtime</dt><dd id="rbRt">--</dd></div>
              <div class="sr-row"><dt>mqtt</dt><dd id="rbMq">--</dd></div>
            </dl>
          </div>
          <label class="check-row standalone" style="margin-top:16px">
            <input type="checkbox" id="rbDiag">
            <span class="check-box" aria-hidden="true"></span>
            <span class="check-text"><strong>Diagnostics on the panel</strong><span class="ct-hint">The view a click and a turn of the knob give on this page. Off also clears a diagnostics setting from Home Assistant until it sends its config again.</span></span>
          </label>
          <div class="page-actions">
            <button type="button" class="btn" data-show="trains"><span class="gl"></span> Show on panel</button>
          </div>
        </div>

        <div class="card">
          <h2 class="card-title">Layout <span class="tag" id="rbFrom">--</span></h2>
          <div class="grid-2">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbPanels">Lists</label>
              <div class="select-wrap"><select id="rbPanels"><option value="2">Both, side by side</option><option value="1">One at a time</option></select></div>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbFont">Type</label>
              <div class="select-wrap"><select id="rbFont"><option value="small">Small - three services</option><option value="large">Large - two services</option></select></div>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbRows">Services per list</label>
              <div class="select-wrap"><select id="rbRows"><option value="1">1</option><option value="2">2</option><option value="3">3</option></select></div>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbSwitch">Seconds per list</label>
              <input type="number" id="rbSwitch" min="3" max="600" step="1">
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbLevel">Colour level</label>
              <div class="range-row">
                <input type="range" id="rbLevel" min="10" max="100" step="5">
                <span class="range-val" id="rbLevelV">--</span>
              </div>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="rbStale">Stale after (s)</label>
              <input type="number" id="rbStale" min="30" max="3600" step="1">
            </div>
          </div>
          <div class="page-actions">
            <button type="button" class="btn btn-accent" id="rbApply">Apply until reboot</button>
          </div>
          <div class="note warn">
            <span class="note-k">owner</span>
            <div>Home Assistant owns these settings through its retained <code>config</code> topic. What you apply here lasts until that config arrives again - on every broker reconnect - or the panel reboots. To keep a layout, change it in the Home Assistant package.</div>
          </div>
          <p class="field-hint" id="rbMsg"></p>
        </div>
      </section>

      <!-- WORLD CLOCK -->
      <section class="page" data-page="pworld" data-need="world">
        <div class="page-header">
          <h1 class="page-h1">World clock</h1>
          <p class="page-lede">A dotted map where daylight is lit and night is dim, with the time in the empty South Pacific. The home city's dot breathes.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Preview</h2>
          <div class="crt oled-preview">
            <div class="oled-pv-head"><span class="ttl">world clock</span><span class="meta" id="wcMeta">--</span></div>
            <div class="oled-stage"><canvas id="wcCanvas" width="128" height="64"></canvas></div>
          </div>
          <p class="field-hint">Drawn here from the panel's own map and city list, with the sun where the panel's clock says it is. The panel recomputes the daylight once a minute.</p>
          <div class="page-actions">
            <button type="button" class="btn" data-show="world"><span class="gl"></span> Show on panel</button>
          </div>
        </div>

        <div class="card">
          <h2 class="card-title">Home city</h2>
          <div class="pn-list pn-radio" id="wcCities"><p class="field-hint">Loading...</p></div>
          <p class="field-hint" id="wcMsg">Kept across reboots.</p>
        </div>
      </section>

      <!-- YACHT RADAR -->
      <section class="page" data-page="pyachts" data-need="yachts">
        <div class="page-header">
          <h1 class="page-h1">Yacht radar</h1>
          <p class="page-lede">Live AIS positions in the Bay of Cannes. The stream to aisstream.io is open only while the page is on the panel, so the table fills once it is showing.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Status <span class="tag" id="yrTag">--</span></h2>
          <div class="status-readout pn-readout">
            <div class="sr-head"><span class="sr-led online" id="yrLed"></span><span class="sr-title" id="yrState">--</span></div>
            <dl class="sr-rows">
              <div class="sr-row"><dt>ais key</dt><dd id="yrKey">--</dd></div>
              <div class="sr-row"><dt>stream</dt><dd id="yrStream">--</dd></div>
              <div class="sr-row"><dt>vessels</dt><dd id="yrCount">--</dd></div>
              <div class="sr-row"><dt>last fix</dt><dd id="yrAge">--</dd></div>
              <div class="sr-row"><dt>seen</dt><dd id="yrLogged">--</dd></div>
            </dl>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" data-show="yachts"><span class="gl"></span> Show on panel</button>
          </div>
        </div>

        <div class="card">
          <h2 class="card-title">Vessels</h2>
          <div class="field">
            <span class="field-label">Table order</span>
            <div class="mode-toggle pn-seg" role="group" aria-label="Table order" id="yrSort">
              <button type="button" data-v="0">Nearest</button>
              <button type="button" data-v="1">Biggest</button>
            </div>
            <p class="field-hint">The same switch a click gives on the panel. Length arrives with a vessel's static data, minutes after its first position.</p>
          </div>
          <div class="crt pn-board">
            <div class="pn-table pn-yr" id="yrRows"></div>
          </div>
        </div>
      </section>

      <!-- LUA EFFECTS -->
      <section class="page" data-page="plua" data-need="lua">
        <div class="page-header">
          <h1 class="page-h1">Lua effects</h1>
          <p class="page-lede">Effects written in Lua and run by the panel's own interpreter.</p>
        </div>
        <div class="card">
          <h2 class="card-title">Effects <span class="tag" id="luaTag">--</span></h2>
          <div class="pn-list" id="luaList"><p class="field-hint">Loading...</p></div>
        </div>
      </section>

      <!-- KNOB -->
      <section class="page" data-page="pknob">
        <div class="page-header">
          <h1 class="page-h1">Knob</h1>
          <p class="page-lede">How the rotary encoder is read. Change a value and turn the knob: the tester below shows exactly what the decoder saw.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Direction &amp; timing</h2>
          <label class="check-row standalone">
            <input type="checkbox" id="knRev">
            <span class="check-box" aria-hidden="true"></span>
            <span class="check-text"><strong>Reverse direction</strong><span class="ct-hint">Clockwise walks backwards. Default off.</span></span>
          </label>
          <div class="grid-2" style="margin-top:16px">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="knLock">Step lock-out</label>
              <div class="range-row">
                <input type="range" id="knLock" min="0" max="100" step="1">
                <span class="range-val" id="knLockV">--</span>
              </div>
              <p class="field-hint" id="knLockH">After a step the decoder ignores the knob this long. Longer drops the steps of a fast turn.</p>
            </div>
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="knDeb">Switch debounce</label>
              <div class="range-row">
                <input type="range" id="knDeb" min="2" max="100" step="1">
                <span class="range-val" id="knDebV">--</span>
              </div>
              <p class="field-hint" id="knDebH">The switch must stay put this long to count as pressed or released.</p>
            </div>
          </div>
          <div class="field" style="margin:16px 0 0">
            <label class="field-label" for="knDet">Detents</label>
            <div class="select-wrap">
              <select id="knDet">
                <option value="-1">Learn from the knob</option>
                <option value="0">One every full cycle (rest at 11)</option>
                <option value="1">One every half cycle (rest at 11 and 00)</option>
              </select>
            </div>
            <p class="field-hint" id="knDetH">--</p>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" id="knDefaults">Restore build defaults</button>
          </div>
          <p class="field-hint" id="knMsg"></p>
        </div>

        <div class="card">
          <h2 class="card-title">Tester <span class="tag" id="knTag">--</span></h2>
          <div class="crt pn-tester">
            <div class="oled-pv-head"><span class="ttl" id="knLast">turn or press the knob</span><span class="meta" id="knHeld">--</span></div>
            <div class="pn-count">
              <div><b id="knCw">0</b><span>clockwise</span></div>
              <div><b id="knCcw">0</b><span>anticlockwise</span></div>
              <div><b id="knClick">0</b><span>clicks</span></div>
              <div><b id="knLong">0</b><span>long</span></div>
            </div>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" id="knZero">Zero the counts</button>
          </div>
          <div class="note">
            <span class="note-k">detents</span>
            <div>Turn one click at a time. Each click should add exactly one step. <strong>Two per click</strong>: the knob rests only at 11 - choose one every full cycle. <strong>One every two clicks</strong>: it also rests at 00 - choose one every half cycle. Counts are the decoder's, since boot; zeroing only resets this view.</div>
          </div>
        </div>
      </section>
      <script src="/panel.js?v=%ASSETVER%"></script>)PNL";

static const char PANEL_CSS[] PROGMEM = R"CSS(.pn-navtag{color:var(--accent-d)!important;background:var(--accent-soft)!important;border-color:var(--accent-line)!important}html.pn-live .save-bar{display:none}.pn-stage{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,1fr);gap:16px;align-items:stretch}.pn-stage .oled-preview{margin-bottom:0}@media (max-width:720px){.pn-stage{grid-template-columns:1fr}}.pn-readout .sr-row{grid-template-columns:74px 1fr}.pn-readout .sr-row dd.warn{color:#ffb454;text-shadow:none}.pn-list{display:flex;flex-direction:column}.pn-row{display:flex;align-items:center;gap:12px;padding:10px 0;border-top:1px solid var(--line-soft)}.pn-row:first-child{border-top:0;padding-top:2px}.pn-row>.check-row,.pn-row>.pn-name{flex:1;min-width:0}.pn-name{font-size:14px;color:var(--ink-soft)}.pn-name strong{color:var(--ink);font-weight:600}.pn-name .ct-hint{display:block;color:var(--dim);font-size:12.5px;margin-top:2px}.pn-here{font-family:var(--mono);font-size:10px;letter-spacing:.05em;text-transform:uppercase;color:var(--accent-d);background:var(--accent-soft);border:1px solid var(--accent-line);border-radius:999px;padding:1px 8px;white-space:nowrap}.pn-row:not(.here) .pn-here{visibility:hidden}.btn-sm{padding:6px 12px;font-size:12.5px}.pn-cards{margin-top:10px}.pn-cards:empty::before{content:"No cards right now.";font-family:var(--mono);font-size:12px;color:var(--dim)}.pn-chips{margin:0}.pn-chips .chip{cursor:pointer;font-family:var(--mono);font-size:11.5px;letter-spacing:.03em;padding:6px 12px}.pn-seg button.on{background:var(--card);color:var(--ink);box-shadow:var(--shadow-card)}.pn-seg button{font-size:12px;padding:7px 14px}.pn-board,.pn-tester{padding:12px 14px}.pn-table{position:relative;z-index:1;display:grid;gap:2px 12px;font-size:12px;line-height:1.5;color:var(--crt-fg);text-shadow:0 0 6px var(--crt-glow);overflow-x:auto}.pn-fb{grid-template-columns:max-content max-content minmax(0,1fr) max-content}.pn-yr{grid-template-columns:minmax(0,1fr) max-content max-content max-content}.pn-table>span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.pn-table .h{color:var(--crt-dim);text-shadow:none;font-size:10.5px;letter-spacing:.06em;text-transform:uppercase}.pn-table .r{text-align:right}.pn-table .dim,.pn-table .m0{color:var(--crt-dim);text-shadow:none}.pn-table .m1{color:#ffb454}.pn-table .now{box-shadow:inset 2px 0 0 #ffb400;padding-left:6px}.pn-table .empty{grid-column:1/-1;color:var(--crt-dim);text-shadow:none}.st-sched{color:#c8c8c8}.st-board{color:#00dcdc}.st-dep{color:#6e9bff}.st-land{color:#00c83c}.st-delay{color:#ffaa00}.st-canc{color:#ff4b4b}.pn-kv{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px;margin:16px 0 0;font-size:13.5px}.pn-kv dt{font-family:var(--mono);font-size:11px;letter-spacing:.05em;text-transform:uppercase;color:var(--dim);padding-top:2px}.pn-kv dd{margin:0;color:var(--ink-soft)}.pn-ok{color:var(--ok)}.pn-warn{color:var(--warn)}.pn-err{color:var(--err)}.pn-radio input[type="radio"]{position:absolute;opacity:0;width:0;height:0}.pn-row input:disabled+.check-box{opacity:.5}.pn-row input:disabled~.check-text{cursor:default}.pn-radio .check-box{border-radius:999px}.pn-radio .check-row input:checked+.check-box::after{border-radius:999px;clip-path:none;width:8px;height:8px}.pn-sun{font-family:var(--mono);font-size:10.5px;letter-spacing:.04em;text-transform:uppercase;border-radius:999px;padding:1px 8px;border:1px solid var(--line);color:var(--dim);background:var(--paper-2);white-space:nowrap}.pn-sun.day{color:var(--warn);border-color:color-mix(in oklab,var(--warn) 40%,var(--line));background:color-mix(in oklab,var(--warn) 10%,var(--card))}.pn-count{position:relative;z-index:1;display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:6px 0 2px}.pn-count div{text-align:center}.pn-count b{display:block;font-size:28px;font-weight:600;line-height:1.15;color:var(--crt-fg);text-shadow:0 0 8px var(--crt-glow);font-variant-numeric:tabular-nums}.pn-count span{font-size:10px;letter-spacing:.06em;text-transform:uppercase;color:var(--crt-dim)}.pn-count b.bump{animation:pn-bump 450ms ease-out}@keyframes pn-bump{0%{color:#fff;text-shadow:0 0 14px var(--crt-glow)}100%{}}@media (prefers-reduced-motion:reduce){.pn-count b.bump{animation:none}}@media (max-width:560px){.pn-count b{font-size:22px}.pn-row{flex-wrap:wrap}})CSS";

#include "web_panel_js.h"

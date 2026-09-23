// The portal's Panel group: markup, style and script, as the source that
// tools/web_assets_gen.py gzips into web_assets.h. Nothing here is compiled.
//
//   PANEL_NAV_HTML    spliced in place of %PANEL_NAV% in PAGE_HTML's sidebar
//   PANEL_PAGES_HTML  spliced in place of %PANEL_PAGES%, after the settings form;
//                     outside it on purpose: nothing here is saved by "Save & apply",
//                     every control applies itself through /api/*
//   PANEL_CSS         served from /panel.css (cached like portal.css)
//   PANEL_JS          served from /panel.js (web_panel_js.h)
//
// The markup is in the page of every build, hidden until /api/portal lists the
// features this build carries: portal.js drops what is not listed (data-need),
// and loads PANEL_CSS and PANEL_JS only where there is a Panel group - the
// builds with CONTROL_ENCODER_ENABLED, the only ones that serve them. Nothing
// is substituted at runtime; the generator refuses a %TOKEN% in the markup.
//
// Components are the portal's own - .card, .field, .check-row, .subcard, .note,
// .chip, .crt, .status-readout - with a handful of additions prefixed pn-.
#pragma once
#include <Arduino.h>

static const char PANEL_NAV_HTML[] PROGMEM = R"PNL(<div class="nav-group" data-need="panel">
        <div class="nav-label">Panel</div>
        <button type="button" class="nav-item" data-nav="pnow">Now showing<span class="nv-tag pn-navtag" id="pnNavTag">live</span></button>
        <button type="button" class="nav-item" data-nav="pmedia" data-need="media">Media</button>
        <button type="button" class="nav-item" data-nav="pworld" data-need="world">World clock</button>
        <button type="button" class="nav-item" data-nav="pyachts" data-need="yachts">Yacht radar</button>
        <button type="button" class="nav-item" data-nav="plua" data-need="lua">Effects &amp; clips<span class="nv-tag">Lua</span></button>
        <button type="button" class="nav-item" data-nav="pknob">Knob</button>
      </div>
      <div class="nav-group" data-need="market">
        <div class="nav-label">Market</div>
        <button type="button" class="nav-item" data-nav="pmarket">Market<span class="nv-tag">HA</span></button>
      </div>)PNL";

static const char PANEL_PAGES_HTML[] PROGMEM = R"PNL(<div id="panelRoot" data-f="" hidden></div>

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

      <!-- RAIL BOARD -->

      <!-- MEDIA -->
      <section class="page" data-page="pmedia" data-need="media">
        <div class="page-header">
          <h1 class="page-h1">Media</h1>
          <p class="page-lede">Now playing on a Home Assistant or Music Assistant player, and a remote for it. Home Assistant's app follows the player chosen here and publishes what it plays; the panel draws it and sends the buttons back over MQTT. There is no sound on the panel.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Player <span class="tag" id="mpSelTag">--</span></h2>
          <div class="grid-2">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="mpPlayer">Follow</label>
              <div class="select-wrap"><select id="mpPlayer"></select></div>
              <p class="field-hint">The players Home Assistant's app allows. Kept across reboots and published to Home Assistant, which follows it.</p>
            </div>
            <div class="field" style="margin-bottom:0">
              <span class="field-label">Home Assistant follows</span>
              <p class="pn-stn" id="mpFollows">--</p>
            </div>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" data-show="media"><span class="gl"></span> Show on panel</button>
          </div>
          <p class="field-hint" id="mpSelMsg"></p>
        </div>

        <div class="card">
          <h2 class="card-title">Now playing <span class="tag" id="mpStTag">--</span></h2>
          <div class="status-readout pn-readout">
            <div class="sr-head"><span class="sr-led" id="mpLed"></span><span class="sr-title" id="mpTitle">--</span></div>
            <dl class="sr-rows">
              <div class="sr-row"><dt>artist</dt><dd id="mpArtist">--</dd></div>
              <div class="sr-row"><dt>album</dt><dd id="mpAlbum">--</dd></div>
              <div class="sr-row"><dt>source</dt><dd id="mpKind">--</dd></div>
              <div class="sr-row"><dt>position</dt><dd id="mpPos">--</dd></div>
            </dl>
          </div>
          <div class="pn-mp-bar" aria-hidden="true"><span id="mpBar"></span></div>
          <div class="page-actions">
            <button type="button" class="btn" data-mp="prev">Previous</button>
            <button type="button" class="btn btn-accent" data-mp="toggle">Play / pause</button>
            <button type="button" class="btn" data-mp="next">Next</button>
          </div>
          <div class="grid-2" style="margin-top:12px">
            <div class="field" style="margin-bottom:0">
              <label class="field-label" for="mpVol">Volume</label>
              <div class="range-row">
                <input type="range" id="mpVol" min="0" max="100" step="1">
                <span class="range-val" id="mpVolV">--</span>
              </div>
            </div>
            <div class="field" style="margin-bottom:0">
              <span class="field-label">Mute</span>
              <label class="check-row standalone">
                <input type="checkbox" id="mpMute">
                <span class="check-box" aria-hidden="true"></span>
                <span class="check-text"><strong>Muted</strong></span>
              </label>
            </div>
          </div>
          <p class="field-hint" id="mpMsg"></p>
        </div>

        <div class="card">
          <h2 class="card-title">Radio favourites <span class="tag" id="mpFavTag">--</span></h2>
          <div class="pn-list" id="mpFavs"></div>
          <p class="field-hint">Music Assistant's favourite radio stations, as Home Assistant's app sends them. On the panel: click into the page and turn the knob; the station under the pointer starts once the knob rests.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Diagnostics <span class="tag" id="mpDiagTag">--</span></h2>
          <div class="status-readout pn-readout">
            <dl class="sr-rows">
              <div class="sr-row"><dt>mqtt</dt><dd id="mpMq">--</dd></div>
              <div class="sr-row"><dt>ha app</dt><dd id="mpBridge">--</dd></div>
              <div class="sr-row"><dt>state age</dt><dd id="mpAge">--</dd></div>
              <div class="sr-row"><dt>player</dt><dd id="mpAvail">--</dd></div>
              <div class="sr-row"><dt>players</dt><dd id="mpPlayers">--</dd></div>
              <div class="sr-row"><dt>selection</dt><dd id="mpSelSent">--</dd></div>
              <div class="sr-row"><dt>commands</dt><dd id="mpCmds">--</dd></div>
              <div class="sr-row"><dt>refused</dt><dd id="mpRefused">--</dd></div>
              <div class="sr-row"><dt>topics</dt><dd id="mpTopics">--</dd></div>
            </dl>
          </div>
        </div>
      </section>

      <!-- MARKET (src/market: a page of its own; the controls come from /api/market's registry) -->
      <section class="page" data-page="pmarket" data-need="market">
        <div class="page-header">
          <h1 class="page-h1">Market</h1>
          <p class="page-lede">Indices, tickers and a backtested portfolio on four panel pages, computed by Home Assistant's app from Yahoo Finance and drawn here from what it publishes. Save publishes the settings; the app recomputes and the panel follows.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Home Assistant <span class="tag" id="mkStTag">--</span></h2>
          <div class="status-readout pn-readout">
            <div class="sr-head"><span class="sr-led" id="mkLed"></span><span class="sr-title" id="mkTitle">--</span></div>
            <dl class="sr-rows">
              <div class="sr-row"><dt>as of</dt><dd id="mkAsof">--</dd></div>
              <div class="sr-row"><dt>fetched</dt><dd id="mkFetched">--</dd></div>
              <div class="sr-row"><dt>window</dt><dd id="mkView">--</dd></div>
              <div class="sr-row"><dt>hold</dt><dd id="mkHold">--</dd></div>
              <div class="sr-row"><dt>rebal</dt><dd id="mkRebal">--</dd></div>
            </dl>
          </div>
          <div class="page-actions">
            <button type="button" class="btn" data-mkshow="markets">Markets</button>
            <button type="button" class="btn" data-mkshow="ticker">Ticker</button>
            <button type="button" class="btn" data-mkshow="portfolio">Portfolio</button>
            <button type="button" class="btn" data-mkshow="holdings">Holdings</button>
          </div>
          <p class="field-hint">Puts that page on the panel now, as a knob turn would.</p>
        </div>

        <div id="mkCore"></div>

        <div class="card">
          <div class="page-actions" style="margin-top:0">
            <button type="button" class="btn btn-accent" id="mkSave"><span class="gl"></span> Save &amp; publish</button>
            <button type="button" class="btn" id="mkRevert">Revert</button>
            <span class="field-hint" id="mkMsg" style="margin:0;align-self:center"></span>
          </div>
          <p class="note plain"><span class="note-k">Note</span>A hypothetical backtest: prices net of the funds' fees, no taxes, no commissions, dividends and contributions as cash until each 31 December. The data is Yahoo Finance's, as the app fetched it.</p>
        </div>

        <h2 class="card-title" style="margin:6px 0 10px">Advanced <span class="tag">folded</span></h2>
        <div id="mkAdv"></div>

        <div class="card">
          <h2 class="card-title">Diagnostics <span class="tag" id="mkDiagTag">--</span></h2>
          <div class="crt pn-board" style="margin-bottom:12px"><div class="mk-diag" id="mkSymbols"></div></div>
          <div class="status-readout pn-readout">
            <dl class="sr-rows">
              <div class="sr-row"><dt>mqtt</dt><dd id="mkMq">--</dd></div>
              <div class="sr-row"><dt>ha app</dt><dd id="mkBridge">--</dd></div>
              <div class="sr-row"><dt>received</dt><dd id="mkRx">--</dd></div>
              <div class="sr-row"><dt>config</dt><dd id="mkCfg">--</dd></div>
              <div class="sr-row"><dt>settings</dt><dd id="mkNvs">--</dd></div>
              <div class="sr-row"><dt>flash copy</dt><dd id="mkFs">--</dd></div>
              <div class="sr-row"><dt>windows</dt><dd id="mkWindows">--</dd></div>
              <div class="sr-row"><dt>topics</dt><dd id="mkTopics">--</dd></div>
            </dl>
          </div>
        </div>
      </section>

      <!-- WORLD CLOCK -->
      <section class="page" data-page="pworld" data-need="world">
        <div class="page-header">
          <h1 class="page-h1">World clock</h1>
          <p class="page-lede">A dotted map where daylight is lit and night is dim. The big time is the home city's, in its own time zone, with its name beside it; when home changes, the name pulses with the home dot for a few seconds.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Preview</h2>
          <div class="crt oled-preview">
            <div class="oled-pv-head"><span class="ttl">world clock</span><span class="meta" id="wcMeta">--</span></div>
            <div class="oled-stage"><canvas id="wcCanvas" width="128" height="64"></canvas></div>
          </div>
          <p class="field-hint">Drawn here from the panel's own map, cities and home time, with the sun where the panel's clock says it is. The panel recomputes the daylight once a minute.</p>
          <div class="page-actions">
            <button type="button" class="btn" data-show="world"><span class="gl"></span> Show on panel</button>
          </div>
        </div>

        <div class="card">
          <h2 class="card-title">Home city</h2>
          <p class="pn-stn" id="wcFrom">--</p>
          <div class="page-actions">
            <button type="button" class="btn btn-sm" id="wcFollow" hidden>Follow the panel's location</button>
          </div>
          <div class="pn-list pn-radio" id="wcCities"><p class="field-hint">Loading...</p></div>
          <p class="field-hint" id="wcMsg">Choosing a city makes it home at once, shows it on the panel, and is kept across reboots.</p>
        </div>

        <div class="card">
          <h2 class="card-title">Add a city <span class="tag" id="wcCount">--</span></h2>
          <div class="field">
            <label class="field-label" for="wcFind">Find a city</label>
            <input type="search" id="wcFind" autocomplete="off" spellcheck="false" placeholder="Tokyo, Sao Paulo, Moskva">
            <p class="field-hint" id="wcFindMsg">Type two letters or more, in any language. This browser asks Open-Meteo; the panel hears only the city you add.</p>
          </div>
          <div class="pn-list" id="wcResults"></div>
          <div id="wcAddForm" hidden>
            <div class="field">
              <label class="field-label" for="wcName">Name on the panel</label>
              <input type="text" id="wcName" class="pn-crs pn-wcname" maxlength="20" autocomplete="off" spellcheck="false" autocapitalize="characters" aria-describedby="wcNameMsg">
              <p class="field-hint" id="wcNameMsg">--</p>
            </div>
            <div class="page-actions">
              <button type="button" class="btn btn-accent" id="wcAddHome">Add and make home</button>
              <button type="button" class="btn" id="wcAdd">Add</button>
            </div>
          </div>
          <p class="field-hint">Up to six cities of your own; the six built-in ones always stay. Search by <a href="https://open-meteo.com/" target="_blank" rel="noopener">Open-Meteo</a> (CC BY 4.0), location data based on GeoNames.</p>
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
          <h1 class="page-h1">Effects &amp; clips</h1>
          <p class="page-lede">Effects written in Lua and run by the panel's own interpreter, and clips stored on the panel.</p>
        </div>
        <div class="card">
          <h2 class="card-title">Effects <span class="tag" id="luaTag">--</span></h2>
          <p class="field-hint">A ticked effect is one the knob and the carousel visit; an unticked one is left out of both and still shows with Show. Delete removes an effect you uploaded; the built-in ones stay.</p>
          <div class="pn-list" id="luaList"><p class="field-hint">Loading...</p></div>
          <p class="field-hint" id="luaMsg"></p>
        </div>
        <div class="card">
          <h2 class="card-title">Add from the gallery <span class="tag" id="galTag">GitHub</span></h2>
          <p class="field-hint">Effects published in the project's gallery on GitHub. This browser fetches the list and the script you pick, and sends it to the panel; the panel itself does not go to the internet for it.</p>
          <div class="pn-list" id="galList"><p class="field-hint">Loading the gallery...</p></div>
          <p class="field-hint" id="galMsg"></p>
        </div>
        <div class="card">
          <h2 class="card-title">Clips <span class="tag" id="clipTag">--</span></h2>
          <p class="field-hint">Pre-rendered animations kept on the panel, the oscilloscope music clips among them. A clip plays in place of the clock; turning the knob or choosing any page ends it.</p>
          <p class="field-hint" id="clipCard"></p>
          <div class="pn-list" id="clipList"><p class="field-hint">Loading...</p></div>
          <div class="page-actions"><button type="button" class="btn btn-sm" id="clipStop">Stop clip</button><button type="button" class="btn btn-sm" id="clipMount" style="display:none">Look for the card again</button></div>
        </div>
        <div class="card">
          <h2 class="card-title">Make a clip <span class="tag">from sound</span></h2>
          <p class="field-hint">Oscilloscope music is sound made to be seen: the left channel moves the beam across, the right one up and down. Pick a track, or any audio or video file, and this page draws it the way a scope would and stores it as a clip. It all happens in this browser: the file stays on the phone, and only the finished clip goes to the panel.</p>
          <div class="field" style="margin-top:16px">
            <label class="field-label" for="cmFile">Sound</label>
            <input type="file" id="cmFile" accept="audio/*,video/*,.wav,.flac,.mp3,.m4a,.mp4,.mov,.webm">
            <p class="field-hint" id="cmInfo">A WAV is read a few seconds at a time, so a 350 MB track is fine. Anything else is decoded whole, in memory.</p>
            <p class="field-hint pn-warn" id="cmFlat"></p>
            <div class="page-actions" id="cmCap" style="display:none"><button type="button" class="btn btn-sm" data-v="0">Record the microphone</button><button type="button" class="btn btn-sm" data-v="1">Record a tab or the screen</button></div>
            <p class="field-hint" id="cmCapH"></p>
          </div>
          <div id="cmBody" hidden>
            <div class="crt oled-preview">
              <div class="oled-pv-head"><span class="ttl" id="cmPvT">preview</span><span class="meta" id="cmPvM">--</span></div>
              <div class="oled-stage"><canvas id="cmCanvas" width="128" height="64"></canvas></div>
            </div>
            <div class="field">
              <label class="field-label" for="cmStart">Start, seconds</label>
              <div class="range-row"><input type="range" id="cmAt" min="0" max="1" step="0.1" value="0" aria-label="Start"><input type="number" id="cmStart" min="0" step="0.1" value="0" inputmode="decimal" style="width:6.5em;flex:none"></div>
            </div>
            <div class="grid-2">
              <div class="field"><label class="field-label" for="cmDur">Length, seconds</label><div class="range-row"><input type="number" id="cmDur" min="1" step="0.5" value="14" inputmode="decimal"><button type="button" class="btn btn-sm" id="cmAll" style="flex:none">Whole track</button></div></div>
              <div class="field"><label class="field-label" for="cmFps">Frame rate</label><div class="select-wrap"><select id="cmFps"><option value="25">25 fps</option><option value="20">20 fps, longer in the same room</option></select></div></div>
              <div class="field"><label class="field-label" for="cmCol">Phosphor</label><div class="select-wrap"><select id="cmCol"><option value="0">Green</option><option value="1">Amber</option><option value="2">Blue</option><option value="3">White</option></select></div></div>
              <div class="field"><label class="field-label" for="cmName">Clip name</label><input type="text" id="cmName" maxlength="24" autocapitalize="off" autocomplete="off" spellcheck="false" placeholder="A-Z a-z 0-9 _ -"></div>
              <div class="field"><label class="field-label" for="cmGain">Brightness</label><div class="range-row"><input type="range" id="cmGain" min="-2" max="2" step="0.25" value="0"><span class="range-val" id="cmGainV">x1.00</span></div></div>
              <div class="field"><label class="field-label" for="cmDecay">Persistence</label><div class="range-row"><input type="range" id="cmDecay" min="0.2" max="0.9" step="0.05" value="0.55"><span class="range-val" id="cmDecayV">0.55</span></div></div>
              <div class="field"><label class="field-label" for="cmZoom">Zoom</label><div class="range-row"><input type="range" id="cmZoom" min="1" max="1.6" step="0.05" value="1"><span class="range-val" id="cmZoomV">x1.00</span></div></div>
            </div>
            <p class="field-hint" id="cmSize"></p>
            <div class="pn-list" id="cmRoom"></div>
            <div class="ota-progress" id="cmProg"><div class="ota-bar"><i id="cmBar"></i></div><div class="ota-pct" id="cmPct"></div></div>
            <div class="page-actions">
              <button type="button" class="btn" id="cmPlay">Preview 4 s</button>
              <button type="button" class="btn btn-accent" id="cmGo">Make the clip</button>
              <button type="button" class="btn" id="cmShow" style="display:none">Play</button>
            </div>
            <p class="field-hint" id="cmMsg"></p>
          </div>
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
      </section>)PNL";

static const char PANEL_CSS[] PROGMEM = R"CSS(.pn-navtag{color:var(--accent-d)!important;background:var(--accent-soft)!important;border-color:var(--accent-line)!important}html.pn-live .save-bar{display:none}.pn-stage{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,1fr);gap:16px;align-items:stretch}.pn-stage .oled-preview{margin-bottom:0}@media (max-width:720px){.pn-stage{grid-template-columns:1fr}}.pn-readout .sr-row{grid-template-columns:74px 1fr}.pn-readout .sr-row dd.warn{color:#ffb454;text-shadow:none}.pn-list{display:flex;flex-direction:column}.pn-row{display:flex;align-items:center;gap:12px;padding:10px 0;border-top:1px solid var(--line-soft)}.pn-row:first-child{border-top:0;padding-top:2px}.pn-row>.check-row,.pn-row>.pn-name{flex:1;min-width:0}.pn-name{font-size:14px;color:var(--ink-soft)}.pn-name strong{color:var(--ink);font-weight:600}.pn-name .ct-hint{display:block;color:var(--dim);font-size:12.5px;margin-top:2px}.pn-here{font-family:var(--mono);font-size:10px;letter-spacing:.05em;text-transform:uppercase;color:var(--accent-d);background:var(--accent-soft);border:1px solid var(--accent-line);border-radius:999px;padding:1px 8px;white-space:nowrap}.pn-row:not(.here) .pn-here{visibility:hidden}.pn-row.pn-sub{padding:6px 0 6px 30px;border-top:0}.pn-row.pn-sub .pn-name{font-size:13.5px}.btn-sm{padding:6px 12px;font-size:12.5px}.pn-cards{margin-top:10px}.pn-cards:empty::before{content:"No cards right now.";font-family:var(--mono);font-size:12px;color:var(--dim)}.pn-chips{margin:0}.pn-chips .chip{cursor:pointer;font-family:var(--mono);font-size:11.5px;letter-spacing:.03em;padding:6px 12px}.pn-seg button.on{background:var(--card);color:var(--ink);box-shadow:var(--shadow-card)}.pn-seg button{font-size:12px;padding:7px 14px}.pn-board,.pn-tester{padding:12px 14px}.pn-table{position:relative;z-index:1;display:grid;gap:2px 12px;font-size:12px;line-height:1.5;color:var(--crt-fg);text-shadow:0 0 6px var(--crt-glow);overflow-x:auto}.pn-fb{grid-template-columns:max-content max-content minmax(0,1fr) max-content}.pn-yr{grid-template-columns:minmax(0,1fr) max-content max-content max-content}.pn-table>span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.pn-table .h{color:var(--crt-dim);text-shadow:none;font-size:10.5px;letter-spacing:.06em;text-transform:uppercase}.pn-table .r{text-align:right}.pn-table .dim,.pn-table .m0{color:var(--crt-dim);text-shadow:none}.pn-table .m1{color:#ffb454}.pn-table .now{box-shadow:inset 2px 0 0 #ffb400;padding-left:6px}.pn-table .empty{grid-column:1/-1;color:var(--crt-dim);text-shadow:none}.st-sched{color:#c8c8c8}.st-board{color:#00dcdc}.st-dep{color:#6e9bff}.st-land{color:#00c83c}.st-delay{color:#ffaa00}.st-canc{color:#ff4b4b}.pn-kv{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px;margin:16px 0 0;font-size:13.5px}.pn-kv dt{font-family:var(--mono);font-size:11px;letter-spacing:.05em;text-transform:uppercase;color:var(--dim);padding-top:2px}.pn-kv dd{margin:0;color:var(--ink-soft)}.pn-ok{color:var(--ok)}.pn-warn{color:var(--warn)}.pn-err{color:var(--err)}.pn-radio input[type="radio"]{position:absolute;opacity:0;width:0;height:0}.pn-row input:disabled+.check-box{opacity:.5}.pn-row input:disabled~.check-text{cursor:default}.pn-radio .check-box{border-radius:999px}.pn-radio .check-row input:checked+.check-box::after{border-radius:999px;clip-path:none;width:8px;height:8px}.pn-sun{font-family:var(--mono);font-size:10.5px;letter-spacing:.04em;text-transform:uppercase;border-radius:999px;padding:1px 8px;border:1px solid var(--line);color:var(--dim);background:var(--paper-2);white-space:nowrap}.pn-sun.day{color:var(--warn);border-color:color-mix(in oklab,var(--warn) 40%,var(--line));background:color-mix(in oklab,var(--warn) 10%,var(--card))}.pn-count{position:relative;z-index:1;display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:6px 0 2px}.pn-count div{text-align:center}.pn-count b{display:block;font-size:28px;font-weight:600;line-height:1.15;color:var(--crt-fg);text-shadow:0 0 8px var(--crt-glow);font-variant-numeric:tabular-nums}.pn-count span{font-size:10px;letter-spacing:.06em;text-transform:uppercase;color:var(--crt-dim)}.pn-count b.bump{animation:pn-bump 450ms ease-out}@keyframes pn-bump{0%{color:#fff;text-shadow:0 0 14px var(--crt-glow)}100%{}}@media (prefers-reduced-motion:reduce){.pn-count b.bump{animation:none}}.pn-crs{font-family:var(--mono);text-transform:uppercase;letter-spacing:.14em;max-width:9em}.pn-stn{margin:8px 0 0;font-size:15px;color:var(--ink)}.pn-stn small{display:block;font-size:12.5px;color:var(--dim);margin-top:2px}@media (max-width:560px){.pn-count b{font-size:22px}.pn-row{flex-wrap:wrap}}.pn-wcname{letter-spacing:.08em;max-width:16em}#wcResults:not(:empty){margin:4px 0 12px}#wcFind{width:100%}.pn-colors{display:flex;flex-wrap:wrap;gap:4px}.pn-sw{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:6px;vertical-align:-1px;box-shadow:0 0 0 1px rgba(0,0,0,.25)}.pn-crs{font-family:var(--mono);text-transform:uppercase;letter-spacing:.14em;max-width:9em}.pn-stn{margin:8px 0 0;font-size:15px;color:var(--ink)}.pn-stn small{display:block;font-size:12.5px;color:var(--dim);margin-top:2px}@media (max-width:560px){.pn-count b{font-size:22px}.pn-row{flex-wrap:wrap}}.pn-thumb{width:64px;height:32px;flex:none;image-rendering:pixelated;background:#000;border-radius:3px}.pn-mp-bar{height:6px;margin-top:14px;border-radius:3px;background:var(--line-soft);overflow:hidden}.pn-mp-bar span{display:block;height:100%;width:0;background:var(--accent-d);transition:width .4s linear}.mk-fold summary{cursor:pointer;display:flex;gap:10px;align-items:center;list-style:none;font-size:15px;font-weight:600;color:var(--ink)}.mk-fold summary::-webkit-details-marker{display:none}.mk-fold summary::before{content:"";width:7px;height:7px;border-right:1.5px solid var(--mute);border-bottom:1.5px solid var(--mute);transform:rotate(-45deg);transition:transform 160ms ease;flex:none}.mk-fold[open] summary::before{transform:rotate(45deg)}.mk-fold-sum{margin-left:auto;font-family:var(--mono);font-size:11.5px;font-weight:500;color:var(--dim);text-align:right;max-width:60%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.mk-fold[open] .mk-fold-sum{display:none}.mk-fold-body{margin-top:16px}.mk-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px 18px}@media (max-width:560px){.mk-grid{grid-template-columns:1fr}}.mk-grid .field{margin-bottom:0}.mk-grid .full{grid-column:1/-1}.mk-unit{font-family:var(--mono);font-size:12px;color:var(--dim);margin-left:8px}.mk-pos{display:grid;grid-template-columns:minmax(0,1fr) 96px 30px 30px;gap:8px;align-items:center;padding:6px 0;border-top:1px solid var(--line-soft)}.mk-pos.head{border-top:0;padding:0 0 4px;font-family:var(--mono);font-size:10.5px;letter-spacing:.05em;text-transform:uppercase;color:var(--dim)}.mk-pos input,.mk-pos select,.mk-more-row input,.mk-more-row select,.mk-addrow input,.mk-addrow select{font-size:13px;padding:7px 10px;max-width:none}.mk-pos .sym{font-family:var(--mono);text-transform:uppercase}.mk-more-row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;padding:2px 0 8px}.mk-more-row[hidden]{display:none}.mk-sum{height:8px;border-radius:4px;background:var(--inset);border:1px solid var(--line);overflow:hidden;margin:12px 0 6px}.mk-sum span{display:block;height:100%;width:0;background:var(--accent);transition:width .2s}.mk-sum.over span{background:var(--err)}.mk-sumtxt{font-family:var(--mono);font-size:12px;color:var(--dim)}.mk-sumtxt.over{color:var(--err)}.mk-bad{border-color:var(--err)!important;box-shadow:0 0 0 3px color-mix(in oklab,var(--err) 18%,transparent)!important}.mk-x{border:0;background:transparent;color:var(--dim);cursor:pointer;font-size:18px;line-height:1;padding:0}.mk-x:hover{color:var(--err)}.mk-more{border:0;background:transparent;color:var(--accent-d);cursor:pointer;font-size:12px;padding:0}.mk-reset{display:inline-block;margin-top:14px;font-size:12.5px;color:var(--accent-d);cursor:pointer;background:0;border:0;padding:0;font-family:inherit}.mk-reset:hover{text-decoration:underline}.mk-addrow{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap;align-items:center}.mk-addrow input{width:auto;max-width:170px}.mk-chips .chip{cursor:default;gap:4px}.mk-chips .chip .cn{font-family:var(--mono)}.mk-diag{position:relative;z-index:1;display:grid;grid-template-columns:max-content max-content max-content max-content minmax(0,1fr);gap:2px 14px;font-size:12px;color:var(--crt-fg);text-shadow:0 0 6px var(--crt-glow);overflow-x:auto}.mk-diag span{white-space:nowrap}.mk-diag .h{color:var(--crt-dim);text-shadow:none;font-size:10.5px;letter-spacing:.06em;text-transform:uppercase}.mk-diag .dim{color:var(--crt-dim);text-shadow:none}.mk-diag .warn{color:#ffb454}.mk-diag .empty{grid-column:1/-1;color:var(--crt-dim);text-shadow:none}textarea.mk-json{width:100%;max-width:520px;min-height:64px;font-family:var(--mono);font-size:12.5px;background:var(--card);color:var(--ink);border:1px solid var(--line-2);border-radius:var(--r-md);padding:10px 12px;resize:vertical}.mk-presets{display:flex;gap:6px;margin-top:8px})CSS";

#include "web_panel_js.h"

# matrix_market: the market dashboard's Home Assistant side

An AppDaemon app that fetches Yahoo Finance history, keeps a store, runs the
portfolio ledger (`tools/market/market_ref.py`, the maths written once), and
publishes the ready-to-draw payloads the LED panel shows over MQTT, plus the
Home Assistant sensors. The design is `docs/18-stock-dashboard.md` in the
knowledge base; the settings inventory is `docs/19-market-dashboard-research.md`.
Everything here ran on a laptop against saved samples and fakes; **nothing has
run inside AppDaemon yet** (see "Not verified").

## Install (the maintainer)

Paths as the AppDaemon add-on's container sees them: the add-on maps its
`addon_config` folder to `/config` (its `config.yaml`, `map: addon_config:rw`;
its init script works on `/config/appdaemon.yaml`), which the host shows as
`/addon_configs/<slug>_appdaemon/`.

1. Copy the package directory `tools/market/appdaemon/matrix_market/` whole to
   `/config/apps/matrix_market/`, and `tools/market/market_ref.py` into that same
   directory (the package finds it there first; `matrix_market/_ref.py`).
2. The add-on's `python_packages` option: `paho-mqtt`. Optional: `yfinance` (its
   crumb handling for the expense ratio; without it the app uses Yahoo's own
   `getcrumb` + `quoteSummary`), `exchange_calendars` (exchange holidays for the
   tape; without it the tape uses Yahoo's session hours and `currentTradingPeriod`,
   which already points at the next session on a holiday). `pandas` only with
   `exchange_calendars`. Restart the add-on after changing the option.
3. Append `apps_market.yaml`'s entry to `/config/apps/apps.yaml`. It has two
   placeholders the app refuses to start with: `device: "<panel-id>"` (the panel's
   six hex digits, on its portal under Market > Diagnostics) and
   `mqtt_host: "<your-broker>"` (the broker by IP: the container has no mDNS). Set
   them there, through `!secret`, or in the local override below. The broker login
   is `!secret mqtt_user` / `mqtt_pass` from AppDaemon's `secrets.yaml`. The store is
   `store_dir: /config/market`.
   **The local override** holds what must not be in this public repository: the
   real allocation and, if wanted, the private wiring (`device`, `mqtt_host`,
   `mqtt_port`, `topic_base`). It is JSON at `/config/market/local.json` (the
   `local_file` setting; `""` for none) and it must **never** go under `apps/`:
   AppDaemon reads every `.yaml` and `.toml` in the apps tree as app configuration
   and raises `BadAppConfig` on a key that is not an app, printing it to its log
   (`app_management.get_app_config_files`, read through deepwiki). The app lays it
   over `apps_market.yaml`'s settings at start and logs `local override read from
   local.json`, `none at ...`, or `refused: ...` (its name, never its content).
   Without it the app runs the committed neutral example: the four indices and a
   60/40 VFINX/VBMFX pair. The panel's `config` payload lays over both. The shape:
   ```json
   {"device": "a1b2c3", "mqtt_host": "192.0.2.10",
    "portfolio": {"positions": [{"sym": "VFINX", "w": 60, "asset_class": "equity"},
                                {"sym": "VBMFX", "w": 40, "asset_class": "bond"}]},
    "tickers": [{"sym": "VFINX", "name": "VFINX"}], "ticker": "VFINX"}
   ```
   On the Mac the owner's copy is `tools/market/store/local.json`, gitignored
   (`**/market/**/local.json`, `**/market/store/`).
4. Watch `appdaemon.log` for `market: v..., device ..., features ...` and then
   `market: start: N payloads, largest ... B`. The first start fetches every
   watched symbol at one request per 10 s and the ECB tables; the topics appear
   under `nickoscope_matrix/<panel-id>/market/#`.
5. Home Assistant: the sensors appear by MQTT discovery under the device
   "Matrix Market". The Lovelace view for the dashboard `stock-market` is
   generated from the effective settings, so it lists the real holdings without
   committing them: `cd tools/market/appdaemon && python3 -m matrix_market.cli
   --local ../store/local.json view > /tmp/portfolio_view.json`, then Edit
   dashboard, Raw configuration editor, append under `views`. The committed
   `tools/market/ha/portfolio_view.json` is the same view on the neutral example.

**The AppDaemon loading rule** (AppDaemon APPGUIDE, "AppDir Structure" and "App
Packages", read through context7 `/appdaemon/appdaemon`, 2026-09-15): AppDaemon
searches every subdirectory of the apps directory for apps and yaml files; a
directory with an `__init__.py` is a Python package, and `apps.yaml` then names
the module with the package prefix, `module: matrix_market.app`; inside the
package the modules import each other relatively (`from .store import Store`).
Scheduler callbacks take `(self, **kwargs)`; this app's timers only enqueue on
its worker. The HA best-practice guide (`references/appdaemon.md`) says the same:
"`__init__.py` required when using dotted module paths".

## The module map

```
matrix_market/
  __init__.py     SCHEMA_V (payload v = 1), CONFIG_V (config v = 2), the version
  app.py          MatrixMarket(hass.Hass): wires the features apps.yaml lists, one worker thread,
                  the config subscription, generations (`gen`, status last)
  config.py       the declarative settings schema (SCHEMA, APP_SCHEMA): validation, defaults, merging
  registry.py     Registry and the tables: FEATURES PROVIDERS MODES METRICS WINDOWS LINES EXCHANGES TIMELINES
  features.py     Feature (start/stop/on_config/contribute/status) and the shared Context (bus, scheduler)
  providers/
    base.py       Provider interface, Quote/Intraday/FundInfo, RateLimited/NotFound, Pacer, Backoff
    httpio.py     the one HTTP door (urllib, cookie jar, the user_agent setting, 429 -> RateLimited and a log line
                  with the User-Agent and the body's first bytes); fakeable. Not http.py: AppDaemon's legacy
                  import method puts the apps tree on sys.path, where http.py would shadow the stdlib
    yahoo.py      chart v8 history, spark quotes, 1-minute intraday, crumb + quoteSummary (yfinance optional)
    ecb.py        EUR/USD EXR.M.USD.EUR.SP00.E through market_ref's table (one FX path with the reference) and
                  ICP.M.U2.N.000000.4.INX (HICP), cached in the store
    fixtures.py   the saved samples as a provider (tests, cli --offline)
  store.py        one JSON per symbol (dates, closes, adj, dividends, splits, meta, TER), manifest, tables
  timeline.py     the trading-day timeline: union of the traded series' days (default) or a calendar
  windows.py      WINDOWS (YTD 1Y 3Y 5Y 10Y MAX + 1M 3M 6M MTD WTD) and METRICS (chg hi lo cagr mdd ann)
  encode.py       the 128-point uint16 base64 line, the decoder, the round-trip check
  ledger.py       the adapter over market_ref: Rules from the settings, both modes, benchmarks, attribution,
                  the portfolio / holdings / stats payloads
  stats.py        drawdowns with dates, volatility, Sharpe, Sortino, Calmar, years, rolling (quantstats' formulas, monthly)
  publisher.py    topics, retained, the 1 900 B guard with per-topic shrink rules, generations
  history.py      feature: the daily fetch, TER, ECB tables, index/ticker payloads
  portfolio.py    feature: the ledger, portfolio/<mode>/<preset>, holdings/<mode>, stats/<mode>/<preset>
  live.py         feature: spark quotes and the selected ticker's intraday line while an exchange is open
  tape.py         feature: exchange states (Yahoo periods > exchange_calendars > weekday rule)
  ha_entities.py  feature: MQTT discovery of the sensors and the alert binary sensors
  cli.py          fetch / compute / publish --dry-run / report / schema
test_matrix_market.py   the tests (fake AppDaemon, fake broker, fake HTTP, the samples)
apps_market.yaml        the apps.yaml entry: placeholders and the neutral example (the real values: local.json)
```

`tools/market/market_ref.py` is the maths. The app adds to it only additively:
`parse_yahoo_chart` / `parse_yahoo_ter`, `Series.adj`, `Rules`, `LedgerExt`,
`simulate_ext` (identical to `simulate()` with the default rules, tested),
`twr`, `xirr`, `annualised`, `period_ends`, `check_days`, `band_bounds`.

## How to add things

- **A setting:** one `Setting(...)` row in `config.SCHEMA` (key, type, default,
  bounds, group, since, doc). Validation, defaults, `Settings.get()`, the
  `cfgErr` in status and the table below follow from it. A new kind of value is
  one checker in `config.TYPES`.
- **A provider:** a class implementing `providers.base.Provider` decorated
  `@PROVIDERS.register("name")`; `apps.yaml: provider: name`.
- **A feature:** a `Feature` subclass with `start/stop/on_config` and, when it
  publishes daily payloads, `contribute(snapshot)`; `@FEATURES.register("name")`
  and the name in `features:`. Features share the `Context` (settings, store,
  provider, publisher, `data`, the `on/emit` bus).
- **A metric:** `@METRICS.register("name")` on `fn(wd, wv, ctx)`; it appears in
  every index, ticker and portfolio payload.
- **A window preset:** `WINDOWS.register("2Y", fn(asof, inception, days))`.
- **A portfolio mode / year-end rule:** `MODES.register("name", run(...))`; the
  two of the design call `market_ref.simulate_ext`, whose `Rules` carry the
  calendar, bands, dividends, flows, costs. `rebal.mode` on the panel picks the
  model shown (`hold`, or REBAL's rule); REBAL is always computed, as the approved
  calendar when the panel shows HOLD.
- **The 60/40 blend** is one constant, `config.BLEND_60_40`: VFINX 60 + VBMFX 40 on
  adjclose (the coordinator's probe of 2026-09-15: both from 2000-01-01; AGG, BND
  and VBTLX start later). Mutual funds are never in the live spark poll.
- **A line on PORTFOLIO:** `@LINES.register("name")` on `fn(result, mode)`.
- **An exchange on the tape:** `EXCHANGES.register("NAME", {"cal", "yahoo", "tz"})`.

## The MQTT contract, under `nickoscope_matrix/<dev>/market/`

Exactly docs/18's topics and fields as the panel implements them (wip/market-panel,
`src/market/market_model.cpp` reads only the fields listed there; the extras below are
ignored by it and read by Home Assistant), plus the additions of docs/18 "Settings
after the research" (marked v2). Every payload carries `v: 1` and `gen` (the
generation); `status` goes last with the same `gen`. Retained; every one under
1 900 B. The panel refuses a payload whole on a wrong type, on a `sym`/`preset`/`mode`
that disagrees with the topic, on an unwatched symbol and on a line that is not 128
points; the statistics go to `market_stats/`, outside its `market/#` subscription, so they are never counted
as unknown topics there (audit MINOR 1).

| Topic | Fields |
|---|---|
| `status` | `v asof fetched state(ok/stale/error) err symbols{sym:{asof bars ter terSrc err}} gen ver` + `cfgErr[{key code}]`, `pfErr`, `realErr`, `benchErr`, `xirrFail`, `backoff`, `refused` when they apply |
| `index/<sym>/<preset>`, `ticker/<sym>/<preset>` | `v sym name cur preset from to last chg hi lo n min max pts cagr mdd` + v2 `ann` |
| `portfolio/<mode>/<preset>` | `v cur mode preset from to value chg sinceStart cagr mdd div terDrag cash n min max pts px bench [gross] [tell]` + v2 `twr ann fx_effect measure mdd_peak mdd_trough mdd_recovery` (`flows: true` and `xirr_ann` only with flows, `xirr_ann` null only when the solver finds no rate, counted in `status.xirrFail`; `px` always, the panel requires it; `real: true`, `scale: "log"` only when on) |
| `holdings/<mode>` | `v asof mode rows[{sym tgt now ret entry}] cash{tgt now}` + v2 per row `cls`, `lo hi` (the Swedroe band, %), `out` (when outside it); `rebal` (count), `classes{}`; `now` and `ret` rounded to 4 decimals |
| `nickoscope_matrix/<panel-id>/market_stats/<mode>/<preset>` (v2, outside `market/#`, for Home Assistant) | `twr ann flows xirr_ann fx_effect mdd{mdd peak trough recovery} mddME ddNow vol sharpe sortino calmar topDD[5] bestY worstY roll1y roll3y bench[<=3 {sym chg ann}] contrib[[sym value] top first] outOfBand[] classes{} rebal trades [costs tax withdrawn] real`; shrink: topDD, contrib, roll3y, roll1y, bench, classes |
| `live` | `v ts q{sym:{last prev day state asof [hi lo]}}` + v2 `delay_s` (capped at 65535, the panel's u32) `feed(LIVE/DELAYED/STALE/CLOSED)`; `asof` is the quote's exchange-local DATE; shrink: hi/lo, then delay_s, then quotes from the end |
| `intraday/<sym>` | `v sym ts n min max pts`: the line is 128 slots across the regular session, `n` the slots filled so far (the panel's shape), the unreached ones zero |
| `tape` | `v ts x[{n s t}]` (`t` the next change, panel local time; `s` OPEN PRE POST CLOSED STALE or `--`) |
| `ha` | `online` / `offline` (the will) |
| `config` (from the panel) | v2, the panel's shape (`src/market/README.md` on wip/market-panel): its default payload validates here with no error and no unknown key (test); unknown keys kept, refused values named in `status.cfgErr` |

Line encoding: 128 `uint16` little-endian, base64, scaled between `min` and
`max` (a flat line mid-scale); with `scale: "log"` the values are log10. The
portfolio payload's floats carry six decimals (nothing the panel or HA prints
changes; four lines and the drawdown dates then fit the budget); the index and
ticker payloads are the reference's to the last digit.

Sizes on the samples (`check_market.py`, 95 messages, 146 retained market topics
in the dry run): the largest payload is 1 513 B (a portfolio window with three
lines); four lines (gross or telltale on) reach ~1 840 B; stats ~1 300 B. The
largest configuration of the tests (8 indices, 8 tickers, 16 positions, four
lines) fits every topic; `live` keeps 16 quotes with their feed and delay, and
beyond 16 the last quotes go (the design's budget).

## Home Assistant entities (MQTT discovery, device "Matrix Market")

Discovery is sent only after the broker has answered, from the worker, at QoS 1 and
retained, with the cache reset on every (re)connect, and again whenever Home
Assistant publishes `online` on `<ha.prefix>/status` (paho queues QoS 1 while
disconnected and drops QoS 0: the first start lost it before, audit M3). Entity
ids come from `default_entity_id` (`sensor.matrix_market_<name>`): HA 2026.4 removed
`object_id` from MQTT and discovery ignores it (release notes 2026-04-01, via context7).

`sensor.matrix_market_portfolio_{value,return,since_start,cagr,mdd,div,ter_drag,cash,mode,asof}`,
v2: `_{xirr,twr_ann,fx,vol,sharpe,sortino,calmar,mdd_dates,mdd_month_end,best_year,worst_year,roll1y,roll3y}`,
`_benchmark_1..3`, `_class_{equity,bond,real_assets,gold,cash}`,
`_holding_<sym>_{share,return}`, `_cash_share`, `_exchange_<name>`, `_status`, `_status_asof`;
`binary_sensor.matrix_market_alert_{day_move,drawdown,out_of_band}` when the
`alerts.*` settings are on. The return sensors follow `display.preset`; the mode
follows `portfolio.rebalance`. The Lovelace view: `tools/market/ha/portfolio_view.json`.

## The CLI (the laptop)

```
cd tools/market/appdaemon
python3 -m matrix_market.cli fetch --samples ../samples   # paced 10 s; stops on 429; store = tools/market/store
python3 -m matrix_market.cli report                       # HOLD and REBAL, MAX and 5Y: value, TWR, ann, XIRR, MDD (both bases), ...
python3 -m matrix_market.cli compute                      # every payload and its size
python3 -m matrix_market.cli publish --dry-run            # the payloads
python3 -m matrix_market.cli schema                       # the settings table
python3 -m matrix_market.cli view                         # the Lovelace view (JSON) from the effective settings
python3 tools/market/check_market.py                      # both test suites, sizes, the dry run
```
`--offline` uses the samples; `--config file.json` lays a `config` payload over the
defaults; the local override is `--local PATH`, else `<store>/local.json` when it
exists, and `--no-local` ignores it (the tests and `check_market.py` always pass it). The golden `report` on the real allocation therefore
runs on the Mac and its figures stay out of the repository.

## Behaviour

- Fetch at `fetch.daily_at` (07:00 local) and on a config change that brings a
  new symbol; one request per `fetch.history_gap_s` (10 s); a 429 arms the
  back-off (5, 15, 60 min), the run stops, the symbols not reached keep their
  store, `status.err` says "rate limited". A failed symbol keeps its store and
  carries `err` in `status.symbols`. TER once per `fetch.ter_days`. The ECB
  1999-2003 table once; the HICP monthly, only when `returns.real` or
  `portfolio.contrib.index_inflation` is on (unreachable: `status.realErr`).
- Live: every `live.poll_s` (60 s) while a watched exchange is OPEN: one spark
  request for every symbol on the panel, one 1-minute chart request for the
  `ticker` the panel selected. Nothing outside sessions.
- Tape: on every state change and every 15 min.
- Everything again every `fetch.keepalive_h` (6 h) and on a broker reconnect.
- **A `config` from the panel is diffed key by key** against the last one (the
  panel's audit rule): identical (a reconnect) does nothing; only `ticker`
  switches the intraday poll and clears the old `intraday/<sym>`; only
  `live.poll_s`, `stale.*`, `fetch.*` reschedules the timers; `display.tz`,
  `display.fx_effect`, `ha.*` reach the features (tape, discovery) without a
  recompute; anything touching the history or the maths recomputes and
  republishes once after a quiet 5 s (a burst of portal saves costs one run),
  fetching only when a symbol is new to the store. A symbol that leaves the
  watch lists has its `index/`, `ticker/` and `intraday/` topics cleared at
  once (empty retained payloads), and the next generation clears whatever else
  it no longer carries.
- The owner's decisions of 11:43: `bench.symbol` defaults to `^SP500TR`,
  converted into the portfolio currency (the ECB table before 2003-12) and
  scaled to the capital so every line starts at C; while it has no history in
  the store the first index stands in (logged). `twr` names the window figure;
  `xirr_ann` and `flows: true` appear only when contributions or withdrawals
  exist; `ann` only for 1Y 3Y 5Y 10Y and a MAX of a year or more; `mdd_peak`,
  `mdd_trough`, `mdd_recovery` (null while open) on every portfolio window, on
  `mdd_basis`; `feed` LIVE under 120 s of delay, DELAYED from there, `delay_s`
  always.
- Returns: `chg` is the time-weighted return of the window (PP's daily formula
  with flows at the close, the ledger's own timing; identical to
  `value_end / value_start - 1` without contributions); `ann` from one year
  (GIPS 2.A.12); `xirr` since inception, annualised; `fx` the FX part.
- The TWR timing choice, stated: Portfolio Performance's formula puts inflows at
  the open; this ledger books them at the close, so a 10 % day with a
  contribution reads 10 %, not 6.7 %. `market_ref.twr(..., timing="pp")` gives
  PP's reading.
- `gross` is V + the TER drag so far (first order; the design's
  `net × exp(TER × years)` needs per-position lots the aggregate ledger does
  not keep); labelled EST, off by default.

## Not verified

- Nothing has run inside AppDaemon or against the real broker: the
  `hass.Hass` and paho calls are the media app's, exercised here against fakes.
- `yfinance` and `exchange_calendars` are not installed on this Mac: their code
  paths are guarded and fall back, but ran only in that fallback.
- Yahoo answered 429 to the first request of every attempt from this Mac on
  2026-09-15 (10:40, 10:50, 11:09, 12:09 - one request each, on the app's own
  back-off ladder): the real store could not be filled here, so no golden
  figure exists yet on daily data or on the real allocation, and `^SP500TR`
  (its first bar, close vs adjclose) is not probed; the fetch order puts it
  with the benchmarks. `python3 -m matrix_market.cli fetch --samples ../samples`
  from another address, or later, fills the store and the missing monthly
  samples in one paced run (about nine minutes for the 20 symbols, the TERs
  and the fixtures); `report` then prints the figures.
- The spark answer's shape is parsed for both the v7 `{"spark":{"result":[..]}}`
  form and the v8 `{sym: {...}}` form; neither was captured live yet.

## Backlog

Out of the scope the owner fixed on 2026-09-15 (the coordinator's cut). Each of these was implemented and
committed before the cut and is covered by tests, but it is outside the audit's re-check: treat it as unaudited.

- MINOR 2, the retained leaves persisted in the store (`publisher.Publisher(store=...)`, table `published_leaves`).
- MINOR 5, every change class of a config diff handled (`config.classes`, `fetch.history_gap_s` to the pacer).
- MINOR 9, only missing or stale symbols fetched (`history.stale_symbols`, `missing_symbols`, the failed-since rule).
- MINOR 12, the month-end basis starting with the window's opening balance (`stats.month_ends`).
- NIT 2 (market_ref's loaders close their files), NIT 3 (the store's size note), NIT 4 (the `.gitignore`
  patterns), NIT 5 (the keepalive publishes status alone), NIT 6 (the Easy Stock card from the settings).

Open, not done: a portfolio-level `contrib` number, which the panel's decoder accepts but neither its layout
nor its README defines; the per-holding contributions stay on `market_stats` for Home Assistant.

## Settings

The keys, groups and bounds are the panel's (`src/market/market_settings.cpp` on
wip/market-panel); generated from `config.SCHEMA` (`python3 -m matrix_market.cli schema`):

| Key | Type | Default | Bounds | Group | Since | What |
|---|---|---|---|---|---|---|
| `indices` | symbols | `4 entries (docs/18)` | (1, 8) | watch | 1 | the MARKETS page's indices, {sym, name}; first = primary and the default benchmark; <= 8 (bus limit) |
| `tickers` | symbols | `[]` | (0, 8) | watch | 1 | the TICKER page's symbols, {sym, name}; <= 8 (bus limit) |
| `ticker` | symbol | `None` |  | watch | 1 | the symbol the panel shows on TICKER; its intraday line is polled |
| `presets` | names | `['YTD', '1Y', '3Y', '5Y', '10Y', 'MAX']` | at validation | watch | 1 | the windows published: the six from YTD and the extras the panel switched on (WTD MTD 1M 3M 6M) |
| `portfolio.capital` | number | `10000.0` | (100.0, 1000000000.0) | portfolio | 1 | the initial capital, in the portfolio currency |
| `portfolio.currency` | enum | `EUR` | ('EUR', 'USD') | portfolio | 1 | the portfolio currency; the FX pair the design has is EURUSD=X |
| `portfolio.inception` | date | `2000-01-01` | at validation | portfolio | 1 | the inception date S; from 1990 (sanity) to today |
| `portfolio.contrib.amount` | number | `0.0` | (0.0, 100000000.0) | portfolio | 1 | an extra contribution, 0 for none |
| `portfolio.contrib.every` | enum | `year` | ('month', 'quarter', 'year') | portfolio | 1 | the contribution period, from the inception's anniversary |
| `portfolio.rebalance` | bool | `False` |  | portfolio | 1 | docs/18's v1 flag; the panel derives it from rebal.mode != hold, and so does the app (rebal.mode decides) |
| `portfolio.positions` | positions | `2 entries (docs/18)` | (0, 16) | portfolio | 1 | the allocation: {sym, w, asset_class, [entry], [proxy]}, weights in percent summing to at most 100; <= 16 (bus limit) |
| `rebal.mode` | enum | `hold` | ('hold', 'calendar', 'bands', 'calendar_or_bands') | portfolio | 2 | the model the panel shows: hold, or REBAL's rule (calendar = the approved 31 December); REBAL is always computed |
| `returns.measure` | enum | `twr` | ('twr', 'mwr') | returns | 2 | the measure named in the payload; chg is always the TWR, xirr the MWR |
| `returns.real` | bool | `False` |  | returns | 2 | real terms: deflate by the euro-area HICP (ECB ICP.M.U2.N.000000.4.INX); unavailable -> status |
| `dividends.mode` | enum | `sweep_yearend` | ('sweep_yearend', 'reinvest_paydate', 'drop') | returns | 2 | dividends to cash until 31 December (the owner's rule) | reinvested at the ex-date close (PV) | dropped |
| `rebal.calendar` | enum | `annual` | ('annual', 'semiannual', 'quarterly', 'monthly') | rebal | 2 | the calendar rule's period ends |
| `rebal.bands.abs_pts` | number | `5.0` | (0.0, 50.0) | rebal | 2 | Swedroe 5/25: +-points for targets of 20 % or more |
| `rebal.bands.rel_pct` | number | `25.0` | (0.0, 100.0) | rebal | 2 | Swedroe 5/25: +-percent of the target below 20 % |
| `rebal.bands.check` | enum | `monthly` | ('daily', 'weekly', 'monthly') | rebal | 2 | how often the bands are tested |
| `rebal.contrib_only` | bool | `False` |  | rebal | 2 | new cash buys the underweight positions the day it arrives |
| `contrib.index_inflation` | bool | `False` |  | flows | 2 | grow the contribution with the HICP (needs the HICP table) |
| `withdraw.amount` | number | `0.0` | (0.0, 100000000.0) | flows | 2 | a periodic withdrawal, 0 for none |
| `withdraw.kind` | enum | `amount` | ('amount', 'pct') | flows | 2 | an amount in the portfolio currency, or a percent of the balance |
| `withdraw.every` | enum | `year` | ('month', 'quarter', 'year') | flows | 2 | the withdrawal period, from the inception's anniversary |
| `cash.yield_pct` | number | `0.0` | (0.0, 20.0) | flows | 2 | interest on the cash row, percent per year |
| `ter` | ter_map | `{}` | (0.0, 5.0) | fees | 1 | TER overrides in percent per year, for funds Yahoo lacks |
| `tax.div_withholding_pct` | number | `0.0` | (0.0, 100.0) | fees | 2 | withholding on dividends, percent |
| `costs.per_trade_fixed` | number | `0.0` | (0.0, 1000.0) | fees | 2 | a fixed cost per buy or sell, in the portfolio currency |
| `costs.per_trade_pct` | number | `0.0` | (0.0, 5.0) | fees | 2 | a cost per trade in percent of the amount |
| `display.fx_effect` | bool | `False` |  | currency | 2 | the panel shows the FX part of the return; the app publishes `fx` regardless |
| `bench.mode` | enum | `single` | ('single', 'blend') | bench | 2 | one symbol's price index, or a blend of adjclose legs |
| `bench.symbol` | symbol | `^SP500TR` |  | bench | 2 | the benchmark on the panel, in the portfolio currency, scaled to the capital (owner, 11:43: ^SP500TR); null = the first index |
| `bench.blend` | blend | `same_weights` |  | bench | 2 | the blend: legs [{sym, w}] (the portal's presets fill them), or 60_40 (VFINX 60 + VBMFX 40, adjclose) | same_weights; [] = same_weights |
| `bench.extra` | symbols | `1 entries (docs/18)` | (0, 2) | bench | 2 | up to two more benchmarks for HA's sensors; ^SP500TR is always fetched |
| `bench.telltale` | bool | `False` |  | bench | 2 | publish portfolio / benchmark as a line (Simba's telltale); it takes the benchmark line's slot |
| `lines.px` | bool | `True` |  | lines | 1 | publish the no-dividend line |
| `lines.bench` | bool | `True` |  | lines | 1 | publish the benchmark line |
| `lines.gross` | bool | `False` |  | lines | 1 | publish the gross-of-fees EST line (off by default, docs/18) |
| `mdd_basis` | enum | `daily` | ('daily', 'month_end') | risk | 2 | the drawdown basis of `mdd`; month_end is Portfolio Visualizer's |
| `stats.risk_free_pct` | number | `0.0` | (0.0, 20.0) | risk | 2 | the risk-free rate for Sharpe and Sortino, percent per year |
| `display.preset` | enum | `1Y` | at validation | display | 1 | the window the HA return/MDD/stats sensors report |
| `display.tz` | tz | `None` |  | display | 1 | the panel's local time zone for the tape's next-change times; null = AppDaemon's |
| `display.scale` | enum | `linear` | ('linear', 'log') | display | 2 | log packs log10 values and flags the payload with scale: log |
| `tape.exchanges` | names | `['NYSE', 'NASDAQ', 'LSE', 'XETRA', 'EURONEXT', 'TOKYO']` | at validation | display | 1 | the exchanges on the tape, in order; <= 8 |
| `live.poll_s` | int | `60` | (30, 300) | data | 2 | the live poll period while an exchange is open; 60 is Yahoo's measured update period |
| `fetch.daily_at` | time | `07:00` |  | data | 2 | the daily fetch, local time (docs/18: 07:00) |
| `fetch.history_gap_s` | int | `10` | (10, 600) | data | 2 | seconds between two history requests; >= 10 is the pacing rule (apps.yaml only) |
| `fetch.ter_days` | int | `30` | (1, 365) | data | 2 | how often the TER is read again (docs/18: 30 days; apps.yaml only) |
| `fetch.keepalive_h` | int | `6` | (1, 48) | data | 2 | republish everything this often (docs/18: 6 h; apps.yaml only) |
| `stale.session_min` | int | `20` | (5, 120) | data | 2 | a quote older than this while the calendar says open is STALE (docs/18) |
| `stale.history_days` | int | `3` | (1, 10) | data | 2 | AS OF older than this many trading days -> stale (docs/18: 3) |
| `alert.day_move_pct` | number | `0.0` | (0.0, 50.0) | alerts | 2 | a binary sensor when a watched symbol moves more than this in the day; 0 = off |
| `alert.drawdown_pct` | number | `0.0` | (0.0, 90.0) | alerts | 2 | a binary sensor when the portfolio is more than this below its peak; 0 = off |
| `alert.out_of_band` | bool | `False` |  | alerts | 2 | a binary sensor when any holding is outside its Swedroe band |
| `ha.discovery` | bool | `True` |  | ha | 1 | publish MQTT discovery for the Home Assistant sensors |
| `ha.prefix` | string | `homeassistant` | 32 | ha | 1 | the MQTT discovery prefix |

Wiring (apps.yaml only):

| Key | Type | Default | Bounds | Group | Since | What |
|---|---|---|---|---|---|---|
| `device` | string | `None` | 12 | app | 1 | the panel's six lower-case hex digits; <panel-id> in the committed yaml (set it there, in !secret or in local.json) |
| `topic_base` | string | `nickoscope_matrix` | 40 | app | 1 | the MQTT root |
| `mqtt_host` | string | `None` | 64 | app | 1 | the broker, by IP (the container has no mDNS); <your-broker> in the committed yaml |
| `mqtt_port` | int | `1883` | (1, 65535) | app | 1 |  |
| `mqtt_user` | string | `None` | 64 | app | 1 | !secret nicko_mqtt_user |
| `mqtt_pass` | string | `None` | 128 | app | 1 | !secret nicko_mqtt_pass |
| `store_dir` | string | `/config/market` | 255 | app | 1 | the store, inside the add-on's /config mount |
| `local_file` | string | `None` | 255 | app | 2 | the local override JSON (the real allocation, the private wiring); default <store_dir>/local.json, "" for none; never under apps/ |
| `provider` | string | `yahoo` | 16 | app | 1 | a name in PROVIDERS |
| `user_agent` | string | `Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36` | 200 | app | 2 | the User-Agent of every request; Yahoo answered 200 to this one and 429 to others (measured 2026-09-15, Yahoo can change it) |
| `features` | names | `['history', 'portfolio', 'live', 'tape', 'ha_entities']` |  | app | 1 | the features to wire, names in FEATURES |

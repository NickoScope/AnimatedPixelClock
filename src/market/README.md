# Stock market dashboard: the panel side

Four pages on the 128 x 64 panel - **MARKETS**, **TICKER**, **PORTFOLIO**,
**HOLDINGS** - drawn from what Home Assistant has computed and published over
the MQTT bus, with an exchange tape on top; the settings in the portal on a
page of their own, published back for the app to follow. Design:
`docs/18-stock-dashboard.md` in the knowledge base; the settings inventory
`docs/19-market-dashboard-research.md` section 5. The pixel truth is
`tools/market/render.py` and the 25 approved frames in `tools/market/preview/`:
the firmware draws the same pixels from the same numbers, and
`tools/market/panel/check_market_panel.py` proves it on the host.

```
Home Assistant ── AppDaemon app (the other helper's) ──MQTT──► panel: status, index/*, ticker/*, portfolio/*,
      ▲                                                          holdings/*, live, intraday/*, tape, ha
      └──────────────────────────────────────────────MQTT─── panel: config (retained, v2)
```

## Flags

| Flag | Needs | In |
|---|---|---|
| `MARKET_ENABLED` | `MQTT_BUS_ENABLED`, `CONTROL_ENCODER_ENABLED` (`#error` otherwise) | `matrix-waveshare-rgb` |
| `MARKET_NO_LOCAL_DEFAULTS` | - | ignore `market_local_defaults.h` (the flag matrix, the host test) |
| `MARKET_LOCAL_DEFAULTS_FILE="path"` | - | take the local defaults from this file, relative to `src/market/` |

`tools/flag_matrix.py`: "market + bus + knob" builds; "market, local defaults"
builds with the example header; "market without the bus" and "market without
the knob" are refused; market is in "everything". Every row builds the neutral
defaults (`-DMARKET_NO_LOCAL_DEFAULTS` in its common flags).

Build-time values in `market.h`, each a design choice and named as one:
`MARKET_ENTER_TIMEOUT_MS` 10000 (10 s of quiet leaves an entered page, docs/18),
`MARKET_FS_SETTLE_MS` 5000 and `MARKET_FS_MIN_GAP_MS` 600000 (the LittleFS
record goes 5 s after the last accepted history payload, at most every 10 min),
`MARKET_FS_CHUNK` 4096 (bytes written per `loop()` pass), `MARKET_NVS_SETTLE_MS`
2500 (the knob's changes, as `src/panel`).

## Files

| | |
|---|---|
| `market_model.h/.cpp` | the records, their decoders and refusals, base64 lines, dates, the number rules, ingest by topic, the watch lists, the LittleFS record. No Arduino: tested on the host |
| `market_layout.h/.cpp` | the four pages and the tape as calls on a `Canvas`; render.py's `MK_*` constants name for name |
| `market_pico.cpp` | Picopixel metrics from the GFX glyph table, so right-aligned text lands the same on the host and the panel |
| `market_settings.h/.cpp` | the 66 settings as one table; defaults, the NVS blob, `/api/market`'s JSON, the config payload |
| `market_settle.h` | the knob's changes, settled: one save and at most one publish per turn |
| `market_local_defaults.h` | **not committed**: one's own defaults, below |
| `market_ha.cpp` | topics and ingest, NVS, LittleFS, the config out, `/api/market`'s content |
| `market_page.cpp` | the `Canvas` on the HUB75 display, the knob, the window row |
| `../web/web_panel.cpp` | `/api/market`; the page in `web_panel_page.h` / `web_panel_js.h` |
| `../panel/panel_pages.h` | the page switches after an update that adds page keys |
| `tools/market/panel/` | the host test, the payload encoder, the check, the mock portal, the local defaults example |

Memory: one `Store` in PSRAM holds everything this module keeps - the model
(87 244 B: 16 symbols x 11 windows, 2 portfolio models x 11, holdings x 2,
live, intraday, tape, status), its spares, three copies of the settings
(3 604 B each), the settings' list buffers, the config and blob buffers, the
topics, the notes and the page's buffers - plus the 87 260 B record buffer;
the boot log prints the store's size. JSON documents use two PSRAM allocators:
payloads capped at 16 KB, the settings' documents (the config payload, the
extra row) at 32 KB. No market buffer is in `.bss`: the settings module takes
its scratch from the store (`mks::setScratch`) and computes its pool offsets
instead of keeping a table. `matrix-waveshare-rgb` static RAM: 100 544 B.

## MQTT

Everything under `nickoscope_matrix/<dev>/market/`. **`<dev>`** is the last
three bytes of the panel's Wi-Fi station MAC in lower-case hex, as the media
player uses it. It is on the portal (Market > Diagnostics > topics) and in the
boot log. The panel takes one bus handler and one subscription, the wildcard
`market/#`; its own `config` comes back on it and is ignored (and, being over
the buffer, PubSubClient reads it to the end and drops it).

| Topic | From | Payload |
|---|---|---|
| `config` | panel, retained | the settings, below |
| `status` | app, retained | `{"v":1,"asof":"2026-09-14","fetched":"2026-09-15T07:02Z","state":"ok\|stale\|error","err":"","symbols":{"VFINX":{"asof":..,"bars":6700,"ter":0.0014,"terSrc":"yahoo"}}}` |
| `index/<sym>/<preset>`, `ticker/<sym>/<preset>` | app, retained | `{"v":1,"sym":"VFINX","name":"VFINX","cur":"USD","preset":"5Y","from":"2021-09-13","to":"2026-09-12","last":548.2,"chg":0.124,"hi":552.1,"lo":327.4,"n":128,"min":327.4,"max":552.1,"pts":"<base64>","cagr":0.07,"mdd":-0.2,"ann":0.071}` |
| `portfolio/<mode>/<preset>` | app, retained | `{"v":1,"mode":"hold","cur":"EUR","preset":"5Y","from":..,"to":..,"value":..,"chg":..,"sinceStart":..,"cagr":..,"mdd":..,"div":..,"terDrag":..,"cash":..,"twr":..,"ann":..,"xirr_ann":null,"flows":false,"mdd_peak":"2020-01-01","mdd_trough":"2020-03-01","mdd_recovery":"2020-08-01","fx_effect":..,"contrib":..,"n":128,"min":..,"max":..,"pts":..,"px":..,"bench":..,"gross":..}` |
| `holdings/<mode>` | app, retained | `{"v":1,"mode":"hold","asof":..,"rows":[{"sym":"VFINX","tgt":60.0,"now":63.1,"ret":0.52,"entry":"2000-01-03"}],"cash":{"tgt":0,"now":1.2}}` |
| `live` | app, retained, every `live.poll_s` while an exchange is open | `{"v":1,"ts":..,"q":{"^GSPC":{"last":7619.98,"prev":7656.98,"day":-0.0048,"state":"OPEN","asof":"2026-09-14","feed":"DELAYED","delay_s":900}}}` |
| `intraday/<sym>` | app, retained, for `config.ticker` only | `{"v":1,"sym":"VFINX","ts":..,"n":49,"min":..,"max":..,"pts":"<base64>"}` |
| `tape` | app, retained | `{"v":1,"ts":..,"x":[{"n":"NYSE","s":"OPEN","t":"20:00"}]}` |
| `ha` | app, retained (its will) | `online` or `offline` |

**Rules on the panel** (`market_model.cpp`). Every payload: `v` must be 1; a
field of the wrong type refuses the **whole payload** and what is on screen
stays; an empty (cleared) retained topic is ignored. A symbol is 1-12 of
`A-Z 0-9 . ^ = -`; a display name 1-8 of the same; a currency three
capitals; an exchange 1-10 of `A-Z 0-9`; dates `YYYY-MM-DD`, real, 1970-2100.
A line (`pts`, `px`, `bench`, `gross`) is exactly 128 `uint16`, little-endian,
scaled between the payload's `min` and `max`, as 344 characters of base64
ending `==`; `n` must be 128 (intraday: `n` is the slots filled so far, 0-128,
and the rest of the line is not drawn). `from` <= `to`, `lo` <= `hi`,
`min` <= `max`. `sym`, `preset` and `mode` must agree with the topic; an
index or ticker whose symbol is not in the settings' lists is refused as
`unwatched symbol`; **`intraday/<sym>` is taken only for the ticker on screen**
(`display.ticker`, or the first ticker) and refused as `not the selected
ticker` otherwise, so a late session of the previous ticker never draws under
the next one's chart. Presets: `WTD MTD 1M 3M 6M YTD 1Y 3Y 5Y 10Y MAX`; the six
from YTD are always on the knob, the extras only while some stored record has
them. States: `OPEN PRE POST CLOSED STALE`; feeds: `LIVE DELAYED STALE CLOSED`.
`cagr` may be null (under three years). The fields of the owner's decisions
of 2026-09-15 11:43 are all optional, so an app from before them still draws,
and each is refused on a wrong type: `ann` (a series or the portfolio,
annualised over the window, null under a year), `twr` (the portfolio's window
return; without it the page draws `chg`, the same figure while nothing flows),
`xirr_ann` (since inception, null without flows), `flows` (boolean: money
flows in, so the footer shows XIRR), `mdd_peak`, `mdd_trough`, `mdd_recovery`
(dates; recovery null when not recovered; a peak after its trough is refused),
`fx_effect`, `contrib`, and per quote `delay_s`. Fields the panel does not know
are ignored, never refused. A quote or a tape item without `state` / `s` has
none: the app omits a state it does not know rather than send `--`. `px` is
always present. The app's risk statistics are published under `market_stats/`,
outside `market/#`, and the panel does not read them. List entries that fail are dropped and
counted (`dropped`): at most 16 holdings, 16 quotes, 8 tape items, 16 status
symbols. `status.err` is upper-cased and cut to 24 characters: it is DATA ERR's
reason. Payloads from the app stay under 1 900 B (the bus buffer is 2 048 B
with the header and the topic); the largest of the preview frames is 1 592 B
(`portfolio/hold/MAX` of `portfolio_contrib_max`).

**Consistency.** Each topic replaces its record whole and only when valid;
a page's `AS OF` is the oldest `to` among the records it drew, so a page with
one series still at yesterday's date says so rather than mixing silently.
`STALE` (amber) when the as-of is more than `stale.history_days` weekdays
before today (3 by default; no holiday calendar on the panel). `DATA ERR`
(red) when the page has no record for its window: the reason is the app's
`status.err` when its state is `error`, `NO <window> DATA` when other windows
exist, else `NOTHING STORED`.

### The config payload, v2

The rows of `market_settings.h` nested by their dotted key (or `path` where
docs/18's v1 shape differs: `portfolio.*`, `ticker`), plus the derived
`portfolio.rebalance` (`rebal.mode` != hold), the `presets` list (the six
standard ones and the extras switched on), `ticker` as the ticker on screen,
and every key of the `extra` row spread in as it came. Rows marked panel-only
(`display.window`, `tape.on`, `tape.speed`, `display.lines.*`,
`display.session`, `display.dwell_s`, `display.colors`, `display.pages.*`,
`holdings.sort`, `holdings.band_mark`, `feed.label`, `presets.*`) stay home:
**the window never leaves the panel**. It is **streamed** with
`mqttBusPublishLarge()` (PubSubClient's `beginPublish`, `write`, `endPublish`;
a short write drops the connection, since the stream would be out of step),
so it may pass the bus buffer; its own ceiling is 4 096 B (`mks::kConfigMax`),
and the portal says when it does not fit. 1 325 B at the neutral defaults with
one extra key.

**When it is published:**
- **a portal save or reset**: the settings are written to NVS first, then the
  config goes out, in the same request - what the app reads is what flash holds;
- **the knob** (window or ticker): never at once. Each detent notes the time
  (`market_settle.h`); once the knob has been quiet `MARKET_NVS_SETTLE_MS`,
  one NVS write, and one publish only if the ticker on screen differs from the
  one last published. The window alone is saved and never published. While the
  knob is still turning nothing is published at all, a reconnect's included;
- every reconnect, the boot, and `{"republish":true}`. A reconnect is counted by the bus
  (`mqttBusConnects()`), not seen as a down-up edge: `mqttBusLoop()` can reconnect inside
  one call when `WiFiClient::connected()` missed the broker's FIN, and the edge would never
  show. The media player's retained selection follows the same counter.

A config whose serialised form is byte for byte the one last published (by
length and CRC-32, `ConfigGate` in `market_settle.h`) is not sent again: a save
that changes only panel-only rows, or a knob turn that comes back to the same
ticker. The boot, a reconnect and `{"republish":true}` send it anyway, since
the broker may have lost the retained copy. The portal says when a save left
the config unchanged; `/api/market`'s `cfgOut` has `unchanged` and `skipped`.

**For the app:** when only `ticker` changed from the previous config, switch
`intraday` to the new ticker and do not recompute anything else.

```json
{"v":2,
 "indices":[{"sym":"^GSPC","name":"SPX"},{"sym":"^IXIC","name":"NDX"},{"sym":"^FCHI","name":"CAC"},{"sym":"^GDAXI","name":"DAX"}],
 "tickers":[{"sym":"VFINX","name":"VFINX"},{"sym":"VBMFX","name":"VBMFX"}],
 "portfolio":{"capital":10000,"currency":"EUR","inception":"2000-01-01",
              "positions":[{"sym":"VFINX","w":60,"asset_class":"equity"},{"sym":"VBMFX","w":40,"asset_class":"bond"}],
              "contrib":{"amount":0,"every":"year"},"rebalance":false},
 "rebal":{"mode":"hold","calendar":"annual","bands":{"abs_pts":5,"rel_pct":25,"check":"monthly"},"contrib_only":false},
 "ticker":"VFINX","tape":{"exchanges":["NYSE","NASDAQ","LSE","XETRA","EURONEXT","TOKYO"]},
 "returns":{"measure":"twr","real":false},"dividends":{"mode":"sweep_yearend"},
 "contrib":{"index_inflation":false},"withdraw":{"amount":0,"kind":"amount","every":"year"},"cash":{"yield_pct":0},
 "ter":{},"tax":{"div_withholding_pct":0},"costs":{"per_trade_fixed":0,"per_trade_pct":0},
 "display":{"fx_effect":false,"scale":"linear"},
 "bench":{"mode":"single","symbol":"^SP500TR","blend":[],"telltale":false},
 "mdd_basis":"daily","stats":{"risk_free_pct":0},
 "live":{"poll_s":60},"fetch":{"daily_at":"07:00"},"stale":{"session_min":20,"history_days":3},
 "alert":{"day_move_pct":0,"drawdown_pct":0,"out_of_band":false},
 "presets":["YTD","1Y","3Y","5Y","10Y","MAX"]}
```

## The settings

One row per setting in `mks::kDefs` (`market_settings.cpp`): key, type,
group, bounds, default, the portal's words. Everything else is derived, so a
new setting is one row. Groups: **Watch**, **Portfolio**, **Display** on the
plain page; **Returns**, **Rebalancing**, **Cash & flows**, **Fees & taxes**,
**Currency**, **Benchmark**, **Risk**, **Display (more)**, **Data**,
**Alerts**, **Pass-through** folded. Types and their JSON:

| Type | JSON | Text form (the blob) |
|---|---|---|
| bool, int, num, enum | as is; an enum is its option's name | `1`, `20`, `5.0`, `hold` |
| date, time, sym | `"2000-01-01"`, `"07:00"`, `"VFINX"` (upper-cased) | the same |
| symlist | `[{"sym":"^GSPC","name":"SPX"}]`; a bare `"^GSPC"` is accepted in | `^GSPC=SPX,...` |
| poslist | `[{"sym","w","entry","asset_class","proxy"}]`, weights with two decimals summing to at most 100 | `SYM:W[:DATE[:CLASS[:PROXY]]],...` |
| wlist | `[{"sym","w"}]`, summing to at most 100 | `SYM:W,...` |
| termap | `{"VWCE.DE":0.22}`, percent a year, 0-10 | `SYM=PCT,...` |
| namelist | `["NYSE",...]` | `NYSE,...` |
| json | an object, verbatim, up to 511 bytes | the compact JSON |

**`display.ticker`** is one of `tickers`: a POST that sets another is refused
(`display.ticker: XYZ is not in tickers`); a POST whose `tickers` list drops
the current one moves it to the first; a stored one the list has lost (an
older build's settings) is moved to the first at boot with a log line
`[market] display.ticker ... is not in tickers: ... instead`.

### Defaults: neutral in the tree, one's own in an uncommitted header

The committed defaults are an example: the four indices `^GSPC=SPX
^IXIC=NDX ^FCHI=CAC ^GDAXI=DAX`; tickers `VFINX VBMFX`; the allocation VFINX 60
(equity) / VBMFX 40 (bond), the shape of Vanguard's Balanced Composite, whose
Yahoo histories start before 2000 (the coordinator's probe of 2026-09-15, not
re-checked here); 10 000 EUR from 2000-01-01, `rebal.mode` hold; window `MAX`.

One's own defaults go in **`src/market/market_local_defaults.h`**, which is in
`.gitignore` and never committed. `market_settings.cpp` includes it with
`#if __has_include("market_local_defaults.h")`; when it is there, each of its
rows replaces that row's neutral default - for `defaults()`, for the portal's
"Reset ... to defaults", for the registry's `def` and for `isDefault`. A row
that does not read falls back to the neutral default, and the boot log says
`[market] local default <key> does not read`. The format, with an example
three-fund allocation, is `tools/market/panel/local_defaults_example.h`:

```cpp
static const mks::LocalDefault kLocalDefaults[] = {
  {"tickers", "VTI=VTI,VXUS=VXUS,BND=BND"},
  {"display.ticker", "VXUS"},
  {"portfolio.positions", "VTI:50::equity,VXUS:30::equity,BND:20::bond"},
};
```

`-DMARKET_NO_LOCAL_DEFAULTS` ignores the header (the flag matrix and the host
test use it, so they check the neutral defaults on any machine);
`-DMARKET_LOCAL_DEFAULTS_FILE="path"` names another file (the host test's
second binary and the matrix's "market, local defaults" row use the example).
**`.gitignore` covers only `src/market/market_local_defaults.h`:** a file
named with `-DMARKET_LOCAL_DEFAULTS_FILE` anywhere else in the tree is not
ignored and would be committed with real weights in it; keep such a file
outside the repository, or add its path to `.gitignore` first.
The check of the local rows at boot probes on the store's spare `Settings` in
PSRAM, so neither build allocates from the heap for it.
`/api/market` reports `"defaults":"local"` or `"neutral"`, and so does the boot
log. Settings already in NVS are not touched by either: the defaults only
decide what a panel without a stored blob, or a reset, shows.

What the firmware honours itself (the rest is passed to the app):
`display.window` and `display.ticker` (the knob changes them too),
`display.pages.*` (the knob and the carousel skip a page switched off),
`display.dwell_s` (the carousel's seconds per market page), `display.lines.*`
and `display.session`, `display.colors` (green/red, blue/red, red up),
`tape.on`, `tape.exchanges` (the names drawn with `--` until the app's tape
arrives), `tape.speed` (0-3 px per frame at 20 fps), `holdings.sort` (the
allocation's order, or by current share), `holdings.band_mark` (`TGT>NOW` in
amber outside `rebal.bands`, the 5/25 rule), `feed.label` (the delay badge by default: LIVE only while the quote is under
2 minutes behind the clock, else `D` and the minutes in dim; `live` keeps LIVE), `stale.history_days`.

**NVS**: namespace `market`, key `cfg`, one blob of `key=text` lines with
`v=2` first, so a row added later reads as its default and a removed one is
skipped; key `v` the schema. 1 434 B at the neutral defaults, refused above
6 144 B. ESP-IDF's NVS (its "Record Size Limitations" for the ESP32-S3, read
2026-09-15): a blob may be 508 000 B or 97.6 % of the partition less 4 000 B,
whichever is lower, and the new value is written before the old is erased, so
free space for a whole copy is needed at every save; strings stop at 4 000 B.
The panel's `nvs` partition (`default_16MB.csv`) is 0x5000 = 20 480 B, shared
with the `panel`, `fb`, `media` and other namespaces. A portal save writes at
once; the knob's changes after the settle above.

## LittleFS: `/market/last.bin`

The whole `Model` as one record, so a reboot with Home Assistant down (or
the broker's retained topics gone) still shows the pages with their as-of:

```
 0  uint32  magic   0x314B544D  ("MKT1")
 4  uint16  version 3           (bump when any record struct changes; 3: the fields of 11:43)
 6  uint16  0
 8  uint32  size    sizeof(Model) = 87 244
12  Model   87 244 B, plain data
    uint32  CRC-32 (IEEE, 0xEDB88320 reflected) over everything before it
total 87 260 B
```

**Writing.** 5 s after the last accepted `status`, `index`, `ticker`,
`portfolio` or `holdings` payload (`live`, `intraday` and `tape` are this
minute's and do not count), at most every 10 min, into `/market/last.tmp` in
4 KB pieces, one per `loop()` pass, then renamed over `last.bin` with no
remove before it. The rename replaces the old record atomically:

- `LittleFS.rename` is `VFSImpl::rename` (arduino-esp32 2.0.17,
  `libraries/FS/src/vfs_api.cpp:125-163`), which checks only that the source
  exists and calls `::rename`;
- the VFS hands that to esp_littlefs's `vfs_littlefs_rename`, which refuses an
  open source or destination and calls `lfs_rename` directly (esp_littlefs
  commit 41873c2, v1.14.1 - the one `tools/sdk/versions.txt` of arduino-esp32
  2.0.17 names - `src/esp_littlefs.c:1916-1941`);
- littlefs at that submodule commit (f53a0cc, `LFS_VERSION` 2.9) documents
  "If the destination exists, it must match the source in type"
  (`lfs.h:511-517`), and `lfs_rename_` (`lfs.c:3913`) deletes an existing
  destination entry and creates the moved one in the same `lfs_dir_commit`
  (`lfs.c:3999-4005`, the `LFS_TYPE_DELETE` tag on `newid` when `prevtag !=
  LFS_ERR_NOENT`): with both files in `/market/` (`samepair`), one metadata
  commit, so a power cut leaves the old record or the new one.

**Reading, at `marketBegin()`.** A `last.tmp` that passes the length, magic,
version, size and CRC is a write that finished all but the rename: the newest
record there is, so the model is taken from it and it is renamed over
`last.bin`. If that rename fails, `last.tmp` stays, the model stays, and the
rename is tried again before the next write; while it keeps failing the write
waits too (a new write would truncate the only whole copy), again after
`MARKET_FS_MIN_GAP_MS`. A rename that fails at the end of a write keeps its
whole `last.tmp` the same way. A `last.tmp` that does not read is a write cut
short and is removed. Otherwise `last.bin` is read; a wrong
length, magic, version, size or CRC leaves the model empty, and the portal's
`fs.note` says which. Then the settings' watch lists re-slot the model, so a
symbol kept keeps its series and one removed is forgotten.

## The pages and the knob

`market_layout.cpp` is render.py's four pages, constant for constant: the
tape on rows 0-6 (Picopixel, `MK_TAPE_FPS` 20), a dark rule at 7, the heading
at 8, the key number at 2x on rows 16-29 (thin spaces and dots: `7 620`,
`699.3` read as one number), the footer at 58. Colours before color565:
amber 255/150/0, white, dim 110/122/128, green 60/200/90, red 230/70/60, the
rule 36/40/44, blue 60/150/255 (the blue/red scheme's up). Numbers as
render.py's `fmt_*`: a space as the thousands separator, one decimal under
1 000, K/M/B only when the plain figure does not fit its slot, percentages
with one decimal under 100 and none above, a sign always except zero, which
is dim. Python's rounding (`%.1f` is the exact decimal, `round()` half to even)
is reproduced (`nearbyint`, `%.1f`). `fmtAmount` falls back to whole K or M
when the decimal does not fit: 21 663 in four characters is `22K` (render.py's
fix of 2026-09-15; before, `0.0M`).

### The owner's decisions of 2026-09-15 11:43, as the panel draws them

The approved previews v2 (`tools/market/preview/README.md`, merged from
`wip/market-previews2`); every constant below is render.py's name for name.

- **Live quote.** While the exchange is OPEN the day change sits behind green
  `LIVE` only when `delay_s` < `MK_LIVE_MAX_S` (120 s); otherwise behind the
  delay badge `MK_DELAY_BADGE` and the whole minutes, dim: `D15`. A quote the
  app calls DELAYED without `delay_s` gets the bare `D`. `feed.label` = `live`
  keeps LIVE whatever the delay; `delayed_badge` is the default.
- **TICKER.** `ANN` and the annualised figure at the heading's right, from a
  one-year window (`ann` in the payload); none under a year. None on MARKETS:
  its free rows 38-40 are three high and Picopixel needs five.
- **PORTFOLIO.** The value in a 6-character slot (`MK_BIG_CHARS_PF`); `TWR`
  (dim) beside the window's time-weighted return; `ANN` under it from a
  one-year window. With `flows` the footer is `XIRR` since inception and `DIV`
  (TER~ and CASH give their room; STALE keeps XIRR alone).
- **The drawdown stop**, PORTFOLIO's second click: the footer becomes `MDD
  -19.8%  20-01>20-03  REC 20-08` (or `NO REC`), the fall from the peak's point
  to the trough's is redrawn on the value line in the fall's colour, a bracket
  under the chart (row `MK_Y_MDD_BAR` 57, end ticks `MK_MDD_TICK_H` 3) joins
  their columns, and the as-of date moves into ANN's slot. A date's point is
  `ceil(offset x 128 / span) - 1` (`slotIndex`, render's `slot_index`). No fall
  at all: `MDD 0.0%` and no marks.
- **Benchmark.** `bench.symbol` defaults to `^SP500TR`; the app publishes the
  line already in the portfolio currency and scaled to the capital.
- **Colours.** Unchanged: green up, red down; blue/red recolours only signed
  changes, so OPEN and LIVE stay green.

The four pages are `CtrlPage`s of their own after the rail board
(`PAGE_MARKET_FIRST..LAST`), one portal switch (`PANEL_KEY_MARKET`), 20 s each
on the carousel (`display.dwell_s`). `PANEL_KEY_MARKET` is bit 8 of NVS
`panel/pages`; a mask written by an older firmware does not have it, so
`panel.cpp` now stores `pgKnown` beside it, one u32: the keys the writing
build knew, and the `pages` value it wrote. At boot the keys added since start
on (`src/panel/panel_pages.h`). A mask without `pgKnown`, or whose `pages` is
not the copy in `pgKnown` (an older firmware rewrote it: this one, then an
older one, then this one again), is taken to know only CLOCK..CARDS, the keys
of the first build to write `pages`, so the Lua, media and market pages start
on once.

The knob: rotate browses pages; a click enters **WINDOW** - rotate steps the
preset among the ones on hand, shown in a black band over the footer with an
amber rule for 1.5 s (`WINDOW` and the presets, the current one white; the
label goes first when eleven do not fit, then the farthest presets); on
MARKETS a second click makes rotate choose the primary index (`TURN: INDEX`),
on TICKER the ticker (`TURN: TICKER`), on HOLDINGS the page of rows (`TURN:
ROWS`); PORTFOLIO's second stop is the drawdown
(`DRAWDOWN`), where rotate still steps the window. The next click, or 10 s of quiet, leaves. On PORTFOLIO's drawdown stop the window row
goes over the heading (rows 8-15) instead of the footer, so the MDD footer and the bracket
on row 57 stay in view while the window changes.
The window and the ticker are settings, saved and (the ticker) published once
the knob settles, as above. Refresh: 20 Hz while the tape moves, else 5 Hz;
nothing else on the pages moves.

## `/api/market`

JSON only, as the other Panel routes (`web_panel.cpp`); a POST body may be
8 KB here (a config with sixteen positions and every row), and one whose
Content-Length is over that is refused before WebServer copies it.

`GET` → `{success, page, showing, ready, dev, root, handler, subscribed, bridge,
accepted, refused, lastRefusal, jsonPeak, rxAgoS, defaults:"local"|"neutral",
config:{every row by its dotted key}, registry:{groups:[{key,label,adv}],
rows:[{key,type,group,label,hint,unit,min,max,dec,opts,cap,panel,def}]},
nvs:{saved,note,bytes,writes,fails,max}, cfgOut:{sent,tooBig,bytes,max,count,
ticker,agoS}, fs:{ready,have,note,bytes,writes,fails,lastMs,pending,gen},
view:{window,mode,ticker,presets}, model:{gen,bytes, indices:[{sym,name,windows,
w:{have,cur,last,chg,hi,lo,cagr,mdd,from,to}, live:{last,day,state,feed,delayS}}],
tickers:[...], portfolio:{hold:{...},rebal:{...}}, holdings:{hold:{asof,rows,
cashNow},rebal}, live, intraday, tape, status:{have,asof,fetched,state,err,
symbols:[...]}}, mqtt:{configured,connected,status}}`. Without PSRAM for the
store: `ready:false` with `storeBytes`, `recordBytes`, the bus fields and no
`config` or `registry`.

`POST` one of:
- `{"config":{"rebal.mode":"bands",...}}` - known keys validated whole
  (nothing changes unless everything is valid; 400 names the field:
  `portfolio.positions: the weights sum to 110.00 %, at most 100`), unknown
  keys kept in `extra` for the app; NVS now, then the config out.
- `{"show":"markets"|"ticker"|"portfolio"|"holdings"}` - the page on the panel now.
- `{"republish":true}` - the config out again.
- `{"reset":"<group key>"|"all"}` - those rows back to their defaults (the
  local header's where it has them); NVS now, then the config out.

The Market page in the portal (`web_panel_page.h`, `web_panel_js.h`) is a
nav group of its own and builds its controls from `registry`: the three core
groups as cards, each advanced group as a fold whose closed line lists what is
not at its default and which carries a "Reset ... to defaults" link; the
allocation as rows with a live sum bar that refuses more than 100 %, the entry
date, asset class and proxy folded per row; symbols and exchanges as chips.
**Save sends only the rows changed in the form**, so a window or a ticker the
knob moved while the form was open is not overwritten; it marks the field the
panel names, and says when the settings were applied but not yet written to
flash. Home Assistant's status and both end values (HOLD and REBAL) sit at the
top; the diagnostics (per-symbol status, NVS, the flash copy, the config out,
the bus) at the bottom. With `ready:false` the page builds no form and says the
panel has no memory for the store.

## Checks

`python3 tools/market/panel/check_market_panel.py`:
1. the host test on the neutral defaults (384 checks: decoders and refusals,
   the fields of 11:43, intraday for the selected ticker only, numbers, dates,
   base64, ingest, watch lists, the record, the layout and the delay badge,
   the settings and `display.ticker`'s rule, four knob detents giving exactly
   one publish after the settle, the config gate, the page switches after an
   update and through an older firmware), and a second build on the example
   local defaults header (13 checks);
2. 702 formatted numbers against render.py's `fmt_*` and `fmt_ym`, whole-K
   amounts included;
3. the base64 packing against `market_ref.pack_pts`;
4. every preview frame through the panel's ingest and layout against the
   merged `frames.json` and the 1:1 PNGs: **31 of 32 identical** in strings and
   pixels, the drawdown stops (with their marks) and the contributions frame
   included; `ticker_err` differs by the currency alone, which the firmware
   cannot know with nothing stored;
5. the layout budget: render.py's `MK_*` and `LBL_*` against `market_layout.h`
   (equal; `MK_SESSION_MIN` and `MK_TAPE_SEP` are the app's and render's
   own), then `render.layout_budget()` run with the firmware's constants,
   Picopixel and 5x7 widths and number formatters: the same blank columns as
   `frames.json`'s `_meta` budget, every slot at least 1;
6. payload sizes; 7. the portal script through jsc.

`python3 tools/market/panel/mock_portal.py` serves the generated portal with
the firmware's registry and validation for a browser (`/mock/nomem?on=1` for
the no-memory state).

## Backlog

From the delta audit of 4727ed0..252b939, deferred by the owner's scope cut:
- **MDD without its months.** A `portfolio` payload with `mdd` but no
  `mdd_peak` / `mdd_trough` draws `MDD 0.0%` on the drawdown stop; `Portfolio`
  should record whether `mdd` came, draw the figure without months and marks,
  and print `0.0%` only when it is absent. The app always sends the dates today.
- **The config gate compares a CRC-32.** Two configs of one length could share
  it; a PSRAM copy of the last one published (at most 4 KB) and `memcmp` would
  rule that out.
- **The knob's state machine** lives in `market_page.cpp`: move it into plain
  C++ as `KnobSettle` is, and test WINDOW, the page's stop / DRAWDOWN, leaving,
  a turn inside the stop, the 10 s timeout and a page change on the host.
- **A comment:** in `market_model.h` the `// "14 SEP"` note sits on `fmtYm`'s
  line; it belongs on `fmtDate`.

## Measured on the panel

Nothing yet: this branch was built without the hardware. To measure once
flashed: the boot log's `[market] store ... B in PSRAM`, the render time of
PORTFOLIO (three curves at 20 fps), the LittleFS write's `lastMs` on the
portal, `jsonPeak`, the boot with the broker down (the pages from the record,
`AS OF` or `STALE`), one config publish per knob turn on the broker, and the
page switches after flashing over an older firmware's NVS.

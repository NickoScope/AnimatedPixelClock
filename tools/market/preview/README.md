# Market dashboard previews, 2026-09-15

Every PNG here is drawn by `tools/market/render.py` from the numbers that
`tools/market/market_ref.py` and `tools/market/preview_returns.py` compute on
the saved answers in `tools/market/samples/`. Nothing is typed. Each frame
exists at 6x (`name.png`) and 1:1 (`name_128x64.png`); `contact_sheet.png`
shows all 32 side by side with a label; `frames.json` lists every string drawn,
and its `_meta` holds the layout budget below. Rerunning the renderer gives the
same bytes (two runs compared on 2026-09-15).

This is the second set. The approved 25 frames of 3024545 are redrawn with the
owner's six decisions of 11:43 (docs/18, "The owner's decisions, 11:43"), and
7 frames are new. "Frames changed vs 3024545" at the end is the list the
firmware step works from.

**What the data is.** The samples are monthly (VOO, ^IXIC, ^FCHI, ^GDAXI,
EURUSD=X, ^SP500TR), weekly (AAPL, 5 years) or quarterly (^GSPC from 1984),
plus the ^GSPC 1-year daily sample and the ECB's monthly EUR/USD table for
1999-2004. Their bars stand in for the daily timeline, so a "31 December" is
the year's last bar and a dividend lands on the first bar on or after its event
date. As-of is 14 SEP 2026 on every page: the oldest last bar among the page's
series. VOO is the only fund with a sample, and no 5-minute session sample
exists. Consequences are marked below.

**Layout, the constants the firmware copies** (`MK_*` in `render.py`): the
tape is rows 0-6 (`MK_TAPE_H` 7, Picopixel top at row 1), one dark row at 7
(`MK_Y_TAPE_RULE`), the page is rows 8-63 (`MK_PAGE_TOP` 8, 56 px). Heading 5x7
at row 8; the key number at 2x on rows 16-29, its 7-character slot ends at
x 82 on TICKER (`MK_BIG_CHARS`), 6 characters and x 70 on MARKETS
(`MK_BIG_CHARS_MKT`) and, new, on PORTFOLIO (`MK_BIG_CHARS_PF`); the footer in
Picopixel at row 58. Fonts: 5x7 for headings, symbols, the key number, the
signed changes; Picopixel for the tape, tags, currencies, secondary rows,
labels, footers. In a number a space (the thousands separator) or a dot
advances half a cell (`MK_THIN_ADV` 3), so `7 620` and `699.3` read as one
number at 2x. Colours before color565: amber 255/150/0 (the panel's own),
white, dim 110/122/128, green 60/200/90, red 230/70/60, the tape rule
36/40/44, and for the blue/red scheme blue 60/150/255; color565 keeps them as
248/148/0, 248/252/248, 104/120/128, 56/200/88, 224/68/56, 32/40/40,
56/148/248. Numbers: a space as the thousands separator, one decimal under
1 000, K and M with one decimal only when the plain figure does not fit its
slot, and whole K or M when even that does not fit; percentages with one
decimal under 100 and none above, a sign always except zero, which is dim.

New constants: `MK_LIVE_MAX_S` 120 and `MK_DELAY_BADGE` `D` (the freshness
badge), `MK_BIG_CHARS_PF` 6, `MK_Y_MDD_BAR` 57 and `MK_MDD_TICK_H` 3 (the
drawdown bracket), and the words `LBL_TWR`, `LBL_ANN`, `LBL_XIRR`, `LBL_MDD`,
`LBL_REC`, `LBL_NO_REC`.

**The tape** (owner, 09:19): `NYSE`, `NASDAQ`, `XETRA`, `EURONEXT` with their
state from the samples' `meta.currentTradingPeriod` (NYSE Arca via VOO,
NasdaqGS via AAPL, XETRA via ^GDAXI, Euronext Paris via ^FCHI): OPEN green,
PRE and POST amber, CLOSED dim, `--` when the app has nothing. LSE and TSE are
not on it: their hours have no source here. The tape scrolls 1 px per frame at
20 fps (`MK_TAPE_FPS`); each preview shows it at the offset its clock gives.
Frames are stamped at five instants: 11:00 Paris on 14 SEP (XETRA and EURONEXT
OPEN, NYSE and NASDAQ PRE), 12:00 New York on 14 SEP (NYSE, NASDAQ OPEN, XETRA
POST), 22:30 Paris on 14 SEP (NYSE, NASDAQ POST), 08:40 Paris on 15 SEP (XETRA
PRE, the rest CLOSED; when the samples were saved) and 08:40 Paris on 21 SEP
(STALE).

## Decision 1: LIVE, the delay badge, CLOSE

While the instrument's exchange is OPEN the last price carries the day change
against the previous close, in its sign colour. In front of it: green `LIVE`
only when the quote is less than 120 s behind the clock (`MK_LIVE_MAX_S`);
otherwise, in the same slot, the delay badge `D` and the whole minutes behind,
in dim (`D15`). When the exchange is not open the label reads `CLOSE 14 SEP`,
as approved.

The lag drawn is measured, not assumed, for Europe: Yahoo's ^GDAXI, ^FCHI and
^FTSE ran 902-906 s behind the clock on every probe on 2026-09-15 09:19-09:21
CEST (docs/18, "Live quotes"); both ends make `D15`, and the renderer checks
that. **Yahoo's US delay is measured only at 15:35 CEST today, so the default
state of the ^GSPC frames depends on that measurement:** they come in both
states, `LIVE` (a real-time feed, as approved) and `D15` (the European lag,
applied to New York as a variant).

A monthly sample has no day change (its last two bars are a month apart), so
MARKETS takes it from Yahoo's own meta: `fulldayChange / (fulldayPrice -
fulldayChange)`. On ^GSPC that gives -0.48321923 %, the daily sample's last two
closes give the same to 1e-11. Only the DAX frame draws it.

For the firmware: today `feed.label` = `delayed_badge` draws `DELAYED` in
amber. The decision makes the badge the rule, `D` and minutes in dim, from the
quote's lag (`delay_s`, or the clock minus the quote time).

## MARKETS

`markets_max_open`, `markets_max_open_d15`, `markets_5y_closed`,
`markets_1y_dax`, `markets_1y_dax_d15`, `markets_stale`, `markets_err`.
Heading `MARKETS` + the window tag; `AS OF 14 SEP` at the right (amber `STALE
14 SEP` on 21 SEP, empty on DATA ERR). The primary index: its mnemonic, the
window change, the last at 2x, a 48x16 sparkline from the 128-point payload,
`LIVE` / `D15` / `CLOSE` under the sparkline; three secondary rows in
Picopixel: mnemonic, last, window change. SPX last 7 620 (7 619.98 on 14 SEP):
MAX from 2000-01-01 +458 %, 5Y +66.8 %, 1Y +11.3 %, YTD +10.8 %; LIVE -0.5 %
against the 11 SEP close of the daily sample. NDX 26 186: +565 % MAX, +81.2 %
5Y, +15.6 % 1Y, +12.7 % YTD. CAC 8 118: +43.4 %, +24.5 %, +2.8 %, -0.4 %. DAX
25 441: +272 %, +66.7 %, +6.5 %, +3.9 %. `markets_1y_dax` has the DAX as the
primary (click, then rotate). The MAX sparkline of the S&P is a staircase
because its sample is quarterly.

New: `markets_max_open_d15` is `markets_max_open` with `D15 -0.5%` (the US
variant). `markets_1y_dax_d15` is the DAX primary at 11:00 Paris during the
XETRA session with Yahoo's measured lag: `DAX +6.5%`, `25 441`, `D15 -0.5%`
(fulldayChange -127.75 on 25 440.81: -0.4996 %).

**No ANN on MARKETS** (decision 4 left it open). Row 16 holds a mnemonic of up
to 8 characters (47 px) and the window change (up to 38 px) in the 76 columns
left of the sparkline; the 2x last fills rows 24-37 to x 70; the sparkline and
its label fill x 79-127 to row 37; the three rows at 41, 49, 57 reach the
bottom. The only free rows, 38-40, are 3 high, and Picopixel needs 5. `ANN
+8.6%` is 33 px, so it would cost a secondary row or the sparkline's label.
All five approved MARKETS frames are pixel-identical.

## TICKER

`ticker_voo_max`, `ticker_voo_5y`, `ticker_voo_ytd`, `ticker_gspc_1y_open`,
`ticker_gspc_1y_open_d15`, `ticker_gspc_1y_open_bluered`, `ticker_aapl_short`,
`ticker_worst_neg`, `ticker_worst_pct`, `ticker_err`. Heading: symbol, window
tag, currency, and new at its right `ANN` from a one-year window; the last at
2x; the window change top right, `LIVE` / `D15` / `CLOSE` bottom right; the
window chart on rows 31-47 with three ticks at the quarters; the session strip
on rows 49-56; footer `HI .. LO ..` (the window's highest and lowest close,
whole units) and `AS OF 14 SEP`. VOO 699.3 USD: MAX from its first bar
2010-10-01 +546 %, `ANN +12.4%`, HI 705 LO 104; 5Y +77.3 %, `ANN +12.1%`,
HI 705 LO 328; YTD from the 2025-12 bar +11.5 %, no ANN (under a year),
HI 705 LO 598. ^GSPC 1Y: 7 620, +15.2 %, HI 7 799 LO 6 344, LIVE -0.5 %, the
strip 49 of 128 slots at 12:00; **no ANN**: the 1-year daily sample starts
2025-09-15, so its "1Y" window is 364 days, under a year, and the frame is
pixel-identical to the approved one. AAPL on MAX: the sample starts 2021-09-13,
so the window starts there (the short history): 333.1, +128 %, `ANN +17.9%`,
HI 334 LO 130. **The session strip is synthetic on every frame:** no 5-minute
sample exists; its shape replays the last 78 daily ^GSPC returns damped 8x,
bent between the previous close and the last (`synthetic_session()`), in dim.
Worst cases: `ABCDEFGH`, the ^IXIC path times -5 (an eight-character symbol, a
six-figure negative: `-130.9K`, HI -5 860 LO -134.9K, +565 %, `ANN +7.4%`
beside the widest heading); `GROWTH8X`, the ^IXIC path stretched to end at
exactly +1 245 % (`52 998`, `+1 245%`, `ANN +10.2%`, HI 55 193 LO 746).

ANN is `(last / first)^(365.25 / days) - 1` over the page's window, shown only
when the window is a year or longer (`preview_returns.annualised`). It sits at
the heading's right, directly above the window change: TICKER's row under the
change already carries `LIVE` / `CLOSE`.

New: `ticker_gspc_1y_open_d15` (`D15 -0.5%`, the US variant) and
`ticker_gspc_1y_open_bluered` (decision 6, below).

## PORTFOLIO

`portfolio_hold_max`, `portfolio_hold_max_mdd`, `portfolio_rebal_max`,
`portfolio_hold_5y`, `portfolio_rebal_5y`, `portfolio_contrib_max`,
`portfolio_synth14_hold_max`, `portfolio_synth14_hold_max_mdd`,
`portfolio_synth14_rebal_5y`, `portfolio_nodiv`, `portfolio_stale`. Heading
`PORTFOLIO EUR`, mode and window tag at the right; the value at 2x in a
6-character slot; beside it `TWR` (dim) and the window's TWR in 5x7; under
that `ANN` from a one-year window; the value line white, the no-dividend line
dim, the benchmark dim and dotted; footer `DIV`, `TER~`, `CASH` and the bare
date (the AS OF label does not fit beside three items; STALE takes the CASH
item's room).

**Decision 3, the return label.** The window figure is the time-weighted return,
labelled `TWR`, from `preview_returns.py`: Portfolio Performance's daily
`1 + r = (MVE + CFout) / (MVB + CFin)`, linked geometrically, contributions
as external flows. With no flows it is the approved `chg` bit for bit (the
renderer asserts it on every such frame, the tests on every window). When
contributions are on, the footer is `XIRR` with its annualised value since
inception, then `DIV`.

**Decision 4, ANN** is the annualised TWR, `ANN +8.6%`, dim word, value in
its sign colour, right-aligned under the TWR, only for windows of a year or
longer. With no flows it equals the ledger's `cagr` exactly where `cagr` is
reported.

**Decision 2, the benchmark** is `^SP500TR` (S&P 500 total return), converted
to EUR and scaled to the capital at the inception, so the value line and the
benchmark both start at 10 000 on 2000-01-01: `capital x close(d) x X(d) /
(close(S) x X(S))` (`market_ref.benchmark_in`), with X from the ECB's
end-of-month rate before 2003-12 and Yahoo's EURUSD=X after. It ends at
75 067.55 EUR on 14 SEP 2026 (39 540.03 at the 5Y window's start). The
approved frames drew ^GSPC's price in USD, which ended at 55 766.01.

The real portfolio is VOO alone (`market_ref.PREVIEW_PORTFOLIO`), 100 %,
10 000 EUR from 2000-01-01. The capital is the reserved share of a fund not
yet trading until the first year-end after VOO's first bar (2010-10-01): the
2010-12 bar, 115.14 USD / 1.3301 = 86.5634 EUR, 115.52 shares; every later
year-end buys VOO with the dividends received. On 14 SEP 2026: value
89 800.95 EUR (`89 801`), MAX `TWR +798%` (+798.01 %), `ANN +8.6%` (+8.5676 %,
the CAGR), MDD -19.8 %, DIV 8 803.90 (`8.8K`), TER~ 170.51 (`171`), CASH
0.55 % (`0.6%`); 5Y from 2021-09-14: `TWR +89.3%`, `ANN +13.6%` (+13.6208 %),
MDD -15.6 %, DIV 4 267.55, TER~ 95.56; YTD +13.5 %. HOLD and REBAL are
identical here: with one position both modes put all cash into it at the
year-end. `portfolio_synth14_*` carry an example allocation
(`market_ref.EXAMPLE_ALLOCATION`, weights descending: 20, 15, 10, 10, 8, 7, 6,
5, 5, 4, 3, 3, 2, 2 %) on fourteen synthetic series `SYNTH01X`..`SYNTH14X`,
each a real sample's path raised to a power (^FCHI 0.7, ^GDAXI 0.8, VOO 0.9,
^GSPC 1.0, ^IXIC 1.6, AAPL 1.2, then 1.3, 0.7, ..): HOLD MAX 49 479.47
(`49 479`, `TWR +395%`, `ANN +6.2%`), REBAL MAX 43 005.52, HOLD 5Y +86.05 %,
REBAL 5Y `TWR +66.6%` (+66.56 %), `ANN +10.7%` (`43 006`); the VOO-based ones
enter 2010-12, the AAPL-based ones 2021-12-27, which is the reserved-cash rule
at work. Not a real portfolio. `portfolio_nodiv`: a single position without
dividends (the S&P path as an EUR fund, TER none, no benchmark): `DIV 0  TER~
0  CASH 0.0%`, the no-dividend line lies under the value line, 53 639 on 10Y,
`TWR +247%`, `ANN +13.2%`. `portfolio_stale`: `STALE 14 SEP` in amber,
`DIV 8.8K  TER~ 171`, ANN kept.

**`portfolio_contrib_max`, new:** VOO with 1 000 EUR a year on the inception's
anniversary (26 contributions to 2026-01-01), HOLD, MAX. Value 234 233.54 EUR
(`234.2K`), `TWR +765%` (+764.59 %: the contributions wait in cash until each
year-end, which costs a little), `ANN +8.4%` (+8.4136 %), footer
`XIRR +9.8%  DIV 22K` (XIRR +9.7907 %, DIV 21 663.12). XIRR beats the
annualised TWR because the capital sat in cash through 2000-2010 while most of
the money arrived later. The approved `chg` (value over the first value) would
have read +2 242 %, and `sinceStart` is +551 %: both count the contributions as
a return, which is why the TWR replaces it. The footer drops TER~ (423.16) and
CASH (0.98 %) for XIRR: with TER~ kept, a four-digit XIRR and six-figure amounts
overflow the footer by 8 columns. `DIV 22K` is also the case the old formatter
got wrong (`0.0M`).

**Decision 5, the drawdown stop** (`portfolio_hold_max_mdd`,
`portfolio_synth14_hold_max_mdd`, new): a click on PORTFOLIO. The footer
becomes `MDD -19.8%  20-01>20-03  REC 20-08`: the window's deepest fall, the
peak's and the trough's months, and the recovery month, or `NO REC` if the
value has not been back at the peak. The dates come from the ledger's daily V
(`preview_returns.drawdown`): the peak is the last day at the running peak
before the trough, the trough the day of the deepest ratio, the recovery the
first later day at or above the peak's value. On the chart the fall from the
peak's point to the trough's is redrawn in red on the value line, and a red
bracket under the chart (row 57, end ticks up to row 55) joins their columns.
The as-of date, pushed out of the footer, takes ANN's slot under the TWR; ANN
comes back with the next click. VOO MAX: -19.82 %, 2020-01-01 > 2020-03-01,
recovered 2020-08-01; on a 26.7-year window those are points 95 and 96, two
columns. The example allocation's HOLD MAX: `MDD -43.1%  00-03>03-02  REC
13-10`, points 0 to 14.

For the firmware: `marketKnobClick()` gives PORTFOLIO two stops today (WINDOW,
then leave). The drawdown is a third: WINDOW, MDD, leave, with the 10 s
timeout as now. The payload needs the dates: for instance `mddPeak`,
`mddTrough`, `mddRec` (null when not recovered) beside `mdd`, and `twr`, `ann`,
`xirr`, which the app computes; the panel finds a date's point as
`ceil(offset x 128 / span) - 1` (`render.slot_index`), the first point
`downsample()` shows it in.

## HOLDINGS

`holdings_hold`, `holdings_synth14_p1`, `holdings_synth14_p4`,
`holdings_synth14_p2_stale`. Heading `HOLDINGS n/m` + the window tag, the mode
at the right; rows: symbol in 5x7, `TGT>NOW` in Picopixel (whole percents),
the return since entry in 5x7 with its sign; `CASH` last; footer: the legend
`TGT>NOW` at the left, `AS OF 14 SEP` at the right. Sorted by target weight,
unchanged (decision 5). The real page: `VOO 100>99% +596%` (86.5634 ->
602.59 EUR per share), `CASH 0>1%` (0.55 %). The synthetic fourteen (the
example allocation) give 15 rows, four pages: page 1 `SYNTH01X 20>18% +28.7%`,
`SYNTH02X 15>14% +186%`, `SYNTH03X 10>10% +481%`, `SYNTH04X 10>10% +458%`
(REBAL); page 2 in HOLD, STALE: `SYNTH05X 8>35% +1 970%` (the four-digit
return), `SYNTH06X 7>3% +108%`, `SYNTH07X 6>2% +59.8%`, `SYNTH08X 5>3%
+151%`; page 4 `SYNTH13X 2>1% +54.2%`, `SYNTH14X 2>2% +452%`, `CASH 0>0%`.
The `ret` of the `holdings/<mode>` payload is since entry, not over the
window; the window tag stays in the heading because the checklist wants it
always visible.

## Decision 6: colours

No change: green up, red down. `ticker_gspc_1y_open_bluered` is
`ticker_gspc_1y_open` in the blue/red scheme as the firmware's registry has it
(`display.colors` = `blue_red`, `market_layout.cpp` `pctCol`): signed changes
blue 60/150/255 up (`+15.2%`), red down (`-0.5%`). The firmware recolours only
signed changes, so the tape's `OPEN` and the word `LIVE` stay green beside the
blue.

## Sources of the benchmark and the FX

**^SP500TR**, `samples/yahoo_SP500TR_1mo.json`, Yahoo chart v8 fetched
2026-09-15 12:21 CEST:
<https://query1.finance.yahoo.com/v8/finance/chart/%5ESP500TR?period1=946684800&period2=1789467706&interval=1mo&events=div%2Csplit>.
322 monthly bars; the first stamped 2000-01-01 (05:00 UTC), close 1 919.84;
the last 2026-09-14, close 17 081.82. `close` equals `adjclose` on every bar;
no null closes, no dividend events; meta `longName` "S&P 500 (TR)",
`firstTradeDate` 1988-01-04, USD. The first request, with a Safari 17
User-Agent, got HTTP 429; the coordinator measured at 12:20 that Yahoo answers
429 to Python's default and to a full Safari 17 string and 200 to
`Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36`, which
`fetch_samples.py` now sends and the second request used.

**ECB**, `samples/ecb_EXR_M_USD_EUR_SP00_E.csv`, the ECB Data Portal's SDMX API:
<https://data-api.ecb.europa.eu/service/data/EXR/M.USD.EUR.SP00.E?format=csvdata&startPeriod=1999-01&endPeriod=2004-12>,
72 months, "ECB reference exchange rate, US dollar/Euro, 2.15 pm (C.E.T.)";
series page <https://data.ecb.europa.eu/data/datasets/EXR/EXR.M.USD.EUR.SP00.E>.
The key's last part is the series variation. The API's code list
<https://data-api.ecb.europa.eu/service/codelist/ECB/CL_EXR_SUFFIX> names `E`
"End-of-period" and `A` "Average". Checked against the data: fetched beside
`EXR.D.USD.EUR.SP00.A` (daily), all 72 `E` months equal the month's last daily
rate and all 72 `A` months the mean of its daily rates. A monthly Yahoo bar
closes at the month's end, so `E` is used; docs/18 names `A`. The table is
`market_ref.ecb_monthly_table()`, FxTable's `ecb` hook: 2000-01 answers 0.9791
USD per EUR, 2003-11 1.1994; from 2003-12-01 Yahoo answers; 1998-12 still
refuses. The benchmark across the join: 6 556.95 on 2003-11-01 (ECB),
6 574.13 on 2003-12-01 (Yahoo).

Where both have the month, Yahoo's monthly EURUSD=X close against the ECB's
end-of-period rate (not explained here; the ECB fixes at 14:15 CET, which
accounts for a few basis points, not 90):

| Month | ECB E | Yahoo close | Yahoo - ECB | bp |
|---|---|---|---|---|
| 2003-12 | 1.2630 | 1.259002 | -0.003998 | -31.7 |
| 2004-01 | 1.2384 | 1.245206 | +0.006806 | +55.0 |
| 2004-02 | 1.2418 | 1.253007 | +0.011207 | +90.2 |
| 2004-03 | 1.2224 | 1.231300 | +0.008900 | +72.8 |
| 2004-04 | 1.1947 | 1.198294 | +0.003594 | +30.1 |
| 2004-05 | 1.2198 | 1.219899 | +0.000099 | +0.8 |
| 2004-06 | 1.2155 | 1.218398 | +0.002898 | +23.8 |
| 2004-07 | 1.2039 | 1.201605 | -0.002295 | -19.1 |
| 2004-08 | 1.2111 | 1.217804 | +0.006704 | +55.4 |
| 2004-09 | 1.2409 | 1.243394 | +0.002494 | +20.1 |
| 2004-10 | 1.2737 | 1.283203 | +0.009503 | +74.6 |
| 2004-11 | 1.3295 | 1.329805 | +0.000305 | +2.3 |
| 2004-12 | 1.3621 | 1.356502 | -0.005598 | -41.1 |

## The returns, `preview_returns.py`

An independent oracle: the HA app computes the same figures its own way, and
the two are cross-checked later. Plain lists, the standard library.
- **TWR**: the formula above, from
  <https://help.portfolio-performance.info/en/concepts/performance/time-weighted/>
  (CFin at the start of the day, CFout at its end before the valuation). The
  product is taken stretch by stretch between flows, where it telescopes: the
  same number, and exactly last / first with no flow.
- **ANN**: `growth^(1 / years) - 1` only for a window of a year or more (GIPS
  2020 2.A.12, as docs/19 section 4 quotes it), years = days / 365.25,
  market_ref's year, so that it equals `cagr`. Portfolio Performance
  annualises over 365 days; the app's choice is to be matched in the
  cross-check.
- **XIRR**: Microsoft's definition, `sum P_i / (1 + r)^((d_i - d_1) / 365) =
  0` (<https://support.microsoft.com/en-us/office/xirr-function-de1242ec-6477-445b-b11b-a303ad9adc9d>),
  the capital and each contribution out on the day they entered the ledger,
  the value back on the last day; bisection, so no second root and the same
  bits every run.
- `test_preview_returns.py`: 15 tests with the arithmetic in the docstrings.

## Layout budget

`layout_budget()` fills every slot the decisions added with its widest string
(the widest Picopixel digit and month, a four-digit percent, a 3-digit delay,
six-figure amounts, an 8-character symbol with the widest tag and currency)
and refuses an overlap. Blank columns left:

| Slot | Blank columns |
|---|---|
| PORTFOLIO row 16: 2x slot, then `TWR` and the change | 3 |
| PORTFOLIO row 25: 2x slot, then `ANN` | 16 |
| PORTFOLIO drawdown stop, row 25: 2x slot, then `STALE dd MON` | 12 |
| PORTFOLIO drawdown stop, footer `MDD .. REC YY-MM` to the right edge | 1 |
| PORTFOLIO contributions footer `XIRR .. DIV ..`, then the date | 31 |
| PORTFOLIO contributions STALE footer `XIRR ..`, then `STALE dd MON` | 41 |
| PORTFOLIO footer as approved, `DIV TER~ CASH`, then the date | 4 |
| TICKER heading, 8-character symbol, tag, currency, then `ANN` | 3 |
| TICKER row 25: 2x slot, then `D999` and the day change | 2 |
| MARKETS row 33: 2x slot, then `D999` and the day change | 14 |

## Listings

From the samples' first bars: VOO 2010-10-01 (the monthly sample; the fund
listed 2010-09), AAPL 2021-09-13 (the 5-year weekly sample, not a listing),
IWDA.AS 2009-09-01 (a parser fixture only, not in any preview), ^GSPC
1984-12-01, ^FCHI 1990-03-01, ^IXIC and ^GDAXI 2000-01-01, ^SP500TR 2000-01-01
(the request's start; Yahoo's `firstTradeDate` 1988-01-04), EURUSD=X
2003-12-01, the ECB table 1999-01. `fetch_samples.py` fetches a fund's monthly
sample once Yahoo allows; `preview_config()` uses the funds of
`PREVIEW_PORTFOLIO` that have one.

## The checklist, again

1. The primary number reads from 3 m in 2 s: yes, one 2x figure per page, white on black; PORTFOLIO's slot went from 7 to 6 characters, which changes no drawn value under 100 000 and prints `234.2K` above.
2. No more than two large numbers on a page: yes, one 2x number; TWR, ANN, XIRR and MDD are 5x7 or Picopixel.
3. The window tag is always visible: yes, in every heading in amber, the drawdown stop and DATA ERR included.
4. AS OF, STALE or DATA ERR is always visible: yes; in the drawdown stop the bare date moves from the footer to the row under the TWR, `STALE dd MON` included (12 columns to spare).
5. Every change carries a sign; zero is not green: yes, TWR, ANN, XIRR, MDD and the day change behind `D15` all go through `fmt_pct` and `pct_col`.
6. The main line and the no-dividend line cannot be confused: yes, white against dim; the benchmark stays dim and dotted; the drawdown's red segment appears only in its stop.
7. Consistent with the rail and flight boards: yes, black ground, amber headings, white primary, dim labels.
8. At most four semantic colours on a page: yes as before (amber, white, dim, the sign colour; green and red counted apart make five on a mixed page); `D15` is dim, the drawdown marks use the sign's red, and the blue/red frame swaps green for blue on the changes while OPEN and LIVE stay green, as in the firmware.
9. Nothing written inside the chart; HI/LO in the footer: yes, the drawdown marks are a red segment and a bracket, no text.
10. No motion that imitates a live feed: yes; `LIVE` now appears only for a quote under 2 minutes old, and a delayed quote says `D15`.
11. Picopixel only for secondary text: yes, the new words and ANN, XIRR, MDD and the dates are Picopixel; the TWR value stays 5x7 as the approved change.
12. Number formats do not jump between frames; worst-case strings fit: yes, formats depend only on magnitude and slot; the budget above holds every new worst case; `fmt_amount` now falls back to whole K / M instead of printing `0.0M` for 10 000-999 499 in four characters.

## Frames changed vs 3024545

Pixels changed, regenerated in place (16):
- `portfolio_hold_max`, `portfolio_rebal_max`, `portfolio_hold_5y`, `portfolio_rebal_5y`, `portfolio_stale`: the 6-character slot, `TWR`, `ANN`, the benchmark line now ^SP500TR in EUR (and so the shared scale of the lines).
- `portfolio_synth14_hold_max`, `portfolio_synth14_rebal_5y`: the example allocation replaces the previous weights, plus the same four changes.
- `portfolio_nodiv`: `TWR`, `ANN` (it has no benchmark).
- `holdings_synth14_p1`, `holdings_synth14_p4`, `holdings_synth14_p2_stale`: the example allocation.
- `ticker_voo_max`, `ticker_voo_5y`, `ticker_aapl_short`, `ticker_worst_neg`, `ticker_worst_pct`: `ANN` at the heading's right.

New (7): `markets_max_open_d15`, `markets_1y_dax_d15`,
`ticker_gspc_1y_open_d15`, `ticker_gspc_1y_open_bluered`,
`portfolio_hold_max_mdd`, `portfolio_synth14_hold_max_mdd`,
`portfolio_contrib_max`.

Pixel-identical (9): `markets_max_open`, `markets_5y_closed`, `markets_1y_dax`,
`markets_stale`, `markets_err`, `ticker_voo_ytd`, `ticker_gspc_1y_open`,
`ticker_err`, `holdings_hold`.

Beside the pixels, for the firmware step: `frames.json` gains `ann`, `twr`,
`xirr`, `mdd_dates`, `marks`, `bench`, `stop`, `lag_s`, `colors` per frame and
`budget`, `bench`, `lags_s`, `example_allocation` in `_meta`; PORTFOLIO's `chg`
string is the TWR (the same string wherever nothing flows); the new `MK_*`
constants and words above; `fmt_amount`'s whole-K fallback; the knob's third
PORTFOLIO stop; the delay badge rule replacing `DELAYED`.

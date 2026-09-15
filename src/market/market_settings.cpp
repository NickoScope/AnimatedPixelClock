#include "market_settings.h"

// One's own defaults (market_settings.h): a named file, else the uncommitted
// header beside this one, unless told to ignore it.
#if defined(MARKET_LOCAL_DEFAULTS_FILE)
#include MARKET_LOCAL_DEFAULTS_FILE
#define MARKET_HAVE_LOCAL_DEFAULTS 1
#elif !defined(MARKET_NO_LOCAL_DEFAULTS) && defined(__has_include)
#if __has_include("market_local_defaults.h")
#include "market_local_defaults.h"
#define MARKET_HAVE_LOCAL_DEFAULTS 1
#endif
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mks {

using market::kNoDate;
using market::kSymLen;
using market::kNameLen;
using market::kExNameLen;

const char *const kTypeKeys[T_COUNT] = {"bool", "int", "num", "enum", "date", "time", "sym", "symlist", "poslist", "wlist",
                                        "termap", "namelist", "json"};
const char *const kGroupKeys[G_COUNT] = {"watch", "portfolio", "display", "returns", "rebal", "flows", "fees", "currency",
                                         "bench", "risk", "display_adv", "data", "alerts", "extra"};
const char *const kGroupLabels[G_COUNT] = {"Watch", "Portfolio", "Display", "Returns", "Rebalancing", "Cash & flows",
                                           "Fees & taxes", "Currency", "Benchmark", "Risk", "Display (more)", "Data",
                                           "Alerts", "Pass-through"};
const char *const kAssetClassKeys[AC_COUNT] = {"", "equity", "bond", "real_assets", "gold", "cash"};

// The neutral allocation: 60 % US stocks, 40 % US bonds, in two index funds
// whose Yahoo history starts before 2000 (the coordinator's probe of
// 2026-09-15: VFINX and VBMFX from 2000-01-01 with adjusted closes and
// dividends; not re-checked here). SYM:WEIGHT:ENTRY:CLASS, the entry empty.
static const char kPositions[] = "VFINX:60::equity,VBMFX:40::bond";

// key, path, type, group, min, max, decimals, cap, default, options, label, hint, unit
const Def kDefs[I_COUNT] = {
  // Watch
  {"indices", nullptr, T_SYMLIST, G_WATCH, 0, kMaxWatch, 0, 192, "^GSPC=SPX,^IXIC=NDX,^FCHI=CAC,^GDAXI=DAX", nullptr, "Indices",
   "The MARKETS page: the first is primary, the next three are the rows. A symbol and the short name the panel draws.", nullptr},
  {"tickers", nullptr, T_SYMLIST, G_WATCH, 0, kMaxWatch, 0, 192, "VFINX=VFINX,VBMFX=VBMFX", nullptr, "Tickers",
   "The TICKER page steps through these.", nullptr},
  // Portfolio
  {"portfolio.capital", nullptr, T_NUM, G_PORTFOLIO, 100, 1e9f, 2, 0, "10000", nullptr, "Initial capital",
   "Invested on the first trading day at or after the inception, in the portfolio currency.", nullptr},
  {"portfolio.currency", nullptr, T_ENUM, G_PORTFOLIO, 0, 0, 0, 0, "EUR", "EUR|USD", "Currency",
   "Every fund in its own currency; the portfolio in this one.", nullptr},
  {"portfolio.inception", nullptr, T_DATE, G_PORTFOLIO, 0, 0, 0, 11, "2000-01-01", nullptr, "Inception",
   "The start of the backtest. MAX shows from here; the other windows only change what is visible.", nullptr},
  {"rebal.mode", nullptr, T_ENUM, G_PORTFOLIO, 0, 0, 0, 0, "hold", "hold|calendar|bands|calendar_or_bands", "Rebalancing",
   "HOLD never sells and puts the cash to work each 31 December; the others sell and buy back to the target weights on a calendar, when a weight leaves its band, or either. The panel shows the chosen model.", nullptr},
  {"portfolio.positions", nullptr, T_POSLIST, G_PORTFOLIO, 0, kMaxPositions, 2, 912, kPositions, nullptr, "Allocation",
   "Target weight per fund, at most 100 % together; the rest stays in cash. Per row: an entry date buys at that day's close instead of the inception; an asset class groups the read-out; a proxy backfills a young fund with an older series.", "%"},
  // Display
  {"display.window", "", T_ENUM, G_DISPLAY, 0, 0, 0, 0, "MAX", "WTD|MTD|1M|3M|6M|YTD|1Y|3Y|5Y|10Y|MAX", "Window",
   "The chart window on every market page. The knob changes it too; both are kept. The extra presets need switching on under Display (more).", nullptr},
  {"display.ticker", "ticker", T_SYM, G_DISPLAY, 0, 0, 0, 13, "VFINX", nullptr, "Ticker on screen",
   "One of the tickers: the one the TICKER page shows and the app polls intraday. The knob changes it too.", nullptr},
  {"tape.on", "", T_BOOL, G_DISPLAY, 0, 0, 0, 0, "1", nullptr, "Exchange tape",
   "The scrolling row of exchanges and their state on top of every market page.", nullptr},
  {"tape.exchanges", nullptr, T_NAMELIST, G_DISPLAY, 0, kMaxTape, 0, 96, "NYSE,NASDAQ,LSE,XETRA,EURONEXT,TOKYO", nullptr,
   "Exchanges on the tape", "In this order. The app knows these names' calendars.", nullptr},
  {"display.lines.px", "", T_BOOL, G_DISPLAY, 0, 0, 0, 0, "1", nullptr, "No-dividend line",
   "PORTFOLIO: the same ledger with the dividends dropped, dim under the value line.", nullptr},
  {"display.lines.bench", "", T_BOOL, G_DISPLAY, 0, 0, 0, 0, "1", nullptr, "Benchmark line",
   "PORTFOLIO: the benchmark scaled to the capital, dotted.", nullptr},
  {"display.session", "", T_BOOL, G_DISPLAY, 0, 0, 0, 0, "1", nullptr, "Session strip",
   "TICKER: today's intraday line under the window chart while the exchange is open.", nullptr},
  {"display.dwell_s", "", T_INT, G_DISPLAY, 5, 60, 0, 0, "20", nullptr, "Seconds per page",
   "How long the carousel holds each market page.", "s"},
  // Returns
  {"returns.measure", nullptr, T_ENUM, G_RETURNS, 0, 0, 0, 0, "twr", "twr|mwr", "Return measure",
   "Time-weighted (the terminals') or money-weighted (XIRR, the trackers'). With no real cash flows the backtest's TWR is the plain figure.", nullptr},
  {"returns.real", nullptr, T_BOOL, G_RETURNS, 0, 0, 0, 0, "0", nullptr, "Real returns", "Divide by the consumer price index.", nullptr},
  {"dividends.mode", nullptr, T_ENUM, G_RETURNS, 0, 0, 0, 0, "sweep_yearend", "sweep_yearend|reinvest_paydate|drop", "Dividends",
   "Swept to cash and invested each 31 December (docs/18's ledger), reinvested on the pay date (Portfolio Visualizer's), or dropped.", nullptr},
  // Rebalancing
  {"rebal.calendar", nullptr, T_ENUM, G_REBAL, 0, 0, 0, 0, "annual", "annual|semiannual|quarterly|monthly", "Calendar",
   "When a calendar rebalance runs; annual is 31 December.", nullptr},
  {"rebal.bands.abs_pts", nullptr, T_NUM, G_REBAL, 0, 50, 1, 0, "5", nullptr, "Band, absolute",
   "A weight this many points off its target is out of band (the 5/25 rule).", "pts"},
  {"rebal.bands.rel_pct", nullptr, T_NUM, G_REBAL, 0, 100, 0, 0, "25", nullptr, "Band, relative",
   "Or this far off in relative terms, for small positions.", "%"},
  {"rebal.bands.check", nullptr, T_ENUM, G_REBAL, 0, 0, 0, 0, "monthly", "daily|weekly|monthly", "Band check", "How often drift is tested.", nullptr},
  {"rebal.contrib_only", nullptr, T_BOOL, G_REBAL, 0, 0, 0, 0, "0", nullptr, "Rebalance with contributions only",
   "New cash buys the underweight positions; nothing is sold.", nullptr},
  // Cash and flows
  {"contrib.amount", "portfolio.contrib.amount", T_NUM, G_FLOWS, 0, 1e8f, 2, 0, "0", nullptr, "Contribution",
   "Added to the cash on schedule, in the portfolio currency. 0 = none.", nullptr},
  {"contrib.every", "portfolio.contrib.every", T_ENUM, G_FLOWS, 0, 0, 0, 0, "year", "month|quarter|year", "Contribution period", nullptr, nullptr},
  {"contrib.index_inflation", nullptr, T_BOOL, G_FLOWS, 0, 0, 0, 0, "0", nullptr, "Index contributions to inflation", nullptr, nullptr},
  {"withdraw.amount", nullptr, T_NUM, G_FLOWS, 0, 1e8f, 2, 0, "0", nullptr, "Withdrawal", "Taken from the portfolio on schedule. 0 = none.", nullptr},
  {"withdraw.kind", nullptr, T_ENUM, G_FLOWS, 0, 0, 0, 0, "amount", "amount|pct", "Withdrawal as", "An amount in the portfolio currency, or a percent of the balance.", nullptr},
  {"withdraw.every", nullptr, T_ENUM, G_FLOWS, 0, 0, 0, 0, "year", "month|quarter|year", "Withdrawal period", nullptr, nullptr},
  {"cash.yield_pct", nullptr, T_NUM, G_FLOWS, 0, 20, 2, 0, "0", nullptr, "Cash yield", "Interest on the cash row, per year.", "%/y"},
  // Fees and taxes
  {"ter", nullptr, T_TERMAP, G_FEES, 0, kMaxTer, 2, 336, "", nullptr, "TER overrides",
   "Expense ratio per fund in percent a year, for funds Yahoo lacks or gets wrong. Prices are already net of fees; TER~ is an estimate with today's ratio.", "%/y"},
  {"display.lines.gross", "", T_BOOL, G_FEES, 0, 0, 0, 0, "0", nullptr, "Gross-of-fees line",
   "PORTFOLIO: the counterfactual without the funds' fees, dotted, when the app sends it. An estimate.", nullptr},
  {"tax.div_withholding_pct", nullptr, T_NUM, G_FEES, 0, 100, 1, 0, "0", nullptr, "Dividend withholding", "Taken off every dividend before it reaches the cash.", "%"},
  {"costs.per_trade_fixed", nullptr, T_NUM, G_FEES, 0, 1000, 2, 0, "0", nullptr, "Cost per trade, fixed", "In the portfolio currency, on every buy and sell.", nullptr},
  {"costs.per_trade_pct", nullptr, T_NUM, G_FEES, 0, 5, 3, 0, "0", nullptr, "Cost per trade, percent", nullptr, "%"},
  // Currency
  {"display.fx_effect", nullptr, T_BOOL, G_CURRENCY, 0, 0, 0, 0, "0", nullptr, "Show the currency effect",
   "The part of the return that is the exchange rate, as one footer figure.", nullptr},
  // Benchmark
  {"bench.mode", nullptr, T_ENUM, G_BENCH, 0, 0, 0, 0, "single", "single|blend", "Benchmark", "One index, or a blend of several.", nullptr},
  {"bench.symbol", nullptr, T_SYM, G_BENCH, 0, 0, 0, 13, "^SP500TR", nullptr, "Benchmark symbol",
   "Total return: ^SP500TR is the S&P 500 with dividends. The app converts it to the portfolio currency and scales it to the capital at the inception.", nullptr},
  {"bench.blend", nullptr, T_WLIST, G_BENCH, 0, kMaxBlend, 2, 160, "", nullptr, "Blend",
   "Symbols and weights, at most 100 % together, on adjusted closes. Presets: 60/40 (VFINX 60, VBMFX 40: the shape of Vanguard's Balanced Composite), and the portfolio's own weights.", "%"},
  {"bench.telltale", nullptr, T_BOOL, G_BENCH, 0, 0, 0, 0, "0", nullptr, "Telltale", "PORTFOLIO: the value divided by the benchmark, as a line.", nullptr},
  // Risk
  {"mdd_basis", nullptr, T_ENUM, G_RISK, 0, 0, 0, 0, "daily", "daily|month_end", "Drawdown basis", "Daily closes, or month ends as Portfolio Visualizer.", nullptr},
  {"stats.risk_free_pct", nullptr, T_NUM, G_RISK, 0, 20, 2, 0, "0", nullptr, "Risk-free rate", "For the risk-adjusted figures in Home Assistant.", "%/y"},
  // Display (more)
  {"presets.wtd", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Window WTD", "Week to date, on the knob when the app publishes it.", nullptr},
  {"presets.mtd", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Window MTD", "Month to date.", nullptr},
  {"presets.1m", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Window 1M", nullptr, nullptr},
  {"presets.3m", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Window 3M", nullptr, nullptr},
  {"presets.6m", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Window 6M", nullptr, nullptr},
  {"display.scale", nullptr, T_ENUM, G_DISPLAY_ADV, 0, 0, 0, 0, "linear", "linear|log", "Chart scale", "Log for the long windows; the app scales the lines.", nullptr},
  {"display.colors", "", T_ENUM, G_DISPLAY_ADV, 0, 0, 0, 0, "green_red", "green_red|blue_red|red_up", "Sign colours",
   "Green up and red down; blue and red for colour-blind readers (Bloomberg's scheme); red up as in East Asia.", nullptr},
  {"display.pages.markets", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "1", nullptr, "Page MARKETS", "Visited by the knob and the carousel.", nullptr},
  {"display.pages.ticker", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "1", nullptr, "Page TICKER", nullptr, nullptr},
  {"display.pages.portfolio", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "1", nullptr, "Page PORTFOLIO", nullptr, nullptr},
  {"display.pages.holdings", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "1", nullptr, "Page HOLDINGS", nullptr, nullptr},
  {"display.lines.value", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "1", nullptr, "Value line", "PORTFOLIO: the bright line itself.", nullptr},
  {"tape.speed", "", T_INT, G_DISPLAY_ADV, 0, 3, 0, 0, "1", nullptr, "Tape speed", "Pixels per frame at 20 frames a second; 0 holds the tape still.", "px"},
  {"holdings.sort", "", T_ENUM, G_DISPLAY_ADV, 0, 0, 0, 0, "target", "target|contribution", "Holdings order",
   "The allocation's own order, or by current share, largest first.", nullptr},
  {"holdings.band_mark", "", T_BOOL, G_DISPLAY_ADV, 0, 0, 0, 0, "0", nullptr, "Mark holdings out of band",
   "HOLDINGS: TGT>NOW in amber when the weight is outside its rebalancing band.", nullptr},
  {"feed.label", "", T_ENUM, G_DISPLAY_ADV, 0, 0, 0, 0, "delayed_badge", "live|delayed_badge", "Quote label",
   "delayed_badge: LIVE only while the quote is under 2 minutes behind the clock, else D and the minutes behind in dim (D15: Yahoo's European quotes ran 15 min behind). live: LIVE whatever the delay.", nullptr},
  // Data
  {"live.poll_s", nullptr, T_INT, G_DATA, 30, 300, 0, 0, "60", nullptr, "Live refresh",
   "How often the app polls quotes while an exchange is open. Yahoo's delayed price moves once a minute.", "s"},
  {"fetch.daily_at", nullptr, T_TIME, G_DATA, 0, 0, 0, 6, "07:00", nullptr, "Daily fetch", "When the app refreshes the history, local time.", nullptr},
  {"stale.session_min", nullptr, T_INT, G_DATA, 5, 120, 0, 0, "20", nullptr, "Quote stale after",
   "In a session, a quote older than this is STALE.", "min"},
  {"stale.history_days", nullptr, T_INT, G_DATA, 1, 10, 0, 0, "3", nullptr, "History stale after",
   "AS OF older than this many trading days reads STALE, on the panel too.", "days"},
  // Alerts
  {"alert.day_move_pct", nullptr, T_NUM, G_ALERTS, 0, 50, 1, 0, "0", nullptr, "Day move alert", "Home Assistant flags a symbol moving more than this in a day. 0 = off.", "%"},
  {"alert.drawdown_pct", nullptr, T_NUM, G_ALERTS, 0, 90, 1, 0, "0", nullptr, "Drawdown alert", "Flag the portfolio this far below its peak. 0 = off.", "%"},
  {"alert.out_of_band", nullptr, T_BOOL, G_ALERTS, 0, 0, 0, 0, "0", nullptr, "Out-of-band alert", "Flag a holding outside its band.", nullptr},
  // Pass-through
  {"extra", "", T_JSON, G_EXTRA, 0, 0, 0, 512, "{}", nullptr, "Passed through to the app",
   "Settings this firmware does not know, kept as they are and published in the config payload.", nullptr},
};

// ── the pool ────────────────────────────────────────────────────────────────
// Summed on each call, at most 66 small additions from the table in flash:
// cheaper than a table of offsets in RAM.
size_t poolOffset(Idx i) {
  size_t o = 0;
  for (int k = 0; k < i && k < I_COUNT; k++) o += kDefs[k].cap;
  return o;
}
size_t poolSize()          { return poolOffset(I_COUNT); }
bool   poolFits()          { return poolSize() <= sizeof(Settings::pool); }

// ── scratch and memory ──────────────────────────────────────────────────────
static Scratch *s_sc = nullptr;
static ArduinoJson::Allocator *s_json = nullptr;

void setScratch(Scratch *sc) { s_sc = sc; }
void setJsonAllocator(ArduinoJson::Allocator *a) { s_json = a; }

static Scratch *sc() {
  if (!s_sc) s_sc = static_cast<Scratch *>(calloc(1, sizeof(Scratch)));   // the host; the panel sets one first
  return s_sc;
}

static ArduinoJson::Allocator *jalloc() { return s_json ? s_json : ArduinoJson::detail::DefaultAllocator::instance(); }

static bool isString(Type t) { return t >= T_DATE; }

// ── options ─────────────────────────────────────────────────────────────────
const char *optionName(Idx i, int option) {
  char *buf = sc()->option;
  const size_t bufCap = sizeof(sc()->option);
  const char *o = kDefs[i].opts;
  if (!o || option < 0) return "";
  for (int k = 0; k < option; k++) {
    o = strchr(o, '|');
    if (!o) return "";
    o++;
  }
  const char *end = strchr(o, '|');
  const size_t n = end ? (size_t)(end - o) : strlen(o);
  if (n >= bufCap) return "";
  memcpy(buf, o, n);
  buf[n] = '\0';
  return buf;
}

int optionIndex(Idx i, const char *name) {
  const char *o = kDefs[i].opts;
  if (!o || !name) return -1;
  for (int k = 0; *o; k++) {
    const char *end = strchr(o, '|');
    const size_t n = end ? (size_t)(end - o) : strlen(o);
    if (strlen(name) == n && !strncmp(o, name, n)) return k;
    if (!end) break;
    o = end + 1;
  }
  return -1;
}

// ── text forms ──────────────────────────────────────────────────────────────
// The next field up to `sep` or the end; false when the input is used up.
static bool field(const char **p, char sep, char *out, size_t cap, bool *more) {
  if (!**p) { *more = false; return false; }
  size_t k = 0;
  while (**p && **p != sep) {
    if (k + 1 >= cap) return false;
    out[k++] = **p;
    (*p)++;
  }
  out[k] = '\0';
  *more = **p == sep;
  if (*more) (*p)++;
  return true;
}

static bool number(const char *s, float *out) {
  if (!*s) return false;
  char *end;
  const double v = strtod(s, &end);
  if (*end || !std::isfinite(v)) return false;
  *out = (float)v;
  return true;
}

static bool dup(char syms[][kSymLen], int n, const char *sym) {
  for (int i = 0; i < n; i++)
    if (!strcmp(syms[i], sym)) return true;
  return false;
}

// -Wformat-truncation in the parsers below is a false alarm: every field is
// checked (validSymbol, validName, validExchange) to fit its buffer before
// the snprintf that copies it, which gcc cannot see through.
int parseSymList(const char *s, char syms[][kSymLen], char names[][kNameLen], int cap) {
  int n = 0;
  if (!s) return -1;
  while (*s) {
    char item[kSymLen + kNameLen + 2];
    bool more;
    if (!field(&s, ',', item, sizeof(item), &more)) return -1;
    char *eq = strchr(item, '=');
    if (eq) *eq++ = '\0';
    if (!market::validSymbol(item) || (eq && !market::validName(eq))) return -1;
    if (n >= cap || dup(syms, n, item)) return -1;
    snprintf(syms[n], kSymLen, "%s", item);
    snprintf(names[n], kNameLen, "%s", eq && *eq ? eq : item);
    n++;
    if (!more && *s) return -1;
  }
  return n;
}

static int assetClass(const char *s) {
  for (int i = 0; i < AC_COUNT; i++)
    if (!strcmp(s, kAssetClassKeys[i])) return i;
  return -1;
}

int parsePositions(const char *s, Position *out, int cap) {
  int n = 0;
  double sum = 0;
  char syms[kMaxPositions][kSymLen];
  if (!s) return -1;
  while (*s) {
    char item[80];
    bool more;
    if (!field(&s, ',', item, sizeof(item), &more)) return -1;
    if (n >= cap) return -1;
    Position &p = out[n];
    memset(&p, 0, sizeof(p));
    p.entry = kNoDate;
    const char *q = item;
    char f[5][kSymLen + 4];
    int nf = 0;
    bool m2 = true;
    while (m2 && nf < 5) {
      if (!field(&q, ':', f[nf], sizeof(f[nf]), &m2)) break;
      nf++;
    }
    if (nf < 2 || m2) return -1;
    if (!market::validSymbol(f[0]) || dup(syms, n, f[0])) return -1;
    if (!number(f[1], &p.w) || p.w <= 0 || p.w > 100) return -1;
    if (nf > 2 && f[2][0] && (p.entry = market::parseDate(f[2])) == kNoDate) return -1;
    int ac = 0;
    if (nf > 3 && (ac = assetClass(f[3])) < 0) return -1;
    if (nf > 4 && f[4][0] && !market::validSymbol(f[4])) return -1;
    snprintf(p.sym, sizeof(p.sym), "%s", f[0]);
    if (nf > 4) snprintf(p.proxy, sizeof(p.proxy), "%s", f[4]);
    p.assetClass = (uint8_t)ac;
    snprintf(syms[n], kSymLen, "%s", f[0]);
    sum += p.w;
    n++;
    if (!more && *s) return -1;
  }
  return sum <= 100.005 ? n : -1;
}

static int parsePairs(const char *s, char sep, char syms[][kSymLen], float *val, int cap, float lo, float hi, bool sumCap) {
  int n = 0;
  double sum = 0;
  if (!s) return -1;
  while (*s) {
    char item[kSymLen + 16];
    bool more;
    if (!field(&s, ',', item, sizeof(item), &more)) return -1;
    char *at = strchr(item, sep);
    if (!at) return -1;
    *at++ = '\0';
    if (n >= cap || !market::validSymbol(item) || dup(syms, n, item)) return -1;
    if (!number(at, &val[n]) || val[n] < lo || val[n] > hi) return -1;
    snprintf(syms[n], kSymLen, "%s", item);
    sum += val[n];
    n++;
    if (!more && *s) return -1;
  }
  return (!sumCap || sum <= 100.005) ? n : -1;
}

int parseWeights(const char *s, char syms[][kSymLen], float *w, int cap) { return parsePairs(s, ':', syms, w, cap, 0.0001f, 100, true); }
int parseTer(const char *s, char syms[][kSymLen], float *pct, int cap)  { return parsePairs(s, '=', syms, pct, cap, 0, 10, false); }

int parseNames(const char *s, char names[][kExNameLen], int cap) {
  int n = 0;
  if (!s) return -1;
  while (*s) {
    char item[kExNameLen + 2];
    bool more;
    if (!field(&s, ',', item, sizeof(item), &more)) return -1;
    if (n >= cap || !market::validExchange(item)) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], item)) return -1;
    snprintf(names[n++], kExNameLen, "%s", item);
    if (!more && *s) return -1;
  }
  return n;
}

bool validTime(const char *s) {
  if (!s || strlen(s) != 5 || s[2] != ':') return false;
  for (int i = 0; i < 5; i++) if (i != 2 && (s[i] < '0' || s[i] > '9')) return false;
  const int h = atoi(s), m = atoi(s + 3);
  return h < 24 && m < 60;
}

// A bound as the portal shows it: whole numbers plain, never 1e+09.
static const char *bound(float v, char *buf, size_t cap) {
  if (v == (float)(long)v && std::fabs(v) < 1e12f) snprintf(buf, cap, "%ld", (long)v);
  else snprintf(buf, cap, "%g", (double)v);
  return buf;
}

static float rounded(float v, uint8_t decimals) {
  double k = 1;
  for (uint8_t i = 0; i < decimals; i++) k *= 10;
  return (float)(std::nearbyint((double)v * k) / k);
}


static bool validText(Idx i, const char *text) {
  const Def &d = kDefs[i];
  if (strlen(text) >= d.cap) return false;
  switch (d.type) {
  case T_DATE:     return market::parseDate(text) != kNoDate;
  case T_TIME:     return validTime(text);
  case T_SYM:      return !*text || market::validSymbol(text);
  case T_SYMLIST:  return parseSymList(text, sc()->syms, sc()->names, (int)d.max) >= 0;
  case T_POSLIST:  return parsePositions(text, sc()->pos, (int)d.max) >= 0;
  case T_WLIST:    return parseWeights(text, sc()->syms, sc()->vals, (int)d.max) >= 0;
  case T_TERMAP:   return parseTer(text, sc()->syms, sc()->vals, (int)d.max) >= 0;
  case T_NAMELIST: return parseNames(text, sc()->ex, (int)d.max) >= 0;
  case T_JSON: {
    JsonDocument doc(jalloc());
    return !deserializeJson(doc, text) && doc.is<JsonObjectConst>();
  }
  default: return false;
  }
}

const char *fromText(Settings &s, Idx i, const char *text) {
  const Def &d = kDefs[i];
  if (!text) return "empty";
  char *end;
  switch (d.type) {
  case T_BOOL:
    if (strcmp(text, "0") && strcmp(text, "1")) return "not 0 or 1";
    s.v[i].i = text[0] == '1';
    return nullptr;
  case T_INT: {
    const long v = strtol(text, &end, 10);
    if (*end || !*text || v < (long)d.min || v > (long)d.max) return "out of range";
    s.v[i].i = (int32_t)v;
    return nullptr;
  }
  case T_NUM: {
    float v;
    if (!number(text, &v) || v < d.min || v > d.max) return "out of range";
    s.v[i].f = rounded(v, d.decimals);
    return nullptr;
  }
  case T_ENUM: {
    const int k = optionIndex(i, text);
    if (k < 0) return "not an option";
    s.v[i].i = k;
    return nullptr;
  }
  default:
    if (!validText(i, text)) return "not valid";
    snprintf(str(s, i), d.cap, "%s", text);
    return nullptr;
  }
}

bool toText(const Settings &s, Idx i, char *out, size_t cap) {
  const Def &d = kDefs[i];
  int n;
  switch (d.type) {
  case T_BOOL: n = snprintf(out, cap, "%d", s.v[i].i ? 1 : 0); break;
  case T_INT:  n = snprintf(out, cap, "%ld", (long)s.v[i].i); break;
  case T_NUM:  n = snprintf(out, cap, "%.*f", d.decimals, (double)s.v[i].f); break;
  case T_ENUM: n = snprintf(out, cap, "%s", optionName(i, s.v[i].i)); break;
  default:     n = snprintf(out, cap, "%s", str(s, i)); break;
  }
  return n >= 0 && (size_t)n < cap;
}

const char *defaultText(Idx i) {
#if defined(MARKET_HAVE_LOCAL_DEFAULTS)
  for (const LocalDefault &l : kLocalDefaults)
    if (l.key && l.text && !strcmp(l.key, kDefs[i].key)) return l.text;
#endif
  return kDefs[i].def;
}

bool localDefaults() {
#if defined(MARKET_HAVE_LOCAL_DEFAULTS)
  return true;
#else
  return false;
#endif
}

void defaults(Settings &s) {
  memset(&s, 0, sizeof(s));
  s.schema = kSchema;
  for (int i = 0; i < I_COUNT; i++)
    if (fromText(s, (Idx)i, defaultText((Idx)i))) fromText(s, (Idx)i, kDefs[i].def);
}

const char *checkLocalDefaults(Settings &probe) {
#if defined(MARKET_HAVE_LOCAL_DEFAULTS)
  for (const LocalDefault &l : kLocalDefaults) {
    int row = -1;
    for (int i = 0; i < I_COUNT; i++)
      if (l.key && !strcmp(l.key, kDefs[i].key)) row = i;
    if (row < 0 || !l.text || fromText(probe, (Idx)row, l.text)) return l.key ? l.key : "(null)";
  }
#else
  (void)probe;
#endif
  return nullptr;
}

bool sameValue(const Settings &a, const Settings &b, Idx i) {
  switch (kDefs[i].type) {
  case T_BOOL: case T_INT: case T_ENUM: return a.v[i].i == b.v[i].i;
  case T_NUM: return a.v[i].f == b.v[i].f;
  default: return !strcmp(str(a, i), str(b, i));
  }
}

bool isDefault(const Settings &s, Idx i) {
  const Def &d = kDefs[i];
  const char *def = defaultText(i);
  switch (d.type) {
  case T_BOOL: case T_INT: return s.v[i].i == atol(def);
  case T_NUM:  return s.v[i].f == rounded((float)atof(def), d.decimals);
  case T_ENUM: return s.v[i].i == optionIndex(i, def);
  default:     return !strcmp(str(s, i), def);
  }
}

// ── the blob ────────────────────────────────────────────────────────────────
size_t blobWrite(const Settings &s, char *out, size_t cap) {
  size_t k = 0;
  int n = snprintf(out, cap, "v=%u\n", (unsigned)kSchema);
  if (n < 0 || (size_t)n >= cap) return 0;
  k = (size_t)n;
  for (int i = 0; i < I_COUNT; i++) {
    n = snprintf(out + k, cap - k, "%s=", kDefs[i].key);
    if (n < 0 || k + (size_t)n >= cap) return 0;
    k += (size_t)n;
    if (!toText(s, (Idx)i, out + k, cap - k)) return 0;
    k += strlen(out + k);
    if (k + 2 > cap) return 0;
    out[k++] = '\n';
    out[k] = '\0';
  }
  return k;
}

static int rowOf(const char *key, size_t n) {
  for (int i = 0; i < I_COUNT; i++)
    if (strlen(kDefs[i].key) == n && !strncmp(kDefs[i].key, key, n)) return i;
  return -1;
}

int blobRead(const char *text, size_t len, Settings &s) {
  defaults(s);
  int read = 0;
  const char *p = text, *end = text + len;
  while (p < end) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    const size_t lineLen = nl ? (size_t)(nl - p) : (size_t)(end - p);
    const char *eq = (const char *)memchr(p, '=', lineLen);
    if (eq) {
      char value[1024];
      const size_t vl = lineLen - (size_t)(eq + 1 - p);
      if (vl < sizeof(value)) {
        memcpy(value, eq + 1, vl);
        value[vl] = '\0';
        if (lineLen >= 2 && p[0] == 'v' && eq == p + 1) {
          s.schema = (uint16_t)atoi(value);
        } else {
          const int i = rowOf(p, (size_t)(eq - p));
          if (i >= 0 && !fromText(s, (Idx)i, value)) read++;
        }
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  s.schema = kSchema;   // whatever was read is now in this schema's form
  return read;
}

// ── JSON ────────────────────────────────────────────────────────────────────
static const char *bad(char *err, size_t cap, const Def &d, const char *why) {
  snprintf(err, cap, "%s: %s", d.key, why);
  return err;
}

static void up(char *s) { market::upper(s); }

// A symbol from JSON, upper-cased into out; false when not a string or not a symbol.
static bool symOf(JsonVariantConst v, char *out, size_t cap, bool allowEmpty) {
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  if (!s || strlen(s) >= cap) return false;
  snprintf(out, cap, "%s", s);
  up(out);
  return (allowEmpty && !*out) || market::validSymbol(out);
}

static bool append(char *text, size_t cap, const char *piece) {
  const size_t k = strlen(text);
  if (k + strlen(piece) + 1 >= cap) return false;
  strcpy(text + k, piece);
  return true;
}

const char *fromJson(Settings &s, Idx i, JsonVariantConst in, char *err, size_t cap) {
  const Def &d = kDefs[i];
  char text[1024] = "";
  switch (d.type) {
  case T_BOOL:
    if (!in.is<bool>()) return bad(err, cap, d, "must be true or false");
    s.v[i].i = in.as<bool>();
    return nullptr;
  case T_INT: {
    if (in.is<bool>() || !in.is<long>()) return bad(err, cap, d, "must be a whole number");
    const long v = in.as<long>();
    if (v < (long)d.min || v > (long)d.max) { snprintf(err, cap, "%s: must be %ld to %ld", d.key, (long)d.min, (long)d.max); return err; }
    s.v[i].i = (int32_t)v;
    return nullptr;
  }
  case T_NUM: {
    if (in.is<bool>() || !in.is<float>()) return bad(err, cap, d, "must be a number");
    const float v = in.as<float>();
    if (!std::isfinite(v) || v < d.min || v > d.max) {
      char lo[24], hi[24];
      snprintf(err, cap, "%s: must be %s to %s", d.key, bound(d.min, lo, sizeof(lo)), bound(d.max, hi, sizeof(hi)));
      return err;
    }
    s.v[i].f = rounded(v, d.decimals);
    return nullptr;
  }
  case T_ENUM: {
    const int k = in.is<const char *>() ? optionIndex(i, in.as<const char *>()) : -1;
    if (k < 0) { snprintf(err, cap, "%s: must be one of %s", d.key, d.opts); return err; }
    s.v[i].i = k;
    return nullptr;
  }
  case T_DATE:
    if (!in.is<const char *>() || market::parseDate(in.as<const char *>()) == kNoDate) return bad(err, cap, d, "must be a date YYYY-MM-DD");
    snprintf(text, sizeof(text), "%s", in.as<const char *>());
    break;
  case T_TIME:
    if (!in.is<const char *>() || !validTime(in.as<const char *>())) return bad(err, cap, d, "must be a time HH:MM");
    snprintf(text, sizeof(text), "%s", in.as<const char *>());
    break;
  case T_SYM:
    if (!symOf(in, text, kSymLen, true)) return bad(err, cap, d, "must be a symbol: up to 12 of A-Z 0-9 . ^ = -");
    break;
  case T_SYMLIST: {
    JsonArrayConst a = in.as<JsonArrayConst>();
    if (a.isNull()) return bad(err, cap, d, "must be a list of {sym, name}");
    for (JsonVariantConst e : a) {
      char sym[kSymLen], name[kNameLen] = "";
      JsonObjectConst o = e.as<JsonObjectConst>();
      if (!symOf(o.isNull() ? e : o["sym"], sym, sizeof(sym), false)) return bad(err, cap, d, "each sym must be up to 12 of A-Z 0-9 . ^ = -");
      if (!o.isNull() && !o["name"].isNull()) {
        if (!o["name"].is<const char *>() || strlen(o["name"].as<const char *>()) >= kNameLen) return bad(err, cap, d, "each name must be up to 8 characters");
        snprintf(name, sizeof(name), "%s", o["name"].as<const char *>());
        up(name);
        if (*name && !market::validName(name)) return bad(err, cap, d, "each name must be up to 8 of A-Z 0-9 . ^ = -");
      }
      // -Wformat-truncation is a false alarm: sym is at most 12 (validSymbol) and
      // name at most 8 (validName), so ",SYM=NAME" is at most 22 of the 23 bytes.
      char piece[kSymLen + kNameLen + 2];
      snprintf(piece, sizeof(piece), "%s%s=%s", *text ? "," : "", sym, *name ? name : sym);
      if (!append(text, sizeof(text), piece)) return bad(err, cap, d, "too many");
    }
    if (parseSymList(text, sc()->syms, sc()->names, (int)d.max) < 0) { snprintf(err, cap, "%s: at most %d, no symbol twice", d.key, (int)d.max); return err; }
    break;
  }
  case T_POSLIST: {
    JsonArrayConst a = in.as<JsonArrayConst>();
    if (a.isNull()) return bad(err, cap, d, "must be a list of {sym, w, entry, asset_class, proxy}");
    double sum = 0;
    for (JsonObjectConst o : a) {
      char sym[kSymLen], proxy[kSymLen] = "", entry[12] = "";
      if (o.isNull() || !symOf(o["sym"], sym, sizeof(sym), false)) return bad(err, cap, d, "each sym must be up to 12 of A-Z 0-9 . ^ = -");
      JsonVariantConst w = o["w"];
      if (w.is<bool>() || !w.is<float>() || w.as<float>() <= 0 || w.as<float>() > 100) { snprintf(err, cap, "%s: %s needs a weight above 0 and at most 100", d.key, sym); return err; }
      const float wr = rounded(w.as<float>(), 2);
      sum += wr;
      if (!o["entry"].isNull()) {
        if (!o["entry"].is<const char *>() || market::parseDate(o["entry"].as<const char *>()) == kNoDate) { snprintf(err, cap, "%s: %s's entry must be a date YYYY-MM-DD", d.key, sym); return err; }
        snprintf(entry, sizeof(entry), "%s", o["entry"].as<const char *>());
      }
      int ac = 0;
      if (!o["asset_class"].isNull()) {
        ac = o["asset_class"].is<const char *>() ? assetClass(o["asset_class"].as<const char *>()) : -1;
        if (ac < 0) { snprintf(err, cap, "%s: %s's asset_class must be equity, bond, real_assets, gold or cash", d.key, sym); return err; }
      }
      if (!o["proxy"].isNull() && !symOf(o["proxy"], proxy, sizeof(proxy), true)) { snprintf(err, cap, "%s: %s's proxy must be a symbol", d.key, sym); return err; }
      char piece[80];
      snprintf(piece, sizeof(piece), "%s%s:%.2f:%s:%s:%s", *text ? "," : "", sym, (double)wr, entry, kAssetClassKeys[ac], proxy);
      // Trim the empty tail so the text form stays short.
      size_t n = strlen(piece);
      while (n && piece[n - 1] == ':') piece[--n] = '\0';
      if (!append(text, sizeof(text), piece)) return bad(err, cap, d, "too many");
    }
    if (sum > 100.005) { snprintf(err, cap, "%s: the weights sum to %.2f %%, at most 100", d.key, sum); return err; }
    if (parsePositions(text, sc()->pos, (int)d.max) < 0) { snprintf(err, cap, "%s: at most %d positions, no symbol twice", d.key, (int)d.max); return err; }
    break;
  }
  case T_WLIST: {
    JsonArrayConst a = in.as<JsonArrayConst>();
    if (a.isNull()) return bad(err, cap, d, "must be a list of {sym, w}");
    for (JsonObjectConst o : a) {
      char sym[kSymLen];
      if (o.isNull() || !symOf(o["sym"], sym, sizeof(sym), false)) return bad(err, cap, d, "each sym must be up to 12 of A-Z 0-9 . ^ = -");
      JsonVariantConst w = o["w"];
      if (w.is<bool>() || !w.is<float>()) return bad(err, cap, d, "each w must be a number");
      char piece[kSymLen + 16];
      snprintf(piece, sizeof(piece), "%s%s:%.2f", *text ? "," : "", sym, (double)rounded(w.as<float>(), 2));
      if (!append(text, sizeof(text), piece)) return bad(err, cap, d, "too many");
    }
    if (parseWeights(text, sc()->syms, sc()->vals, (int)d.max) < 0) { snprintf(err, cap, "%s: at most %d symbols, weights above 0 summing to at most 100", d.key, (int)d.max); return err; }
    break;
  }
  case T_TERMAP: {
    JsonObjectConst o = in.as<JsonObjectConst>();
    if (o.isNull()) return bad(err, cap, d, "must be an object of symbol: percent");
    for (JsonPairConst kv : o) {
      char sym[kSymLen];
      snprintf(sym, sizeof(sym), "%.12s", kv.key().c_str());
      up(sym);
      if (!market::validSymbol(sym)) return bad(err, cap, d, "each key must be a symbol");
      JsonVariantConst v = kv.value();
      if (v.is<bool>() || !v.is<float>()) return bad(err, cap, d, "each value must be a percent a year");
      char piece[kSymLen + 16];
      snprintf(piece, sizeof(piece), "%s%s=%.2f", *text ? "," : "", sym, (double)rounded(v.as<float>(), 2));
      if (!append(text, sizeof(text), piece)) return bad(err, cap, d, "too many");
    }
    if (parseTer(text, sc()->syms, sc()->vals, (int)d.max) < 0) { snprintf(err, cap, "%s: at most %d symbols, 0 to 10 %% a year", d.key, (int)d.max); return err; }
    break;
  }
  case T_NAMELIST: {
    JsonArrayConst a = in.as<JsonArrayConst>();
    if (a.isNull()) return bad(err, cap, d, "must be a list of names");
    for (JsonVariantConst e : a) {
      char name[kExNameLen];
      if (!e.is<const char *>() || strlen(e.as<const char *>()) >= sizeof(name)) return bad(err, cap, d, "each name up to 10 of A-Z 0-9");
      snprintf(name, sizeof(name), "%s", e.as<const char *>());
      up(name);
      char piece[kExNameLen + 2];
      snprintf(piece, sizeof(piece), "%s%s", *text ? "," : "", name);
      if (!append(text, sizeof(text), piece)) return bad(err, cap, d, "too many");
    }
    if (parseNames(text, sc()->ex, (int)d.max) < 0) { snprintf(err, cap, "%s: at most %d names of A-Z 0-9, none twice", d.key, (int)d.max); return err; }
    break;
  }
  case T_JSON: {
    if (!in.is<JsonObjectConst>()) return bad(err, cap, d, "must be an object");
    const size_t n = measureJson(in);
    if (n >= d.cap || n >= sizeof(text)) { snprintf(err, cap, "%s: at most %u bytes of JSON", d.key, (unsigned)(d.cap - 1)); return err; }
    serializeJson(in, text, sizeof(text));
    break;
  }
  default:
    return bad(err, cap, d, "unknown type");
  }
  if (strlen(text) >= d.cap) return bad(err, cap, d, "too long");
  snprintf(str(s, i), d.cap, "%s", text);
  return nullptr;
}

void toJson(const Settings &s, Idx i, JsonVariant out) {
  const Def &d = kDefs[i];
  const char *t = isString(d.type) ? str(s, i) : "";
  switch (d.type) {
  case T_BOOL: out.set(s.v[i].i != 0); return;
  case T_INT:  out.set((long)s.v[i].i); return;
  case T_NUM:  out.set(rounded(s.v[i].f, d.decimals)); return;
  case T_ENUM: out.set(optionName(i, s.v[i].i)); return;
  case T_DATE: case T_TIME: case T_SYM: out.set(t); return;
  case T_SYMLIST: {
    JsonArray a = out.to<JsonArray>();
    const int n = parseSymList(t, sc()->syms, sc()->names, (int)d.max);
    for (int k = 0; k < n; k++) { JsonObject o = a.add<JsonObject>(); o["sym"] = (const char *)sc()->syms[k]; o["name"] = (const char *)sc()->names[k]; }
    return;
  }
  case T_POSLIST: {
    JsonArray a = out.to<JsonArray>();
    const int n = parsePositions(t, sc()->pos, (int)d.max);
    for (int k = 0; k < n; k++) {
      JsonObject o = a.add<JsonObject>();
      o["sym"] = (const char *)sc()->pos[k].sym;
      o["w"] = rounded(sc()->pos[k].w, 2);
      // The optional fields only when set: fourteen rows of nulls cost 360 B.
      if (sc()->pos[k].entry != kNoDate) {
        int y, m, dd;
        market::civilFromDays(sc()->pos[k].entry, &y, &m, &dd);
        char iso[12];
        snprintf(iso, sizeof(iso), "%04d-%02d-%02d", y, m, dd);
        o["entry"] = iso;
      }
      if (sc()->pos[k].assetClass) o["asset_class"] = kAssetClassKeys[sc()->pos[k].assetClass];
      if (sc()->pos[k].proxy[0]) o["proxy"] = (const char *)sc()->pos[k].proxy;
    }
    return;
  }
  case T_WLIST: {
    JsonArray a = out.to<JsonArray>();
    const int n = parseWeights(t, sc()->syms, sc()->vals, (int)d.max);
    for (int k = 0; k < n; k++) { JsonObject o = a.add<JsonObject>(); o["sym"] = (const char *)sc()->syms[k]; o["w"] = rounded(sc()->vals[k], 2); }
    return;
  }
  case T_TERMAP: {
    JsonObject o = out.to<JsonObject>();
    const int n = parseTer(t, sc()->syms, sc()->vals, (int)d.max);
    for (int k = 0; k < n; k++) o[(const char *)sc()->syms[k]] = rounded(sc()->vals[k], 2);
    return;
  }
  case T_NAMELIST: {
    JsonArray a = out.to<JsonArray>();
    const int n = parseNames(t, sc()->ex, (int)d.max);
    for (int k = 0; k < n; k++) a.add((const char *)sc()->ex[k]);
    return;
  }
  case T_JSON: {
    JsonDocument doc(jalloc());
    if (deserializeJson(doc, t) || !doc.is<JsonObjectConst>()) { out.to<JsonObject>(); return; }
    out.set(doc.as<JsonVariantConst>());
    return;
  }
  default: return;
  }
}

const char *apply(JsonObjectConst in, Settings &s, Settings &scratch, char *err, size_t cap, bool *changed) {
  if (in.isNull()) { snprintf(err, cap, "config must be an object"); return err; }
  memcpy(&scratch, &s, sizeof(Settings));
  JsonDocument extra(jalloc());
  bool extraTouched = false;
  if (deserializeJson(extra, str(s, I_EXTRA)) || !extra.is<JsonObject>()) extra.to<JsonObject>();
  for (JsonPairConst kv : in) {
    const int i = rowOf(kv.key().c_str(), strlen(kv.key().c_str()));
    if (i >= 0) {
      if (fromJson(scratch, (Idx)i, kv.value(), err, cap)) return err;
    } else {
      // A key this firmware does not know: kept for the app, whole.
      if (strlen(kv.key().c_str()) > 48) { snprintf(err, cap, "%.48s: key too long", kv.key().c_str()); return err; }
      extra[kv.key().c_str()].set(kv.value());
      extraTouched = true;
    }
  }
  if (extraTouched) {
    if (fromJson(scratch, I_EXTRA, extra.as<JsonVariantConst>(), err, cap)) {
      snprintf(err, cap, "extra: the unknown keys do not fit %u bytes; drop some", (unsigned)(kDefs[I_EXTRA].cap - 1));
      return err;
    }
  }
  // display.ticker is one of tickers: refused when the POST names another,
  // moved to the first when the list no longer has it.
  if (*str(scratch, I_TICKER) && !tickerListed(scratch)) {
    if (!in["display.ticker"].isNull()) {
      snprintf(err, cap, "display.ticker: %.12s is not in tickers", str(scratch, I_TICKER));
      return err;
    }
    const int n = parseSymList(str(scratch, I_TICKERS), sc()->syms, sc()->names, kMaxWatch);
    snprintf(str(scratch, I_TICKER), kDefs[I_TICKER].cap, "%s", n > 0 ? sc()->syms[0] : "");
  }
  for (int i = 0; i < I_COUNT; i++) {
    const bool moved = !sameValue(s, scratch, (Idx)i);
    if (changed) changed[i] = moved;
  }
  memcpy(&s, &scratch, sizeof(Settings));
  return nullptr;
}

bool tickerListed(const Settings &s) {
  const int n = parseSymList(str(s, I_TICKERS), sc()->syms, sc()->names, kMaxWatch);
  for (int k = 0; k < n; k++)
    if (!strcmp(sc()->syms[k], str(s, I_TICKER))) return true;
  return false;
}

const char *effectiveTicker(const Settings &s) {
  char *out = sc()->ticker;
  out[0] = '\0';
  const int n = parseSymList(str(s, I_TICKERS), sc()->syms, sc()->names, kMaxWatch);
  for (int k = 0; k < n; k++)
    if (!strcmp(sc()->syms[k], str(s, I_TICKER))) { snprintf(out, kSymLen, "%s", sc()->syms[k]); return out; }
  if (n > 0) snprintf(out, kSymLen, "%s", sc()->syms[0]);
  return out;
}

void configJson(const Settings &s, JsonObject out) {
  for (int i = 0; i < I_COUNT; i++) toJson(s, (Idx)i, out[kDefs[i].key].to<JsonVariant>());
}

void registryJson(JsonObject out, const Settings &dflt) {
  JsonArray groups = out["groups"].to<JsonArray>();
  for (int g = 0; g < G_COUNT; g++) {
    JsonObject o = groups.add<JsonObject>();
    o["key"] = kGroupKeys[g];
    o["label"] = kGroupLabels[g];
    o["adv"] = g >= kFirstAdvanced;
  }
  JsonArray rows = out["rows"].to<JsonArray>();
  for (int i = 0; i < I_COUNT; i++) {
    const Def &d = kDefs[i];
    JsonObject o = rows.add<JsonObject>();
    o["key"] = d.key;
    o["type"] = kTypeKeys[d.type];
    o["group"] = kGroupKeys[d.group];
    o["label"] = d.label;
    if (d.hint) o["hint"] = d.hint;
    if (d.unit) o["unit"] = d.unit;
    if (d.type == T_INT || d.type == T_NUM) { o["min"] = d.min; o["max"] = d.max; o["dec"] = d.decimals; }
    if (d.type >= T_SYMLIST && d.type != T_JSON) o["max"] = (int)d.max;
    if (d.type == T_JSON) o["cap"] = d.cap - 1;
    if (d.opts) {
      JsonArray a = o["opts"].to<JsonArray>();
      for (int k = 0; *optionName((Idx)i, k); k++) a.add(optionName((Idx)i, k));
    }
    o["panel"] = d.path && !*d.path;
    toJson(dflt, (Idx)i, o["def"].to<JsonVariant>());
  }
}

// out["a"]["b"]["c"], made along the way.
static JsonVariant nested(JsonObject out, const char *path) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s", path);
  JsonObject o = out;
  char *p = buf;
  for (;;) {
    char *dot = strchr(p, '.');
    if (!dot) return o[p].to<JsonVariant>();
    *dot = '\0';
    JsonVariant v = o[p];
    o = v.is<JsonObject>() ? v.as<JsonObject>() : o[p].to<JsonObject>();
    p = dot + 1;
  }
}

void payloadJson(const Settings &s, JsonObject out) {
  out["v"] = 2;
  for (int i = 0; i < I_COUNT; i++) {
    const Def &d = kDefs[i];
    if (i == I_EXTRA || (d.path && !*d.path)) continue;
    toJson(s, (Idx)i, nested(out, d.path ? d.path : d.key));
  }
  // docs/18's v1 fields the rows above do not produce, and the ticker on
  // screen rather than a stored one the list has lost.
  out["portfolio"]["rebalance"] = s.v[I_REBAL_MODE].i != 0;
  out["ticker"] = effectiveTicker(s);
  JsonArray presets = out["presets"].to<JsonArray>();
  const uint16_t mask = presetMask(s);
  for (uint8_t p = 0; p < market::kPresets; p++)
    if (mask & (1u << p)) presets.add(market::kPresetKeys[p]);
  // The pass-through, only where no row owns the key.
  JsonDocument extra(jalloc());
  if (!deserializeJson(extra, str(s, I_EXTRA)) && extra.is<JsonObjectConst>())
    for (JsonPairConst kv : extra.as<JsonObjectConst>())
      if (!out[kv.key().c_str()].is<JsonVariantConst>() || out[kv.key().c_str()].isNull()) out[kv.key().c_str()].set(kv.value());
}

// ── reads ───────────────────────────────────────────────────────────────────
uint8_t windowPreset(const Settings &s) {
  const int p = s.v[I_WINDOW].i;
  return (uint8_t)(p >= 0 && p < market::kPresets ? p : market::P_MAX);
}

bool setWindow(Settings &s, uint8_t preset) {
  if (preset >= market::kPresets) return false;
  s.v[I_WINDOW].i = preset;
  return true;
}

bool setTicker(Settings &s, const char *sym) {
  if (!market::validSymbol(sym) || strlen(sym) >= kDefs[I_TICKER].cap) return false;
  snprintf(str(s, I_TICKER), kDefs[I_TICKER].cap, "%s", sym);
  return true;
}

uint16_t presetMask(const Settings &s) {
  uint16_t m = 0;
  for (uint8_t p = market::P_YTD; p < market::kPresets; p++) m |= (uint16_t)(1u << p);
  for (int k = 0; k < 5; k++)
    if (s.v[kPresetRows[k]].i) m |= (uint16_t)(1u << k);   // WTD MTD 1M 3M 6M are presets 0..4
  return m;
}

uint8_t shownMode(const Settings &s) { return s.v[I_REBAL_MODE].i == 0 ? market::M_HOLD : market::M_REBAL; }

}  // namespace mks

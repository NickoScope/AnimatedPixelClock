#pragma once
// Stock market dashboard: the data model, its validation and the number rules.
//
// Plain C++ and ArduinoJson, no Arduino core, so tools/market/panel/ compiles
// it on the host: the decoders, the number formatting, the LittleFS record and
// the page layout are tested there against the same code the panel runs.
//
// The wire contract (topics, every field, who publishes what) is in
// src/market/README.md; the design in docs/18-stock-dashboard.md of the
// knowledge base. What this file enforces:
//
//   * schema "v":1 on every payload from Home Assistant;
//   * every field of the right JSON type, or the whole payload is refused and
//     what the page shows stays as it was;
//   * every string cut to its buffer; symbols from a closed alphabet;
//   * a line is exactly 128 uint16 points, little-endian, base64 (344
//     characters), scaled between the payload's min and max;
//   * at most kMaxWatch indices and tickers, kMaxPositions holdings, kMaxTape
//     exchanges; the rest dropped and counted.
//
// Dates are days since 1970-01-01 (civil), kNoDate when absent. Everything
// that draws a number goes through the fmt* functions below, which are the
// design's rules as tools/market/render.py states them; check_market_panel.py
// holds the two implementations against each other on the same values.

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

namespace market {

static const long     kSchema       = 1;
static const uint8_t  kPoints       = 128;    // points per published line
static const size_t   kPtsB64       = 344;    // 256 bytes of uint16 in base64, "==" padded
static const uint8_t  kMaxWatch     = 8;      // indices, and tickers
static const uint8_t  kMaxSymbols   = 16;     // both lists together: live quotes, status
static const uint8_t  kMaxPositions = 16;
static const uint8_t  kMaxTape      = 8;
static const uint8_t  kPresets      = 11;    // WTD MTD 1M 3M 6M and the six standard ones
static const size_t   kSymLen       = 13;     // ^GSPC, EURUSD=X, IWDA.AS, BRK-B: up to 12 characters
static const size_t   kNameLen      = 9;      // the display name: up to 8 characters (ABCDEFGH)
static const size_t   kCurLen       = 4;      // ISO 4217: three capitals
static const size_t   kErrLen       = 25;     // the app's error, as DATA ERR's reason
static const size_t   kExNameLen    = 11;     // an exchange on the tape: EURONEXT
static const size_t   kNextLen      = 6;      // the tape's next change, "20:00"
static const size_t   kFetchedLen   = 21;     // "2026-09-15T07:02Z"
static const size_t   kTerSrcLen    = 8;      // yahoo, manual
static const int32_t  kNoDate       = INT32_MIN;
// src/mqtt/mqtt_bus.cpp's buffer holds the fixed header and the topic as well
// as the payload, so a payload must stay this far under it.
static const size_t   kBusBuffer    = 2048;
static const size_t   kPayloadMax   = 1900;
// AS OF older than this many trading days is STALE (docs/18, glossary).
static const int      kStaleTradingDays = 3;

// The window presets, in knob order; the topic and the payload name them. The
// first five are the extras of docs/19, on the knob only while the app
// publishes them; the six from YTD are the standard set (docs/18).
enum Preset : uint8_t { P_WTD = 0, P_MTD, P_1M, P_3M, P_6M, P_YTD, P_1Y, P_3Y, P_5Y, P_10Y, P_MAX };
extern const char *const kPresetKeys[kPresets];          // "WTD" "MTD" "1M" "3M" "6M" "YTD" "1Y" "3Y" "5Y" "10Y" "MAX"
static const uint16_t kStandardPresets = 0x7E0;          // bits P_YTD..P_MAX

// An exchange's or a quote's session state, as the app names it.
enum ExState : uint8_t { X_NONE = 0, X_OPEN, X_PRE, X_POST, X_CLOSED, X_STALE, X_COUNT };
extern const char *const kExStateKeys[X_COUNT];          // "--" "OPEN" "PRE" "POST" "CLOSED" "STALE"

// The portfolio's two models.
enum Mode : uint8_t { M_HOLD = 0, M_REBAL, M_COUNT };
extern const char *const kModeKeys[M_COUNT];             // "hold" "rebal"

// A quote's feed, from live.q[sym].feed (docs/19): how fresh the figure is.
enum Feed : uint8_t { F_NONE = 0, F_LIVE, F_DELAYED, F_STALE, F_CLOSED, F_COUNT };
extern const char *const kFeedKeys[F_COUNT];             // "" "LIVE" "DELAYED" "STALE" "CLOSED"

// The app's own health, from status.state.
enum AppState : uint8_t { A_OK = 0, A_STALE, A_ERROR, A_COUNT };
extern const char *const kAppStateKeys[A_COUNT];         // "ok" "stale" "error"

// ── records ─────────────────────────────────────────────────────────────────
// index/<sym>/<preset> and ticker/<sym>/<preset>: one window of one symbol.
struct Series {
  bool     have;
  uint8_t  preset;
  char     sym[kSymLen];
  char     name[kNameLen];
  char     cur[kCurLen];
  int32_t  from, to;            // the window's first and last bar
  float    last, chg, hi, lo;   // chg: last / first in the window - 1
  float    min, max;            // the line's scale
  float    cagr, mdd;
  bool     hasCagr;             // cagr is null under three years
  float    ann;                 // annualised over the window, from a year up (TICKER's ANN)
  bool     hasAnn;
  uint16_t pts[kPoints];
};

// One watched symbol: its slot's name and every window of it.
struct Symbol {
  char    sym[kSymLen];
  char    name[kNameLen];       // the panel's display name (the mnemonic SPX for ^GSPC)
  Series  p[kPresets];
};

// portfolio/<mode>/<preset>
struct Portfolio {
  bool     have;
  uint8_t  mode, preset;
  char     cur[kCurLen];
  int32_t  from, to;
  float    value, chg, sinceStart, cagr, mdd, div, terDrag, cash;
  float    min, max;
  bool     hasCagr, hasBench, hasGross;
  // The owner's decisions of 2026-09-15 11:43 (docs/18): the window's return is
  // the time-weighted one, ANN its annualised figure from a year up, XIRR since
  // inception when money flows in, and the window's drawdown with its months.
  float    twr, ann, xirrAnn, fxEffect, contrib;
  bool     hasTwr, hasAnn, hasXirr, flows, hasFx, hasContrib;
  int32_t  mddPeak, mddTrough, mddRecovery;   // kNoDate when none (recovery: not recovered)
  uint16_t pts[kPoints];        // the ledger V
  uint16_t px[kPoints];         // the same without dividends
  uint16_t bench[kPoints];      // the first index scaled to the capital
  uint16_t gross[kPoints];      // the EST counterfactual, off by default
};

// holdings/<mode>
struct HoldRow {
  char    sym[kSymLen];
  float   tgt, now;             // percent
  float   ret;                  // since entry, a fraction; valid when hasRet
  bool    hasRet;
  int32_t entry;                // kNoDate before the fund lists
};
struct Holdings {
  bool     have;
  uint8_t  mode;
  int32_t  asof;
  uint8_t  n, dropped;
  HoldRow  rows[kMaxPositions];
  float    cashTgt, cashNow;    // percent
};

// live: one quote per watched symbol
struct Quote {
  char     sym[kSymLen];
  float    last, prev, day;     // day: against the previous close, valid when hasDay
  bool     hasDay;
  uint8_t  state;               // ExState
  uint8_t  feed;                // Feed, F_NONE when the app sends none
  uint16_t delayS;              // seconds behind the clock, valid when hasDelay
  bool     hasDelay;
  int32_t  asof;
};
struct Live {
  bool     have;
  uint32_t ts;
  uint8_t  n, dropped;
  Quote    q[kMaxSymbols];
};

// intraday/<sym>: today's session for the ticker on screen
struct Intraday {
  bool     have;
  char     sym[kSymLen];
  uint32_t ts;
  uint8_t  n;                   // slots filled so far, 0..128; the rest are not drawn
  float    min, max;
  uint16_t pts[kPoints];
};

// tape
struct TapeItem {
  char    name[kExNameLen];
  uint8_t state;                // ExState
  char    next[kNextLen];       // the next change, the panel's local time
};
struct Tape {
  bool     have;
  uint32_t ts;
  uint8_t  n, dropped;
  TapeItem x[kMaxTape];
};

// status
struct SymStatus {
  char     sym[kSymLen];
  int32_t  asof;
  uint32_t bars;
  float    ter;                 // a fraction per year, valid when hasTer
  bool     hasTer;
  char     terSrc[kTerSrcLen];
};
struct Status {
  bool      have;
  int32_t   asof;
  char      fetched[kFetchedLen];
  uint8_t   state;              // AppState
  char      err[kErrLen];
  uint8_t   n, dropped;
  SymStatus s[kMaxSymbols];
};

// Everything the panel keeps, and writes to LittleFS as one record. Plain data,
// no pointers, so a memcpy is its serialisation (recordWrite below adds the
// header and the CRC). Bump kRecordVersion when any struct above changes.
struct Model {
  uint32_t  gen;                // counts accepted payloads
  uint8_t   nIdx, nTk;
  Symbol    idx[kMaxWatch];
  Symbol    tk[kMaxWatch];
  Portfolio pf[M_COUNT][kPresets];
  Holdings  hd[M_COUNT];
  Live      live;
  Intraday  intra;
  Tape      tape;
  Status    status;
};

// ── ingest ──────────────────────────────────────────────────────────────────
// Where a parse lands before it is copied over the live record: one spare of
// each kind, and a Symbol for setWatch's swaps. market_ha.cpp puts it in PSRAM.
struct Spare {
  Series    s;
  Portfolio p;
  Holdings  h;
  Live      l;
  Intraday  i;
  Tape      t;
  Status    st;
  Symbol    sym;
};

// The watch lists from the settings, in their order: a symbol kept keeps its
// series, a new one starts empty, a dropped one is forgotten. `tickers` picks
// m.tk over m.idx. Names may be nullptr entries (the symbol is then the name).
void setWatch(Model &m, Spare &sp, bool tickers, const char *const *syms, const char *const *names, uint8_t n);

// One JSON payload by its topic leaf (after ".../market/"): status,
// index/<sym>/<preset>, ticker/<sym>/<preset>, portfolio/<mode>/<preset>,
// holdings/<mode>, live, intraday/<sym>, tape. `ticker` is the ticker the
// TICKER page shows: intraday/<sym> is taken only for it. Returns nullptr when
// the model took it (m.gen advanced), else why not: a decoder's reason,
// "unknown topic", "unwatched symbol", "not the selected ticker", "bad preset",
// "bad mode".
const char *ingest(Model &m, Spare &sp, const char *leaf, JsonObjectConst in, const char *ticker);

// ── decoders ────────────────────────────────────────────────────────────────
// Each returns nullptr with *out filled, or a short reason (a literal). *out is
// scratch: on a reason its content is undefined, so parse into a spare and
// copy it over the live one only on success - market_ha.cpp does, which is
// what keeps a bad payload off the screen. `sym` and `preset` / `mode` are what
// the topic said; the payload must agree.
const char *seriesFrom(JsonObjectConst in, const char *sym, uint8_t preset, Series *out);
const char *portfolioFrom(JsonObjectConst in, uint8_t mode, uint8_t preset, Portfolio *out);
const char *holdingsFrom(JsonObjectConst in, uint8_t mode, Holdings *out);
const char *liveFrom(JsonObjectConst in, Live *out);
const char *intradayFrom(JsonObjectConst in, const char *sym, Intraday *out);
const char *tapeFrom(JsonObjectConst in, Tape *out);
const char *statusFrom(JsonObjectConst in, Status *out);

// 128 uint16 little-endian from 344 characters of base64; false on anything else.
bool decodePts(const char *b64, uint16_t out[kPoints]);

// A Yahoo symbol: 1 to 12 of A-Z 0-9 . ^ = -
bool validSymbol(const char *s);
// A display name: 1 to 8 of A-Z 0-9 . ^ = - (drawn in 5x7 and Picopixel)
bool validName(const char *s);
// A currency: three capitals.
bool validCurrency(const char *s);
// An exchange on the tape: 1 to 10 of A-Z 0-9
bool validExchange(const char *s);
int  presetIndex(const char *s);    // -1 when not one of kPresetKeys
int  modeIndex(const char *s);
int  exStateIndex(const char *s);   // "OPEN" .. "STALE"; -1 otherwise
int  feedIndex(const char *s);      // "LIVE" .. "CLOSED"; -1 otherwise

// Bounded copy of ASCII: printable 0x20..0x7E only, anything else becomes '?'.
size_t copyText(char *dst, size_t cap, const char *src);

// Upper-case ASCII in place.
void upper(char *s);

// ── dates ───────────────────────────────────────────────────────────────────
int32_t daysFromCivil(int y, int m, int d);
void    civilFromDays(int32_t z, int *y, int *m, int *d);
int32_t parseDate(const char *iso);              // "YYYY-MM-DD" -> days, kNoDate on anything else
int     weekday(int32_t days);                   // 0 = Monday .. 6 = Sunday
// Weekdays strictly after `a` up to and including `b` (0 when b <= a): the
// design's trading-day count without a holiday calendar (market_ref.py).
int     weekdaysAfter(int32_t a, int32_t b);
bool    isStale(int32_t asof, int32_t today, int tradingDays = kStaleTradingDays);   // weekdaysAfter(asof, today) > tradingDays

// ── numbers, the design's rules ─────────────────────────────────────────────
// Each writes into out (cap bytes) and returns the length. The rounding is
// Python's: %.1f is the exact decimal, round() is half to even (nearbyint).
size_t thousands(char *out, size_t cap, long n);                 // 26186 -> "26 186"
size_t fmtValue(char *out, size_t cap, double v, int slot);      // a price or value in `slot` characters
size_t fmtAmount(char *out, size_t cap, double v, int slot);     // a sum of money: whole units, then K / M
size_t fmtPct(char *out, size_t cap, double x, bool have);       // "+12.4%" "-0.4%" "0.0%" "+1 245%" "--"
size_t fmtShare(char *out, size_t cap, double x);                // "0.5%" "54%"
size_t fmtDate(char *out, size_t cap, int32_t days);
size_t fmtYm(char *out, size_t cap, int32_t days);               // "20-03": the drawdown's months             // "14 SEP"
// The sign colour's rule: 0 = zero or none (dim), 1 = up (green), -1 = down (red).
int    pctSign(double x, bool have);

// ── fonts ───────────────────────────────────────────────────────────────────
// Picopixel metrics from the GFX glyph table (market_pico.cpp): the ink width
// as Adafruit_GFX::getTextBounds gives it, and the cursor advance.
int16_t picoInk(const char *s);
int16_t picoAdv(const char *s);
// 5x7: 6 per glyph less the final blank column, times k; in a number (`thin`)
// a space or a dot advances half a cell.
int16_t w5(const char *s, uint8_t k, bool thin);

// ── the LittleFS record ─────────────────────────────────────────────────────
// "MKT1", the version, the model's size, the model, CRC-32 of all before it.
static const uint32_t kRecordMagic   = 0x314B544DUL;   // "MKT1" little-endian
static const uint16_t kRecordVersion = 3;
size_t   recordSize();                                          // bytes recordWrite needs
size_t   recordWrite(const Model &m, uint8_t *out, size_t cap); // 0 when cap is short
const char *recordRead(const uint8_t *in, size_t len, Model *out);  // nullptr, or why not
uint32_t crc32(const uint8_t *p, size_t n, uint32_t seed = 0);

}  // namespace market

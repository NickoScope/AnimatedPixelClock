#pragma once
// Stock market dashboard: the settings as a table.
//
// One row per setting - key, type, bounds, default, group, the words the
// portal shows - and everything else is derived from the rows: the defaults,
// the NVS blob, the JSON of /api/market (in and out, with validation), the
// registry the portal builds its controls from, and the retained `config`
// payload (v2) the Home Assistant app reads. Adding a setting is one row.
//
// Keys are dotted, as docs/19-market-dashboard-research.md section 5 names
// them; /api/market uses them flat ("rebal.mode"), the payload nests them
// ({"rebal":{"mode":..}}). A row's `path` is where it lands in the payload
// when that differs from the key (docs/18's v1 shape is kept: portfolio.*,
// ticker); "" keeps a row on the panel. The `extra` row carries keys this
// firmware does not know: a POST with an unknown key inside "config" lands
// there whole (JSON, up to the row's capacity) and goes out again in the
// payload untouched, so the app and the panel grow independently.
//
// The committed defaults are neutral: an example 60/40 allocation, not
// anyone's. One's own defaults go in src/market/market_local_defaults.h, which
// is never committed (.gitignore): when it is there its rows replace the
// neutral ones (defaultText below); -DMARKET_NO_LOCAL_DEFAULTS ignores it, and
// -DMARKET_LOCAL_DEFAULTS_FILE="path" names another file (the host test and the
// flag matrix use tools/market/panel/local_defaults_example.h). The display
// rows' defaults reproduce the approved previews (tools/market/preview/).
//
// Storage: one blob in NVS namespace "market", key "cfg": "key=text\n" per
// row, so a row added later reads as its default and a row removed is
// skipped. ESP-IDF's NVS (docs, "Record Size Limitations", read 2026-09-15):
// a blob may be 508 000 B or 97.6 % of the partition less 4000 B, whichever
// is lower; the new value is written before the old is erased, so the free
// space for a whole new copy is needed at every save. The panel's nvs
// partition (default_16MB.csv) is 0x5000 = 20 480 B, shared with the other
// namespaces: the blob here is a few KB and refused above kBlobMax.
//
// Plain C++ and ArduinoJson, no Arduino core: market_ha.cpp does the NVS
// calls; tools/market/panel/ tests everything else on the host.

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "market_model.h"

namespace mks {

// One row of a local defaults header: a setting's dotted key and its default
// in the text form.
struct LocalDefault {
  const char *key;
  const char *text;
};

static const uint16_t kSchema  = 2;      // the settings' own version, the blob's first line
static const size_t   kBlobMax = 6144;   // a save above this is refused (see the header)
// The retained `config` payload is streamed (mqttBusPublishLarge), so it may
// pass the bus buffer's 1 900 B; this is its own ceiling, and the buffer it
// is serialised into. The default allocation makes about 1 950 B.
static const size_t   kConfigMax = 4096;
static const uint8_t  kMaxWatch = market::kMaxWatch;
static const uint8_t  kMaxPositions = market::kMaxPositions;
static const uint8_t  kMaxTape = market::kMaxTape;
static const uint8_t  kMaxTer = 16;
static const uint8_t  kMaxBlend = 8;

enum Type : uint8_t {
  T_BOOL, T_INT, T_NUM, T_ENUM,
  T_DATE,       // "YYYY-MM-DD"
  T_TIME,       // "HH:MM"
  T_SYM,        // one symbol, may be empty
  T_SYMLIST,    // "SYM=NAME,..."                          <-> [{"sym","name"}]   (a bare "SYM" is accepted in)
  T_POSLIST,    // "SYM:W[:DATE[:CLASS[:PROXY]]],..."      <-> [{"sym","w","entry","asset_class","proxy"}]
  T_WLIST,      // "SYM:W,..."                             <-> [{"sym","w"}]
  T_TERMAP,     // "SYM=PCT,..."                           <-> {"SYM": pct}
  T_NAMELIST,   // "NAME,..."                              <-> ["NAME"]
  T_JSON,       // an object, verbatim
  T_COUNT
};
extern const char *const kTypeKeys[T_COUNT];

// The page's blocks: the first three are the plain page, the rest are folds.
enum Group : uint8_t {
  G_WATCH, G_PORTFOLIO, G_DISPLAY,
  G_RETURNS, G_REBAL, G_FLOWS, G_FEES, G_CURRENCY, G_BENCH, G_RISK, G_DISPLAY_ADV, G_DATA, G_ALERTS, G_EXTRA,
  G_COUNT
};
extern const char *const kGroupKeys[G_COUNT];
extern const char *const kGroupLabels[G_COUNT];
static const Group kFirstAdvanced = G_RETURNS;

enum AssetClass : uint8_t { AC_NONE = 0, AC_EQUITY, AC_BOND, AC_REAL_ASSETS, AC_GOLD, AC_CASH, AC_COUNT };
extern const char *const kAssetClassKeys[AC_COUNT];   // "" "equity" "bond" "real_assets" "gold" "cash"

struct Def {
  const char *key;       // dotted; /api/market's config key and the portal's id
  const char *path;      // nullptr: the key; "": panel only, not published; else where it lands in the payload
  Type        type;
  Group       group;
  float       min, max;  // T_INT, T_NUM: the range; the list types: max = entries
  uint8_t     decimals;  // T_NUM: as printed and stored
  uint16_t    cap;       // string types: the text form's capacity in the pool, NUL included
  const char *def;       // the default, in the text form
  const char *opts;      // T_ENUM: "a|b|c"
  const char *label;
  const char *hint;
  const char *unit;
};

// Row order is the page order inside each group. The names are the
// firmware's direct reads; the table is the truth for everything else.
enum Idx : uint8_t {
  I_INDICES = 0, I_TICKERS,
  I_CAPITAL, I_CURRENCY, I_INCEPTION, I_REBAL_MODE, I_POSITIONS,
  I_WINDOW, I_TICKER, I_TAPE_ON, I_TAPE, I_LINE_PX, I_LINE_BENCH, I_SESSION, I_DWELL_S,
  I_RET_MEASURE, I_RET_REAL, I_DIV_MODE,
  I_REBAL_CAL, I_BAND_ABS, I_BAND_REL, I_BAND_CHECK, I_REBAL_CONTRIB_ONLY,
  I_CONTRIB_AMOUNT, I_CONTRIB_EVERY, I_CONTRIB_INDEX, I_WD_AMOUNT, I_WD_KIND, I_WD_EVERY, I_CASH_YIELD,
  I_TER, I_LINE_GROSS, I_TAX_DIV, I_COST_FIXED, I_COST_PCT,
  I_FX_EFFECT,
  I_BENCH_MODE, I_BENCH_SYM, I_BENCH_BLEND, I_BENCH_TELLTALE,
  I_MDD_BASIS, I_RISK_FREE,
  I_PRESET_WTD, I_PRESET_MTD, I_PRESET_1M, I_PRESET_3M, I_PRESET_6M,
  I_SCALE, I_COLORS, I_PAGE_MARKETS, I_PAGE_TICKER, I_PAGE_PORTFOLIO, I_PAGE_HOLDINGS,
  I_LINE_VALUE, I_TAPE_SPEED, I_HD_SORT, I_HD_BAND_MARK, I_FEED_LABEL,
  I_LIVE_POLL_S, I_FETCH_AT, I_STALE_SESSION_MIN, I_STALE_HISTORY_DAYS,
  I_ALERT_DAY, I_ALERT_DD, I_ALERT_BAND,
  I_EXTRA,
  I_COUNT
};
extern const Def kDefs[I_COUNT];

// The extra window presets, in the order of the window row's options and of
// market::Preset: which rows switch which preset on.
static const Idx kPresetRows[5] = {I_PRESET_WTD, I_PRESET_MTD, I_PRESET_1M, I_PRESET_3M, I_PRESET_6M};

struct Value {
  int32_t i;   // T_BOOL, T_INT, T_ENUM (the option's index)
  float   f;   // T_NUM
};

// The strings live in one pool, each row at a fixed offset with the capacity
// its Def states: about 4 KB, the whole struct under 5 KB.
size_t poolSize();
size_t poolOffset(Idx i);

struct Settings {
  uint16_t schema;
  Value    v[I_COUNT];
  char     pool[3072];
};
static_assert(sizeof(Settings::pool) >= 3072, "pool");
bool poolFits();   // sizeof(pool) >= poolSize(): checked once at start

inline char       *str(Settings &s, Idx i)       { return s.pool + poolOffset(i); }
inline const char *str(const Settings &s, Idx i) { return s.pool + poolOffset(i); }

struct Position {
  char    sym[market::kSymLen];
  float   w;
  int32_t entry;        // market::kNoDate when none
  uint8_t assetClass;   // AssetClass
  char    proxy[market::kSymLen];
};

// The lists' working buffers, about 1 KB. Kept out of .bss: market_ha.cpp
// hands in one inside its PSRAM store before anything else is called; on the
// host the first use allocates one.
struct Scratch {
  char     syms[kMaxPositions][market::kSymLen];
  char     names[kMaxWatch][market::kNameLen];
  char     ex[kMaxTape][market::kExNameLen];
  float    vals[kMaxPositions];
  Position pos[kMaxPositions];
  char     option[24];
  char     ticker[market::kSymLen];
};
void setScratch(Scratch *s);
// Where the settings' own JSON documents (the extra row, a parse check) take
// their memory; nullptr = ArduinoJson's default (malloc).
void setJsonAllocator(ArduinoJson::Allocator *a);

// A row's default text: the local header's when it has the row, else the
// neutral one.
const char *defaultText(Idx i);
bool        localDefaults();          // a local defaults header is compiled in
// nullptr, or the key of the first local row that does not read. `probe` is
// scratch the caller owns (market_ha.cpp: the store's spare Settings, in PSRAM).
const char *checkLocalDefaults(Settings &probe);

// The ticker the TICKER page shows and the app polls intraday: display.ticker
// when it is one of tickers, else the first ticker, "" without tickers. The
// pointer is into the scratch: valid until the next settings call.
const char *effectiveTicker(const Settings &s);
bool        tickerListed(const Settings &s);   // display.ticker is one of tickers

// ── text forms ──────────────────────────────────────────────────────────────
// Each parser returns the count, or -1 when the text is not valid.
int parseSymList(const char *s, char syms[][market::kSymLen], char names[][market::kNameLen], int cap);
int parsePositions(const char *s, Position *out, int cap);
int parseWeights(const char *s, char syms[][market::kSymLen], float *w, int cap);
int parseTer(const char *s, char syms[][market::kSymLen], float *pct, int cap);
int parseNames(const char *s, char names[][market::kExNameLen], int cap);
bool validTime(const char *s);   // "HH:MM"

// A value's text form (the blob); false when the value is not valid for the row.
bool toText(const Settings &s, Idx i, char *out, size_t cap);
// A value from its text form (the blob, or the default); nullptr, or why not.
const char *fromText(Settings &s, Idx i, const char *text);

void defaults(Settings &s);
bool isDefault(const Settings &s, Idx i);
bool sameValue(const Settings &a, const Settings &b, Idx i);

// ── the blob ────────────────────────────────────────────────────────────────
// "v=2\nkey=text\n..." for every row; 0 when it does not fit cap.
size_t blobWrite(const Settings &s, char *out, size_t cap);
// Defaults, then every row the blob names and this build knows; rows that
// fail their check keep the default. Returns how many rows were read.
int blobRead(const char *text, size_t len, Settings &s);

// ── JSON ────────────────────────────────────────────────────────────────────
// One row from JSON; nullptr, or a message naming the field in err.
const char *fromJson(Settings &s, Idx i, JsonVariantConst in, char *err, size_t cap);
// One row to JSON.
void toJson(const Settings &s, Idx i, JsonVariant out);

// The portal's {"config":{...}} into `scratch` first, then over `s` when every
// key is valid: known keys by their row, unknown keys gathered into the extra
// row (refused when they do not fit). display.ticker set to a symbol that is
// not in tickers is refused; a tickers list that loses the current ticker
// moves display.ticker to its first. Returns nullptr on success, else the
// message in err. `changed` (I_COUNT flags, may be nullptr) says which rows moved.
const char *apply(JsonObjectConst in, Settings &s, Settings &scratch, char *err, size_t cap, bool *changed);

void configJson(const Settings &s, JsonObject out);      // every row by key: /api/market's "config"
// "groups" and "rows" with each row's default from `dflt` (a Settings after defaults()).
void registryJson(JsonObject out, const Settings &dflt);
// The retained `config` payload for the app: v2, the rows nested by path, the
// derived portfolio.rebalance, the presets list, the extra row spread in.
void payloadJson(const Settings &s, JsonObject out);

// ── reads for the firmware ──────────────────────────────────────────────────
inline bool  flag(const Settings &s, Idx i)   { return s.v[i].i != 0; }
inline int   option(const Settings &s, Idx i) { return s.v[i].i; }
inline float number(const Settings &s, Idx i) { return s.v[i].f; }
const char *optionName(Idx i, int option);   // "EUR"; "" out of range
int  optionIndex(Idx i, const char *name);   // -1 when not an option
// The window row's option is a market::Preset: the two lists are kept in step.
uint8_t  windowPreset(const Settings &s);
bool     setWindow(Settings &s, uint8_t preset);
bool     setTicker(Settings &s, const char *sym);
// Which presets the settings ask the app for: the six standard ones and the
// extras switched on, as a bit per market::Preset.
uint16_t presetMask(const Settings &s);
// The mode the panel shows: M_HOLD when rebal.mode is hold, else M_REBAL.
uint8_t  shownMode(const Settings &s);

}  // namespace mks

#pragma once
// Stock market dashboard: the four pages and the tape as drawing calls.
//
// One code path for the panel and the host: the layout below draws through a
// Canvas, which market_page.cpp implements on the HUB75 display and
// tools/market/panel/market_host_test.cpp as a recorder and a 128x64 raster.
// Every constant is the MK_* block of tools/market/render.py, copied name for
// name; the 25 frames in tools/market/preview/ are what this must reproduce,
// and check_market_panel.py holds the two against each other.
//
// No Arduino here: plain C++ on top of market_model.h.

#include "market_model.h"

namespace market {

static const int16_t W = 128;
static const int16_t H = 64;

// ── layout, render.py's MK_ block ───────────────────────────────────────────
static const int16_t MK_TAPE_H       = 7;     // the exchange tape: rows 0-6, Picopixel
static const int16_t MK_Y_TAPE       = 1;     // Picopixel top inside the tape: glyph rows 1-5
static const int16_t MK_Y_TAPE_RULE  = 7;     // one dark row between the tape and the page
static const int16_t MK_TAPE_FPS     = 20;    // the tape moves 1 px per frame at this rate
static const int16_t MK_PAGE_TOP     = 8;     // the page: rows 8-63, 56 px
static const int16_t MK_X_LEFT       = 1;
static const int16_t MK_X_RIGHT      = 127;   // inclusive: right-aligned text ends on this column
static const int16_t MK_GAP          = 3;
static const int16_t MK_ADV          = 6;     // 5x7 advance
static const int16_t MK_THIN_ADV     = 3;     // in a number, a space or a dot advances half a cell
static const uint8_t MK_BIG          = 2;     // the key number's scale: 10x14 glyphs, advance 12
static const int16_t MK_PICO_ASCENT  = 4;     // Picopixel: the baseline is 4 rows under the top
static const int16_t MK_PICO_DROP    = 2;     // Picopixel top below a 5x7 row's top, so both sit on one baseline
static const int16_t MK_Y_HEAD       = 8;     // 5x7 heading, rows 8-14
static const int16_t MK_Y_BLOCK      = 16;    // the key number, rows 16-29; the 5x7 line beside its top half is at 16
static const int16_t MK_Y_BLOCK2     = 23;    // the 5x7 line beside its bottom half, rows 23-29
static const int16_t MK_BIG_CHARS    = 7;     // TICKER, PORTFOLIO: the 2x number's slot in characters, a sign included
static const int16_t MK_Y_FOOT       = 58;    // Picopixel footer, rows 58-62
static const int16_t MK_Y_ERR        = 30;    // DATA ERR, 5x7, centred
static const int16_t MK_Y_ERR_WHY    = 41;    // its reason, Picopixel, centred
static const int16_t MK_TICKS[3]     = {32, 64, 96};   // three time ticks, at the quarters of a full-width chart
static const int16_t MK_TICK_H       = 2;
static const uint16_t MK_LIVE_MAX_S  = 120;   // LIVE only while the quote is less than this many seconds behind the clock (owner, 11:43)
static const char    MK_DELAY_BADGE[] = "D";  // else this badge and the whole minutes behind, dim: D15
// the words of the six decisions (owner, 2026-09-15 11:43); render.py's LBL_*
static const char    MK_LBL_TWR[]    = "TWR";
static const char    MK_LBL_ANN[]    = "ANN";
static const char    MK_LBL_XIRR[]   = "XIRR";
static const char    MK_LBL_MDD[]    = "MDD";
static const char    MK_LBL_REC[]    = "REC";
static const char    MK_LBL_NO_REC[] = "NO REC";
// MARKETS
static const int16_t MK_Y_MKT_SYM    = 16;    // the primary's mnemonic and window change, rows 16-22
static const int16_t MK_Y_MKT_BIG    = 24;    // the primary's last at 2x, rows 24-37
static const int16_t MK_X_MKT_CHG_R  = 76;    // the primary's window change ends here, left of the sparkline
static const int16_t MK_BIG_CHARS_MKT = 6;    // the 2x slot on MARKETS: x 1-70, before the sparkline
static const int16_t MK_X_SPARK = 79, MK_Y_SPARK = 16, MK_SPARK_W = 48, MK_SPARK_H = 16;   // rows 16-31, x 79-126
static const int16_t MK_Y_MKT_LIVE   = 33;    // LIVE / CLOSE under the sparkline, rows 33-37
static const int16_t MK_Y_MKT_ROW0   = 41;    // three secondary rows, Picopixel: 41, 49, 57
static const int16_t MK_MKT_ROW_H    = 8;
static const int16_t MK_MKT_ROWS     = 3;
static const int16_t MK_X_MKT_VAL_R  = 88;    // secondary rows: the last ends here, the change at MK_X_RIGHT
// TICKER
static const int16_t MK_Y_TK_CHART = 31, MK_TK_CHART_H = 17;   // the window chart, rows 31-47
static const int16_t MK_Y_TK_SESS  = 49, MK_TK_SESS_H  = 8;    // today's session, rows 49-56
// PORTFOLIO
static const int16_t MK_Y_PF_CHART = 31, MK_PF_CHART_H = 26;   // rows 31-56
static const int16_t MK_BIG_CHARS_PF = 6;    // PORTFOLIO's 2x slot, x 1-70, so TWR and a four-digit change fit beside it
static const int16_t MK_Y_MDD_BAR    = 57;   // MK_Y_PF_CHART + MK_PF_CHART_H: the drawdown's bracket under the chart
static const int16_t MK_MDD_TICK_H   = 3;    // the bracket's end ticks, rows 55-57, at the peak's and the trough's columns
// HOLDINGS
static const int16_t MK_Y_HD_ROW0 = 17, MK_HD_ROW_H = 9, MK_HD_ROWS = 4;   // rows at 17, 26, 35, 44
static const int16_t MK_X_HD_MID_R   = 84;    // TGT>NOW ends here; the return at MK_X_RIGHT
// The knob's window row (not in render.py): a band over the footer for 1.5 s.
static const int16_t MK_Y_STATUS     = 56;
static const int16_t MK_Y_STATUS_TOP = 8;    // the same band over the heading, rows 8-15: on PORTFOLIO's drawdown stop
static const int16_t MK_STATUS_H     = 8;
static const uint16_t MK_STATUS_MS   = 1500;

// ── colours, before color565 ────────────────────────────────────────────────
enum Col : uint8_t { C_BLACK = 0, C_AMBER, C_WHITE, C_DIM, C_GREEN, C_RED, C_RULE, C_BLUE, C_COUNT };
extern const uint8_t kColRgb[C_COUNT][3];

// The sign colours (docs/19, display.colors): green up and red down; blue up
// and red down for colour-blind readers; red up as in East Asia.
enum Colors : uint8_t { CS_GREEN_RED = 0, CS_BLUE_RED, CS_RED_UP };

// What a string is, for the host's layout dump; the panel ignores it.
enum Role : uint8_t {
  R_NONE = 0, R_TAPE, R_HEAD_TITLE, R_HEAD_TAG, R_HEAD_CUR, R_HEAD_RIGHT, R_ASOF, R_ERR, R_ERR_WHY,
  R_PRI_MN, R_PRI_CHG, R_PRI_LAST, R_LIVE, R_ROW_MN, R_ROW_VAL, R_ROW_CHG, R_BIG, R_CHG, R_FOOT,
  R_HD_SYM, R_HD_MID, R_HD_RET, R_LEGEND, R_STATUS, R_TWR, R_ANN, R_XIRR, R_MARKS, R_COUNT
};
extern const char *const kRoleKeys[R_COUNT];

// Where the layout draws. Coordinates are panel pixels; `top` is the top row of
// a 5x7 cell (Picopixel's baseline is MK_PICO_ASCENT below its top).
class Canvas {
 public:
  virtual ~Canvas() {}
  virtual void glyph5(int16_t x, int16_t top, char ch, Col c, uint8_t k) = 0;   // one classic 5x7 glyph, scaled k
  virtual void textP(int16_t x, int16_t top, const char *s, Col c) = 0;          // Picopixel, from its top
  virtual void dot(int16_t x, int16_t y, Col c) = 0;
  virtual void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Col c) = 0;  // Adafruit_GFX::writeLine
  virtual void hline(int16_t y, int16_t x0, int16_t x1, Col c) = 0;             // x1 inclusive
  virtual void fill(int16_t x, int16_t y, int16_t w, int16_t h, Col c) = 0;
  // A recorder's hooks: the whole string a put5 / putp is about to draw, and a
  // composite note (the tape's tokens) with no position of its own.
  virtual void label5(int16_t, int16_t, const char *, Col, uint8_t, bool, Role) {}
  virtual void labelP(int16_t, int16_t, const char *, Col, Role) {}
  virtual void note(Role, const char *) {}
};

// ── the pages ───────────────────────────────────────────────────────────────
enum Page : uint8_t { PG_MARKETS = 0, PG_TICKER, PG_PORTFOLIO, PG_HOLDINGS, PG_COUNT };
extern const char *const kPageNames[PG_COUNT];   // "MARKETS" "TICKER" "PORTFOLIO" "HOLDINGS"

// What the knob and the settings decide; the model has the rest.
struct View {
  uint8_t  page;         // Page
  uint8_t  preset;       // Preset, global for the market pages
  uint8_t  primary;      // MARKETS: the index shown large, as an offset into idx[] (the list rotates)
  uint8_t  ticker;       // TICKER: index into tk[]
  uint8_t  hdPage;       // HOLDINGS: which page of four rows
  uint8_t  mode;         // Mode: what the portfolio pages show
  int32_t  today;        // the local date, kNoDate without a clock (then nothing is STALE)
  uint32_t tapePhase;    // pixels the tape has scrolled; reduced modulo its cycle here
  bool     tapeOn;
  bool     showPx, showBench, showGross, showSession, showValue;
  uint8_t  colors;       // Colors
  bool     sortByNow;    // HOLDINGS by current share, largest first (holdings.sort = contribution)
  bool     bandMark;     // HOLDINGS: TGT>NOW in amber outside the band (holdings.band_mark)
  float    bandAbs, bandRel;   // rebal.bands: points off target, percent off target
  bool     feedBadge;    // feed.label = delayed_badge: LIVE under MK_LIVE_MAX_S behind the clock, else D and the minutes
  bool     mddStop;      // PORTFOLIO's third knob stop: the drawdown with its months, marks on the chart
  int      staleDays;    // AS OF older than this many weekdays is STALE (stale.history_days)
  // The exchanges from the settings, drawn with "--" while the app has sent no tape.
  uint8_t     nTapeNames;
  const char *tapeNames[kMaxTape];
  // The portfolio currency and the ticker symbol from the settings, for a
  // heading before any payload has arrived.
  const char *currency;
  const char *tickerSym;
};

// What a page decided while drawing, for the portal and the tests.
struct PageInfo {
  int32_t asof;          // the oldest series drawn, kNoDate on DATA ERR
  bool    stale;
  bool    error;         // DATA ERR
  uint8_t hdPages;       // HOLDINGS: pages of rows
  uint8_t hdPage;        // the one drawn
};

void drawPage(Canvas &c, const Model &m, const View &v, PageInfo *info);
// The knob's window feedback, over the footer: the presets in `mask` (a bit per
// Preset), the current one bright.
// `top` puts the band over the heading (rows 8-15, the rule at 15) instead of
// the footer: PORTFOLIO's drawdown stop keeps its MDD footer and its bracket on
// row 57 in view while the window changes.
void drawStatusRow(Canvas &c, uint8_t preset, uint16_t mask, bool top = false);
// The presets the knob may step to: the six standard ones, and an extra one
// while any stored series or portfolio has it (docs/19: only when HA publishes it).
uint16_t presetsAvailable(const Model &m);

// Exposed for the tests.
int16_t curveX(int16_t x0, int16_t w, int i, int n);
int16_t curveY(int16_t y0, int16_t h, uint16_t u);
// The downsampled point that first shows `day` in a window from..to: point i is
// the last bar at or before (i + 1) of 128 equal steps (market_ref.downsample),
// so a bar at offset o of a span S is first shown by ceil(o 128 / S) - 1 (render.slot_index).
int16_t slotIndex(int32_t from, int32_t to, int32_t day);

}  // namespace market

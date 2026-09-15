// Stock market dashboard: the four 128 x 64 pages on the HUB75 display, and
// the knob inside them.
//
// The pages themselves are market_layout.cpp, drawn here through a Canvas on
// the display (the classic 5x7 through drawChar, glyph by glyph, so a number's
// thin space and dot advance half a cell; PicopixelFB through print). The
// host draws the same layout into a raster and holds it against the approved
// previews (tools/market/panel/check_market_panel.py), so this file is only
// the glue: colours as color565, the clock's date, the tape's phase, the
// knob's modes and the window row.
//
// The knob (docs/18): a click enters WINDOW - rotate steps the preset among
// the ones on hand, shown in a status row over the footer for 1.5 s; on
// MARKETS a second click makes rotate choose the primary index, on TICKER the
// ticker, on HOLDINGS the page of rows; PORTFOLIO has two stops. The next
// click, or MARKET_ENTER_TIMEOUT_MS of quiet, leaves. The preset and the
// ticker are the panel's, kept in NVS through market_ha.cpp.

#include "market.h"

#if defined(MARKET_ENABLED)

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"
#include "../panel/panel.h"

using namespace market;

// ── the knob ────────────────────────────────────────────────────────────────
// Not market::Mode (hold / rebal): this is where the knob's turns go.
enum KnobMode : uint8_t { KNOB_NONE = 0, KNOB_WINDOW, KNOB_LIST };
static uint8_t  s_mode     = KNOB_NONE;
static uint8_t  s_modeSub  = 0;        // the page the mode belongs to
static uint8_t  s_primary  = 0;        // MARKETS: the index shown large
static uint8_t  s_hdPage   = 0;        // HOLDINGS: the page of rows
static uint32_t s_statusUntil = 0;     // the window row shows until this millis(), 0 = not shown
static uint32_t s_tapePhase = 0;       // pixels the tape has scrolled

static const char *const MK_T_WINDOW = "TURN: WINDOW";
static const char *const MK_T_INDEX  = "TURN: INDEX";
static const char *const MK_T_TICKER = "TURN: TICKER";
static const char *const MK_T_ROWS   = "TURN: ROWS";
static const char *const MK_T_MDD    = "DRAWDOWN";
static const char *const MK_T_PAGES  = "TURN: PAGES";
static const char *const MK_W_NOMEM  = "NO MEMORY";

// The ticker on screen: display.ticker when the list has it, else the first
// (market_ha.cpp moves a stored one the list lost, and logs it, at boot).
static uint8_t tickerIndex() {
  const Model &m = model();
  const char *want = selectedTicker();
  for (uint8_t i = 0; i < m.nTk; i++)
    if (!strcmp(m.tk[i].sym, want)) return i;
  return 0;
}

static uint8_t holdingsPages() {
  const Holdings &h = model().hd[mks::shownMode(settings())];
  const int rows = (h.have ? h.n : 0) + 1;
  return (uint8_t)((rows + MK_HD_ROWS - 1) / MK_HD_ROWS);
}

bool marketKnobClick(uint8_t sub, bool entered) {
  if (!entered || s_mode == KNOB_NONE || s_modeSub != sub) {
    s_mode    = KNOB_WINDOW;
    s_modeSub = sub;
    s_statusUntil = millis() + MK_STATUS_MS;
    return true;
  }
  if (s_mode == KNOB_WINDOW) {   // the page's own stop; PORTFOLIO's is the drawdown
    s_mode = KNOB_LIST;
    s_statusUntil = 0;
    return true;
  }
  s_mode = KNOB_NONE;
  s_statusUntil = 0;
  return false;
}

const char *marketKnobHint() {
  if (s_mode == KNOB_WINDOW) return MK_T_WINDOW;
  if (s_mode == KNOB_LIST) {
    switch (s_modeSub) {
    case PG_MARKETS:  return MK_T_INDEX;
    case PG_TICKER:   return MK_T_TICKER;
    case PG_HOLDINGS: return MK_T_ROWS;
    case PG_PORTFOLIO: return MK_T_MDD;
    default:          break;
    }
  }
  return MK_T_PAGES;
}

void marketKnob(uint8_t sub, int8_t d) {
  if (!ready()) return;
  const Model &m = model();
  const int step = d > 0 ? 1 : -1;
  const bool mddStop = s_mode == KNOB_LIST && s_modeSub == sub && sub == PG_PORTFOLIO;
  if (s_mode == KNOB_LIST && s_modeSub == sub && !mddStop) {
    switch (sub) {
    case PG_MARKETS:
      if (m.nIdx) s_primary = (uint8_t)((s_primary + step + m.nIdx) % m.nIdx);
      break;
    case PG_TICKER:
      if (m.nTk) noteTicker(m.tk[(tickerIndex() + step + m.nTk) % m.nTk].sym);
      break;
    case PG_HOLDINGS: {
      const uint8_t pages = holdingsPages();
      s_hdPage = (uint8_t)((s_hdPage + step + pages) % pages);
      break;
    }
    default: break;
    }
    return;
  }
  // WINDOW: the next preset the model has, in knob order.
  const uint16_t mask = presetsAvailable(m);
  uint8_t p = mks::windowPreset(settings());
  for (int k = 0; k < kPresets; k++) {
    p = (uint8_t)((p + step + kPresets) % kPresets);
    if (mask & (1u << p)) break;
  }
  noteWindow(p);
  if (!mddStop) {                  // in the drawdown stop the window steps and the drawdown stays
    s_mode    = KNOB_WINDOW;
    s_modeSub = sub;
  }
  s_statusUntil = millis() + MK_STATUS_MS;
}

void market::pageTick() {
  // Left by a click, the timeout, the carousel or the portal: the mode goes with it.
  const bool here = panelEnteredPage() && panelPageKey(panelCurrentPage()) == PANEL_KEY_MARKET;
  if (s_mode != KNOB_NONE && !here) {
    s_mode = KNOB_NONE;
    s_statusUntil = 0;
  }
}

uint8_t marketRefreshHz() {
  if (!ready()) return 5;
  const mks::Settings &s = settings();
  return (mks::flag(s, mks::I_TAPE_ON) && mks::option(s, mks::I_TAPE_SPEED) > 0) ? 20 : 5;
}

// ── drawing ─────────────────────────────────────────────────────────────────
static uint16_t s_col[C_COUNT];
static bool     s_colReady = false;

static void colours() {
  if (s_colReady) return;
  for (int c = 0; c < C_COUNT; c++) s_col[c] = display.color565(kColRgb[c][0], kColRgb[c][1], kColRgb[c][2]);
  s_colReady = true;
}

// The layout's Canvas on the HUB75 display.
class PanelCanvas : public Canvas {
 public:
  void glyph5(int16_t x, int16_t top, char ch, Col c, uint8_t k) override {
    display.setFont(NULL);
    display.drawChar(x, top, (unsigned char)ch, s_col[c], s_col[c], k);   // bg == colour: no background
  }
  void textP(int16_t x, int16_t top, const char *s, Col c) override {
    display.setFont(&PicopixelFB);
    display.setTextColor(s_col[c]);
    display.setCursor(x, (int16_t)(top + MK_PICO_ASCENT));
    display.print(s);
  }
  void dot(int16_t x, int16_t y, Col c) override { display.drawPixel(x, y, s_col[c]); }
  void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Col c) override { display.drawLine(x0, y0, x1, y1, s_col[c]); }
  void hline(int16_t y, int16_t x0, int16_t x1, Col c) override { display.drawFastHLine(x0, y, (int16_t)(x1 - x0 + 1), s_col[c]); }
  void fill(int16_t x, int16_t y, int16_t w, int16_t h, Col c) override { display.fillRect(x, y, w, h, s_col[c]); }
};

static int32_t today() {
  const time_t t = time(nullptr);
  if (t < 1700000000) return kNoDate;   // before NTP the clock reads 1970
  struct tm lt;
  localtime_r(&t, &lt);
  return daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static View buildView(uint8_t sub) {
  const mks::Settings &s = settings();
  View v;
  memset(&v, 0, sizeof(v));
  v.page    = sub;
  v.preset  = mks::windowPreset(s);
  v.primary = s_primary;
  v.ticker  = tickerIndex();
  v.hdPage  = s_hdPage;
  v.mode    = mks::shownMode(s);
  v.today   = today();
  v.tapePhase   = s_tapePhase;
  v.tapeOn      = mks::flag(s, mks::I_TAPE_ON);
  v.showPx      = mks::flag(s, mks::I_LINE_PX);
  v.showBench   = mks::flag(s, mks::I_LINE_BENCH);
  v.showGross   = mks::flag(s, mks::I_LINE_GROSS);
  v.showSession = mks::flag(s, mks::I_SESSION);
  v.showValue   = mks::flag(s, mks::I_LINE_VALUE);
  v.colors      = (uint8_t)mks::option(s, mks::I_COLORS);
  v.sortByNow   = mks::option(s, mks::I_HD_SORT) == 1;
  v.bandMark    = mks::flag(s, mks::I_HD_BAND_MARK);
  v.bandAbs     = mks::number(s, mks::I_BAND_ABS);
  v.bandRel     = mks::number(s, mks::I_BAND_REL);
  v.feedBadge   = mks::option(s, mks::I_FEED_LABEL) == 1;
  v.mddStop     = sub == PG_PORTFOLIO && s_mode == KNOB_LIST && s_modeSub == PG_PORTFOLIO;
  v.staleDays   = mks::option(s, mks::I_STALE_HISTORY_DAYS);
  PageBuf &pb = pageBuf();
  const int n = mks::parseNames(mks::str(s, mks::I_TAPE), pb.tape, kMaxTape);
  v.nTapeNames = (uint8_t)(n < 0 ? 0 : n);
  for (uint8_t i = 0; i < v.nTapeNames; i++) v.tapeNames[i] = pb.tape[i];
  snprintf(pb.currency, sizeof(pb.currency), "%s", mks::optionName(mks::I_CURRENCY, mks::option(s, mks::I_CURRENCY)));
  v.currency  = pb.currency;
  v.tickerSym = mks::str(s, mks::I_TICKER);
  return v;
}

void marketRender(uint8_t sub) {
  colours();
  display.setTextSize(1);
  display.setTextWrap(false);
  if (!ready()) {
    display.setFont(NULL);
    display.setTextColor(s_col[C_AMBER]);
    display.setCursor((int16_t)((W - w5(MK_W_NOMEM, 1, false)) / 2), 28);
    display.print(MK_W_NOMEM);
    return;
  }
  PanelCanvas canvas;
  const View v = buildView(sub < PG_COUNT ? sub : (uint8_t)PG_MARKETS);
  PageInfo info;
  drawPage(canvas, model(), v, &info);
  if (s_hdPage != info.hdPage && sub == PG_HOLDINGS) s_hdPage = info.hdPage;   // fewer pages than before
  if (s_statusUntil) {
    // Over the heading on the drawdown stop, whose MDD footer and bracket (row 57) stay in view.
    if ((int32_t)(millis() - s_statusUntil) < 0) drawStatusRow(canvas, v.preset, presetsAvailable(model()), v.mddStop);
    else s_statusUntil = 0;
  }
  // The tape moves tape.speed pixels a frame, at the 20 fps this page asks for.
  if (v.tapeOn) s_tapePhase += (uint32_t)mks::option(settings(), mks::I_TAPE_SPEED);
  display.setFont(NULL);   // other pages draw with the built-in font and would inherit this one
}

#endif  // MARKET_ENABLED

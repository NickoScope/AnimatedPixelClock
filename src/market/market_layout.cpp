#include "market_layout.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace market {

const uint8_t kColRgb[C_COUNT][3] = {
  {0, 0, 0},        // C_BLACK
  {255, 150, 0},    // C_AMBER  the panel's amber (railboard.cpp, media_page.cpp)
  {255, 255, 255},  // C_WHITE
  {110, 122, 128},  // C_DIM    the design's dim
  {60, 200, 90},    // C_GREEN  softened: never 0/255
  {230, 70, 60},    // C_RED
  {36, 40, 44},     // C_RULE   structural, not semantic: the row under the tape
  {60, 150, 255},   // C_BLUE   the up colour of the blue/red scheme
};

const char *const kRoleKeys[R_COUNT] = {
  "none", "tape", "head.title", "head.tag", "head.cur", "head.right", "asof", "err", "err.why",
  "pri.mn", "pri.chg", "pri.last", "live", "row.mn", "row.val", "row.chg", "big", "chg", "foot",
  "hd.sym", "hd.mid", "hd.ret", "legend", "status", "twr", "ann", "xirr", "marks"};

const char *const kPageNames[PG_COUNT] = {"MARKETS", "TICKER", "PORTFOLIO", "HOLDINGS"};

// ── text primitives, render.py's Frame methods ──────────────────────────────
// Adafruit GFX drawChar for the classic font. With `thin`, a space advances
// half a cell and a dot is drawn 2 columns left and advances half a cell, so
// `7 620` and `699.3` read as one number. Returns the column after the ink.
static int16_t put5(Canvas &f, int16_t x, int16_t top, const char *s, Col c, uint8_t k = 1, bool thin = false,
                    Role r = R_NONE) {
  f.label5(x, top, s, c, k, thin, r);
  for (const char *p = s; *p; p++) {
    if (thin && *p == ' ') { x = (int16_t)(x + MK_THIN_ADV * k); continue; }
    const int16_t dx0 = (thin && *p == '.') ? (int16_t)(-2 * k) : 0;
    f.glyph5((int16_t)(x + dx0), top, *p, c, k);
    x = (int16_t)(x + ((thin && *p == '.') ? MK_THIN_ADV : MK_ADV) * k);
  }
  return (int16_t)(x - k);
}

static int16_t put5Right(Canvas &f, int16_t right, int16_t top, const char *s, Col c, uint8_t k = 1, bool thin = false,
                         Role r = R_NONE) {
  const int16_t x = (int16_t)(right - w5(s, k, thin) + 1);
  put5(f, x, top, s, c, k, thin, r);
  return x;
}

static int16_t putp(Canvas &f, int16_t x, int16_t top, const char *s, Col c, Role r = R_NONE) {
  f.labelP(x, top, s, c, r);
  f.textP(x, top, s, c);
  return (int16_t)(x + picoInk(s));   // the column after the ink
}

static int16_t putpRight(Canvas &f, int16_t right, int16_t top, const char *s, Col c, Role r = R_NONE) {
  const int16_t x = (int16_t)(right - picoInk(s) + 1);
  putp(f, x, top, s, c, r);
  return x;
}

struct Seg { const char *s; Col c; };

// Coloured pieces of one Picopixel string; spacing as if drawn in one go.
static int16_t putpSegs(Canvas &f, int16_t x, int16_t top, const Seg *segs, int n, Role r = R_NONE) {
  for (int i = 0; i < n; i++) {
    f.labelP(x, top, segs[i].s, segs[i].c, r);
    f.textP(x, top, segs[i].s, segs[i].c);
    x = (int16_t)(x + picoAdv(segs[i].s));
  }
  return x;
}

static int16_t putpSegsRight(Canvas &f, int16_t right, int16_t top, const Seg *segs, int n, Role r = R_NONE) {
  char full[96] = "";
  size_t k = 0;
  for (int i = 0; i < n; i++) {
    const size_t len = strlen(segs[i].s);
    if (k + len >= sizeof(full)) break;
    memcpy(full + k, segs[i].s, len);
    k += len;
  }
  full[k] = '\0';
  return putpSegs(f, (int16_t)(right - picoInk(full) + 1), top, segs, n, r);
}

// ── numbers and words, the design's rules ───────────────────────────────────
static Col pctCol(double x, bool have, uint8_t colors) {
  const int s = pctSign(x, have);
  if (s == 0) return C_DIM;
  switch (colors) {
  case CS_BLUE_RED: return s > 0 ? C_BLUE : C_RED;
  case CS_RED_UP:   return s > 0 ? C_RED : C_GREEN;
  default:          return s > 0 ? C_GREEN : C_RED;
  }
}

static int staleDays(const View &v) { return v.staleDays > 0 ? v.staleDays : kStaleTradingDays; }

// The sign colour of a fall: the drawdown's marks use it.
static Col downCol(uint8_t colors) { return colors == CS_RED_UP ? C_GREEN : C_RED; }

// `ANN +8.6%`: the word dim, the value in its sign colour; nothing under a year.
static int annSegs(bool have, float ann, uint8_t colors, Seg *segs, char *word, size_t wcap, char *pct, size_t pcap) {
  if (!have) return 0;
  snprintf(word, wcap, "%s ", MK_LBL_ANN);
  fmtPct(pct, pcap, ann, true);
  segs[0] = {word, C_DIM};
  segs[1] = {pct, pctCol(ann, true, colors)};
  return 2;
}

// A bit per preset that some stored series or portfolio has.
static uint16_t dataMask(const Model &m) {
  uint16_t mask = 0;
  for (uint8_t p = 0; p < kPresets; p++) {
    bool any = m.pf[M_HOLD][p].have || m.pf[M_REBAL][p].have;
    for (uint8_t i = 0; i < m.nIdx && !any; i++) any = m.idx[i].p[p].have;
    for (uint8_t i = 0; i < m.nTk && !any; i++) any = m.tk[i].p[p].have;
    if (any) mask |= (uint16_t)(1u << p);
  }
  return mask;
}

static Col stateCol(uint8_t st) {
  switch (st) {
  case X_OPEN:  return C_GREEN;
  case X_PRE:
  case X_POST:
  case X_STALE: return C_AMBER;
  default:      return C_DIM;   // CLOSED, and "--" when the app has none
  }
}

// ── shared pieces ───────────────────────────────────────────────────────────
// `tokens` = the app's tape, or the settings' exchanges with "--". Scrolled by
// the frame clock: MK_TAPE_FPS px per second, cycling over the tape's length.
static void drawTape(Canvas &f, const Model &m, const View &v) {
  static const char kSep[] = "   ";
  Seg segs[kMaxTape * 3];
  char names[kMaxTape][kExNameLen + 1];
  char noteBuf[kMaxTape * (kExNameLen + 8) + 1] = "";
  int n = 0;
  size_t nk = 0;
  const int count = m.tape.have ? m.tape.n : v.nTapeNames;
  for (int i = 0; i < count && i < kMaxTape; i++) {
    const char *name = m.tape.have ? m.tape.x[i].name : v.tapeNames[i];
    const uint8_t st = m.tape.have ? m.tape.x[i].state : (uint8_t)X_NONE;
    snprintf(names[i], sizeof(names[i]), "%s ", name);
    segs[n++] = {names[i], C_DIM};
    segs[n++] = {kExStateKeys[st < X_COUNT ? st : (uint8_t)X_NONE], stateCol(st)};
    segs[n++] = {kSep, C_DIM};
    nk += (size_t)snprintf(noteBuf + nk, sizeof(noteBuf) - nk, "%s%s %s", i ? "  " : "", name,
                           kExStateKeys[st < X_COUNT ? st : (uint8_t)X_NONE]);
    if (nk >= sizeof(noteBuf)) nk = sizeof(noteBuf) - 1;
  }
  f.note(R_TAPE, noteBuf);
  if (n) {
    int16_t cycle = 0;
    for (int i = 0; i < n; i++) cycle = (int16_t)(cycle + picoAdv(segs[i].s));
    int16_t x = (int16_t)(-(int16_t)(v.tapePhase % (uint32_t)cycle));
    while (x < W) {
      putpSegs(f, x, MK_Y_TAPE, segs, n, R_TAPE);
      x = (int16_t)(x + cycle);
    }
  }
  f.hline(MK_Y_TAPE_RULE, 0, W - 1, C_RULE);
}

static int16_t head(Canvas &f, const char *title, const char *tag, const char *cur) {
  int16_t x = put5(f, MK_X_LEFT, MK_Y_HEAD, title, C_AMBER, 1, false, R_HEAD_TITLE);
  const int16_t y = MK_Y_HEAD + MK_PICO_DROP;
  if (tag && *tag) x = putp(f, (int16_t)(x + MK_GAP), y, tag, C_AMBER, R_HEAD_TAG);
  if (cur && *cur) x = putp(f, (int16_t)(x + MK_GAP), y, cur, C_DIM, R_HEAD_CUR);
  return x;
}

// STALE dd MON in amber, else AS OF dd MON (or the bare date) in dim; nothing
// on DATA ERR, which has no date.
static void asofSlot(Canvas &f, int16_t top, const PageInfo &info, bool label = true, int16_t right = MK_X_RIGHT) {
  if (info.error) return;
  char d[8], s[24];
  fmtDate(d, sizeof(d), info.asof);
  if (info.stale) snprintf(s, sizeof(s), "STALE %s", d);
  else            snprintf(s, sizeof(s), "%s%s", label ? "AS OF " : "", d);
  putpRight(f, right, top, s, info.stale ? C_AMBER : C_DIM, R_ASOF);
}

// The quote the app has for a symbol, if any.
static const Quote *quoteOf(const Model &m, const char *sym) {
  if (!m.live.have) return nullptr;
  for (uint8_t i = 0; i < m.live.n; i++)
    if (!strcmp(m.live.q[i].sym, sym)) return &m.live.q[i];
  return nullptr;
}

// While the exchange is OPEN: the day change behind LIVE in green when the quote
// is less than MK_LIVE_MAX_S behind the clock, else behind the delay badge and
// the whole minutes in dim (D15; owner, 2026-09-15 11:43). A quote without
// delay_s that the app calls DELAYED gets the bare badge. feed.label = live
// keeps LIVE whatever the delay. Else CLOSE and the date.
static int liveSegs(const Quote *q, int32_t asof, const View &v, Seg *segs, char *pct, size_t pcap, char *date, size_t dcap,
                    char *badge, size_t bcap) {
  if (q && q->state == X_OPEN) {
    fmtPct(pct, pcap, q->day, q->hasDay);
    const bool delayed = v.feedBadge && (q->hasDelay ? q->delayS >= MK_LIVE_MAX_S : q->feed == F_DELAYED);
    if (!delayed) {
      segs[0] = {"LIVE ", C_GREEN};
    } else {
      if (q->hasDelay) snprintf(badge, bcap, "%s%u ", MK_DELAY_BADGE, (unsigned)(q->delayS / 60));
      else             snprintf(badge, bcap, "%s ", MK_DELAY_BADGE);
      segs[0] = {badge, C_DIM};
    }
    segs[1] = {pct, pctCol(q->day, q->hasDay, v.colors)};
    return 2;
  }
  fmtDate(date, dcap, asof);
  segs[0] = {"CLOSE ", C_DIM};
  segs[1] = {date, C_DIM};
  return 2;
}

// DATA ERR with the app's reason when it reports one, else NOTHING STORED;
// when the model has something for other windows, the window is what is missing.
static void drawError(Canvas &f, const Model &m, const View &v) {
  static const char kErr[] = "DATA ERR";
  put5(f, (int16_t)((W - w5(kErr, 1, false)) / 2), MK_Y_ERR, kErr, C_RED, 1, false, R_ERR);
  char why[32];
  if (m.status.have && m.status.state == A_ERROR && m.status.err[0]) snprintf(why, sizeof(why), "%s", m.status.err);
  else if (m.gen && !(dataMask(m) & (1u << v.preset))) snprintf(why, sizeof(why), "NO %s DATA", kPresetKeys[v.preset]);
  else snprintf(why, sizeof(why), "NOTHING STORED");
  putp(f, (int16_t)((W - picoInk(why)) / 2), MK_Y_ERR_WHY, why, C_DIM, R_ERR_WHY);
}

// ── curves ──────────────────────────────────────────────────────────────────
int16_t curveX(int16_t x0, int16_t w, int i, int n) {
  return (int16_t)(x0 + (n > 1 ? (2 * i * (w - 1) + (n - 1)) / (2 * (n - 1)) : 0));
}

int16_t slotIndex(int32_t from, int32_t to, int32_t day) {
  const int32_t span = to - from;
  if (span <= 0) return kPoints - 1;
  int32_t o = day - from;
  if (o < 0) o = 0;
  if (o > span) o = span;
  const int32_t c = (o * kPoints + span - 1) / span;   // ceil(o 128 / span)
  return (int16_t)(c > 0 ? c - 1 : 0);
}

int16_t curveY(int16_t y0, int16_t h, uint16_t u) {
  return (int16_t)(y0 + (h - 1) - (int32_t)(((int32_t)u * (h - 1) + 32767) / 65535));
}

// 128 uint16 points across w columns, the panel's own mapping. A dotted line
// keeps every other column and does not join the dots; `nValid` slots are
// drawn, the rest are not yet.
static void drawCurve(Canvas &f, int16_t x0, int16_t y0, int16_t w, int16_t h, const uint16_t *u16, Col c,
                      int nValid = kPoints, bool dotted = false) {
  const int n = kPoints;
  const int m = nValid < 0 ? 0 : (nValid > n ? n : nValid);
  if (!m) return;
  if (dotted) {
    for (int i = 0; i < m; i++) {
      const int16_t x = curveX(x0, w, i, n);
      if ((x - x0) % 2 == 0) f.dot(x, curveY(y0, h, u16[i]), c);
    }
    return;
  }
  int16_t ax = curveX(x0, w, 0, n), ay = curveY(y0, h, u16[0]);
  f.dot(ax, ay, c);
  for (int i = 1; i < m; i++) {
    const int16_t bx = curveX(x0, w, i, n), by = curveY(y0, h, u16[i]);
    f.line(ax, ay, bx, by, c);
    ax = bx;
    ay = by;
  }
}

static void drawTicks(Canvas &f, int16_t x0, int16_t y0, int16_t w, int16_t h) {
  for (int t = 0; t < 3; t++)
    for (int16_t k = 0; k < MK_TICK_H; k++)
      f.dot((int16_t)(x0 + MK_TICKS[t] * (w - 1) / (W - 1)), (int16_t)(y0 + h - 1 - k), C_DIM);
}

// ── the pages ───────────────────────────────────────────────────────────────
static int32_t older(int32_t a, int32_t b) {
  if (a == kNoDate) return b;
  if (b == kNoDate) return a;
  return a < b ? a : b;
}

// Heading MARKETS + window tag, AS OF at the right; the primary index:
// mnemonic, window change, last at 2x, a 48x16 sparkline, LIVE / CLOSE; three
// secondary rows. The index list rotates so v.primary comes first.
static void pageMarkets(Canvas &f, const Model &m, const View &v, PageInfo &info) {
  const Series *order[kMaxWatch];
  const Symbol *syms[kMaxWatch];
  int n = 0;
  for (uint8_t k = 0; k < m.nIdx && k < kMaxWatch; k++) {
    const uint8_t i = (uint8_t)((v.primary + k) % m.nIdx);
    const Series &s = m.idx[i].p[v.preset];
    if (!s.have) continue;
    order[n] = &s;
    syms[n++] = &m.idx[i];
  }
  info.error = n == 0;
  for (int k = 0; k < n; k++) info.asof = older(info.asof, order[k]->to);
  info.stale = !info.error && isStale(info.asof, v.today, staleDays(v));

  head(f, "MARKETS", kPresetKeys[v.preset], nullptr);
  asofSlot(f, MK_Y_HEAD + MK_PICO_DROP, info);
  if (info.error) { drawError(f, m, v); return; }

  const Series &p = *order[0];
  const char *mn = syms[0]->name[0] ? syms[0]->name : (p.name[0] ? p.name : p.sym);
  char chg[16], big[16], pct[16], date[8], badge[12];
  put5(f, MK_X_LEFT, MK_Y_MKT_SYM, mn, C_WHITE, 1, false, R_PRI_MN);
  fmtPct(chg, sizeof(chg), p.chg, true);
  put5Right(f, MK_X_MKT_CHG_R, MK_Y_MKT_SYM, chg, pctCol(p.chg, true, v.colors), 1, true, R_PRI_CHG);
  fmtValue(big, sizeof(big), p.last, MK_BIG_CHARS_MKT);
  put5(f, MK_X_LEFT, MK_Y_MKT_BIG, big, C_WHITE, MK_BIG, true, R_PRI_LAST);
  drawCurve(f, MK_X_SPARK, MK_Y_SPARK, MK_SPARK_W, MK_SPARK_H, p.pts, C_WHITE);
  Seg segs[2];
  const int ns = liveSegs(quoteOf(m, p.sym), info.asof, v, segs, pct, sizeof(pct), date, sizeof(date), badge, sizeof(badge));
  putpSegsRight(f, MK_X_RIGHT, MK_Y_MKT_LIVE, segs, ns, R_LIVE);

  for (int k = 1; k < n && k <= MK_MKT_ROWS; k++) {
    const int16_t y = (int16_t)(MK_Y_MKT_ROW0 + (k - 1) * MK_MKT_ROW_H);
    const Series &q = *order[k];
    const char *rmn = syms[k]->name[0] ? syms[k]->name : (q.name[0] ? q.name : q.sym);
    char val[16], rchg[16];
    fmtValue(val, sizeof(val), q.last, 7);
    fmtPct(rchg, sizeof(rchg), q.chg, true);
    putp(f, MK_X_LEFT, y, rmn, C_DIM, R_ROW_MN);
    putpRight(f, MK_X_MKT_VAL_R, y, val, C_DIM, R_ROW_VAL);
    putpRight(f, MK_X_RIGHT, y, rchg, pctCol(q.chg, true, v.colors), R_ROW_CHG);
  }
}

// Heading: symbol, window tag, currency. The last at 2x; the window change on
// the top line, LIVE / CLOSE on the bottom line; the window chart with three
// ticks; the session strip; footer HI / LO and AS OF.
static void pageTicker(Canvas &f, const Model &m, const View &v, PageInfo &info) {
  const Symbol *sym = (m.nTk && v.ticker < m.nTk) ? &m.tk[v.ticker] : (m.nTk ? &m.tk[0] : nullptr);
  const Series *p = (sym && sym->p[v.preset].have) ? &sym->p[v.preset] : nullptr;
  info.error = p == nullptr;
  if (p) { info.asof = p->to; info.stale = isStale(info.asof, v.today, staleDays(v)); }

  const char *symText = sym ? sym->sym : (v.tickerSym ? v.tickerSym : "");
  head(f, symText[0] ? symText : "TICKER", kPresetKeys[v.preset], p ? p->cur : nullptr);
  if (info.error) { drawError(f, m, v); return; }

  char big[16], chg[16], pct[16], date[8], hi[16], lo[16], foot[48], badge[12], annWord[8], annPct[16];
  // ANN at the heading's right from a one-year window (owner, 11:43): the row
  // under the change already carries LIVE / D / CLOSE.
  Seg ann[2];
  const int na = annSegs(p->hasAnn, p->ann, v.colors, ann, annWord, sizeof(annWord), annPct, sizeof(annPct));
  if (na) putpSegsRight(f, MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, ann, na, R_ANN);
  fmtValue(big, sizeof(big), p->last, MK_BIG_CHARS);
  put5(f, MK_X_LEFT, MK_Y_BLOCK, big, C_WHITE, MK_BIG, true, R_BIG);
  fmtPct(chg, sizeof(chg), p->chg, true);
  put5Right(f, MK_X_RIGHT, MK_Y_BLOCK, chg, pctCol(p->chg, true, v.colors), 1, true, R_CHG);
  Seg segs[2];
  const int ns = liveSegs(quoteOf(m, p->sym), info.asof, v, segs, pct, sizeof(pct), date, sizeof(date), badge, sizeof(badge));
  putpSegsRight(f, MK_X_RIGHT, MK_Y_BLOCK2 + MK_PICO_DROP, segs, ns, R_LIVE);
  drawTicks(f, 0, MK_Y_TK_CHART, W, MK_TK_CHART_H);
  drawCurve(f, 0, MK_Y_TK_CHART, W, MK_TK_CHART_H, p->pts, C_WHITE);
  drawTicks(f, 0, MK_Y_TK_SESS, W, MK_TK_SESS_H);
  if (v.showSession && m.intra.have && !strcmp(m.intra.sym, p->sym))
    drawCurve(f, 0, MK_Y_TK_SESS, W, MK_TK_SESS_H, m.intra.pts, C_DIM, m.intra.n);
  fmtAmount(hi, sizeof(hi), p->hi, 7);
  fmtAmount(lo, sizeof(lo), p->lo, 7);
  snprintf(foot, sizeof(foot), "HI %s  LO %s", hi, lo);
  putp(f, MK_X_LEFT, MK_Y_FOOT, foot, C_DIM, R_FOOT);
  asofSlot(f, MK_Y_FOOT, info);
}

// Heading PORTFOLIO + currency, mode and window tag at the right. The value at
// 2x and the window change; the value line bright, the no-dividend line dim,
// the benchmark dim and dotted; footer DIV, TER~, CASH and the bare date.
static void pagePortfolio(Canvas &f, const Model &m, const View &v, PageInfo &info) {
  const Portfolio &p = m.pf[v.mode < M_COUNT ? v.mode : (uint8_t)M_HOLD][v.preset];
  info.error = !p.have;
  if (p.have) { info.asof = p.to; info.stale = isStale(info.asof, v.today, staleDays(v)); }

  head(f, "PORTFOLIO", nullptr, p.have ? p.cur : v.currency);
  char right[16];
  snprintf(right, sizeof(right), "%s  %s", v.mode == M_REBAL ? "REBAL" : "HOLD", kPresetKeys[v.preset]);
  putpRight(f, MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, right, C_AMBER, R_HEAD_RIGHT);
  if (info.error) { drawError(f, m, v); return; }

  // The value in a 6-character slot, the window's TWR beside it with its label,
  // ANN under that from a one-year window (owner, 2026-09-15 11:43). A payload
  // without twr draws chg: the same figure while nothing flows.
  char big[16], chg[16], annWord[8], annPct[16];
  fmtValue(big, sizeof(big), p.value, MK_BIG_CHARS_PF);
  put5(f, MK_X_LEFT, MK_Y_BLOCK, big, C_WHITE, MK_BIG, true, R_BIG);
  const float twr = p.hasTwr ? p.twr : p.chg;
  fmtPct(chg, sizeof(chg), twr, true);
  const int16_t xChg = put5Right(f, MK_X_RIGHT, MK_Y_BLOCK, chg, pctCol(twr, true, v.colors), 1, true, R_CHG);
  putpRight(f, (int16_t)(xChg - MK_GAP - 1), MK_Y_BLOCK + MK_PICO_DROP, MK_LBL_TWR, C_DIM, R_TWR);
  if (v.mddStop) {
    asofSlot(f, MK_Y_BLOCK2 + MK_PICO_DROP, info, false);   // pushed out of the footer into ANN's place
  } else {
    Seg ann[2];
    const int na = annSegs(p.hasAnn, p.ann, v.colors, ann, annWord, sizeof(annWord), annPct, sizeof(annPct));
    if (na) putpSegsRight(f, MK_X_RIGHT, MK_Y_BLOCK2 + MK_PICO_DROP, ann, na, R_ANN);
  }
  drawTicks(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H);
  if (p.hasBench && v.showBench) drawCurve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, p.bench, C_DIM, kPoints, true);
  if (p.hasGross && v.showGross) drawCurve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, p.gross, C_DIM, kPoints, true);
  if (v.showPx) drawCurve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, p.px, C_DIM);
  if (v.showValue) drawCurve(f, 0, MK_Y_PF_CHART, W, MK_PF_CHART_H, p.pts, C_WHITE);

  if (v.mddStop) {
    // The drawdown stop: MDD, the peak's and the trough's months, the recovery
    // month or NO REC; the fall redrawn on the value line in the fall's colour,
    // and a bracket under the chart between their columns.
    const Col down = downCol(v.colors);
    char word[8], pct[16], ymA[8], ymB[8], ymR[8], tail[40];
    Seg segs[3];
    int ns;
    snprintf(word, sizeof(word), "%s ", MK_LBL_MDD);
    if (p.mddPeak != kNoDate && p.mddTrough != kNoDate) {
      const int16_t i0 = slotIndex(p.from, p.to, p.mddPeak), i1 = slotIndex(p.from, p.to, p.mddTrough);
      int16_t ax = curveX(0, W, i0, kPoints), ay = curveY(MK_Y_PF_CHART, MK_PF_CHART_H, p.pts[i0]);
      f.dot(ax, ay, down);
      for (int i = i0 + 1; i <= i1; i++) {
        const int16_t bx = curveX(0, W, i, kPoints), by = curveY(MK_Y_PF_CHART, MK_PF_CHART_H, p.pts[i]);
        f.line(ax, ay, bx, by, down);
        ax = bx;
        ay = by;
      }
      const int16_t xa = curveX(0, W, i0, kPoints), xb = curveX(0, W, i1, kPoints);
      f.hline(MK_Y_MDD_BAR, xa, xb, down);
      for (int16_t x : {xa, xb})
        for (int16_t k = 0; k < MK_MDD_TICK_H; k++) f.dot(x, (int16_t)(MK_Y_MDD_BAR - k), down);
      char marks[40];
      snprintf(marks, sizeof(marks), "%d,%d,%d,%d", i0, i1, xa, xb);
      f.note(R_MARKS, marks);
      fmtPct(pct, sizeof(pct), p.mdd, true);
      fmtYm(ymA, sizeof(ymA), p.mddPeak);
      fmtYm(ymB, sizeof(ymB), p.mddTrough);
      if (p.mddRecovery != kNoDate) {
        fmtYm(ymR, sizeof(ymR), p.mddRecovery);
        snprintf(tail, sizeof(tail), "  %s>%s  %s %s", ymA, ymB, MK_LBL_REC, ymR);
      } else {
        snprintf(tail, sizeof(tail), "  %s>%s  %s", ymA, ymB, MK_LBL_NO_REC);
      }
      segs[0] = {word, C_DIM};
      segs[1] = {pct, pctCol(p.mdd, true, v.colors)};
      segs[2] = {tail, C_DIM};
      ns = 3;
    } else {
      fmtPct(pct, sizeof(pct), 0.0, true);   // no fall at all
      segs[0] = {word, C_DIM};
      segs[1] = {pct, C_DIM};
      ns = 2;
    }
    putpSegs(f, MK_X_LEFT, MK_Y_FOOT, segs, ns, R_FOOT);
    return;
  }

  char div[16], ter[16], cash[16], foot[64];
  fmtAmount(div, sizeof(div), p.div, 4);
  fmtAmount(ter, sizeof(ter), p.terDrag, 4);
  fmtShare(cash, sizeof(cash), p.cash);
  if (p.flows) {
    // Money flows in: XIRR since inception, then DIV; TER~ and CASH give their
    // room (with TER~ kept, a four-digit XIRR and six-figure amounts overflow).
    char word[8], pct[16], rest[24];
    snprintf(word, sizeof(word), "%s ", MK_LBL_XIRR);
    fmtPct(pct, sizeof(pct), p.xirrAnn, p.hasXirr);
    if (info.stale) rest[0] = '\0';            // STALE dd MON takes DIV's room
    else snprintf(rest, sizeof(rest), "  DIV %s", div);
    Seg segs[3] = {{word, C_DIM}, {pct, pctCol(p.xirrAnn, p.hasXirr, v.colors)}, {rest, C_DIM}};
    putpSegs(f, MK_X_LEFT, MK_Y_FOOT, segs, 3, R_FOOT);
    f.note(R_XIRR, pct);
  } else {
    if (info.stale) snprintf(foot, sizeof(foot), "DIV %s  TER~ %s", div, ter);   // STALE dd MON needs the CASH item's room
    else            snprintf(foot, sizeof(foot), "DIV %s  TER~ %s  CASH %s", div, ter, cash);
    putp(f, MK_X_LEFT, MK_Y_FOOT, foot, C_DIM, R_FOOT);
  }
  asofSlot(f, MK_Y_FOOT, info, false);
}

// Heading HOLDINGS n/m + window tag, the mode at the right. Rows: symbol (5x7),
// target > now (Picopixel), return since entry (5x7, signed); CASH last.
// Footer: the column legend at the left, AS OF at the right.
static void pageHoldings(Canvas &f, const Model &m, const View &v, PageInfo &info) {
  const Holdings &h = m.hd[v.mode < M_COUNT ? v.mode : (uint8_t)M_HOLD];
  info.error = !h.have;
  if (h.have) { info.asof = h.asof; info.stale = isStale(info.asof, v.today, staleDays(v)); }
  const int n = h.have ? h.n : 0;
  const int rows = n + 1;   // CASH last
  const int pages = (rows + MK_HD_ROWS - 1) / MK_HD_ROWS;
  const int page = v.hdPage < pages ? v.hdPage : pages - 1;
  info.hdPages = (uint8_t)pages;
  info.hdPage  = (uint8_t)page;

  // The allocation's own order, or by current share, largest first (docs/19
  // holdings.sort); a stable insertion sort over at most sixteen rows.
  uint8_t order[kMaxPositions];
  for (int i = 0; i < n; i++) order[i] = (uint8_t)i;
  if (v.sortByNow)
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && h.rows[order[j]].now > h.rows[order[j - 1]].now; j--) {
        const uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
      }

  char title[24];
  snprintf(title, sizeof(title), "HOLDINGS %d/%d", page + 1, pages);
  head(f, title, kPresetKeys[v.preset], nullptr);
  putpRight(f, MK_X_RIGHT, MK_Y_HEAD + MK_PICO_DROP, v.mode == M_REBAL ? "REBAL" : "HOLD", C_AMBER, R_HEAD_RIGHT);
  if (info.error) { drawError(f, m, v); return; }

  for (int k = 0; k < MK_HD_ROWS; k++) {
    const int r = page * MK_HD_ROWS + k;
    if (r >= rows) break;
    const bool cash = r == rows - 1;
    const HoldRow *row = cash ? nullptr : &h.rows[order[r]];
    const char *sym = cash ? "CASH" : row->sym;
    const float tgt = cash ? h.cashTgt : row->tgt, now = cash ? h.cashNow : row->now;
    const int16_t y = (int16_t)(MK_Y_HD_ROW0 + k * MK_HD_ROW_H);
    char mid[16], ret[16];
    snprintf(mid, sizeof(mid), "%ld>%ld%%", (long)std::nearbyint(tgt), (long)std::nearbyint(now));
    // Out of band: this many points off the target, or this far off in
    // relative terms (the 5/25 rule), in amber when asked for.
    const float off = std::fabs(now - tgt);
    const bool out = v.bandMark && !cash && (off > v.bandAbs || (tgt > 0 && off / tgt * 100.0f > v.bandRel));
    put5(f, MK_X_LEFT, y, sym, cash ? C_DIM : C_WHITE, 1, false, R_HD_SYM);
    putpRight(f, MK_X_HD_MID_R, (int16_t)(y + MK_PICO_DROP), mid, out ? C_AMBER : C_DIM, R_HD_MID);
    if (!cash && row->hasRet) {
      fmtPct(ret, sizeof(ret), row->ret, true);
      put5Right(f, MK_X_RIGHT, y, ret, pctCol(row->ret, true, v.colors), 1, true, R_HD_RET);
    }
  }
  putp(f, MK_X_LEFT, MK_Y_FOOT, "TGT>NOW", C_DIM, R_LEGEND);
  asofSlot(f, MK_Y_FOOT, info);
}

void drawPage(Canvas &f, const Model &m, const View &v, PageInfo *out) {
  PageInfo info;
  memset(&info, 0, sizeof(info));
  info.asof = kNoDate;
  if (v.tapeOn) drawTape(f, m, v);
  switch (v.page) {
  case PG_TICKER:    pageTicker(f, m, v, info); break;
  case PG_PORTFOLIO: pagePortfolio(f, m, v, info); break;
  case PG_HOLDINGS:  pageHoldings(f, m, v, info); break;
  default:           pageMarkets(f, m, v, info); break;
  }
  if (out) *out = info;
}

uint16_t presetsAvailable(const Model &m) { return (uint16_t)(kStandardPresets | dataMask(m)); }

// The window row: WINDOW at the left, the presets on the knob at the right
// with the current one in white, on a black band over the footer with an
// amber rule. When the list is too wide the label goes first, then the
// presets farthest from the current one.
void drawStatusRow(Canvas &f, uint8_t preset, uint16_t mask, bool top) {
  // At the foot: the rule on the band's top row, the text two rows under it.
  // At the top: the text a row under the band's top, the rule on its last row.
  const int16_t y0 = top ? MK_Y_STATUS_TOP : MK_Y_STATUS;
  const int16_t ty = top ? (int16_t)(MK_Y_STATUS_TOP + 1) : MK_Y_FOOT;
  f.fill(0, y0, W, MK_STATUS_H, C_BLACK);
  f.hline(top ? (int16_t)(y0 + MK_STATUS_H - 1) : y0, 0, W - 1, C_AMBER);
  static const char kLabel[] = "WINDOW";
  uint8_t list[kPresets];
  int n = 0, cur = 0;
  for (uint8_t p = 0; p < kPresets; p++)
    if ((mask & (1u << p)) || p == preset) { if (p == preset) cur = n; list[n++] = p; }
  // The width of the presets lo..hi with their spaces.
  auto width = [&](int lo, int hi) {
    int16_t w = 0;
    for (int i = lo; i <= hi; i++) w = (int16_t)(w + picoAdv(kPresetKeys[list[i]]) + (i > lo ? picoAdv(" ") : 0));
    return w;
  };
  const int16_t roomAll = (int16_t)(MK_X_RIGHT - MK_X_LEFT + 1);
  const int16_t roomLabel = (int16_t)(roomAll - picoInk(kLabel) - 2 * MK_GAP);
  int lo = 0, hi = n - 1;
  bool label = width(lo, hi) <= roomLabel;
  if (!label && width(lo, hi) > roomAll) {
    // Too many even alone: the current one and its neighbours, right first.
    lo = hi = cur;
    for (bool grew = true; grew;) {
      grew = false;
      if (hi + 1 < n && width(lo, hi + 1) <= roomAll) { hi++; grew = true; }
      else if (lo > 0 && width(lo - 1, hi) <= roomAll) { lo--; grew = true; }
    }
  }
  if (label) putp(f, MK_X_LEFT, ty, kLabel, C_AMBER, R_STATUS);
  Seg segs[kPresets * 2];
  int ns = 0;
  for (int i = lo; i <= hi; i++) {
    if (i > lo) segs[ns++] = {" ", C_DIM};
    segs[ns++] = {kPresetKeys[list[i]], list[i] == preset ? C_WHITE : C_DIM};
  }
  putpSegsRight(f, MK_X_RIGHT, ty, segs, ns, R_STATUS);
}

}  // namespace market

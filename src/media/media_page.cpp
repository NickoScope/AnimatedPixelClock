// Media player: the 128 x 64 page and the knob inside it.
//
// Now playing (docs/17-media-player.md 6): the source tag, the player and the
// state icon with the clock; the artist; the title, scrolling when it is wider
// than the panel; the album dim; the progress, extrapolated from Home
// Assistant's anchor; the volume bar; a hint for the knob.
//
//   [MA] NickoScope32 Audio S3   > 21:39     [TUNE] RADIO FAVOURITES    3/16
//   Quinn XCII                                 80s80s Radio
//   Olive Tree                                > Classic Vinyl HD
//   Olive Tree                                  FUN Radio
//   0:56 ======------------------ 4:01          |   |   |   #   |   |
//   VOL ==========---------- 50                 ====================
//   CLICK: TUNE, VOLUME                        STARTS IN 0.8 S
//
// Other screens: OFFLINE (no broker, or the app's will says offline), WAITING
// (connected, nothing from Home Assistant yet), NO PLAYER (it follows none, or
// the one it follows is unavailable), and STALE - the last state, dimmed, the
// progress frozen, a red STALE tag and how long since the last update.
//
// The knob (docs/17 3.5): a click enters TUNE, the next VOLUME, the next leaves.
// In TUNE rotation walks Music Assistant's radio favourites and the one under
// the pointer starts once the knob has rested MEDIA_TUNE_REST_MS; without
// favourites rotation is next / previous track. In VOLUME each detent is
// MEDIA_VOL_STEP percent, coalesced in media_ha.cpp.
//
// Text: every string from Home Assistant is transliterated before it is drawn
// (media_model.h), because the fonts are ASCII. tools/media/render.py reads
// every number, colour and word below, so change them only here.

#include "media.h"

#if defined(MEDIAPLAYER_ENABLED)

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"
#include "../mqtt/mqtt_bus.h"
#include "../panel/panel.h"

using namespace media;

// ── layout ──────────────────────────────────────────────────────────────────
// Two faces, as on the rail board: Picopixel (5 px capitals, drawn from its
// baseline MP_ASCENT below the top of a capital) and the built-in 5x7 font for
// the title and the big words (positioned from the top, 6 px a character).
static const int16_t MP_ASCENT      = 4;
static const int16_t MP_BIG_ADV     = 6;
static const int16_t MP_X_LEFT      = 2;
static const int16_t MP_X_RIGHT     = 126;   // exclusive
static const int16_t MP_X_RIGHT_IN  = 122;   // exclusive, while the "entered" mark sits in the corner
static const int16_t MP_GAP         = 3;
static const int16_t MP_Y_HEAD      = 1;     // tag box on rows 0-6
static const int16_t MP_Y_ARTIST    = 10;
static const int16_t MP_Y_TITLE     = 17;    // big: rows 17-24
static const int16_t MP_Y_ALBUM     = 27;
static const int16_t MP_Y_PROGRESS  = 37;    // bar on rows 38-40
static const int16_t MP_Y_VOLUME    = 46;    // bar on rows 47-49
static const int16_t MP_Y_HINT      = 57;
static const int16_t MP_X_VOL_BAR   = 18;
static const int16_t MP_VOL_BAR_W   = 60;
static const int16_t MP_ICON_W      = 5;
static const int16_t MP_Y_BIG       = 20;    // screens
static const int16_t MP_Y_LINE1     = 32;
static const int16_t MP_Y_LINE2     = 40;
static const int16_t MP_X_TUNE_NAME = 10;    // tune view
static const int16_t MP_Y_TUNE_PREV = 11;
static const int16_t MP_Y_TUNE_CUR  = 19;
static const int16_t MP_Y_TUNE_NEXT = 30;
static const int16_t MP_Y_DIAL      = 41;    // ticks on rows 41-43, the pointer on 39-45
static const int16_t MP_Y_WAIT      = 49;
static const int16_t MP_X_DIAL0     = 4;
static const int16_t MP_X_DIAL1     = 123;
// The volume band, rows 16-43: from under the artist, so no row of the title
// (17-24) is left showing above it - the first preview had two.
static const int16_t MP_Y_OV0       = 16;
static const int16_t MP_Y_OV1       = 43;
static const int16_t MP_X_OV0       = 6;
static const int16_t MP_X_OV1       = 122;
static const int16_t MP_Y_OV_BAR    = 31;
static const int16_t MP_SCROLL_HOLD_MS = 1500;
static const int16_t MP_SCROLL_END_MS  = 1000;
static const int16_t MP_SCROLL_PX_S    = 24;
static const int16_t MP_NEW_TRACK_MS   = 3000;   // the title in amber after a change of track
static const int16_t MP_SENT_SHOW_MS   = 3000;
static const int16_t MP_TRACK_STEP_MS  = 350;    // next / previous: one command this often
static const int16_t MP_STALE_LEVEL    = 45;     // percent: the stale screen is the old one, dimmed

// ── colour ──────────────────────────────────────────────────────────────────
static const uint8_t MP_COL_WHITE[3]  = {255, 255, 255};
static const uint8_t MP_COL_ARTIST[3] = {120, 200, 255};
static const uint8_t MP_COL_DIM[3]    = {110, 116, 122};
static const uint8_t MP_COL_AMBER[3]  = {255, 150,   0};
static const uint8_t MP_COL_GREEN[3]  = { 48, 232,  64};
static const uint8_t MP_COL_RED[3]    = {255,  36,  24};
static const uint8_t MP_COL_MUSIC[3]  = {  0, 190, 220};
static const uint8_t MP_COL_RADIO[3]  = {255,  96,   0};
static const uint8_t MP_COL_TTS[3]    = {200,  60, 200};
static const uint8_t MP_COL_TRACK[3]  = { 40,  44,  48};   // the empty part of a bar

// ── words ───────────────────────────────────────────────────────────────────
static const char *const MP_W_OFFLINE  = "OFFLINE";
static const char *const MP_W_WAITING  = "WAITING";
static const char *const MP_W_NOPLAYER = "NO PLAYER";
static const char *const MP_W_NOMEM    = "NO MEMORY";
static const char *const MP_W_STALE    = "STALE";
static const char *const MP_W_LIVE     = "LIVE";
static const char *const MP_W_TUNE     = "TUNE";
static const char *const MP_W_MEDIA    = "MEDIA";
static const char *const MP_W_FAVS     = "RADIO FAVOURITES";
static const char *const MP_W_VOLUME   = "VOLUME";
static const char *const MP_W_VOL      = "VOL";
static const char *const MP_W_MUTED    = "MUTED";
static const char *const MP_W_IDLE     = "Nothing playing";
static const char *const MP_W_OFF      = "Player is off";
static const char *const MP_H_BROWSE   = "CLICK: TUNE, VOLUME";
static const char *const MP_H_TUNE     = "TURN: STATION  CLICK: VOLUME";
static const char *const MP_H_TRACK    = "TURN: TRACK  CLICK: VOLUME";
static const char *const MP_H_VOLUME   = "TURN: VOLUME  CLICK: DONE";
static const char *const MP_H_STARTS   = "STARTS IN";
static const char *const MP_H_SENT     = "SENT";
static const char *const MP_H_NOTSENT  = "NOT SENT";
static const char *const MP_H_NEXT     = "NEXT >>";
static const char *const MP_H_PREV     = "<< PREV";
static const char *const MP_H_NOUPDATE = "NO UPDATE FOR";
static const char *const MP_L_BROKER   = "MQTT BROKER";
static const char *const MP_L_APP      = "HOME ASSISTANT APP";
static const char *const MP_L_APP_OFF  = "SAYS IT IS OFFLINE";
static const char *const MP_L_FOR_HA   = "FOR HOME ASSISTANT";
static const char *const MP_L_NONE     = "HOME ASSISTANT FOLLOWS NONE";
static const char *const MP_L_CHOOSE   = "CHOOSE ONE IN THE PORTAL";
static const char *const MP_L_UNAVAIL  = "UNAVAILABLE";
static const char *const MP_L_DEV      = "DEV";
static const char *const MP_T_STATION  = "TURN: STATION";   // the toasts
static const char *const MP_T_TRACK    = "TURN: TRACK";
static const char *const MP_T_VOLUME   = "TURN: VOLUME";
static const char *const MP_T_PAGES    = "TURN: PAGES";

// ── the knob ────────────────────────────────────────────────────────────────
enum Mode : uint8_t { MODE_NONE = 0, MODE_TUNE, MODE_VOLUME };
static uint8_t  s_mode        = MODE_NONE;
static int16_t  s_tuneIdx     = 0;
static char     s_tuneId[kFavIdLen]   = "";   // the favourite under the pointer, by id: the list may change
static char     s_playedId[kFavIdLen] = "";   // the last one sent
static uint32_t s_tuneMovedMs = 0;            // 0 = nothing waiting to start
static uint32_t s_tuneSentMs  = 0;
static bool     s_tuneOk      = false;
static int8_t   s_trackSteps  = 0;            // next (+) / previous (-) still to send
static int8_t   s_trackDir    = 0;
static uint32_t s_trackSentMs = 0;
static uint32_t s_trackShownMs = 0;
static uint32_t s_overlayMs   = 0;
static bool     s_moving      = false;        // the last frame had something in motion

static int16_t findFav(const Favs &f, const char *id) {
  for (uint8_t i = 0; i < f.n; i++)
    if (!strcmp(f.f[i].id, id)) return i;
  return -1;
}

static void syncTuneIndex(const Favs &f) {
  if (!f.n) { s_tuneIdx = 0; return; }
  int16_t i = s_tuneId[0] ? findFav(f, s_tuneId) : -1;
  if (i < 0) {
    i = s_tuneIdx < f.n ? s_tuneIdx : 0;
    copyUtf8(s_tuneId, sizeof(s_tuneId), f.f[i].id);
  }
  s_tuneIdx = i;
}

static void commitTune() {
  if (!s_tuneMovedMs) return;
  s_tuneMovedMs = 0;
  if (!ready()) return;
  const Favs &f = favs();
  const int16_t i = findFav(f, s_tuneId);
  if (i < 0) return;                 // gone from the list meanwhile
  s_tuneOk = sendFav(f.f[i].id);
  s_tuneSentMs = millis() | 1;
  if (s_tuneOk) copyUtf8(s_playedId, sizeof(s_playedId), f.f[i].id);
}

bool mediaKnobClick(bool entered) {
  if (!entered || s_mode == MODE_NONE) {
    s_mode = MODE_TUNE;
    if (ready()) syncTuneIndex(favs());
    return true;
  }
  if (s_mode == MODE_TUNE) {
    commitTune();                    // a click is a decision: no waiting for the rest
    s_mode = MODE_VOLUME;
    s_overlayMs = millis() | 1;
    return true;
  }
  s_mode = MODE_NONE;
  s_overlayMs = 0;
  return false;
}

const char *mediaKnobHint() {
  switch (s_mode) {
  case MODE_TUNE:   return (ready() && favs().n) ? MP_T_STATION : MP_T_TRACK;
  case MODE_VOLUME: return MP_T_VOLUME;
  default:          return MP_T_PAGES;
  }
}

void mediaKnob(int8_t d) {
  if (!ready()) return;
  const uint32_t ms = millis();
  if (s_mode == MODE_VOLUME) {
    nudgeVolume(d * MEDIA_VOL_STEP);
    s_overlayMs = ms | 1;
    return;
  }
  s_mode = MODE_TUNE;
  const Favs &f = favs();
  if (f.n) {
    syncTuneIndex(f);
    s_tuneIdx = (int16_t)((s_tuneIdx + (d > 0 ? 1 : f.n - 1)) % f.n);
    copyUtf8(s_tuneId, sizeof(s_tuneId), f.f[s_tuneIdx].id);
    s_tuneMovedMs = ms | 1;
  } else {
    const int next = s_trackSteps + (d > 0 ? 1 : -1);
    if (next >= -5 && next <= 5) s_trackSteps = (int8_t)next;
    s_trackDir = d > 0 ? 1 : -1;
    s_trackShownMs = ms | 1;
  }
}

void media::pageTick() {
  const uint32_t ms = millis();
  // Left by a click, the 30 s timeout, the carousel or the portal: the mode
  // goes with it, and a station still waiting to start starts now.
  const bool here = panelEnteredPage() && panelPageKey(panelCurrentPage()) == PANEL_KEY_MEDIA;
  if (s_mode != MODE_NONE && !here) {
    if (s_mode == MODE_TUNE) commitTune();
    s_mode = MODE_NONE;
    s_overlayMs = 0;
  }
  if (s_tuneMovedMs && ms - s_tuneMovedMs >= (uint32_t)MEDIA_TUNE_REST_MS) commitTune();
  if (s_trackSteps && (!s_trackSentMs || ms - s_trackSentMs >= (uint32_t)MP_TRACK_STEP_MS)) {
    const bool fwd = s_trackSteps > 0;
    sendSimple(fwd ? "next" : "prev");
    s_trackDir = fwd ? 1 : -1;
    s_trackSteps = (int8_t)(s_trackSteps + (fwd ? -1 : 1));
    s_trackSentMs = s_trackShownMs = ms | 1;
  }
}

static bool overlayOn(uint32_t ms) { return s_overlayMs && ms - s_overlayMs < (uint32_t)MEDIA_OVERLAY_MS; }

uint8_t mediaRefreshHz() {
  if (s_moving || s_tuneMovedMs || s_trackSteps || overlayOn(millis())) return 20;
  return 5;   // the clock and the progress: whole seconds
}

// ── drawing ─────────────────────────────────────────────────────────────────
static uint8_t s_level = 100;

static uint16_t col(const uint8_t c[3]) {
  return display.color565((uint8_t)(c[0] * s_level / 100), (uint8_t)(c[1] * s_level / 100),
                          (uint8_t)(c[2] * s_level / 100));
}

static int16_t textW(const char *s) {
  if (!s || !*s) return 0;
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return (int16_t)bw;
}

static int16_t bigW(const char *s) {
  const size_t n = strlen(s);
  return n ? (int16_t)(n * MP_BIG_ADV - 1) : 0;
}

static void put(int16_t x, int16_t top, const char *s, uint16_t c) {
  display.setTextColor(c);
  display.setCursor(x, top + MP_ASCENT);
  display.print(s);
}

static void putRight(int16_t right, int16_t top, const char *s, uint16_t c) { put(right - textW(s), top, s, c); }
static void putCentre(int16_t top, const char *s, uint16_t c) { put((128 - textW(s)) / 2, top, s, c); }

static void putBig(int16_t x, int16_t top, const char *s, uint16_t c) {
  display.setFont(NULL);
  display.setTextColor(c);
  display.setCursor(x, top);
  display.print(s);
  display.setFont(&PicopixelFB);
}

// Cut at the right until it fits: a name cut short still reads, a title scrolls.
static void fitCut(char *s, int16_t room) {
  size_t n = strlen(s);
  while (n && textW(s) > room) s[--n] = '\0';
  while (n && s[n - 1] == ' ') s[--n] = '\0';
}

static uint32_t textKey(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

// Hold at the start, glide to the end, hold, jump back. Restarts whenever the
// text changes - which is how a new track starts its title from the left.
static uint32_t s_scrollKey = 0;
static uint32_t s_scrollAt  = 0;

static int16_t marquee(const char *text, int16_t w, int16_t room, uint32_t ms) {
  if (w <= room) return 0;
  const uint32_t key = textKey(text);
  if (key != s_scrollKey) { s_scrollKey = key; s_scrollAt = ms; }
  s_moving = true;
  const uint32_t travel = (uint32_t)(w - room);
  const uint32_t moveMs = travel * 1000UL / MP_SCROLL_PX_S;
  const uint32_t cycle  = MP_SCROLL_HOLD_MS + moveMs + MP_SCROLL_END_MS;
  const uint32_t t      = (ms - s_scrollAt) % cycle;
  if (t < (uint32_t)MP_SCROLL_HOLD_MS) return 0;
  if (t < MP_SCROLL_HOLD_MS + moveMs) return (int16_t)((t - MP_SCROLL_HOLD_MS) * MP_SCROLL_PX_S / 1000UL);
  return (int16_t)travel;
}

// A line in the big font from `left` to MP_X_RIGHT; the margins are cleared
// again after a scrolled line, so it never runs into the panel's edge columns.
static void drawBigLine(int16_t top, int16_t left, const char *text, uint16_t c, uint32_t ms) {
  const int16_t off = marquee(text, bigW(text), MP_X_RIGHT - left, ms);
  putBig(left - off, top, text, c);
  if (off) {
    display.fillRect(0, top, left, 8, 0);
    display.fillRect(MP_X_RIGHT, top, 128 - MP_X_RIGHT, 8, 0);
  }
}

// A filled tag with black text; returns the first column after it.
static int16_t drawTag(int16_t x, const char *text, uint16_t bg) {
  const int16_t w = textW(text) + 4;
  display.fillRect(x, MP_Y_HEAD - 1, w, 7, bg);
  put(x + 2, MP_Y_HEAD, text, 0);
  return x + w;
}

static void drawStateIcon(int16_t x, int16_t top, uint8_t st) {
  switch (st) {
  case ST_PLAYING: {
    const uint16_t c = col(MP_COL_GREEN);
    for (int16_t i = 0; i < 3; i++) display.drawFastVLine(x + 1 + i, top + i, 5 - 2 * i, c);
    break;
  }
  case ST_PAUSED: {
    const uint16_t c = col(MP_COL_AMBER);
    display.fillRect(x, top, 2, 5, c);
    display.fillRect(x + 3, top, 2, 5, c);
    break;
  }
  case ST_IDLE: display.drawRect(x, top, 5, 5, col(MP_COL_DIM)); break;
  case ST_OFF:  display.drawCircle(x + 2, top + 2, 2, col(MP_COL_DIM)); break;
  default: {
    const uint16_t c = col(MP_COL_RED);
    display.drawLine(x, top, x + 4, top + 4, c);
    display.drawLine(x, top + 4, x + 4, top, c);
    break;
  }
  }
}

// The right end of the header: the clock and the state icon, or STALE.
// Returns its left edge. st >= ST_COUNT draws the clock alone.
static int16_t drawHeadRight(time_t t, bool synced, uint8_t st, bool isStale) {
  const int16_t right = panelEnteredPage() ? MP_X_RIGHT_IN : MP_X_RIGHT;
  if (isStale) {
    const int16_t w = textW(MP_W_STALE) + 4;
    drawTag(right - w, MP_W_STALE, display.color565(MP_COL_RED[0], MP_COL_RED[1], MP_COL_RED[2]));   // full level
    return right - w;
  }
  char clk[6] = "--:--";
  if (synced) {
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(clk, sizeof(clk), "%02d:%02d", lt.tm_hour, lt.tm_min);
  }
  const int16_t cx = right - textW(clk);
  put(cx, MP_Y_HEAD, clk, col(MP_COL_DIM));
  if (st >= ST_COUNT) return cx;
  const int16_t ix = cx - MP_GAP - MP_ICON_W;
  drawStateIcon(ix, MP_Y_HEAD, st);
  return ix;
}

static void drawProgress(const NowPlaying &n, time_t t, bool synced, bool isStale) {
  const int16_t top = MP_Y_PROGRESS;
  if (n.kind == K_RADIO || !n.dur) {
    if (n.st != ST_PLAYING) return;   // a stream that is not playing has nothing to count
    const uint16_t red = col(MP_COL_RED);
    display.fillRect(MP_X_LEFT, top + 1, 3, 3, red);
    put(MP_X_LEFT + 5, top, MP_W_LIVE, red);
    return;
  }
  const uint32_t pos = (synced && !isStale) ? positionAt(n, (int64_t)t) : n.pos;
  char el[12], tot[12];
  formatClock(el, sizeof(el), pos);
  formatClock(tot, sizeof(tot), n.dur);
  put(MP_X_LEFT, top, el, col(MP_COL_DIM));
  putRight(MP_X_RIGHT, top, tot, col(MP_COL_DIM));
  const int16_t x0 = MP_X_LEFT + textW(el) + MP_GAP;
  const int16_t x1 = MP_X_RIGHT - textW(tot) - MP_GAP;
  if (x1 <= x0) return;
  display.fillRect(x0, top + 1, x1 - x0, 3, col(MP_COL_TRACK));
  const int16_t done = (int16_t)((uint64_t)(x1 - x0) * pos / n.dur);
  if (done > 0) display.fillRect(x0, top + 1, done, 3, col(n.st == ST_PLAYING ? MP_COL_GREEN : MP_COL_AMBER));
}

static void drawVolumeRow(const NowPlaying &n) {
  const int16_t top = MP_Y_VOLUME;
  const bool active = s_mode == MODE_VOLUME;
  put(MP_X_LEFT, top, MP_W_VOL, col(active ? MP_COL_AMBER : MP_COL_DIM));
  const int v = volumeShown();
  display.fillRect(MP_X_VOL_BAR, top + 1, MP_VOL_BAR_W, 3, col(MP_COL_TRACK));
  if (v > 0)
    display.fillRect(MP_X_VOL_BAR, top + 1, (int16_t)(MP_VOL_BAR_W * v / 100), 3,
                     col(n.muted ? MP_COL_DIM : (active ? MP_COL_AMBER : MP_COL_ARTIST)));
  char val[8];
  if (n.muted)    snprintf(val, sizeof(val), "%s", MP_W_MUTED);
  else if (v < 0) snprintf(val, sizeof(val), "--");
  else            snprintf(val, sizeof(val), "%d", v);
  const int16_t tx = MP_X_VOL_BAR + MP_VOL_BAR_W + MP_GAP;
  put(tx, top, val, col(n.muted ? MP_COL_RED : (active ? MP_COL_AMBER : MP_COL_DIM)));
  // The last command's failure, when there is room for it.
  if (n.err[0] && MP_X_RIGHT - textW(n.err) >= tx + textW(val) + MP_GAP) putRight(MP_X_RIGHT, top, n.err, col(MP_COL_RED));
}

static void drawHint(uint32_t ms, bool isStale) {
  const int16_t top = MP_Y_HINT;
  if (isStale) {
    char t[32];
    const long a = ageS();
    if (a < 0)         snprintf(t, sizeof(t), "%s A WHILE", MP_H_NOUPDATE);
    else if (a < 120)  snprintf(t, sizeof(t), "%s %ld S", MP_H_NOUPDATE, a);
    else if (a < 7200) snprintf(t, sizeof(t), "%s %ld MIN", MP_H_NOUPDATE, a / 60);
    else               snprintf(t, sizeof(t), "%s %ld H", MP_H_NOUPDATE, a / 3600);
    put(MP_X_LEFT, top, t, display.color565(MP_COL_RED[0], MP_COL_RED[1], MP_COL_RED[2]));   // full level
    return;
  }
  if (s_trackSteps || (s_trackShownMs && ms - s_trackShownMs < 1200)) {
    put(MP_X_LEFT, top, s_trackDir > 0 ? MP_H_NEXT : MP_H_PREV, col(MP_COL_AMBER));
    return;
  }
  const char *h = s_mode == MODE_VOLUME ? MP_H_VOLUME : (s_mode == MODE_TUNE ? (favs().n ? MP_H_TUNE : MP_H_TRACK) : MP_H_BROWSE);
  put(MP_X_LEFT, top, h, col(s_mode != MODE_NONE ? MP_COL_AMBER : MP_COL_DIM));
}

static void drawNowPlaying(time_t t, bool synced, uint32_t ms) {
  const NowPlaying &n = media::now();
  const bool isStale = media::stale();
  s_level = isStale ? MP_STALE_LEVEL : 100;
  char buf[kTitleLen];

  const uint8_t *tag = n.kind == K_RADIO ? MP_COL_RADIO : (n.kind == K_TTS ? MP_COL_TTS : MP_COL_MUSIC);
  const int16_t x = drawTag(0, n.src, col(tag)) + MP_GAP;
  const int16_t right = drawHeadRight(t, synced, n.st, isStale) - MP_GAP;
  translit(n.name, buf, sizeof(buf));
  fitCut(buf, right - x);
  put(x, MP_Y_HEAD, buf, col(MP_COL_DIM));

  translit(n.artist, buf, sizeof(buf));
  fitCut(buf, MP_X_RIGHT - MP_X_LEFT);
  put(MP_X_LEFT, MP_Y_ARTIST, buf, col(MP_COL_ARTIST));

  translit(n.title, buf, sizeof(buf));
  if (buf[0]) {
    const bool fresh = newTrackMs() && ms - newTrackMs() < (uint32_t)MP_NEW_TRACK_MS;
    drawBigLine(MP_Y_TITLE, MP_X_LEFT, buf, col(fresh ? MP_COL_AMBER : MP_COL_WHITE), ms);
  } else {
    putBig(MP_X_LEFT, MP_Y_TITLE, n.st == ST_OFF ? MP_W_OFF : MP_W_IDLE, col(MP_COL_DIM));
  }

  translit(n.album, buf, sizeof(buf));
  fitCut(buf, MP_X_RIGHT - MP_X_LEFT);
  put(MP_X_LEFT, MP_Y_ALBUM, buf, col(MP_COL_DIM));

  drawProgress(n, t, synced, isStale);
  drawVolumeRow(n);
  drawHint(ms, isStale);
}

static int16_t dialX(int16_t k, int16_t n) {
  if (n < 2) return (MP_X_DIAL0 + MP_X_DIAL1) / 2;
  return (int16_t)(MP_X_DIAL0 + (int32_t)k * (MP_X_DIAL1 - MP_X_DIAL0) / (n - 1));
}

static void drawTune(uint32_t ms) {
  const Favs &f = favs();
  s_level = 100;
  syncTuneIndex(f);
  const int16_t n = f.n, i = s_tuneIdx;
  char buf[kNameLen];

  const int16_t x = drawTag(0, MP_W_TUNE, col(MP_COL_RADIO)) + MP_GAP;
  char count[8];
  snprintf(count, sizeof(count), "%d/%d", i + 1, n);
  putRight(panelEnteredPage() ? MP_X_RIGHT_IN : MP_X_RIGHT, MP_Y_HEAD, count, col(MP_COL_DIM));
  put(x, MP_Y_HEAD, MP_W_FAVS, col(MP_COL_DIM));

  if (n > 1) {
    translit(f.f[(i + n - 1) % n].name, buf, sizeof(buf));
    fitCut(buf, MP_X_RIGHT - MP_X_TUNE_NAME);
    put(MP_X_TUNE_NAME, MP_Y_TUNE_PREV, buf, col(MP_COL_DIM));
  }
  translit(f.f[i].name, buf, sizeof(buf));
  drawBigLine(MP_Y_TUNE_CUR, MP_X_TUNE_NAME, buf, col(MP_COL_AMBER), ms);
  putBig(MP_X_LEFT, MP_Y_TUNE_CUR, ">", col(MP_COL_AMBER));
  if (n > 2) {
    translit(f.f[(i + 1) % n].name, buf, sizeof(buf));
    fitCut(buf, MP_X_RIGHT - MP_X_TUNE_NAME);
    put(MP_X_TUNE_NAME, MP_Y_TUNE_NEXT, buf, col(MP_COL_DIM));
  }

  // The dial: a tick per favourite, green for the last one sent, the pointer amber.
  const int16_t played = s_playedId[0] ? findFav(f, s_playedId) : -1;
  for (int16_t k = 0; k < n; k++)
    display.drawFastVLine(dialX(k, n), MP_Y_DIAL, 3, col(k == played ? MP_COL_GREEN : MP_COL_DIM));
  display.drawFastVLine(dialX(i, n), MP_Y_DIAL - 2, 7, col(MP_COL_AMBER));

  if (s_tuneMovedMs) {
    const uint32_t gone = ms - s_tuneMovedMs;
    const uint32_t left = gone >= (uint32_t)MEDIA_TUNE_REST_MS ? 0 : MEDIA_TUNE_REST_MS - gone;
    const int16_t w = (int16_t)((MP_X_DIAL1 - MP_X_DIAL0 + 1) * left / MEDIA_TUNE_REST_MS);
    if (w > 0) display.fillRect(MP_X_DIAL0, MP_Y_WAIT, w, 2, col(MP_COL_AMBER));
    char t[24];
    snprintf(t, sizeof(t), "%s %lu.%lu S", MP_H_STARTS, (unsigned long)(left / 1000), (unsigned long)(left / 100 % 10));
    put(MP_X_LEFT, MP_Y_HINT, t, col(MP_COL_AMBER));
    s_moving = true;
  } else if (s_tuneSentMs && ms - s_tuneSentMs < (uint32_t)MP_SENT_SHOW_MS) {
    put(MP_X_LEFT, MP_Y_HINT, s_tuneOk ? MP_H_SENT : MP_H_NOTSENT, col(s_tuneOk ? MP_COL_GREEN : MP_COL_RED));
  } else {
    put(MP_X_LEFT, MP_Y_HINT, MP_H_TUNE, col(MP_COL_DIM));
  }
}

static void drawScreen(const char *big, const char *line1, const char *line2, time_t t, bool synced) {
  s_level = 100;
  drawTag(0, MP_W_MEDIA, col(MP_COL_DIM));
  drawHeadRight(t, synced, ST_COUNT, false);
  putBig((128 - bigW(big)) / 2, MP_Y_BIG, big, col(MP_COL_WHITE));
  char buf[kNameLen + 16];
  const char *lines[2] = {line1, line2};
  const int16_t tops[2] = {MP_Y_LINE1, MP_Y_LINE2};
  for (uint8_t k = 0; k < 2; k++) {
    if (!lines[k] || !*lines[k]) continue;
    translit(lines[k], buf, sizeof(buf));
    fitCut(buf, MP_X_RIGHT - MP_X_LEFT);
    putCentre(tops[k], buf, col(MP_COL_DIM));
  }
  if (deviceId()[0]) {
    snprintf(buf, sizeof(buf), "%s %s", MP_L_DEV, deviceId());
    put(MP_X_LEFT, MP_Y_HINT, buf, col(MP_COL_DIM));
  }
}

static void drawVolumeOverlay() {
  s_level = 100;
  const int v = volumeShown();
  const bool muted = media::now().muted;
  const uint16_t amber = col(MP_COL_AMBER);
  display.fillRect(0, MP_Y_OV0, 128, MP_Y_OV1 - MP_Y_OV0 + 1, 0);
  display.drawFastHLine(0, MP_Y_OV0, 128, amber);
  display.drawFastHLine(0, MP_Y_OV1, 128, amber);
  put(MP_X_OV0, MP_Y_OV0 + 4, MP_W_VOLUME, amber);
  if (muted) put(MP_X_OV0 + textW(MP_W_VOLUME) + 6, MP_Y_OV0 + 4, MP_W_MUTED, col(MP_COL_RED));
  char t[6];
  if (v < 0) snprintf(t, sizeof(t), "--");
  else       snprintf(t, sizeof(t), "%d", v);
  putBig(MP_X_OV1 - bigW(t), MP_Y_OV0 + 3, t, amber);
  const int16_t w = MP_X_OV1 - MP_X_OV0;
  display.fillRect(MP_X_OV0, MP_Y_OV_BAR, w, 6, col(MP_COL_TRACK));
  if (v > 0) display.fillRect(MP_X_OV0, MP_Y_OV_BAR, (int16_t)(w * v / 100), 6, muted ? col(MP_COL_DIM) : amber);
}

void mediaRender() {
  const time_t   t      = time(nullptr);
  const bool     synced = t > 1700000000;   // before NTP the clock reads 1970
  const uint32_t ms     = millis();
  s_moving = false;
  display.setFont(&PicopixelFB);
  display.setTextSize(1);
  display.setTextWrap(false);

  if (!ready())                                drawScreen(MP_W_NOMEM, "", "", t, synced);
  else if (!mqttBusConnected())                drawScreen(MP_W_OFFLINE, mqttBusStatus(), MP_L_BROKER, t, synced);
  else if (haveBridge() && !bridgeOnline())    drawScreen(MP_W_OFFLINE, MP_L_APP, MP_L_APP_OFF, t, synced);
  else if (!media::now().have)                 drawScreen(MP_W_WAITING, MP_L_FOR_HA, "", t, synced);
  else if (s_mode == MODE_TUNE && favs().n)    drawTune(ms);
  else if (!media::now().player[0])            drawScreen(MP_W_NOPLAYER, MP_L_NONE, MP_L_CHOOSE, t, synced);
  else if (media::now().st == ST_UNAVAILABLE)  drawScreen(MP_W_NOPLAYER, media::now().name, MP_L_UNAVAIL, t, synced);
  else                                         drawNowPlaying(t, synced, ms);

  if (ready() && mqttBusConnected() && media::now().have && overlayOn(ms)) drawVolumeOverlay();
  s_level = 100;
  display.setFont(NULL);   // other pages draw with the built-in font and would inherit this one
}

#endif  // MEDIAPLAYER_ENABLED

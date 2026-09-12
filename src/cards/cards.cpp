#include "cards.h"

#if defined(CARDS_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "../display/display.h"
#include "../fonts/picopixel_fb.h"
#include "../mqtt/mqtt_bus.h"

#define CARD_TOPIC  MQTT_BASE "/card/"
#define NOTIFY_TOPIC MQTT_BASE "/notify"

struct Card {
  char     name[CARD_NAME_LEN];
  char     title[CARD_TITLE_LEN];
  char     text[CARD_TEXT_LEN];
  uint16_t colour;
  uint16_t barColour;
  int8_t   progress;          // -1 = none
  uint32_t lifetimeMs;        // 0 = forever
  uint32_t seen;
};

static Card    s_cards[CARD_MAX];
static uint8_t s_count = 0;

static char     s_nTitle[CARD_TITLE_LEN];
static char     s_nText[CARD_TEXT_LEN];
static uint16_t s_nColour = 0;
static uint32_t s_nUntil  = 0;     // 0 = no notification
static bool     s_nHold   = false;

// ---------------------------------------------------------------- helpers
static uint16_t parseColour(JsonVariantConst v, uint16_t fallback) {
  if (v.isNull()) return fallback;
  const char *s = v.as<const char *>();
  if (!s || *s != '#' || strlen(s) < 7) return fallback;
  const long n = strtol(s + 1, NULL, 16);
  return display.color565((n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF);
}

static void copyField(char *dst, size_t n, JsonVariantConst v) {
  const char *s = v.is<const char *>() ? v.as<const char *>() : "";
  strncpy(dst, s ? s : "", n - 1);
  dst[n - 1] = '\0';
}

static Card *slotFor(const char *name) {
  for (uint8_t i = 0; i < s_count; i++)
    if (!strcmp(s_cards[i].name, name)) return &s_cards[i];
  if (s_count >= CARD_MAX) return NULL;              // full: the newest is refused
  Card *c = &s_cards[s_count++];
  memset(c, 0, sizeof(*c));
  strncpy(c->name, name, CARD_NAME_LEN - 1);
  return c;
}

static void removeCard(const char *name) {
  for (uint8_t i = 0; i < s_count; i++) {
    if (!strcmp(s_cards[i].name, name)) {
      for (uint8_t j = i; j + 1 < s_count; j++) s_cards[j] = s_cards[j + 1];
      s_count--;
      return;
    }
  }
}

// ---------------------------------------------------------------- ingest
static void onCard(const char *topic, const uint8_t *payload, uint16_t len) {
  const char *name = topic + strlen(CARD_TOPIC);
  if (!*name) return;
  // An empty payload deletes. That is also what a retained topic cleared from
  // Home Assistant looks like, so the two agree by construction.
  if (len == 0) { removeCard(name); return; }

  JsonDocument doc;
  if (deserializeJson(doc, (const char *)payload, len)) return;

  Card *c = slotFor(name);
  if (!c) return;
  copyField(c->title, sizeof(c->title), doc["title"]);
  copyField(c->text,  sizeof(c->text),  doc["text"]);
  c->colour    = parseColour(doc["color"],     display.color565(255, 255, 255));
  c->barColour = parseColour(doc["progressC"], display.color565(0, 200, 255));
  c->progress  = doc["progress"].is<int>() ? (int8_t)constrain(doc["progress"].as<int>(), 0, 100) : -1;
  const uint32_t lt = doc["lifetime"] | 0U;
  c->lifetimeMs = lt * 1000UL;
  c->seen = millis();
}

static void onNotify(const char *topic, const uint8_t *payload, uint16_t len) {
  (void)topic;
  if (len == 0) { s_nUntil = 0; return; }
  JsonDocument doc;
  if (deserializeJson(doc, (const char *)payload, len)) return;
  copyField(s_nTitle, sizeof(s_nTitle), doc["title"]);
  copyField(s_nText,  sizeof(s_nText),  doc["text"]);
  s_nColour = parseColour(doc["color"], display.color565(255, 180, 0));
  s_nHold   = doc["hold"] | false;
  const uint32_t secs = doc["duration"] | 6U;
  s_nUntil  = millis() + secs * 1000UL;
}

void cardsBegin() {
  mqttBusOnMessage(NOTIFY_TOPIC, onNotify);   // more specific first
  mqttBusOnMessage(CARD_TOPIC,   onCard);
  mqttBusSubscribe(CARD_TOPIC "+");
  mqttBusSubscribe(NOTIFY_TOPIC);
}

void cardsLoop() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < s_count; ) {
    if (s_cards[i].lifetimeMs && (now - s_cards[i].seen) > s_cards[i].lifetimeMs)
      removeCard(s_cards[i].name);            // removeCard compacts, so do not advance
    else
      i++;
  }
  if (s_nUntil && !s_nHold && (int32_t)(now - s_nUntil) >= 0) s_nUntil = 0;
}

uint8_t     cardsCount()          { return s_count; }
const char *cardsName(uint8_t i)  { return i < s_count ? s_cards[i].name : ""; }
bool        cardsNotifyActive()   { return s_nUntil != 0; }
void        cardsNotifyDismiss()  { s_nUntil = 0; }

// ---------------------------------------------------------------- render
// Text is wrapped by measuring, not by counting characters: the font is
// proportional, so a character count is the wrong unit and W is three times M.
static void wrapText(const char *s, uint16_t maxW, char *l1, char *l2, size_t n) {
  int16_t bx, by; uint16_t bw, bh;
  l1[0] = l2[0] = '\0';
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  if (bw <= maxW) { strncpy(l1, s, n - 1); l1[n - 1] = '\0'; return; }
  // Break at the last space that still fits.
  size_t cut = 0;
  char buf[CARD_TEXT_LEN];
  for (size_t i = 0; i < strlen(s) && i < n - 1; i++) {
    if (s[i] != ' ') continue;
    strncpy(buf, s, i); buf[i] = '\0';
    display.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    if (bw <= maxW) cut = i; else break;
  }
  if (!cut) { strncpy(l1, s, n - 1); l1[n - 1] = '\0'; return; }  // one long word
  strncpy(l1, s, cut); l1[cut] = '\0';
  strncpy(l2, s + cut + 1, n - 1); l2[n - 1] = '\0';
}

// Layout, in TOP-of-line coordinates. GFX positions a custom font from the
// BASELINE, so every cursor here adds the ascent - the same convention the
// flight board uses, and the reason the host renderer can mirror these numbers
// directly. Getting this wrong drew the rule straight through the title.
static const int16_t CARD_ASCENT  = 4;
static const int16_t CARD_Y_TITLE = 6;
static const int16_t CARD_Y_RULE  = 14;
static const int16_t CARD_Y_TEXT  = 28;    // single line
static const int16_t CARD_Y_TEXT1 = 24;    // two lines
static const int16_t CARD_Y_TEXT2 = 32;
static const int16_t CARD_Y_BAR   = 52;
static const int16_t CARD_W_TEXT  = 118;   // room the text may use

static void textTop(int16_t x, int16_t top, const char *s) {
  display.setCursor(x, top + CARD_ASCENT);
  display.print(s);
}

static int16_t centred(const char *s) {
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return (128 - (int16_t)bw) / 2;
}

static void drawBody(const char *title, const char *text, uint16_t colour,
                     int8_t progress, uint16_t barColour) {
  display.setFont(&PicopixelFB);
  display.setTextWrap(false);
  display.setTextSize(1);                 // sticky: the animated clocks leave it at 3

  if (title[0]) {
    display.setTextColor(display.color565(120, 132, 138));
    textTop(centred(title), CARD_Y_TITLE, title);
    display.drawFastHLine(8, CARD_Y_RULE, 112, display.color565(40, 48, 54));
  }

  char l1[CARD_TEXT_LEN], l2[CARD_TEXT_LEN];
  wrapText(text, CARD_W_TEXT, l1, l2, sizeof(l1));
  display.setTextColor(colour);
  if (l2[0]) {
    textTop(centred(l1), CARD_Y_TEXT1, l1);
    textTop(centred(l2), CARD_Y_TEXT2, l2);
  } else {
    textTop(centred(l1), CARD_Y_TEXT, l1);
  }

  if (progress >= 0) {
    const int16_t x = 10, w = 108, h = 6;
    display.drawRect(x, CARD_Y_BAR, w, h, display.color565(40, 48, 54));
    const int16_t fill = (int16_t)((w - 2) * progress / 100);
    if (fill > 0) display.fillRect(x + 1, CARD_Y_BAR + 1, fill, h - 2, barColour);
  }
  display.setFont(NULL);
}

void cardsRender(uint8_t i) {
  display.fillScreen(0);
  if (i >= s_count) return;
  const Card &c = s_cards[i];
  drawBody(c.title, c.text, c.colour, c.progress, c.barColour);
}

void cardsNotifyRender() {
  if (!s_nUntil) return;
  // A panel is a wall object seen from across a room: a notification has to
  // take the screen, not share it. Everything under it goes.
  display.fillScreen(0);
  display.drawRect(0, 0, 128, 64, s_nColour);
  drawBody(s_nTitle, s_nText, s_nColour, -1, 0);
  if (s_nHold) {                        // say that it is waiting for a press
    display.setFont(&PicopixelFB);
    display.setTextSize(1);
    display.setTextColor(display.color565(90, 100, 110));
    textTop(centred("PRESS"), 56, "PRESS");
    display.setFont(NULL);
  }
}

#endif  // CARDS_ENABLED

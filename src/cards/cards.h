#pragma once
// Cards: pages Home Assistant invents, without a firmware change.
//
// The idea is AWTRIX's and it is the right one: a page carries no logic, it
// renders what someone else worked out. Publishing to a name that does not
// exist creates a page; publishing an empty payload removes it. So a card for
// the laundry, the bin day or the electricity price costs a Home Assistant
// automation and nothing here.
//
//   MQTT_BASE/card/<name>   retained JSON, empty payload deletes
//   MQTT_BASE/notify        a temporary overlay over whatever is showing
//
// Payload, all fields optional:
//   title     small line at the top
//   text      the thing you came to read
//   color     "#RRGGBB"
//   progress  0..100, draws a bar
//   progressC "#RRGGBB" for the bar
//   icon      name of an icon in the store, drawn to the left of the text
//   duration  seconds this card holds the screen when the panel is cycling
//   lifetime  seconds; the card removes itself if not updated in time
//   hold      notify only: stay until dismissed rather than timing out
//   duration  notify only: seconds on screen, default 6
//
// A card that stops being updated should not keep lying, which is what
// `lifetime` is for: our flight board shows its age, but a page whose source
// died stays on screen for ever.

#include <stdint.h>

#if defined(CARDS_ENABLED) && !defined(MQTT_BUS_ENABLED)
#error "CARDS_ENABLED needs MQTT_BUS_ENABLED: the connection lives in src/mqtt/mqtt_bus"
#endif

#if defined(CARDS_ENABLED)

#define CARD_MAX       6
#define CARD_NAME_LEN  14
#define CARD_TITLE_LEN 22
#define CARD_TEXT_LEN  64
#define CARD_ICON_LEN  18

void     cardsBegin();          // registers the MQTT handlers and subscribes
void     cardsLoop();           // expiry, and the notification timer

uint8_t  cardsCount();          // live cards, in the order they arrived
void     cardsRender(uint8_t i);
const char *cardsName(uint8_t i);
uint16_t    cardsDuration(uint8_t i);   // seconds for the carousel, 0 = default

bool     cardsNotifyActive();   // an overlay wants the screen
void     cardsNotifyRender();   // draw it over whatever the page drew
void     cardsNotifyDismiss();  // the knob was pressed

#endif  // CARDS_ENABLED

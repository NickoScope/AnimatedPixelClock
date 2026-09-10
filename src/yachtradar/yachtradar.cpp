#include "yachtradar.h"

#if defined(YACHTRADAR_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <math.h>
#include <string.h>

#include "../display/display.h"
#include "basemap.h"
#include "../fonts/picopixel_fb.h"

// ---------------------------------------------------------------- geometry
// Centre and scale are fx34's, unchanged - the coastline was clipped to them,
// so moving either would leave the shoreline in the wrong place.
static const float YR_CENTRE_LAT = 43.5030f;
static const float YR_CENTRE_LON = 7.0050f;
static const float YR_DAC_PER_KM = 300.0f;
static const float YR_R_DAC      = 1900.0f;   // outer ring = 6.33 km

// Subscription box, sized to the outer ring plus a few percent (fx34's values).
static const float YR_BBOX_LAT_HALF = 0.060f;
static const float YR_BBOX_LON_HALF = 0.082f;

// Panel layout. Left half is the plot, right half the table.
static const int16_t YR_CX      = 31;
static const int16_t YR_CY      = 32;
static const int16_t YR_R_PX    = 31;
static const int16_t YR_X_TABLE = 66;
static const int16_t YR_X_RIGHT = 126;
static const int16_t YR_GAP     = 3;
static const int16_t YR_Y_TITLE = 1;
static const int16_t YR_Y_SUB   = 7;
static const int16_t YR_Y_RULE  = 15;
static const int16_t YR_Y_ROW0  = 17;
static const int16_t YR_ROW_H   = 7;

// A vessel drops off the plot after this long without a position report.
// fx34 uses five minutes: under ITU-R M.1371 an anchored vessel reports about
// every three, so this is under two periods and a lost packet can blink a
// stationary boat. Kept deliberately - a ghost of a departed vessel is worse.
static const uint32_t YR_TTL_MS = 5UL * 60UL * 1000UL;

// AIS ship types we care about: 36 sailing, 37 pleasure craft.
static const uint16_t YR_MIN_LENGTH_M = 24;

struct YrVessel {
  uint32_t mmsi;
  float    lat, lon;
  float    sog;
  uint16_t length_m;
  uint8_t  ship_type;
  char     name[YR_NAME_LEN];
  uint32_t last_seen;
  uint32_t first_seen;
};

static YrVessel     s_v[YR_MAX_VESSELS];
static uint8_t      s_count    = 0;
static uint32_t     s_lastPos  = 0;
// Vessels admitted to the table since boot. A boat that leaves the bay and
// returns is counted twice - this is "arrivals seen", not "distinct hulls".
static uint32_t     s_logged   = 0;
static bool         s_open     = false;
static bool         s_conn     = false;
static bool         s_subbed   = false;
static bool         s_noKey    = false;
static String       s_key;
static WebSocketsClient s_ws;

// ---------------------------------------------------------------- vessels
static YrVessel *find(uint32_t mmsi) {
  for (uint8_t i = 0; i < s_count; i++)
    if (s_v[i].mmsi == mmsi) return &s_v[i];
  return NULL;
}

static float distDac(const YrVessel &v) {
  const float dx = (v.lon - YR_CENTRE_LON) * 111.320f *
                   cosf(YR_CENTRE_LAT * (float)DEG_TO_RAD) * YR_DAC_PER_KM;
  const float dy = (v.lat - YR_CENTRE_LAT) * 110.574f * YR_DAC_PER_KM;
  return sqrtf(dx * dx + dy * dy);
}

// Evict the vessel furthest from the centre once the table is full: the plot
// is about what is in the bay, and the edge is where "in the bay" runs out.
static YrVessel *slotFor(uint32_t mmsi) {
  YrVessel *v = find(mmsi);
  if (v) return v;
  if (s_count < YR_MAX_VESSELS) {
    v = &s_v[s_count++];
    memset(v, 0, sizeof(*v));
    v->mmsi = mmsi;
    v->first_seen = millis();
    s_logged++;
    return v;
  }
  uint8_t worst = 0; float worstD = -1.0f;
  for (uint8_t i = 0; i < s_count; i++) {
    const float d = distDac(s_v[i]);
    if (d > worstD) { worstD = d; worst = i; }
  }
  v = &s_v[worst];
  memset(v, 0, sizeof(*v));
  v->mmsi = mmsi;
  v->first_seen = millis();
  s_logged++;
  return v;
}

static void expire() {
  const uint32_t now = millis();
  uint8_t w = 0;
  for (uint8_t i = 0; i < s_count; i++)
    if (now - s_v[i].last_seen < YR_TTL_MS) s_v[w++] = s_v[i];
  s_count = w;
}

static YrMotion motionOf(const YrVessel &v) {
  if (v.sog < 0.5f) return YR_ANCHORED;
  if (v.sog < 3.0f) return YR_MANOEUVRE;
  return YR_UNDERWAY;
}

// ---------------------------------------------------------------- AIS feed
// A vessel is plotted as soon as position and name are known. ShipStaticData
// is event-driven and rare - roughly every six minutes for a vessel under way
// - and AISstream puts the name in MetaData on every message, so waiting for
// static data means an empty screen over a bay full of boats. Type and length
// refine the entry when they arrive; they do not gate it. This is the same
// decision the NickoScope32 bridge reached after measuring it live.
static void onMessage(const char *payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;

  const char *type = doc["MessageType"] | "";
  JsonObjectConst meta = doc["MetaData"];
  const uint32_t mmsi = meta["MMSI"] | 0U;
  if (!mmsi) return;

  if (!strcmp(type, "PositionReport")) {
    JsonObjectConst pr = doc["Message"]["PositionReport"];
    if (pr["Latitude"].isNull() || pr["Longitude"].isNull()) return;
    YrVessel *v = slotFor(mmsi);
    v->lat = pr["Latitude"].as<float>();
    v->lon = pr["Longitude"].as<float>();
    v->sog = pr["Sog"] | 0.0f;
    const char *nm = meta["ShipName"] | "";
    if (nm[0]) {
      strncpy(v->name, nm, sizeof(v->name) - 1);
      v->name[sizeof(v->name) - 1] = '\0';
      // AISstream pads names with trailing spaces out of the AIS frame.
      for (int i = (int)strlen(v->name) - 1; i >= 0 && v->name[i] == ' '; i--)
        v->name[i] = '\0';
    }
    v->last_seen = millis();
    s_lastPos = v->last_seen;
  } else if (!strcmp(type, "ShipStaticData")) {
    JsonObjectConst sd = doc["Message"]["ShipStaticData"];
    YrVessel *v = find(mmsi);
    if (!v) return;
    JsonObjectConst dim = sd["Dimension"];
    const uint16_t l = (uint16_t)((dim["A"] | 0) + (dim["B"] | 0));
    if (l) v->length_m = l;
    const uint8_t t = sd["Type"] | 0;
    if (t) v->ship_type = t;
  }
}

static void subscribe() {
  // One message, built by hand: ArduinoJson would cost a document for a shape
  // that never varies.
  String m = F("{\"APIKey\":\"");
  m += s_key;
  m += F("\",\"BoundingBoxes\":[[[");
  m += String(YR_CENTRE_LAT - YR_BBOX_LAT_HALF, 4); m += ',';
  m += String(YR_CENTRE_LON - YR_BBOX_LON_HALF, 4); m += F("],[");
  m += String(YR_CENTRE_LAT + YR_BBOX_LAT_HALF, 4); m += ',';
  m += String(YR_CENTRE_LON + YR_BBOX_LON_HALF, 4); m += F("]]],");
  m += F("\"FilterMessageTypes\":[\"PositionReport\",\"ShipStaticData\"]}");
  s_ws.sendTXT(m);
  s_subbed = true;
}

static void onEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
  case WStype_CONNECTED:
    s_conn = true; s_subbed = false;
    subscribe();
    break;
  case WStype_DISCONNECTED:
    s_conn = false; s_subbed = false;
    break;
  case WStype_TEXT:
    if (!s_subbed) subscribe();
    onMessage((const char *)payload, len);
    break;
  default:
    break;
  }
}

bool yachtRadarBegin() {
  if (s_open) return !s_noKey;
  Preferences p;
  if (p.begin("yr", true)) {          // read-only
    s_key = p.getString("ais", "");
    p.end();
  }
  if (s_key.isEmpty()) { s_noKey = true; return false; }
  s_noKey = false;
  // Empty fingerprint makes the library call setInsecure() internally, so no
  // CA bundle is carried. Same call the NickoScope32 S3 has been running on.
  s_ws.beginSSL("stream.aisstream.io", 443, "/v0/stream");
  s_ws.onEvent(onEvent);
  s_ws.setReconnectInterval(5000);
  s_ws.enableHeartbeat(15000, 3000, 2);
  s_open = true;
  return true;
}

void yachtRadarLoop() {
  if (s_open) s_ws.loop();
}

void yachtRadarStop() {
  if (!s_open) return;
  s_ws.disconnect();
  s_open = s_conn = s_subbed = false;
}


// ---------------------------------------------------------------- render
static uint16_t colourFor(const YrVessel &v, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t rr, gg, bb;
  if (v.length_m >= 60)      { rr = 255; gg = 190; bb =  40; }   // mega, amber
  else switch (motionOf(v)) {
  case YR_ANCHORED:          { rr =  60; gg = 255; bb =  90; } break;
  case YR_MANOEUVRE:         { rr =   0; gg = 255; bb = 210; } break;
  default:                   { rr = 255; gg = 120; bb = 255; } break;
  }
  if (r) *r = rr; if (g) *g = gg; if (b) *b = bb;
  return display.color565(rr, gg, bb);
}

// Blend against the baked map rather than over it. The basemap is in flash, so
// the pixel underneath is known exactly - which is what lets a vessel carry a
// sub-pixel halo instead of snapping to the pixel grid. On a 64 px chart that
// halo is the only resolution left to spend.
static void blendOverMap(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (a == 0 || x < 0 || x >= YR_MAP_W || y < 0 || y >= YR_MAP_H) return;
  const uint16_t m = kYrBasemap[y * YR_MAP_W + x];
  const uint8_t mr = (uint8_t)((m >> 8) & 0xF8);
  const uint8_t mg = (uint8_t)((m >> 3) & 0xFC);
  const uint8_t mb = (uint8_t)((m << 3) & 0xF8);
  const uint16_t ia = (uint16_t)(255 - a);
  display.drawPixelRGB888(x, y,
      (uint8_t)((mr * ia + r * a) / 255),
      (uint8_t)((mg * ia + g * a) / 255),
      (uint8_t)((mb * ia + b * a) / 255));
}

// Wu-style 2x2 split of a fractional position, in 1/256 units.
static void softDot(float fx, float fy, uint8_t r, uint8_t g, uint8_t b, float alpha) {
  const int16_t x0 = (int16_t)floorf(fx), y0 = (int16_t)floorf(fy);
  const float ax = fx - x0, ay = fy - y0;
  blendOverMap(x0,     y0,     r, g, b, (uint8_t)(255.0f * alpha * (1 - ax) * (1 - ay)));
  blendOverMap(x0 + 1, y0,     r, g, b, (uint8_t)(255.0f * alpha *      ax  * (1 - ay)));
  blendOverMap(x0,     y0 + 1, r, g, b, (uint8_t)(255.0f * alpha * (1 - ax) *      ay));
  blendOverMap(x0 + 1, y0 + 1, r, g, b, (uint8_t)(255.0f * alpha *      ax  *      ay));
}

// lat/lon -> panel pixel, via the DAC space the map was baked in.
static void project(const YrVessel &v, float *px, float *py) {
  const float dx = (v.lon - YR_CENTRE_LON) * 111.320f *
                   cosf(YR_CENTRE_LAT * (float)DEG_TO_RAD) * YR_DAC_PER_KM;
  const float dy = (v.lat - YR_CENTRE_LAT) * 110.574f * YR_DAC_PER_KM;
  *px = YR_CX + dx * YR_R_PX / YR_R_DAC;
  *py = YR_CY - dy * YR_R_PX / YR_R_DAC;     // row 0 is the top
}

// Order for the table: nearest first. The chart already shows where everything
// is, so the list earns its place by answering "what is closest to me".
static void sortByRange(uint8_t *idx, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {          // insertion sort, n <= 16
    const uint8_t k = idx[i];
    const float   d = distDac(s_v[k]);
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && distDac(s_v[idx[j]]) > d) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = k;
  }
}

void yachtRadarRender() {
  display.fillScreen(0);
  display.setFont(&PicopixelFB);
  display.setTextWrap(false);
  int16_t bx, by; uint16_t bw, bh;

  const uint16_t white = display.color565(255, 255, 255);
  const uint16_t dim   = display.color565(110, 122, 128);

  // --- left: the chart ---
  // Blitted, not drawn: sea shaded by measured depth, land by measured
  // elevation with a hillshade, shoreline on top. All of it settled at build
  // time by tools/yr_basemap_gen.py, so the panel spends nothing on it.
  for (int16_t y = 0; y < YR_MAP_H; y++)
    for (int16_t x = 0; x < YR_MAP_W; x++)
      display.drawPixel(x, y, kYrBasemap[y * YR_MAP_W + x]);

  display.setTextColor(display.color565(200, 230, 255));
  display.setCursor(YR_CX - 1, 0 + 4);      display.print('N');
  display.setCursor(YR_CX - 1, 57 + 4);     display.print('S');
  display.setCursor(1, YR_CY - 3 + 4);      display.print('W');
  display.setCursor(57, YR_CY - 3 + 4);     display.print('E');

  expire();
  uint8_t idx[YR_MAX_VESSELS];
  for (uint8_t i = 0; i < s_count; i++) idx[i] = i;
  sortByRange(idx, s_count);

  for (uint8_t i = 0; i < s_count; i++) {
    const YrVessel &v = s_v[idx[i]];
    float fx, fy; project(v, &fx, &fy);
    const int16_t ix = (int16_t)lroundf(fx), iy = (int16_t)lroundf(fy);
    if (ix < 0 || ix >= YR_MAP_W || iy < 0 || iy >= YR_MAP_H) continue;
    uint8_t r, g, b; colourFor(v, &r, &g, &b);

    // A dark ring first: over a coloured chart a bright dot alone reads as part
    // of the map, and the vessels are the point of the page.
    for (int8_t dx = -1; dx <= 1; dx++)
      for (int8_t dy = -1; dy <= 1; dy++)
        if (dx || dy) blendOverMap(ix + dx, iy + dy, 0, 0, 0, 90);

    softDot(fx, fy, r, g, b, 0.5f);                  // sub-pixel halo
    blendOverMap(ix, iy, r, g, b, 255);              // core, full brightness
    if (v.length_m >= 60) {                          // mega: size at a glance
      blendOverMap(ix - 1, iy, r, g, b, 190);
      blendOverMap(ix + 1, iy, r, g, b, 190);
      blendOverMap(ix, iy - 1, r, g, b, 190);
      blendOverMap(ix, iy + 1, r, g, b, 190);
    }
  }

  // --- right: the table ---
  display.setTextColor(white);
  display.setCursor(YR_X_TABLE, YR_Y_TITLE + 4);
  display.print(F("BAY OF CANNES"));

  char sub[24];
  if (s_noKey)            strncpy(sub, "NO AIS KEY", sizeof(sub));
  else if (!s_conn)       strncpy(sub, "CONNECTING", sizeof(sub));
  else if (s_count == 0)  strncpy(sub, "SCANNING", sizeof(sub));
  else snprintf(sub, sizeof(sub), "%u NOW  %lu SEEN",
                (unsigned)s_count, (unsigned long)s_logged);
  sub[sizeof(sub) - 1] = '\0';
  display.setTextColor(dim);
  display.setCursor(YR_X_TABLE, YR_Y_SUB + 4);
  display.print(sub);
  display.drawFastHLine(YR_X_TABLE - 1, YR_Y_RULE, YR_X_RIGHT - YR_X_TABLE + 1,
                        display.color565(40, 48, 54));

  const uint8_t rows = s_count < YR_TABLE_ROWS ? s_count : YR_TABLE_ROWS;
  for (uint8_t i = 0; i < rows; i++) {
    const YrVessel &v = s_v[idx[i]];
    const int16_t base = YR_Y_ROW0 + i * YR_ROW_H + 4;
    const uint16_t c = colourFor(v, NULL, NULL, NULL);
    display.setTextColor(c);

    // Length is the one number worth the width - it is what separates a
    // tender from a superyacht - so it is drawn first and the name takes
    // whatever is left, trimmed the way the flight board trims a city.
    char len[6] = "";
    uint16_t lenW = 0;
    if (v.length_m) {
      snprintf(len, sizeof(len), "%uM", (unsigned)v.length_m);
      display.getTextBounds(len, 0, 0, &bx, &by, &bw, &bh);
      lenW = bw;
      display.setCursor(YR_X_RIGHT - (int16_t)bw, base);
      display.print(len);
    }

    char nm[YR_NAME_LEN];
    if (v.name[0]) { strncpy(nm, v.name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0'; }
    else snprintf(nm, sizeof(nm), "%06lu", (unsigned long)(v.mmsi % 1000000UL));
    const int16_t room = (YR_X_RIGHT - (int16_t)lenW - YR_GAP) - YR_X_TABLE;
    for (;;) {
      if (nm[0] == '\0') break;
      display.getTextBounds(nm, 0, 0, &bx, &by, &bw, &bh);
      if ((int16_t)bw <= room) break;
      char *sp = strrchr(nm, ' ');
      if (sp == NULL) { nm[strlen(nm) - 1] = '\0'; continue; }  // one word: shave
      *sp = '\0';
    }
    display.setCursor(YR_X_TABLE, base);
    display.print(nm);
  }

  display.setFont(NULL);   // other pages draw with the built-in font
}

bool     yachtRadarHasData() { return s_count > 0; }
uint8_t  yachtRadarCount()   { return s_count; }
uint32_t yachtRadarAge()     { return s_lastPos ? millis() - s_lastPos : 0xFFFFFFFFUL; }

#endif  // YACHTRADAR_ENABLED

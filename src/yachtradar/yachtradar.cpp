#include "yachtradar.h"

#if defined(YACHTRADAR_ENABLED)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
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

// AIS ship types: 36 sailing, 37 pleasure craft. Not an admission filter - type
// and length arrive with ShipStaticData, minutes after the first position, so
// filtering on them at the door is the empty-screen bug all over again. They
// decide who gets evicted when the table is full instead.
static const uint8_t  YR_TYPE_SAIL     = 36;
static const uint8_t  YR_TYPE_PLEASURE = 37;
static const uint16_t YR_MIN_LENGTH_M  = 24;

struct YrVessel {
  uint32_t mmsi;
  float    lat, lon;
  float    sog;
  uint16_t length_m;
  uint8_t  ship_type;
  char     name[YR_NAME_LEN];
  uint32_t last_seen;
  uint32_t first_seen;
  uint32_t last_ping;          // millis() when the sweep last crossed it, 0 = never
};

static YrVessel     s_v[YR_MAX_VESSELS];
static uint8_t      s_count    = 0;
static uint32_t     s_lastPos  = 0;
// Vessels admitted to the table since boot. A boat that leaves the bay and
// returns is counted twice - this is "arrivals seen", not "distinct hulls".
static uint32_t     s_logged   = 0;
// What the stream actually delivers, so an empty screen can be told apart:
// no frames at all, frames but nothing in the box, or an error from the server.
static uint32_t     s_frames   = 0;
static uint32_t     s_positions = 0;
static uint32_t     s_lastFrame = 0;
static char         s_err[64]  = "";
static bool         s_open     = false;
static bool         s_conn     = false;
static bool         s_subbed   = false;
static bool         s_noKey    = false;
static uint8_t      s_scroll   = 0;      // first table row shown
// By length, longest first: the owner's order for the list (2026-09-14). The
// knob and the portal can still switch to range.
static bool         s_bySize   = true;    // false = by range
static String       s_key;
static WebSocketsClient s_ws;

// ---------------------------------------------------------------- vessels
static YrVessel *find(uint32_t mmsi) {
  for (uint8_t i = 0; i < s_count; i++)
    if (s_v[i].mmsi == mmsi) return &s_v[i];
  return NULL;
}

// Static data - length and type - comes about every six minutes per vessel
// (see onMessage), and often for a hull that has no slot yet: before its first
// position in the box, or while the table is full. Dropping it then leaves the
// row without a length for another six minutes, which is what the owner saw.
// The last sizes seen are kept here and handed over when the vessel is plotted.
struct YrStatic { uint32_t mmsi; uint16_t length_m; uint8_t ship_type; };
static const uint8_t YR_STATIC_CACHE = 32;
static YrStatic s_static[YR_STATIC_CACHE];
static uint8_t  s_staticNext = 0;

static void rememberStatic(uint32_t mmsi, uint16_t len, uint8_t type) {
  if (!len && !type) return;
  for (YrStatic &s : s_static)
    if (s.mmsi == mmsi) {
      if (len)  s.length_m  = len;
      if (type) s.ship_type = type;
      return;
    }
  s_static[s_staticNext] = {mmsi, len, type};          // oldest entry goes
  s_staticNext = (uint8_t)((s_staticNext + 1) % YR_STATIC_CACHE);
}

static void recallStatic(YrVessel *v) {
  if (v->length_m && v->ship_type) return;
  for (const YrStatic &s : s_static)
    if (s.mmsi == v->mmsi) {
      if (!v->length_m)  v->length_m  = s.length_m;
      if (!v->ship_type) v->ship_type = s.ship_type;
      return;
    }
}

static float distDac(const YrVessel &v) {
  const float dx = (v.lon - YR_CENTRE_LON) * 111.320f *
                   cosf(YR_CENTRE_LAT * (float)DEG_TO_RAD) * YR_DAC_PER_KM;
  const float dy = (v.lat - YR_CENTRE_LAT) * 110.574f * YR_DAC_PER_KM;
  return sqrtf(dx * dx + dy * dy);
}

// Is this the kind of vessel the page exists for? Unknown counts as yes: a
// vessel that has not sent static data yet must not lose its slot for it.
static bool looksLikeYacht(const YrVessel &v) {
  if (v.ship_type == YR_TYPE_SAIL || v.ship_type == YR_TYPE_PLEASURE) return true;
  if (v.ship_type == 0) return true;                 // no static data yet
  return v.length_m >= YR_MIN_LENGTH_M;              // big enough to be one anyway
}

// Evict the vessel furthest from the centre once the table is full - but a
// ferry crossing the middle of the bay must not push out a yacht moored at the
// edge, which is what pure distance would do. Non-yachts go first.
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
    const float d = distDac(s_v[i]) + (looksLikeYacht(s_v[i]) ? 0.0f : 100000.0f);
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

  // A reply carrying an "error" field (the shape aisstream.io's docs are said to
  // use for a rejected key or subscription - not verified here) is kept for the
  // portal instead of being parsed as a vessel.
  const char *err = doc["error"] | (const char *)nullptr;
  if (err) {
    strncpy(s_err, err, sizeof(s_err) - 1);
    s_err[sizeof(s_err) - 1] = '\0';
    return;
  }

  const char *type = doc["MessageType"] | "";
  JsonObjectConst meta = doc["MetaData"];
  const uint32_t mmsi = meta["MMSI"] | 0U;
  if (!mmsi) return;

  // Length and type, from whichever message carries them: kept for later, and
  // applied now if the vessel is already on the plot.
  auto takeStatic = [mmsi](JsonObjectConst dim, uint8_t shipType) {
    const uint16_t l = (uint16_t)((dim["A"] | 0) + (dim["B"] | 0));
    rememberStatic(mmsi, l, shipType);
    if (YrVessel *v = find(mmsi)) {
      if (l)        v->length_m  = l;
      if (shipType) v->ship_type = shipType;
    }
  };

  JsonObjectConst msg = doc["Message"];
  JsonObjectConst pos;
  if      (!strcmp(type, "PositionReport"))               pos = msg["PositionReport"];
  else if (!strcmp(type, "StandardClassBPositionReport")) pos = msg["StandardClassBPositionReport"];
  else if (!strcmp(type, "ExtendedClassBPositionReport")) pos = msg["ExtendedClassBPositionReport"];

  if (!pos.isNull()) {
    if (pos["Latitude"].isNull() || pos["Longitude"].isNull()) return;
    s_positions++;
    YrVessel *v = slotFor(mmsi);
    recallStatic(v);
    v->lat = pos["Latitude"].as<float>();
    v->lon = pos["Longitude"].as<float>();
    v->sog = pos["Sog"] | 0.0f;
    const char *nm = meta["ShipName"] | (const char *)(pos["Name"] | "");
    if (nm[0]) {
      strncpy(v->name, nm, sizeof(v->name) - 1);
      v->name[sizeof(v->name) - 1] = '\0';
      // AISstream pads names with trailing spaces out of the AIS frame.
      for (int i = (int)strlen(v->name) - 1; i >= 0 && v->name[i] == ' '; i--)
        v->name[i] = '\0';
    }
    if (!strcmp(type, "ExtendedClassBPositionReport"))    // carries its own size
      takeStatic(pos["Dimension"], pos["Type"] | 0);
    v->last_seen = millis();
    s_lastPos = v->last_seen;
  } else if (!strcmp(type, "ShipStaticData")) {
    JsonObjectConst sd = msg["ShipStaticData"];
    takeStatic(sd["Dimension"], sd["Type"] | 0);
  } else if (!strcmp(type, "StaticDataReport")) {
    // Class B static data comes in two parts; part B (PartNumber true) holds the
    // dimensions and type, part A only the name that MetaData already gives.
    JsonObjectConst sr = msg["StaticDataReport"];
    if (!(sr["PartNumber"] | false)) return;
    JsonObjectConst rb = sr["ReportB"];
    takeStatic(rb["Dimension"], rb["ShipType"] | 0);
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
  // Class B too. Most yachts carry Class B transponders, which report position
  // in StandardClassBPositionReport / ExtendedClassBPositionReport and size in
  // StaticDataReport part B - names and fields from aisstream's own models,
  // github.com/aisstream/ais-message-models (golang/aisStream/docs). Class A
  // alone, as the flagship subscribes, misses most of the bay.
  m += F("\"FilterMessageTypes\":[\"PositionReport\",\"ShipStaticData\","
         "\"StandardClassBPositionReport\",\"ExtendedClassBPositionReport\","
         "\"StaticDataReport\"]}");
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
  case WStype_BIN:
    // aisstream.io sends its JSON as BINARY frames (opcode 0x2), not text. The
    // flagship found that live and fixed it in v33.0.3 (src/iot/yacht_radar.cpp,
    // NickoScope32 S3); this port had kept only the text case, so every message
    // was dropped: on the panel, 2026-09-14, four minutes connected and 0 frames.
    s_frames++;
    s_lastFrame = millis();
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
    // isKey first: getString() logs an error for a missing key, and the
    // carousel reaches this page every 75 s - on an unprovisioned board that
    // was one error line every lap. isKey() reads without logging.
    s_key = p.isKey("ais") ? p.getString("ais", "") : String();
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
  if (!s_open) return;
  // The websocket pump can block far longer than the task watchdog allows.
  // arduinoWebSockets passes its 5 s timeout to the socket only; the TLS
  // handshake loop inside WiFiClientSecure is bounded by handshake_timeout,
  // which is 120 s and is never overridden. main.cpp arms a 15 s watchdog with
  // panic=true, so an unreachable broker or a lossy handshake is a reboot loop
  // rather than a slow page. Unsubscribe for the duration of the call.
  esp_task_wdt_delete(NULL);
  s_ws.loop();
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();
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

// Order for the table. The chart already shows where everything is, so the list
// earns its place by answering one of two questions - what is nearest, or what
// is biggest - and the knob picks which.
static float sortKey(uint8_t i) {
  return s_bySize ? -(float)s_v[i].length_m : distDac(s_v[i]);
}
static void sortRows(uint8_t *idx, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {          // insertion sort, n <= 16
    const uint8_t k = idx[i];
    const float   d = sortKey(k);
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sortKey(idx[j]) > d) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = k;
  }
}

// ---- sweep and list motion
// The beam turns once per YR_SWEEP_MS, fx34's rate on the flagship (6 s since
// v45.3.12), with a fading afterglow behind it.
static const uint32_t YR_SWEEP_MS     = 6000;
static const int8_t   YR_TRAIL_LINES  = 10;
static const float    YR_TRAIL_STEP   = 4.0f * (float)DEG_TO_RAD;
static const float    YR_TWO_PI       = 6.2831853f;
// A vessel the beam crosses flashes on the chart and in the list: up in 150 ms,
// held to 1100 ms, gone by 1500 ms - fx34's badge timing.
static const uint32_t YR_PING_UP_MS   = 150;
static const uint32_t YR_PING_HOLD_MS = 1100;
static const uint32_t YR_PING_MS      = 1500;
// A list longer than the screen walks by itself, in a loop: the top row holds
// for YR_ROW_HOLD_MS, then everything slides up one row. At the page's 10 Hz,
// 700 ms is one pixel a frame. A turn of the knob moves it by hand and the walk
// resumes after YR_KNOB_PAUSE_MS.
static const uint32_t YR_ROW_HOLD_MS   = 1500;
static const uint32_t YR_ROW_SLIDE_MS  = 700;
static const uint32_t YR_KNOB_PAUSE_MS = 3000;

static uint32_t s_scrollAnchor = 0;    // the walk counts from here
static uint8_t  s_firstShown   = 0;    // row at the top in the last frame
static float    s_prevSweep    = 0.0f;
static uint32_t s_lastRender   = 0;

void yachtRadarScroll(int8_t delta) {
  if (s_count <= YR_TABLE_ROWS) { s_scroll = 0; return; }
  int16_t v = ((int16_t)s_firstShown + delta) % (int16_t)s_count;
  if (v < 0) v += s_count;
  s_scroll = (uint8_t)v;
  s_scrollAnchor = millis() + YR_KNOB_PAUSE_MS;
}
void yachtRadarToggleSort() { s_bySize = !s_bySize; s_scroll = 0; s_scrollAnchor = millis(); }
bool yachtRadarSortsBySize() { return s_bySize; }

static float pingLevel(const YrVessel &v, uint32_t now) {
  if (!v.last_ping) return 0.0f;
  const uint32_t t = now - v.last_ping;
  if (t >= YR_PING_MS)     return 0.0f;
  if (t < YR_PING_UP_MS)   return (float)t / YR_PING_UP_MS;
  if (t < YR_PING_HOLD_MS) return 1.0f;
  return 1.0f - (float)(t - YR_PING_HOLD_MS) / (float)(YR_PING_MS - YR_PING_HOLD_MS);
}

// Did the beam pass bearing a, clockwise from `from` to `to`?
static bool swept(float from, float to, float a) {
  if (to >= from) return a > from && a <= to;
  return a > from || a <= to;                 // wrapped past north
}

static void drawSweep(float theta) {
  // Oldest first, so the bright leading edge wins where the lines overlap.
  for (int8_t k = YR_TRAIL_LINES - 1; k >= 0; k--) {
    const float a = theta - k * YR_TRAIL_STEP;
    const uint8_t alpha = k == 0 ? 200 : (uint8_t)(100 * (YR_TRAIL_LINES - k) / YR_TRAIL_LINES);
    const float sx = sinf(a), cy = cosf(a);
    for (int16_t r = 2; r <= YR_R_PX; r++)
      blendOverMap((int16_t)lroundf(YR_CX + r * sx), (int16_t)lroundf(YR_CY - r * cy),
                   120, 255, 170, alpha);
  }
}

static uint8_t towardWhite(uint8_t c, float f) { return (uint8_t)(c + (255 - c) * f); }

static void drawRow(const YrVessel &v, int16_t top, uint32_t now, uint16_t dim) {
  int16_t bx, by; uint16_t bw, bh;
  const int16_t base = top + 4;
  uint8_t r, g, b; colourFor(v, &r, &g, &b);
  const float f = pingLevel(v, now);
  if (f > 0.0f) {
    // The swept vessel's row lights up with its dot: a tinted bar under it and
    // the text pushed towards white, fading together.
    display.fillRect(YR_X_TABLE - 2, top, YR_X_RIGHT - YR_X_TABLE + 4, YR_ROW_H - 1,
                     display.color565((uint8_t)(r * f * 0.35f), (uint8_t)(g * f * 0.35f),
                                      (uint8_t)(b * f * 0.35f)));
    r = towardWhite(r, f); g = towardWhite(g, f); b = towardWhite(b, f);
  }
  const uint16_t c = display.color565(r, g, b);

  // Length is the one number worth the width - it is what separates a tender
  // from a superyacht - so it is drawn first and the name takes what is left.
  // Until the vessel's static data arrives (minutes, for some) a dim "--"
  // holds its place.
  char len[6];
  if (v.length_m) snprintf(len, sizeof(len), "%uM", (unsigned)v.length_m);
  else            strncpy(len, "--", sizeof(len));
  display.getTextBounds(len, 0, 0, &bx, &by, &bw, &bh);
  const uint16_t lenW = bw;
  display.setTextColor(v.length_m ? c : dim);
  display.setCursor(YR_X_RIGHT - (int16_t)bw, base);
  display.print(len);

  char nm[YR_NAME_LEN];
  if (v.name[0]) { strncpy(nm, v.name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0'; }
  else snprintf(nm, sizeof(nm), "%06lu", (unsigned long)(v.mmsi % 1000000UL));
  const int16_t room = (YR_X_RIGHT - (int16_t)lenW - YR_GAP) - YR_X_TABLE;
  for (;;) {
    if (nm[0] == '\0') break;
    display.getTextBounds(nm, 0, 0, &bx, &by, &bw, &bh);
    if ((int16_t)bw <= room) break;
    // Same rule as the flight board's city column, and for the same reason:
    // a shaved word reads as a typo. Drop trailing words; if the first one
    // still will not fit, show nothing rather than half of it.
    char *sp = strrchr(nm, ' ');
    if (sp == NULL) { nm[0] = '\0'; break; }
    *sp = '\0';
  }
  display.setTextColor(c);
  display.setCursor(YR_X_TABLE, base);
  display.print(nm);
}

void yachtRadarRender() {
  display.fillScreen(0);
  display.setFont(&PicopixelFB);
  display.setTextWrap(false);
  display.setTextSize(1);   // sticky: animated clocks leave it at 3

  const uint16_t white = display.color565(255, 255, 255);
  const uint16_t dim   = display.color565(110, 122, 128);
  const uint32_t now   = millis();

  // --- left: the chart ---
  // Blitted, not drawn: sea shaded by measured depth, land by measured
  // elevation with a hillshade, shoreline on top. The shading, the fill and
  // the anti-aliasing are settled at build time by tools/yr_basemap_gen.py -
  // what remains here is 4096 drawPixel calls a frame, which is not free but
  // is a copy rather than a computation.
  for (int16_t y = 0; y < YR_MAP_H; y++)
    for (int16_t x = 0; x < YR_MAP_W; x++)
      display.drawPixel(x, y, kYrBasemap[y * YR_MAP_W + x]);

  const float theta = YR_TWO_PI * (float)(now % YR_SWEEP_MS) / (float)YR_SWEEP_MS;
  if (now - s_lastRender > 500) s_prevSweep = theta;   // back on the page: no burst of stale pings
  s_lastRender = now;
  drawSweep(theta);

  display.setTextColor(display.color565(200, 230, 255));
  display.setCursor(YR_CX - 1, 0 + 4);      display.print('N');
  display.setCursor(YR_CX - 1, 57 + 4);     display.print('S');
  display.setCursor(1, YR_CY - 3 + 4);      display.print('W');
  display.setCursor(57, YR_CY - 3 + 4);     display.print('E');

  expire();
  uint8_t idx[YR_MAX_VESSELS];
  for (uint8_t i = 0; i < s_count; i++) idx[i] = i;
  sortRows(idx, s_count);

  for (uint8_t i = 0; i < s_count; i++) {
    YrVessel &v = s_v[idx[i]];
    float fx, fy; project(v, &fx, &fy);
    const int16_t ix = (int16_t)lroundf(fx), iy = (int16_t)lroundf(fy);
    if (ix < 0 || ix >= YR_MAP_W || iy < 0 || iy >= YR_MAP_H) continue;

    float bearing = atan2f(fx - YR_CX, YR_CY - fy);   // 0 = north, clockwise
    if (bearing < 0) bearing += YR_TWO_PI;
    if (swept(s_prevSweep, theta, bearing)) v.last_ping = now ? now : 1;
    const float f = pingLevel(v, now);

    uint8_t r, g, b; colourFor(v, &r, &g, &b);

    // A dark ring first: over a coloured chart a bright dot alone reads as part
    // of the map, and the vessels are the point of the page.
    for (int8_t dx = -1; dx <= 1; dx++)
      for (int8_t dy = -1; dy <= 1; dy++)
        if (dx || dy) blendOverMap(ix + dx, iy + dy, 0, 0, 0, 90);

    if (f > 0.0f) {
      // Swept: the hull flares - a ring in its own colour and a cross beyond it,
      // both fading with the flash.
      const uint8_t ring = (uint8_t)(230.0f * f);
      for (int8_t dx = -1; dx <= 1; dx++)
        for (int8_t dy = -1; dy <= 1; dy++)
          if (dx || dy) blendOverMap(ix + dx, iy + dy, r, g, b, ring);
      blendOverMap(ix - 2, iy, r, g, b, ring / 2);
      blendOverMap(ix + 2, iy, r, g, b, ring / 2);
      blendOverMap(ix, iy - 2, r, g, b, ring / 2);
      blendOverMap(ix, iy + 2, r, g, b, ring / 2);
    }

    softDot(fx, fy, r, g, b, 0.5f);                  // sub-pixel halo
    blendOverMap(ix, iy, towardWhite(r, f), towardWhite(g, f), towardWhite(b, f), 255);
    if (v.length_m >= 60) {                          // mega: size at a glance
      blendOverMap(ix - 1, iy, r, g, b, 190);
      blendOverMap(ix + 1, iy, r, g, b, 190);
      blendOverMap(ix, iy - 1, r, g, b, 190);
      blendOverMap(ix, iy + 1, r, g, b, 190);
    }
  }
  s_prevSweep = theta;

  // --- right: the table ---
  // Rows first, then the header over whatever slid up under it.
  const bool walking = s_count > YR_TABLE_ROWS;
  uint8_t first = 0;
  int16_t slide = 0;
  if (walking) {
    const uint32_t cycle = YR_ROW_HOLD_MS + YR_ROW_SLIDE_MS;
    const uint32_t t = (int32_t)(now - s_scrollAnchor) > 0 ? now - s_scrollAnchor : 0;
    first = (uint8_t)((s_scroll + t / cycle) % s_count);
    const uint32_t in = t % cycle;
    if (in > YR_ROW_HOLD_MS) slide = (int16_t)((in - YR_ROW_HOLD_MS) * YR_ROW_H / YR_ROW_SLIDE_MS);
  } else {
    s_scroll = 0;
  }
  s_firstShown = first;
  const uint8_t rows = walking ? (uint8_t)(YR_TABLE_ROWS + 2) : s_count;
  for (uint8_t i = 0; i < rows; i++) {
    const int16_t top = YR_Y_ROW0 + i * YR_ROW_H - slide;
    if (top >= 64) break;
    drawRow(s_v[idx[(first + i) % s_count]], top, now, dim);
  }

  display.fillRect(YR_X_TABLE - 2, 0, 128 - (YR_X_TABLE - 2), YR_Y_ROW0 - 1, 0);
  display.setTextColor(white);
  display.setCursor(YR_X_TABLE, YR_Y_TITLE + 4);
  display.print(F("BAY OF CANNES"));

  char sub[24];
  if (s_noKey)            strncpy(sub, "NO AIS KEY", sizeof(sub));
  else if (!s_conn)       strncpy(sub, "CONNECTING", sizeof(sub));
  else if (s_count == 0)  strncpy(sub, "SCANNING", sizeof(sub));
  else snprintf(sub, sizeof(sub), "%u NOW  BY %s",
                (unsigned)s_count, s_bySize ? "SIZE" : "RANGE");
  sub[sizeof(sub) - 1] = '\0';
  display.setTextColor(dim);
  display.setCursor(YR_X_TABLE, YR_Y_SUB + 4);
  display.print(sub);
  display.drawFastHLine(YR_X_TABLE - 1, YR_Y_RULE, YR_X_RIGHT - YR_X_TABLE + 1,
                        display.color565(40, 48, 54));

  display.setFont(NULL);   // other pages draw with the built-in font
}

bool     yachtRadarHasData() { return s_count > 0; }
uint8_t  yachtRadarCount()   { return s_count; }
uint32_t yachtRadarAge()     { return s_lastPos ? millis() - s_lastPos : 0xFFFFFFFFUL; }

void yachtRadarSetSortBySize(bool bySize) {
  if (bySize != s_bySize) yachtRadarToggleSort();
}

// Whether NVS holds a key. Read once and remembered: the key only changes by
// flashing the provisioning image, which reboots. isKey() rather than
// getString(), which logs an error for a missing key.
static bool keyStored() {
  static int8_t known = -1;
  if (s_open) return true;                   // begin() opens only with a key
  if (known < 0) {
    known = 0;
    Preferences p;
    if (p.begin("yr", true)) {
      known = p.isKey("ais") ? 1 : 0;
      p.end();
    }
  }
  return known == 1 && !s_noKey;             // stored but empty: begin() said so
}

void yachtRadarStatusJson(JsonObject out) {
  out["keyPresent"] = keyStored();
  out["open"]       = s_open;               // only while the page is on screen
  out["connected"]  = s_conn;
  out["count"]      = s_count;
  out["logged"]     = s_logged;
  out["frames"]     = s_frames;             // every text frame since boot
  out["positions"]  = s_positions;          // position reports among them
  if (s_lastFrame) out["frameAge"] = (millis() - s_lastFrame) / 1000UL;
  if (s_err[0]) out["error"] = (const char *)s_err;
  out["bySize"]     = s_bySize;
  if (s_lastPos) out["age"] = (millis() - s_lastPos) / 1000UL;
  uint8_t idx[YR_MAX_VESSELS];
  for (uint8_t i = 0; i < s_count; i++) idx[i] = i;
  sortRows(idx, s_count);
  JsonArray list = out["vessels"].to<JsonArray>();
  for (uint8_t k = 0; k < s_count; k++) {
    const YrVessel &v = s_v[idx[k]];
    JsonObject o = list.add<JsonObject>();
    if (v.name[0]) o["name"] = (const char *)v.name;
    else           o["mmsi"] = v.mmsi;
    o["km"]  = distDac(v) / YR_DAC_PER_KM;
    o["sog"] = v.sog;
    if (v.length_m) o["len"] = v.length_m;
    o["m"]   = (uint8_t)motionOf(v);          // 0 anchored, 1 manoeuvring, 2 under way
    if (v.length_m) o["len"] = v.length_m;    // metres, once static data has come
  }
}

#endif  // YACHTRADAR_ENABLED

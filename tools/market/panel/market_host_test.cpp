// Host test for src/market/market_model.{h,cpp}, market_layout.{h,cpp} and
// market_pico.cpp: the decoders and their refusals, the number rules, dates,
// the base64 lines, ingest by topic, the watch lists, the LittleFS record, and
// the four pages drawn through the same Canvas the panel draws through - here
// a recorder (every string with its x, y, font and colour) and a 128x64 raster
// with the panel's own fonts. Built and run by tools/market/panel/check_market_panel.py.
//
//   market_host_test                      the tests
//   market_host_test fmt                  stdin: "value V SLOT" | "amount V SLOT" | "pct X" | "share X" | "date ISO"
//                                         -> one formatted line each
//   market_host_test layout SPEC [PPM]    a page from a spec (watch lists, payloads by topic, the view):
//                                         JSON with the items, notes, info and refusals; the raster as P6 to PPM
//   market_host_test parse LEAF FILE      one payload through ingest(): OK, or REFUSED and why
//   market_host_test local                built with -DMARKET_LOCAL_DEFAULTS_FILE: the local defaults mechanism
//   market_host_test measure              stdin: "p TEXT" | "5 K THIN TEXT" | "v V SLOT" | "a V SLOT" -> Picopixel ink,
//                                         5x7 width, fmtValue, fmtAmount, one line each (the layout budget check)
//   market_host_test webjson              the registry, the default config and the config payload as JSON (the mock portal)
//   market_host_test apply BASE IN        a POST's config over a base config through mks::apply, as /api/market does

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "market_layout.h"
#include "market_model.h"
#include "market_settings.h"
#include "market_settle.h"
#define CONTROL_ENCODER_ENABLED    // panel.h's page keys, for the page-mask test
#include "panel/panel.h"
#include "panel/panel_pages.h"
#include "picopixel_fb.h"   // PicopixelFB: the panel's small font, the U corrected
#include "glcdfont.c"       // Adafruit's classic 5x7: font[]

using namespace market;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQS(a, b) do { const std::string x_(a), y_(b); if (x_ == y_) g_pass++; else { g_fail++; \
  std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); } } while (0)

static Model g_m;
static Spare g_sp;

// ── a recording, rasterising canvas ─────────────────────────────────────────
struct Item {
  std::string role, text, font;
  int x, y, k, col;
  bool thin;
};

class Rec : public Canvas {
 public:
  std::vector<Item> items;
  std::map<std::string, std::string> notes;
  uint8_t px[H][W][3];

  Rec() { memset(px, 0, sizeof(px)); }

  void put(int x, int y, Col c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    // color565 keeps 5/6/5 bits: what the panel shows and render.py saves.
    px[y][x][0] = (uint8_t)(kColRgb[c][0] >> 3 << 3);
    px[y][x][1] = (uint8_t)(kColRgb[c][1] >> 2 << 2);
    px[y][x][2] = (uint8_t)(kColRgb[c][2] >> 3 << 3);
  }

  void glyph5(int16_t x, int16_t top, char ch, Col c, uint8_t k) override {
    const unsigned char uc = (unsigned char)ch;
    for (int i = 0; i < 5; i++) {
      const uint8_t bits = font[uc * 5 + i];
      for (int j = 0; j < 8; j++)
        if ((bits >> j) & 1)
          for (int dx = 0; dx < k; dx++)
            for (int dy = 0; dy < k; dy++) put(x + i * k + dx, top + j * k + dy, c);
    }
  }

  // Adafruit_GFX::drawChar for a custom font, from the cursor on the baseline.
  void textP(int16_t x, int16_t top, const char *s, Col c) override {
    const int base = top + MK_PICO_ASCENT;
    for (; *s; s++) {
      unsigned char ch = (unsigned char)*s;
      if (ch < PicopixelFB.first || ch > PicopixelFB.last) ch = '?';
      const GFXglyph &g = PicopixelFB.glyph[ch - PicopixelFB.first];
      const uint8_t *bmp = PicopixelFB.bitmap + g.bitmapOffset;
      uint8_t bits = 0, bit = 0;
      for (int yy = 0; yy < g.height; yy++)
        for (int xx = 0; xx < g.width; xx++) {
          if (!(bit++ & 7)) bits = *bmp++;
          if (bits & 0x80) put(x + g.xOffset + xx, base + g.yOffset + yy, c);
          bits = (uint8_t)(bits << 1);
        }
      x = (int16_t)(x + g.xAdvance);
    }
  }

  void dot(int16_t x, int16_t y, Col c) override { put(x, y, c); }

  void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Col c) override {   // Adafruit_GFX::writeLine
    const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    const int dx = x1 - x0, dy = std::abs(y1 - y0), ystep = y0 < y1 ? 1 : -1;
    int err = dx / 2;
    for (; x0 <= x1; x0++) {
      if (steep) put(y0, x0, c); else put(x0, y0, c);
      err -= dy;
      if (err < 0) { y0 = (int16_t)(y0 + ystep); err += dx; }
    }
  }

  void hline(int16_t y, int16_t x0, int16_t x1, Col c) override { for (int x = x0; x <= x1; x++) put(x, y, c); }
  void fill(int16_t x, int16_t y, int16_t w, int16_t h, Col c) override {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put(x + i, y + j, c);
  }

  void label5(int16_t x, int16_t top, const char *s, Col c, uint8_t k, bool thin, Role r) override {
    items.push_back({kRoleKeys[r], s, "5x7", x, top, k, c, thin});
  }
  void labelP(int16_t x, int16_t top, const char *s, Col c, Role r) override {
    items.push_back({kRoleKeys[r], s, "pico", x, top, 1, c, false});
  }
  void note(Role r, const char *s) override { notes[kRoleKeys[r]] = s; }

  bool writePpm(const char *path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << W << " " << H << "\n255\n";
    f.write((const char *)px, sizeof(px));
    return (bool)f;
  }
};

// ── helpers ─────────────────────────────────────────────────────────────────
static std::string b64(const std::vector<uint16_t> &u) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> raw;
  for (uint16_t v : u) { raw.push_back((uint8_t)(v & 0xFF)); raw.push_back((uint8_t)(v >> 8)); }
  std::string out;
  for (size_t i = 0; i < raw.size(); i += 3) {
    uint32_t bits = (uint32_t)raw[i] << 16;
    if (i + 1 < raw.size()) bits |= (uint32_t)raw[i + 1] << 8;
    if (i + 2 < raw.size()) bits |= raw[i + 2];
    out += A[(bits >> 18) & 63];
    out += A[(bits >> 12) & 63];
    out += i + 1 < raw.size() ? A[(bits >> 6) & 63] : '=';
    out += i + 2 < raw.size() ? A[bits & 63] : '=';
  }
  return out;
}

static std::vector<uint16_t> ramp() {
  std::vector<uint16_t> u;
  for (int i = 0; i < kPoints; i++) u.push_back((uint16_t)(i * 515));
  return u;
}

static std::string readFile(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The ticker on screen, as market_ha.cpp passes it to ingest().
static const char *g_ticker = "VOO";

// Through ingest() by topic, as market_ha.cpp does; "" when accepted.
static std::string feed(const char *leaf, const std::string &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "unparsable";
  const char *why = ingest(g_m, g_sp, leaf, doc.as<JsonObjectConst>(), g_ticker);
  return why ? why : "";
}

// A valid index payload with one key replaced by raw JSON, or removed when raw is null.
static std::string indexJson(const char *key = nullptr, const char *raw = nullptr) {
  std::map<std::string, std::string> f = {
    {"v", "1"}, {"sym", "\"^GSPC\""}, {"name", "\"SPX\""}, {"cur", "\"USD\""}, {"preset", "\"MAX\""},
    {"from", "\"2000-01-01\""}, {"to", "\"2026-09-14\""}, {"last", "7619.98"}, {"chg", "4.58"},
    {"hi", "7799.2"}, {"lo", "676.5"}, {"n", "128"}, {"min", "676.5"}, {"max", "7799.2"},
    {"pts", "\"" + b64(ramp()) + "\""}, {"cagr", "0.066"}, {"mdd", "-0.567"}};
  if (key) { if (raw) f[key] = raw; else f.erase(key); }
  std::string s = "{";
  for (auto &kv : f) s += (s.size() > 1 ? "," : "") + std::string("\"") + kv.first + "\":" + kv.second;
  return s + "}";
}

static std::string portfolioJson(const char *key = nullptr, const char *raw = nullptr) {
  std::map<std::string, std::string> f = {
    {"v", "1"}, {"cur", "\"EUR\""}, {"mode", "\"hold\""}, {"preset", "\"MAX\""}, {"from", "\"2000-01-01\""},
    {"to", "\"2026-09-14\""}, {"value", "89800.95"}, {"chg", "7.98"}, {"sinceStart", "7.98"}, {"cagr", "0.086"},
    {"mdd", "-0.198"}, {"div", "8803.9"}, {"terDrag", "170.51"}, {"cash", "0.0056"}, {"n", "128"}, {"min", "9000"},
    {"max", "90000"}, {"pts", "\"" + b64(ramp()) + "\""}, {"px", "\"" + b64(ramp()) + "\""}, {"bench", "\"" + b64(ramp()) + "\""}};
  if (key) { if (raw) f[key] = raw; else f.erase(key); }
  std::string s = "{";
  for (auto &kv : f) s += (s.size() > 1 ? "," : "") + std::string("\"") + kv.first + "\":" + kv.second;
  return s + "}";
}

static void watch(std::vector<const char *> idx, std::vector<const char *> tk) {
  setWatch(g_m, g_sp, false, idx.data(), nullptr, (uint8_t)idx.size());
  setWatch(g_m, g_sp, true, tk.data(), nullptr, (uint8_t)tk.size());
}

// ── the tests ───────────────────────────────────────────────────────────────
static void testDates() {
  const int32_t d = parseDate("2026-09-14");
  CHECK(d == daysFromCivil(2026, 9, 14));
  int y, m, dd;
  civilFromDays(d, &y, &m, &dd);
  CHECK(y == 2026 && m == 9 && dd == 14);
  CHECK(weekday(d) == 0);                                  // a Monday
  CHECK(weekday(parseDate("2026-09-20")) == 6);            // a Sunday
  CHECK(parseDate("2026-02-30") == kNoDate);
  CHECK(parseDate("2026-9-14") == kNoDate);
  CHECK(parseDate("") == kNoDate);
  CHECK(parseDate(nullptr) == kNoDate);
  CHECK(weekdaysAfter(d, parseDate("2026-09-17")) == 3);
  CHECK(weekdaysAfter(d, parseDate("2026-09-21")) == 5);
  CHECK(weekdaysAfter(d, parseDate("2026-10-12")) == 20);
  CHECK(weekdaysAfter(d, d) == 0);
  CHECK(!isStale(d, parseDate("2026-09-17")));
  CHECK(isStale(d, parseDate("2026-09-18")));
  CHECK(isStale(d, parseDate("2026-09-21")));
  CHECK(!isStale(d, kNoDate));
  char b[8];
  fmtDate(b, sizeof(b), d);
  EQS(b, "14 SEP");
  fmtDate(b, sizeof(b), parseDate("2026-01-05"));
  EQS(b, "05 JAN");
}

static void testNumbers() {
  char b[32];
  thousands(b, sizeof(b), 26186);      EQS(b, "26 186");
  thousands(b, sizeof(b), 999);        EQS(b, "999");
  thousands(b, sizeof(b), 1000000);    EQS(b, "1 000 000");
  thousands(b, sizeof(b), -1245);      EQS(b, "-1 245");
  fmtValue(b, sizeof(b), 7619.98, 6);  EQS(b, "7 620");
  fmtValue(b, sizeof(b), 699.3, 7);    EQS(b, "699.3");
  fmtValue(b, sizeof(b), 26186.4, 7);  EQS(b, "26 186");
  fmtValue(b, sizeof(b), -130900.0, 7); EQS(b, "-130.9K");
  fmtValue(b, sizeof(b), 89800.95, 7); EQS(b, "89 801");
  fmtValue(b, sizeof(b), 1234567.0, 7); EQS(b, "1234.6K");   // K fits the slot first, as render.py
  fmtValue(b, sizeof(b), 1234567.0, 6); EQS(b, "1.2M");
  fmtValue(b, sizeof(b), 2.5, 7);      EQS(b, "2.5");
  fmtAmount(b, sizeof(b), 8803.90, 4); EQS(b, "8.8K");
  fmtAmount(b, sizeof(b), 170.51, 4);  EQS(b, "171");
  fmtAmount(b, sizeof(b), 705.4, 7);   EQS(b, "705");
  fmtAmount(b, sizeof(b), -134900.0, 7); EQS(b, "-134.9K");
  fmtAmount(b, sizeof(b), 0.0, 4);     EQS(b, "0");
  fmtAmount(b, sizeof(b), 2.5, 4);     EQS(b, "2");     // Python's round: half to even
  fmtAmount(b, sizeof(b), 3.5, 4);     EQS(b, "4");
  fmtAmount(b, sizeof(b), 21663.12, 4); EQS(b, "22K");    // not 0.0M any more
  fmtAmount(b, sizeof(b), 12345, 4);    EQS(b, "12K");
  fmtAmount(b, sizeof(b), 999499, 4);   EQS(b, "999K");
  fmtAmount(b, sizeof(b), 999500, 4);   EQS(b, "1.0M");
  fmtAmount(b, sizeof(b), -21663, 4);   EQS(b, "-22K");
  fmtYm(b, sizeof(b), parseDate("2020-03-01")); EQS(b, "20-03");
  fmtYm(b, sizeof(b), parseDate("2003-02-01")); EQS(b, "03-02");
  // The drawdown's points (render.slot_index, the frames' marks).
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2026-09-14"), parseDate("2020-01-01")) == 95);
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2026-09-14"), parseDate("2020-03-01")) == 96);
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2026-09-14"), parseDate("2000-03-01")) == 0);
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2026-09-14"), parseDate("2003-02-01")) == 14);
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2026-09-14"), parseDate("2026-09-14")) == 127);
  CHECK(slotIndex(parseDate("2000-01-01"), parseDate("2000-01-01"), parseDate("2000-01-01")) == 127);
  fmtPct(b, sizeof(b), 4.58, true);    EQS(b, "+458%");
  fmtPct(b, sizeof(b), 0.124, true);   EQS(b, "+12.4%");
  fmtPct(b, sizeof(b), 12.45, true);   EQS(b, "+1 245%");
  fmtPct(b, sizeof(b), -0.0048, true); EQS(b, "-0.5%");
  fmtPct(b, sizeof(b), 0.0004, true);  EQS(b, "0.0%");
  fmtPct(b, sizeof(b), -0.0004, true); EQS(b, "0.0%");
  fmtPct(b, sizeof(b), 0.9996, true);  EQS(b, "+100%");
  fmtPct(b, sizeof(b), 0.0, false);    EQS(b, "--");
  fmtPct(b, sizeof(b), 0.0015, true);  EQS(b, "+0.1%");   // 0.15 rounds as the exact decimal, not 0.2
  CHECK(pctSign(0.0004, true) == 0 && pctSign(0.01, true) == 1 && pctSign(-0.01, true) == -1 && pctSign(1, false) == 0);
  fmtShare(b, sizeof(b), 0.0056);      EQS(b, "0.6%");
  fmtShare(b, sizeof(b), 0.0055);      EQS(b, "0.5%");     // 0.0055 * 100 is 0.5499.. in a double, in Python too
  fmtShare(b, sizeof(b), 0.54);        EQS(b, "54%");
  fmtShare(b, sizeof(b), 0.0);         EQS(b, "0.0%");
  fmtShare(b, sizeof(b), 0.999);       EQS(b, "100%");
}

static void testFonts() {
  CHECK(w5("DATA ERR", 1, false) == 47);
  CHECK(w5("7 620", 2, true) == 52);
  CHECK(w5("699.3", 2, true) == 52);
  CHECK(w5("", 1, false) == 0);
  CHECK(picoAdv("A") == 4 && picoInk("A") == 3);
  CHECK(picoAdv(" ") == 2 && picoInk(" ") == 0);
  CHECK(picoInk("A A") == 9);
  CHECK(picoInk("AS OF 14 SEP") > 30 && picoInk("AS OF 14 SEP") < 50);
}

static void testBase64() {
  const std::vector<uint16_t> u = ramp();
  const std::string s = b64(u);
  CHECK(s.size() == kPtsB64);
  uint16_t out[kPoints];
  CHECK(decodePts(s.c_str(), out));
  bool same = true;
  for (int i = 0; i < kPoints; i++) same = same && out[i] == u[i];
  CHECK(same);
  CHECK(!decodePts(s.substr(0, 340).c_str(), out));
  std::string bad = s; bad[10] = '*';
  CHECK(!decodePts(bad.c_str(), out));
  std::string nopad = s; nopad[342] = 'A'; nopad[343] = 'A';
  CHECK(!decodePts(nopad.c_str(), out));
  CHECK(!decodePts(nullptr, out));
}

static void testSeries() {
  watch({"^GSPC", "^IXIC"}, {"VOO"});
  EQS(feed("index/^GSPC/MAX", indexJson()), "");
  CHECK(g_m.idx[0].p[P_MAX].have);
  CHECK(std::fabs(g_m.idx[0].p[P_MAX].last - 7619.98f) < 0.01f);
  CHECK(g_m.idx[0].p[P_MAX].hasCagr);
  EQS(g_m.idx[0].p[P_MAX].name, "SPX");
  EQS(g_m.idx[0].p[P_MAX].cur, "USD");
  CHECK(g_m.idx[0].p[P_MAX].pts[127] == 127 * 515);
  const uint32_t gen = g_m.gen;
  EQS(feed("index/^GSPC/MAX", indexJson("v", "2")), "v");
  EQS(feed("index/^GSPC/MAX", indexJson("v", "true")), "v");
  EQS(feed("index/^GSPC/MAX", indexJson("sym", "\"^IXIC\"")), "sym differs from the topic");
  EQS(feed("index/^GSPC/MAX", indexJson("preset", "\"5Y\"")), "preset");
  EQS(feed("index/^GSPC/5Y", indexJson()), "preset");
  EQS(feed("index/^GSPC/MAX", indexJson("cur", "\"usd\"")), "cur");
  EQS(feed("index/^GSPC/MAX", indexJson("last", "\"7620\"")), "last");
  EQS(feed("index/^GSPC/MAX", indexJson("last", nullptr)), "last");
  EQS(feed("index/^GSPC/MAX", indexJson("chg", "true")), "chg");
  EQS(feed("index/^GSPC/MAX", indexJson("n", "127")), "n");
  EQS(feed("index/^GSPC/MAX", indexJson("pts", "\"AAAA\"")), "pts");
  EQS(feed("index/^GSPC/MAX", indexJson("pts", "12")), "pts");
  EQS(feed("index/^GSPC/MAX", indexJson("from", "\"2027-01-01\"")), "from after to");
  EQS(feed("index/^GSPC/MAX", indexJson("to", "\"14 SEP\"")), "to");
  EQS(feed("index/^GSPC/MAX", indexJson("min", "9000")), "min above max");
  EQS(feed("index/^GSPC/MAX", indexJson("name", "\"a very long name\"")), "name");
  EQS(feed("index/^GSPC/MAX", indexJson("cagr", "null")), "");          // null cagr: under three years
  CHECK(!g_m.idx[0].p[P_MAX].hasCagr);
  EQS(feed("index/^GSPC/MAX", indexJson("name", nullptr)), "");           // no name: the symbol
  EQS(g_m.idx[0].p[P_MAX].name, "^GSPC");
  CHECK(g_m.gen == gen + 2);
  EQS(feed("index/^FCHI/MAX", indexJson("sym", "\"^FCHI\"")), "unwatched symbol");
  EQS(feed("ticker/^GSPC/MAX", indexJson()), "unwatched symbol");      // an index is not a ticker
  EQS(feed("ticker/VOO/1Y", indexJson("sym", "\"VOO\"")), "preset");     // the payload says MAX
  EQS(feed("index/^GSPC/2Y", indexJson()), "bad preset");
  EQS(feed("index/^GSPC", indexJson()), "unknown topic");
  EQS(feed("index/^GSPC/MAX/more", indexJson()), "unknown topic");
  EQS(feed("config", indexJson()), "unknown topic");
  EQS(feed("", indexJson()), "unknown topic");
  // A refused payload leaves the live record as it was.
  CHECK(g_m.idx[0].p[P_MAX].have && g_m.idx[0].p[P_MAX].pts[127] == 127 * 515);
  CHECK(!g_m.idx[1].p[P_MAX].have);
}

static void testPortfolio() {
  EQS(feed("portfolio/hold/MAX", portfolioJson()), "");
  CHECK(g_m.pf[M_HOLD][P_MAX].have && g_m.pf[M_HOLD][P_MAX].hasBench && !g_m.pf[M_HOLD][P_MAX].hasGross);
  EQS(feed("portfolio/rebal/MAX", portfolioJson()), "mode");
  EQS(feed("portfolio/hold/5Y", portfolioJson()), "preset");
  EQS(feed("portfolio/hold/MAX", portfolioJson("px", nullptr)), "px");
  EQS(feed("portfolio/hold/MAX", portfolioJson("bench", nullptr)), "");
  CHECK(!g_m.pf[M_HOLD][P_MAX].hasBench);
  EQS(feed("portfolio/hold/MAX", portfolioJson("bench", "\"xx\"")), "bench");
  EQS(feed("portfolio/hold/MAX", portfolioJson("cash", "\"1%\"")), "cash");
  EQS(feed("portfolio/hold/MAX", portfolioJson("cagr", "null")), "");
  EQS(feed("portfolio/both/MAX", portfolioJson()), "bad mode");
  EQS(feed("portfolio/hold", portfolioJson()), "unknown topic");
  CHECK(!g_m.pf[M_REBAL][P_MAX].have);
  // The fields of 11:43: all optional, each refused on a wrong type; unknown ones ignored.
  EQS(feed("portfolio/hold/MAX", portfolioJson("twr", "7.98") + ""), "");
  const std::string full = portfolioJson().substr(0, portfolioJson().size() - 1) +
      ",\"twr\":7.65,\"ann\":0.084,\"xirr_ann\":0.098,\"flows\":true,\"fx_effect\":0.021,\"contrib\":26000,"
      "\"mdd_peak\":\"2020-01-01\",\"mdd_trough\":\"2020-03-01\",\"mdd_recovery\":null,\"stats\":{\"sharpe\":0.5},\"new_field\":[1]}";
  EQS(feed("portfolio/hold/MAX", full), "");
  const Portfolio &pf = g_m.pf[M_HOLD][P_MAX];
  CHECK(pf.hasTwr && pf.hasAnn && pf.hasXirr && pf.flows && pf.hasFx && pf.hasContrib);
  CHECK(pf.mddPeak == parseDate("2020-01-01") && pf.mddTrough == parseDate("2020-03-01") && pf.mddRecovery == kNoDate);
  EQS(feed("portfolio/hold/MAX", portfolioJson("flows", "\"yes\"")), "flows");
  EQS(feed("portfolio/hold/MAX", portfolioJson("twr", "\"7%\"")), "twr");
  EQS(feed("portfolio/hold/MAX", portfolioJson("xirr_ann", "true")), "xirr_ann");
  EQS(feed("portfolio/hold/MAX", portfolioJson("mdd_peak", "\"2020-13-01\"")), "mdd_peak");
  EQS(feed("portfolio/hold/MAX", portfolioJson().substr(0, portfolioJson().size() - 1) +
      ",\"mdd_peak\":\"2020-03-01\",\"mdd_trough\":\"2020-01-01\"}"), "mdd_peak after mdd_trough");
  EQS(feed("index/^GSPC/MAX", indexJson("ann", "0.066")), "");
  CHECK(g_m.idx[0].p[P_MAX].hasAnn);
  EQS(feed("index/^GSPC/MAX", indexJson("ann", "\"6%\"")), "ann");
  EQS(feed("portfolio/hold/MAX", portfolioJson()), "");   // back to the plain one for the tests after
}

static void testHoldings() {
  std::string rows;
  for (int i = 0; i < 17; i++) {
    char r[128];
    snprintf(r, sizeof(r), "%s{\"sym\":\"S%02d\",\"tgt\":5.0,\"now\":4.9,\"ret\":0.5,\"entry\":\"2010-12-01\"}", i ? "," : "", i);
    rows += r;
  }
  EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"asof\":\"2026-09-14\",\"rows\":[" + rows + "],\"cash\":{\"tgt\":0,\"now\":1.2}}"), "");
  CHECK(g_m.hd[M_HOLD].have && g_m.hd[M_HOLD].n == kMaxPositions && g_m.hd[M_HOLD].dropped == 1);
  CHECK(std::fabs(g_m.hd[M_HOLD].cashNow - 1.2f) < 0.001f);
  EQS(feed("holdings/rebal", "{\"v\":1,\"mode\":\"rebal\",\"asof\":\"2026-09-14\",\"rows\":["
                             "{\"sym\":\"VOO\",\"tgt\":100,\"now\":99.4,\"ret\":5.96,\"entry\":\"2010-12-01\"},"
                             "{\"sym\":\"LATE\",\"tgt\":10,\"now\":0,\"ret\":null,\"entry\":null},"
                             "{\"sym\":\"bad sym\",\"tgt\":1,\"now\":1},"
                             "{\"sym\":\"VOO\",\"tgt\":1,\"now\":1}],\"cash\":{\"tgt\":0,\"now\":0.6}}"), "");
  CHECK(g_m.hd[M_REBAL].n == 2 && g_m.hd[M_REBAL].dropped == 2);
  CHECK(g_m.hd[M_REBAL].rows[0].hasRet && !g_m.hd[M_REBAL].rows[1].hasRet && g_m.hd[M_REBAL].rows[1].entry == kNoDate);
  EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"asof\":\"2026-09-14\",\"rows\":[]}"), "cash");
  EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"asof\":\"2026-09-14\",\"rows\":{},\"cash\":{\"tgt\":0,\"now\":0}}"), "rows");
  EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"rows\":[],\"cash\":{\"tgt\":0,\"now\":0}}"), "asof");
}

static void testLiveTapeStatus() {
  EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"^GSPC\":{\"last\":7619.98,\"prev\":7656.98,\"day\":-0.0048,\"state\":\"OPEN\",\"asof\":\"2026-09-14\"},"
                   "\"VOO\":{\"last\":699.3,\"prev\":null,\"day\":null,\"state\":\"CLOSED\"},"
                   "\"BAD\":{\"last\":1,\"state\":\"open\"},\"bad sym\":{\"last\":1,\"state\":\"OPEN\"}}}"), "");
  CHECK(g_m.live.have && g_m.live.n == 2 && g_m.live.dropped == 2);
  CHECK(g_m.live.q[0].state == X_OPEN && g_m.live.q[0].hasDay && g_m.live.q[1].state == X_CLOSED && !g_m.live.q[1].hasDay);
  EQS(feed("live", "{\"v\":1,\"ts\":12,\"q\":{}}"), "ts");
  EQS(feed("live", "{\"v\":1,\"ts\":1789412345}"), "q");
  // The app omits a state it does not know: the quote stays, with none.
  EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"VOO\":{\"last\":699.3,\"delay_s\":902}}}"), "");
  CHECK(g_m.live.n == 1 && g_m.live.q[0].state == X_NONE && g_m.live.q[0].hasDelay && g_m.live.q[0].delayS == 902);
  EQS(feed("tape", "{\"v\":1,\"ts\":1789412345,\"x\":[{\"n\":\"LSE\"}]}"), "");
  CHECK(g_m.tape.n == 1 && g_m.tape.x[0].state == X_NONE);
  EQS(feed("tape", "{\"v\":1,\"ts\":1789412345,\"x\":[{\"n\":\"NYSE\",\"s\":\"OPEN\",\"t\":\"20:00\"},{\"n\":\"LSE\",\"s\":\"CLOSED\",\"t\":\"08:00\"},"
                   "{\"n\":\"XETRA\",\"s\":\"post\"},{\"n\":\"NYSE\",\"s\":\"OPEN\"},{\"n\":\"A VERY LONG ONE\",\"s\":\"OPEN\"}]}"), "");
  CHECK(g_m.tape.have && g_m.tape.n == 2 && g_m.tape.dropped == 3);
  EQS(g_m.tape.x[0].next, "20:00");
  EQS(feed("tape", "{\"v\":1,\"ts\":1789412345,\"x\":\"NYSE\"}"), "x");
  EQS(feed("status", "{\"v\":1,\"asof\":\"2026-09-14\",\"fetched\":\"2026-09-15T07:02Z\",\"state\":\"error\",\"err\":\"yahoo 429\","
                     "\"symbols\":{\"VOO\":{\"asof\":\"2026-09-12\",\"bars\":6700,\"ter\":0.0003,\"terSrc\":\"yahoo\"},\"AAPL\":{\"asof\":null,\"bars\":0,\"ter\":null}}}"), "");
  CHECK(g_m.status.have && g_m.status.state == A_ERROR && g_m.status.n == 2);
  EQS(g_m.status.err, "YAHOO 429");
  CHECK(g_m.status.s[0].hasTer && !g_m.status.s[1].hasTer && g_m.status.s[1].asof == kNoDate);
  EQS(feed("status", "{\"v\":1,\"state\":\"broken\"}"), "state");
  EQS(feed("status", "{\"v\":1,\"state\":\"ok\"}"), "");
  EQS(feed("intraday/VOO", "{\"v\":1,\"sym\":\"VOO\",\"ts\":1789412345,\"n\":49,\"min\":690,\"max\":700,\"pts\":\"" + b64(ramp()) + "\"}"), "");
  CHECK(g_m.intra.have && g_m.intra.n == 49);
  EQS(feed("intraday/VOO", "{\"v\":1,\"sym\":\"VOO\",\"ts\":1789412345,\"n\":129,\"min\":690,\"max\":700,\"pts\":\"" + b64(ramp()) + "\"}"), "n");
  EQS(feed("intraday/VOO", "{\"v\":1,\"sym\":\"AAPL\",\"ts\":1789412345,\"n\":1,\"min\":690,\"max\":700,\"pts\":\"" + b64(ramp()) + "\"}"), "sym differs from the topic");
  // Only the ticker on screen: another symbol's session is refused whole.
  const uint8_t before = g_m.intra.n;
  EQS(feed("intraday/AAPL", "{\"v\":1,\"sym\":\"AAPL\",\"ts\":1789412345,\"n\":3,\"min\":1,\"max\":2,\"pts\":\"" + b64(ramp()) + "\"}"), "not the selected ticker");
  CHECK(g_m.intra.n == before && !strcmp(g_m.intra.sym, "VOO"));
}

static void testWatch() {
  memset(&g_m, 0, sizeof(g_m));
  watch({"A", "B", "C"}, {});
  EQS(feed("index/A/MAX", indexJson("sym", "\"A\"")), "");
  EQS(feed("index/C/MAX", indexJson("sym", "\"C\"")), "");
  watch({"C", "A"}, {});
  CHECK(g_m.nIdx == 2);
  EQS(g_m.idx[0].sym, "C"); EQS(g_m.idx[1].sym, "A");
  CHECK(g_m.idx[0].p[P_MAX].have && g_m.idx[1].p[P_MAX].have && !g_m.idx[2].sym[0]);
  const char *syms[] = {"D", "A", "E"};
  const char *names[] = {"DEE", nullptr, "E"};
  setWatch(g_m, g_sp, false, syms, names, 3);
  EQS(g_m.idx[0].sym, "D"); EQS(g_m.idx[0].name, "DEE"); EQS(g_m.idx[1].sym, "A"); EQS(g_m.idx[1].name, "A");
  CHECK(!g_m.idx[0].p[P_MAX].have && g_m.idx[1].p[P_MAX].have && !g_m.idx[2].p[P_MAX].have);
  watch({"E", "D", "A"}, {});
  EQS(g_m.idx[2].sym, "A");
  CHECK(g_m.idx[2].p[P_MAX].have);
  watch({}, {});
  CHECK(g_m.nIdx == 0 && !g_m.idx[0].sym[0]);
}

static void testRecord() {
  memset(&g_m, 0, sizeof(g_m));
  watch({"^GSPC"}, {"VOO"});
  EQS(feed("index/^GSPC/MAX", indexJson()), "");
  std::vector<uint8_t> buf(recordSize());
  CHECK(recordWrite(g_m, buf.data(), buf.size()) == buf.size());
  CHECK(recordWrite(g_m, buf.data(), buf.size() - 1) == 0);
  Model back;
  memset(&back, 0xAA, sizeof(back));
  CHECK(recordRead(buf.data(), buf.size(), &back) == nullptr);
  CHECK(!memcmp(&back, &g_m, sizeof(Model)));
  buf[100] ^= 1;
  EQS(recordRead(buf.data(), buf.size(), &back), "crc");
  buf[100] ^= 1;
  EQS(recordRead(buf.data(), buf.size() - 4, &back), "length");
  buf[0] = 'X';
  EQS(recordRead(buf.data(), buf.size(), &back), "magic");
  buf[0] = 'M';
  buf[4] = 9;
  EQS(recordRead(buf.data(), buf.size(), &back), "version");
  std::printf("  model %zu B, record %zu B\n", sizeof(Model), recordSize());
}

static View baseView() {
  View v;
  memset(&v, 0, sizeof(v));
  v.today = parseDate("2026-09-15");
  v.tapeOn = v.showPx = v.showBench = v.showSession = v.showValue = true;
  v.feedBadge = true;   // feed.label's default: the delay badge
  v.colors = CS_GREEN_RED;
  v.bandAbs = 5;
  v.bandRel = 25;
  v.staleDays = 3;
  v.preset = P_MAX;
  v.currency = "EUR";
  v.tickerSym = "VOO";
  v.nTapeNames = 2;
  v.tapeNames[0] = "NYSE";
  v.tapeNames[1] = "XETRA";
  return v;
}

static bool hasItem(const Rec &r, const char *role, const char *text) {
  for (const Item &i : r.items) if (i.role == role && i.text == text) return true;
  return false;
}

static void testLayout() {
  memset(&g_m, 0, sizeof(g_m));
  watch({"^GSPC", "^IXIC"}, {"VOO"});
  View v = baseView();
  PageInfo info;
  {
    Rec r;
    v.page = PG_MARKETS;
    drawPage(r, g_m, v, &info);
    CHECK(info.error && info.asof == kNoDate);
    CHECK(hasItem(r, "err", "DATA ERR") && hasItem(r, "err.why", "NOTHING STORED") && hasItem(r, "head.tag", "MAX"));
    EQS(r.notes["tape"], "NYSE --  XETRA --");
    CHECK(!hasItem(r, "asof", "AS OF --"));
  }
  EQS(feed("index/^GSPC/MAX", indexJson()), "");
  EQS(feed("index/^IXIC/MAX", indexJson("sym", "\"^IXIC\"")), "");
  EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"^GSPC\":{\"last\":7619.98,\"prev\":7656.98,\"day\":-0.0048,\"state\":\"OPEN\"}}}"), "");
  {
    Rec r;
    drawPage(r, g_m, v, &info);
    CHECK(!info.error && !info.stale && info.asof == parseDate("2026-09-14"));
    CHECK(hasItem(r, "pri.mn", "^GSPC") && hasItem(r, "pri.last", "7 620") && hasItem(r, "pri.chg", "+458%"));
    CHECK(hasItem(r, "live", "LIVE ") && hasItem(r, "live", "-0.5%") && hasItem(r, "asof", "AS OF 14 SEP"));
    CHECK(hasItem(r, "row.mn", "^IXIC"));
    // The sparkline: white pixels inside its box, none outside it on those rows.
    bool inside = false, outside = false;
    for (int y = MK_Y_SPARK; y < MK_Y_SPARK + MK_SPARK_H; y++)
      for (int x = 0; x < W; x++) {
        const bool white = r.px[y][x][0] == 248 && r.px[y][x][1] == 252;
        if (x >= MK_X_SPARK && x < MK_X_SPARK + MK_SPARK_W) inside = inside || white;
        else if (x > 76) outside = outside || white;
      }
    CHECK(inside && !outside);
  }
  {
    Rec r;
    v.primary = 1;                                        // the knob made the NASDAQ primary
    drawPage(r, g_m, v, &info);
    CHECK(hasItem(r, "pri.mn", "^IXIC") && hasItem(r, "row.mn", "^GSPC") && hasItem(r, "live", "CLOSE "));
    v.primary = 0;
  }
  {
    Rec r;
    v.today = parseDate("2026-09-21");
    drawPage(r, g_m, v, &info);
    CHECK(info.stale && hasItem(r, "asof", "STALE 14 SEP"));
    v.today = parseDate("2026-09-15");
  }
  {
    Rec r;
    v.page = PG_TICKER;
    drawPage(r, g_m, v, &info);
    CHECK(info.error && hasItem(r, "head.title", "VOO") && !hasItem(r, "head.cur", "USD"));
    EQS(feed("ticker/VOO/MAX", indexJson("sym", "\"VOO\"")), "");
    Rec r2;
    drawPage(r2, g_m, v, &info);
    CHECK(!info.error && hasItem(r2, "head.cur", "USD") && hasItem(r2, "big", "7 620") && hasItem(r2, "foot", "HI 7 799  LO 676"));   // 676.5 rounds half to even, as Python does
  }
  {
    Rec r;
    v.page = PG_PORTFOLIO;
    drawPage(r, g_m, v, &info);
    CHECK(info.error && hasItem(r, "head.cur", "EUR") && hasItem(r, "head.right", "HOLD  MAX"));
    EQS(feed("portfolio/hold/MAX", portfolioJson()), "");
    Rec r2;
    drawPage(r2, g_m, v, &info);
    CHECK(!info.error && hasItem(r2, "big", "89 801") && hasItem(r2, "chg", "+798%") && hasItem(r2, "foot", "DIV 8.8K  TER~ 171  CASH 0.6%"));
    CHECK(hasItem(r2, "asof", "14 SEP"));
    v.today = parseDate("2026-09-21");
    Rec r3;
    drawPage(r3, g_m, v, &info);
    CHECK(hasItem(r3, "foot", "DIV 8.8K  TER~ 171") && hasItem(r3, "asof", "STALE 14 SEP"));
    v.today = parseDate("2026-09-15");
  }
  {
    Rec r;
    v.page = PG_HOLDINGS;
    drawPage(r, g_m, v, &info);
    CHECK(info.error && hasItem(r, "head.title", "HOLDINGS 1/1"));
    EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"asof\":\"2026-09-14\",\"rows\":["
                              "{\"sym\":\"VOO\",\"tgt\":100,\"now\":99.4,\"ret\":5.96,\"entry\":\"2010-12-01\"}],\"cash\":{\"tgt\":0,\"now\":0.55}}"), "");
    Rec r2;
    drawPage(r2, g_m, v, &info);
    CHECK(!info.error && info.hdPages == 1 && hasItem(r2, "hd.sym", "VOO") && hasItem(r2, "hd.mid", "100>99%") &&
          hasItem(r2, "hd.ret", "+596%") && hasItem(r2, "hd.sym", "CASH") && hasItem(r2, "hd.mid", "0>1%"));
    v.hdPage = 7;                                         // past the end: the last page
    Rec r3;
    drawPage(r3, g_m, v, &info);
    CHECK(info.hdPage == 0);
  }
  {
    Rec r;
    drawStatusRow(r, P_5Y, kStandardPresets);
    CHECK(hasItem(r, "status", "WINDOW") && hasItem(r, "status", "5Y") && hasItem(r, "status", "MAX") && !hasItem(r, "status", "1M"));
    Rec r2;
    drawStatusRow(r2, P_1M, 0x7FF);   // every preset: the label goes, the list stays whole or nearly
    CHECK(hasItem(r2, "status", "1M") && !hasItem(r2, "status", "WINDOW"));
    CHECK(presetsAvailable(g_m) == kStandardPresets);
    EQS(feed("index/^GSPC/1M", indexJson("preset", "\"1M\"")), "");
    CHECK(presetsAvailable(g_m) == (kStandardPresets | (1u << P_1M)));
  }
  {
    // The honoured settings: blue/red and red-up colours, holdings by share, the band mark, the delayed badge.
    Rec r;
    v.page = PG_HOLDINGS;
    v.hdPage = 0;
    EQS(feed("holdings/hold", "{\"v\":1,\"mode\":\"hold\",\"asof\":\"2026-09-14\",\"rows\":["
                              "{\"sym\":\"AAA\",\"tgt\":10,\"now\":4,\"ret\":0.1},{\"sym\":\"BBB\",\"tgt\":50,\"now\":56,\"ret\":-0.2},"
                              "{\"sym\":\"CCC\",\"tgt\":40,\"now\":40,\"ret\":0.0}],\"cash\":{\"tgt\":0,\"now\":0}}"), "");
    drawPage(r, g_m, v, &info);
    CHECK(r.items[0].role != "hd.sym" || true);
    int aaa = -1, bbb = -1, ccc = -1;
    for (size_t i = 0; i < r.items.size(); i++) {
      if (r.items[i].role == "hd.sym" && r.items[i].text == "AAA") aaa = (int)i;
      if (r.items[i].role == "hd.sym" && r.items[i].text == "BBB") bbb = (int)i;
      if (r.items[i].role == "hd.sym" && r.items[i].text == "CCC") ccc = (int)i;
    }
    CHECK(aaa >= 0 && bbb > aaa && ccc > bbb);                  // the allocation's order
    bool amberMid = false;
    for (const Item &it : r.items) amberMid = amberMid || (it.role == "hd.mid" && it.col == C_AMBER);
    CHECK(!amberMid);                                           // no band mark by default
    v.sortByNow = true;
    v.bandMark = true;
    v.colors = CS_BLUE_RED;
    Rec r2;
    drawPage(r2, g_m, v, &info);
    int order[3] = {-1, -1, -1};
    int marks = 0, blue = 0, red = 0, green = 0;
    for (size_t i = 0; i < r2.items.size(); i++) {
      const Item &it = r2.items[i];
      if (it.role == "hd.sym") { if (it.text == "BBB") order[0] = (int)i; if (it.text == "CCC") order[1] = (int)i; if (it.text == "AAA") order[2] = (int)i; }
      if (it.role == "hd.mid" && it.col == C_AMBER) marks++;
      if (it.role == "hd.ret") { blue += it.col == C_BLUE; red += it.col == C_RED; green += it.col == C_GREEN; }
    }
    CHECK(order[0] >= 0 && order[1] > order[0] && order[2] > order[1]);   // by share: BBB 56, CCC 40, AAA 4
    CHECK(marks == 2);                                          // AAA: 6 pts off and 60 % off; BBB: 6 pts off
    CHECK(blue == 1 && red == 1 && green == 0);                 // +10 % blue, -20 % red, 0.0 % dim
    v.colors = CS_RED_UP;
    Rec r3;
    drawPage(r3, g_m, v, &info);
    red = green = 0;
    for (const Item &it : r3.items) if (it.role == "hd.ret") { red += it.col == C_RED; green += it.col == C_GREEN; }
    CHECK(red == 1 && green == 1);
    v.sortByNow = v.bandMark = false;
    v.colors = CS_GREEN_RED;
    // The delay badge (owner, 11:43): LIVE under 120 s behind the clock, else D and the whole minutes, dim.
    EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"^GSPC\":{\"last\":7619.98,\"prev\":7656.98,\"day\":-0.0048,\"state\":\"OPEN\",\"feed\":\"DELAYED\",\"delay_s\":900}}}"), "");
    CHECK(g_m.live.q[0].feed == F_DELAYED && g_m.live.q[0].delayS == 900);
    v.page = PG_MARKETS;
    Rec r4;
    drawPage(r4, g_m, v, &info);
    bool dimBadge = false;
    for (const Item &it : r4.items) dimBadge = dimBadge || (it.role == "live" && it.text == "D15 " && it.col == C_DIM);
    CHECK(dimBadge && !hasItem(r4, "live", "LIVE "));
    v.feedBadge = false;                                        // feed.label = live: LIVE whatever the delay
    Rec r5;
    drawPage(r5, g_m, v, &info);
    CHECK(hasItem(r5, "live", "LIVE ") && !hasItem(r5, "live", "D15 "));
    v.feedBadge = true;
    EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"^GSPC\":{\"last\":7619.98,\"day\":-0.0048,\"state\":\"OPEN\",\"delay_s\":119}}}"), "");
    Rec r5b;
    drawPage(r5b, g_m, v, &info);
    CHECK(hasItem(r5b, "live", "LIVE "));
    EQS(feed("live", "{\"v\":1,\"ts\":1789412345,\"q\":{\"^GSPC\":{\"last\":7619.98,\"day\":-0.0048,\"state\":\"OPEN\",\"feed\":\"DELAYED\"}}}"), "");
    Rec r5c;
    drawPage(r5c, g_m, v, &info);
    CHECK(hasItem(r5c, "live", "D "));                         // DELAYED without delay_s: the bare badge
    // The stale threshold from the settings.
    v.today = parseDate("2026-09-18");   // four weekdays after the 14th
    Rec r6;
    drawPage(r6, g_m, v, &info);
    CHECK(info.stale);
    v.staleDays = 5;
    Rec r7;
    drawPage(r7, g_m, v, &info);
    CHECK(!info.stale);
    v.staleDays = 3;
    v.today = parseDate("2026-09-15");
  }
  CHECK(curveX(0, 128, 0, 128) == 0 && curveX(0, 128, 127, 128) == 127 && curveX(79, 48, 127, 128) == 126);
  CHECK(curveY(31, 17, 0) == 47 && curveY(31, 17, 65535) == 31);
}

// ── the knob's settle ───────────────────────────────────────────────────────
static void testSettle() {
  const uint32_t settle = 2500;   // MARKET_NVS_SETTLE_MS
  KnobSettle st;
  st.clear();
  // Four detents through the tickers, 150 ms apart; loop() passes every 10 ms,
  // the knob's events handled before marketLoop() in the same pass.
  const uint32_t ticks[4] = {1000, 1150, 1300, 1450};
  int k = 0, saves = 0, pubs = 0;
  uint32_t pubAt = 0;
  for (uint32_t t = 0; t <= 10000; t += 10) {
    while (k < 4 && ticks[k] <= t) st.note(ticks[k++], true);
    const uint8_t a = st.tick(t, settle);
    if (a & SETTLE_SAVE) saves++;
    if (a & SETTLE_PUBLISH) { pubs++; pubAt = t; }
  }
  CHECK(saves == 1 && pubs == 1);
  CHECK(pubAt == 1450 + settle);                 // once, when the last detent has settled, not before
  // The same millisecond as the change: nothing (millis() | 1 would wrap here).
  st.note(100, false);
  CHECK(st.tick(100, settle) == 0 && st.tick(2599, settle) == 0 && st.tick(2600, settle) == SETTLE_SAVE);
  CHECK(st.tick(9000, settle) == 0);
  // The window alone: saved, never published.
  st.note(20000, false); st.note(20300, false); st.note(20600, false);
  CHECK(st.tick(23099, settle) == 0 && st.tick(23100, settle) == SETTLE_SAVE);
  // A ticker among window turns: one save and one publish.
  st.note(30000, false); st.note(30100, true); st.note(30200, false);
  CHECK(st.tick(32700, settle) == (SETTLE_SAVE | SETTLE_PUBLISH));
  // Across the millis() wrap.
  st.note(0xFFFFFF00u, true);
  CHECK(st.tick(0xFFFFFF00u + 100u, settle) == 0 && st.tick((uint32_t)(0xFFFFFF00u + 2500u), settle) == (SETTLE_SAVE | SETTLE_PUBLISH));
  // A portal save wrote and published everything: nothing left for later.
  st.note(40000, true);
  st.clear();
  CHECK(!st.pending() && st.tick(50000, settle) == 0);
}

// ── a reconnect inside one bus loop ─────────────────────────────────────────
static void testConnects() {
  ConnectWatch w;
  w.seen = 0;
  uint32_t connects = 0;           // mqttBusConnects()
  bool connected = false, wasUp = false;
  int forced = 0, forcedByEdge = 0;
  auto marketLoop = [&]() {        // what configTick() sees, and what the old edge saw
    const bool up = connected;
    if (w.fresh(connects)) forced++;
    if (up && !wasUp) forcedByEdge++;
    wasUp = up;
  };
  marketLoop();
  CHECK(forced == 0);
  connects = 1; connected = true;
  marketLoop();
  CHECK(forced == 1 && forcedByEdge == 1);
  marketLoop();
  CHECK(forced == 1);
  // The broker dropped the session, WiFiClient::connected() missed the FIN, and
  // mqttBusLoop() reconnected inside one call: connected before and after.
  connects = 2;
  marketLoop();
  CHECK(forced == 2 && forcedByEdge == 1);   // the counter forces the config; the edge did not
  connected = false;
  marketLoop();
  connects = 3; connected = true;
  marketLoop();
  CHECK(forced == 3 && forcedByEdge == 2);
}

// ── the window row on the drawdown stop ─────────────────────────────────────
static void testStatusOnDrawdown() {
  memset(&g_m, 0, sizeof(g_m));
  watch({"^GSPC"}, {"VOO"});
  View v = baseView();
  v.page = PG_PORTFOLIO;
  v.mddStop = true;
  PageInfo info;
  const std::string base = portfolioJson();
  EQS(feed("portfolio/hold/MAX", base.substr(0, base.size() - 1) +
           ",\"mdd_peak\":\"2020-01-01\",\"mdd_trough\":\"2020-03-01\",\"mdd_recovery\":\"2020-08-01\"}"), "");
  Rec plain, top, foot;
  drawPage(plain, g_m, v, &info);
  drawPage(top, g_m, v, &info);
  drawStatusRow(top, P_MAX, kStandardPresets, true);
  drawPage(foot, g_m, v, &info);
  drawStatusRow(foot, P_MAX, kStandardPresets, false);
  bool belowSame = true, footCovered = false, topText = false;
  for (int y = MK_Y_STATUS_TOP + MK_STATUS_H; y < H; y++)
    for (int x = 0; x < W; x++) belowSame = belowSame && !memcmp(plain.px[y][x], top.px[y][x], 3);
  for (int y = MK_Y_STATUS; y < H; y++)
    for (int x = 0; x < W; x++) footCovered = footCovered || memcmp(plain.px[y][x], foot.px[y][x], 3);
  for (const Item &it : top.items) topText = topText || (it.role == "status" && it.y == MK_Y_STATUS_TOP + 1);
  CHECK(belowSame && topText);   // at the top: the MDD footer and the bracket on row 57 untouched
  CHECK(footCovered);            // at the foot it would cover them
}

// ── the page switches after an update ───────────────────────────────────────
static void testPageMask() {
  const uint16_t all = (uint16_t)((1u << PANEL_KEY_COUNT) - 1);
  const uint16_t legacy = (uint16_t)((1u << PANEL_KEY_LUA) - 1);   // panel.cpp LEGACY_KNOWN_PAGES: CLOCK..CARDS
  const uint16_t clock = (uint16_t)(1u << PANEL_KEY_CLOCK);
  CHECK(PANEL_KEY_LUA == 6 && PANEL_KEY_MEDIA == 7 && PANEL_KEY_MARKET == 8 && all == 0x1FF && legacy == 0x3F);
  // No stamp: a mask from a build that knew at least CLOCK..CARDS.
  CHECK(panelPagesMigrate(0x003F, false, 0, all, legacy, clock) == 0x01FF);    // before Lua: Lua, media, market start on
  CHECK(panelPagesMigrate(0x003D, false, 0, all, legacy, clock) == 0x01FD);    // a page switched off then stays off
  CHECK(panelPagesMigrate(0x007F, false, 0, all, legacy, clock) == 0x01FF);    // a Lua build's mask
  // A stamp written against the stored mask: what that build knew.
  CHECK(panelPagesMigrate(0x00FD, true, panelPagesStamp(0x00FF, 0x00FD), all, legacy, clock) == 0x01FD);   // market starts on
  CHECK(panelPagesMigrate(0x007D, true, panelPagesStamp(0x00FF, 0x007D), all, legacy, clock) == 0x017D);   // media off, stays off
  CHECK(panelPagesMigrate(0x00FD, true, panelPagesStamp(0x01FF, 0x00FD), all, legacy, clock) == 0x00FD);   // market off here, stays off
  CHECK(panelPagesMigrate(0x0000, true, panelPagesStamp(0x01FF, 0x0000), all, legacy, clock) == 0x0001);   // the clock cannot be off
  CHECK(panelPagesMigrate(0xFFFF, true, 0xFFFFFFFFu, all, legacy, clock) == 0x01FF);                        // bits past this build's keys dropped

  // This firmware, then one that knows only CLOCK..MEDIA and no stamp, then this one again.
  struct Nvs { bool pages; uint16_t mask; bool stamp; uint32_t value; } nvs = {false, 0, false, 0};
  auto boot = [&](uint16_t *cur) {   // panelBegin()
    *cur = nvs.pages ? panelPagesMigrate(nvs.mask, nvs.stamp, nvs.value, all, legacy, clock) : (uint16_t)(all | clock);
  };
  auto save = [&](uint16_t cur) {    // panelTick(): pages, then the stamp against it
    nvs.pages = true; nvs.mask = cur;
    nvs.stamp = true; nvs.value = panelPagesStamp(all, cur);
  };
  auto olderSave = [&](uint16_t cur) {   // the older build: its own keys only, no stamp
    const uint16_t olderAll = (uint16_t)((1u << PANEL_KEY_MARKET) - 1);
    nvs.pages = true; nvs.mask = (uint16_t)((cur & olderAll) | clock);
  };
  uint16_t cur;
  boot(&cur);
  CHECK(cur == 0x01FF);
  cur = (uint16_t)(cur & ~(1u << PANEL_KEY_FLIGHTS));    // flights switched off here
  save(cur);
  // The older build boots, masks the market bit off as it reads, and the owner switches the yachts off there.
  uint16_t older = (uint16_t)(nvs.mask & ((1u << PANEL_KEY_MARKET) - 1));
  older = (uint16_t)(older & ~(1u << PANEL_KEY_YACHTS));
  olderSave(older);
  CHECK(nvs.mask == 0x00EB && nvs.value == panelPagesStamp(0x01FF, 0x01FB));   // the stamp still says market was known
  boot(&cur);
  CHECK(cur == 0x01EB);                                  // market back on, flights and yachts stay off
  // Without the stamp's copy of "pages", the stale stamp would have hidden the market pages.
  CHECK((uint16_t)((nvs.mask & (nvs.value >> 16)) | (all & ~(nvs.value >> 16)) | clock) == 0x00EB);
  save(cur);
  CHECK(nvs.value == panelPagesStamp(0x01FF, 0x01EB));
  // The older build again, but nothing changed there: the stamp still matches, and a market page
  // switched off here stays off.
  cur = (uint16_t)(cur & ~(1u << PANEL_KEY_MARKET));
  save(cur);
  boot(&cur);
  CHECK(cur == 0x00EB);
}

// ── the settings registry ───────────────────────────────────────────────────
static mks::Settings g_s, g_scratch, g_dflt;

static std::string applyJson(const std::string &json, bool *changed = nullptr) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "unparsable";
  char err[160] = "";
  const char *why = mks::apply(doc.as<JsonObjectConst>(), g_s, g_scratch, err, sizeof(err), changed);
  return why ? why : "";
}

static std::string payloadText() {
  JsonDocument doc;
  mks::payloadJson(g_s, doc.to<JsonObject>());
  std::string s;
  serializeJson(doc, s);
  return s;
}

static void testSettings() {
  using namespace mks;
  CHECK(poolFits());
  CHECK(!localDefaults() && checkLocalDefaults(g_scratch) == nullptr);   // this binary: -DMARKET_NO_LOCAL_DEFAULTS
  std::printf("  settings %zu B, pool %zu of %zu B, %d rows\n", sizeof(Settings), poolSize(), sizeof(Settings::pool), (int)I_COUNT);
  defaults(g_dflt);
  defaults(g_s);
  bool allDefault = true;
  for (int i = 0; i < I_COUNT; i++) {
    if (!isDefault(g_s, (Idx)i)) { allDefault = false; std::printf("  row %s does not read back as its default\n", kDefs[i].key); }
  }
  CHECK(allDefault);
  for (uint8_t p = 0; p < kPresets; p++) CHECK(!strcmp(optionName(I_WINDOW, p), kPresetKeys[p]));   // the two lists in step
  CHECK(windowPreset(g_s) == P_MAX);
  CHECK(presetMask(g_s) == kStandardPresets);
  CHECK(shownMode(g_s) == M_HOLD);
  EQS(str(g_s, I_TICKER), "VFINX");
  EQS(str(g_s, I_TICKERS), "VFINX=VFINX,VBMFX=VBMFX");
  CHECK(tickerListed(g_s));
  EQS(effectiveTicker(g_s), "VFINX");
  CHECK(number(g_s, I_CAPITAL) == 10000.0f && option(g_s, I_DWELL_S) == 20 && flag(g_s, I_TAPE_ON) && !flag(g_s, I_HD_BAND_MARK));
  Position pos[mks::kMaxPositions];
  const int n = parsePositions(str(g_s, I_POSITIONS), pos, mks::kMaxPositions);
  CHECK(n == 2);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += pos[i].w;
  CHECK(std::fabs(sum - 100.0) < 0.001);
  CHECK(pos[0].assetClass == AC_EQUITY && pos[1].assetClass == AC_BOND && pos[0].w == 60.0f && pos[1].w == 40.0f);
  EQS(pos[0].sym, "VFINX");
  EQS(pos[1].sym, "VBMFX");

  // The blob: written, read back the same; unknown keys and bad values skipped.
  static char blob[kBlobMax];
  const size_t len = blobWrite(g_s, blob, sizeof(blob));
  CHECK(len > 0 && len < kBlobMax);
  std::printf("  blob %zu B of %zu\n", len, (size_t)kBlobMax);
  EQS(applyJson("{\"portfolio.capital\":20000,\"rebal.mode\":\"bands\",\"tape.speed\":2,\"tape.exchanges\":[\"nyse\",\"lse\"]}"), "");
  const size_t len2 = blobWrite(g_s, blob, sizeof(blob));
  Settings back;
  CHECK(blobRead(blob, len2, back) == I_COUNT);
  bool same = true;
  for (int i = 0; i < I_COUNT; i++) same = same && sameValue(g_s, back, (Idx)i);
  CHECK(same && back.v[I_TAPE_SPEED].i == 2 && back.v[I_REBAL_MODE].i == 2);
  EQS(str(back, I_TAPE), "NYSE,LSE");
  const char *odd = "v=1\nno.such.key=1\ntape.speed=9\nportfolio.capital=abc\ndisplay.window=5Y\n";
  CHECK(blobRead(odd, strlen(odd), back) == 1);
  CHECK(back.v[I_TAPE_SPEED].i == 1 && back.v[I_CAPITAL].f == 10000.0f && windowPreset(back) == P_5Y && back.schema == mks::kSchema);

  // apply: whole or nothing, unknown keys into extra, errors naming the field.
  defaults(g_s);
  bool changed[I_COUNT];
  EQS(applyJson("{\"portfolio.capital\":20000,\"display.window\":\"1Y\",\"unknown.key\":{\"a\":1},\"tape.speed\":9}", changed), "tape.speed: must be 0 to 3");
  CHECK(number(g_s, I_CAPITAL) == 10000.0f);                  // nothing changed
  EQS(applyJson("{\"portfolio.capital\":20000,\"display.window\":\"1Y\",\"unknown.key\":{\"a\":1}}", changed), "");
  CHECK(changed[I_CAPITAL] && changed[I_WINDOW] && changed[I_EXTRA] && !changed[I_TICKER]);
  CHECK(windowPreset(g_s) == P_1Y);
  EQS(str(g_s, I_EXTRA), "{\"unknown.key\":{\"a\":1}}");
  EQS(applyJson("{\"other\":true}"), "");
  EQS(str(g_s, I_EXTRA), "{\"unknown.key\":{\"a\":1},\"other\":true}");
  EQS(applyJson("{\"extra\":{\"x\":1}}"), "");
  EQS(str(g_s, I_EXTRA), "{\"x\":1}");
  std::string big = "{\"k\":\"" + std::string(600, 'x') + "\"}";
  CHECK(applyJson(big).find("extra:") == 0);
  EQS(applyJson("{\"display.window\":\"2Y\"}"), "display.window: must be one of WTD|MTD|1M|3M|6M|YTD|1Y|3Y|5Y|10Y|MAX");
  EQS(applyJson("{\"portfolio.positions\":[{\"sym\":\"ABC\",\"w\":60},{\"sym\":\"XYZ\",\"w\":50}]}"),
      "portfolio.positions: the weights sum to 110.00 %, at most 100");
  EQS(applyJson("{\"portfolio.positions\":[{\"sym\":\"ABC\",\"w\":60},{\"sym\":\"ABC\",\"w\":10}]}"),
      "portfolio.positions: at most 16 positions, no symbol twice");
  EQS(applyJson("{\"portfolio.positions\":[{\"sym\":\"abc\",\"w\":60.005,\"entry\":\"2015-01-02\",\"asset_class\":\"equity\",\"proxy\":\"^gspc\"},{\"sym\":\"XYZ\",\"w\":10}]}"), "");
  EQS(str(g_s, I_POSITIONS), "ABC:60.00:2015-01-02:equity:^GSPC,XYZ:10.00");
  EQS(applyJson("{\"portfolio.positions\":[{\"sym\":\"ABC\",\"w\":0}]}"), "portfolio.positions: ABC needs a weight above 0 and at most 100");
  EQS(applyJson("{\"indices\":[\"^gspc\",{\"sym\":\"^IXIC\",\"name\":\"ndx\"}]}"), "");
  EQS(str(g_s, I_INDICES), "^GSPC=^GSPC,^IXIC=NDX");
  EQS(applyJson("{\"indices\":[1]}"), "indices: each sym must be up to 12 of A-Z 0-9 . ^ = -");
  EQS(applyJson("{\"ter\":{\"vwce.de\":0.22,\"AAPL\":0}}"), "");
  EQS(str(g_s, I_TER), "VWCE.DE=0.22,AAPL=0.00");
  EQS(applyJson("{\"ter\":{\"VOO\":11}}"), "ter: at most 16 symbols, 0 to 10 % a year");
  EQS(applyJson("{\"bench.blend\":[{\"sym\":\"^GSPC\",\"w\":60},{\"sym\":\"AGG\",\"w\":40}]}"), "");
  EQS(applyJson("{\"bench.blend\":[{\"sym\":\"^GSPC\",\"w\":60},{\"sym\":\"AGG\",\"w\":41}]}"), "bench.blend: at most 8 symbols, weights above 0 summing to at most 100");
  EQS(applyJson("{\"fetch.daily_at\":\"7:00\"}"), "fetch.daily_at: must be a time HH:MM");
  EQS(applyJson("{\"fetch.daily_at\":\"23:59\",\"portfolio.inception\":\"2026-02-30\"}"), "portfolio.inception: must be a date YYYY-MM-DD");
  EQS(applyJson("{\"display.ticker\":\"\"}"), "");                // empty: the first ticker
  EQS(effectiveTicker(g_s), "VFINX");
  EQS(applyJson("{\"display.ticker\":\"brk-b\"}"), "display.ticker: BRK-B is not in tickers");
  EQS(applyJson("{\"tickers\":[\"VFINX\",\"brk-b\"],\"display.ticker\":\"brk-b\"}"), "");
  EQS(str(g_s, I_TICKER), "BRK-B");
  bool moved[I_COUNT];
  EQS(applyJson("{\"tickers\":[\"VBMFX\",\"VFINX\"]}", moved), "");   // the list loses BRK-B: the first takes its place
  EQS(str(g_s, I_TICKER), "VBMFX");
  CHECK(moved[I_TICKER] && moved[I_TICKERS]);
  EQS(applyJson("{\"rebal.bands.abs_pts\":\"5\"}"), "rebal.bands.abs_pts: must be a number");
  EQS(applyJson("{\"tape.on\":1}"), "tape.on: must be true or false");
  EQS(applyJson("[1]"), "config must be an object");
  CHECK(setWindow(g_s, P_3Y) && windowPreset(g_s) == P_3Y && !setWindow(g_s, 11));
  CHECK(setTicker(g_s, "XYZ") && !setTicker(g_s, "bad sym"));
  EQS(effectiveTicker(g_s), "VBMFX");                              // the knob only picks listed ones; a stray one shows the first
  CHECK(!tickerListed(g_s));

  // The payload: v2, nested by path, the v1 fields, the extras spread, the presets.
  defaults(g_s);
  EQS(applyJson("{\"portfolio.capital\":20000,\"rebal.mode\":\"calendar\",\"presets.1m\":true,\"unknown.key\":7,\"contrib.amount\":100}"), "");
  {
    JsonDocument doc;
    mks::payloadJson(g_s, doc.to<JsonObject>());
    CHECK(doc["v"] == 2);
    CHECK(doc["portfolio"]["capital"] == 20000);
    CHECK(doc["portfolio"]["rebalance"] == true);
    CHECK(doc["portfolio"]["contrib"]["amount"] == 100);
    CHECK(doc["portfolio"]["contrib"]["every"] == "year");
    CHECK(doc["rebal"]["mode"] == "calendar");
    CHECK(doc["ticker"] == "VFINX");
    CHECK(doc["indices"][0]["sym"] == "^GSPC" && doc["indices"][0]["name"] == "SPX");
    CHECK(doc["tickers"].size() == 2);
    CHECK(doc["portfolio"]["positions"].size() == 2 && doc["portfolio"]["positions"][0]["asset_class"] == "equity" &&
          doc["portfolio"]["positions"][1]["asset_class"] == "bond" && doc["portfolio"]["positions"][0]["w"] == 60);
    CHECK(doc["tape"]["exchanges"].size() == 6);
    CHECK(doc["live"]["poll_s"] == 60 && doc["fetch"]["daily_at"] == "07:00" && doc["stale"]["history_days"] == 3);
    CHECK(doc["presets"].size() == 7 && doc["presets"][0] == "1M" && doc["presets"][1] == "YTD");
    CHECK(doc["unknown.key"] == 7);
    CHECK(doc["display"]["window"].isNull() && doc["extra"].isNull());   // the panel's own rows stay home
    CHECK(doc["display"]["scale"] == "linear");                          // and the app's display rows go
    const std::string text = payloadText();
    // The config gate: a panel-only row, or a ticker that comes back, gives the same bytes.
    ConfigGate gate;
    gate.clear();
    CHECK(gate.differs(text.size(), crc32((const uint8_t *)text.data(), text.size())));
    gate.sent(text.size(), crc32((const uint8_t *)text.data(), text.size()));
    EQS(applyJson("{\"display.window\":\"1Y\",\"tape.speed\":3,\"display.colors\":\"red_up\"}"), "");
    std::string again = payloadText();
    CHECK(again == text && !gate.differs(again.size(), crc32((const uint8_t *)again.data(), again.size())));
    EQS(applyJson("{\"display.ticker\":\"VBMFX\"}"), "");
    again = payloadText();
    CHECK(gate.differs(again.size(), crc32((const uint8_t *)again.data(), again.size())));
    EQS(applyJson("{\"display.ticker\":\"VFINX\"}"), "");
    again = payloadText();
    CHECK(!gate.differs(again.size(), crc32((const uint8_t *)again.data(), again.size())));
    EQS(applyJson("{\"portfolio.capital\":20001}"), "");
    again = payloadText();
    CHECK(gate.differs(again.size(), crc32((const uint8_t *)again.data(), again.size())));
    EQS(applyJson("{\"portfolio.capital\":20000,\"display.window\":\"MAX\",\"tape.speed\":1,\"display.colors\":\"green_red\"}"), "");
    std::printf("  config payload %zu B (default allocation, one extra key); streamed, ceiling %zu B\n", text.size(), (size_t)kConfigMax);
    CHECK(text.size() < kConfigMax);
  }
  {
    JsonDocument doc;
    JsonObject cfg = doc.to<JsonObject>();
    configJson(g_s, cfg);
    CHECK(cfg.size() == I_COUNT && cfg["display.window"] == "MAX" && cfg["extra"]["unknown.key"] == 7);
    JsonDocument reg;
    registryJson(reg.to<JsonObject>(), g_dflt);
    CHECK(reg["rows"].size() == I_COUNT && reg["groups"].size() == G_COUNT);
    CHECK(reg["rows"][0]["key"] == "indices" && reg["rows"][0]["def"][0]["name"] == "SPX");
    CHECK(reg["rows"][I_WINDOW]["opts"].size() == kPresets && reg["rows"][I_WINDOW]["panel"] == true);
    CHECK(reg["rows"][I_DWELL_S]["min"] == 5 && reg["rows"][I_DWELL_S]["max"] == 60);
  }
}

// Built with -DMARKET_LOCAL_DEFAULTS_FILE (tools/market/panel/local_defaults_example.h).
static int runLocal() {
  using namespace mks;
  CHECK(localDefaults());
  CHECK(checkLocalDefaults(g_scratch) == nullptr);   // probes on caller scratch: no heap
  defaults(g_s);
  EQS(defaultText(I_TICKERS), "VTI=VTI,VXUS=VXUS,BND=BND");
  EQS(defaultText(I_INDICES), kDefs[I_INDICES].def);               // a row the header does not list: neutral
  EQS(str(g_s, I_TICKERS), "VTI=VTI,VXUS=VXUS,BND=BND");
  EQS(str(g_s, I_TICKER), "VXUS");
  Position pos[mks::kMaxPositions];
  CHECK(parsePositions(str(g_s, I_POSITIONS), pos, mks::kMaxPositions) == 3);
  bool all = true;
  for (int i = 0; i < I_COUNT; i++) all = all && isDefault(g_s, (Idx)i);
  CHECK(all);
  EQS(applyJson("{\"tickers\":[\"VTI\"]}"), "");                  // moved away, then back to the local default
  CHECK(!isDefault(g_s, I_TICKERS));
  CHECK(!fromText(g_s, I_TICKERS, defaultText(I_TICKERS)) && isDefault(g_s, I_TICKERS));
  JsonDocument doc;
  payloadJson(g_s, doc.to<JsonObject>());
  CHECK(doc["portfolio"]["positions"].size() == 3 && doc["tickers"].size() == 3);
  JsonDocument reg;
  defaults(g_dflt);
  registryJson(reg.to<JsonObject>(), g_dflt);
  CHECK(reg["rows"][I_TICKER]["def"] == "VXUS");
  std::printf("  local defaults: %d checks, %d failed\n", g_pass + g_fail, g_fail);
  return g_fail ? 1 : 0;
}

// ── modes ───────────────────────────────────────────────────────────────────
// One query a line, one answer a line, flushed: the budget check keeps this open.
static int runMeasure() {
  std::string line;
  char b[64];
  while (std::getline(std::cin, line)) {
    const std::string rest = line.size() > 2 ? line.substr(2) : std::string();
    if (line.size() < 2) std::printf("?\n");
    else if (line[0] == 'p') std::printf("%d\n", picoInk(rest.c_str()));
    else if (line[0] == '5' && rest.size() >= 3) std::printf("%d\n", w5(rest.substr(4 <= rest.size() ? 4 : rest.size()).c_str(), (uint8_t)(rest[0] - '0'), rest[2] == '1'));
    else if (line[0] == 'v' || line[0] == 'a') {
      double val = 0;
      int slot = 0;
      std::sscanf(rest.c_str(), "%lf %d", &val, &slot);
      if (line[0] == 'v') fmtValue(b, sizeof(b), val, slot);
      else fmtAmount(b, sizeof(b), val, slot);
      std::printf("%s\n", b);
    } else std::printf("?\n");
    std::fflush(stdout);
  }
  return 0;
}

static int runFmt() {
  std::string line;
  char b[32];
  while (std::getline(std::cin, line)) {
    std::istringstream ss(line);
    std::string kind, arg;
    double v = 0;
    int slot = 7;
    ss >> kind;
    if (kind == "date") { ss >> arg; fmtDate(b, sizeof(b), parseDate(arg.c_str())); }
    else if (kind == "pct") { ss >> arg; if (arg == "none") fmtPct(b, sizeof(b), 0, false); else fmtPct(b, sizeof(b), atof(arg.c_str()), true); }
    else if (kind == "share") { ss >> v; fmtShare(b, sizeof(b), v); }
    else if (kind == "ym") { ss >> arg; fmtYm(b, sizeof(b), parseDate(arg.c_str())); }
    else if (kind == "value") { ss >> v >> slot; fmtValue(b, sizeof(b), v, slot); }
    else if (kind == "amount") { ss >> v >> slot; fmtAmount(b, sizeof(b), v, slot); }
    else { snprintf(b, sizeof(b), "?"); }
    std::printf("%s\n", b);
  }
  return 0;
}

static std::string jsonEsc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); o += t; }
    else o += c;
  }
  return o;
}

static int runLayout(const char *specPath, const char *ppm) {
  const std::string text = readFile(specPath);
  JsonDocument doc;
  if (deserializeJson(doc, text)) { std::fprintf(stderr, "spec: unparsable\n"); return 2; }
  memset(&g_m, 0, sizeof(g_m));
  std::vector<std::string> syms, names;
  std::vector<const char *> ps, pn;
  for (int t = 0; t < 2; t++) {
    syms.clear(); names.clear(); ps.clear(); pn.clear();
    for (JsonObjectConst o : doc["watch"][t ? "tickers" : "indices"].as<JsonArrayConst>()) {
      syms.push_back(o["sym"].as<const char *>());
      names.push_back(o["name"].isNull() ? "" : o["name"].as<const char *>());
    }
    for (size_t i = 0; i < syms.size(); i++) { ps.push_back(syms[i].c_str()); pn.push_back(names[i].c_str()); }
    setWatch(g_m, g_sp, t == 1, ps.data(), pn.data(), (uint8_t)ps.size());
  }
  std::vector<std::string> refused;
  const std::string specTicker = doc["view"]["tickerSym"] | "";
  for (JsonObjectConst p : doc["payloads"].as<JsonArrayConst>()) {
    const char *topic = p["topic"].as<const char *>();
    const char *why = ingest(g_m, g_sp, topic, p["json"].as<JsonObjectConst>(), specTicker.c_str());
    if (why) refused.push_back(std::string(topic) + ": " + why);
  }
  JsonObjectConst vj = doc["view"].as<JsonObjectConst>();
  View v = baseView();
  const int pg = presetIndex(vj["preset"] | "MAX");
  v.preset = (uint8_t)(pg < 0 ? P_MAX : pg);
  const char *page = vj["page"] | "markets";
  v.page = !strcmp(page, "ticker") ? PG_TICKER : (!strcmp(page, "portfolio") ? PG_PORTFOLIO : (!strcmp(page, "holdings") ? PG_HOLDINGS : PG_MARKETS));
  v.primary = vj["primary"] | 0;
  v.ticker = vj["ticker"] | 0;
  v.hdPage = vj["hdPage"] | 0;
  const int mode = modeIndex(vj["mode"] | "hold");
  v.mode = (uint8_t)(mode < 0 ? M_HOLD : mode);
  v.today = vj["today"].isNull() ? kNoDate : parseDate(vj["today"].as<const char *>());
  v.tapePhase = vj["tapePhase"] | 0;
  v.tapeOn = vj["tapeOn"] | true;
  v.showPx = vj["showPx"] | true;
  v.showBench = vj["showBench"] | true;
  v.showGross = vj["showGross"] | false;
  v.showSession = vj["showSession"] | true;
  v.showValue = vj["showValue"] | true;
  v.colors = vj["colors"] | 0;
  v.sortByNow = vj["sortByNow"] | false;
  v.bandMark = vj["bandMark"] | false;
  v.bandAbs = vj["bandAbs"] | 5.0f;
  v.bandRel = vj["bandRel"] | 25.0f;
  v.feedBadge = vj["feedBadge"] | true;
  v.mddStop = vj["mddStop"] | false;
  v.staleDays = vj["staleDays"] | 3;
  std::vector<std::string> tapeNames;
  for (JsonVariantConst n : vj["tapeNames"].as<JsonArrayConst>()) tapeNames.push_back(n.as<const char *>());
  v.nTapeNames = (uint8_t)(tapeNames.size() > kMaxTape ? kMaxTape : tapeNames.size());
  for (uint8_t i = 0; i < v.nTapeNames; i++) v.tapeNames[i] = tapeNames[i].c_str();
  std::string currency = vj["currency"] | "EUR", tickerSym = vj["tickerSym"] | "";
  v.currency = currency.c_str();
  v.tickerSym = tickerSym.c_str();

  Rec r;
  PageInfo info;
  drawPage(r, g_m, v, &info);
  if (!vj["statusRow"].isNull() && vj["statusRow"].as<bool>()) drawStatusRow(r, v.preset, presetsAvailable(g_m));
  if (ppm && !r.writePpm(ppm)) { std::fprintf(stderr, "cannot write %s\n", ppm); return 2; }

  std::string out = "{\"items\":[";
  for (size_t i = 0; i < r.items.size(); i++) {
    const Item &it = r.items[i];
    char t[64];
    snprintf(t, sizeof(t), "\"x\":%d,\"y\":%d,\"k\":%d,\"col\":%d,\"thin\":%s", it.x, it.y, it.k, it.col, it.thin ? "true" : "false");
    out += (i ? "," : "") + std::string("{\"role\":\"") + it.role + "\",\"font\":\"" + it.font + "\",\"text\":\"" + jsonEsc(it.text) + "\"," + t + "}";
  }
  out += "],\"notes\":{";
  bool first = true;
  for (auto &kv : r.notes) { out += (first ? "" : ",") + std::string("\"") + kv.first + "\":\"" + jsonEsc(kv.second) + "\""; first = false; }
  char d[8];
  fmtDate(d, sizeof(d), info.asof);
  char infoBuf[160];
  snprintf(infoBuf, sizeof(infoBuf), "},\"info\":{\"asof\":\"%s\",\"stale\":%s,\"error\":%s,\"hdPages\":%d,\"hdPage\":%d,\"gen\":%lu},\"refused\":[",
           info.asof == kNoDate ? "" : d, info.stale ? "true" : "false", info.error ? "true" : "false", info.hdPages, info.hdPage,
           (unsigned long)g_m.gen);
  out += infoBuf;
  for (size_t i = 0; i < refused.size(); i++) out += (i ? "," : "") + std::string("\"") + jsonEsc(refused[i]) + "\"";
  out += "]}\n";
  std::fputs(out.c_str(), stdout);
  return 0;
}

static int runParse(const char *leaf, const char *path) {
  memset(&g_m, 0, sizeof(g_m));
  // An index or ticker payload needs its symbol watched: take it from the topic.
  char part[3][kSymLen + 4] = {{0}};
  int n = 0, k = 0;
  for (const char *p = leaf; *p && n < 3; p++) {
    if (*p == '/') { part[n][k] = 0; n++; k = 0; }
    else if (k < (int)sizeof(part[0]) - 1) part[n][k++] = *p;
  }
  part[n][k] = 0;
  if (n == 2 && (!strcmp(part[0], "index") || !strcmp(part[0], "ticker"))) {
    const char *s[] = {part[1]};
    setWatch(g_m, g_sp, part[0][0] == 't', s, nullptr, 1);
  }
  if (n == 1 && !strcmp(part[0], "intraday")) g_ticker = part[1];   // a sample is for the ticker it names
  const std::string r = feed(leaf, readFile(path));
  if (r.empty()) { std::printf("OK\n"); return 0; }
  std::printf("REFUSED %s\n", r.c_str());
  return 1;
}

// The registry and the default config as /api/market sends them (the mock portal).
static int runWebJson() {
  mks::defaults(g_s);
  JsonDocument doc;
  JsonObject out = doc.to<JsonObject>();
  mks::configJson(g_s, out["config"].to<JsonObject>());
  mks::registryJson(out["registry"].to<JsonObject>(), g_s);
  mks::payloadJson(g_s, out["payload"].to<JsonObject>());
  serializeJson(doc, std::cout);
  std::cout << "\n";
  return 0;
}

// A POST's config over a base config, through mks::apply: {"ok":true,"config":{..},"payload":{..}} or {"ok":false,"error":".."}.
static int runApply(const char *basePath, const char *inPath) {
  mks::defaults(g_s);
  char err[160] = "";
  JsonDocument base, in, out;
  if (deserializeJson(base, readFile(basePath)) || deserializeJson(in, readFile(inPath))) { std::printf("{\"ok\":false,\"error\":\"unparsable\"}\n"); return 2; }
  if (mks::apply(base.as<JsonObjectConst>(), g_s, g_scratch, err, sizeof(err), nullptr)) { std::printf("{\"ok\":false,\"error\":\"base: %s\"}\n", err); return 2; }
  const char *why = mks::apply(in.as<JsonObjectConst>(), g_s, g_scratch, err, sizeof(err), nullptr);
  JsonObject o = out.to<JsonObject>();
  o["ok"] = why == nullptr;
  if (why) o["error"] = err;
  mks::configJson(g_s, o["config"].to<JsonObject>());
  mks::payloadJson(g_s, o["payload"].to<JsonObject>());
  serializeJson(out, std::cout);
  std::cout << "\n";
  return why ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "local")) return runLocal();
  if (argc >= 2 && !strcmp(argv[1], "measure")) return runMeasure();
  if (argc >= 2 && !strcmp(argv[1], "webjson")) return runWebJson();
  if (argc >= 4 && !strcmp(argv[1], "apply")) return runApply(argv[2], argv[3]);
  if (argc >= 2 && !strcmp(argv[1], "fmt")) return runFmt();
  if (argc >= 3 && !strcmp(argv[1], "layout")) return runLayout(argv[2], argc >= 4 ? argv[3] : nullptr);
  if (argc >= 4 && !strcmp(argv[1], "parse")) return runParse(argv[2], argv[3]);
  testDates();
  testNumbers();
  testFonts();
  testBase64();
  testSeries();
  testPortfolio();
  testHoldings();
  testLiveTapeStatus();
  testWatch();
  testRecord();
  testLayout();
  testSettle();
  testConnects();
  testStatusOnDrawdown();
  testPageMask();
  testSettings();
  std::printf("  %d checks, %d failed\n", g_pass + g_fail, g_fail);
  return g_fail ? 1 : 0;
}

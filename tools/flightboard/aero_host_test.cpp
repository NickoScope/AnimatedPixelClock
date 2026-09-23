// Host test for src/flightboard/aero_transform.cpp and fb_settings.h. Built and
// run by tools/flightboard/check_aero.py; not part of any firmware image.
//
//   aero_host_test <samples dir>
//
// Prints FAIL lines, one "BOARD arr|dep {...}" line per direction for the
// Python reference to compare, "SIZE" lines, and "RESULT <failures>" last.

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "aero_transform.h"
#include "fb_settings.h"
#include "fb_zone.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { g_fail++; std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)
#define CHECK_EQ(a, b) do { if ((long long)(a) != (long long)(b)) { g_fail++; std::printf("FAIL %s:%d  %s = %lld, want %lld\n", __FILE__, __LINE__, #a, (long long)(a), (long long)(b)); } } while (0)

struct Counting : ArduinoJson::Allocator {
  size_t used = 0, peak = 0;
  union Hdr { size_t size; max_align_t a; };
  void *allocate(size_t n) override { return reallocate(nullptr, n); }
  void deallocate(void *p) override {
    if (!p) return;
    Hdr *h = static_cast<Hdr *>(p) - 1;
    used -= h->size;
    std::free(h);
  }
  void *reallocate(void *p, size_t n) override {
    Hdr *o = p ? static_cast<Hdr *>(p) - 1 : nullptr;
    const size_t was = o ? o->size : 0;
    Hdr *h = static_cast<Hdr *>(std::realloc(o, sizeof(Hdr) + n));
    h->size = n;
    used = used - was + n;
    if (used > peak) peak = used;
    return h + 1;
  }
};

static std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static const char *stKey(uint8_t st) {
  static const char *k[] = {"sched", "board", "dep", "land", "delay", "canc", ""};
  return k[st < 7 ? st : 6];
}

static const char *trkKey(uint8_t st) {
  static const char *k[] = {"WAIT", "NOTFOUND", "SCHED", "DELAYED", "TAXI", "ENROUTE", "LANDED", "CANCELLED", "DIVERTED"};
  return st < FB_TRK_COUNT ? k[st] : "?";
}

// One list, parsed with the filter and without: both must give the same result.
static void parse(const std::string &dir, aero::List l, aero::ListResult *out) {
  const std::string body = slurp(dir + "/aero_" + aero::listName(l) + ".json");
  Counting a;
  JsonDocument filter, doc(&a), plain;
  aero::listFilter(filter, l);
  CHECK(!deserializeJson(doc, body, DeserializationOption::Filter(filter)));
  CHECK(aero::parseList(doc.as<JsonVariantConst>(), l, out));
  CHECK(!deserializeJson(plain, body));
  aero::ListResult unfiltered;
  CHECK(aero::parseList(plain.as<JsonVariantConst>(), l, &unfiltered));
  CHECK(!memcmp(out, &unfiltered, sizeof(unfiltered)));
  std::printf("SIZE %s bytes=%zu records=%u filtered_peak=%zu\n", aero::listName(l), body.size(), out->seen, a.peak);
}

static void printBoard(const char *dir, const aero::Board &b) {
  JsonDocument d;
  d["count"] = b.count;
  d["now_idx"] = b.nowIdx;
  JsonArray rows = d["rows"].to<JsonArray>();
  for (uint8_t i = 0; i < b.count; i++) {
    JsonObject r = rows.add<JsonObject>();
    r["fn"] = b.rows[i].fn;
    r["ct"] = b.rows[i].ct;
    r["cy"] = b.rows[i].cy;
    r["st"] = stKey(b.rows[i].st);
    r["t"] = (long long)b.rows[i].t;
  }
  std::string s;
  serializeJson(d, s);
  std::printf("BOARD %s %s\n", dir, s.c_str());
}

static void boards(const std::string &dir, int64_t now) {
  aero::ListResult lr[aero::LIST_COUNT];
  for (uint8_t l = 0; l < aero::LIST_COUNT; l++) parse(dir, (aero::List)l, &lr[l]);

  // The rules each record in gen_aero_fixture.py is there for.
  CHECK(lr[aero::ARR_PAST].more);
  CHECK(!lr[aero::ARR_NEXT].more);
  CHECK_EQ(lr[aero::ARR_PAST].seen, 7);
  CHECK_EQ(lr[aero::ARR_PAST].noIdent, 1);
  CHECK_EQ(lr[aero::ARR_PAST].noTime, 1);

  aero::Board arr, dep;
  aero::buildBoard(&lr[aero::ARR_PAST], &lr[aero::ARR_NEXT], now, &arr);
  aero::buildBoard(&lr[aero::DEP_PAST], &lr[aero::DEP_NEXT], now, &dep);
  CHECK_EQ(arr.dup, 1);
  CHECK_EQ(arr.outside, 2);                     // SWR560 at -130, SAS4371 at +200
  bool sawDlh = false, sawLtz = false, sawLot = false;
  for (uint8_t i = 0; i < arr.count; i++) {
    const aero::Cand &c = arr.rows[i];
    if (!strcmp(c.fn, "LH1234")) { sawDlh = true; CHECK_EQ(c.st, FB_LAND); CHECK_EQ(c.t, now - 600); }
    if (!strcmp(c.fn, "MYJ12"))  { sawLtz = true; CHECK_STR(c.ct, "LFTZ"); CHECK_STR(c.cy, "LA MOLE"); }
    if (!strcmp(c.fn, "LO415"))  { sawLot = true; CHECK_STR(c.cy, "KRAKOW"); }
    if (!strcmp(c.fn, "IB3456")) CHECK_EQ(c.st, FB_LAND);      // actual_on only
    if (!strcmp(c.fn, "VY1500")) CHECK_EQ(c.st, FB_DELAY);
    if (!strcmp(c.fn, "U24410")) CHECK_EQ(c.st, FB_CANC);
    if (!strcmp(c.fn, "KL1263")) CHECK_EQ(c.st, FB_BOARD);
    if (!strcmp(c.fn, "AF7302")) CHECK_EQ(c.st, FB_DEP);
    if (i) CHECK(arr.rows[i - 1].t <= c.t);
  }
  CHECK(sawDlh && sawLtz && sawLot);
  CHECK_EQ(dep.dup, 1);                          // NOID1, keyed on ident|scheduled_out
  CHECK_EQ(dep.count, FB_MAX_ROWS);              // 24 in the window, fifteen kept
  CHECK_EQ(dep.nowIdx, aero::kHalf);
  printBoard("arr", arr);
  printBoard("dep", dep);

  // A board from one list only, and from nothing.
  aero::Board only, none;
  aero::buildBoard(nullptr, &lr[aero::ARR_NEXT], now, &only);
  CHECK(only.count > 0);
  aero::buildBoard(nullptr, nullptr, now, &none);
  CHECK_EQ(none.count, 0);
  CHECK_EQ(none.nowIdx, 0);
}

static void tracks(const std::string &dir) {
  JsonDocument all;
  CHECK(!deserializeJson(all, slurp(dir + "/aero_tracks.json")));
  const int64_t now = all["now"].as<long long>();
  for (JsonPairConst kv : all["cases"].as<JsonObjectConst>()) {
    JsonObjectConst c = kv.value(), e = c["expect"];
    std::string body;
    serializeJson(c["answer"], body);
    JsonDocument filter, doc;
    aero::trackFilter(filter);
    CHECK(!deserializeJson(doc, body, DeserializationOption::Filter(filter)));
    aero::Track t;
    const bool ok = aero::pickTrack(doc.as<JsonVariantConst>(), now, c["added"].as<long long>(), &t);
    const char *name = kv.key().c_str();
    int before = g_fail;
    CHECK(ok);
    CHECK_STR(trkKey(t.state), e["state"].as<const char *>());
    CHECK_STR(t.fn, e["fn"].as<const char *>());
    CHECK_STR(t.from, e["frm"].as<const char *>());
    CHECK_STR(t.to, e["to"].as<const char *>());
    CHECK_EQ(t.current, e["current"].as<bool>());
    CHECK_EQ(t.flights, e["flights"].as<int>());
    CHECK_EQ(aero::trackNextS(t, now), e["next_s"].as<long long>());
    CHECK_EQ(aero::trackExpiresAt(t), e["expires"].as<long long>());
    CHECK_EQ(aero::trackShownTime(t), e["shown"].as<long long>());
    CHECK_EQ(aero::trackDelayMin(t), e["delay_min"].as<int>());
    if (!e["arrival"].isNull()) CHECK_EQ(aero::trackShowsArrival(t), e["arrival"].as<bool>());
    if (!e["from_tz"].isNull()) CHECK_STR(t.fromTz, e["from_tz"].as<const char *>());
    if (!e["to_tz"].isNull()) CHECK_STR(t.toTz, e["to_tz"].as<const char *>());
    if (g_fail != before) std::printf("  ^ in track case %s\n", name);
  }
  JsonDocument bad;
  deserializeJson(bad, "{\"links\":null}");
  aero::Track t;
  CHECK(!aero::pickTrack(bad.as<JsonVariantConst>(), now, now, &t));
}

static void units() {
  int64_t t = 0;
  CHECK(aero::parseTime("2026-09-14T18:00:00Z", &t) && t == 1789408800);
  CHECK(aero::parseTime("2026-09-14T20:00:00+02:00", &t) && t == 1789408800);
  CHECK(aero::parseTime("2026-09-14T18:00:00.123Z", &t) && t == 1789408800);
  CHECK(!aero::parseTime("2026-09-14T18:00:00", &t));        // no zone: refused
  CHECK(!aero::parseTime("2026-09-14", &t));
  CHECK(!aero::parseTime("", &t));
  CHECK(!aero::parseTime(nullptr, &t));
  char buf[24];
  aero::formatTime(1789408800, buf, sizeof(buf));
  CHECK_STR(buf, "2026-09-14T18:00:00Z");

  char q[160];
  CHECK(aero::listQuery("LFMN", aero::ARR_NEXT, 1789408800, q, sizeof(q)));
  CHECK_STR(q, "/airports/LFMN/flights/scheduled_arrivals?type=Airline&max_pages=1"
               "&start=2026-09-14T16:00:00Z&end=2026-09-14T20:00:00Z");
  CHECK(aero::listQuery("LFMN", aero::DEP_PAST, 1789408800, q, sizeof(q)));
  CHECK_STR(q, "/airports/LFMN/flights/departures?type=Airline&max_pages=1&start=2026-09-14T16:00:00Z");
  CHECK(aero::listQuery("EGLL", aero::ARR_PAST, 0, q, sizeof(q)));
  CHECK_STR(q, "/airports/EGLL/flights/arrivals?type=Airline&max_pages=1");
  CHECK(!aero::listQuery("LFMN", aero::ARR_NEXT, 1789408800, q, 40));
  CHECK(aero::trackQuery("AF7301", 1789408800, q, sizeof(q)));
  CHECK_STR(q, "/flights/AF7301?ident_type=designator&max_pages=1&start=2026-09-13T18:00:00Z&end=2026-09-16T17:00:00Z");

  char city[FB_CY_LEN];
  aero::cleanCity("Paris (Orly)", city, sizeof(city));            CHECK_STR(city, "PARIS");
  aero::cleanCity("Bordeaux/Merignac", city, sizeof(city));       CHECK_STR(city, "BORDEAUX");
  aero::cleanCity("Zürich", city, sizeof(city));                  CHECK_STR(city, "ZURICH");
  aero::cleanCity("Łódź", city, sizeof(city));                    CHECK_STR(city, "LODZ");
  aero::cleanCity("  São  Paulo ", city, sizeof(city));           CHECK_STR(city, "SAO PAULO");
  aero::cleanCity("東京", city, sizeof(city));                     CHECK_STR(city, "");
  aero::cleanCity("Frankfurt am Main", city, sizeof(city));       CHECK_STR(city, "FRANKFURT AM M");   // cut to the column's 14
  aero::cleanCity(nullptr, city, sizeof(city));                   CHECK_STR(city, "");
  // Every letter of U+00C0..U+017F, for check_aero.py to hold against Python.
  std::string latin;
  for (unsigned cp = 0xC0; cp <= 0x17F; cp++) {
    latin += (char)(0xC0 | (cp >> 6));
    latin += (char)(0x80 | (cp & 0x3F));
  }
  char folded[400];
  aero::cleanCity(latin.c_str(), folded, sizeof(folded));
  std::printf("FOLD %s\n", folded);

  char id[FB_IDENT_LEN];
  CHECK(aero::normaliseIdent("af 7301", id, sizeof(id)));  CHECK_STR(id, "AF7301");
  CHECK(aero::normaliseIdent("BA336", id, sizeof(id)));    CHECK_STR(id, "BA336");
  CHECK(aero::normaliseIdent("AFR7301", id, sizeof(id)));  CHECK_STR(id, "AFR7301");
  CHECK(aero::normaliseIdent("U21234", id, sizeof(id)));   CHECK_STR(id, "U21234");
  CHECK(aero::normaliseIdent("9W12", id, sizeof(id)));
  CHECK(aero::normaliseIdent("BA2490A", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("12345", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("AF", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("AF12345", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("AF-7301", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("F-HBNA", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("AFR73011", id, sizeof(id)));
  CHECK(!aero::normaliseIdent("", id, sizeof(id)));
}

static int width4(const char *s) {   // 4 px a letter, counted in letters as the panel counts them
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; n++) utf8Next(&p);
  return 4 * n;
}
static bool known(const char *iana) { return tzdbPosix(iana) != nullptr; }

// ── airport-local time ──────────────────────────────────────────────────────
static const char *listed(const char *code) {
  if (!strcmp(code, "NCE") || !strcmp(code, "LFMN")) return "Europe/Paris";
  if (!strcmp(code, "XXX")) return "Mars/Olympus_Mons";          // a listed airport with a zone the table lacks
  return nullptr;
}

static void zones() {
  // Reference values from Python's zoneinfo (tzdata), 2026: the EU changes at
  // 01:00 UTC on 29 March and 25 October, the US at 07:00 UTC on 8 March and
  // 06:00 UTC on 1 November; Phoenix and Kolkata keep one offset.
  struct Want { const char *zone; int64_t utc; const char *hm; int32_t off; };
  static const Want kWant[] = {
    {"Europe/London",    1774745999, "00:59",     0}, {"Europe/Paris", 1774745999, "01:59",  3600},
    {"Europe/London",    1774746000, "02:00",  3600}, {"Europe/Paris", 1774746000, "03:00",  7200},
    {"Europe/London",    1792889999, "01:59",  3600}, {"Europe/Paris", 1792889999, "02:59",  7200},
    {"Europe/London",    1792890000, "01:00",     0}, {"Europe/Paris", 1792890000, "02:00",  3600},
    {"America/New_York", 1772953140, "01:59", -18000}, {"America/New_York", 1772953200, "03:00", -14400},
    {"America/New_York", 1793512740, "01:59", -14400}, {"America/New_York", 1793512800, "01:00", -18000},
    {"America/Phoenix",  1793512740, "22:59", -25200}, {"America/Phoenix",  1793512800, "23:00", -25200},
    {"Asia/Kolkata",     1789408800, "23:30",  19800}, {"Asia/Tokyo",       1789408800, "03:00",  32400},
  };
  for (const Want &w : kWant) {
    fbz::Zone z;
    CHECK(fbz::load(w.zone, &z));
    char hm[6];
    fbz::hm(w.utc, z, hm, sizeof(hm));
    if (strcmp(hm, w.hm) || fbz::offset(z, w.utc) != w.off)
      std::printf("  ^ %s at %lld: %s %d, want %s %d\n", w.zone, (long long)w.utc, hm, fbz::offset(z, w.utc), w.hm, w.off);
    CHECK_STR(hm, w.hm);
    CHECK_EQ(fbz::offset(z, w.utc), w.off);
  }
  // The grid check_aero.py holds against zoneinfo: every hour and the second
  // before it, 2026-01-01 to 2028-01-01, in zones with and without summer time.
  static const char *kGrid[] = {"Europe/London", "Europe/Paris", "America/New_York", "America/Phoenix",
                                "Asia/Kolkata", "Australia/Lord_Howe", "America/Sao_Paulo"};
  for (const char *name : kGrid) {
    fbz::Zone z;
    CHECK(fbz::load(name, &z));
    for (int64_t t = 1767225600; t < 1830297600; t += 3600)
      for (int64_t s : {t - 1, t}) {
        char hm[6];
        fbz::hm(s, z, hm, sizeof(hm));
        std::printf("ZONE %s %lld %s %d\n", name, (long long)s, hm, fbz::offset(z, s));
      }
  }
  fbz::Zone none;
  CHECK(!fbz::load("", &none) && !fbz::load(nullptr, &none) && !fbz::load("Etc/GMT+5", &none));
  char hm[6];
  fbz::hm(1789408800, none, hm, sizeof(hm));   CHECK_STR(hm, "18:00");        // unknown: UTC
  fbz::hm(0, none, hm, sizeof(hm));            CHECK_STR(hm, "--:--");

  char cue[8];
  fbz::cue(3600, 7200, cue, sizeof(cue));      CHECK_STR(cue, "-1");          // London from a Paris panel, summer
  fbz::cue(-14400, 7200, cue, sizeof(cue));    CHECK_STR(cue, "-6");          // New York
  fbz::cue(32400, 7200, cue, sizeof(cue));     CHECK_STR(cue, "+7");          // Tokyo
  fbz::cue(19800, 7200, cue, sizeof(cue));     CHECK_STR(cue, "+3:30");       // Kolkata
  fbz::cue(-34200, 0, cue, sizeof(cue));       CHECK_STR(cue, "-9:30");       // Marquesas from UTC
  fbz::cue(7200, 7200, cue, sizeof(cue));      CHECK_STR(cue, "");
  CHECK_EQ(fbz::offsetOfLocal(2026, 9, 14, 20, 0, 0, 1789408800), 7200);
  CHECK_EQ(fbz::offsetOfLocal(2026, 9, 14, 14, 0, 0, 1789408800), -14400);     // New York, same day
  CHECK_EQ(fbz::offsetOfLocal(2026, 9, 15, 3, 0, 0, 1789408800), 32400);       // Tokyo, the next day
  CHECK_EQ(fbz::offsetOfLocal(2026, 9, 13, 23, 0, 0, 1789340400), 0);         // UTC, the day before
  CHECK_EQ(fbz::offsetOfLocal(2026, 9, 13, 22, 0, 0, 1789408800), -72000);     // a local date a day behind

  fbz::Zone z;
  CHECK_EQ(fbz::pick("Europe/London", "LHR", listed, &z), fbz::SRC_SENT);
  CHECK_EQ(fbz::pick("", "NCE", listed, &z), fbz::SRC_LIST);
  CHECK_EQ(fbz::offset(z, 1789408800), 7200);
  CHECK_EQ(fbz::pick("Mars/Olympus_Mons", "LFMN", listed, &z), fbz::SRC_LIST);   // a name the table lacks falls through
  CHECK_EQ(fbz::pick(nullptr, "LFTZ", listed, &z), fbz::SRC_UTC);
  CHECK(!z.known);
  CHECK_EQ(fbz::pick("", "XXX", listed, &z), fbz::SRC_UTC);                     // listed, but its zone unknown
  CHECK_EQ(fbz::pick(nullptr, nullptr, nullptr, &z), fbz::SRC_UTC);
}
static int width8(const char *s) { return 8 * (int)strlen(s); }

static void settings() {
  fbs::Budget b = fbs::defaults();
  CHECK(fbs::valid(b));
  char err[96];
  JsonDocument in;
  deserializeJson(in, "{\"floor_min\":30,\"day_cap\":50,\"month_cap\":1200}");
  CHECK(fbs::apply(in.as<JsonObjectConst>(), b, err, sizeof(err)) == nullptr);
  CHECK(b.floorMin == 30 && b.dayCap == 50 && b.monthCap == 1200);
  const fbs::Budget kept = b;
  for (const char *bad : {"{\"floor_min\":4}", "{\"floor_min\":241}", "{\"day_cap\":1001}", "{\"day_cap\":-1}",
                          "{\"month_cap\":20001}", "{\"day_cap\":1.5}", "{\"day_cap\":\"30\"}", "{\"day_cap\":true}",
                          "{\"floor_min\":30,\"nope\":1}"}) {
    JsonDocument d;
    deserializeJson(d, bad);
    fbs::Budget t = b;
    const char *why = fbs::apply(d.as<JsonObjectConst>(), t, err, sizeof(err));
    CHECK(why != nullptr);
    if (why) CHECK(strncmp(why, "budget.", 7) == 0);
    CHECK(t.floorMin == kept.floorMin && t.dayCap == kept.dayCap && t.monthCap == kept.monthCap);
  }
  JsonDocument off;
  deserializeJson(off, "{\"day_cap\":0}");
  CHECK(fbs::apply(off.as<JsonObjectConst>(), b, err, sizeof(err)) == nullptr);

  // Counters: a new UTC day resets the day, a new month the month; no clock, no call.
  fbs::Usage u{};
  const fbs::Budget cap = {15, 10, 25};
  CHECK(!fbs::roll(u, 0));
  CHECK(fbs::roll(u, 1789408800));                          // 2026-09-14
  CHECK_EQ(u.month, 2026 * 12 + 8);
  u.dayN = 10; u.monthN = 20;
  CHECK_STR(fbs::refuse(u, cap, false, false), "DAY CAP");
  CHECK(fbs::roll(u, 1789408800 + 6 * 3600));                // 00:00 UTC on the 15th
  CHECK_EQ(u.dayN, 0);
  CHECK_EQ(u.monthN, 20);
  CHECK(fbs::refuse(u, cap, true, false) == nullptr);
  u.dayN = 8;
  CHECK_STR(fbs::refuse(u, cap, true, true), "DAY CAP");     // the last fifth is the trackers'
  CHECK(fbs::refuse(u, cap, false, true) == nullptr);
  CHECK_EQ(fbs::boardDayCap(cap, true), 8);
  CHECK_EQ(fbs::boardDayCap({15, 3, 25}, true), 2);          // at least one kept
  CHECK_EQ(fbs::boardDayCap({15, 1, 25}, true), 0);
  u.monthN = 25;
  CHECK_STR(fbs::refuse(u, cap, false, false), "MONTH CAP");
  CHECK_STR(fbs::refuse(u, {15, 0, 25}, false, false), "OFF");
  CHECK(fbs::roll(u, 1790812800));                           // 2026-10-01 00:00 UTC
  CHECK_EQ(u.monthN, 0);
  CHECK_EQ(u.month, 2026 * 12 + 9);
  CHECK_EQ(fbs::monthOf(1790812799), 2026 * 12 + 8);         // a second earlier: still September

  // Custom airports.
  FbAirport a{};
  strcpy(a.icao, "KJFK"); strcpy(a.iata, "JFK"); strcpy(a.name, "NEW YORK"); strcpy(a.tz, "America/New_York");
  CHECK(fbs::checkAirport(a, width4, known) == nullptr);
  FbAirport x = a; strcpy(x.icao, "kjfk");     CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.icao, "1JFK");               CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.icao, "K00A");               CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.iata, "");                   CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.iata, "JF");                 CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.name, "");                   CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.name, "New York");           CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  // The system font's Cyrillic capitals (src/fonts/name_chars.h), 2 bytes a letter.
  x = a; strcpy(x.name, "СОЧИ");               CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.name, "ВНУКОВО");            CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // 14 bytes > 12
  x = a; strcpy(x.name, "Внуково");            CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // lowercase
  x = a; strcpy(x.name, "ЁЛКИ-2");             CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.name, "\xD0");               CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // broken UTF-8
  x = a; strcpy(x.name, "NEW  YORK");          CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.name, " NEWYORK");           CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.name, "ST. JOHN'S");         CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.name, "ABCDEFGHIJKL");       CHECK(fbs::checkAirport(x, width4, known) == nullptr);   // 48 px: fits 56
  CHECK(fbs::checkAirport(x, width8, known) != nullptr);                                                  // 96 px: too wide
  x = a; strcpy(x.tz, "");                     CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // the board needs a zone
  x = a; strcpy(x.tz, "UTC");                  CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // not in tzdb.h's areas
  x = a; strcpy(x.tz, "Etc/GMT+5");            CHECK(fbs::checkAirport(x, width4, known) != nullptr);   // the 3 mwgg zones it lacks
  x = a; strcpy(x.tz, "Asia/Kolkata");         CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.tz, "America/Argentina/Buenos_Aires"); CHECK(fbs::checkAirport(x, width4, known) == nullptr);
  x = a; strcpy(x.tz, "Paris");                CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.tz, "Europe//Paris");        CHECK(fbs::checkAirport(x, width4, known) != nullptr);
  x = a; strcpy(x.tz, "Europe/Paris;rm");      CHECK(fbs::checkAirport(x, width4, known) != nullptr);
}

// Answers saved from somewhere else: whichever of the four files exist, no
// fixture-specific checks, the boards and the skip counters printed.
static int real(const std::string &dir, int64_t now) {
  aero::ListResult lr[aero::LIST_COUNT];
  bool have[aero::LIST_COUNT] = {};
  for (uint8_t l = 0; l < aero::LIST_COUNT; l++) {
    const std::string body = slurp(dir + "/aero_" + aero::listName((aero::List)l) + ".json");
    if (body.empty()) continue;
    JsonDocument filter, doc;
    aero::listFilter(filter, (aero::List)l);
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { std::printf("FAIL %s: not JSON\n", aero::listName((aero::List)l)); return 1; }
    have[l] = aero::parseList(doc.as<JsonVariantConst>(), (aero::List)l, &lr[l]);
    std::printf("LIST %-20s ok=%d seen=%u kept=%u noTime=%u noIdent=%u overflow=%u more=%d\n", aero::listName((aero::List)l),
                have[l], lr[l].seen, lr[l].n, lr[l].noTime, lr[l].noIdent, lr[l].overflow, lr[l].more);
  }
  for (bool dep : {false, true}) {
    aero::Board b;
    const aero::List p = dep ? aero::DEP_PAST : aero::ARR_PAST, n = dep ? aero::DEP_NEXT : aero::ARR_NEXT;
    aero::buildBoard(have[p] ? &lr[p] : nullptr, have[n] ? &lr[n] : nullptr, now, &b);
    std::printf("outside=%u dup=%u\n", b.outside, b.dup);
    printBoard(dep ? "dep" : "arr", b);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { std::printf("usage: aero_host_test <samples dir> [real <now epoch>]\n"); return 2; }
  const std::string dir = argv[1];
  if (argc >= 4 && !strcmp(argv[2], "real")) return real(dir, atoll(argv[3]));
  units();
  zones();
  settings();
  boards(dir, 1789408800);
  tracks(dir);
  std::printf("RESULT %d\n", g_fail);
  return g_fail ? 1 : 0;
}

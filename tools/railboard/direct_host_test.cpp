// Host test for src/railboard/rtt_transform.cpp. Built and run by
// tools/railboard/check_direct.py; not part of any firmware image.
//
//   direct_host_test <small fixture> [<big fixture>...]
//
// Prints FAIL lines, one "LISTS {...}" line with the small fixture's result for
// the Python port to compare against, one "SIZE ..." line per big fixture, and
// "RESULT <failures>" last.

#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "rtt_transform.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

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

static std::string slurp(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static int64_t T(const char *iso) {
  int64_t v = 0;
  if (!rtt::parseTime(iso, &v)) {
    g_fail++;
    std::printf("FAIL parseTime(%s)\n", iso);
  }
  return v;
}

static void testTimes() {
  int64_t v = 0;
  CHECK(rtt::parseTime("2026-09-14T13:05:07Z", &v) && v == 1789391107);
  CHECK(rtt::parseTime("2026-09-14T14:05:07+01:00", &v) && v == 1789391107);
  CHECK(rtt::parseTime("2026-09-14T14:05:07+0100", &v) && v == 1789391107);
  CHECK(rtt::parseTime("2026-09-14T08:35:07-04:30", &v) && v == 1789391107);
  CHECK(rtt::parseTime("2026-09-14T13:05:07.123Z", &v) && v == 1789391107);
  CHECK(rtt::parseTime("1970-01-01T00:00:00Z", &v) && v == 0);
  CHECK(rtt::parseTime("2000-02-29T12:00:00Z", &v) && v == 951825600);
  CHECK(!rtt::parseTime("2026-09-14T14:05:07", &v));        // no zone: refused, not guessed
  CHECK(!rtt::parseTime("2026-13-14T14:05:07Z", &v));
  CHECK(!rtt::parseTime("2026-09-14T14:05:60Z", &v));
  CHECK(!rtt::parseTime("2026-09-14T14:05:07Zjunk", &v));
  CHECK(!rtt::parseTime("", &v));
  CHECK(!rtt::parseTime(nullptr, &v));

  char buf[32];
  rtt::formatTime(1789391107, buf, sizeof(buf));
  CHECK(!std::strcmp(buf, "2026-09-14T13:05:07Z"));
  char q[96];
  rtt::buildQuery("GLD", 1789391107, true, q, sizeof(q));
  CHECK(!std::strcmp(q, "code=GLD&timeFrom=2026-09-14T12:35:07Z&timeWindow=90"));
  rtt::buildQuery("GLD", 0, false, q, sizeof(q));
  CHECK(!std::strcmp(q, "code=GLD"));
}

struct Want {
  uint8_t side;
  const char *t, *x;
  uint8_t st, d;
  const char *p, *n, *o;
};

// What the package's template makes of samples/rtt_location_small.json at
// 14:05:07 BST, rule by rule - see gen_rtt_fixture.small() for the inputs.
static const Want kWant[] = {
  {RB_DEP, "2026-09-14T14:04:30+01:00", "2026-09-14T14:04:30+01:00", RB_OK,       0, "2",   "Woking",                   "SW"},
  {RB_DEP, "2026-09-14T14:08:00+01:00", "2026-09-14T14:08:00+01:00", RB_OK,       0, "5",   "London Waterloo",          "SW"},
  {RB_DEP, "2026-09-14T14:12:00+01:00", "2026-09-14T14:19:00+01:00", RB_LATE,     7, "3",   "Portsmouth Harbour",       "SW"},
  {RB_DEP, "2026-09-14T14:17:00+01:00", nullptr,                     RB_CANC,     0, "8",   "Reading",                  "GW"},
  {RB_DEP, "2026-09-14T14:20:00+01:00", nullptr,                     RB_CANC,     0, "1",   "Ascot",                    "SW"},
  {RB_DEP, "2026-09-14T14:25:00+01:00", "2026-09-14T14:25:00+01:00", RB_OK,       0, "4",   "Aldershot",                "SW"},
  {RB_DEP, "2026-09-14T14:30:00+01:00", nullptr,                     RB_NOREPORT, 0, "",    "Farnham",                  "BUS"},
  {RB_DEP, "2026-09-14T14:40:00+01:00", "2026-09-14T14:40:00+01:00", RB_OK,       0, "10A", "London Waterloo &",        "SW"},
  {RB_ARR, "2026-09-14T13:59:00+01:00", "2026-09-14T14:04:00+01:00", RB_ARRIVED,  5, "6",   "Redhill",                  "GW"},
  {RB_ARR, "2026-09-14T14:14:00+01:00", "2026-09-14T14:14:00+01:00", RB_OK,       0, "1",   "Haslemere",                "SW"},
  {RB_ARR, "2026-09-14T14:21:00+01:00", nullptr,                     RB_CANC,     0, "4",   "Woking",                   "SW"},
  {RB_ARR, "2026-09-14T14:33:00+01:00", nullptr,                     RB_NOREPORT, 0, "7",   "Gatwick Airport",          "GW"},
  {RB_ARR, "2026-09-14T14:40:00+01:00", "2026-09-14T14:41:00+01:00", RB_LATE,     1, "",    "Abcdefghijklmnopqrstuvwx", "SW"},
};

static void printLists(const rtt::Lists &l) {
  JsonDocument out;
  static const char *const ST[] = {"ok", "late", "canc", "nr", "arr"};
  out["stn"] = l.stn;
  out["rt"]  = l.rt;
  for (uint8_t side = RB_DEP; side <= RB_ARR; side++) {
    JsonArray a = out[side == RB_DEP ? "dep" : "arr"].to<JsonArray>();
    for (uint8_t i = 0; i < l.board[side].count; i++) {
      const RbService &s = l.board[side].s[i];
      JsonObject o = a.add<JsonObject>();
      o["t"] = s.t; o["x"] = s.x; o["p"] = s.p; o["n"] = s.n; o["o"] = s.o;
      o["st"] = ST[s.st]; o["d"] = s.d;
    }
  }
  std::string json;
  serializeJson(out, json);
  std::printf("LISTS %s\n", json.c_str());
}

static void testSmall(const char *path) {
  const std::string body = slurp(path);
  const int64_t now = T("2026-09-14T14:05:07+01:00");

  JsonDocument filter, doc, whole;
  rtt::locationFilter(filter);
  CHECK(!deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter),
                         DeserializationOption::NestingLimit(12)));
  CHECK(!deserializeJson(whole, body.data(), body.size()));

  static rtt::Lists l, lw;
  CHECK(rtt::transform(doc.as<JsonVariantConst>(), now, "GLD", &l));
  CHECK(rtt::transform(whole.as<JsonVariantConst>(), now, "GLD", &lw));
  CHECK(std::memcmp(&l, &lw, sizeof(l)) == 0);           // the filter keeps everything the transform reads

  CHECK(!std::strcmp(l.stn, "Guildford"));
  CHECK(!std::strcmp(l.rt, "OK"));
  CHECK(l.seen == 19);
  CHECK(l.board[RB_DEP].count == 8);
  CHECK(l.board[RB_ARR].count == 5);
  uint8_t idx[2] = {0, 0};
  for (const Want &w : kWant) {
    const uint8_t i = idx[w.side]++;
    if (i >= l.board[w.side].count) { g_fail++; std::printf("FAIL missing row %u/%u\n", w.side, i); continue; }
    const RbService &s = l.board[w.side].s[i];
    const bool ok = s.t == (uint32_t)T(w.t) && s.x == (w.x ? (uint32_t)T(w.x) : 0U) && s.st == w.st &&
                    s.d == w.d && !std::strcmp(s.p, w.p) && !std::strcmp(s.n, w.n) && !std::strcmp(s.o, w.o);
    if (!ok) {
      g_fail++;
      std::printf("FAIL row side %u #%u: got t=%u x=%u st=%u d=%u p=%s n=%s o=%s\n", w.side, i, s.t, s.x, s.st,
                  s.d, s.p, s.n, s.o);
    }
  }
  printLists(l);

  // Not a line-up answer: no services array.
  JsonDocument bad;
  CHECK(!deserializeJson(bad, "{\"error\":\"nope\"}"));
  CHECK(!rtt::transform(bad.as<JsonVariantConst>(), now, "GLD", &l));
  CHECK(l.board[RB_DEP].count == 0 && !std::strcmp(l.stn, "GLD"));

  // 204: empty and valid, named by its code.
  rtt::emptyLists("WAT", &l);
  CHECK(l.board[RB_ARR].count == 0 && !std::strcmp(l.stn, "WAT"));

  // /api/get_access_token (main.yml 1644-1669).
  JsonDocument tokFilter, tok;
  rtt::tokenFilter(tokFilter);
  const char *answer = "{\"token\":\"abc.def.ghi\",\"entitlements\":[\"allowDetailed\"],"
                       "\"validUntil\":\"2026-09-14T15:05:07+01:00\"}";
  CHECK(!deserializeJson(tok, answer, DeserializationOption::Filter(tokFilter)));
  char token[64];
  int64_t until = 0;
  CHECK(rtt::accessToken(tok.as<JsonVariantConst>(), token, sizeof(token), &until));
  CHECK(!std::strcmp(token, "abc.def.ghi") && until == 1789394707);
  CHECK(!rtt::accessToken(tok.as<JsonVariantConst>(), token, 5, &until));   // does not fit: refused
  JsonDocument none;
  CHECK(!deserializeJson(none, "{\"validUntil\":\"2026-09-14T15:05:07Z\"}"));
  CHECK(!rtt::accessToken(none.as<JsonVariantConst>(), token, sizeof(token), &until));
}

static void measure(const char *path) {
  const std::string body = slurp(path);
  Counting alloc;
  const auto t0 = std::chrono::steady_clock::now();
  static rtt::Lists l;
  bool ok;
  {
    JsonDocument filter(&alloc), doc(&alloc);
    rtt::locationFilter(filter);
    const DeserializationError e = deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter),
                                                   DeserializationOption::NestingLimit(12));
    ok = !e && rtt::transform(doc.as<JsonVariantConst>(), 1789391107, "GLD", &l);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ok);
  std::printf("SIZE %s bytes=%zu services=%u peak=%zu ms=%.2f dep=%u arr=%u\n", path, body.size(), l.seen,
              alloc.peak, ms, l.board[RB_DEP].count, l.board[RB_ARR].count);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: %s <small fixture> [big fixtures...]\n", argv[0]);
    return 2;
  }
  testTimes();
  testSmall(argv[1]);
  for (int i = 2; i < argc; i++) measure(argv[i]);
  std::printf("RESULT %d\n", g_fail);
  return g_fail ? 1 : 0;
}

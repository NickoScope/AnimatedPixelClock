// Host test for src/railboard/rb_settings.h: the portal's config validation and
// the "due soon" rule. Built and run by tools/railboard/check_direct.py.

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "rb_settings.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace rbs;

static const Settings kBase = defaults(8, 10, 100, 80);

// Applies one JSON object to a copy of kBase. Returns the error, or "" on success.
static std::string run(const char *json, Settings *out = nullptr) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "unparsable test input";
  Settings s = kBase;
  char err[96];
  const char *e = apply(doc.as<JsonObjectConst>(), s, err, sizeof(err));
  if (out) *out = s;
  if (e) {
    CHECK(same(s, kBase));          // refused: nothing changed
    return e;
  }
  return "";
}

static void accepted(const char *json) {
  const std::string e = run(json);
  if (!e.empty()) { g_fail++; std::printf("FAIL refused %s: %s\n", json, e.c_str()); }
}

static void refused(const char *json, const char *mentions) {
  const std::string e = run(json);
  if (e.empty() || e.find(mentions) == std::string::npos) {
    g_fail++;
    std::printf("FAIL %s: expected an error naming %s, got \"%s\"\n", json, mentions, e.c_str());
  }
}

static void testValidation() {
  CHECK(valid(kBase));
  CHECK(kBase.rowColour == AMBER && kBase.headColour == WHITE && kBase.dueColour == GREEN);
  CHECK(kBase.dueMin == 3 && kBase.clockSeconds);
  CHECK(kPaletteCount == 6 && paletteIndex("cyan") == CYAN && paletteIndex("red") < 0 && paletteIndex(nullptr) < 0);

  for (const char *j : {"{}", "{\"rows\":1}", "{\"rows\":8}", "{\"switch_s\":3}", "{\"switch_s\":600}",
                        "{\"level\":10}", "{\"level\":100}", "{\"stale_s\":30}", "{\"stale_s\":3600}",
                        "{\"due_min\":0}", "{\"due_min\":15}", "{\"clock_seconds\":false}", "{\"clock_seconds\":true}",
                        "{\"row_color\":\"yellow\"}", "{\"row_color\":\"white\"}", "{\"head_color\":\"amber\"}",
                        "{\"due_color\":\"cyan\"}", "{\"row_color\":\"green\",\"due_color\":\"cyan\"}",
                        "{\"row_color\":\"green\",\"due_min\":0}"})
    accepted(j);

  refused("{\"rows\":0}", "rows");
  refused("{\"rows\":9}", "rows");
  refused("{\"rows\":1.5}", "rows");
  refused("{\"rows\":\"3\"}", "rows");
  refused("{\"rows\":true}", "rows");
  refused("{\"rows\":null}", "rows");
  refused("{\"switch_s\":2}", "switch_s");
  refused("{\"switch_s\":601}", "switch_s");
  refused("{\"level\":9}", "level");
  refused("{\"level\":101}", "level");
  refused("{\"stale_s\":29}", "stale_s");
  refused("{\"stale_s\":3601}", "stale_s");
  refused("{\"due_min\":-1}", "due_min");
  refused("{\"due_min\":16}", "due_min");
  refused("{\"clock_seconds\":1}", "clock_seconds");
  refused("{\"clock_seconds\":\"yes\"}", "clock_seconds");
  refused("{\"row_color\":\"red\"}", "row_color");
  refused("{\"row_color\":\"Amber\"}", "row_color");
  refused("{\"head_color\":5}", "head_color");
  refused("{\"due_color\":\"\"}", "due_color");
  refused("{\"font\":\"large\"}", "font");
  refused("{\"panels\":2}", "panels");
  refused("{\"row_color\":\"green\"}", "due_color");         // the highlight would vanish
  refused("{\"due_color\":\"amber\"}", "due_color");
  refused("{\"rows\":5,\"level\":5}", "level");               // one bad key: nothing applied

  Settings s;
  CHECK(run("{\"rows\":5,\"row_color\":\"yellow\",\"clock_seconds\":false,\"due_min\":7}", &s).empty());
  CHECK(s.rows == 5 && s.rowColour == YELLOW && !s.clockSeconds && s.dueMin == 7);
  CHECK(s.switchS == kBase.switchS && s.headColour == kBase.headColour);   // the rest kept

  JsonDocument out;
  toJson(kBase, out.to<JsonObject>());
  CHECK(!std::strcmp(out["row_color"] | "", "amber") && (out["due_min"] | -1) == 3 && (out["clock_seconds"] | false));
  Settings round = defaults(1, 3, 10, 30);
  char err[96];
  CHECK(apply(out.as<JsonObjectConst>(), round, err, sizeof(err)) == nullptr && same(round, kBase));   // GET feeds POST
  JsonDocument b;
  boundsJson(b.to<JsonObject>());
  CHECK((b["due_min"][1] | 0) == 15 && (b["rows"][1] | 0) == 8 && b["colors"].size() == 6);
  CHECK(!std::strcmp(b["colors"][0]["hex"] | "", "#ff9600"));
}

static RbService svc(uint32_t t, uint32_t x, uint8_t st) {
  RbService s;
  std::memset(&s, 0, sizeof(s));
  s.t = t; s.x = x; s.st = st;
  return s;
}

static void testDueSoon() {
  const int64_t now = 1789391107;
  CHECK(dueSoon(svc(now + 180, 0, RB_NOREPORT), now, 3));          // exactly 3 min: due
  CHECK(!dueSoon(svc(now + 181, 0, RB_NOREPORT), now, 3));         // a second over: not
  CHECK(dueSoon(svc(now + 60, now + 60, RB_OK), now, 1));
  CHECK(!dueSoon(svc(now + 120, 0, RB_NOREPORT), now, 1));
  CHECK(!dueSoon(svc(now + 120, now + 600, RB_LATE), now, 3));     // scheduled soon, expected later: not
  CHECK(dueSoon(svc(now - 600, now + 150, RB_LATE), now, 3));      // scheduled long ago, expected soon: due
  CHECK(dueSoon(svc(now - 30, 0, RB_NOREPORT), now, 3));           // at or past its time, not reported gone
  CHECK(dueSoon(svc(now - 90, now - 60, RB_ARRIVED), now, 3));     // arrived: due until the list drops it
  CHECK(!dueSoon(svc(now + 60, 0, RB_CANC), now, 3));              // cancelled never
  CHECK(!dueSoon(svc(now + 60, now + 60, RB_OK), now, 0));         // off
  CHECK(!dueSoon(svc(0, 0, RB_OK), now, 3));                        // no time at all
  CHECK(!dueSoon(svc(now + 60, 0, RB_OK), 0, 3));                   // no clock (the page passes 0)
  CHECK(dueSoon(svc(now + 900, 0, RB_OK), now, 15));
}

int main() {
  testValidation();
  testDueSoon();
  std::printf("SETTINGS RESULT %d\n", g_fail);
  return g_fail ? 1 : 0;
}
